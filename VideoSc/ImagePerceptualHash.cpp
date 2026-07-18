#include "pch.h"

#include "ImagePerceptualHash.h"

#include <pdq/cpp/hashing/pdqhashing.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace videosc {
namespace {

constexpr std::size_t kPdqSide = 64;
constexpr std::size_t kNormalizedSide = 256;
constexpr std::size_t kZoneGridSide = 4;
constexpr std::size_t kZoneSide = kNormalizedSide / kZoneGridSide;
constexpr std::size_t kPHashSide = 32;
constexpr std::size_t kPHashDctSide = 8;
constexpr double kPi = 3.1415926535897932384626433832795;

/** @brief pHash 的固定 DCT 系数，进程内只构造一次。 */
const std::array<double, kPHashDctSide * kPHashSide>& DctCoefficients() {
    static const auto coefficients = [] {
        std::array<double, kPHashDctSide * kPHashSide> values{};
        for (std::size_t frequency = 0; frequency < kPHashDctSide; ++frequency) {
            const double scale = frequency == 0
                                     ? std::sqrt(1.0 / static_cast<double>(kPHashSide))
                                     : std::sqrt(2.0 / static_cast<double>(kPHashSide));
            for (std::size_t sample = 0; sample < kPHashSide; ++sample) {
                values[frequency * kPHashSide + sample] =
                    scale * std::cos(kPi * static_cast<double>((2 * sample + 1) * frequency) /
                                     static_cast<double>(2 * kPHashSide));
            }
        }
        return values;
    }();
    return coefficients;
}

/**
 * @brief 对一个 64×64 固定分区生成 64 位 pHash。
 * @param zone 分区左上角。
 * @param stride 原 256×256 灰度面的行跨度。
 * @return bit0 固定为零，其余 63 位对应排除 DC 后的 8×8 低频系数。
 */
std::uint64_t ComputeZonePHash(const std::uint8_t* zone, const std::ptrdiff_t stride) {
    std::array<double, kPHashSide * kPHashSide> downsampled{};
    for (std::size_t y = 0; y < kPHashSide; ++y) {
        const std::uint8_t* row0 = zone + static_cast<std::ptrdiff_t>(y * 2) * stride;
        const std::uint8_t* row1 = row0 + stride;
        for (std::size_t x = 0; x < kPHashSide; ++x) {
            const std::size_t source_x = x * 2;
            const unsigned int sum = static_cast<unsigned int>(row0[source_x]) +
                                     static_cast<unsigned int>(row0[source_x + 1]) +
                                     static_cast<unsigned int>(row1[source_x]) +
                                     static_cast<unsigned int>(row1[source_x + 1]);
            downsampled[y * kPHashSide + x] = static_cast<double>(sum) * 0.25;
        }
    }

    const auto& coefficients = DctCoefficients();
    std::array<double, kPHashDctSide * kPHashSide> horizontal{};
    for (std::size_t y = 0; y < kPHashSide; ++y) {
        for (std::size_t u = 0; u < kPHashDctSide; ++u) {
            double sum = 0.0;
            for (std::size_t x = 0; x < kPHashSide; ++x) {
                sum += downsampled[y * kPHashSide + x] * coefficients[u * kPHashSide + x];
            }
            horizontal[u * kPHashSide + y] = sum;
        }
    }

    std::array<double, kPHashDctSide * kPHashDctSide> dct{};
    for (std::size_t v = 0; v < kPHashDctSide; ++v) {
        for (std::size_t u = 0; u < kPHashDctSide; ++u) {
            double sum = 0.0;
            for (std::size_t y = 0; y < kPHashSide; ++y) {
                sum += horizontal[u * kPHashSide + y] * coefficients[v * kPHashSide + y];
            }
            dct[v * kPHashDctSide + u] = sum;
        }
    }

    std::array<double, 63> ac_coefficients{};
    std::copy(dct.begin() + 1, dct.end(), ac_coefficients.begin());
    auto median_position = ac_coefficients.begin() + ac_coefficients.size() / 2;
    std::nth_element(ac_coefficients.begin(), median_position, ac_coefficients.end());
    const double median = *median_position;

    std::uint64_t hash = 0;
    for (std::size_t coefficient = 1; coefficient < dct.size(); ++coefficient) {
        if (dct[coefficient] > median) hash |= (std::uint64_t{1} << coefficient);
    }
    return hash;
}

/**
 * @brief 把 Meta Hash256 转为其 64 字符十六进制表示对应的 32 字节顺序。
 * @param source Meta 官方 PDQ 结果。
 * @param destination 接收规范化持久字节。
 */
void CopyCanonicalPdqBytes(
    const facebook::pdq::hashing::Hash256& source,
    std::array<std::uint8_t, kPdqHashBytes>& destination) {
    for (std::size_t output_word = 0; output_word < 16; ++output_word) {
        const std::uint16_t word = source.w[15 - output_word];
        destination[output_word * 2] = static_cast<std::uint8_t>(word >> 8);
        destination[output_word * 2 + 1] = static_cast<std::uint8_t>(word & 0xffU);
    }
}

}  // namespace

bool ComputeImagePerceptualFeatures(const std::uint8_t* gray64,
                                    const std::ptrdiff_t gray64_stride,
                                    const std::uint8_t* gray256,
                                    const std::ptrdiff_t gray256_stride,
                                    ImagePerceptualFeatures& output,
                                    std::string& error) {
    output = {};
    if (gray64 == nullptr || gray256 == nullptr || gray64_stride < static_cast<std::ptrdiff_t>(kPdqSide) ||
        gray256_stride < static_cast<std::ptrdiff_t>(kNormalizedSide)) {
        error = "Invalid perceptual image planes";
        return false;
    }

    // 官方 PDQ 支持已由站点解码器缩放好的 64×64 灰度面，可避免为超大图片分配两张全尺寸 float 图。
    std::array<float, kPdqSide * kPdqSide> luma{};
    for (std::size_t y = 0; y < kPdqSide; ++y) {
        const std::uint8_t* row = gray64 + static_cast<std::ptrdiff_t>(y) * gray64_stride;
        for (std::size_t x = 0; x < kPdqSide; ++x) luma[y * kPdqSide + x] = static_cast<float>(row[x]);
    }
    std::array<float, kPdqSide * kPdqSide> workspace{};
    float pdq64[kPdqSide][kPdqSide]{};
    float pdq16x64[16][kPdqSide]{};
    float pdq16x16[16][16]{};
    facebook::pdq::hashing::Hash256 pdq_hash;
    int pdq_quality = 0;
    facebook::pdq::hashing::pdqHash256FromFloatLuma(luma.data(),
                                                    workspace.data(),
                                                    static_cast<int>(kPdqSide),
                                                    static_cast<int>(kPdqSide),
                                                    pdq64,
                                                    pdq16x64,
                                                    pdq16x16,
                                                    pdq_hash,
                                                    pdq_quality);
    CopyCanonicalPdqBytes(pdq_hash, output.pdq_hash);
    output.pdq_quality = static_cast<std::uint8_t>((std::clamp)(pdq_quality, 0, 100));

    for (std::size_t zone_y = 0; zone_y < kZoneGridSide; ++zone_y) {
        for (std::size_t zone_x = 0; zone_x < kZoneGridSide; ++zone_x) {
            const std::uint8_t* zone = gray256 + static_cast<std::ptrdiff_t>(zone_y * kZoneSide) *
                                                        gray256_stride +
                                       zone_x * kZoneSide;
            output.zoned_phashes[zone_y * kZoneGridSide + zone_x] =
                ComputeZonePHash(zone, gray256_stride);
        }
    }

    error.clear();
    return true;
}

}  // namespace videosc

