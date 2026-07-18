#include "MySqlClient.h"

#include <Windows.h>
#include <mysql/mysql.h>

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace videosc::dedup {
namespace {

/** @brief 将 UTF-16 配置转换为严格 UTF-8。 */
std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8,
                                           WC_ERR_INVALID_CHARS,
                                           value.data(),
                                           static_cast<int>(value.size()),
                                           nullptr,
                                           0,
                                           nullptr,
                                           nullptr);
    if (length <= 0) throw std::runtime_error("Cannot encode MySQL configuration as UTF-8");
    std::string result(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8,
                            WC_ERR_INVALID_CHARS,
                            value.data(),
                            static_cast<int>(value.size()),
                            result.data(),
                            length,
                            nullptr,
                            nullptr) != length) {
        throw std::runtime_error("Cannot encode MySQL configuration as UTF-8");
    }
    return result;
}

/** @brief 将配置 TLS 枚举映射为 MySQL 客户端枚举。 */
mysql_ssl_mode ToNativeTlsMode(const MySqlTlsMode mode) {
    switch (mode) {
        case MySqlTlsMode::Disabled: return SSL_MODE_DISABLED;
        case MySqlTlsMode::Preferred: return SSL_MODE_PREFERRED;
        case MySqlTlsMode::Required: return SSL_MODE_REQUIRED;
        case MySqlTlsMode::VerifyCa: return SSL_MODE_VERIFY_CA;
        case MySqlTlsMode::VerifyIdentity:
            return SSL_MODE_VERIFY_IDENTITY;
    }
    return SSL_MODE_DISABLED;
}

/** @brief 从当前连接复制原生错误，避免返回指针越过连接生命周期。 */
MySqlStatus NativeFailure(MYSQL* connection, const std::string& fallback) {
    MySqlStatus status;
    status.native_error = connection == nullptr ? 0U : mysql_errno(connection);
    const char* message = connection == nullptr ? nullptr : mysql_error(connection);
    status.message = message != nullptr && message[0] != '\0' ? message : fallback;
    return status;
}

/** @brief 构造无敏感信息的成功状态。 */
MySqlStatus Success() {
    MySqlStatus status;
    status.succeeded = true;
    return status;
}

}  // namespace

class MySqlClient::Impl final {
public:
    explicit Impl(DatabaseConfig value) : config(std::move(value)) {}

    ~Impl() {
        DisconnectUnlocked();
        if (!config.password.empty()) {
            SecureZeroMemory(config.password.data(), config.password.size() * sizeof(wchar_t));
            config.password.clear();
        }
    }

    /** @brief 关闭连接，调用方必须持有 mutex 或处于析构阶段。 */
    void DisconnectUnlocked() noexcept {
        if (connection != nullptr) {
            mysql_close(connection);
            connection = nullptr;
        }
    }

    DatabaseConfig config;
    MYSQL* connection = nullptr;
    mutable std::mutex mutex;
};

MySqlClient::MySqlClient(DatabaseConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}

MySqlClient::~MySqlClient() = default;

MySqlStatus MySqlClient::Connect() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->DisconnectUnlocked();

    static std::once_flag library_flag;
    static int library_result = 1;
    std::call_once(library_flag, [] { library_result = mysql_library_init(0, nullptr, nullptr); });
    if (library_result != 0) return NativeFailure(nullptr, "mysql_library_init failed");

    MYSQL* connection = mysql_init(nullptr);
    if (connection == nullptr) return NativeFailure(nullptr, "mysql_init failed");

    const unsigned int connectTimeout = impl_->config.connect_timeout_seconds;
    const unsigned int commandTimeout = impl_->config.command_timeout_seconds;
    const mysql_ssl_mode sslMode = ToNativeTlsMode(impl_->config.tls_mode);
    // allowPublicKeyRetrieval=true：MySQL 8.0 默认 root 使用 caching_sha2_password，
    // 非 SSL 连接时客户端需要从服务器获取公钥才能完成密码加密认证。
    const unsigned int allowPublicKeyRetrieval = 1;
    if (mysql_options(connection, MYSQL_OPT_CONNECT_TIMEOUT, &connectTimeout) != 0 ||
        mysql_options(connection, MYSQL_OPT_READ_TIMEOUT, &commandTimeout) != 0 ||
        mysql_options(connection, MYSQL_OPT_WRITE_TIMEOUT, &commandTimeout) != 0 ||
        mysql_options(connection, MYSQL_OPT_SSL_MODE, &sslMode) != 0 ||
        mysql_options(connection, MYSQL_OPT_GET_SERVER_PUBLIC_KEY, &allowPublicKeyRetrieval) != 0) {
        MySqlStatus failure = NativeFailure(connection, "Cannot configure MySQL connection");
        mysql_close(connection);
        return failure;
    }

    try {
        const std::string key = impl_->config.tls_private_key_path.empty()
                                    ? std::string{}
                                    : WideToUtf8(impl_->config.tls_private_key_path.wstring());
        const std::string certificate = impl_->config.tls_certificate_path.empty()
                                            ? std::string{}
                                            : WideToUtf8(impl_->config.tls_certificate_path.wstring());
        const std::string ca = impl_->config.tls_ca_path.empty()
                                   ? std::string{}
                                   : WideToUtf8(impl_->config.tls_ca_path.wstring());
        if (!key.empty() || !certificate.empty() || !ca.empty()) {
            mysql_ssl_set(connection,
                          key.empty() ? nullptr : key.c_str(),
                          certificate.empty() ? nullptr : certificate.c_str(),
                          ca.empty() ? nullptr : ca.c_str(),
                          nullptr,
                          nullptr);
        }
        const std::string host = WideToUtf8(impl_->config.host);
        const std::string database = WideToUtf8(impl_->config.database_name);
        const std::string user = WideToUtf8(impl_->config.user_name);
        std::string password = WideToUtf8(impl_->config.password);
        MYSQL* connected = mysql_real_connect(connection,
                                              host.c_str(),
                                              user.c_str(),
                                              password.c_str(),
                                              database.c_str(),
                                              impl_->config.port,
                                              nullptr,
                                              0);
        if (!password.empty()) SecureZeroMemory(password.data(), password.size());
        if (connected == nullptr) {
            MySqlStatus failure = NativeFailure(connection, "mysql_real_connect failed");
            mysql_close(connection);
            return failure;
        }
    } catch (const std::exception& exception) {
        mysql_close(connection);
        return {false, 0, exception.what()};
    }

    if (mysql_set_character_set(connection, "utf8mb4") != 0) {
        MySqlStatus failure = NativeFailure(connection, "Cannot select utf8mb4");
        mysql_close(connection);
        return failure;
    }
    impl_->connection = connection;
    return Success();
}

void MySqlClient::Disconnect() noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->DisconnectUnlocked();
}

bool MySqlClient::is_connected() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->connection != nullptr;
}

MySqlStatus MySqlClient::Ping() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->connection == nullptr) return NativeFailure(nullptr, "MySQL is not connected");
    if (mysql_ping(impl_->connection) != 0) return NativeFailure(impl_->connection, "mysql_ping failed");
    return Success();
}

MySqlStatus MySqlClient::Execute(const std::string& sql) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->connection == nullptr) return NativeFailure(nullptr, "MySQL is not connected");
    if (mysql_real_query(impl_->connection, sql.data(), static_cast<unsigned long>(sql.size())) != 0) {
        return NativeFailure(impl_->connection, "MySQL command failed");
    }
    MYSQL_RES* unexpected = mysql_store_result(impl_->connection);
    if (unexpected != nullptr) mysql_free_result(unexpected);
    return Success();
}

MySqlStatus MySqlClient::ExecuteTransaction(const std::vector<std::string>& statements) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->connection == nullptr) return NativeFailure(nullptr, "MySQL is not connected");
    if (mysql_real_query(impl_->connection, "START TRANSACTION", 17) != 0) {
        return NativeFailure(impl_->connection, "Cannot start MySQL transaction");
    }
    for (const std::string& statement : statements) {
        if (mysql_real_query(impl_->connection,
                             statement.data(),
                             static_cast<unsigned long>(statement.size())) != 0) {
            MySqlStatus failure = NativeFailure(impl_->connection, "MySQL transaction statement failed");
            mysql_rollback(impl_->connection);
            return failure;
        }
        MYSQL_RES* unexpected = mysql_store_result(impl_->connection);
        if (unexpected != nullptr) mysql_free_result(unexpected);
    }
    if (mysql_commit(impl_->connection) != 0) {
        MySqlStatus failure = NativeFailure(impl_->connection, "Cannot commit MySQL transaction");
        mysql_rollback(impl_->connection);
        return failure;
    }
    return Success();
}

MySqlStatus MySqlClient::Query(const std::string& sql, const RowVisitor& visitor) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->connection == nullptr) return NativeFailure(nullptr, "MySQL is not connected");
    if (!visitor) return NativeFailure(nullptr, "MySQL row visitor is empty");
    if (mysql_real_query(impl_->connection, sql.data(), static_cast<unsigned long>(sql.size())) != 0) {
        return NativeFailure(impl_->connection, "MySQL query failed");
    }
    MYSQL_RES* result = mysql_use_result(impl_->connection);
    if (result == nullptr) return NativeFailure(impl_->connection, "MySQL query returned no result set");
    const unsigned int columnCount = mysql_num_fields(result);
    try {
        while (MYSQL_ROW nativeRow = mysql_fetch_row(result)) {
            const unsigned long* lengths = mysql_fetch_lengths(result);
            MySqlRow row;
            row.reserve(columnCount);
            for (unsigned int index = 0; index < columnCount; ++index) {
                if (nativeRow[index] == nullptr) {
                    row.emplace_back(std::nullopt);
                } else {
                    row.emplace_back(std::string(nativeRow[index], lengths[index]));
                }
            }
            if (!visitor(row)) break;
        }
    } catch (const std::exception& exception) {
        mysql_free_result(result);
        return {false, 0, exception.what()};
    }
    const unsigned int fetchError = mysql_errno(impl_->connection);
    MySqlStatus status = fetchError == 0 ? Success() : NativeFailure(impl_->connection, "MySQL row fetch failed");
    mysql_free_result(result);
    return status;
}

MySqlStatus MySqlClient::EscapeLiteral(const std::string& value, std::string& literal) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->connection == nullptr) return NativeFailure(nullptr, "MySQL is not connected");
    std::string escaped(value.size() * 2 + 1, '\0');
    const unsigned long length = mysql_real_escape_string(impl_->connection,
                                                           escaped.data(),
                                                           value.data(),
                                                           static_cast<unsigned long>(value.size()));
    escaped.resize(length);
    literal.clear();
    literal.reserve(escaped.size() + 2);
    literal.push_back('\'');
    literal += escaped;
    literal.push_back('\'');
    return Success();
}

}  // namespace videosc::dedup
