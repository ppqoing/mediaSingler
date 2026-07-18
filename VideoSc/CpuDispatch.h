#pragma once

#include <cstdint>

namespace videosc {

/**
 * @brief VideoSc DLL 内部的运行时 CPU 位计数分派。
 *
 * 该类型不跨 DLL ABI 暴露；静态画面识别和兼容汉明距离 API 共用它，保证旧 CPU 不会
 * 无条件执行 POPCNT 指令。
 */
class CpuDispatch final {
public:
    /** @param value 待计数值。 @return 0 到 64 的置位数量。 */
    static std::uint32_t Popcount64(std::uint64_t value) noexcept;

    /** @param value 待计数值。 @return 0 到 32 的置位数量。 */
    static std::uint32_t Popcount32(std::uint32_t value) noexcept;

    /** @return 当前 CPU 是否支持 POPCNT。 */
    static bool HasHardwarePopcnt() noexcept;
};

}  // namespace videosc
