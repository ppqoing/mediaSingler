#include "ScanCoordinator.h"

#include "ScanOptionsCodec.h"
#include "../concurrency/TaskThreadPool.h"
#include "../dedup/DHashSimilarity.h"
#include "../discovery/NativeFileDiscovery.h"
#include "../models/CoreModelCodec.h"
#include "../logging/ApplicationErrorLogger.h"
#include "../scheduling/FileHasher.h"
#include "VideoSc.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <map>
#include <new>
#include <random>
#include <set>
#include <stdexcept>
#include <utility>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace videosc::dedup {
namespace {

/** @brief 生成非零随机 64 位本地标识；记录落盘后保持稳定。 */
std::uint64_t RandomId() {
    std::uint64_t value = 0;
    do {
        if (BCryptGenRandom(nullptr,
                            reinterpret_cast<PUCHAR>(&value),
                            sizeof(value),
                            BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
            value = static_cast<std::uint64_t>(GetTickCount64()) ^
                    (static_cast<std::uint64_t>(GetCurrentProcessId()) << 32);
        }
    } while (value == 0);
    return value;
}

/** @brief 严格 UTF-16 到 UTF-8，用于 RocksDB 键和 VideoSc API。 */
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
    if (length <= 0) throw std::runtime_error("Cannot encode scan path as UTF-8");
    std::string result(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8,
                            WC_ERR_INVALID_CHARS,
                            value.data(),
                            static_cast<int>(value.size()),
                            result.data(),
                            length,
                            nullptr,
                            nullptr) != length) {
        throw std::runtime_error("Cannot encode scan path as UTF-8");
    }
    return result;
}

/** @brief UTF-8 到 UTF-16，用于把发现器错误消息转为 GUI 显示。 */
std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8,
                                           MB_ERR_INVALID_CHARS,
                                           value.data(),
                                           static_cast<int>(value.size()),
                                           nullptr,
                                           0);
    if (length <= 0) return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8,
                            MB_ERR_INVALID_CHARS,
                            value.data(),
                            static_cast<int>(value.size()),
                            result.data(),
                            length) != length) {
        return {};
    }
    return result;
}

/** @brief 路径记录在 RocksDB 中使用完整规范路径键，避免摘要碰撞误复用。 */
std::string PathRecordKey(const std::wstring& normalized_path) {
    return "path/" + WideToUtf8(normalized_path);
}

/** @brief 当前扫描任务的媒体待处理键。 */
std::string MediaTaskPrefix(const std::uint64_t scan_id) {
    return "media/" + std::to_string(scan_id) + "/";
}

/** @brief 同一 SHA-512 在一个任务内只保留一个媒体处理项。 */
std::string MediaTaskKey(const std::uint64_t scan_id, const Sha512Digest& sha512) {
    return MediaTaskPrefix(scan_id) + Sha512ToHex(sha512);
}

/** @brief 范围判断使用绝对路径、统一分隔符和 invariant 小写。 */
std::wstring NormalizeScopePath(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::path absolute = std::filesystem::absolute(path, error).lexically_normal();
    std::wstring value = (error ? path : absolute).wstring();
    std::replace(value.begin(), value.end(), L'/', L'\\');
    std::transform(value.begin(), value.end(), value.begin(), [](const wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    while (value.size() > 3 && value.back() == L'\\') value.pop_back();
    return value;
}

/** @brief 检查路径位于扫描根本身或其目录边界下，避免 D:\a 误匹配 D:\abc。 */
bool IsUnderRoot(const std::wstring& normalized_path, const std::wstring& normalized_root) {
    if (normalized_path == normalized_root) return true;
    return normalized_path.size() > normalized_root.size() &&
           normalized_path.compare(0, normalized_root.size(), normalized_root) == 0 &&
           (normalized_root.back() == L'\\' || normalized_path[normalized_root.size()] == L'\\');
}

/** @brief 读取 RocksDB 路径记录；不存在与损坏保持可区分。 */
std::optional<FilePathRecord> LoadPathRecord(RocksStore& store,
                                             const std::string& key,
                                             bool& corrupted) {
    std::string value;
    const RocksStatus status = store.Get(RocksColumnFamily::FilePaths, key, value);
    if (!status.succeeded) {
        corrupted = status.message != "not_found";
        return std::nullopt;
    }
    std::string error;
    std::optional<FilePathRecord> record = CoreModelCodec::DeserializeFilePath(value, error);
    corrupted = !record.has_value();
    return record;
}

/** @brief 读取共享内容记录。 */
std::optional<ShaFileData> LoadShaData(RocksStore& store,
                                      const Sha512Digest& sha512,
                                      bool& corrupted) {
    std::string value;
    const RocksStatus status = store.Get(RocksColumnFamily::ShaFileData, Sha512ToHex(sha512), value);
    if (!status.succeeded) {
        corrupted = status.message != "not_found";
        return std::nullopt;
    }
    std::string error;
    std::optional<ShaFileData> data = CoreModelCodec::DeserializeShaFileData(value, error);
    corrupted = !data.has_value();
    return data;
}

/** @brief 查询现有文件身份，增量扫描用它识别同大小同时间但已被替换的路径。 */
std::optional<FileIdentity> QueryIdentity(const std::filesystem::path& path) {
    HANDLE file = CreateFileW(path.c_str(),
                              FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::nullopt;
    BY_HANDLE_FILE_INFORMATION information{};
    const BOOL succeeded = GetFileInformationByHandle(file, &information);
    CloseHandle(file);
    if (!succeeded) return std::nullopt;
    FileIdentity identity;
    identity.volume_serial = information.dwVolumeSerialNumber;
    identity.file_id_low = (static_cast<std::uint64_t>(information.nFileIndexHigh) << 32) |
                           static_cast<std::uint64_t>(information.nFileIndexLow);
    return identity;
}

/** @brief 判断旧路径记录能否安全复用 SHA-512。 */
bool CanReuse(const FilePathRecord& old_record, const FilePathRecord& discovered) {
    if (!old_record.sha512.has_value() || old_record.size_bytes != discovered.size_bytes ||
        old_record.last_write_time_utc_ms != discovered.last_write_time_utc_ms) {
        return false;
    }
    if (old_record.identity.volume_serial == 0 && old_record.identity.file_id_low == 0) return false;
    const std::optional<FileIdentity> identity = QueryIdentity(discovered.path);
    return identity.has_value() && identity->volume_serial == old_record.identity.volume_serial &&
           identity->file_id_high == old_record.identity.file_id_high &&
           identity->file_id_low == old_record.identity.file_id_low;
}

/** @brief 将文件哈希失败映射为可持久化路径状态。 */
FilePathState FailedPathState(const FileHashStatus status) {
    return status == FileHashStatus::ReadTimeout ? FilePathState::ReadTimeout : FilePathState::Unreadable;
}

/** @brief 按自动识别介质选择单盘读取线程，未知介质按 HDD 保守处理。 */
std::uint32_t ReadThreadsFor(const ScanOptions& options, const StorageMediaType media_type) {
    return media_type == StorageMediaType::Ssd
               ? options.storage().ssd_read_threads_per_disk
               : options.storage().hdd_read_threads_per_disk;
}

/** @brief 当前内容是否已经具备文件类型要求的全部版本化媒体特征。 */
bool MediaComplete(const ShaFileData& data,
                   const MediaKind kind,
                   const std::string& algorithm_version) {
    if (data.media_algorithm_version != algorithm_version) return false;
    if (kind == MediaKind::Image) {
        return data.image_pdq_hash.has_value() && data.image_pdq_quality.has_value() &&
               data.has_image_zoned_phashes &&
               data.image_perceptual_algorithm_version == VIDEOSC_IMAGE_PERCEPTUAL_ALGORITHM_VERSION &&
               data.image_structural_algorithm_version == VIDEOSC_IMAGE_STRUCTURAL_ALGORITHM_VERSION;
    }
    if (kind == MediaKind::Video) {
        return data.has_video_dhashes &&
               std::all_of(data.video_dhashes.begin(), data.video_dhashes.end(),
                           [](const std::uint64_t hash) { return hash != 0; });
    }
    return true;
}

/** @return 稳定的 SHA-512 失败状态名。 */
const char* HashStatusName(const FileHashStatus status) {
    switch (status) {
        case FileHashStatus::Succeeded: return "succeeded";
        case FileHashStatus::InvalidArgument: return "invalid_argument";
        case FileHashStatus::OpenFailed: return "open_failed";
        case FileHashStatus::ReadFailed: return "read_failed";
        case FileHashStatus::ReadTimeout: return "read_timeout";
        case FileHashStatus::Cancelled: return "cancelled";
        case FileHashStatus::FileChanged: return "file_changed";
        case FileHashStatus::CryptoFailed: return "crypto_failed";
        case FileHashStatus::UnexpectedFailure: return "unexpected_failure";
    }
    return "unknown";
}

/** @return 执行日志使用的媒体类型名。 */
const char* MediaKindName(const MediaKind kind) {
    switch (kind) {
        case MediaKind::Image: return "image";
        case MediaKind::Video: return "video";
        case MediaKind::Audio: return "audio";
        case MediaKind::Other: return "other";
    }
    return "other";
}

/** @brief VideoSc 媒体回调读取协调器原子取消标志。 */
int __cdecl ShouldCancelMedia(void* context) {
    return static_cast<const std::atomic_bool*>(context)->load(std::memory_order_relaxed) ? 1 : 0;
}

/** @brief 媒体阶段可增长、不回收的计算并发闸门。 */
class PhaseComputeGate final {
public:
    PhaseComputeGate(const std::uint32_t maximum, const bool adaptive)
        : capacity_(adaptive ? 1U : maximum), maximum_(maximum) {}

    /** @brief 等待一个媒体计算令牌。 */
    bool Acquire(const std::atomic_bool& cancelled) {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [&] { return active_ < capacity_ || cancelled.load(); });
        if (cancelled.load()) return false;
        ++active_;
        return true;
    }

    /** @brief 归还令牌。 */
    void Release() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_ > 0) --active_;
        condition_.notify_one();
    }

    /** @brief 增加一个令牌并唤醒等待线程。 */
    bool IncreaseOne() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (capacity_ >= maximum_) return false;
        ++capacity_;
        condition_.notify_all();
        return true;
    }

    /** @brief 取消时唤醒全部等待线程。 */
    void WakeAll() { condition_.notify_all(); }

    std::uint32_t capacity() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return capacity_;
    }

    std::uint32_t active() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return active_;
    }

private:
    std::uint32_t capacity_ = 1;
    std::uint32_t maximum_ = 1;
    std::uint32_t active_ = 0;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
};

/** @brief 媒体阶段系统总 CPU 采样器。 */
class SystemCpuSampler final {
public:
    std::optional<double> Sample() {
        FILETIME idle{};
        FILETIME kernel{};
        FILETIME user{};
        if (!GetSystemTimes(&idle, &kernel, &user)) return std::nullopt;
        const auto value = [](const FILETIME& time) {
            return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) |
                   static_cast<std::uint64_t>(time.dwLowDateTime);
        };
        const std::uint64_t currentIdle = value(idle);
        const std::uint64_t currentKernel = value(kernel);
        const std::uint64_t currentUser = value(user);
        if (!initialized_) {
            idle_ = currentIdle;
            kernel_ = currentKernel;
            user_ = currentUser;
            initialized_ = true;
            return std::nullopt;
        }
        const std::uint64_t idleDelta = currentIdle - idle_;
        const std::uint64_t total = (currentKernel - kernel_) + (currentUser - user_);
        idle_ = currentIdle;
        kernel_ = currentKernel;
        user_ = currentUser;
        if (total == 0 || idleDelta > total) return std::nullopt;
        return static_cast<double>(total - idleDelta) * 100.0 / static_cast<double>(total);
    }

private:
    bool initialized_ = false;
    std::uint64_t idle_ = 0;
    std::uint64_t kernel_ = 0;
    std::uint64_t user_ = 0;
};

bool IsTerminalDiscoveryPhase(const DiscoveryRootPhase phase) {
    return phase == DiscoveryRootPhase::Completed ||
           phase == DiscoveryRootPhase::Cancelled ||
           phase == DiscoveryRootPhase::Failed;
}

}  // namespace

bool ShouldRetryImageFeatureAnalysis(const int status_code,
                                     const std::uint32_t attempted_count,
                                     const bool cancellation_requested) noexcept {
    return !cancellation_requested && attempted_count == 1 &&
           status_code == VIDEOSC_ERR_MEDIA_TIMEOUT;
}

ScanCoordinator::ScanCoordinator(ScanOptions options,
                                 RocksStore& store,
                                 MySqlSyncQueue& sync_queue,
                                 MySqlClient& mysql_client,
                                 std::function<void()> sync_wake)
    : options_(std::move(options)),
      store_(store),
      sync_queue_(sync_queue),
      mysql_reader_(mysql_client),
      sync_wake_(std::move(sync_wake)),
      checkpoint_store_(store),
      execution_logger_(options_.logging()) {
    progress_.configured_compute_threads = options_.compute().worker_threads;
}

ScanCoordinator::~ScanCoordinator() {
    Cancel();
    Wait();
}

bool ScanCoordinator::Start(const std::optional<std::uint64_t> resume_scan_id) {
    if (running_.exchange(true)) return false;
    if (worker_.joinable()) worker_.join();
    cancel_requested_.store(false);
    fatal_error_.store(false);
    {
        std::lock_guard<std::mutex> lock(discovery_warning_mutex_);
        discovery_warning_.clear();
    }
    const std::uint64_t scanId = resume_scan_id.value_or(RandomId());
    active_scan_id_ = scanId;
    {
        std::lock_guard<std::mutex> lock(progress_mutex_);
        progress_ = {};
        progress_.scan_id = scanId;
        progress_.phase = ScanPhase::Discovering;
        progress_.configured_compute_threads = options_.compute().worker_threads;
        progress_.allowed_compute_threads = options_.compute().adaptive_worker_threads
                                                ? 1U
                                                : options_.compute().worker_threads;
        progress_.rocksdb_writable = store_.is_open();
        discovery_started_at_.clear();
    }
    try {
        hash_scheduler_ = std::make_unique<DiskHashScheduler>(
            std::make_shared<VideoScFileHasher>(options_.io()),
            options_.compute().worker_threads,
            options_.storage().max_concurrent_file_reads,
            options_.compute().adaptive_worker_threads,
            options_.compute().cpu_target_percent,
            options_.storage().adaptive_read_threads,
            options_.storage().disk_read_target_percent);
        worker_ = std::thread(&ScanCoordinator::WorkerMain, this, scanId);
        return true;
    } catch (const std::exception& exception) {
        ApplicationErrorLogger::Write(
            {"error", "thread_start", "DedupCore", "scan_coordinator_start",
             exception.what(), "std::exception", {}, 0});
    } catch (...) {
        ApplicationErrorLogger::Write(
            {"error", "thread_start", "DedupCore", "scan_coordinator_start",
             "扫描协调线程创建失败", "unknown_exception", {}, 0});
    }
    hash_scheduler_.reset();
    running_.store(false);
    SetFailure(L"扫描线程创建失败。");
    return false;
}

void ScanCoordinator::Cancel() {
    if (!running_.load()) return;
    cancel_requested_.store(true);
    {
        std::lock_guard<std::mutex> lock(progress_mutex_);
        progress_.cancellation_requested = true;
        for (auto& root : progress_.discovery_roots) {
            if (!IsTerminalDiscoveryPhase(root.phase)) root.phase = DiscoveryRootPhase::Cancelling;
        }
    }
    {
        std::lock_guard<std::mutex> lock(scheduler_control_mutex_);
        if (hash_scheduler_) hash_scheduler_->RequestCancel();
    }
}

void ScanCoordinator::Wait() {
    if (worker_.joinable()) worker_.join();
}

bool ScanCoordinator::is_running() const noexcept {
    return running_.load();
}

ProgressSnapshot ScanCoordinator::snapshot() const {
    ProgressSnapshot result;
    {
        std::lock_guard<std::mutex> lock(progress_mutex_);
        result = progress_;
        const auto now = std::chrono::steady_clock::now();
        const std::size_t count = std::min(result.discovery_roots.size(), discovery_started_at_.size());
        for (std::size_t index = 0; index < count; ++index) {
            if (!IsTerminalDiscoveryPhase(result.discovery_roots[index].phase)) {
                result.discovery_roots[index].elapsed_milliseconds =
                    static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - discovery_started_at_[index]).count());
            }
        }
    }
    std::lock_guard<std::mutex> schedulerLock(scheduler_control_mutex_);
    if (hash_scheduler_) {
        const auto channels = hash_scheduler_->GetSnapshots();
        if (result.phase == ScanPhase::Hashing) {
            const SchedulerComputeSnapshot compute = hash_scheduler_->GetComputeSnapshot();
            result.active_file_reads = compute.active_file_reads;
            result.active_compute_threads = compute.active_compute_threads;
            result.allowed_compute_threads = compute.allowed_compute_threads;
            result.system_cpu_percent = compute.system_cpu_percent;
        }
        result.disks.clear();
        result.disks.reserve(channels.size());
        result.queued_compute_tasks = 0;
        for (const DiskChannelSnapshot& channel : channels) {
            DiskProgress disk;
            disk.storage_target_key = channel.storage_target_key;
            disk.media_type = channel.media_type == StorageMediaType::Hdd
                                  ? L"HDD"
                                  : (channel.media_type == StorageMediaType::Ssd ? L"SSD" : L"Unknown");
            disk.configured_read_threads = channel.configured_threads;
            disk.allowed_read_threads = channel.allowed_threads;
            disk.active_read_threads = channel.active_threads;
            disk.queued_files = channel.queued_files;
            result.queued_compute_tasks += channel.queued_files;
            disk.bytes_read = channel.bytes_read;
            disk.read_utilization_percent = channel.disk_read_utilization_percent;
            disk.read_utilization_available = channel.disk_utilization_available;
            disk.unreadable_files = channel.failed_files;
            disk.timeout_files = channel.timeout_files;
            result.disks.push_back(std::move(disk));
        }
    }
    {
        std::lock_guard<std::mutex> lock(discovery_warning_mutex_);
        result.discovery_warning = discovery_warning_;
    }
    return result;
}

bool ScanCoordinator::FinalizeSynchronized(std::string& error) {
    if (active_scan_id_ == 0 || !manifest_) {
        error = "扫描清单尚未初始化";
        return false;
    }
    std::uint64_t staged = 0;
    std::uint64_t pending = 0;
    RocksStatus status = sync_queue_.StagedCount(active_scan_id_, staged);
    if (!status.succeeded) {
        error = status.message;
        return false;
    }
    status = sync_queue_.PendingCount(active_scan_id_, pending);
    if (!status.succeeded) {
        error = status.message;
        return false;
    }
    if (staged != 0 || pending != 0) {
        error = "当前扫描仍有待同步操作";
        return false;
    }
    if (!manifest_->Cleanup(error)) return false;
    {
        std::lock_guard<std::mutex> lock(progress_mutex_);
        progress_.phase = ScanPhase::CompletedSynchronized;
        progress_.shared_sync_complete = true;
    }
    SaveCheckpoint(ScanPhase::CompletedSynchronized);
    error.clear();
    return true;
}

bool ScanCoordinator::generate_similar_report() const noexcept {
    return options_.generate_similar_report();
}

void ScanCoordinator::InitializeDiscoveryProgress(const std::vector<DiscoveryRoot>& roots) {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(progress_mutex_);
    progress_.discovery_roots.clear();
    progress_.discovery_roots.reserve(roots.size());
    discovery_started_at_.assign(roots.size(), now);
    for (const auto& root : roots) {
        DiscoveryRootProgress item;
        item.root_path = root.path;
        item.backend = options_.discovery().method == DiscoveryMethod::Everything
                           ? DiscoveryBackend::Everything
                           : DiscoveryBackend::Native;
        item.phase = options_.discovery().method == DiscoveryMethod::Everything
                         ? DiscoveryRootPhase::PreparingEverything
                         : DiscoveryRootPhase::ScanningNative;
        progress_.discovery_roots.push_back(std::move(item));
    }
}

void ScanCoordinator::UpdateDiscoveryProgress(const std::uint32_t root_priority,
                                              const DiscoveryBackend backend,
                                              const DiscoveryRootPhase phase,
                                              const std::wstring& fallback_reason) {
    std::lock_guard<std::mutex> lock(progress_mutex_);
    const std::size_t index = static_cast<std::size_t>(root_priority);
    if (index >= progress_.discovery_roots.size()) return;
    auto& item = progress_.discovery_roots[index];
    item.backend = backend;
    item.phase = progress_.cancellation_requested && !IsTerminalDiscoveryPhase(phase)
                     ? DiscoveryRootPhase::Cancelling
                     : phase;
    if (!fallback_reason.empty()) item.fallback_reason = fallback_reason;
}

void ScanCoordinator::CompleteDiscoveryProgress(const std::uint32_t root_priority,
                                                const DiscoveryRootPhase phase) {
    std::lock_guard<std::mutex> lock(progress_mutex_);
    const std::size_t index = static_cast<std::size_t>(root_priority);
    if (index >= progress_.discovery_roots.size()) return;
    auto& item = progress_.discovery_roots[index];
    item.phase = phase;
    if (index < discovery_started_at_.size()) {
        item.elapsed_milliseconds = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - discovery_started_at_[index]).count());
    }
}

void ScanCoordinator::IncrementDiscoveryProgress(const std::uint32_t root_priority) {
    std::lock_guard<std::mutex> lock(progress_mutex_);
    ++progress_.discovered_files;
    const std::size_t index = static_cast<std::size_t>(root_priority);
    if (index < progress_.discovery_roots.size()) {
        ++progress_.discovery_roots[index].discovered_files;
    }
}

void ScanCoordinator::WorkerMain(const std::uint64_t scan_id) {
    try {
        std::string executionLogError;
        if (!execution_logger_.EnsureWritable(executionLogError)) {
            throw std::runtime_error("Cannot prepare execution logs: " + executionLogError);
        }
        if (!execution_logger_.WriteEvent(
                {scan_id, "scan", "started", "discovery", "扫描任务已启动", 0, 0},
                executionLogError)) {
            throw std::runtime_error("Cannot write scan start event: " + executionLogError);
        }
        const auto writeStage = [&](const char* event, const char* stage, const char* message) {
            ProgressSnapshot snapshot;
            {
                std::lock_guard<std::mutex> lock(progress_mutex_);
                snapshot = progress_;
            }
            std::string error;
            if (!execution_logger_.WriteEvent(
                    {scan_id,
                     "scan",
                     event,
                     stage,
                     message,
                     snapshot.hashed_files + snapshot.media_processed_files,
                     snapshot.discovered_files},
                    error)) {
                throw std::runtime_error("Cannot write scan stage event: " + error);
            }
        };
        const std::filesystem::path dataRoot = options_.rocksdb().directory.parent_path().empty()
                                                   ? options_.rocksdb().directory
                                                   : options_.rocksdb().directory.parent_path();
        manifest_ = std::make_unique<ScanManifest>(dataRoot, scan_id);
        std::string manifestError;
        if (!manifest_->Begin(manifestError)) {
            throw std::runtime_error("Cannot prepare scan manifests: " + manifestError);
        }
        const std::string seenPrefix = "manifest_seen/" + std::to_string(scan_id) + "/";
        const RocksStatus clearedSeen = store_.DeletePrefix(
            RocksColumnFamily::Checkpoints, seenPrefix, 4096, false);
        if (!clearedSeen.succeeded) throw std::runtime_error("Cannot reset manifest seen set");
        SaveCheckpoint(ScanPhase::Discovering);
        writeStage("phase_started", "discovery", "开始发现文件");
        std::vector<DiscoveryRoot> roots;
        roots.reserve(options_.scan_roots().size());
        for (std::size_t index = 0; index < options_.scan_roots().size(); ++index) {
            std::string error;
            auto root = NativeFileDiscovery::PrepareRoot(options_.scan_roots()[index],
                                                         static_cast<std::uint32_t>(index),
                                                         options_.io().hdd_extent_optimization,
                                                         error);
            if (!root.has_value()) {
                SetFailure(L"扫描路径磁盘拓扑查询失败：" + options_.scan_roots()[index].wstring());
                break;
            }
            // 记录每盘介质类型，供阶段1电梯排序判断 HDD
            disk_media_types_[root->storage_target_key] = root->media_type;
            roots.push_back(std::move(*root));
        }
        if (roots.empty() || fatal_error_.load()) throw std::runtime_error("No usable scan roots");
        InitializeDiscoveryProgress(roots);

        // 注入 Everything 路径配置（发现器内部 call_once 懒加载）
        if (options_.discovery().method == DiscoveryMethod::Everything) {
            EverythingFileDiscovery::Configure(options_.discovery());
        }

        // 阶段1：发现（收集到 per-disk buffer，不提交哈希）。Everything 对所有根只执行一个批量查询。
        const auto visitor = [this](DiscoveredFile&& file) {
            return CollectDiscovered(std::move(file));
        };
        if (options_.discovery().method == DiscoveryMethod::Everything) {
            const auto stageVisitor = [this](const std::uint32_t priority,
                                             const EverythingDiscoveryStage stage) {
                DiscoveryRootPhase phase = DiscoveryRootPhase::PreparingEverything;
                switch (stage) {
                    case EverythingDiscoveryStage::Preparing:
                        phase = DiscoveryRootPhase::PreparingEverything;
                        break;
                    case EverythingDiscoveryStage::QueryingIndex:
                        phase = DiscoveryRootPhase::QueryingEverything;
                        break;
                    case EverythingDiscoveryStage::ProcessingResults:
                        phase = DiscoveryRootPhase::ProcessingEverythingResults;
                        break;
                    case EverythingDiscoveryStage::QueryingPhysicalLocation:
                        phase = DiscoveryRootPhase::QueryingPhysicalLocation;
                        break;
                }
                UpdateDiscoveryProgress(priority, DiscoveryBackend::Everything, phase);
            };

            const auto everythingResults = EverythingFileDiscovery::EnumerateRoots(
                roots, scan_id, cancel_requested_, visitor, stageVisitor);
            for (std::size_t index = 0; index < roots.size(); ++index) {
                const DiscoveryRoot& root = roots[index];
                const EverythingRootResult& result = everythingResults[index];
                if (result.covered_by_higher_priority) {
                    UpdateDiscoveryProgress(root.priority,
                                            DiscoveryBackend::Everything,
                                            DiscoveryRootPhase::Completed,
                                            L"已由更高优先级扫描路径覆盖");
                    CompleteDiscoveryProgress(root.priority, DiscoveryRootPhase::Completed);
                    continue;
                }
                if (fatal_error_.load()) {
                    CompleteDiscoveryProgress(root.priority, DiscoveryRootPhase::Failed);
                    continue;
                }
                if (result.stats.cancelled || cancel_requested_.load()) {
                    CompleteDiscoveryProgress(root.priority, DiscoveryRootPhase::Cancelled);
                    continue;
                }
                if (result.stats.error.empty() && result.stats.discovered_files != 0) {
                    CompleteDiscoveryProgress(root.priority, DiscoveryRootPhase::Completed);
                    continue;
                }

                const std::wstring reason = result.stats.error.empty()
                                                ? L"Everything 未返回文件"
                                                : Utf8ToWide(result.stats.error);
                UpdateDiscoveryProgress(root.priority,
                                        DiscoveryBackend::EverythingThenNative,
                                        DiscoveryRootPhase::NativeFallback,
                                        reason);
                {
                    std::lock_guard<std::mutex> warnLock(discovery_warning_mutex_);
                    if (discovery_warning_.empty()) {
                        discovery_warning_ = L"Everything 文件发现未覆盖扫描路径，已回退系统原生遍历：\n- 扫描路径：" +
                                             root.path.wstring() + L"\n- 原因：" + reason;
                    }
                }

                const DiscoveryStats nativeStats = NativeFileDiscovery::Enumerate(
                    root, scan_id, cancel_requested_, visitor);
                if (fatal_error_.load()) {
                    CompleteDiscoveryProgress(root.priority, DiscoveryRootPhase::Failed);
                } else if (nativeStats.cancelled || cancel_requested_.load()) {
                    CompleteDiscoveryProgress(root.priority, DiscoveryRootPhase::Cancelled);
                } else if (!nativeStats.error.empty()) {
                    CompleteDiscoveryProgress(root.priority, DiscoveryRootPhase::Failed);
                    SetFailure(L"文件发现失败：" + root.path.wstring());
                } else {
                    CompleteDiscoveryProgress(root.priority, DiscoveryRootPhase::Completed);
                }
            }
        } else {
            std::vector<std::thread> discoveryThreads;
            discoveryThreads.reserve(roots.size());
            try {
                for (const DiscoveryRoot& root : roots) {
                    discoveryThreads.emplace_back([this, root, scan_id, visitor] {
                        try {
                            UpdateDiscoveryProgress(root.priority,
                                                    DiscoveryBackend::Native,
                                                    DiscoveryRootPhase::ScanningNative);
                            const DiscoveryStats stats = NativeFileDiscovery::Enumerate(
                                root, scan_id, cancel_requested_, visitor);
                            if (fatal_error_.load()) {
                                CompleteDiscoveryProgress(root.priority, DiscoveryRootPhase::Failed);
                            } else if (stats.cancelled || cancel_requested_.load()) {
                                CompleteDiscoveryProgress(root.priority, DiscoveryRootPhase::Cancelled);
                            } else if (!stats.error.empty()) {
                                CompleteDiscoveryProgress(root.priority, DiscoveryRootPhase::Failed);
                                SetFailure(L"文件发现失败：" + root.path.wstring());
                            } else {
                                CompleteDiscoveryProgress(root.priority, DiscoveryRootPhase::Completed);
                            }
                        } catch (const std::bad_alloc&) {
                            CompleteDiscoveryProgress(root.priority, DiscoveryRootPhase::Failed);
                            SetFailure(L"文件发现线程内存不足。");
                            ApplicationErrorLogger::Write(
                                {"error", "thread_exception", "DedupCore", "scan_discovery",
                                 "文件发现线程内存不足", "std::bad_alloc", {}, 0});
                        } catch (const std::exception& exception) {
                            CompleteDiscoveryProgress(root.priority, DiscoveryRootPhase::Failed);
                            SetFailure(L"文件发现线程异常。");
                            ApplicationErrorLogger::Write(
                                {"error", "thread_exception", "DedupCore", "scan_discovery",
                                 exception.what(), "std::exception", {}, 0});
                        } catch (...) {
                            CompleteDiscoveryProgress(root.priority, DiscoveryRootPhase::Failed);
                            SetFailure(L"文件发现线程发生未知异常。");
                            ApplicationErrorLogger::Write(
                                {"error", "thread_exception", "DedupCore", "scan_discovery",
                                 "文件发现线程发生未知异常", "unknown_exception", {}, 0});
                        }
                    });
                }
            } catch (...) {
                cancel_requested_.store(true);
                for (std::thread& thread : discoveryThreads) {
                    if (thread.joinable()) thread.join();
                }
                throw;
            }
            for (std::thread& thread : discoveryThreads) thread.join();
        }

        if (cancel_requested_.load()) throw std::runtime_error("Scan cancelled during discovery");
        if (fatal_error_.load()) throw std::runtime_error("File discovery failed");

        // Barrier：发现完成后原子发布每盘 JSONL，再流式联合本地与 MySQL 规划缺失能力。
        std::string finalManifestError;
        if (!manifest_->Complete(finalManifestError)) {
            throw std::runtime_error("Cannot finalize scan manifests: " + finalManifestError);
        }
        const RocksStatus removedSeen = store_.DeletePrefix(
            RocksColumnFamily::Checkpoints,
            "manifest_seen/" + std::to_string(scan_id) + "/",
            4096,
            false);
        if (!removedSeen.succeeded) throw std::runtime_error("Cannot clear manifest seen set");
        {
            std::lock_guard<std::mutex> lock(progress_mutex_);
            progress_.phase = ScanPhase::Planning;
        }
        SaveCheckpoint(ScanPhase::Planning);
        writeStage("phase_started", "planning", "开始联合本地与 MySQL 规划任务");
        if (!PlanManifestFiles(scan_id)) throw std::runtime_error("Cannot plan scan manifests");

        if (!MarkUnseenPaths(scan_id)) throw std::runtime_error("Cannot mark unseen paths");

        // 阶段2：启动哈希调度器，按电梯排序顺序批量提交
        std::map<std::wstring, DiskChannelOptions> channelMap;
        for (const DiscoveryRoot& root : roots) {
            DiskChannelOptions& channel = channelMap[root.storage_target_key];
            channel.storage_target_key = root.storage_target_key;
            channel.media_type = root.media_type;
            channel.read_threads = ReadThreadsFor(options_, root.media_type);
            channel.queue_capacity = options_.io().per_disk_queue_capacity;
            channel.hdd_extent_optimization = options_.io().hdd_extent_optimization;
            channel.hdd_sort_window = options_.io().hdd_sort_window;
        }
        std::vector<DiskChannelOptions> channels;
        for (auto& item : channelMap) channels.push_back(std::move(item.second));
        {
            std::lock_guard<std::mutex> lock(progress_mutex_);
            if (!fatal_error_.load() && !cancel_requested_.load()) progress_.phase = ScanPhase::Hashing;
        }
        SaveCheckpoint(ScanPhase::Hashing);
        writeStage("phase_started", "sha512", "开始计算 SHA-512");
        {
            std::lock_guard<std::mutex> lock(scheduler_control_mutex_);
            if (cancel_requested_.load()) throw std::runtime_error("Scan cancelled before hashing");
            hash_scheduler_->Start(std::move(channels), [this](FileHashOutcome outcome) {
                HandleHashCompleted(std::move(outcome));
            });
        }
        SubmitDiscoveredJobs();
        {
            std::lock_guard<std::mutex> lock(scheduler_control_mutex_);
            if (cancel_requested_.load()) hash_scheduler_->RequestCancel();
            else hash_scheduler_->CloseSubmissions();
        }
        {
            std::lock_guard<std::mutex> lock(scheduler_control_mutex_);
            hash_scheduler_->Join();
        }
        disk_media_types_.clear();
        if (fatal_error_.load()) throw std::runtime_error("Fatal scan persistence error");
        if (cancel_requested_.load()) {
            {
                std::lock_guard<std::mutex> lock(progress_mutex_);
                progress_.phase = ScanPhase::Cancelled;
            }
            SaveCheckpoint(ScanPhase::Cancelled);
            writeStage("cancelled", "sha512", "扫描已取消");
            running_.store(false);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(progress_mutex_);
            progress_.phase = ScanPhase::ExtractingMedia;
        }
        SaveCheckpoint(ScanPhase::ExtractingMedia);
        writeStage("phase_started", "media_features", "开始提取媒体特征");
        if (!ProcessMediaPhase() || fatal_error_.load()) throw std::runtime_error("Media phase failed");
        {
            std::lock_guard<std::mutex> lock(progress_mutex_);
            progress_.phase = ScanPhase::FlushingSyncTail;
        }
        SaveCheckpoint(ScanPhase::FlushingSyncTail);
        writeStage("phase_started", "sync_tail", "开始发布 MySQL 同步尾批");
        if (!PublishAvailableBatches(true) || fatal_error_.load()) {
            throw std::runtime_error("Cannot publish final MySQL sync batch");
        }
        {
            std::lock_guard<std::mutex> lock(progress_mutex_);
            progress_.phase = ScanPhase::CompletedLocal;
            progress_.local_scan_complete = true;
        }
        SaveCheckpoint(ScanPhase::CompletedLocal);
        writeStage("completed", "completed_local", "本地扫描计算完成");
    } catch (const std::bad_alloc&) {
        ApplicationErrorLogger::Write(
            {"error", "thread_exception", "DedupCore", "scan_coordinator",
             "扫描协调线程内存不足", "std::bad_alloc", {}, 0});
        try { SetFailure(L"扫描协调线程内存不足。"); SaveCheckpoint(ScanPhase::Failed); } catch (...) {}
    } catch (const std::exception& exception) {
        ApplicationErrorLogger::Write(
            {"error", "thread_exception", "DedupCore", "scan_coordinator",
             exception.what(), "std::exception", {}, 0});
        try {
            const ScanPhase failurePhase = fatal_error_.load() ? ScanPhase::Failed
                                                                : (cancel_requested_.load() ? ScanPhase::Cancelled
                                                                                           : ScanPhase::Failed);
            if (failurePhase == ScanPhase::Failed) {
                SetFailure(L"扫描协调线程异常。");
            } else {
                std::lock_guard<std::mutex> lock(progress_mutex_);
                progress_.phase = ScanPhase::Cancelled;
                progress_.cancellation_requested = true;
                const auto now = std::chrono::steady_clock::now();
                for (std::size_t index = 0; index < progress_.discovery_roots.size(); ++index) {
                    auto& root = progress_.discovery_roots[index];
                    if (!IsTerminalDiscoveryPhase(root.phase)) root.phase = DiscoveryRootPhase::Cancelled;
                    if (index < discovery_started_at_.size()) {
                        root.elapsed_milliseconds = static_cast<std::uint64_t>(
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                now - discovery_started_at_[index]).count());
                    }
                }
            }
            SaveCheckpoint(failurePhase);
        } catch (...) {}
    } catch (...) {
        ApplicationErrorLogger::Write(
            {"error", "thread_exception", "DedupCore", "scan_coordinator",
             "扫描协调线程发生未知异常", "unknown_exception", {}, 0});
        try { SetFailure(L"扫描协调线程发生未知异常。"); SaveCheckpoint(ScanPhase::Failed); } catch (...) {}
    }
    ScanPhase finalPhase = ScanPhase::Idle;
    {
        std::lock_guard<std::mutex> lock(progress_mutex_);
        finalPhase = progress_.phase;
    }
    if (finalPhase == ScanPhase::Failed || finalPhase == ScanPhase::Cancelled) {
        std::string executionLogError;
        execution_logger_.WriteEvent(
            {scan_id,
             "scan",
             finalPhase == ScanPhase::Cancelled ? "cancelled" : "failed",
             finalPhase == ScanPhase::Cancelled ? "cancelled" : "failed",
             finalPhase == ScanPhase::Cancelled ? "扫描已取消" : "扫描失败",
             0,
             0},
            executionLogError);
    }
    running_.store(false);
}

bool ScanCoordinator::MarkUnseenPaths(const std::uint64_t scan_id) {
    std::vector<std::wstring> normalizedRoots;
    normalizedRoots.reserve(options_.scan_roots().size());
    for (const std::filesystem::path& root : options_.scan_roots()) {
        normalizedRoots.push_back(NormalizeScopePath(root));
    }
    const RocksStatus iterated = store_.ForEachPrefix(
        RocksColumnFamily::FilePaths,
        "path/",
        0,
        [&](const std::string_view key, const std::string_view value) {
            if (cancel_requested_.load() || fatal_error_.load()) return false;
            std::string error;
            std::optional<FilePathRecord> record =
                CoreModelCodec::DeserializeFilePath(std::string(value), error);
            if (!record.has_value()) {
                SetFailure(L"RocksDB 路径记录损坏。");
                return false;
            }
            if (record->scan_id == scan_id || record->state == FilePathState::ProgramDeleted) return true;
            const std::wstring normalizedPath = NormalizeScopePath(record->path);
            const bool inScope = std::any_of(normalizedRoots.begin(), normalizedRoots.end(), [&](const auto& root) {
                return IsUnderRoot(normalizedPath, root);
            });
            const FilePathState desired = inScope ? FilePathState::Missing : FilePathState::OutOfScope;
            if (record->state == desired) return true;
            record->scan_id = scan_id;
            record->state = desired;
            if (!record->sha512.has_value()) {
                record->sync_state = SyncState::LocalOnly;
                const RocksStatus saved = store_.Put(RocksColumnFamily::FilePaths,
                                                     key,
                                                     CoreModelCodec::SerializeFilePath(*record),
                                                     false);
                if (!saved.succeeded) SetFailure(L"RocksDB 缺失路径状态写入失败。");
                return saved.succeeded;
            }
            record->sync_state = SyncState::Pending;
            SyncOperation operation;
            operation.kind = SyncOperationKind::UpsertFilePath;
            operation.file_path = *record;
            return StageOperation(
                {{RocksColumnFamily::FilePaths,
                  std::string(key),
                  CoreModelCodec::SerializeFilePath(*record)}},
                std::move(operation));
        });
    if (!iterated.succeeded) SetFailure(L"RocksDB 缺失路径扫描失败。");
    return iterated.succeeded && !fatal_error_.load();
}

bool ScanCoordinator::CollectDiscovered(DiscoveredFile&& file) {
    if (cancel_requested_.load() || fatal_error_.load()) return false;
    const std::uint32_t rootPriority = file.record.scan_root_priority;
    const std::string seenKey = "manifest_seen/" + std::to_string(file.record.scan_id) + "/" +
                                WideToUtf8(file.record.normalized_path_key);
    {
        std::lock_guard<std::mutex> lock(discovery_record_mutex_);
        std::string existingPriority;
        const RocksStatus existing = store_.Get(
            RocksColumnFamily::Checkpoints, seenKey, existingPriority);
        if (existing.succeeded) {
            std::uint64_t parsed = 0;
            const auto parsedResult = std::from_chars(existingPriority.data(),
                                                      existingPriority.data() + existingPriority.size(),
                                                      parsed);
            if (parsedResult.ec != std::errc{} ||
                parsedResult.ptr != existingPriority.data() + existingPriority.size()) {
                SetFailure(L"扫描清单去重记录损坏。");
                return false;
            }
            if (parsed <= rootPriority) return true;
        } else if (existing.message != "not_found") {
            SetFailure(L"扫描清单去重记录读取失败。");
            return false;
        }
        const RocksStatus saved = store_.Put(RocksColumnFamily::Checkpoints,
                                             seenKey,
                                             std::to_string(rootPriority),
                                             false);
        if (!saved.succeeded) {
            SetFailure(L"扫描清单去重记录写入失败。");
            return false;
        }
    }
    const auto media = disk_media_types_.find(file.record.storage_target_key);
    const StorageMediaType mediaType = media == disk_media_types_.end()
                                           ? StorageMediaType::Unknown
                                           : media->second;
    std::string manifestError;
    if (!manifest_ || !manifest_->Append(file, mediaType, manifestError)) {
        SetFailure(L"按物理盘写入扫描清单失败。");
        return false;
    }
    IncrementDiscoveryProgress(rootPriority);
    return true;
}

bool ScanCoordinator::PlanManifestFiles(const std::uint64_t scan_id) {
    if (!manifest_) return false;
    constexpr std::size_t kPlanningBatchSize = 512;
    const std::string hashSeenPrefix = "planned_hash_seen/" + std::to_string(scan_id) + "/";
    const RocksStatus clearedHashSeen = store_.DeletePrefix(
        RocksColumnFamily::Checkpoints, hashSeenPrefix, 4096, false);
    if (!clearedHashSeen.succeeded) return false;
    std::vector<DiscoveredFile> batch;
    batch.reserve(kPlanningBatchSize);
    bool mysqlAvailable = true;
    std::uint64_t hashTotal = 0;

    const auto markDegraded = [&] {
        mysqlAvailable = false;
        std::lock_guard<std::mutex> lock(progress_mutex_);
        progress_.mysql_planning_degraded = true;
    };

    const auto processBatch = [&]() -> bool {
        if (batch.empty()) return true;
        std::vector<std::wstring> normalizedPaths;
        normalizedPaths.reserve(batch.size());
        for (const DiscoveredFile& file : batch) {
            normalizedPaths.push_back(file.record.normalized_path_key);
        }

        std::unordered_map<std::wstring, FilePathRecord> remotePaths;
        if (mysqlAvailable) {
            const MySqlStatus loaded = mysql_reader_.LoadPaths(normalizedPaths, remotePaths);
            if (!loaded.succeeded) markDegraded();
        }

        struct PlannedItem {
            DiscoveredFile* file = nullptr;
            std::optional<FilePathRecord> existing;
            std::optional<ShaFileData> content;
            bool reuse = false;
            bool already_pending = false;
        };
        std::vector<PlannedItem> planned;
        planned.reserve(batch.size());
        std::vector<Sha512Digest> missingContents;

        for (DiscoveredFile& file : batch) {
            PlannedItem item;
            item.file = &file;
            bool corrupted = false;
            item.existing = LoadPathRecord(
                store_, PathRecordKey(file.record.normalized_path_key), corrupted);
            if (corrupted) {
                SetFailure(L"RocksDB 路径记录损坏。");
                return false;
            }
            if (!item.existing.has_value()) {
                const auto remote = remotePaths.find(file.record.normalized_path_key);
                if (remote != remotePaths.end()) item.existing = remote->second;
            }
            item.already_pending = item.existing.has_value() &&
                                   item.existing->scan_id == scan_id &&
                                   item.existing->state == FilePathState::Pending;
            item.reuse = item.existing.has_value() && CanReuse(*item.existing, file.record);
            if (item.reuse && item.existing->sha512.has_value() &&
                (file.media_kind == MediaKind::Image || file.media_kind == MediaKind::Video)) {
                bool contentCorrupted = false;
                item.content = LoadShaData(store_, *item.existing->sha512, contentCorrupted);
                if (contentCorrupted) {
                    SetFailure(L"RocksDB 内容记录损坏。");
                    return false;
                }
                if ((!item.content.has_value() ||
                     !MediaComplete(*item.content, file.media_kind, options_.algorithm_version())) &&
                    mysqlAvailable) {
                    missingContents.push_back(*item.existing->sha512);
                }
            }
            planned.push_back(std::move(item));
        }

        std::unordered_map<std::string, ShaFileData> remoteContents;
        if (mysqlAvailable && !missingContents.empty()) {
            const MySqlStatus loaded = mysql_reader_.LoadContents(missingContents, remoteContents);
            if (!loaded.succeeded) markDegraded();
        }

        for (PlannedItem& item : planned) {
            DiscoveredFile& file = *item.file;
            const std::string pathKey = PathRecordKey(file.record.normalized_path_key);
            if (item.reuse) {
                FilePathRecord reused = *item.existing;
                reused.scan_id = scan_id;
                reused.path = file.record.path;
                reused.normalized_path_key = file.record.normalized_path_key;
                reused.volume_guid = file.record.volume_guid;
                reused.storage_target_key = file.record.storage_target_key;
                reused.extension = file.record.extension;
                reused.scan_root_priority = (std::min)(reused.scan_root_priority,
                                                        file.record.scan_root_priority);
                reused.state = FilePathState::Unchanged;

                if ((!item.content.has_value() ||
                     !MediaComplete(*item.content, file.media_kind, options_.algorithm_version())) &&
                    reused.sha512.has_value()) {
                    const auto remote = remoteContents.find(Sha512ToHex(*reused.sha512));
                    if (remote != remoteContents.end() &&
                        MediaComplete(remote->second, file.media_kind, options_.algorithm_version())) {
                        item.content = remote->second;
                        const RocksStatus savedContent = store_.Put(
                            RocksColumnFamily::ShaFileData,
                            Sha512ToHex(remote->second.sha512),
                            CoreModelCodec::SerializeShaFileData(remote->second),
                            false);
                        if (!savedContent.succeeded) {
                            SetFailure(L"MySQL 内容规划缓存写入失败。");
                            return false;
                        }
                    }
                }
                if ((file.media_kind == MediaKind::Image || file.media_kind == MediaKind::Video) &&
                    (!item.content.has_value() ||
                     !MediaComplete(*item.content, file.media_kind, options_.algorithm_version()))) {
                    const RocksStatus mediaSaved = store_.Put(
                        RocksColumnFamily::Checkpoints,
                        MediaTaskKey(scan_id, *reused.sha512),
                        CoreModelCodec::SerializeFilePath(reused),
                        false);
                    if (!mediaSaved.succeeded) {
                        SetFailure(L"RocksDB 媒体任务写入失败。");
                        return false;
                    }
                }
                const RocksStatus saved = store_.Put(RocksColumnFamily::FilePaths,
                                                     pathKey,
                                                     CoreModelCodec::SerializeFilePath(reused),
                                                     false);
                if (!saved.succeeded) {
                    SetFailure(L"RocksDB 复用路径写入失败。");
                    return false;
                }
                continue;
            }

            FilePathRecord pending = file.record;
            pending.path_id = item.existing.has_value() ? item.existing->path_id : RandomId();
            pending.scan_root_priority = item.existing.has_value() && item.already_pending
                                             ? (std::min)(item.existing->scan_root_priority,
                                                          file.record.scan_root_priority)
                                             : file.record.scan_root_priority;
            pending.state = FilePathState::Pending;
            pending.sync_state = SyncState::LocalOnly;
            const RocksStatus saved = store_.Put(RocksColumnFamily::FilePaths,
                                                 pathKey,
                                                 CoreModelCodec::SerializeFilePath(pending),
                                                 false);
            if (!saved.succeeded) {
                SetFailure(L"RocksDB 待哈希路径写入失败。");
                return false;
            }
            const std::string hashSeenKey = hashSeenPrefix + WideToUtf8(pending.normalized_path_key);
            std::string ignored;
            const RocksStatus hashSeen = store_.Get(
                RocksColumnFamily::Checkpoints, hashSeenKey, ignored);
            if (!hashSeen.succeeded) {
                if (hashSeen.message != "not_found") {
                    SetFailure(L"哈希规划去重记录读取失败。");
                    return false;
                }
                const RocksStatus marked = store_.Put(
                    RocksColumnFamily::Checkpoints, hashSeenKey, std::string{}, false);
                if (!marked.succeeded) {
                    SetFailure(L"哈希规划去重记录写入失败。");
                    return false;
                }
                ++hashTotal;
            }
        }
        batch.clear();
        return true;
    };

    std::string manifestError;
    const bool iterated = manifest_->ForEach(
        [&](const ScanManifestFile&, DiscoveredFile&& file) {
            if (cancel_requested_.load() || fatal_error_.load()) return false;
            batch.push_back(std::move(file));
            return batch.size() < kPlanningBatchSize || processBatch();
        },
        manifestError);
    if (!iterated || !manifestError.empty() || !processBatch()) {
        if (!fatal_error_.load()) SetFailure(L"扫描清单规划失败。");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(progress_mutex_);
        progress_.hash_total_files = hashTotal;
    }
    store_.DeletePrefix(RocksColumnFamily::Checkpoints, hashSeenPrefix, 4096, false);
    return true;
}

void ScanCoordinator::SubmitDiscoveredJobs() {
    if (!manifest_) {
        SetFailure(L"扫描清单尚未初始化。");
        return;
    }
    const std::vector<ScanManifestFile> manifests = manifest_->files();
    std::vector<std::thread> producers;
    producers.reserve(manifests.size());
    try {
        for (const ScanManifestFile& manifestFile : manifests) {
            producers.emplace_back([this, manifestFile] {
                try {
                    std::string manifestError;
                    const bool iterated = manifest_->ForEachFile(
                        manifestFile,
                        [&](const ScanManifestFile&, DiscoveredFile&& file) {
                            if (cancel_requested_.load() || fatal_error_.load()) return false;
                            bool corrupted = false;
                            std::optional<FilePathRecord> planned = LoadPathRecord(
                                store_, PathRecordKey(file.record.normalized_path_key), corrupted);
                            if (corrupted) {
                                SetFailure(L"RocksDB 已规划路径记录损坏。");
                                return false;
                            }
                            if (!planned.has_value() || planned->scan_id != active_scan_id_ ||
                                planned->state != FilePathState::Pending ||
                                planned->scan_root_priority != file.record.scan_root_priority) {
                                return true;
                            }
                            FileHashJob job;
                            job.job_id = planned->path_id;
                            job.scan_id = planned->scan_id;
                            job.path = planned->path;
                            job.storage_target_key = planned->storage_target_key;
                            job.physical_start_byte = file.physical_start_byte;
                            job.discovered_record = *planned;
                            job.media_kind = file.media_kind;
                            while (!cancel_requested_.load() && !fatal_error_.load()) {
                                if (hash_scheduler_->Submit(job, 250)) return true;
                            }
                            return false;
                        },
                        manifestError);
                    if (!iterated || !manifestError.empty()) SetFailure(L"扫描清单读取失败。");
                } catch (const std::exception& exception) {
                    ApplicationErrorLogger::Write(
                        {"error", "thread_exception", "DedupCore", "manifest_submitter",
                         exception.what(), "std::exception", {}, 0});
                    SetFailure(L"扫描清单提交线程异常。");
                } catch (...) {
                    SetFailure(L"扫描清单提交线程发生未知异常。");
                }
            });
        }
    } catch (...) {
        cancel_requested_.store(true);
        for (std::thread& producer : producers) if (producer.joinable()) producer.join();
        throw;
    }
    for (std::thread& producer : producers) producer.join();
}

void ScanCoordinator::HandleHashCompleted(FileHashOutcome outcome) {
    // 调度器已经完成一次哈希尝试；先推进阶段进度，后续持久化失败由任务终态单独表达。
    {
        std::lock_guard<std::mutex> lock(progress_mutex_);
        ++progress_.hash_processed_files;
    }
    FilePathRecord record = std::move(outcome.job.discovered_record);
    const std::string pathKey = PathRecordKey(record.normalized_path_key);
    if (outcome.result.status != FileHashStatus::Succeeded || !outcome.result.sha512.has_value()) {
        record.state = FailedPathState(outcome.result.status);
        record.sync_state = SyncState::LocalOnly;
        if (outcome.result.status != FileHashStatus::Cancelled) {
            ExecutionFailureRecord failure;
            failure.task_id = record.scan_id;
            failure.path_id = record.path_id;
            failure.task = "scan";
            failure.stage = "sha512";
            failure.operation = "hash_file";
            failure.path = record.path;
            failure.storage_target_key = record.storage_target_key;
            failure.media_kind = MediaKindName(outcome.job.media_kind);
            failure.status = HashStatusName(outcome.result.status);
            failure.status_code = static_cast<std::uint32_t>(outcome.result.status);
            failure.native_error = outcome.result.system_error;
            failure.failed_offset = outcome.result.failed_offset;
            failure.bytes_read = outcome.result.bytes_read;
            std::string logError;
            if (!execution_logger_.WriteFailure(failure, logError)) {
                SetFailure(L"扫描读取错误日志写入失败。");
                return;
            }
        }
        const RocksStatus saved = store_.Put(RocksColumnFamily::FilePaths,
                                             pathKey,
                                             CoreModelCodec::SerializeFilePath(record),
                                             true);
        if (!saved.succeeded) SetFailure(L"RocksDB 哈希失败状态写入失败。");
        std::lock_guard<std::mutex> lock(progress_mutex_);
        if (outcome.result.status != FileHashStatus::Cancelled) ++progress_.failed_files;
        progress_.bytes_read += outcome.result.bytes_read;
        return;
    }

    record.sha512 = outcome.result.sha512;
    record.size_bytes = outcome.result.file_size;
    record.identity = outcome.result.identity;
    record.creation_time_utc_ms = outcome.result.creation_time_utc_ms;
    record.last_write_time_utc_ms = outcome.result.last_write_time_utc_ms;
    record.state = FilePathState::Available;
    record.sync_state = SyncState::Pending;

    bool corrupted = false;
    std::optional<ShaFileData> content = LoadShaData(store_, *record.sha512, corrupted);
    if (corrupted) {
        SetFailure(L"RocksDB 内容记录损坏。");
        return;
    }
    const bool mediaFile = outcome.job.media_kind == MediaKind::Image ||
                           outcome.job.media_kind == MediaKind::Video;
    if ((!content.has_value() ||
         (mediaFile && !MediaComplete(*content, outcome.job.media_kind, options_.algorithm_version())))) {
        bool planningDegraded = false;
        {
            std::lock_guard<std::mutex> lock(progress_mutex_);
            planningDegraded = progress_.mysql_planning_degraded;
        }
        if (!planningDegraded) {
            std::unordered_map<std::string, ShaFileData> remote;
            const MySqlStatus loaded = mysql_reader_.LoadContents({*record.sha512}, remote);
            if (loaded.succeeded) {
                const auto found = remote.find(Sha512ToHex(*record.sha512));
                if (found != remote.end() &&
                    (!mediaFile || MediaComplete(found->second,
                                                  outcome.job.media_kind,
                                                  options_.algorithm_version()))) {
                    content = found->second;
                    const RocksStatus cached = store_.Put(
                        RocksColumnFamily::ShaFileData,
                        Sha512ToHex(content->sha512),
                        CoreModelCodec::SerializeShaFileData(*content),
                        false);
                    if (!cached.succeeded) {
                        SetFailure(L"MySQL 内容复用缓存写入失败。");
                        return;
                    }
                }
            } else {
                std::lock_guard<std::mutex> lock(progress_mutex_);
                progress_.mysql_planning_degraded = true;
            }
        }
    }
    if (!content.has_value()) {
        ShaFileData created;
        created.sha512 = *record.sha512;
        created.content_size_bytes = record.size_bytes;
        created.media_kind = outcome.job.media_kind;
        SyncOperation contentOperation;
        contentOperation.kind = SyncOperationKind::UpsertShaFileData;
        contentOperation.sha_file_data = created;
        if (!StageOperation(
                {{RocksColumnFamily::ShaFileData,
                  Sha512ToHex(created.sha512),
                  CoreModelCodec::SerializeShaFileData(created)}},
                std::move(contentOperation))) {
            SetFailure(L"内容记录与 MySQL 待同步消息写入失败。");
            return;
        }
        content = created;
    }

    std::vector<RocksMutation> pathMutations = {
        {RocksColumnFamily::FilePaths, pathKey, CoreModelCodec::SerializeFilePath(record)},
    };
    if ((outcome.job.media_kind == MediaKind::Image || outcome.job.media_kind == MediaKind::Video) &&
        !MediaComplete(*content, outcome.job.media_kind, options_.algorithm_version())) {
        pathMutations.push_back({RocksColumnFamily::Checkpoints,
                                 MediaTaskKey(record.scan_id, *record.sha512),
                                 CoreModelCodec::SerializeFilePath(record)});
    }
    SyncOperation pathOperation;
    pathOperation.kind = SyncOperationKind::UpsertFilePath;
    pathOperation.file_path = record;
    if (!StageOperation(pathMutations, std::move(pathOperation))) {
        SetFailure(L"路径映射与 MySQL 待同步消息写入失败。");
        return;
    }
    std::lock_guard<std::mutex> lock(progress_mutex_);
    ++progress_.hashed_files;
    progress_.bytes_read += outcome.result.bytes_read;
}

bool ScanCoordinator::ProcessMediaPhase() {
    const std::size_t capacity = (std::max<std::size_t>)(2, options_.compute().worker_threads * 2ULL);
    PhaseComputeGate computeGate(options_.compute().worker_threads,
                                 options_.compute().adaptive_worker_threads);
    std::atomic_bool controllerStop{false};
    std::mutex controllerMutex;
    std::condition_variable controllerCondition;
    std::thread controller;

    // 媒体任务键已经按 SHA-512 去重；先做轻量计数，确保进度分母稳定且不会倒退。
    std::uint64_t scanId = 0;
    {
        std::lock_guard<std::mutex> lock(progress_mutex_);
        scanId = progress_.scan_id;
    }
    const std::string mediaTaskPrefix = MediaTaskPrefix(scanId);
    std::uint64_t mediaTotalFiles = 0;
    const RocksStatus counted = store_.ForEachPrefix(
        RocksColumnFamily::Checkpoints,
        mediaTaskPrefix,
        0,
        [&](const std::string_view, const std::string_view) {
            if (cancel_requested_.load() || fatal_error_.load()) return false;
            ++mediaTotalFiles;
            return true;
        });
    if (!counted.succeeded) {
        SetFailure(L"RocksDB 媒体任务计数失败。");
        return false;
    }
    if (cancel_requested_.load() || fatal_error_.load()) return false;
    {
        std::lock_guard<std::mutex> lock(progress_mutex_);
        progress_.media_total_files = mediaTotalFiles;
        progress_.media_total_known = true;
        progress_.allowed_compute_threads = computeGate.capacity();
        progress_.active_compute_threads = 0;
        progress_.system_cpu_percent = 0.0;
    }

    try {
        TaskThreadPool mediaPool("media-compute",
                                 options_.compute().worker_threads,
                                 capacity);
        if (options_.compute().adaptive_worker_threads) {
            controller = std::thread([&] {
                try {
                    SystemCpuSampler sampler;
                    while (!controllerStop.load() && !cancel_requested_.load() && !fatal_error_.load()) {
                        {
                            std::unique_lock<std::mutex> lock(controllerMutex);
                            controllerCondition.wait_for(lock, std::chrono::seconds(1), [&] {
                                return controllerStop.load() || cancel_requested_.load() || fatal_error_.load();
                            });
                        }
                        if (controllerStop.load() || cancel_requested_.load() || fatal_error_.load()) break;
                        const std::optional<double> load = sampler.Sample();
                        if (!load.has_value()) continue;
                        bool hasPending = false;
                        {
                            std::lock_guard<std::mutex> lock(progress_mutex_);
                            progress_.system_cpu_percent = *load;
                            hasPending = progress_.media_processed_files < progress_.media_total_files;
                        }
                        if (hasPending && *load < options_.compute().cpu_target_percent &&
                            computeGate.IncreaseOne()) {
                            std::lock_guard<std::mutex> lock(progress_mutex_);
                            progress_.allowed_compute_threads = computeGate.capacity();
                        }
                    }
                } catch (const std::exception& exception) {
                    ApplicationErrorLogger::Write(
                        {"error", "thread_exception", "DedupCore", "media_cpu_controller",
                         exception.what(), "std::exception", {}, 0});
                } catch (...) {
                    ApplicationErrorLogger::Write(
                        {"error", "thread_exception", "DedupCore", "media_cpu_controller",
                         "媒体 CPU 自适应线程发生未知异常", "unknown_exception", {}, 0});
                }
            });
        }

        const RocksStatus iterated = store_.ForEachPrefix(
            RocksColumnFamily::Checkpoints,
            mediaTaskPrefix,
            0,
            [&](const std::string_view key, const std::string_view value) {
                if (cancel_requested_.load() || fatal_error_.load()) return false;
                std::string taskKey(key);
                std::string taskValue(value);
                const bool submitted = mediaPool.Submit(
                    [&, taskKey = std::move(taskKey), taskValue = std::move(taskValue)]() mutable {
                        if (cancel_requested_.load() || fatal_error_.load()) return;
                        if (!computeGate.Acquire(cancel_requested_)) return;
                        {
                            std::lock_guard<std::mutex> lock(progress_mutex_);
                            progress_.active_compute_threads = computeGate.active();
                        }
                        try {
                            ProcessMediaTask(std::move(taskKey), std::move(taskValue));
                        } catch (...) {
                            computeGate.Release();
                            {
                                std::lock_guard<std::mutex> lock(progress_mutex_);
                                progress_.active_compute_threads = computeGate.active();
                            }
                            throw;
                        }
                        computeGate.Release();
                        {
                            std::lock_guard<std::mutex> lock(progress_mutex_);
                            progress_.active_compute_threads = computeGate.active();
                        }
                    });
                if (!submitted) return false;
                const TaskThreadPoolSnapshot poolSnapshot = mediaPool.snapshot();
                {
                    std::lock_guard<std::mutex> lock(progress_mutex_);
                    progress_.queued_compute_tasks = poolSnapshot.queued_tasks;
                }
                return true;
            });

        mediaPool.CloseSubmissions();
        if (cancel_requested_.load() || fatal_error_.load()) {
            computeGate.WakeAll();
            mediaPool.RequestCancel();
        }
        mediaPool.Join();
        controllerStop.store(true);
        controllerCondition.notify_all();
        if (controller.joinable()) controller.join();
        {
            std::lock_guard<std::mutex> lock(progress_mutex_);
            progress_.queued_compute_tasks = 0;
            progress_.active_compute_threads = 0;
        }

        const std::string workerFailure = mediaPool.failure_message();
        if (!workerFailure.empty()) {
            ApplicationErrorLogger::Write(
                {"error", "thread_exception", "DedupCore", "media_analysis",
                 workerFailure, "task_exception", {}, 0});
            SetFailure(L"媒体处理线程异常。");
        }
        if (!iterated.succeeded) SetFailure(L"RocksDB 媒体任务迭代失败。");
        return iterated.succeeded && workerFailure.empty() &&
               !fatal_error_.load() && !cancel_requested_.load();
    } catch (const std::bad_alloc&) {
        cancel_requested_.store(true);
        computeGate.WakeAll();
        controllerStop.store(true);
        controllerCondition.notify_all();
        if (controller.joinable()) controller.join();
        ApplicationErrorLogger::Write(
            {"error", "thread_start", "DedupCore", "media_worker_start",
             "媒体线程池内存不足", "std::bad_alloc", {}, 0});
        SetFailure(L"媒体线程池内存不足。");
        return false;
    } catch (const std::exception& exception) {
        cancel_requested_.store(true);
        computeGate.WakeAll();
        controllerStop.store(true);
        controllerCondition.notify_all();
        if (controller.joinable()) controller.join();
        ApplicationErrorLogger::Write(
            {"error", "thread_start", "DedupCore", "media_worker_start",
             exception.what(), "std::exception", {}, 0});
        SetFailure(L"媒体线程池创建或执行失败。");
        return false;
    } catch (...) {
        cancel_requested_.store(true);
        computeGate.WakeAll();
        controllerStop.store(true);
        controllerCondition.notify_all();
        if (controller.joinable()) controller.join();
        ApplicationErrorLogger::Write(
            {"error", "thread_start", "DedupCore", "media_worker_start",
             "媒体线程池创建或执行失败", "unknown_exception", {}, 0});
        SetFailure(L"媒体线程池创建或执行失败。");
        return false;
    }
}

void ScanCoordinator::ProcessMediaTask(std::string task_key, std::string task_value) {
    if (cancel_requested_.load() || fatal_error_.load()) return;
    std::string decodeError;
    std::optional<FilePathRecord> record = CoreModelCodec::DeserializeFilePath(task_value, decodeError);
    if (!record.has_value() || !record->sha512.has_value()) {
        SetFailure(L"媒体任务记录损坏。");
        return;
    }
    bool corrupted = false;
    std::optional<ShaFileData> data = LoadShaData(store_, *record->sha512, corrupted);
    if (!data.has_value() || corrupted) {
        SetFailure(L"媒体内容记录缺失或损坏。");
        return;
    }
    const MediaKind kind = NativeFileDiscovery::ClassifyMedia(record->path);
    const std::string inputPath = WideToUtf8(record->path.wstring());
    std::filesystem::path contactSheet;
    std::string contactSheetUtf8;
    if (kind == MediaKind::Video) {
        const std::string shaHex = Sha512ToHex(*record->sha512);
        const wchar_t* extension = options_.thumbnails().format == ThumbnailFormat::Png ? L".png" : L".jpg";
        contactSheet = options_.thumbnails().root_directory / std::filesystem::path(shaHex.substr(0, 2)) /
                       std::filesystem::path(shaHex + WideToUtf8(extension));
        contactSheetUtf8 = WideToUtf8(contactSheet.wstring());
    }
    VideoScMediaOptions mediaOptions{};
    mediaOptions.structSize = sizeof(mediaOptions);
    mediaOptions.mediaKindHint = VIDEOSC_MEDIA_VIDEO;
    mediaOptions.contactSheetCellLongEdge = options_.thumbnails().video_cell_long_edge;
    mediaOptions.ffmpegThreadCount = options_.compute().ffmpeg_threads_per_task;
    mediaOptions.noProgressTimeoutMilliseconds = options_.io().no_progress_timeout_seconds * 1000U;
    mediaOptions.shouldCancel = ShouldCancelMedia;
    mediaOptions.cancelContext = &cancel_requested_;
    mediaOptions.contactSheetPath = contactSheetUtf8.empty() ? nullptr : contactSheetUtf8.c_str();
    VideoScMediaResult result{};

    VideoScImageFeatureOptionsV1 imageOptions{};
    imageOptions.structSize = sizeof(imageOptions);
    imageOptions.ffmpegThreadCount = options_.compute().ffmpeg_threads_per_task;
    imageOptions.noProgressTimeoutMilliseconds = options_.io().no_progress_timeout_seconds * 1000U;
    imageOptions.shouldCancel = ShouldCancelMedia;
    imageOptions.cancelContext = &cancel_requested_;
    VideoScImageFeatureResultV1 imageResult{};
    imageResult.structSize = sizeof(imageResult);

    std::uint32_t imageAnalysisAttempts = 0;
    bool succeeded = false;
    if (kind == MediaKind::Image) {
        ++imageAnalysisAttempts;
        succeeded = AnalyzeImagePerceptualFeaturesV1(inputPath.c_str(), &imageOptions, &imageResult) != 0;
        if (!succeeded &&
            ShouldRetryImageFeatureAnalysis(imageResult.statusCode,
                                            imageAnalysisAttempts,
                                            cancel_requested_.load(std::memory_order_relaxed))) {
            // 超时可能是瞬时磁盘或解码器阻塞；只重试一次，避免坏文件长期占用扫描线程。
            FreeVideoScImageFeatureResultV1(&imageResult);
            imageResult = {};
            imageResult.structSize = sizeof(imageResult);
            ++imageAnalysisAttempts;
            succeeded = AnalyzeImagePerceptualFeaturesV1(inputPath.c_str(), &imageOptions, &imageResult) != 0;
        }
    } else {
        succeeded = AnalyzeMediaFile(inputPath.c_str(), &mediaOptions, &result) != 0;
    }
    if (!succeeded && kind == MediaKind::Video && mediaOptions.contactSheetPath != nullptr &&
        !cancel_requested_.load()) {
        // 拼图只是展示产物；保存失败时不应阻止六帧 dHash，故无输出路径重试一次。
        FreeVideoScMediaResult(&result);
        result = {};
        mediaOptions.contactSheetPath = nullptr;
        succeeded = AnalyzeMediaFile(inputPath.c_str(), &mediaOptions, &result) != 0;
        if (succeeded) contactSheet.clear();
    }
    const bool imageFeatureValid = kind == MediaKind::Image && imageResult.hasPdqHash != 0 &&
                                   imageResult.hasZonedPHashes != 0 &&
                                   imageResult.perceptualAlgorithmVersion ==
                                       VIDEOSC_IMAGE_PERCEPTUAL_ALGORITHM_VERSION;
    const bool videoFeatureValid = kind == MediaKind::Video && result.hasVideoDHashes != 0 &&
                                   std::all_of(std::begin(result.videoDHashes),
                                               std::end(result.videoDHashes),
                                               [](const std::uint64_t hash) { return hash != 0; });
    const bool featureSucceeded = succeeded && (imageFeatureValid || videoFeatureValid);
    if (featureSucceeded) {
        data->media_kind = kind;
        if (kind == MediaKind::Image) {
            data->mime_type = imageResult.mimeType == nullptr ? "" : imageResult.mimeType;
            data->container_name = imageResult.containerName == nullptr ? "" : imageResult.containerName;
            data->width = imageResult.width;
            data->height = imageResult.height;
            data->video_codec = imageResult.imageCodec == nullptr ? "" : imageResult.imageCodec;
            data->pixel_format = imageResult.pixelFormat == nullptr ? "" : imageResult.pixelFormat;
            if (imageResult.hasImageDHash != 0) data->image_dhash = imageResult.imageDHash;
            PdqHash256 pdq{};
            std::copy(std::begin(imageResult.pdqHash), std::end(imageResult.pdqHash), pdq.begin());
            data->image_pdq_hash = pdq;
            data->image_pdq_quality = imageResult.pdqQuality;
            std::copy(std::begin(imageResult.zonedPHashes),
                      std::end(imageResult.zonedPHashes),
                      data->image_zoned_phashes.begin());
            data->has_image_zoned_phashes = true;
            data->image_perceptual_algorithm_version = imageResult.perceptualAlgorithmVersion;
            data->image_structural_algorithm_version = VIDEOSC_IMAGE_STRUCTURAL_ALGORITHM_VERSION;
        } else {
            data->mime_type = result.mimeType == nullptr ? "" : result.mimeType;
            data->container_name = result.containerName == nullptr ? "" : result.containerName;
            data->width = result.width;
            data->height = result.height;
            data->video_duration_ms = result.durationMilliseconds;
            data->video_frame_rate = result.frameRate;
            data->video_bitrate = result.bitrate;
            data->video_codec = result.videoCodec == nullptr ? "" : result.videoCodec;
            data->pixel_format = result.pixelFormat == nullptr ? "" : result.pixelFormat;
            std::copy(std::begin(result.videoDHashes), std::end(result.videoDHashes), data->video_dhashes.begin());
            data->has_video_dhashes = true;
            data->static_visual = result.staticVisual != 0;
            data->contact_sheet_path = contactSheet;
        }
        data->media_algorithm_version = options_.algorithm_version();
        DHashCandidateIndex index(store_,
                                  options_.algorithm_version(),
                                  {},
                                  options_.dhash_similarity().image_max_hamming_distance,
                                  options_.dhash_similarity().video_max_average_hamming_distance);
        const RocksStatus indexed = kind == MediaKind::Image && data->image_dhash.has_value() &&
                                            *data->image_dhash != 0
                                        ? index.AddImage(*data)
                                        : kind == MediaKind::Video ? index.AddVideo(*data)
                                                                   : RocksStatus{true, {}};
        if (!indexed.succeeded) SetFailure(L"dHash 候选索引写入失败。");
    } else if (!cancel_requested_.load()) {
        data->media_algorithm_version = "media-error-v1";
        ExecutionFailureRecord failure;
        failure.task_id = record->scan_id;
        failure.path_id = record->path_id;
        failure.task = "scan";
        failure.stage = "media";
        failure.operation = kind == MediaKind::Image ? "image_perceptual_features" : "video_six_frame_dhash";
        failure.path = record->path;
        failure.storage_target_key = record->storage_target_key;
        failure.media_kind = MediaKindName(kind);
        failure.status = "media_analysis_failed";
        failure.status_code = static_cast<std::uint32_t>(
            (std::max)(0, kind == MediaKind::Image ? imageResult.statusCode : result.statusCode));
        failure.native_error = kind == MediaKind::Image ? imageResult.nativeError : result.nativeError;
        const char* errorMessage = kind == MediaKind::Image ? imageResult.errorMessage : result.errorMessage;
        failure.detail = errorMessage == nullptr ? "missing_media_feature" : errorMessage;
        std::string logError;
        if (!execution_logger_.WriteFailure(failure, logError)) {
            if (kind == MediaKind::Image) FreeVideoScImageFeatureResultV1(&imageResult);
            else FreeVideoScMediaResult(&result);
            SetFailure(L"媒体失败日志写入失败。");
            return;
        }
        std::lock_guard<std::mutex> lock(progress_mutex_);
        ++progress_.failed_files;
        if (kind == MediaKind::Image) ++progress_.image_feature_failed_contents;
    }
    if (kind == MediaKind::Image) FreeVideoScImageFeatureResultV1(&imageResult);
    else FreeVideoScMediaResult(&result);
    if (fatal_error_.load() || cancel_requested_.load()) return;

    SyncOperation operation;
    operation.kind = SyncOperationKind::UpsertShaFileData;
    operation.sha_file_data = *data;
    if (!StageOperation(
            {{RocksColumnFamily::ShaFileData,
              Sha512ToHex(data->sha512),
              CoreModelCodec::SerializeShaFileData(*data)},
             {RocksColumnFamily::Checkpoints, std::move(task_key), std::nullopt}},
            std::move(operation))) {
        SetFailure(L"媒体结果与 MySQL 待同步消息写入失败。");
        return;
    }
    std::lock_guard<std::mutex> lock(progress_mutex_);
    ++progress_.media_processed_files;
}

bool ScanCoordinator::StageOperation(const std::vector<RocksMutation>& mutations,
                                     SyncOperation operation) {
    bool inserted = false;
    const RocksStatus staged = sync_queue_.Stage(
        active_scan_id_, mutations, std::move(operation), inserted);
    if (!staged.succeeded) return false;
    (void)inserted;
    if (!RefreshSyncProgress()) return false;
    return PublishAvailableBatches(false);
}

bool ScanCoordinator::PublishAvailableBatches(const bool final_flush) {
    std::lock_guard<std::mutex> lock(publish_mutex_);
    const std::uint64_t threshold = (std::max<std::uint64_t>)(
        1, options_.database().sync_batch_size);
    while (!cancel_requested_.load() || final_flush) {
        std::uint64_t staged = 0;
        const RocksStatus counted = sync_queue_.StagedCount(active_scan_id_, staged);
        if (!counted.succeeded) return false;
        if (staged == 0 || (!final_flush && staged < threshold)) break;
        const std::size_t maximum = static_cast<std::size_t>(
            (std::min<std::uint64_t>)(staged, threshold));
        std::size_t published = 0;
        const RocksStatus status = sync_queue_.PublishStaged(
            active_scan_id_, maximum, published);
        if (!status.succeeded || published == 0) return false;
        if (sync_wake_) {
            try {
                sync_wake_();
            } catch (...) {
            }
        }
    }
    return RefreshSyncProgress();
}

bool ScanCoordinator::RefreshSyncProgress() {
    std::uint64_t staged = 0;
    std::uint64_t pending = 0;
    RocksStatus status = sync_queue_.StagedCount(active_scan_id_, staged);
    if (!status.succeeded) return false;
    status = sync_queue_.PendingCount(active_scan_id_, pending);
    if (!status.succeeded) return false;
    std::lock_guard<std::mutex> lock(progress_mutex_);
    progress_.mysql_staged_operations = staged;
    progress_.mysql_pending_operations = pending;
    return true;
}

void ScanCoordinator::SaveCheckpoint(const ScanPhase phase) {
    ScanCheckpoint checkpoint;
    {
        std::lock_guard<std::mutex> lock(progress_mutex_);
        checkpoint.scan_id = progress_.scan_id;
        checkpoint.phase = phase;
        checkpoint.discovered_files = progress_.discovered_files;
        checkpoint.completed_files = progress_.hashed_files;
        checkpoint.failed_files = progress_.failed_files;
        checkpoint.media_completed_files = progress_.media_processed_files;
        checkpoint.image_feature_failed_contents = progress_.image_feature_failed_contents;
    }
    checkpoint.scan_options_json = ScanOptionsCodec::Serialize(options_);
    checkpoint.updated_utc_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count();
    const RocksStatus saved = checkpoint_store_.Save(checkpoint);
    if (!saved.succeeded) SetFailure(L"扫描检查点写入失败。");
}

void ScanCoordinator::SetFailure(const std::wstring& message) {
    std::wstring finalMessage = message;
    ExecutionFailureRecord failure;
    failure.task_id = active_scan_id_;
    failure.task = "scan";
    failure.stage = "coordinator";
    failure.operation = "fatal_error";
    failure.status = "failed";
    failure.detail = WideToUtf8(message);
    std::string logError;
    if (!execution_logger_.WriteFailure(failure, logError)) {
        finalMessage += L"（执行失败日志写入失败：" + Utf8ToWide(logError) + L"）";
    }
    fatal_error_.store(true);
    cancel_requested_.store(true);
    std::lock_guard<std::mutex> lock(progress_mutex_);
    progress_.phase = ScanPhase::Failed;
    progress_.latest_error = finalMessage;
    const auto now = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < progress_.discovery_roots.size(); ++index) {
        auto& root = progress_.discovery_roots[index];
        if (!IsTerminalDiscoveryPhase(root.phase)) root.phase = DiscoveryRootPhase::Failed;
        if (index < discovery_started_at_.size()) {
            root.elapsed_milliseconds = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - discovery_started_at_[index]).count());
        }
    }
}

}  // namespace videosc::dedup
