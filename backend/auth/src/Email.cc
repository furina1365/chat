#include "auth/Email.hpp"
#include "Support.hpp"
#include "common/Curl.hpp"
#include <curl/curl.h>
#include <cstring>
#include <iomanip>
#include <ctime>

namespace auth
{
struct Email::Impl
{
  db::Redis &redis;
  EmailConfig config;
  std::string password;
  Status Send(const std::string &recipient, const std::string &subject, const std::string &text)
  {
    return detail::Guard<std::monostate>([&] {
      if (config.smtp.host.empty()) throw Error{ErrorCode::Unavailable, "邮件服务尚未配置，请稍后重试。"};
      transport::InitCurl();
      std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
      if (!curl) throw std::runtime_error("创建邮件传输失败");
      const auto host = config.smtp.host.find(':') == config.smtp.host.npos ? config.smtp.host : "[" + config.smtp.host + "]";
      auto url = std::string(config.smtp.security == SmtpSecurity::Tls ? "smtps://" : "smtp://") + host + ":" + std::to_string(config.smtp.port);
      auto sender = "<" + config.smtp.from_address + ">";
      auto target = "<" + recipient + ">";
      auto now = std::time(nullptr);
      std::tm utc{};
      gmtime_r(&now, &utc);
      char date[80];
      std::strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S +0000", &utc);
      std::string name;
      if (!config.smtp.from_name.empty())
      {
        std::string encoded(4 * ((config.smtp.from_name.size() + 2) / 3) + 1, '\0');
        auto length = EVP_EncodeBlock(reinterpret_cast<unsigned char *>(encoded.data()), reinterpret_cast<const unsigned char *>(config.smtp.from_name.data()), config.smtp.from_name.size());
        encoded.resize(length);
        name = "=?UTF-8?B?" + encoded + "?= ";
      }
      std::string data = "From: " + name + sender + "\r\nDate: " + date +
                         "\r\nMessage-ID: <" + detail::Random(16) + "@" + config.smtp.from_address.substr(config.smtp.from_address.find('@') + 1) + ">" +
                         "\r\nTo: " + target + "\r\nSubject: " + subject +
                         "\r\nMIME-Version: 1.0\r\nContent-Type: text/plain; charset=utf-8\r\n"
                         "Content-Transfer-Encoding: 8bit\r\n\r\n" + text + "\r\n";
      struct Upload { const std::string &data; std::size_t offset = 0; } upload{data};
      auto read = +[](char *buffer, std::size_t size, std::size_t count, void *opaque) -> std::size_t {
        auto &upload = *static_cast<Upload *>(opaque);
        auto length = std::min(size * count, upload.data.size() - upload.offset);
        std::memcpy(buffer, upload.data.data() + upload.offset, length);
        upload.offset += length;
        return length;
      };
      std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> recipients(curl_slist_append(nullptr, target.c_str()), curl_slist_free_all);
      curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
      curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
      curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, static_cast<long>(config.smtp.timeout.count()));
      curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(config.smtp.timeout.count()));
      curl_easy_setopt(curl.get(), CURLOPT_USE_SSL, CURLUSESSL_ALL);
      if (!config.smtp.ca_file.empty()) curl_easy_setopt(curl.get(), CURLOPT_CAINFO, config.smtp.ca_file.c_str());
      if (!config.smtp.username.empty())
      {
        curl_easy_setopt(curl.get(), CURLOPT_USERNAME, config.smtp.username.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_PASSWORD, password.c_str());
      }
      curl_easy_setopt(curl.get(), CURLOPT_MAIL_FROM, sender.c_str());
      curl_easy_setopt(curl.get(), CURLOPT_MAIL_RCPT, recipients.get());
      curl_easy_setopt(curl.get(), CURLOPT_READFUNCTION, read);
      curl_easy_setopt(curl.get(), CURLOPT_READDATA, &upload);
      curl_easy_setopt(curl.get(), CURLOPT_UPLOAD, 1L);
      if (curl_easy_perform(curl.get()) != CURLE_OK) throw Error{ErrorCode::Unavailable, "邮件发送失败，请稍后重新申请。"};
      return std::monostate{};
    });
  }
};
Email::Email(db::Redis &redis, const EmailConfig &config)
    : impl_(std::make_unique<Impl>(Impl{redis, config, db::Password(config.smtp.host.empty() ? "" : config.smtp.password_env)}))
{
  if (config.code_timeout.count() <= 0 || config.resend_interval.count() <= 0 || !config.max_code_attempts)
    throw std::invalid_argument("无效的验证码配置");
  if (!config.smtp.host.empty())
  {
    try { impl_->config.smtp.from_address = detail::EmailAddress(config.smtp.from_address); }
    catch (const Error &) { throw std::invalid_argument("SMTP 发件邮箱无效"); }
    if (config.smtp.from_name.find_first_of("\r\n") != config.smtp.from_name.npos ||
        config.smtp.host.find_first_of("/\r\n") != config.smtp.host.npos)
      throw std::invalid_argument("无效的 SMTP 配置");
  }
}
Email::~Email() = default;
Status Email::SendCode(UserId user_id, const std::string &input, EmailPurpose purpose)
{
  return detail::Guard<std::monostate>([&] {
    auto email = detail::EmailAddress(input);
    if (impl_->config.smtp.host.empty()) throw Error{ErrorCode::Unavailable, "邮件服务尚未配置。"};
    auto root = impl_->config.key_namespace + ":" + std::to_string(user_id);
    auto key = root + (purpose == EmailPurpose::Bind ? ":bind" : ":reset");
    std::uint32_t random;
    do { if (RAND_bytes(reinterpret_cast<unsigned char *>(&random), sizeof(random)) != 1) throw std::runtime_error("随机数生成失败"); }
    while (random >= 4294000000U);
    std::ostringstream code;
    code << std::setw(6) << std::setfill('0') << random % 1000000;
    auto digest = detail::Hash(code.str());
    static const std::string script = R"(
      if redis.call('EXISTS',KEYS[2])==1 then return 0 end
      local generation=redis.call('GET',KEYS[3]); if not generation then generation=ARGV[4]; redis.call('SET',KEYS[3],generation) end
      redis.call('HSET',KEYS[1],'email',ARGV[1],'digest',ARGV[2],'attempts',0,'generation',generation)
      redis.call('EXPIRE',KEYS[1],ARGV[3]); redis.call('SET',KEYS[2],'1','EX',ARGV[5]); return 1
    )";
    auto reply = db::Command(impl_->redis, {"EVAL", script, "3", key, key + ":cooldown", root + ":generation",
        email, digest, std::to_string(impl_->config.code_timeout.count()), detail::Random(16), std::to_string(impl_->config.resend_interval.count())});
    if (reply.integer != 1) throw Error{ErrorCode::RateLimited, "验证码已发送，请稍后再申请。"};
    auto result = impl_->Send(email, "Chat verification code", "您的验证码：" + code.str() +
        "\r\n用途：" + (purpose == EmailPurpose::Bind ? std::string("绑定或更换邮箱") : std::string("重置密码")) +
        "\r\n有效期：" + std::to_string(impl_->config.code_timeout.count()) + " 秒。请勿转发。");
    if (auto *error = std::get_if<Error>(&result))
    {
      db::Command(impl_->redis, {"EVAL", "if redis.call('HGET',KEYS[1],'digest')==ARGV[1] then redis.call('DEL',KEYS[1]) end; return 1", "1", key, digest});
      throw *error;
    }
    return std::monostate{};
  });
}
Status Email::ConsumeCode(UserId user_id, const std::string &input, EmailPurpose purpose, const std::string &code)
{
  return detail::Guard<std::monostate>([&] {
    auto email = detail::EmailAddress(input);
    if (code.size() != 6 || code.find_first_not_of("0123456789") != code.npos) throw Error{ErrorCode::InvalidCode, "验证码无效或已过期，请重新申请。"};
    auto root = impl_->config.key_namespace + ":" + std::to_string(user_id);
    auto key = root + (purpose == EmailPurpose::Bind ? ":bind" : ":reset");
    static const std::string script = R"(
      if redis.call('EXISTS',KEYS[1])==0 then return 0 end
      local n=redis.call('HINCRBY',KEYS[1],'attempts',1)
      local valid=redis.call('HGET',KEYS[1],'email')==ARGV[1] and redis.call('HGET',KEYS[1],'digest')==ARGV[2]
        and redis.call('HGET',KEYS[1],'generation')==redis.call('GET',KEYS[2])
      if valid or n>=tonumber(ARGV[3]) then redis.call('DEL',KEYS[1]) end
      if valid then return 1 else return 0 end
    )";
    auto reply = db::Command(impl_->redis, {"EVAL", script, "2", key, root + ":generation", email, detail::Hash(code), std::to_string(impl_->config.max_code_attempts)});
    if (reply.integer != 1) throw Error{ErrorCode::InvalidCode, "验证码无效或已过期，请重新申请。"};
    return std::monostate{};
  });
}
Status Email::InvalidateCodes(UserId user_id)
{
  return detail::Guard<std::monostate>([&] {
    db::Command(impl_->redis, {"SET", impl_->config.key_namespace + ":" + std::to_string(user_id) + ":generation", detail::Random(16)});
    return std::monostate{};
  });
}
Status Email::NotifyPasswordChanged(const std::string &email)
{
  return detail::Guard<std::monostate>([&] {
    return detail::Take(impl_->Send(detail::EmailAddress(email), "Chat password changed",
        "您的 Chat 账号密码已修改，所有旧登录已退出。如非本人操作，请立即通过邮箱重置密码。"));
  });
}
} // namespace auth
