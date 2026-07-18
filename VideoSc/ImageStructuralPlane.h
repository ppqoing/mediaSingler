#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace videosc {

inline constexpr std::size_t kStructuralImageSide = 256;
inline constexpr std::size_t kStructuralBlockSide = 16;
inline constexpr std::size_t kStructuralBlockCount = 256;
inline constexpr std::uint32_t kImageStructuralAlgorithmVersion = 1;

/**
 * @brief 一张图片的报告期结构面与可复用统计量。
 *
 * 该对象由 VideoSc DLL 创建和释放，只用于同坐标的整图比较；它不实现旋转、裁剪、
 * 平移或局部特征搜索，也不进入持久化数据模型。
 */
struct ImageStructuralPlane {
    std::array<std::uint8_t, kStructuralImageSide * kStructuralImageSide> gray{};
    std::array<std::uint16_t, kStructuralImageSide * kStructuralImageSide> edge{};
    std::array<std::uint64_t, kStructuralBlockCount> block_gray_sum{};
    std::array<std::uint64_t, kStructuralBlockCount> block_gray_square_sum{};
    std::uint64_t edge_sum = 0;
    std::uint64_t edge_square_sum = 0;
};

/** @brief 结构比较的项目阈值输入；阈值只影响通过块计数，不改变原始聚合分。 */
struct ImageStructuralCompareOptions {
    double block_pass_score = 0.90;
};

/** @brief 结构比较输出；所有量化分均使用 0～1,000,000 的固定整数范围。 */
struct ImageStructuralCompareResult {
    std::uint32_t global_edge_zncc_millionths = 0;
    std::uint32_t trimmed_block_score_millionths = 0;
    std::uint32_t passing_block_percent_millionths = 0;
    std::uint32_t compared_block_count = 0;
};

/**
 * @brief 从 256×256 灰度面创建结构面并预计算不随候选变化的统计量。
 * @param gray256 归一化灰度面。
 * @param stride 灰度面的行跨度，单位为字节。
 * @param error 失败时接收错误说明。
 * @return 成功时返回独占结构面；输入无效时返回空指针。
 */
std::unique_ptr<ImageStructuralPlane> BuildImageStructuralPlane(const std::uint8_t* gray256,
                                                                std::ptrdiff_t stride,
                                                                std::string& error);

/**
 * @brief 比较两张同坐标结构面并执行最多 25% 最差块裁剪。
 * @param left 左侧结构面。
 * @param right 右侧结构面。
 * @param options 块通过阈值。
 * @param output 接收量化后的全局与块级证据。
 * @param error 失败时接收错误说明。
 * @return 比较成功返回 true；输入或阈值无效返回 false。
 */
bool CompareImageStructuralPlanes(const ImageStructuralPlane& left,
                                  const ImageStructuralPlane& right,
                                  const ImageStructuralCompareOptions& options,
                                  ImageStructuralCompareResult& output,
                                  std::string& error);

}  // namespace videosc

