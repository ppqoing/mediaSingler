#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace videosc {

inline constexpr std::size_t kPdqHashBytes = 32;
inline constexpr std::size_t kZonedPHashCount = 16;
inline constexpr std::uint32_t kImagePerceptualAlgorithmVersion = 1;

/**
 * @brief 一张归一化图片的持久化感知特征。
 *
 * PDQ 使用 Meta 官方 256 位位序的文本字节顺序；分区 pHash 按从左到右、
 * 从上到下保存 4×4 共 16 个 64 位结果。该类型不持有解码缓冲区。
 */
struct ImagePerceptualFeatures {
    std::array<std::uint8_t, kPdqHashBytes> pdq_hash{};
    std::uint8_t pdq_quality = 0;
    std::array<std::uint64_t, kZonedPHashCount> zoned_phashes{};
};

/**
 * @brief 从固定尺寸灰度面生成 PDQ-256 与 4×4 分区 pHash。
 * @param gray64 64×64 灰度面；由 FFmpeg 面积缩放从同一解码帧生成。
 * @param gray64_stride 64×64 灰度面的行跨度，单位为字节。
 * @param gray256 256×256 灰度面；供 4×4 分区 pHash 使用。
 * @param gray256_stride 256×256 灰度面的行跨度，单位为字节。
 * @param output 接收完整感知特征。
 * @param error 失败时接收不含文件路径的错误说明。
 * @return 全部特征成功生成时返回 true，否则返回 false。
 */
bool ComputeImagePerceptualFeatures(const std::uint8_t* gray64,
                                    std::ptrdiff_t gray64_stride,
                                    const std::uint8_t* gray256,
                                    std::ptrdiff_t gray256_stride,
                                    ImagePerceptualFeatures& output,
                                    std::string& error);

}  // namespace videosc

