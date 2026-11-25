#pragma once

#include "auth/Auth.hpp"
#include "Mailer.hpp"
#include "db/MySql.hpp"

namespace auth
{
  struct Auth::Impl
  {
    db::MySql &mysql;
    db::Redis &redis;
    Config config;
    detail::Mailer mailer;
    std::string dummy_hash;
    Impl(db::MySql &mysql, db::Redis &redis, const Config &config);
    Result<SessionTicket> CreateSession(UserId user_id, const std::string &user_agent);
    Status RevokeAll(UserId user_id);
    Status SendCode(UserId user_id, const std::string &email, EmailPurpose purpose);
    Status ConsumeCode(UserId user_id, const std::string &email, EmailPurpose purpose, const std::string &code);
    Status InvalidateCodes(UserId user_id);
    Status NotifyPasswordChanged(const std::string &email);

    void Limit(const std::string &operation, const std::string &identifier, bool login = false);
    std::vector<std::optional<std::string>> LockedUser(db::MySql::Connection &connection, UserId id);
    std::vector<std::optional<std::string>> LockedUser(db::MySql::Connection &connection, UserId id,
                                                    const std::string &password);
    std::string VerificationKey(const AuthContext &context);
    void RequireVerification(db::MySql::Connection &connection, const AuthContext &context);
    void RequireAdmin(const AuthContext &context);
  };
} // namespace auth
