#pragma once

#include "MySqlClient.h"
#include "SyncOperation.h"
#include "../logging/ExecutionLogger.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace videosc::dedup {

/** @brief 后台同步服务的线程安全状态快照。 */
struct MySqlSyncSnapshot {
    bool running = false;
    bool connected = false;
    std::uint64_t synchronized_operations = 0;
    std::uint64_t last_batch_size = 0;
    std::int64_t last_success_utc_ms = 0;
    unsigned int last_native_error = 0;
    std::string last_error;
};

/** @brief 将一批持久化同步消息转换成批量 SQL 并在一个事务内提交。 */
class MySqlSyncExecutor final {
public:
    explicit MySqlSyncExecutor(MySqlClient& client);

    /** @brief 应用一个有序批次；对同一路径的混合写删退化为逐条语句以保持顺序。 */
    MySqlStatus Apply(const std::vector<SyncOperation>& operations);

private:
    MySqlClient& client_;
};

/**
 * @brief RocksDB 到 MySQL 的可停止后台补同步服务。
 *
 * 最早消息未到重试时间时不会越过它处理后续消息；失败消息保留在 RocksDB 并指数退避。
 */
class MySqlSyncService final {
public:
    MySqlSyncService(MySqlSyncQueue& queue,
                     MySqlClient& client,
                     std::uint32_t batch_size,
                     std::uint32_t base_retry_seconds,
                     LoggingConfig logging);
    ~MySqlSyncService();

    MySqlSyncService(const MySqlSyncService&) = delete;
    MySqlSyncService& operator=(const MySqlSyncService&) = delete;

    /** @brief 启动后台线程；重复调用无效。 */
    void Start();

    /** @brief 请求停止并等待线程退出。 */
    void Stop();

    /** @brief 只发布停止请求并唤醒线程，不阻塞调用线程。 */
    void RequestStop() noexcept;

    /** @brief 等待已经收到停止请求的工作线程结束。 */
    void Wait();

    /** @brief 返回后台同步线程是否仍在运行。 */
    bool IsRunning() const;

    /** @brief 新消息入队后唤醒后台线程。 */
    void Wake();

    /** @brief 返回不暴露密码和 SQL 的状态快照。 */
    MySqlSyncSnapshot snapshot() const;

private:
    void WorkerLoop();
    void WaitFor(std::uint32_t milliseconds);
    void RecordFailure(const MySqlStatus& status);

    MySqlSyncQueue& queue_;
    MySqlClient& client_;
    MySqlSyncExecutor executor_;
    ExecutionLogger execution_logger_;
    std::uint32_t batch_size_;
    std::uint32_t base_retry_seconds_;
    std::atomic<bool> stop_requested_{false};
    std::thread worker_;
    mutable std::mutex wait_mutex_;
    std::condition_variable wake_condition_;
    mutable std::mutex snapshot_mutex_;
    MySqlSyncSnapshot snapshot_;
};

}  // namespace videosc::dedup
