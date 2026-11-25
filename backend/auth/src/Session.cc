#include "AuthImpl.hpp"
#include "Support.hpp"

namespace auth
{
  namespace
  {
    bool ValidId(const std::string &id)
    {
      return id.size() == 64 && id.find_first_not_of("0123456789abcdef") == id.npos;
    }
    std::string UserAgent(const std::string &input)
    {
      auto value = input.substr(0, 512);
      for (char &c : value)
        if (static_cast<unsigned char>(c) < 32 || c == 127)
          c = ' ';
      return value;
    }
  }
  Result<SessionTicket> Auth::Impl::CreateSession(UserId user_id, const std::string &user_agent)
  {
    return detail::Guard<SessionTicket>([&]
                                        {
    if (!user_id) throw Error{ErrorCode::InvalidRequest, "无效用户。"};
    auto token = detail::Random();
    auto id = detail::Hash(token);
    static const std::string script = R"(
      local security_stamp=redis.call('GET',KEYS[2])
      if not security_stamp then security_stamp=ARGV[2]; redis.call('SET',KEYS[2],security_stamp) end
      local time=redis.call('TIME'); local now=tonumber(time[1])*1000+math.floor(tonumber(time[2])/1000)
      local expires=now+tonumber(ARGV[3])
      redis.call('HSET',KEYS[1],'uid',ARGV[1],'security_stamp',security_stamp,'created',now,'active',now,'expires',expires,'user_agent',ARGV[5])
      redis.call('PEXPIRE',KEYS[1],ARGV[3])
      redis.call('ZREMRANGEBYSCORE',KEYS[3],'-inf',now)
      redis.call('ZADD',KEYS[3],expires,ARGV[4])
      redis.call('PEXPIRE',KEYS[3],ARGV[3])
      return expires
    )";
    auto reply = db::Command(redis, {"EVAL", script, "3", config.session_namespace + ":" + id,
        config.session_namespace + ":user:" + std::to_string(user_id),
        config.session_namespace + ":devices:" + std::to_string(user_id), std::to_string(user_id), detail::Random(16),
        std::to_string(config.session_idle_timeout.count() * 1000), id, UserAgent(user_agent)});
    return SessionTicket{{user_id, id}, token, TimePoint(std::chrono::milliseconds(reply.integer))}; });
  }
  Result<AuthContext> Auth::Authenticate(const std::string &token, const std::string &user_agent)
  {
    return detail::Guard<AuthContext>([&]
                                      {
    if (!ValidId(token))
      throw Error{ErrorCode::Unauthenticated, "请先登录。"};
    auto id = detail::Hash(token);
    static const std::string script = R"(
      local uid=redis.call('HGET',KEYS[1],'uid'); if not uid then return false end
      local devices=ARGV[3]..uid
      local security_stamp=redis.call('GET',ARGV[1]..uid)
      if not security_stamp or security_stamp~=redis.call('HGET',KEYS[1],'security_stamp') then redis.call('DEL',KEYS[1]); redis.call('ZREM',devices,ARGV[4]); return false end
      local time=redis.call('TIME'); local now=tonumber(time[1])*1000+math.floor(tonumber(time[2])/1000)
      local active=tonumber(redis.call('HGET',KEYS[1],'active'))
      if now-active>=tonumber(ARGV[2]) then redis.call('DEL',KEYS[1]); redis.call('ZREM',devices,ARGV[4]); return false end
      local expires=now+tonumber(ARGV[2])
      redis.call('HSET',KEYS[1],'active',now,'expires',expires)
      redis.call('PEXPIRE',KEYS[1],ARGV[2])
      if ARGV[5]~='' and (redis.call('HGET',KEYS[1],'user_agent') or '')=='' then redis.call('HSET',KEYS[1],'user_agent',ARGV[5]) end
      redis.call('ZREMRANGEBYSCORE',devices,'-inf',now)
      redis.call('ZADD',devices,expires,ARGV[4])
      redis.call('PEXPIRE',devices,ARGV[2])
      return uid
    )";
    auto reply = db::Command(impl_->redis, {"EVAL", script, "1", impl_->config.session_namespace + ":" + id,
        impl_->config.session_namespace + ":user:", std::to_string(impl_->config.session_idle_timeout.count() * 1000),
        impl_->config.session_namespace + ":devices:", id, UserAgent(user_agent)});
    if (reply.type != db::RedisReply::Type::String) throw Error{ErrorCode::Unauthenticated, "登录已过期，请重新登录。"};
    return AuthContext{std::stoull(reply.text), id}; });
  }
  Result<SessionPage> Auth::ListSessions(const AuthContext &context, const std::string &cursor)
  {
    return detail::Guard<SessionPage>([&]
                                      {
    if (cursor.empty() || cursor.size() > 20 || cursor.find_first_not_of("0123456789") != cursor.npos
        || (cursor.size() == 20 && cursor > "18446744073709551615"))
      throw Error{ErrorCode::InvalidRequest, "设备列表游标无效。"};
    static const std::string script = R"(
      local security_stamp=redis.call('GET',KEYS[2])
      if not security_stamp or redis.call('HGET',KEYS[1],'uid')~=ARGV[1] or redis.call('HGET',KEYS[1],'security_stamp')~=security_stamp then return false end
      local time=redis.call('TIME'); local now=tonumber(time[1])*1000+math.floor(tonumber(time[2])/1000)
      redis.call('ZREMRANGEBYSCORE',KEYS[3],'-inf',now)
      local page=redis.call('ZSCAN',KEYS[3],ARGV[2],'COUNT',50)
      local result={}
      for i=1,#page[2],2 do
        local id=page[2][i]; local key=ARGV[3]..id
        local fields=redis.call('HMGET',key,'uid','security_stamp','user_agent','created','active')
        if fields[1]==ARGV[1] and fields[2]==security_stamp and fields[5] and now-tonumber(fields[5])<tonumber(ARGV[4]) then
          table.insert(result,{id,fields[3] or '',fields[4],fields[5],tostring(tonumber(fields[5])+tonumber(ARGV[4]))})
        else
          redis.call('ZREM',KEYS[3],id)
        end
      end
      return {page[1],result}
    )";
    auto prefix = impl_->config.session_namespace;
    auto user = std::to_string(context.user_id);
    auto reply = db::Command(impl_->redis, {"EVAL", script, "3", prefix + ":" + context.session_id,
        prefix + ":user:" + user, prefix + ":devices:" + user, user, cursor, prefix + ":",
        std::to_string(impl_->config.session_idle_timeout.count() * 1000)});
    if (reply.type != db::RedisReply::Type::Array) throw Error{ErrorCode::Unauthenticated, "登录已过期，请重新登录。"};
    SessionPage page;
    page.next_cursor = reply.elements.at(0).text;
    for (const auto &item : reply.elements.at(1).elements)
    {
      const auto &values = item.elements;
      auto time = [&](std::size_t i) { return TimePoint(std::chrono::milliseconds(std::stoll(values.at(i).text))); };
      page.items.push_back({values.at(0).text, values.at(1).text, time(2), time(3), time(4), values.at(0).text == context.session_id});
    }
    return page; });
  }
  Status Auth::LogoutDevice(const AuthContext &context, const SessionId &id)
  {
    return detail::Guard<std::monostate>([&]
                                         {
    impl_->Limit("logout_device", std::to_string(context.user_id));
    if (!ValidId(id)) throw Error{ErrorCode::InvalidRequest, "设备会话标识无效。"};
    static const std::string script = R"(
      local security_stamp=redis.call('GET',KEYS[3])
      if not security_stamp or redis.call('HGET',KEYS[1],'uid')~=ARGV[1] or redis.call('HGET',KEYS[1],'security_stamp')~=security_stamp then return -1 end
      if redis.call('HGET',KEYS[2],'uid')~=ARGV[1] or redis.call('HGET',KEYS[2],'security_stamp')~=security_stamp then return 0 end
      redis.call('DEL',KEYS[2]); redis.call('ZREM',KEYS[4],ARGV[2]); return 1
    )";
    auto prefix = impl_->config.session_namespace;
    auto user = std::to_string(context.user_id);
    auto reply = db::Command(impl_->redis, {"EVAL", script, "4", prefix + ":" + context.session_id,
        prefix + ":" + id, prefix + ":user:" + user, prefix + ":devices:" + user, user, id});
    if (reply.integer == -1) throw Error{ErrorCode::Unauthenticated, "登录已过期，请重新登录。"};
    if (!reply.integer) throw Error{ErrorCode::NotFound, "该设备已退出或不存在。"};
    return std::monostate{}; });
  }
  Status Auth::Logout(const AuthContext &context)
  {
    return detail::Guard<std::monostate>([&]
                                         {
    static const std::string script = "if redis.call('HGET',KEYS[1],'uid')==ARGV[1] then redis.call('DEL',KEYS[1]); redis.call('ZREM',KEYS[2],ARGV[2]) end; return 1";
    db::Command(impl_->redis, {"EVAL", script, "2", impl_->config.session_namespace + ":" + context.session_id,
        impl_->config.session_namespace + ":devices:" + std::to_string(context.user_id), std::to_string(context.user_id), context.session_id});
    return std::monostate{}; });
  }
  Status Auth::Impl::RevokeAll(UserId user_id)
  {
    return detail::Guard<std::monostate>([&]
                                         {
    static const std::string script = "redis.call('SET',KEYS[1],ARGV[1]); redis.call('DEL',KEYS[2]); return 1";
    db::Command(redis, {"EVAL", script, "2", config.session_namespace + ":user:" + std::to_string(user_id),
        config.session_namespace + ":devices:" + std::to_string(user_id), detail::Random(16)});
    return std::monostate{}; });
  }
} // namespace auth
