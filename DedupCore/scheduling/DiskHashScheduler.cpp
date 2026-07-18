#include "DiskHashScheduler.h"
#include "../logging/ApplicationErrorLogger.h"

#include <Windows.h>
#include <winioctl.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace videosc::dedup {
namespace {

/** @brief C++17 兼容的全局计算并发闸门。 */
class ConcurrencyGate final {
public:
    /**
     * @brief 创建固定容量闸门。
     * @param capacity 同时持有令牌的最大线程数。
     */
    ConcurrencyGate(const std::uint32_t initial_capacity, const std::uint32_t maximum_capacity)
        : capacity_(initial_capacity), maximum_capacity_(maximum_capacity) {}

    /**
     * @brief 等待令牌，取消时立即返回。
     * @param cancelled 调度器取消状态。
     * @return 获得令牌返回 true，取消返回 false。
     */
    bool Acquire(const std::atomic_bool& cancelled) {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [&] {
            return active_ < capacity_ || cancelled.load(std::memory_order_relaxed);
        });
        if (cancelled.load(std::memory_order_relaxed)) {
            return false;
        }
        ++active_;
        return true;
    }

    /** @brief 归还一个令牌并唤醒等待线程。 */
    void Release() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (active_ > 0) {
                --active_;
            }
        }
        condition_.notify_one();
    }

    /** @brief 取消时唤醒全部等待线程重新检查状态。 */
    void WakeAll() {
        condition_.notify_all();
    }

    /** @brief 在不超过硬上限的前提下增加一个令牌。 */
    bool IncreaseOne() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (capacity_ >= maximum_capacity_) return false;
        ++capacity_;
        condition_.notify_all();
        return true;
    }

    /** @brief 采样不可用时恢复到配置的固定并发上限。 */
    void RestoreMaximum() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            capacity_ = maximum_capacity_;
        }
        condition_.notify_all();
    }

    /** @return 当前允许并发。 */
    std::uint32_t capacity() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return capacity_;
    }

    /** @return 当前持有令牌数。 */
    std::uint32_t active() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return active_;
    }

private:
    std::uint32_t capacity_ = 1;
    std::uint32_t maximum_capacity_ = 1;
    std::uint32_t active_ = 0;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
};

/** @brief 基于 GetSystemTimes 差值采样系统总 CPU。 */
class CpuLoadSampler final {
public:
    /** @return 第一次调用只建立基线并返回空；之后返回 0 到 100。 */
    std::optional<double> Sample() {
        FILETIME idle{};
        FILETIME kernel{};
        FILETIME user{};
        if (!GetSystemTimes(&idle, &kernel, &user)) return std::nullopt;
        const auto value = [](const FILETIME& time) {
            return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) |
                   static_cast<std::uint64_t>(time.dwLowDateTime);
        };
        const std::uint64_t idleValue = value(idle);
        const std::uint64_t kernelValue = value(kernel);
        const std::uint64_t userValue = value(user);
        if (!initialized_) {
            idle_ = idleValue;
            kernel_ = kernelValue;
            user_ = userValue;
            initialized_ = true;
            return std::nullopt;
        }
        const std::uint64_t idleDelta = idleValue - idle_;
        const std::uint64_t kernelDelta = kernelValue - kernel_;
        const std::uint64_t userDelta = userValue - user_;
        idle_ = idleValue;
        kernel_ = kernelValue;
        user_ = userValue;
        const std::uint64_t total = kernelDelta + userDelta;
        if (total == 0 || idleDelta > total) return std::nullopt;
        return static_cast<double>(total - idleDelta) * 100.0 / static_cast<double>(total);
    }

private:
    bool initialized_ = false;
    std::uint64_t idle_ = 0;
    std::uint64_t kernel_ = 0;
    std::uint64_t user_ = 0;
};

/** @brief 单次物理磁盘占用采样结果。 */
struct DiskUtilizationResult {
    bool available = false;
    bool has_sample = false;
    double percent = 0.0;
};

/** @brief 使用 IOCTL_DISK_PERFORMANCE 对物理磁盘读取通道进行一秒差值采样。 */
class DiskUtilizationSampler final {
public:
    ~DiskUtilizationSampler() {
        for (auto& item : disks_) {
            if (item.second.handle != INVALID_HANDLE_VALUE) CloseHandle(item.second.handle);
        }
    }

    /** @brief 采样 PhysicalDriveN 或 PhysicalSet:n,m；复合卷采用成员盘最高占用。 */
    DiskUtilizationResult Sample(const std::wstring& storage_target_key) {
        const std::vector<std::uint32_t> numbers = ParseDiskNumbers(storage_target_key);
        if (numbers.empty()) return {};

        DiskUtilizationResult combined;
        combined.available = true;
        combined.has_sample = true;
        for (const std::uint32_t number : numbers) {
            const DiskUtilizationResult current = SampleDisk(number);
            if (!current.available) return {};
            if (!current.has_sample) combined.has_sample = false;
            if (current.has_sample) combined.percent = (std::max)(combined.percent, current.percent);
        }
        return combined;
    }

private:
    struct DiskState {
        HANDLE handle = INVALID_HANDLE_VALUE;
        bool initialized = false;
        std::uint64_t query_time = 0;
        std::uint64_t idle_time = 0;
    };

    static std::vector<std::uint32_t> ParseDiskNumbers(const std::wstring& key) {
        constexpr wchar_t singlePrefix[] = L"PhysicalDrive";
        constexpr wchar_t setPrefix[] = L"PhysicalSet:";
        std::wstring values;
        if (key.rfind(singlePrefix, 0) == 0) {
            values = key.substr((sizeof(singlePrefix) / sizeof(wchar_t)) - 1);
        } else if (key.rfind(setPrefix, 0) == 0) {
            values = key.substr((sizeof(setPrefix) / sizeof(wchar_t)) - 1);
        } else {
            return {};
        }

        std::vector<std::uint32_t> result;
        std::size_t offset = 0;
        while (offset < values.size()) {
            wchar_t* end = nullptr;
            const wchar_t* start = values.c_str() + offset;
            const unsigned long value = std::wcstoul(start, &end, 10);
            if (end == start || value > (std::numeric_limits<std::uint32_t>::max)()) return {};
            result.push_back(static_cast<std::uint32_t>(value));
            offset = static_cast<std::size_t>(end - values.c_str());
            if (offset == values.size()) break;
            if (values[offset] != L',') return {};
            ++offset;
        }
        return result;
    }

    DiskUtilizationResult SampleDisk(const std::uint32_t number) {
        DiskState& state = disks_[number];
        if (state.handle == INVALID_HANDLE_VALUE) {
            const std::wstring path = L"\\\\.\\PhysicalDrive" + std::to_wstring(number);
            state.handle = CreateFileW(path.c_str(), 0,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (state.handle == INVALID_HANDLE_VALUE) return {};
        }

        DISK_PERFORMANCE performance{};
        DWORD returned = 0;
        if (!DeviceIoControl(state.handle, IOCTL_DISK_PERFORMANCE, nullptr, 0,
                             &performance, sizeof(performance), &returned, nullptr) ||
            returned < sizeof(performance)) {
            return {};
        }

        const std::uint64_t query = static_cast<std::uint64_t>(performance.QueryTime.QuadPart);
        const std::uint64_t idle = static_cast<std::uint64_t>(performance.IdleTime.QuadPart);
        if (!state.initialized) {
            state.initialized = true;
            state.query_time = query;
            state.idle_time = idle;
            return {true, false, 0.0};
        }

        const std::uint64_t queryDelta = query - state.query_time;
        const std::uint64_t idleDelta = idle - state.idle_time;
        state.query_time = query;
        state.idle_time = idle;
        if (queryDelta == 0 || idleDelta > queryDelta) return {true, false, 0.0};
        const double busy = 100.0 - static_cast<double>(idleDelta) * 100.0 /
                                        static_cast<double>(queryDelta);
        return {true, true, (std::max)(0.0, (std::min)(100.0, busy))};
    }

    std::unordered_map<std::uint32_t, DiskState> disks_;
};

/**
 * @brief 在队列前部有界窗口中选择下一 HDD 任务。
 * @param queue 待处理任务。
 * @param window_size 最大候选数量。
 * @param current_position 当前磁头近似物理位置。
 * @param ascending 当前电梯方向，会在触底或触顶时翻转。
 * @return 队列索引；没有物理位置时退化为队首。
 */
std::size_t SelectHddJob(const std::deque<FileHashJob>& queue,
                         const std::uint32_t window_size,
                         std::uint64_t& current_position,
                         bool& ascending) {
    const std::size_t count = (std::min<std::size_t>)(queue.size(), window_size);
    std::optional<std::size_t> selected;
    std::uint64_t selected_position = 0;

    if (ascending) {
        for (std::size_t index = 0; index < count; ++index) {
            if (!queue[index].physical_start_byte.has_value()) continue;
            const std::uint64_t position = *queue[index].physical_start_byte;
            if (position >= current_position &&
                (!selected.has_value() || position < selected_position)) {
                selected = index;
                selected_position = position;
            }
        }
        if (!selected.has_value()) {
            ascending = false;
            for (std::size_t index = 0; index < count; ++index) {
                if (!queue[index].physical_start_byte.has_value()) continue;
                const std::uint64_t position = *queue[index].physical_start_byte;
                if (!selected.has_value() || position > selected_position) {
                    selected = index;
                    selected_position = position;
                }
            }
        }
    } else {
        for (std::size_t index = 0; index < count; ++index) {
            if (!queue[index].physical_start_byte.has_value()) continue;
            const std::uint64_t position = *queue[index].physical_start_byte;
            if (position <= current_position &&
                (!selected.has_value() || position > selected_position)) {
                selected = index;
                selected_position = position;
            }
        }
        if (!selected.has_value()) {
            ascending = true;
            for (std::size_t index = 0; index < count; ++index) {
                if (!queue[index].physical_start_byte.has_value()) continue;
                const std::uint64_t position = *queue[index].physical_start_byte;
                if (!selected.has_value() || position < selected_position) {
                    selected = index;
                    selected_position = position;
                }
            }
        }
    }

    if (selected.has_value()) {
        current_position = selected_position;
        return *selected;
    }
    return 0;
}

}  // namespace

/** @brief DiskHashScheduler 的线程、队列和同步实现。 */
class DiskHashScheduler::Impl final {
public:
    /** @brief 单个物理存储目标的有界队列和工作线程。 */
    struct Channel {
        Channel(DiskChannelOptions value, const bool adaptive_reads)
            : options(std::move(value)),
              read_permit(adaptive_reads ? 1U : options.read_threads, options.read_threads) {}

        DiskChannelOptions options;
        ConcurrencyGate read_permit;
        mutable std::mutex mutex;
        std::condition_variable has_work;
        std::condition_variable has_space;
        std::deque<FileHashJob> queue;
        std::vector<std::thread> workers;
        bool accepting = true;
        bool stop = false;
        bool ascending = true;
        std::uint64_t current_position = 0;
        std::uint32_t active_threads = 0;
        std::uint64_t completed_files = 0;
        std::uint64_t failed_files = 0;
        std::uint64_t timeout_files = 0;
        std::uint64_t bytes_read = 0;
        double disk_read_utilization_percent = 0.0;
        bool disk_utilization_available = false;
    };

    /**
     * @brief 保存共享哈希器和全局并发预算。
     * @param hasher_value 文件哈希实现。
     * @param maximum_concurrent_computations 全局计算并发。
     */
    Impl(std::shared_ptr<IFileHasher> hasher_value,
         const std::uint32_t maximum_concurrent_computations,
         const std::uint32_t maximum_concurrent_file_reads,
         const bool adaptive_computations_value,
         const std::uint32_t cpu_target_percent_value,
         const bool adaptive_file_reads_value,
         const std::uint32_t disk_read_target_percent_value)
        : hasher(std::move(hasher_value)),
          compute_gate(adaptive_computations_value ? 1U : maximum_concurrent_computations,
                       maximum_concurrent_computations),
          read_gate(maximum_concurrent_file_reads, maximum_concurrent_file_reads),
          maximum_compute_threads(maximum_concurrent_computations),
          adaptive_computations(adaptive_computations_value),
          cpu_target_percent(cpu_target_percent_value),
          adaptive_file_reads(adaptive_file_reads_value),
          disk_read_target_percent(disk_read_target_percent_value) {}

    /** @brief 启动每个物理盘的工作线程。 */
    void Start(std::vector<DiskChannelOptions> channel_options, CompletionCallback callback) {
        if (started) {
            throw std::logic_error("DiskHashScheduler has already started");
        }
        std::unordered_set<std::wstring> keys;
        for (DiskChannelOptions& options : channel_options) {
            if (options.storage_target_key.empty() || options.read_threads == 0 ||
                options.queue_capacity == 0 || options.hdd_sort_window == 0 ||
                !keys.insert(options.storage_target_key).second) {
                throw std::invalid_argument("Invalid or duplicate disk channel options");
            }
            // 先复制映射键，再移动配置；函数实参求值顺序不能用来保证移动发生在取键之后。
            std::wstring channel_key = options.storage_target_key;
            channels.emplace(std::move(channel_key),
                             std::make_unique<Channel>(std::move(options), adaptive_file_reads));
        }
        if (channels.empty()) {
            throw std::invalid_argument("At least one disk channel is required");
        }

        completion = std::move(callback);
        started = true;
        try {
            if (adaptive_computations || adaptive_file_reads) {
                controller_thread = std::thread([this] { AdaptiveControllerLoop(); });
            }
            for (auto& item : channels) {
                Channel* channel = item.second.get();
                for (std::uint32_t index = 0; index < channel->options.read_threads; ++index) {
                    channel->workers.emplace_back([this, channel] { WorkerLoop(*channel); });
                }
            }
        } catch (...) {
            RequestCancel();
            JoinWorkers();
            throw;
        }
    }

    /** @brief 队列满时等待指定时间并提交任务。 */
    bool Submit(FileHashJob job, const std::uint32_t timeout_milliseconds) {
        if (!started || joined || cancelled.load(std::memory_order_relaxed)) return false;
        const auto iterator = channels.find(job.storage_target_key);
        if (iterator == channels.end()) return false;
        Channel& channel = *iterator->second;
        std::unique_lock<std::mutex> lock(channel.mutex);
        const auto has_capacity = [&] {
            return channel.queue.size() < channel.options.queue_capacity || !channel.accepting || channel.stop ||
                   cancelled.load(std::memory_order_relaxed);
        };
        if (timeout_milliseconds == 0) {
            if (!has_capacity()) return false;
        } else if (!channel.has_space.wait_for(
                       lock, std::chrono::milliseconds(timeout_milliseconds), has_capacity)) {
            return false;
        }
        if (!channel.accepting || channel.stop || cancelled.load(std::memory_order_relaxed) ||
            channel.queue.size() >= channel.options.queue_capacity) {
            return false;
        }
        channel.queue.push_back(std::move(job));
        outstanding.fetch_add(1, std::memory_order_relaxed);
        lock.unlock();
        channel.has_work.notify_one();
        return true;
    }

    /** @brief 关闭全部通道的提交入口。 */
    void CloseSubmissions() {
        if (!started) return;
        for (auto& item : channels) {
            Channel& channel = *item.second;
            {
                std::lock_guard<std::mutex> lock(channel.mutex);
                channel.accepting = false;
            }
            channel.has_work.notify_all();
            channel.has_space.notify_all();
            channel.read_permit.WakeAll();
        }
    }

    /** @brief 等待 outstanding 计数归零。 */
    bool WaitUntilIdle(const std::uint32_t timeout_milliseconds) {
        std::unique_lock<std::mutex> lock(idle_mutex);
        return idle_condition.wait_for(lock, std::chrono::milliseconds(timeout_milliseconds), [&] {
            return outstanding.load(std::memory_order_relaxed) == 0;
        });
    }

    /** @brief 非阻塞取消并丢弃尚未开始任务；Pending 路径由断点恢复重新发现。 */
    void RequestCancel() {
        if (!started || joined) return;
        bool expected = false;
        if (!cancelled.compare_exchange_strong(expected, true, std::memory_order_relaxed)) return;
        std::uint64_t cancelledJobs = 0;
        for (auto& item : channels) {
            Channel& channel = *item.second;
            {
                std::lock_guard<std::mutex> lock(channel.mutex);
                channel.accepting = false;
                channel.stop = true;
                while (!channel.queue.empty()) {
                    channel.queue.pop_front();
                    ++cancelledJobs;
                }
            }
            channel.has_work.notify_all();
            channel.has_space.notify_all();
        }
        compute_gate.WakeAll();
        read_gate.WakeAll();
        controller_condition.notify_all();
        for (std::uint64_t index = 0; index < cancelledJobs; ++index) {
            CompleteOutstanding();
        }
    }

    /** @brief 请求取消并回收所有工作线程。 */
    void CancelAndJoin() {
        if (!started || joined) return;
        RequestCancel();
        JoinWorkers();
    }

    /** @brief 关闭提交并等待所有队列自然清空。 */
    void Join() {
        if (!started || joined) return;
        CloseSubmissions();
        JoinWorkers();
    }

    /** @brief 复制所有通道计数和队列长度。 */
    std::vector<DiskChannelSnapshot> GetSnapshots() const {
        std::vector<DiskChannelSnapshot> snapshots;
        snapshots.reserve(channels.size());
        for (const auto& item : channels) {
            const Channel& channel = *item.second;
            std::lock_guard<std::mutex> lock(channel.mutex);
            DiskChannelSnapshot snapshot;
            snapshot.storage_target_key = channel.options.storage_target_key;
            snapshot.media_type = channel.options.media_type;
            snapshot.configured_threads = channel.options.read_threads;
            snapshot.allowed_threads = channel.read_permit.capacity();
            snapshot.active_threads = channel.read_permit.active();
            snapshot.queued_files = channel.queue.size();
            snapshot.completed_files = channel.completed_files;
            snapshot.failed_files = channel.failed_files;
            snapshot.timeout_files = channel.timeout_files;
            snapshot.bytes_read = channel.bytes_read;
            snapshot.disk_read_utilization_percent = channel.disk_read_utilization_percent;
            snapshot.disk_utilization_available = channel.disk_utilization_available;
            snapshots.push_back(std::move(snapshot));
        }
        return snapshots;
    }

    /** @brief 复制全局读取、计算和 CPU 状态。 */
    SchedulerComputeSnapshot GetComputeSnapshot() const {
        SchedulerComputeSnapshot snapshot;
        snapshot.active_file_reads = read_gate.active();
        snapshot.active_compute_threads = compute_gate.active();
        snapshot.allowed_compute_threads = compute_gate.capacity();
        snapshot.maximum_compute_threads = maximum_compute_threads;
        snapshot.system_cpu_percent = system_cpu_percent.load(std::memory_order_relaxed);
        return snapshot;
    }

    /** @brief 每秒采样系统 CPU，并在有积压且低于目标时只增加一个并发。 */
    void AdaptiveControllerLoop() {
        try {
            CpuLoadSampler cpuSampler;
            DiskUtilizationSampler diskSampler;
            while (!cancelled.load(std::memory_order_relaxed) &&
                   !controller_stop.load(std::memory_order_relaxed)) {
                {
                    std::unique_lock<std::mutex> lock(controller_mutex);
                    controller_condition.wait_for(lock, std::chrono::seconds(1), [&] {
                        return cancelled.load(std::memory_order_relaxed) ||
                               controller_stop.load(std::memory_order_relaxed);
                    });
                }
                if (cancelled.load(std::memory_order_relaxed) ||
                    controller_stop.load(std::memory_order_relaxed)) {
                    break;
                }
                if (adaptive_computations) {
                    const std::optional<double> load = cpuSampler.Sample();
                    if (load.has_value()) {
                        system_cpu_percent.store(*load, std::memory_order_relaxed);
                        if (outstanding.load(std::memory_order_relaxed) > compute_gate.active() &&
                            *load < static_cast<double>(cpu_target_percent)) {
                            compute_gate.IncreaseOne();
                        }
                    }
                }

                if (adaptive_file_reads) {
                    for (auto& item : channels) {
                        Channel& channel = *item.second;
                        const DiskUtilizationResult sample =
                            diskSampler.Sample(channel.options.storage_target_key);
                        std::uint64_t pending = 0;
                        {
                            std::lock_guard<std::mutex> lock(channel.mutex);
                            channel.disk_utilization_available = sample.available && sample.has_sample;
                            if (sample.has_sample) {
                                channel.disk_read_utilization_percent = sample.percent;
                            }
                            pending = channel.queue.size() + channel.active_threads;
                        }
                        if (!sample.available) {
                            // 驱动不支持性能计数时保留旧的固定线程行为，避免永久停留在单线程。
                            channel.read_permit.RestoreMaximum();
                        } else if (sample.has_sample &&
                                   pending > channel.read_permit.active() &&
                                   sample.percent < static_cast<double>(disk_read_target_percent)) {
                            channel.read_permit.IncreaseOne();
                        }
                    }
                }
            }
        } catch (const std::exception& exception) {
            ApplicationErrorLogger::Write(
                {"error", "thread_exception", "DedupCore", "hash_cpu_controller",
                 exception.what(), "std::exception", {}, 0});
        } catch (...) {
            ApplicationErrorLogger::Write(
                {"error", "thread_exception", "DedupCore", "hash_cpu_controller",
                 "CPU 自适应控制线程发生未知异常", "unknown_exception", {}, 0});
        }
    }

    /** @brief 单工作线程持续从所属物理盘队列取任务。 */
    void WorkerLoop(Channel& channel) {
        FileHashJob job;
        bool ownsOutstandingJob = false;
        try {
        while (PopJob(channel, job)) {
            ownsOutstandingJob = true;
            FileHashResult result;
            const bool acquired = compute_gate.Acquire(cancelled);
            if (!acquired) {
                result.status = FileHashStatus::Cancelled;
            } else {
                const bool acquiredDiskRead = channel.read_permit.Acquire(cancelled);
                if (!acquiredDiskRead) {
                    result.status = FileHashStatus::Cancelled;
                } else {
                    const bool acquiredRead = read_gate.Acquire(cancelled);
                    if (!acquiredRead) {
                        result.status = FileHashStatus::Cancelled;
                    } else {
                        try {
                            result = hasher->Hash(job.path, cancelled);
                        } catch (const std::bad_alloc&) {
                            result.status = FileHashStatus::UnexpectedFailure;
                            ApplicationErrorLogger::Write(
                                {"error", "thread_exception", "DedupCore", "disk_hash_worker",
                                 "哈希工作线程内存不足", "std::bad_alloc", {}, 0});
                        } catch (const std::exception& exception) {
                            result.status = FileHashStatus::UnexpectedFailure;
                            ApplicationErrorLogger::Write(
                                {"error", "thread_exception", "DedupCore", "disk_hash_worker",
                                 exception.what(), "std::exception", {}, 0});
                        } catch (...) {
                            result.status = FileHashStatus::UnexpectedFailure;
                            ApplicationErrorLogger::Write(
                                {"error", "thread_exception", "DedupCore", "disk_hash_worker",
                                 "哈希工作线程发生未知异常", "unknown_exception", {}, 0});
                        }
                        read_gate.Release();
                    }
                    channel.read_permit.Release();
                }
                compute_gate.Release();
            }

            {
                std::lock_guard<std::mutex> lock(channel.mutex);
                if (channel.active_threads > 0) --channel.active_threads;
                ++channel.completed_files;
                channel.bytes_read += result.bytes_read;
                if (result.status != FileHashStatus::Succeeded) ++channel.failed_files;
                if (result.status == FileHashStatus::ReadTimeout) ++channel.timeout_files;
            }
            InvokeCompletion({std::move(job), std::move(result)});
            CompleteOutstanding();
            ownsOutstandingJob = false;
        }
        } catch (const std::exception& exception) {
            if (ownsOutstandingJob) CompleteOutstanding();
            ApplicationErrorLogger::Write(
                {"error", "thread_exception", "DedupCore", "disk_hash_worker_boundary",
                 exception.what(), "std::exception", {}, 0});
            try { RequestCancel(); } catch (...) {}
        } catch (...) {
            if (ownsOutstandingJob) CompleteOutstanding();
            ApplicationErrorLogger::Write(
                {"error", "thread_exception", "DedupCore", "disk_hash_worker_boundary",
                 "磁盘哈希工作线程发生未知异常", "unknown_exception", {}, 0});
            try { RequestCancel(); } catch (...) {}
        }
    }

    /** @brief 从队列取出一个任务；HDD 使用有界电梯选择。 */
    bool PopJob(Channel& channel, FileHashJob& job) {
        std::unique_lock<std::mutex> lock(channel.mutex);
        channel.has_work.wait(lock, [&] {
            return channel.stop || !channel.queue.empty() || !channel.accepting;
        });
        if (channel.stop || (channel.queue.empty() && !channel.accepting)) return false;
        if (channel.queue.empty()) return false;

        std::size_t selected = 0;
        if (channel.options.media_type == StorageMediaType::Hdd &&
            channel.options.hdd_extent_optimization) {
            selected = SelectHddJob(channel.queue,
                                    channel.options.hdd_sort_window,
                                    channel.current_position,
                                    channel.ascending);
        }
        job = std::move(channel.queue[selected]);
        channel.queue.erase(channel.queue.begin() + static_cast<std::ptrdiff_t>(selected));
        ++channel.active_threads;
        lock.unlock();
        channel.has_space.notify_one();
        return true;
    }

    /** @brief 串行调用完成回调，避免 RocksDB 写入适配器被并发重入。 */
    void InvokeCompletion(FileHashOutcome outcome) {
        if (!completion) return;
        std::lock_guard<std::mutex> lock(completion_mutex);
        try {
            completion(std::move(outcome));
        } catch (const std::exception& exception) {
            ApplicationErrorLogger::Write(
                {"error", "callback_exception", "DedupCore", "hash_completion",
                 exception.what(), "std::exception", {}, 0});
            RequestCancel();
        } catch (...) {
            ApplicationErrorLogger::Write(
                {"error", "callback_exception", "DedupCore", "hash_completion",
                 "哈希完成回调发生未知异常", "unknown_exception", {}, 0});
            RequestCancel();
        }
    }

    /** @brief 递减未完成任务并在归零时唤醒等待者。 */
    void CompleteOutstanding() {
        const std::uint64_t previous = outstanding.fetch_sub(1, std::memory_order_relaxed);
        if (previous <= 1) {
            idle_condition.notify_all();
        }
    }

    /** @brief 等待所有通道线程退出并标记调度器已回收。 */
    void JoinWorkers() {
        for (auto& item : channels) {
            for (std::thread& worker : item.second->workers) {
                if (worker.joinable()) worker.join();
            }
        }
        controller_stop.store(true, std::memory_order_relaxed);
        controller_condition.notify_all();
        if (controller_thread.joinable()) controller_thread.join();
        joined = true;
        idle_condition.notify_all();
    }

    std::shared_ptr<IFileHasher> hasher;
    ConcurrencyGate compute_gate;
    ConcurrencyGate read_gate;
    std::uint32_t maximum_compute_threads = 1;
    bool adaptive_computations = false;
    std::uint32_t cpu_target_percent = 90;
    bool adaptive_file_reads = false;
    std::uint32_t disk_read_target_percent = 90;
    std::atomic<double> system_cpu_percent{0.0};
    std::atomic_bool controller_stop{false};
    std::thread controller_thread;
    std::mutex controller_mutex;
    std::condition_variable controller_condition;
    CompletionCallback completion;
    std::unordered_map<std::wstring, std::unique_ptr<Channel>> channels;
    std::atomic_bool cancelled{false};
    std::atomic<std::uint64_t> outstanding{0};
    std::mutex idle_mutex;
    std::condition_variable idle_condition;
    std::mutex completion_mutex;
    bool started = false;
    bool joined = false;
};

DiskHashScheduler::DiskHashScheduler(std::shared_ptr<IFileHasher> hasher,
                                     const std::uint32_t maximum_concurrent_computations,
                                     const std::uint32_t maximum_concurrent_file_reads,
                                     const bool adaptive_computations,
                                     const std::uint32_t cpu_target_percent,
                                     const bool adaptive_file_reads,
                                     const std::uint32_t disk_read_target_percent) {
    if (!hasher || maximum_concurrent_computations == 0 || maximum_concurrent_file_reads == 0 ||
        cpu_target_percent == 0 || cpu_target_percent > 100 || disk_read_target_percent < 10 ||
        disk_read_target_percent > 100) {
        throw std::invalid_argument("Hasher, read concurrency and computation limits are required");
    }
    impl_ = std::make_unique<Impl>(std::move(hasher),
                                   maximum_concurrent_computations,
                                   maximum_concurrent_file_reads,
                                   adaptive_computations,
                                   cpu_target_percent,
                                   adaptive_file_reads,
                                   disk_read_target_percent);
}

DiskHashScheduler::~DiskHashScheduler() {
    impl_->CancelAndJoin();
}

void DiskHashScheduler::Start(std::vector<DiskChannelOptions> channels,
                              CompletionCallback completion) {
    impl_->Start(std::move(channels), std::move(completion));
}

bool DiskHashScheduler::Submit(FileHashJob job, const std::uint32_t timeout_milliseconds) {
    return impl_->Submit(std::move(job), timeout_milliseconds);
}

void DiskHashScheduler::CloseSubmissions() {
    impl_->CloseSubmissions();
}

bool DiskHashScheduler::WaitUntilIdle(const std::uint32_t timeout_milliseconds) {
    return impl_->WaitUntilIdle(timeout_milliseconds);
}

void DiskHashScheduler::RequestCancel() {
    impl_->RequestCancel();
}

void DiskHashScheduler::CancelAndJoin() {
    impl_->CancelAndJoin();
}

void DiskHashScheduler::Join() {
    impl_->Join();
}

std::vector<DiskChannelSnapshot> DiskHashScheduler::GetSnapshots() const {
    return impl_->GetSnapshots();
}

SchedulerComputeSnapshot DiskHashScheduler::GetComputeSnapshot() const {
    return impl_->GetComputeSnapshot();
}

}  // namespace videosc::dedup
