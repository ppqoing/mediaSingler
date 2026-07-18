#include "TaskThreadPool.h"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

namespace videosc::dedup {

TaskThreadPool::TaskThreadPool(std::string name,
                               const std::uint32_t worker_count,
                               const std::size_t queue_capacity)
    : name_(std::move(name)), queue_capacity_(queue_capacity) {
    if (name_.empty() || worker_count == 0 || queue_capacity_ == 0) {
        throw std::invalid_argument("Invalid task thread pool options");
    }
    workers_.reserve(worker_count);
    try {
        for (std::uint32_t index = 0; index < worker_count; ++index) {
            workers_.emplace_back([this] { WorkerMain(); });
        }
    } catch (...) {
        RequestCancel();
        for (std::thread& worker : workers_) {
            if (worker.joinable()) worker.join();
        }
        throw;
    }
}

TaskThreadPool::~TaskThreadPool() {
    CancelAndJoin();
}

bool TaskThreadPool::Submit(Task task) {
    if (!task) return false;
    std::unique_lock<std::mutex> lock(mutex_);
    queue_space_.wait(lock, [this] {
        return cancelling_ || !accepting_ || tasks_.size() < queue_capacity_;
    });
    if (cancelling_ || !accepting_) return false;
    tasks_.push_back(std::move(task));
    task_available_.notify_one();
    return true;
}

void TaskThreadPool::CloseSubmissions() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        accepting_ = false;
    }
    task_available_.notify_all();
    queue_space_.notify_all();
}

void TaskThreadPool::WaitUntilIdle() {
    std::unique_lock<std::mutex> lock(mutex_);
    idle_.wait(lock, [this] { return tasks_.empty() && active_threads_ == 0; });
}

void TaskThreadPool::RequestCancel() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        accepting_ = false;
        cancelling_ = true;
        tasks_.clear();
    }
    task_available_.notify_all();
    queue_space_.notify_all();
    idle_.notify_all();
}

void TaskThreadPool::Join() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (joined_) return;
        accepting_ = false;
    }
    task_available_.notify_all();
    queue_space_.notify_all();
    for (std::thread& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    joined_ = true;
}

void TaskThreadPool::CancelAndJoin() {
    RequestCancel();
    Join();
}

TaskThreadPoolSnapshot TaskThreadPool::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {name_,
            static_cast<std::uint32_t>(workers_.size()),
            active_threads_,
            static_cast<std::uint64_t>(tasks_.size()),
            completed_tasks_,
            failed_tasks_,
            accepting_,
            cancelling_};
}

std::string TaskThreadPool::failure_message() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return failure_message_;
}

void TaskThreadPool::WorkerMain() {
    while (true) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            task_available_.wait(lock, [this] {
                return cancelling_ || !tasks_.empty() || !accepting_;
            });
            if (cancelling_) return;
            if (tasks_.empty()) {
                if (!accepting_) return;
                continue;
            }
            task = std::move(tasks_.front());
            tasks_.pop_front();
            ++active_threads_;
            queue_space_.notify_one();
        }

        bool failed = false;
        std::string message;
        try {
            task();
        } catch (const std::exception& exception) {
            failed = true;
            message = exception.what();
        } catch (...) {
            failed = true;
            message = "unknown task exception";
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            --active_threads_;
            ++completed_tasks_;
            if (failed) {
                ++failed_tasks_;
                if (failure_message_.empty()) failure_message_ = std::move(message);
            }
            if (tasks_.empty() && active_threads_ == 0) idle_.notify_all();
        }
    }
}

}  // namespace videosc::dedup

