#pragma once

#include <cstdint>

namespace videosc::dedup {

/**
 * @brief 统一提供运行时 CPU 能力分派的位计数内核。
 *
 * 本类型不保存业务状态；图片 PDQ、分区 pHash 和旧 dHash 必须通过本入口，避免在不支持
 * POPCNT 的 CPU 上执行非法指令。
 */
class PopcountKernel final {
public:
    /**
     * @brief 计算 64 位值中置位数量。
     * @param value 待计数的 64 位值。
     * @param force_scalar 为 true 时强制使用可移植 SWAR 路径，供兼容模式和测试使用。
     * @return 0 到 64 的置位数量。
     */
    static std::uint32_t Count64(std::uint64_t value, bool force_scalar = false) noexcept;

    /**
     * @brief 查询当前进程是否检测到硬件 POPCNT。
     * @return CPU 与操作系统支持 POPCNT 时返回 true。
     */
    static bool HasHardwarePopcnt() noexcept;
};

}  // namespace videosc::dedup
