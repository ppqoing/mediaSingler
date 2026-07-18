#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace videosc::dedup {

/** @brief 有界任务线程池的只读运行快照。 */
struct TaskThreadPoolSnapshot {
    std::string name;
    std::uint32_t configured_threads = 0;
    std::uint32_t active_threads = 0;
    std::uint64_t queued_tasks = 0;
    std::uint64_t completed_tasks = 0;
    std::uint64_t failed_tasks = 0;
    bool accepting = false;
    bool cancelling = false;
};

/**
 * @brief 带有界背压、异常收集、排空和取消能力的固定线程池。
 *
 * 线程池只负责执行不可变任务，不理解扫描、报告或 GUI 状态。调用者必须在任务捕获中传入
 * 自己的取消标志，并在销毁 RocksDB 等依赖之前调用 Join 或 CancelAndJoin。
 */
class TaskThreadPool final {
public:
    using Task = std::function<void()>;

    /**
     * @brief 创建并立即启动固定数量的工作线程。
     * @param name 用于进度和日志的稳定线程池名称。
     * @param worker_count 工作线程数，必须大于零。
     * @param queue_capacity 等待队列上限，必须大于零。
     * @throws std::invalid_argument 参数非法时抛出。
     * @throws std::system_error 无法创建工作线程时抛出。
     */
    TaskThreadPool(std::string name, std::uint32_t worker_count, std::size_t queue_capacity);

    /** @brief 析构时取消等待任务并等待全部工作线程退出。 */
    ~TaskThreadPool();

    TaskThreadPool(const TaskThreadPool&) = delete;
    TaskThreadPool& operator=(const TaskThreadPool&) = delete;

    /**
     * @brief 将任务放入有界队列；队列满时等待消费者释放空间。
     * @param task 不可为空的任务。
     * @return 成功入队返回 true；停止接单、取消或任务为空时返回 false。
     */
    bool Submit(Task task);

    /** @brief 停止接收新任务；已入队任务继续执行。 */
    void CloseSubmissions();

    /** @brief 等待队列清空且活动任务归零。 */
    void WaitUntilIdle();

    /** @brief 请求取消并丢弃尚未开始的任务；活动任务由调用者取消标志协作退出。 */
    void RequestCancel();

    /** @brief 停止接单并等待所有工作线程退出。 */
    void Join();

    /** @brief 请求取消后等待所有工作线程退出。 */
    void CancelAndJoin();

    /** @return 当前线程、队列、完成数和异常数的线程安全快照。 */
    TaskThreadPoolSnapshot snapshot() const;

    /** @return 首个任务异常文本；没有异常时为空。 */
    std::string failure_message() const;

private:
    /** @brief 工作线程循环；只在队列锁外执行用户任务。 */
    void WorkerMain();

    std::string name_;
    std::size_t queue_capacity_ = 0;
    mutable std::mutex mutex_;
    std::condition_variable task_available_;
    std::condition_variable queue_space_;
    std::condition_variable idle_;
    std::deque<Task> tasks_;
    std::vector<std::thread> workers_;
    std::uint32_t active_threads_ = 0;
    std::uint64_t completed_tasks_ = 0;
    std::uint64_t failed_tasks_ = 0;
    std::string failure_message_;
    bool accepting_ = true;
    bool cancelling_ = false;
    bool joined_ = false;
};

}  // namespace videosc::dedup

