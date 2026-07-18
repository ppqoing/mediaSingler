#include "MySqlSyncService.h"
#include "../logging/ApplicationErrorLogger.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <iomanip>
#include <locale>
#include <map>
#include <new>
#include <set>
#include <sstream>
#include <stdexcept>

#pragma comment(lib, "bcrypt.lib")

namespace videosc::dedup {
namespace {

constexpr std::size_t kPathHashBytes = 32;

/** @brief 返回当前 Unix UTC 毫秒。 */
std::int64_t UtcNowMilliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/** @brief 严格 UTF-16 到 UTF-8 转换。 */
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
    if (length <= 0) throw std::runtime_error("Cannot encode MySQL payload as UTF-8");
    std::string result(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8,
                            WC_ERR_INVALID_CHARS,
                            value.data(),
                            static_cast<int>(value.size()),
                            result.data(),
                            length,
                            nullptr,
                            nullptr) != length) {
        throw std::runtime_error("Cannot encode MySQL payload as UTF-8");
    }
    return result;
}

/** @brief 把二进制字节编码为大写十六进制，不使用数据库 HEX 函数。 */
std::string HexBytes(const std::uint8_t* bytes, const std::size_t size) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string value(size * 2, '0');
    for (std::size_t index = 0; index < size; ++index) {
        value[index * 2] = digits[bytes[index] >> 4];
        value[index * 2 + 1] = digits[bytes[index] & 0x0F];
    }
    return value;
}

/** @brief 使用 Windows CNG 计算规范化路径的 SHA-256 索引键。 */
std::array<std::uint8_t, kPathHashBytes> HashNormalizedPath(const std::string& normalizedPath) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectLength = 0;
    DWORD returned = 0;
    std::vector<std::uint8_t> object;
    std::array<std::uint8_t, kPathHashBytes> digest{};
    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status >= 0) {
        status = BCryptGetProperty(algorithm,
                                   BCRYPT_OBJECT_LENGTH,
                                   reinterpret_cast<PUCHAR>(&objectLength),
                                   sizeof(objectLength),
                                   &returned,
                                   0);
    }
    if (status >= 0) {
        object.resize(objectLength);
        status = BCryptCreateHash(algorithm,
                                  &hash,
                                  object.data(),
                                  static_cast<ULONG>(object.size()),
                                  nullptr,
                                  0,
                                  0);
    }
    if (status >= 0 && !normalizedPath.empty()) {
        status = BCryptHashData(hash,
                                reinterpret_cast<PUCHAR>(const_cast<char*>(normalizedPath.data())),
                                static_cast<ULONG>(normalizedPath.size()),
                                0);
    }
    if (status >= 0) status = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    if (hash != nullptr) BCryptDestroyHash(hash);
    if (algorithm != nullptr) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) throw std::runtime_error("Cannot hash normalized path");
    return digest;
}

/** @brief 以经典区域设置输出可往返的 double。 */
std::string DoubleLiteral(const double value) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(17) << value;
    return stream.str();
}

/** @brief 将六个 dHash 按网络字节序编码为固定 48 字节。 */
std::string VideoDhashHex(const std::array<std::uint64_t, 6>& hashes) {
    std::array<std::uint8_t, 48> bytes{};
    for (std::size_t hashIndex = 0; hashIndex < hashes.size(); ++hashIndex) {
        for (std::size_t byteIndex = 0; byteIndex < 8; ++byteIndex) {
            bytes[hashIndex * 8 + byteIndex] =
                static_cast<std::uint8_t>(hashes[hashIndex] >> ((7 - byteIndex) * 8));
        }
    }
    return HexBytes(bytes.data(), bytes.size());
}

/** @brief 将 16 个分区 pHash 按网络字节序编码为固定 128 字节。 */
std::string ZonedPHashesHex(const std::array<std::uint64_t, 16>& hashes) {
    std::array<std::uint8_t, 128> bytes{};
    for (std::size_t hashIndex = 0; hashIndex < hashes.size(); ++hashIndex) {
        for (std::size_t byteIndex = 0; byteIndex < 8; ++byteIndex) {
            bytes[hashIndex * 8 + byteIndex] =
                static_cast<std::uint8_t>(hashes[hashIndex] >> ((7 - byteIndex) * 8));
        }
    }
    return HexBytes(bytes.data(), bytes.size());
}

/** @brief 判断路径状态是否应出现在当前精确重复报告中。 */
bool IsActiveState(const FilePathState state) {
    return state == FilePathState::Available || state == FilePathState::Unchanged;
}

/** @brief 使用连接转义 UTF-8 文本，并在失败时抛出不含原值的异常。 */
std::string Escape(MySqlClient& client, const std::string& value) {
    std::string literal;
    const MySqlStatus status = client.EscapeLiteral(value, literal);
    if (!status.succeeded) throw std::runtime_error(status.message);
    return literal;
}

/** @brief 构造内容记录批量 upsert。 */
std::string BuildContentUpsert(MySqlClient& client, const std::vector<const ShaFileData*>& items) {
    std::string sql =
        "INSERT INTO sha512_file_data(sha512,content_size_bytes,media_kind,mime_type,container_name,width,height,"
        "image_dhash,image_pdq_hash,image_pdq_quality,image_zoned_phashes,image_perceptual_algorithm_version,"
        "image_structural_algorithm_version,video_duration_ms,video_frame_rate,video_bitrate,video_codec,pixel_format,video_dhashes,"
        "has_video_dhashes,static_visual,contact_sheet_path,media_algorithm_version) VALUES ";
    for (std::size_t index = 0; index < items.size(); ++index) {
        const ShaFileData& data = *items[index];
        if (index != 0) sql += ',';
        sql += "(X'" + Sha512ToHex(data.sha512) + "'," + std::to_string(data.content_size_bytes) + ',' +
               std::to_string(static_cast<int>(data.media_kind)) + ',' + Escape(client, data.mime_type) + ',' +
               Escape(client, data.container_name) + ',' + std::to_string(data.width) + ',' +
               std::to_string(data.height) + ',';
        sql += data.image_dhash.has_value() ? std::to_string(*data.image_dhash) : "NULL";
        sql += data.image_pdq_hash.has_value()
                   ? ",X'" + HexBytes(data.image_pdq_hash->data(), data.image_pdq_hash->size()) + "'"
                   : ",NULL";
        sql += data.image_pdq_quality.has_value() ? ',' + std::to_string(*data.image_pdq_quality) : ",NULL";
        sql += data.has_image_zoned_phashes
                   ? ",X'" + ZonedPHashesHex(data.image_zoned_phashes) + "'"
                   : ",NULL";
        sql += ',' + std::to_string(data.image_perceptual_algorithm_version) + ',' +
               std::to_string(data.image_structural_algorithm_version);
        sql += ',' + std::to_string(data.video_duration_ms) + ',' + DoubleLiteral(data.video_frame_rate) + ',' +
               std::to_string(data.video_bitrate) + ',' + Escape(client, data.video_codec) + ',' +
               Escape(client, data.pixel_format) + ',';
        sql += data.has_video_dhashes ? "X'" + VideoDhashHex(data.video_dhashes) + "'" : "NULL";
        sql += ',' + std::to_string(data.has_video_dhashes ? 1 : 0) + ',' +
               std::to_string(data.static_visual ? 1 : 0) + ',' +
               Escape(client, WideToUtf8(data.contact_sheet_path.wstring())) + ',' +
               Escape(client, data.media_algorithm_version) + ')';
    }
    sql +=
        " AS incoming ON DUPLICATE KEY UPDATE "
        "content_size_bytes=incoming.content_size_bytes,media_kind=incoming.media_kind,mime_type=incoming.mime_type,"
        "container_name=incoming.container_name,width=incoming.width,height=incoming.height,image_dhash=incoming.image_dhash,"
        "image_pdq_hash=incoming.image_pdq_hash,image_pdq_quality=incoming.image_pdq_quality,"
        "image_zoned_phashes=incoming.image_zoned_phashes,"
        "image_perceptual_algorithm_version=incoming.image_perceptual_algorithm_version,"
        "image_structural_algorithm_version=incoming.image_structural_algorithm_version,"
        "video_duration_ms=incoming.video_duration_ms,video_frame_rate=incoming.video_frame_rate,"
        "video_bitrate=incoming.video_bitrate,video_codec=incoming.video_codec,pixel_format=incoming.pixel_format,"
        "video_dhashes=incoming.video_dhashes,has_video_dhashes=incoming.has_video_dhashes,"
        "static_visual=incoming.static_visual,contact_sheet_path=incoming.contact_sheet_path,"
        "media_algorithm_version=incoming.media_algorithm_version";
    return sql;
}

/** @brief 构造路径映射批量 upsert。 */
std::string BuildPathUpsert(MySqlClient& client, const std::vector<const FilePathRecord*>& items) {
    std::string sql =
        "INSERT INTO file_path_sha512(path_id,path_hash,full_path,normalized_path,sha512,scan_id,volume_serial,"
        "file_id_high,file_id_low,volume_guid,storage_target_key,size_bytes,extension,creation_time_utc_ms,"
        "last_write_time_utc_ms,scan_root_priority,path_state,active) VALUES ";
    for (std::size_t index = 0; index < items.size(); ++index) {
        const FilePathRecord& path = *items[index];
        if (!path.sha512.has_value()) throw std::runtime_error("Path sync operation has no SHA-512");
        const std::string normalized = WideToUtf8(path.normalized_path_key);
        const auto pathHash = HashNormalizedPath(normalized);
        if (index != 0) sql += ',';
        sql += '(' + std::to_string(path.path_id) + ",X'" + HexBytes(pathHash.data(), pathHash.size()) + "'," +
               Escape(client, WideToUtf8(path.path.wstring())) + ',' + Escape(client, normalized) + ",X'" +
               Sha512ToHex(*path.sha512) + "'," + std::to_string(path.scan_id) + ',' +
               std::to_string(path.identity.volume_serial) + ',' + std::to_string(path.identity.file_id_high) + ',' +
               std::to_string(path.identity.file_id_low) + ',' + Escape(client, WideToUtf8(path.volume_guid)) + ',' +
               Escape(client, WideToUtf8(path.storage_target_key)) + ',' + std::to_string(path.size_bytes) + ',' +
               Escape(client, WideToUtf8(path.extension)) + ',' + std::to_string(path.creation_time_utc_ms) + ',' +
               std::to_string(path.last_write_time_utc_ms) + ',' + std::to_string(path.scan_root_priority) + ',' +
               std::to_string(static_cast<int>(path.state)) + ',' + std::to_string(IsActiveState(path.state) ? 1 : 0) + ')';
    }
    sql +=
        " AS incoming ON DUPLICATE KEY UPDATE path_hash=incoming.path_hash,full_path=incoming.full_path,"
        "normalized_path=incoming.normalized_path,sha512=incoming.sha512,scan_id=incoming.scan_id,"
        "volume_serial=incoming.volume_serial,file_id_high=incoming.file_id_high,file_id_low=incoming.file_id_low,"
        "volume_guid=incoming.volume_guid,storage_target_key=incoming.storage_target_key,size_bytes=incoming.size_bytes,"
        "extension=incoming.extension,creation_time_utc_ms=incoming.creation_time_utc_ms,"
        "last_write_time_utc_ms=incoming.last_write_time_utc_ms,scan_root_priority=incoming.scan_root_priority,"
        "path_state=incoming.path_state,active=incoming.active";
    return sql;
}

/** @brief 构造带期望 SHA-512 条件的批量路径删除。 */
std::string BuildPathDelete(const std::vector<const SyncOperation*>& items) {
    std::string sql = "DELETE FROM file_path_sha512 WHERE ";
    for (std::size_t index = 0; index < items.size(); ++index) {
        const SyncOperation& operation = *items[index];
        if (!operation.expected_sha512.has_value() || operation.delete_path_id == 0) {
            throw std::runtime_error("Path delete sync operation is incomplete");
        }
        if (index != 0) sql += " OR ";
        sql += "(path_id=" + std::to_string(operation.delete_path_id) + " AND sha512=X'" +
               Sha512ToHex(*operation.expected_sha512) + "')";
    }
    return sql;
}

}  // namespace

MySqlSyncExecutor::MySqlSyncExecutor(MySqlClient& client) : client_(client) {}

MySqlStatus MySqlSyncExecutor::Apply(const std::vector<SyncOperation>& operations) {
    if (operations.empty()) return {true, 0, {}};
    try {
        std::vector<const ShaFileData*> contents;
        std::vector<const FilePathRecord*> paths;
        std::vector<const SyncOperation*> deletes;
        std::map<std::uint64_t, std::set<SyncOperationKind>> pathKinds;
        for (const SyncOperation& operation : operations) {
            switch (operation.kind) {
                case SyncOperationKind::UpsertShaFileData:
                    if (!operation.sha_file_data.has_value()) throw std::runtime_error("Missing content sync payload");
                    contents.push_back(&*operation.sha_file_data);
                    break;
                case SyncOperationKind::UpsertFilePath:
                    if (!operation.file_path.has_value()) throw std::runtime_error("Missing path sync payload");
                    paths.push_back(&*operation.file_path);
                    pathKinds[operation.file_path->path_id].insert(operation.kind);
                    break;
                case SyncOperationKind::DeleteFilePath:
                    deletes.push_back(&operation);
                    pathKinds[operation.delete_path_id].insert(operation.kind);
                    break;
            }
        }

        const bool hasMixedPathOperations = std::any_of(
            pathKinds.begin(), pathKinds.end(), [](const auto& item) { return item.second.size() > 1; });
        std::vector<std::string> statements;
        if (hasMixedPathOperations) {
            for (const SyncOperation& operation : operations) {
                if (operation.kind == SyncOperationKind::UpsertShaFileData) {
                    statements.push_back(BuildContentUpsert(client_, {&*operation.sha_file_data}));
                } else if (operation.kind == SyncOperationKind::UpsertFilePath) {
                    statements.push_back(BuildPathUpsert(client_, {&*operation.file_path}));
                } else {
                    statements.push_back(BuildPathDelete({&operation}));
                }
            }
        } else {
            if (!contents.empty()) statements.push_back(BuildContentUpsert(client_, contents));
            if (!paths.empty()) statements.push_back(BuildPathUpsert(client_, paths));
            if (!deletes.empty()) statements.push_back(BuildPathDelete(deletes));
        }
        return client_.ExecuteTransaction(statements);
    } catch (const std::exception& exception) {
        return {false, 0, exception.what()};
    }
}

MySqlSyncService::MySqlSyncService(MySqlSyncQueue& queue,
                                   MySqlClient& client,
                                   const std::uint32_t batch_size,
                                   const std::uint32_t base_retry_seconds,
                                   LoggingConfig logging)
    : queue_(queue),
      client_(client),
      executor_(client),
      execution_logger_(std::move(logging)),
      batch_size_(std::max<std::uint32_t>(1, batch_size)),
      base_retry_seconds_(std::max<std::uint32_t>(1, base_retry_seconds)) {}

MySqlSyncService::~MySqlSyncService() {
    Stop();
}

void MySqlSyncService::Start() {
    if (worker_.joinable()) return;
    stop_requested_.store(false);
    worker_ = std::thread(&MySqlSyncService::WorkerLoop, this);
}

void MySqlSyncService::Stop() {
    RequestStop();
    Wait();
}

void MySqlSyncService::RequestStop() noexcept {
    stop_requested_.store(true);
    wake_condition_.notify_all();
}

void MySqlSyncService::Wait() {
    if (worker_.joinable()) worker_.join();
}

bool MySqlSyncService::IsRunning() const {
    return snapshot().running;
}

void MySqlSyncService::Wake() {
    wake_condition_.notify_all();
}

MySqlSyncSnapshot MySqlSyncService::snapshot() const {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    return snapshot_;
}

void MySqlSyncService::WaitFor(const std::uint32_t milliseconds) {
    std::unique_lock<std::mutex> lock(wait_mutex_);
    wake_condition_.wait_for(lock,
                             std::chrono::milliseconds(milliseconds),
                             [&] { return stop_requested_.load(); });
}

void MySqlSyncService::RecordFailure(const MySqlStatus& status) {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    snapshot_.connected = false;
    snapshot_.last_native_error = status.native_error;
    snapshot_.last_error = status.message;
}

void MySqlSyncService::WorkerLoop() {
    const bool initiallyConnected = client_.is_connected();
    {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        snapshot_.running = true;
        // 运行时前置检查会预先连接并校验 schema，快照必须继承该有效连接状态。
        snapshot_.connected = initiallyConnected;
    }
    std::string executionLogError;
    const auto logThreadFailure = [&](const std::string& detail) {
        ExecutionFailureRecord failure;
        failure.task = "mysql_sync";
        failure.stage = "thread";
        failure.operation = "unhandled_exception";
        failure.status = "failed";
        failure.detail = detail;
        return execution_logger_.WriteFailure(failure, executionLogError);
    };
    try {
    if (!execution_logger_.EnsureWritable(executionLogError) ||
        !execution_logger_.WriteEvent(
            {0, "mysql_sync", "started", "background_sync", "MySQL 后台同步已启动", 0, 0},
            executionLogError)) {
        RecordFailure({false, 0, "执行日志不可写：" + executionLogError});
        throw std::runtime_error("mysql_sync_execution_log_unavailable");
    }
    while (!stop_requested_.load()) {
        if (!client_.is_connected()) {
            const MySqlStatus connect = client_.Connect();
            if (!connect.succeeded) {
                ExecutionFailureRecord failure;
                failure.task = "mysql_sync";
                failure.stage = "connect";
                failure.operation = "mysql_connect";
                failure.status = "failed";
                failure.native_error = connect.native_error;
                failure.detail = connect.message;
                if (!execution_logger_.WriteFailure(failure, executionLogError)) {
                    throw std::runtime_error("mysql_sync_failure_log_write_failed: " + executionLogError);
                }
                RecordFailure(connect);
                WaitFor(base_retry_seconds_ * 1000U);
                continue;
            }
            std::lock_guard<std::mutex> lock(snapshot_mutex_);
            snapshot_.connected = true;
            snapshot_.last_error.clear();
            snapshot_.last_native_error = 0;
        }

        std::vector<SyncOperation> operations;
        const RocksStatus read = queue_.ReadBatch(batch_size_, operations);
        if (!read.succeeded) {
            ExecutionFailureRecord failure;
            failure.task = "mysql_sync";
            failure.stage = "queue_read";
            failure.operation = "rocksdb_read_batch";
            failure.status = "failed";
            failure.detail = read.message;
            if (!execution_logger_.WriteFailure(failure, executionLogError)) {
                throw std::runtime_error("mysql_sync_failure_log_write_failed: " + executionLogError);
            }
            RecordFailure({false, 0, read.message});
            WaitFor(base_retry_seconds_ * 1000U);
            continue;
        }
        if (operations.empty()) {
            WaitFor(1000);
            continue;
        }
        const std::int64_t now = UtcNowMilliseconds();
        if (operations.front().next_attempt_utc_ms > now) {
            const std::int64_t remaining = operations.front().next_attempt_utc_ms - now;
            WaitFor(static_cast<std::uint32_t>(std::min<std::int64_t>(remaining, 60000)));
            continue;
        }
        const auto firstDelayed = std::find_if(operations.begin(), operations.end(), [&](const SyncOperation& operation) {
            return operation.next_attempt_utc_ms > now;
        });
        operations.erase(firstDelayed, operations.end());

        const MySqlStatus applied = executor_.Apply(operations);
        if (!applied.succeeded) {
            client_.Disconnect();
            for (SyncOperation& operation : operations) {
                ExecutionFailureRecord failure;
                failure.task_id = operation.batch_scan_id;
                failure.task = "mysql_sync";
                failure.stage = "apply_batch";
                failure.operation = "mysql_transaction";
                failure.status = "failed";
                failure.native_error = applied.native_error;
                failure.detail = applied.message;
                if (operation.file_path.has_value()) {
                    failure.path_id = operation.file_path->path_id;
                    failure.path = operation.file_path->path;
                    failure.storage_target_key = operation.file_path->storage_target_key;
                }
                if (!execution_logger_.WriteFailure(failure, executionLogError)) {
                    throw std::runtime_error("mysql_sync_failure_log_write_failed: " + executionLogError);
                }
                operation.attempt_count = std::min<std::uint32_t>(operation.attempt_count + 1, 31);
                const std::uint32_t shift = std::min<std::uint32_t>(operation.attempt_count - 1, 6);
                const std::uint64_t delaySeconds =
                    std::min<std::uint64_t>(static_cast<std::uint64_t>(base_retry_seconds_) << shift, 300);
                operation.next_attempt_utc_ms = now + static_cast<std::int64_t>(delaySeconds * 1000);
                operation.last_native_error = applied.native_error;
                const RocksStatus retry = queue_.SaveRetry(operation);
                if (!retry.succeeded) RecordFailure({false, 0, retry.message});
            }
            RecordFailure(applied);
            WaitFor(base_retry_seconds_ * 1000U);
            continue;
        }

        std::vector<std::uint64_t> sequences;
        sequences.reserve(operations.size());
        for (const SyncOperation& operation : operations) sequences.push_back(operation.sequence);
        const RocksStatus acknowledged = queue_.Acknowledge(sequences);
        if (!acknowledged.succeeded) {
            ExecutionFailureRecord failure;
            failure.task_id = operations.front().batch_scan_id;
            failure.task = "mysql_sync";
            failure.stage = "acknowledge";
            failure.operation = "rocksdb_acknowledge";
            failure.status = "failed";
            failure.detail = acknowledged.message;
            if (!execution_logger_.WriteFailure(failure, executionLogError)) {
                throw std::runtime_error("mysql_sync_failure_log_write_failed: " + executionLogError);
            }
            RecordFailure({false, 0, acknowledged.message});
            WaitFor(base_retry_seconds_ * 1000U);
            continue;
        }
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        snapshot_.connected = true;
        snapshot_.synchronized_operations += operations.size();
        snapshot_.last_batch_size = operations.size();
        snapshot_.last_success_utc_ms = UtcNowMilliseconds();
        snapshot_.last_native_error = 0;
        snapshot_.last_error.clear();
        if (!execution_logger_.WriteEvent(
                {operations.front().batch_scan_id,
                 "mysql_sync",
                 "batch_completed",
                 "apply_batch",
                 "MySQL 同步批次完成",
                 operations.size(),
                 operations.size()},
                executionLogError)) {
            throw std::runtime_error("mysql_sync_event_log_write_failed: " + executionLogError);
        }
    }
    } catch (const std::bad_alloc&) {
        logThreadFailure("MySQL 同步线程内存不足");
        RecordFailure({false, 0, "MySQL 同步线程内存不足"});
        ApplicationErrorLogger::Write(
            {"error", "thread_exception", "DedupCore", "mysql_sync",
             "MySQL 同步线程内存不足", "std::bad_alloc", {}, 0});
    } catch (const std::exception& exception) {
        logThreadFailure(exception.what());
        RecordFailure({false, 0, exception.what()});
        ApplicationErrorLogger::Write(
            {"error", "thread_exception", "DedupCore", "mysql_sync",
             exception.what(), "std::exception", {}, 0});
    } catch (...) {
        logThreadFailure("MySQL 同步线程发生未知异常");
        RecordFailure({false, 0, "MySQL 同步线程发生未知异常"});
        ApplicationErrorLogger::Write(
            {"error", "thread_exception", "DedupCore", "mysql_sync",
             "MySQL 同步线程发生未知异常", "unknown_exception", {}, 0});
    }
    if (!execution_logger_.WriteEvent(
            {0, "mysql_sync", "stopped", "background_sync", "MySQL 后台同步已停止", 0, 0},
            executionLogError)) {
        RecordFailure({false, 0, "MySQL 同步结束日志写入失败：" + executionLogError});
    }
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    snapshot_.running = false;
}

}  // namespace videosc::dedup
