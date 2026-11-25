#include "Mailer.hpp"
#include "Support.hpp"
#include "common/Curl.hpp"
#include <curl/curl.h>
#include <cstring>
#include <ctime>

namespace auth::detail
{
Mailer::Mailer(const SmtpConfig &config) : config_(config)
{
  if (!config_.host.empty())
  {
    try { config_.from_address = detail::EmailAddress(config_.from_address); }
    catch (const Error &) { throw std::invalid_argument("Invalid SMTP sender address"); }
    if (config_.from_name.find_first_of("\r\n") != config_.from_name.npos ||
        config_.host.find_first_of("/\r\n") != config_.host.npos)
      throw std::invalid_argument("Invalid SMTP configuration");
  }
}
Status Mailer::Send(const std::string &recipient, const std::string &subject, const std::string &text)
{
  return detail::Guard<std::monostate>([&] {
    if (config_.host.empty()) throw Error{ErrorCode::Unavailable, "邮件服务尚未配置，请稍后重试。"};
    transport::InitCurl();
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl) throw std::runtime_error("创建邮件传输失败");
    const auto host = config_.host.find(':') == config_.host.npos ? config_.host : "[" + config_.host + "]";
    auto url = std::string(config_.security == SmtpSecurity::Tls ? "smtps://" : "smtp://") + host + ":" + std::to_string(config_.port);
    auto sender = "<" + config_.from_address + ">";
    auto target = "<" + recipient + ">";
    auto now = std::time(nullptr);
    std::tm utc{};
    gmtime_r(&now, &utc);
    char date[80];
    std::strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S +0000", &utc);
    std::string name;
    if (!config_.from_name.empty())
    {
      std::string encoded(4 * ((config_.from_name.size() + 2) / 3) + 1, '\0');
      auto length = EVP_EncodeBlock(reinterpret_cast<unsigned char *>(encoded.data()), reinterpret_cast<const unsigned char *>(config_.from_name.data()), config_.from_name.size());
      encoded.resize(length);
      name = "=?UTF-8?B?" + encoded + "?= ";
    }
    std::string data = "From: " + name + sender + "\r\nDate: " + date +
                       "\r\nMessage-ID: <" + detail::Random(16) + "@" + config_.from_address.substr(config_.from_address.find('@') + 1) + ">" +
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
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, static_cast<long>(config_.timeout.count()));
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(config_.timeout.count()));
    curl_easy_setopt(curl.get(), CURLOPT_USE_SSL, CURLUSESSL_ALL);
    if (!config_.ca_file.empty()) curl_easy_setopt(curl.get(), CURLOPT_CAINFO, config_.ca_file.c_str());
    if (!config_.username.empty())
    {
      curl_easy_setopt(curl.get(), CURLOPT_USERNAME, config_.username.c_str());
      curl_easy_setopt(curl.get(), CURLOPT_PASSWORD, config_.password.c_str());
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
} // namespace auth::detail
