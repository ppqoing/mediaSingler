#pragma once

#include "../models/CoreModels.h"
#include "../config/AppConfig.h"
#include "NativeFileDiscovery.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace videosc::dedup {

/** @brief Everything 批量发现期间可报告给协调器的细分阶段。 */
enum class EverythingDiscoveryStage {
    Preparing,
    QueryingIndex,
    ProcessingResults,
    QueryingPhysicalLocation,
};

/** @brief 单个扫描根在 Everything 批量查询中的结果与覆盖关系。 */
struct EverythingRootResult {
    DiscoveryStats stats;
    bool covered_by_higher_priority = false;
};

/**
 * @brief 基于 Everything64.dll SDK 索引查询的流式文件发现器。
 *
 * 与 NativeFileDiscovery 实现相同的 PrepareRoot/Enumerate 接口签名。
 * Everything 未运行时自动启动并等待数据库加载完成。
 * SDK 查询通过内部 mutex 串行化（Everything IPC 非线程安全）。
 * Enumerate 返回 error 非空时，上层应回退 NativeFileDiscovery。
 */
class EverythingFileDiscovery final {
public:
    using FileVisitor = NativeFileDiscovery::FileVisitor;
    using StageVisitor = std::function<void(std::uint32_t root_priority,
                                            EverythingDiscoveryStage stage)>;

    /** @brief 设置 Everything 路径、分页和等待参数（扫描启动前调用）。 */
    static void Configure(const DiscoveryConfig& config);

    /** @brief 查询扫描根的卷 GUID、物理存储目标和介质类型（委托 NativeFileDiscovery）。 */
    static std::optional<DiscoveryRoot> PrepareRoot(const std::filesystem::path& path,
                                                    std::uint32_t priority,
                                                    bool hdd_extent_optimization,
                                                    std::string& error);

    /**
     * @brief 通过 Everything SDK 索引查询枚举一个扫描根下的所有文件。
     * @return DiscoveryStats；error 非空表示 SDK 不可用，上层应回退。
     */
    static DiscoveryStats Enumerate(const DiscoveryRoot& root,
                                    std::uint64_t scan_id,
                                    const std::atomic_bool& cancel_requested,
                                    const FileVisitor& visitor);

    /**
     * @brief 使用一个 OR 查询批量枚举多个扫描根，并按根优先级路由结果。
     * @param roots 已准备且按配置优先级排列的扫描根。
     * @param scan_id 当前扫描标识。
     * @param cancel_requested 跨线程取消标志。
     * @param visitor 单文件消费回调；返回 false 时停止批量枚举。
     * @param stage_visitor 可选的单根阶段通知回调。
     * @return 与 roots 顺序一致的结果列表。
     */
    static std::vector<EverythingRootResult> EnumerateRoots(
        const std::vector<DiscoveryRoot>& roots,
        std::uint64_t scan_id,
        const std::atomic_bool& cancel_requested,
        const FileVisitor& visitor,
        const StageVisitor& stage_visitor = {});

private:
    struct SdkState;
    static SdkState& State();
    static bool EnsureReady(const std::atomic_bool& cancel_requested,
                            bool& cancelled,
                            std::string& error);
};

}  // namespace videosc::dedup
