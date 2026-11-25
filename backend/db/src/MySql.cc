#include "db/MySql.hpp"
#include "Support.hpp"
#include "Pool.hpp"
#include <soci/soci.h>
#include <soci/mysql/soci-mysql.h>
#include <mutex>

namespace db
{
namespace
{
// SOCI 的连接可以跨线程借用；MySQL 客户端仍要求每个使用线程初始化。
void InitThread()
{
  static std::once_flag initialized;
  std::call_once(initialized, [] {
    if (mysql_library_init(0, nullptr, nullptr)) throw std::runtime_error("Failed to initialize the MySQL client");
  });
  struct Thread
  {
    Thread() { if (mysql_thread_init()) throw std::runtime_error("Failed to initialize the MySQL thread"); }
    ~Thread() { mysql_thread_end(); }
  };
  thread_local Thread thread;
}
bool Lost(unsigned int code) { return code == 2006 || code == 2013 || code == 2055; }
Error SqlError(const soci::mysql_soci_error &error, bool write)
{
  auto code = static_cast<unsigned int>(error.get_backend_error_code());
  auto type = Lost(code) ? (write ? ErrorCode::OutcomeUnknown : ErrorCode::Unavailable)
                        : code == 1062 ? ErrorCode::Conflict
                        : code == 1205 || code == 3024 ? ErrorCode::Timeout : ErrorCode::Query;
  return {type, "MySQL 操作失败", code};
}
std::string QuoteOption(const std::string &value)
{
  std::string quoted = "'";
  for (char c : value)
  {
    if (c == '\0') throw std::invalid_argument("MySQL connection configuration must not contain NUL");
    if (c == '\'' || c == '\\') quoted += '\\';
    quoted += c;
  }
  return quoted + '\'';
}
// 保留调用方的 ? 参数约定；值交给 SOCI use()，不拼接到 SQL 中。
std::string Placeholders(const std::string &sql, std::size_t expected)
{
  std::string result;
  std::size_t count = 0;
  char quote = 0;
  for (std::size_t i = 0; i < sql.size(); ++i)
  {
    char c = sql[i];
    if (quote)
    {
      result += c;
      if (c == '\\' && i + 1 < sql.size()) result += sql[++i];
      else if (c == quote)
      {
        if (i + 1 < sql.size() && sql[i + 1] == quote) result += sql[++i];
        else quote = 0;
      }
    }
    else if (c == '\'' || c == '"' || c == '`') { quote = c; result += c; }
    else if (c == '?') result += ":p" + std::to_string(count++);
    else result += c;
  }
  if (count != expected) throw Error{ErrorCode::InvalidArgument, "SQL 占位参数数量不符"};
  return result;
}

class Statement
{
public:
  Statement(soci::session &session, const std::string &sql, const SqlParams &params)
      : values_(params), indicators_(params.size(), soci::i_ok), statement(session)
  {
    auto *backend = static_cast<soci::mysql_session_backend *>(session.get_backend());
    if (!params.empty() && (backend->conn_->server_status & SERVER_STATUS_NO_BACKSLASH_ESCAPES))
      throw Error{ErrorCode::InvalidArgument, "SOCI 参数绑定不支持 NO_BACKSLASH_ESCAPES"};
    statement.alloc();
    statement.prepare(Placeholders(sql, params.size()));
    for (std::size_t i = 0; i < values_.size(); ++i)
    {
      auto name = "p" + std::to_string(i);
      auto &value = values_[i];
      if (std::holds_alternative<std::nullptr_t>(value))
      {
        indicators_[i] = soci::i_null;
        value = std::string{};
      }
      if (auto *text = std::get_if<std::string>(&value))
        statement.exchange(soci::use(*text, indicators_[i], name));
      else if (auto *number = std::get_if<std::int64_t>(&value))
        statement.exchange(soci::use(*number, name));
      else if (auto *number = std::get_if<std::uint64_t>(&value))
        statement.exchange(soci::use(*number, name));
      else statement.exchange(soci::use(std::get<double>(value), name));
    }
    statement.define_and_bind();
    statement.execute();
  }
private:
  SqlParams values_;
  std::vector<soci::indicator> indicators_;
public:
  soci::statement statement;
};
}

struct MySql::Impl
{
  detail::Pool<soci::session> pool;
  explicit Impl(const MySqlConfig &config)
      : pool(config.pool_size, config.acquire_timeout,
          [config] {
            InitThread();
            std::string options = "host=" + QuoteOption(config.host)
                + " db=" + QuoteOption(config.database) + " user=" + QuoteOption(config.username)
                + " password=" + QuoteOption(config.password) + " port=" + std::to_string(config.port)
                + " charset=utf8mb4 connect_timeout=" + std::to_string(config.connect_timeout.count())
                + " read_timeout=" + std::to_string(config.io_timeout.count())
                + " write_timeout=" + std::to_string(config.io_timeout.count());
            auto session = std::make_unique<soci::session>(soci::mysql, options);
            session->set_query_context_logging_mode(soci::log_context::never);
            // SOCI 使用 mysql_real_escape_string；仅调整当前连接的兼容模式。
            *session << "SET SESSION sql_mode = TRIM(BOTH ',' FROM REPLACE("
                        "CONCAT(',', @@SESSION.sql_mode, ','), ',NO_BACKSLASH_ESCAPES,', ','))";
            return session.release();
          }, [](soci::session *session) { InitThread(); delete session; }) {}
};

struct MySql::Connection::Impl
{
  detail::Pool<soci::session>::Lease lease;
  bool transaction = false;
  explicit Impl(detail::Pool<soci::session>::Lease value) : lease(std::move(value)) {}
  ~Impl()
  {
    InitThread();
    if (transaction && lease.healthy)
    {
      try { lease.handle->rollback(); }
      catch (...) { lease.healthy = false; }
    }
  }
  Error Failure(const soci::mysql_soci_error &error, bool write)
  {
    if (Lost(error.get_backend_error_code())) lease.healthy = false;
    auto result = SqlError(error, write);
    auto *backend = static_cast<soci::mysql_session_backend *>(lease.handle->get_backend());
    result.sql_state = mysql_sqlstate(backend->conn_);
    return result;
  }
};

MySql::MySql(const MySqlConfig &config) : impl_(std::make_unique<Impl>(config)) {}
MySql::~MySql() = default;
MySql::Connection::Connection(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
MySql::Connection::~Connection() = default;
MySql::Connection::Connection(Connection &&) noexcept = default;
MySql::Connection &MySql::Connection::operator=(Connection &&) noexcept = default;

Result<MySql::Connection> MySql::Acquire()
{
  try
  {
    InitThread();
    auto lease = impl_->pool.Acquire();
    if (!lease) return Error{ErrorCode::PoolExhausted, "MySQL 连接池繁忙"};
    return Connection(std::make_unique<Connection::Impl>(std::move(*lease)));
  }
  catch (...) { return Error{ErrorCode::Unavailable, "MySQL 连接不可用"}; }
}

Result<Rows> MySql::Connection::Query(const std::string &sql, const SqlParams &params)
{
  if (!impl_->lease.healthy) return Error{ErrorCode::Unavailable, "MySQL 连接不可用"};
  try
  {
    InitThread();
    Statement query(*impl_->lease.handle, sql, params);
    auto *backend = static_cast<soci::mysql_statement_backend *>(query.statement.get_backend());
    Rows rows;
    if (!backend->result_) return rows;
    // 只读取原生元数据；取值走 SOCI 的 string into，保留 DECIMAL、日期和二进制原值。
    auto count = mysql_num_fields(backend->result_);
    auto *fields = mysql_fetch_fields(backend->result_);
    if (backend->get_number_of_rows() > 2000) return Error{ErrorCode::Query, "查询结果过大"};
    std::vector<std::string> values(count);
    std::vector<soci::indicator> indicators(count);
    std::vector<std::unique_ptr<soci::details::standard_into_type_backend>> bindings;
    int position = 1;
    for (unsigned int i = 0; i < count; ++i)
    {
      rows.columns.emplace_back(fields[i].name);
      auto binding = std::unique_ptr<soci::details::standard_into_type_backend>(query.statement.make_into_type_backend());
      binding->define_by_pos(position, &values[i], soci::details::x_stdstring);
      binding->pre_fetch();
      bindings.push_back(std::move(binding));
    }
    std::size_t bytes = 0;
    while (backend->fetch(1) == soci::details::statement_backend::ef_success)
    {
      std::vector<std::optional<std::string>> row;
      for (unsigned int i = 0; i < count; ++i)
      {
        bindings[i]->post_fetch(true, true, &indicators[i]);
        if (indicators[i] == soci::i_null) row.emplace_back(std::nullopt);
        else
        {
          if (values[i].size() > 16 * 1024 * 1024 - bytes) return Error{ErrorCode::Query, "查询结果过大"};
          bytes += values[i].size();
          row.emplace_back(std::move(values[i]));
        }
      }
      rows.values.push_back(std::move(row));
    }
    return rows;
  }
  catch (const Error &error) { return error; }
  catch (const soci::mysql_soci_error &error) { return impl_->Failure(error, false); }
  catch (...) { return Error{ErrorCode::Query, "读取 MySQL 结果失败"}; }
}

Result<WriteResult> MySql::Connection::Execute(const std::string &sql, const SqlParams &params)
{
  if (!impl_->lease.healthy) return Error{ErrorCode::Unavailable, "MySQL 连接不可用"};
  try
  {
    InitThread();
    Statement query(*impl_->lease.handle, sql, params);
    long long insert_id = 0;
    impl_->lease.handle->get_last_insert_id("", insert_id);
    return WriteResult{static_cast<std::uint64_t>(query.statement.get_affected_rows()), static_cast<std::uint64_t>(insert_id)};
  }
  catch (const Error &error) { return error; }
  catch (const soci::mysql_soci_error &error) { return impl_->Failure(error, true); }
  catch (...) { return Error{ErrorCode::Query, "执行 MySQL 语句失败"}; }
}
Status MySql::Connection::Begin()
{
  if (!impl_->lease.healthy) return Error{ErrorCode::Unavailable, "MySQL 连接不可用"};
  if (impl_->transaction) return Error{ErrorCode::InvalidArgument, "事务已经开始"};
  try
  {
    InitThread();
    impl_->lease.handle->begin();
    impl_->transaction = true;
    return std::monostate{};
  }
  catch (const soci::mysql_soci_error &error) { return impl_->Failure(error, false); }
  catch (...) { return Error{ErrorCode::Query, "MySQL 开始事务失败"}; }
}
Status MySql::Connection::Commit()
{
  if (!impl_->transaction) return Error{ErrorCode::InvalidArgument, "没有事务"};
  if (!impl_->lease.healthy) return Error{ErrorCode::OutcomeUnknown, "MySQL 提交结果未知"};
  try
  {
    InitThread();
    impl_->lease.handle->commit();
    impl_->transaction = false;
    return std::monostate{};
  }
  catch (...)
  {
    impl_->lease.healthy = false;
    return Error{ErrorCode::OutcomeUnknown, "MySQL 提交结果未知"};
  }
}
Status MySql::Connection::Rollback()
{
  if (!impl_->transaction) return std::monostate{};
  if (!impl_->lease.healthy) return Error{ErrorCode::Unavailable, "MySQL 连接不可用"};
  try
  {
    InitThread();
    impl_->lease.handle->rollback();
    impl_->transaction = false;
    return std::monostate{};
  }
  catch (...)
  {
    impl_->lease.healthy = false;
    return Error{ErrorCode::Unavailable, "MySQL 回滚失败"};
  }
}
} // namespace db
