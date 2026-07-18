#include "pch.h"

#include "ImageStructuralPlane.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace videosc {
namespace {

constexpr std::size_t kBlockGridSide = kStructuralImageSide / kStructuralBlockSide;
constexpr std::size_t kPixelsPerBlock = kStructuralBlockSide * kStructuralBlockSide;
constexpr std::uint32_t kScoreScale = 1'000'000;

/**
 * @brief 由预计算边缘统计量和本次点积计算全局 ZNCC。
 * @param left 左侧结构面。
 * @param right 右侧结构面。
 * @return 归一化到 0～1 的边缘相关性；零方差且完全相同返回 1。
 */
double ComputeGlobalEdgeZncc(const ImageStructuralPlane& left, const ImageStructuralPlane& right) {
    long double product_sum = 0.0L;
    bool identical = true;
    for (std::size_t index = 0; index < left.edge.size(); ++index) {
        product_sum += static_cast<long double>(left.edge[index]) * right.edge[index];
        identical = identical && left.edge[index] == right.edge[index];
    }

    const long double count = static_cast<long double>(left.edge.size());
    const long double numerator = count * product_sum -
                                  static_cast<long double>(left.edge_sum) * right.edge_sum;
    const long double left_variance = count * static_cast<long double>(left.edge_square_sum) -
                                      static_cast<long double>(left.edge_sum) * left.edge_sum;
    const long double right_variance = count * static_cast<long double>(right.edge_square_sum) -
                                       static_cast<long double>(right.edge_sum) * right.edge_sum;
    if (left_variance <= 0.0L || right_variance <= 0.0L) return identical ? 1.0 : 0.0;
    const long double denominator = std::sqrt(left_variance * right_variance);
    return (std::clamp)(static_cast<double>(numerator / denominator), 0.0, 1.0);
}

/**
 * @brief 计算一个 16×16 固定坐标块的 ZNCC/SSIM 组合分。
 * @param left 左侧结构面。
 * @param right 右侧结构面。
 * @param block_index 行优先块序号。
 * @return 0～1 的块结构分。
 */
double ComputeBlockScore(const ImageStructuralPlane& left,
                         const ImageStructuralPlane& right,
                         const std::size_t block_index) {
    const std::size_t block_y = block_index / kBlockGridSide;
    const std::size_t block_x = block_index % kBlockGridSide;
    long double product_sum = 0.0L;
    bool identical = true;
    for (std::size_t local_y = 0; local_y < kStructuralBlockSide; ++local_y) {
        const std::size_t image_y = block_y * kStructuralBlockSide + local_y;
        const std::size_t offset = image_y * kStructuralImageSide + block_x * kStructuralBlockSide;
        for (std::size_t local_x = 0; local_x < kStructuralBlockSide; ++local_x) {
            const std::uint8_t left_value = left.gray[offset + local_x];
            const std::uint8_t right_value = right.gray[offset + local_x];
            product_sum += static_cast<long double>(left_value) * right_value;
            identical = identical && left_value == right_value;
        }
    }

    const long double count = static_cast<long double>(kPixelsPerBlock);
    const long double left_sum = static_cast<long double>(left.block_gray_sum[block_index]);
    const long double right_sum = static_cast<long double>(right.block_gray_sum[block_index]);
    const long double left_square_sum = static_cast<long double>(left.block_gray_square_sum[block_index]);
    const long double right_square_sum = static_cast<long double>(right.block_gray_square_sum[block_index]);
    const long double numerator = count * product_sum - left_sum * right_sum;
    const long double left_variance_scaled = count * left_square_sum - left_sum * left_sum;
    const long double right_variance_scaled = count * right_square_sum - right_sum * right_sum;

    double zncc = 0.0;
    if (left_variance_scaled <= 0.0L || right_variance_scaled <= 0.0L) {
        zncc = identical ? 1.0 : 0.0;
    } else {
        zncc = static_cast<double>(numerator /
                                   std::sqrt(left_variance_scaled * right_variance_scaled));
    }
    zncc = (std::clamp)(zncc, 0.0, 1.0);

    const long double left_mean = left_sum / count;
    const long double right_mean = right_sum / count;
    const long double left_variance = left_square_sum / count - left_mean * left_mean;
    const long double right_variance = right_square_sum / count - right_mean * right_mean;
    const long double covariance = product_sum / count - left_mean * right_mean;
    constexpr long double c1 = 6.5025L;
    constexpr long double c2 = 58.5225L;
    const long double ssim_denominator =
        (left_mean * left_mean + right_mean * right_mean + c1) *
        (left_variance + right_variance + c2);
    const double ssim = ssim_denominator <= 0.0L
                            ? (identical ? 1.0 : 0.0)
                            : (std::clamp)(static_cast<double>(
                                               ((2.0L * left_mean * right_mean + c1) *
                                                (2.0L * covariance + c2)) /
                                               ssim_denominator),
                                           0.0,
                                           1.0);
    return zncc * 0.65 + ssim * 0.35;
}

/** @brief 把有限 0～1 分数量化为跨 CPU 稳定的百万分整数。 */
std::uint32_t QuantizeScore(const double score) {
    const double normalized = std::isfinite(score) ? (std::clamp)(score, 0.0, 1.0) : 0.0;
    return static_cast<std::uint32_t>(std::llround(normalized * kScoreScale));
}

}  // namespace

std::unique_ptr<ImageStructuralPlane> BuildImageStructuralPlane(const std::uint8_t* gray256,
                                                                const std::ptrdiff_t stride,
                                                                std::string& error) {
    if (gray256 == nullptr || stride < static_cast<std::ptrdiff_t>(kStructuralImageSide)) {
        error = "Invalid structural image plane";
        return nullptr;
    }

    auto plane = std::make_unique<ImageStructuralPlane>();
    for (std::size_t y = 0; y < kStructuralImageSide; ++y) {
        const std::uint8_t* source = gray256 + static_cast<std::ptrdiff_t>(y) * stride;
        std::copy_n(source, kStructuralImageSide, plane->gray.begin() + y * kStructuralImageSide);
    }

    // 镜像边界会把整图外缘复制到自身，避免边框颜色被补零放大成虚假强边缘。
    for (std::size_t y = 0; y < kStructuralImageSide; ++y) {
        const std::size_t previous_y = y == 0 ? 0 : y - 1;
        const std::size_t next_y = y + 1 == kStructuralImageSide ? y : y + 1;
        for (std::size_t x = 0; x < kStructuralImageSide; ++x) {
            const std::size_t previous_x = x == 0 ? 0 : x - 1;
            const std::size_t next_x = x + 1 == kStructuralImageSide ? x : x + 1;
            const auto pixel = [&](const std::size_t sample_y, const std::size_t sample_x) {
                return static_cast<int>(plane->gray[sample_y * kStructuralImageSide + sample_x]);
            };
            const int gradient_x = -pixel(previous_y, previous_x) + pixel(previous_y, next_x) -
                                   2 * pixel(y, previous_x) + 2 * pixel(y, next_x) -
                                   pixel(next_y, previous_x) + pixel(next_y, next_x);
            const int gradient_y = -pixel(previous_y, previous_x) - 2 * pixel(previous_y, x) -
                                   pixel(previous_y, next_x) + pixel(next_y, previous_x) +
                                   2 * pixel(next_y, x) + pixel(next_y, next_x);
            const std::uint16_t magnitude = static_cast<std::uint16_t>(
                (std::min)(2040, std::abs(gradient_x) + std::abs(gradient_y)));
            plane->edge[y * kStructuralImageSide + x] = magnitude;
            plane->edge_sum += magnitude;
            plane->edge_square_sum += static_cast<std::uint64_t>(magnitude) * magnitude;

            const std::size_t block = (y / kStructuralBlockSide) * kBlockGridSide +
                                      (x / kStructuralBlockSide);
            const std::uint64_t gray = plane->gray[y * kStructuralImageSide + x];
            plane->block_gray_sum[block] += gray;
            plane->block_gray_square_sum[block] += gray * gray;
        }
    }

    error.clear();
    return plane;
}

bool CompareImageStructuralPlanes(const ImageStructuralPlane& left,
                                  const ImageStructuralPlane& right,
                                  const ImageStructuralCompareOptions& options,
                                  ImageStructuralCompareResult& output,
                                  std::string& error) {
    output = {};
    if (!std::isfinite(options.block_pass_score) || options.block_pass_score < 0.0 ||
        options.block_pass_score > 1.0) {
        error = "Invalid structural block threshold";
        return false;
    }

    std::array<double, kStructuralBlockCount> block_scores{};
    std::uint32_t passing_blocks = 0;
    for (std::size_t block = 0; block < block_scores.size(); ++block) {
        block_scores[block] = ComputeBlockScore(left, right, block);
        if (block_scores[block] >= options.block_pass_score) ++passing_blocks;
    }
    std::sort(block_scores.begin(), block_scores.end());

    // 丢弃最差 25% 块以容忍水印，但剩余 75% 固定坐标区域必须共同决定聚合分。
    constexpr std::size_t ignored_blocks = kStructuralBlockCount / 4;
    long double retained_sum = 0.0L;
    for (std::size_t block = ignored_blocks; block < block_scores.size(); ++block) {
        retained_sum += block_scores[block];
    }
    const double trimmed_score = static_cast<double>(
        retained_sum / static_cast<long double>(kStructuralBlockCount - ignored_blocks));

    output.global_edge_zncc_millionths = QuantizeScore(ComputeGlobalEdgeZncc(left, right));
    output.trimmed_block_score_millionths = QuantizeScore(trimmed_score);
    output.passing_block_percent_millionths = QuantizeScore(
        static_cast<double>(passing_blocks) / static_cast<double>(kStructuralBlockCount));
    output.compared_block_count = static_cast<std::uint32_t>(kStructuralBlockCount);
    error.clear();
    return true;
}

}  // namespace videosc

