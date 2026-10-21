#pragma once

#include "Types.hpp"

#include <chrono>
#include <memory>
#include <string>

namespace db
{
  class Redis;
}

namespace auth
{
  struct SessionConfig
  {
    std::string key_namespace = "auth:session";
    std::chrono::seconds idle_timeout{24 * 60 * 60};
    std::chrono::seconds absolute_timeout{7 * 24 * 60 * 60};
  };

  // 登录会话 key 的唯一业务入口。Redis 须比本对象活得更久。
  // 所有方法允许并发调用，执行时可能阻塞，应在业务 worker 中执行。
  class Session
  {
  public:
    explicit Session(db::Redis &redis, const SessionConfig &config = {});
    ~Session();
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

    Result<SessionTicket> Create(UserId user_id, const std::string &user_agent = {});
    // 校验闲置和绝对过期时间，并原子续期；已撤销会话不能被续期恢复。
    Result<AuthContext> Authenticate(const std::string &token, const std::string &user_agent = {});
    // 游标为 Redis 提供的不透明十进制串，"0" 表示起点或遍历结束。
    Result<SessionPage> List(const AuthContext &context, const std::string &cursor = "0");
    Status RevokeDevice(const AuthContext &context, const SessionId &id);
    // 只撤销当前设备；重复撤销已不存在的会话视为成功。
    Status Revoke(const AuthContext &context);
    Status RevokeAll(UserId user_id);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
  };
} // namespace auth
