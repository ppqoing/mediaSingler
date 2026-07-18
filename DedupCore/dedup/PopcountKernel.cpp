#include "PopcountKernel.h"

#include <intrin.h>

namespace videosc::dedup {
namespace {

/** @brief 启动后只执行一次 CPUID，避免热点路径重复探测。 */
bool DetectHardwarePopcnt() noexcept {
    int registers[4]{};
    __cpuid(registers, 1);
    return (registers[2] & (1 << 23)) != 0;
}

/** @brief 不依赖扩展指令集的 64 位 SWAR 位计数。 */
std::uint32_t SoftwarePopcount64(std::uint64_t value) noexcept {
    value -= (value >> 1) & 0x5555555555555555ULL;
    value = (value & 0x3333333333333333ULL) + ((value >> 2) & 0x3333333333333333ULL);
    value = (value + (value >> 4)) & 0x0f0f0f0f0f0f0f0fULL;
    return static_cast<std::uint32_t>((value * 0x0101010101010101ULL) >> 56);
}

}  // namespace

std::uint32_t PopcountKernel::Count64(const std::uint64_t value,
                                      const bool force_scalar) noexcept {
    static const bool hardware = DetectHardwarePopcnt();
    return hardware && !force_scalar ? static_cast<std::uint32_t>(__popcnt64(value))
                                     : SoftwarePopcount64(value);
}

bool PopcountKernel::HasHardwarePopcnt() noexcept {
    static const bool hardware = DetectHardwarePopcnt();
    return hardware;
}

}  // namespace videosc::dedup
