#include "RuntimeLogFeed.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <mutex>
#include <utility>

namespace videosc::dedup {
namespace {

/** @brief 实时日志进程状态；只允许通过 FeedState() 获取。 */
struct RuntimeLogFeedState {
    std::mutex mutex;
    std::deque<RuntimeLogEntry> entries;
    std::uint64_t next_entry_id = 1;
    std::uint64_t next_sequence = 1;
    std::uint64_t dropped_entries = 0;
};

/** @return 进程内唯一实时日志状态。 */
RuntimeLogFeedState& FeedState() {
    static RuntimeLogFeedState state;
    return state;
}

/** @return 当前 Unix UTC 毫秒。 */
std::int64_t CurrentUtcMilliseconds() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/**
 * @brief 把多行文本压成单行，避免日志表格被控制字符破坏。
 * @param value 待处理文本。
 */
void NormalizeOneLine(std::string& value) noexcept {
    for (char& character : value) {
        if (character == '\r' || character == '\n' || character == '\t') character = ' ';
    }
}

/**
 * @brief 判断两条记录是否属于同一高频聚合类别。
 * @param left 已存在记录。
 * @param right 新记录。
 * @return 任务、阶段、操作和错误码语义一致时返回 true。
 */
bool HasSameAggregationKey(const RuntimeLogEntry& left,
                           const RuntimeLogEntry& right) noexcept {
    return left.severity == right.severity && left.task_id == right.task_id &&
           left.task == right.task && left.stage == right.stage &&
           left.operation == right.operation && left.status_code == right.status_code &&
           left.native_error == right.native_error;
}

}  // namespace

void RuntimeLogFeed::Publish(RuntimeLogEntry entry) noexcept {
    try {
        NormalizeOneLine(entry.task);
        NormalizeOneLine(entry.stage);
        NormalizeOneLine(entry.operation);
        NormalizeOneLine(entry.subject);
        NormalizeOneLine(entry.message);
        if (entry.utc_ms == 0) entry.utc_ms = CurrentUtcMilliseconds();
        if (entry.repeat_count == 0) entry.repeat_count = 1;

        RuntimeLogFeedState& state = FeedState();
        std::lock_guard<std::mutex> lock(state.mutex);

        // 只在最近的小窗口内寻找同类项，既能收敛突发刷屏，也避免每次发布线性扫描 4096 条。
        const std::size_t search_begin = state.entries.size() > 64 ? state.entries.size() - 64 : 0;
        for (std::size_t index = state.entries.size(); index > search_begin; --index) {
            const std::size_t current = index - 1;
            if (!HasSameAggregationKey(state.entries[current], entry)) continue;
            RuntimeLogEntry aggregated = std::move(state.entries[current]);
            state.entries.erase(state.entries.begin() + static_cast<std::ptrdiff_t>(current));
            aggregated.sequence = state.next_sequence++;
            aggregated.utc_ms = entry.utc_ms;
            aggregated.repeat_count += entry.repeat_count;
            state.entries.push_back(std::move(aggregated));
            return;
        }

        entry.entry_id = state.next_entry_id++;
        entry.sequence = state.next_sequence++;
        state.entries.push_back(std::move(entry));
        if (state.entries.size() > Capacity()) {
            state.entries.pop_front();
            ++state.dropped_entries;
        }
    } catch (...) {
        // 实时 UI 日志是诊断镜像，失败不得改变扫描、报告或文件日志语义。
    }
}

RuntimeLogSnapshot RuntimeLogFeed::SnapshotSince(const std::uint64_t after_sequence) noexcept {
    try {
        RuntimeLogFeedState& state = FeedState();
        std::lock_guard<std::mutex> lock(state.mutex);
        RuntimeLogSnapshot snapshot;
        snapshot.latest_sequence = state.next_sequence - 1;
        snapshot.dropped_entries = state.dropped_entries;
        for (const RuntimeLogEntry& entry : state.entries) {
            if (entry.sequence > after_sequence) snapshot.entries.push_back(entry);
        }
        return snapshot;
    } catch (...) {
        return {};
    }
}

void RuntimeLogFeed::ResetForTests() noexcept {
    try {
        RuntimeLogFeedState& state = FeedState();
        std::lock_guard<std::mutex> lock(state.mutex);
        state.entries.clear();
        state.next_entry_id = 1;
        state.next_sequence = 1;
        state.dropped_entries = 0;
    } catch (...) {
        // 测试清理失败不允许从 noexcept 边界逃逸。
    }
}

}  // namespace videosc::dedup
