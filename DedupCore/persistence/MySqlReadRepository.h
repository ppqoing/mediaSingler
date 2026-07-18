#pragma once

#include "MySqlClient.h"
#include "../models/CoreModels.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace videosc::dedup {

/** @brief 当前共享库图片作用域的 V1 特征完整性快照。 */
struct ImageFeatureCompletenessSnapshot {
    std::uint64_t total_images = 0;
    std::uint64_t complete_images = 0;
    std::uint64_t incomplete_images = 0;

    /** @return 总数与分项守恒且没有未完成图片时返回 true。 */
    bool complete() const noexcept {
        return total_images == complete_images && incomplete_images == 0;
    }
};

/** @brief 一个待回填唯一图片 SHA 及其全部 active 路径候选。 */
struct ImageFeatureBackfillTarget {
    ShaFileData content;
    std::vector<FilePathRecord> active_paths;
};

/**
 * @brief 只读访问 MySQL 中的路径映射和媒体内容。
 *
 * 扫描规划使用有界批量查询，相似报告使用流式回调；本类不缓存全表，也不修改数据库。
 */
class MySqlReadRepository final {
public:
    using ContentVisitor = std::function<bool(ShaFileData&& data)>;
    using PathVisitor = std::function<bool(FilePathRecord&& path)>;
    using ImageBackfillVisitor = std::function<bool(ImageFeatureBackfillTarget&& target)>;

    /**
     * @brief 绑定线程安全的 MySQL 客户端。
     * @param client 生命周期必须覆盖本仓库。
     */
    explicit MySqlReadRepository(MySqlClient& client);

    /**
     * @brief 按规范化路径的 SHA-256 索引批量加载路径映射。
     * @param normalized_paths 已规范化路径。
     * @param records 输出以完整规范化路径为键的远端记录。
     * @return 查询状态；摘要碰撞会通过完整路径复核排除。
     */
    MySqlStatus LoadPaths(
        const std::vector<std::wstring>& normalized_paths,
        std::unordered_map<std::wstring, FilePathRecord>& records);

    /**
     * @brief 按 SHA-512 批量加载内容记录。
     * @param digests 内容摘要。
     * @param records 输出以 SHA-512 十六进制为键的内容记录。
     * @return 查询或解析状态。
     */
    MySqlStatus LoadContents(
        const std::vector<Sha512Digest>& digests,
        std::unordered_map<std::string, ShaFileData>& records);

    /**
     * @brief 流式读取指定算法版本的全部有效图片和视频视觉内容。
     * @param algorithm_version 媒体算法版本。
     * @param visitor 每条记录回调，返回 false 可提前停止。
     * @return 查询或解析状态。
     */
    MySqlStatus StreamVisualContents(const std::string& algorithm_version,
                                     const ContentVisitor& visitor);

    /**
     * @brief 统计至少具有一条 active 路径的唯一图片 SHA 特征完整性。
     * @param algorithm_version 当前媒体算法版本。
     * @param snapshot 输出守恒快照。
     * @return 查询或解析状态。
     */
    MySqlStatus CountImageFeatureCompleteness(
        const std::string& algorithm_version,
        ImageFeatureCompletenessSnapshot& snapshot);

    /**
     * @brief 按 SHA 流式读取缺失或版本过期图片及其全部 active 路径。
     * @param algorithm_version 当前媒体算法版本。
     * @param visitor 每个唯一 SHA 回调一次；返回 false 可提前停止。
     * @return 查询或解析状态。
     */
    MySqlStatus StreamIncompleteImageFeatureTargets(
        const std::string& algorithm_version,
        const ImageBackfillVisitor& visitor);

    /**
     * @brief 流式读取 MySQL 全部 active 路径。
     * @param visitor 每条路径回调，返回 false 可提前停止。
     * @return 查询或解析状态。
     */
    MySqlStatus StreamActivePaths(const PathVisitor& visitor);

private:
    MySqlClient& client_;
};

}  // namespace videosc::dedup
