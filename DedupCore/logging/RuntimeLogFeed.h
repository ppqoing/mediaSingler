#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace videosc::dedup {

/** @brief 实时日志级别；文件日志语义不由该枚举改变。 */
enum class RuntimeLogSeverity {
    Info,
    Warning,
    Error,
};

/**
 * @brief 一条可由后台线程发布、由 GUI 增量读取的脱敏运行日志。
 *
 * entry_id 标识同一聚合条目，sequence 标识最近一次更新顺序。高频同类错误更新
 * 原条目的 repeat_count，而不会让窗口内存随重复次数增长。
 */
struct RuntimeLogEntry {
    /** @brief 聚合条目的进程内稳定 ID。 */
    std::uint64_t entry_id = 0;
    /** @brief 每次新增或聚合更新都会递增的进程内序号。 */
    std::uint64_t sequence = 0;
    /** @brief 最近一次发生时间，Unix UTC 毫秒。 */
    std::int64_t utc_ms = 0;
    RuntimeLogSeverity severity = RuntimeLogSeverity::Info;
    std::uint64_t task_id = 0;
    std::string task;
    std::string stage;
    std::string operation;
    std::uint32_t status_code = 0;
    std::int64_t native_error = 0;
    /** @brief 路径、SHA-512 或候选对等可复制定位信息。 */
    std::string subject;
    std::string message;
    /** @brief 同一聚合键连续高频出现的累计次数。 */
    std::uint64_t repeat_count = 1;
};

/** @brief 一次增量读取结果及有界队列淘汰统计。 */
struct RuntimeLogSnapshot {
    std::vector<RuntimeLogEntry> entries;
    std::uint64_t latest_sequence = 0;
    std::uint64_t dropped_entries = 0;
};

/**
 * @brief 进程级、有界、线程安全的实时日志镜像。
 *
 * 该类型不写文件、不调用其他日志器，也不把自身故障传播给业务线程。落盘审计继续由
 * ExecutionLogger、ScanErrorLogger 和 ApplicationErrorLogger 负责。
 */
class RuntimeLogFeed final {
public:
    /** @return 进程内最多保留的聚合日志条目数。 */
    static constexpr std::size_t Capacity() noexcept { return 4096; }

    /**
     * @brief 发布或聚合一条运行日志。
     * @param entry 已脱敏的日志内容；时间为零时自动使用当前 UTC 时间。
     *
     * 任何内存或同步异常都会在内部吞掉，不能改变原业务结果。
     */
    static void Publish(RuntimeLogEntry entry) noexcept;

    /**
     * @brief 读取指定序号之后新增或更新的日志条目。
     * @param after_sequence GUI 上次成功消费的最新序号。
     * @return 按更新顺序排列的增量快照；读取失败时返回空快照且不推进序号。
     */
    static RuntimeLogSnapshot SnapshotSince(std::uint64_t after_sequence) noexcept;

    /** @brief 仅供确定性测试清空进程内实时日志，不删除任何文件日志。 */
    static void ResetForTests() noexcept;
};

}  // namespace videosc::dedup
