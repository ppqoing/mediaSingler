#include "StructuralVerificationScheduler.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace videosc::dedup {

StructuralVerificationScheduler::StructuralVerificationScheduler(
    const std::uint32_t requested_workers,
    const ComputeConfig compute,
    const StorageConfig storage,
    const std::atomic_bool& cancel_requested)
    : effective_worker_count_((std::max)(
          1U,
          (std::min)({requested_workers,
                      (std::max)(1U, compute.worker_threads),
                      (std::max)(1U, storage.max_concurrent_file_reads)}))),
      per_disk_limit_((std::max)(1U, storage.hdd_read_threads_per_disk)),
      cancel_requested_(cancel_requested) {}

std::uint32_t StructuralVerificationScheduler::effective_worker_count() const noexcept {
    return effective_worker_count_;
}

bool StructuralVerificationScheduler::RunWithReadBudget(
    const std::string& storage_target_key,
    const std::function<void()>& operation) {
    if (!operation) return false;
    const std::string key = storage_target_key.empty() ? "unknown-hdd" : storage_target_key;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!cancel_requested_.load(std::memory_order_relaxed) &&
               active_by_disk_[key] >= per_disk_limit_) {
            available_.wait_for(lock, std::chrono::milliseconds(100));
        }
        if (cancel_requested_.load(std::memory_order_relaxed)) return false;
        ++active_by_disk_[key];
    }
    try {
        operation();
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (--active_by_disk_[key] == 0) active_by_disk_.erase(key);
        }
        available_.notify_all();
        throw;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (--active_by_disk_[key] == 0) active_by_disk_.erase(key);
    }
    available_.notify_all();
    return true;
}

}  // namespace videosc::dedup
