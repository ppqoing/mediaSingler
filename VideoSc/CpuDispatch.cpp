#include "pch.h"
#include "CpuDispatch.h"

#include <intrin.h>

namespace videosc {
namespace {

/** @brief 通过 CPUID 检测 POPCNT，结果由调用入口静态缓存。 */
bool DetectHardwarePopcnt() noexcept {
    int registers[4]{};
    __cpuid(registers, 1);
    return (registers[2] & (1 << 23)) != 0;
}

/** @brief 64 位可移植 SWAR 位计数。 */
std::uint32_t SoftwarePopcount64(std::uint64_t value) noexcept {
    value -= (value >> 1) & 0x5555555555555555ULL;
    value = (value & 0x3333333333333333ULL) + ((value >> 2) & 0x3333333333333333ULL);
    value = (value + (value >> 4)) & 0x0f0f0f0f0f0f0f0fULL;
    return static_cast<std::uint32_t>((value * 0x0101010101010101ULL) >> 56);
}

}  // namespace

std::uint32_t CpuDispatch::Popcount64(const std::uint64_t value) noexcept {
    static const bool hardware = DetectHardwarePopcnt();
    return hardware ? static_cast<std::uint32_t>(__popcnt64(value))
                    : SoftwarePopcount64(value);
}

std::uint32_t CpuDispatch::Popcount32(const std::uint32_t value) noexcept {
    return Popcount64(value);
}

bool CpuDispatch::HasHardwarePopcnt() noexcept {
    static const bool hardware = DetectHardwarePopcnt();
    return hardware;
}

}  // namespace videosc
