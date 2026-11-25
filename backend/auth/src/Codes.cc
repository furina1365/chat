#include "AuthImpl.hpp"
#include "Support.hpp"
#include <iomanip>

namespace auth
{
Status Auth::Impl::SendCode(UserId user_id, const std::string &input, EmailPurpose purpose)
{
  return detail::Guard<std::monostate>([&] {
    auto email = detail::EmailAddress(input);
    if (config.smtp.host.empty()) throw Error{ErrorCode::Unavailable, "邮件服务尚未配置。"};
    auto root = config.email_namespace + ":" + std::to_string(user_id);
    auto key = root + (purpose == EmailPurpose::Bind ? ":bind" : ":reset");
    std::uint32_t random;
    do { if (RAND_bytes(reinterpret_cast<unsigned char *>(&random), sizeof(random)) != 1) throw std::runtime_error("随机数生成失败"); }
    while (random >= 4294000000U);
    std::ostringstream code;
    code << std::setw(6) << std::setfill('0') << random % 1000000;
    auto digest = detail::Hash(code.str());
    static const std::string script = R"(
      if redis.call('EXISTS',KEYS[2])==1 then return 0 end
      local security_stamp=redis.call('GET',KEYS[3]); if not security_stamp then security_stamp=ARGV[4]; redis.call('SET',KEYS[3],security_stamp) end
      redis.call('HSET',KEYS[1],'email',ARGV[1],'digest',ARGV[2],'attempts',0,'security_stamp',security_stamp)
      redis.call('EXPIRE',KEYS[1],ARGV[3]); redis.call('SET',KEYS[2],'1','EX',ARGV[5]); return 1
    )";
    auto reply = db::Command(redis, {"EVAL", script, "3", key, key + ":cooldown", root + ":security_stamp",
        email, digest, std::to_string(config.code_timeout.count()), detail::Random(16), std::to_string(config.resend_interval.count())});
    if (reply.integer != 1) throw Error{ErrorCode::RateLimited, "验证码已发送，请稍后再申请。"};
    auto result = mailer.Send(email, "Verification code", "您的验证码：" + code.str() +
        "\r\n用途：" + (purpose == EmailPurpose::Bind ? std::string("绑定或更换邮箱") : std::string("重置密码")) +
        "\r\n有效期：" + std::to_string(config.code_timeout.count()) + " 秒。请勿转发。");
    if (auto *error = std::get_if<Error>(&result))
    {
      db::Command(redis, {"EVAL", "if redis.call('HGET',KEYS[1],'digest')==ARGV[1] then redis.call('DEL',KEYS[1]) end; return 1", "1", key, digest});
      throw *error;
    }
    return std::monostate{};
  });
}
Status Auth::Impl::ConsumeCode(UserId user_id, const std::string &input, EmailPurpose purpose, const std::string &code)
{
  return detail::Guard<std::monostate>([&] {
    auto email = detail::EmailAddress(input);
    if (code.size() != 6 || code.find_first_not_of("0123456789") != code.npos) throw Error{ErrorCode::InvalidCode, "验证码无效或已过期，请重新申请。"};
    auto root = config.email_namespace + ":" + std::to_string(user_id);
    auto key = root + (purpose == EmailPurpose::Bind ? ":bind" : ":reset");
    static const std::string script = R"(
      if redis.call('EXISTS',KEYS[1])==0 then return 0 end
      local n=redis.call('HINCRBY',KEYS[1],'attempts',1)
      local valid=redis.call('HGET',KEYS[1],'email')==ARGV[1] and redis.call('HGET',KEYS[1],'digest')==ARGV[2]
        and redis.call('HGET',KEYS[1],'security_stamp')==redis.call('GET',KEYS[2])
      if valid or n>=tonumber(ARGV[3]) then redis.call('DEL',KEYS[1]) end
      if valid then return 1 else return 0 end
    )";
    auto reply = db::Command(redis, {"EVAL", script, "2", key, root + ":security_stamp", email, detail::Hash(code), std::to_string(config.max_code_attempts)});
    if (reply.integer != 1) throw Error{ErrorCode::InvalidCode, "验证码无效或已过期，请重新申请。"};
    return std::monostate{};
  });
}
Status Auth::Impl::InvalidateCodes(UserId user_id)
{
  return detail::Guard<std::monostate>([&] {
    db::Command(redis, {"SET", config.email_namespace + ":" + std::to_string(user_id) + ":security_stamp", detail::Random(16)});
    return std::monostate{};
  });
}
Status Auth::Impl::NotifyPasswordChanged(const std::string &email)
{
  return detail::Guard<std::monostate>([&] {
    return detail::Take(mailer.Send(detail::EmailAddress(email), "Password changed",
        "您的账号密码已修改，所有旧登录已退出。如非本人操作，请立即通过邮箱重置密码。"));
  });
}
} // namespace auth
