#include "DeletionService.h"

#include <Windows.h>
#include <bcrypt.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace videosc::dedup {
namespace {

using Json = nlohmann::json;
constexpr std::string_view kDeletePrefix = "delete/";

std::string OneLine(std::string value) {
    for (char& character : value) {
        if (character == '\r' || character == '\n' || character == '\t') character = ' ';
    }
    return value;
}

std::string UtcText(const std::int64_t milliseconds) {
    const std::time_t seconds = static_cast<std::time_t>(milliseconds / 1000);
    std::tm value{};
    if (gmtime_s(&value, &seconds) != 0) return std::to_string(milliseconds);
    std::ostringstream output;
    output << std::put_time(&value, "%Y-%m-%d %H:%M:%S") << '.'
           << std::setw(3) << std::setfill('0') << (milliseconds % 1000) << " UTC";
    return output.str();
}

/** @brief 生成非零批次 ID。 */
std::uint64_t RandomBatchId() {
    std::uint64_t value = 0;
    if (BCryptGenRandom(nullptr,
                        reinterpret_cast<PUCHAR>(&value),
                        sizeof(value),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0 ||
        value == 0) {
        value = static_cast<std::uint64_t>(GetTickCount64()) ^
                (static_cast<std::uint64_t>(GetCurrentProcessId()) << 32);
    }
    return value == 0 ? 1 : value;
}

/** @brief 严格 UTF-16 到 UTF-8。 */
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
    if (length <= 0) throw std::runtime_error("Cannot encode deletion path as UTF-8");
    std::string result(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8,
                            WC_ERR_INVALID_CHARS,
                            value.data(),
                            static_cast<int>(value.size()),
                            result.data(),
                            length,
                            nullptr,
                            nullptr) != length) {
        throw std::runtime_error("Cannot encode deletion path as UTF-8");
    }
    return result;
}

/** @brief 严格 UTF-8 到 UTF-16。 */
std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8,
                                           MB_ERR_INVALID_CHARS,
                                           value.data(),
                                           static_cast<int>(value.size()),
                                           nullptr,
                                           0);
    if (length <= 0) throw std::runtime_error("Cannot decode deletion path from UTF-8");
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8,
                            MB_ERR_INVALID_CHARS,
                            value.data(),
                            static_cast<int>(value.size()),
                            result.data(),
                            length) != length) {
        throw std::runtime_error("Cannot decode deletion path from UTF-8");
    }
    return result;
}

/** @brief 路径映射 RocksDB 键与扫描协调器保持一致。 */
std::string PathRecordKey(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::path absolute = std::filesystem::absolute(path, error).lexically_normal();
    std::wstring normalized = (error ? path : absolute).wstring();
    std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](const wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return "path/" + WideToUtf8(normalized);
}

/** @brief 删除意图键包含批次和 path_id，便于崩溃后有界恢复。 */
std::string IntentKey(const std::uint64_t batch_id, const std::uint64_t path_id) {
    return std::string(kDeletePrefix) + std::to_string(batch_id) + "/" + std::to_string(path_id);
}

/** @brief 删除意图 JSON 不包含数据库凭据。 */
std::string EncodeIntent(const std::uint64_t batch_id,
                         const std::uint64_t group_id,
                         const DuplicateMember& member,
                         const std::string& state) {
    return Json({{"version", 1},
                 {"batch_id", batch_id},
                 {"group_id", group_id},
                 {"path_id", member.path_id},
                 {"path", WideToUtf8(member.path.wstring())},
                 {"sha512", Sha512ToHex(member.content_sha512)},
                 {"state", state}})
        .dump();
}

/** @brief 从删除意图恢复最小成员信息。 */
bool DecodeIntent(const std::string& value,
                  std::uint64_t& batch_id,
                  std::uint64_t& group_id,
                  DuplicateMember& member,
                  std::string& state) {
    try {
        const Json json = Json::parse(value);
        if (json.at("version").get<int>() != 1) return false;
        batch_id = json.at("batch_id").get<std::uint64_t>();
        group_id = json.at("group_id").get<std::uint64_t>();
        member.path_id = json.at("path_id").get<std::uint64_t>();
        member.path = Utf8ToWide(json.at("path").get<std::string>());
        const auto sha = Sha512FromHex(json.at("sha512").get<std::string>());
        if (!sha.has_value()) return false;
        member.content_sha512 = *sha;
        state = json.at("state").get<std::string>();
        return state == "prepared" || state == "file_deleted";
    } catch (...) {
        return false;
    }
}

/** @brief 质量比较仅返回业务质量，最终稳定决胜统一交给 path_id。 */
auto QualityTuple(const DuplicateMember& member) {
    return std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>(
        static_cast<std::uint64_t>(member.width) * member.height,
        member.bitrate,
        member.size_bytes);
}

/** @brief 单个策略比较；正数表示 left 更适合保留，零表示继续使用下一优先级。 */
int CompareByPolicy(const DuplicateMember& left,
                    const DuplicateMember& right,
                    const KeepPolicy policy) {
    switch (policy) {
        case KeepPolicy::Newest:
            return left.last_write_time_utc_ms == right.last_write_time_utc_ms
                       ? 0
                       : (left.last_write_time_utc_ms > right.last_write_time_utc_ms ? 1 : -1);
        case KeepPolicy::Oldest:
            return left.last_write_time_utc_ms == right.last_write_time_utc_ms
                       ? 0
                       : (left.last_write_time_utc_ms < right.last_write_time_utc_ms ? 1 : -1);
        case KeepPolicy::Smallest:
            return left.size_bytes == right.size_bytes ? 0 : (left.size_bytes < right.size_bytes ? 1 : -1);
        case KeepPolicy::Largest:
            return left.size_bytes == right.size_bytes ? 0 : (left.size_bytes > right.size_bytes ? 1 : -1);
        case KeepPolicy::HighestQuality:
            return QualityTuple(left) == QualityTuple(right)
                       ? 0
                       : (QualityTuple(left) > QualityTuple(right) ? 1 : -1);
        case KeepPolicy::PathPriority:
            return left.scan_root_priority == right.scan_root_priority
                       ? 0
                       : (left.scan_root_priority < right.scan_root_priority ? 1 : -1);
    }
    return 0;
}

/** @brief 固定优先级比较，全部业务规则平局时使用较小 path_id。 */
bool Prefer(const DuplicateMember& left,
            const DuplicateMember& right,
            const RetentionPolicySet& policies) {
    const std::pair<KeepPolicy, bool> ordered[] = {
        {KeepPolicy::PathPriority, policies.path_priority},
        {KeepPolicy::HighestQuality, policies.highest_quality},
        {KeepPolicy::Newest, policies.newest},
        {KeepPolicy::Oldest, policies.oldest},
        {KeepPolicy::Largest, policies.largest},
        {KeepPolicy::Smallest, policies.smallest},
    };
    for (const auto& item : ordered) {
        if (!item.second) continue;
        const int compared = CompareByPolicy(left, right, item.first);
        if (compared != 0) return compared > 0;
    }
    return left.path_id < right.path_id;
}

/** @brief 规范化用户输入的物理磁盘键，避免首尾空白和大小写造成假性不匹配。 */
std::wstring NormalizeStorageTarget(std::wstring value) {
    const auto isSpace = [](const wchar_t character) { return std::iswspace(character) != 0; };
    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), isSpace));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), isSpace).base(), value.end());
    std::transform(value.begin(), value.end(), value.begin(), [](const wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return value;
}

/** @brief 对一个成员重新计算完整 SHA-512。 */
bool VerifyMember(IFileHasher& hasher,
                  const DuplicateMember& member,
                  std::uint32_t& system_error,
                  std::string& message) {
    std::atomic_bool cancel{false};
    const FileHashResult result = hasher.Hash(member.path, cancel);
    system_error = result.system_error;
    if (result.status != FileHashStatus::Succeeded || !result.sha512.has_value()) {
        message = "sha512_revalidation_failed";
        return false;
    }
    if (*result.sha512 != member.content_sha512) {
        message = "sha512_changed";
        return false;
    }
    return true;
}

}  // namespace

bool RetentionPolicySet::any() const noexcept {
    return path_priority || highest_quality || newest || oldest || largest || smallest;
}

RetentionPolicySet RetentionPolicySet::FromSingle(const KeepPolicy policy) noexcept {
    RetentionPolicySet result;
    result.highest_quality = false;
    switch (policy) {
        case KeepPolicy::Newest: result.newest = true; break;
        case KeepPolicy::Oldest: result.oldest = true; break;
        case KeepPolicy::Smallest: result.smallest = true; break;
        case KeepPolicy::Largest: result.largest = true; break;
        case KeepPolicy::HighestQuality: result.highest_quality = true; break;
        case KeepPolicy::PathPriority: result.path_priority = true; break;
    }
    return result;
}

DeletionPlanResult DeletionPlanner::SelectDetailed(
    const DuplicateGroup& group,
    const RetentionPolicySet& policies,
    const std::optional<std::wstring>& target_storage_target) {
    DeletionPlanResult result;
    if (!policies.any()) {
        result.status = DeletionPlanStatus::InvalidPolicy;
        result.message = "至少需要启用一个保留策略";
        return result;
    }
    if (group.members.size() < 2) {
        result.status = DeletionPlanStatus::InsufficientMembers;
        result.message = "重复组成员少于两个";
        return result;
    }

    const std::wstring normalizedTarget =
        target_storage_target.has_value() ? NormalizeStorageTarget(*target_storage_target) : std::wstring{};
    const bool hasTarget = !normalizedTarget.empty();
    const auto matchesTarget = [&](const DuplicateMember& member) {
        return hasTarget && NormalizeStorageTarget(member.storage_target_key) == normalizedTarget;
    };
    if (hasTarget) {
        result.matched_target_members = static_cast<std::uint64_t>(std::count_if(
            group.members.begin(), group.members.end(), matchesTarget));
        if (result.matched_target_members == 0) {
            result.status = DeletionPlanStatus::TargetStorageNotFound;
            result.message = "指定磁盘在当前重复组中没有匹配成员";
            return result;
        }
    }

    std::vector<const DuplicateMember*> retainCandidates;
    if (hasTarget) {
        for (const DuplicateMember& member : group.members) {
            if (!matchesTarget(member)) retainCandidates.push_back(&member);
        }
    }
    if (retainCandidates.empty()) {
        for (const DuplicateMember& member : group.members) retainCandidates.push_back(&member);
    }
    const DuplicateMember* retained = retainCandidates.front();
    for (const DuplicateMember* candidate : retainCandidates) {
        if (Prefer(*candidate, *retained, policies)) retained = candidate;
    }

    DuplicateGroup plan = group;
    plan.retained_path_id = retained->path_id;
    plan.selected_for_deletion.clear();
    plan.reclaimable_bytes = 0;
    for (const DuplicateMember& member : plan.members) {
        if (member.path_id == retained->path_id) continue;
        if (hasTarget && !matchesTarget(member)) continue;
        plan.selected_for_deletion.push_back(member.path_id);
        plan.reclaimable_bytes += member.size_bytes;
    }
    if (plan.selected_for_deletion.empty()) {
        result.status = DeletionPlanStatus::NoDeletionCandidates;
        result.message = "当前条件没有可删除候选";
        return result;
    }
    if (plan.selected_for_deletion.size() >= plan.members.size()) {
        result.status = DeletionPlanStatus::UnsafeWholeGroupSelection;
        result.message = "选择会覆盖整个重复组";
        return result;
    }
    result.status = DeletionPlanStatus::Succeeded;
    result.plan = std::move(plan);
    result.message = "ok";
    return result;
}

std::optional<DuplicateGroup> DeletionPlanner::Select(
    const DuplicateGroup& group,
    const KeepPolicy keep_policy,
    const std::optional<std::wstring>& target_storage_target) {
    DeletionPlanResult result = SelectDetailed(
        group, RetentionPolicySet::FromSingle(keep_policy), target_storage_target);
    return result.succeeded() ? std::move(result.plan) : std::nullopt;
}

OperationLogger::OperationLogger(LoggingConfig config)
    : config_(config), execution_logger_(std::move(config)) {}

bool OperationLogger::EnsureWritable(std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::error_code filesystemError;
    std::filesystem::create_directories(config_.directory, filesystemError);
    if (filesystemError) {
        error = filesystemError.message();
        return false;
    }
    std::ofstream stream(config_.directory / L"operations.log", std::ios::binary | std::ios::app);
    if (!stream) {
        error = "cannot_open_operation_log";
        return false;
    }
    stream.flush();
    if (!stream) {
        error = "cannot_flush_operation_log";
        return false;
    }
    return execution_logger_.EnsureWritable(error);
}

bool OperationLogger::RotateIfNeeded() {
    const std::filesystem::path active = config_.directory / L"operations.log";
    std::error_code error;
    const std::uint64_t size = std::filesystem::exists(active, error) ? std::filesystem::file_size(active, error) : 0;
    if (error || size < static_cast<std::uint64_t>(config_.rotate_file_mib) * 1024ULL * 1024ULL) return !error;
    for (std::uint32_t index = config_.rotate_count; index > 0; --index) {
        const std::filesystem::path destination =
            config_.directory / (L"operations.log." + std::to_wstring(index));
        const std::filesystem::path source =
            index == 1 ? active : config_.directory / (L"operations.log." + std::to_wstring(index - 1));
        if (!std::filesystem::exists(source, error)) continue;
        std::filesystem::remove(destination, error);
        error.clear();
        std::filesystem::rename(source, destination, error);
        if (error) return false;
    }
    return true;
}

bool OperationLogger::Write(const std::uint64_t batch_id,
                            const std::uint64_t group_id,
                            const DuplicateMember& member,
                            const std::string& action,
                            const bool succeeded,
                            const std::uint32_t system_error,
                            const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!RotateIfNeeded()) return false;
    const std::int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
    std::ostringstream text;
    text << '[' << UtcText(now) << "] [永久删除]"
         << " 批次ID=" << batch_id
         << " 组ID=" << group_id
         << " 路径ID=" << member.path_id
         << " 操作=" << OneLine(action)
         << " 结果=" << (succeeded ? "成功" : "失败")
         << " 系统错误=" << system_error
         << " SHA512=" << Sha512ToHex(member.content_sha512)
         << " 路径=" << OneLine(WideToUtf8(member.path.wstring()))
         << " 原因=" << OneLine(message) << '\n';
    const std::string line = text.str();
    std::ofstream stream(config_.directory / L"operations.log", std::ios::binary | std::ios::app);
    if (!stream) return false;
    stream.write(line.data(), static_cast<std::streamsize>(line.size()));
    stream.flush();
    if (!stream) return false;
    if (!succeeded) {
        ExecutionFailureRecord failure;
        failure.task_id = batch_id;
        failure.path_id = member.path_id;
        failure.task = "permanent_delete";
        failure.stage = "delete";
        failure.operation = action;
        failure.path = member.path;
        failure.storage_target_key = member.storage_target_key;
        failure.status = "failed";
        failure.native_error = system_error;
        failure.detail = message;
        std::string error;
        if (!execution_logger_.WriteFailure(failure, error)) return false;
    }
    return true;
}

DeletionExecutor::DeletionExecutor(RocksStore& store,
                                   MySqlSyncQueue& sync_queue,
                                   std::shared_ptr<IFileHasher> hasher,
                                   OperationLogger& logger)
    : store_(store), sync_queue_(sync_queue), hasher_(std::move(hasher)), logger_(logger) {
    if (!hasher_) throw std::invalid_argument("Deletion hasher is required");
}

DeletionBatchResult DeletionExecutor::Execute(const DuplicateGroup& plan,
                                               const ProgressCallback& progress,
                                               const StopCallback& should_stop) {
    DeletionBatchResult batch;
    batch.batch_id = RandomBatchId();
    if (!plan.retained_path_id.has_value() || plan.selected_for_deletion.empty() ||
        plan.selected_for_deletion.size() >= plan.members.size()) {
        return batch;
    }
    batch.retained_path_id = *plan.retained_path_id;
    const auto retained = std::find_if(plan.members.begin(), plan.members.end(), [&](const DuplicateMember& member) {
        return member.path_id == *plan.retained_path_id;
    });
    if (retained == plan.members.end()) return batch;
    std::string logError;
    if (!logger_.EnsureWritable(logError)) return batch;

    std::uint32_t retainedError = 0;
    std::string retainedMessage;
    if (!VerifyMember(*hasher_, *retained, retainedError, retainedMessage)) {
        if (!logger_.Write(batch.batch_id,
                           plan.group_id,
                           *retained,
                           "verify_retained",
                           false,
                           retainedError,
                           retainedMessage)) {
            throw std::runtime_error("delete_failure_log_write_failed");
        }
        return batch;
    }

    // 先验证计划内引用，删除意图改为逐文件写入，关闭时不会留下尚未开始文件的 prepared 记录。
    for (const std::uint64_t pathId : plan.selected_for_deletion) {
        const auto member = std::find_if(plan.members.begin(), plan.members.end(), [&](const DuplicateMember& value) {
            return value.path_id == pathId;
        });
        if (member == plan.members.end() || member->path_id == retained->path_id) return batch;
    }
    batch.preflight_succeeded = true;

    for (const std::uint64_t pathId : plan.selected_for_deletion) {
        if (should_stop && should_stop()) break;
        const auto member = std::find_if(plan.members.begin(), plan.members.end(), [&](const DuplicateMember& value) {
            return value.path_id == pathId;
        });
        DeletionItemResult item;
        item.path_id = pathId;
        item.path = member->path;
        const std::string intentKey = IntentKey(batch.batch_id, pathId);
        const RocksStatus prepared = store_.Put(
            RocksColumnFamily::Tombstones,
            intentKey,
            EncodeIntent(batch.batch_id, plan.group_id, *member, "prepared"),
            true);
        if (!prepared.succeeded) {
            item.message = "delete_intent_write_failed";
            batch.items.push_back(std::move(item));
            if (progress) progress(batch.items.back(), batch);
            continue;
        }
        if (!VerifyMember(*hasher_, *member, item.system_error, item.message)) {
            if (!logger_.Write(batch.batch_id,
                               plan.group_id,
                               *member,
                               "delete_file",
                               false,
                               item.system_error,
                               item.message)) {
                throw std::runtime_error("delete_failure_log_write_failed");
            }
            store_.Delete(RocksColumnFamily::Tombstones, intentKey, true);
            batch.items.push_back(std::move(item));
            if (progress) progress(batch.items.back(), batch);
            continue;
        }
        if (!DeleteFileW(member->path.c_str())) {
            item.system_error = GetLastError();
            item.message = "delete_file_failed";
            if (!logger_.Write(batch.batch_id,
                               plan.group_id,
                               *member,
                               "delete_file",
                               false,
                               item.system_error,
                               item.message)) {
                throw std::runtime_error("delete_failure_log_write_failed");
            }
            store_.Delete(RocksColumnFamily::Tombstones, intentKey, true);
            batch.items.push_back(std::move(item));
            if (progress) progress(batch.items.back(), batch);
            continue;
        }
        item.deleted = true;
        item.message = "deleted";
        ++batch.deleted_files;
        batch.reclaimed_bytes += member->size_bytes;
        if (!logger_.Write(batch.batch_id, plan.group_id, *member, "delete_file", true, 0, "deleted")) {
            throw std::runtime_error("delete_operation_log_write_failed");
        }
        store_.Put(RocksColumnFamily::Tombstones,
                   intentKey,
                   EncodeIntent(batch.batch_id, plan.group_id, *member, "file_deleted"),
                   true);
        SyncOperation operation;
        operation.kind = SyncOperationKind::DeleteFilePath;
        operation.delete_path_id = member->path_id;
        operation.expected_sha512 = member->content_sha512;
        const RocksStatus queued = sync_queue_.Enqueue(
            {{RocksColumnFamily::FilePaths, PathRecordKey(member->path), std::nullopt},
             {RocksColumnFamily::Tombstones, intentKey, std::nullopt}},
            operation);
        item.mapping_delete_queued = queued.succeeded;
        if (!logger_.Write(batch.batch_id,
                           plan.group_id,
                           *member,
                           "queue_mapping_delete",
                           queued.succeeded,
                           0,
                           queued.message)) {
            throw std::runtime_error("delete_mapping_log_write_failed");
        }
        batch.items.push_back(std::move(item));
        if (progress) progress(batch.items.back(), batch);
    }
    return batch;
}

RocksStatus DeletionExecutor::RecoverPendingDeletes() {
    std::vector<std::pair<std::string, std::string>> intents;
    RocksStatus status = store_.ForEachPrefix(
        RocksColumnFamily::Tombstones,
        kDeletePrefix,
        0,
        [&](const std::string_view key, const std::string_view value) {
            intents.emplace_back(key, value);
            return true;
        });
    if (!status.succeeded) return status;
    for (const auto& item : intents) {
        std::uint64_t batchId = 0;
        std::uint64_t groupId = 0;
        DuplicateMember member;
        std::string state;
        if (!DecodeIntent(item.second, batchId, groupId, member, state)) return {false, "invalid_delete_intent"};
        if (state == "prepared" && GetFileAttributesW(member.path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            status = store_.Delete(RocksColumnFamily::Tombstones, item.first, true);
            if (!status.succeeded) return status;
            continue;
        }
        SyncOperation operation;
        operation.kind = SyncOperationKind::DeleteFilePath;
        operation.delete_path_id = member.path_id;
        operation.expected_sha512 = member.content_sha512;
        status = sync_queue_.Enqueue(
            {{RocksColumnFamily::FilePaths, PathRecordKey(member.path), std::nullopt},
             {RocksColumnFamily::Tombstones, item.first, std::nullopt}},
            operation);
        if (!status.succeeded) return status;
        logger_.Write(batchId, groupId, member, "recover_mapping_delete", true, 0, "queued");
    }
    return {true, {}};
}

}  // namespace videosc::dedup
