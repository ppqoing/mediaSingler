#pragma once

#include "../config/AppConfig.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace videosc::dedup {

/**
 * @brief 为图片结构三筛提供全局 CPU/读取上限和每物理盘读取许可。
 *
 * 报告线程池大小由 effective_worker_count 限制；每次潜在结构解码还必须取得 storage_target_key
 * 的许可。无法识别介质时使用更安全的 HDD 每盘上限。
 */
class StructuralVerificationScheduler final {
public:
    /**
     * @param requested_workers 图片三筛配置线程数。
     * @param compute 全局 CPU 配置。
     * @param storage 全局/每盘读取配置。
     * @param cancel_requested 外部取消标志。
     */
    StructuralVerificationScheduler(std::uint32_t requested_workers,
                                    ComputeConfig compute,
                                    StorageConfig storage,
                                    const std::atomic_bool& cancel_requested);

    /** @return 同时满足图片、CPU 和全局读取上限的工作线程数。 */
    std::uint32_t effective_worker_count() const noexcept;

    /**
     * @brief 在取得目标磁盘许可后执行一个可能触发解码的操作。
     * @param storage_target_key 物理存储稳定键；空值按未知 HDD 处理。
     * @param operation 锁外执行的操作。
     * @return 成功执行返回 true；取消前未取得许可返回 false。
     */
    bool RunWithReadBudget(const std::string& storage_target_key,
                           const std::function<void()>& operation);

private:
    std::uint32_t effective_worker_count_ = 1;
    std::uint32_t per_disk_limit_ = 1;
    const std::atomic_bool& cancel_requested_;
    std::mutex mutex_;
    std::condition_variable available_;
    std::unordered_map<std::string, std::uint32_t> active_by_disk_;
};

}  // namespace videosc::dedup
