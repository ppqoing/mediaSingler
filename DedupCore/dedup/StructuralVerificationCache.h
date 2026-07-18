#pragma once

#include "../../VideoSc/VideoSc.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <future>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace videosc::dedup {

/**
 * @brief 一个由 VideoSc DLL 拥有底层内存的只读结构面句柄。
 *
 * 该对象只能由 StructuralVerificationCache 创建；析构时调用 DLL 配套释放函数。
 */
class CachedImageStructure final {
public:
    explicit CachedImageStructure(VideoScImageStructureResultV1 result) noexcept;
    ~CachedImageStructure();
    CachedImageStructure(const CachedImageStructure&) = delete;
    CachedImageStructure& operator=(const CachedImageStructure&) = delete;

    /** @return 可传给 CompareImageStructuresV1 的非空句柄。 */
    const void* native_handle() const noexcept;

private:
    VideoScImageStructureResultV1 result_{};
};

/** @brief 结构面加载终态，用于区分算法拒绝与 I/O/解码故障。 */
enum class StructuralLoadStatus {
    Succeeded,
    NoReadablePath,
    OpenFailed,
    TimedOut,
    DecodeFailed,
    Cancelled,
};

/** @brief 一个 SHA 的结构面候选路径及物理存储键。 */
struct StructuralPathCandidate {
    std::string image_path_utf8;
    std::string storage_target_key_utf8;
};

/** @brief 一次缓存查找结果；失败时 structure 为空并返回稳定终态。 */
struct StructuralCacheLookup {
    std::shared_ptr<const CachedImageStructure> structure;
    std::string error;
    StructuralLoadStatus status = StructuralLoadStatus::DecodeFailed;
    std::string selected_path_utf8;
};

/** @brief 报告级结构缓存计数器快照。 */
struct StructuralCacheStats {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t shared_waits = 0;
    std::uint64_t evictions = 0;
    std::uint64_t decodes = 0;
    std::uint64_t path_retries = 0;
    std::uint64_t timeouts = 0;
    std::uint64_t decode_failures = 0;
};

/**
 * @brief 按内容 SHA 共享加载、按近似字节预算淘汰的报告级结构面 LRU。
 *
 * 相同 SHA 的并发请求共享一个 future，避免第三筛中同一图片重复解码。
 */
class StructuralVerificationCache final {
public:
    StructuralVerificationCache(std::size_t maximum_bytes,
                                std::uint32_t ffmpeg_threads,
                                std::uint32_t timeout_milliseconds,
                                const std::atomic_bool& cancel_requested);

    /**
     * @brief 获取或加载一个结构面。
     * @param sha512_hex 128 字符内容键。
     * @param image_path_utf8 当前可读取的 UTF-8 图片路径。
     * @return 成功时含共享结构面；失败时含稳定错误说明。
     */
    StructuralCacheLookup Get(const std::string& sha512_hex,
                              const std::string& image_path_utf8);

    /**
     * @brief 按顺序尝试一个 SHA 的多条 active 路径并缓存首个成功结构面。
     * @param sha512_hex 128 字符内容键。
     * @param candidates 已按报告稳定规则排序的路径候选。
     * @return 成功、超时、取消或终态解码分类。
     */
    StructuralCacheLookup Get(const std::string& sha512_hex,
                              const std::vector<StructuralPathCandidate>& candidates);

    /** @return 当前累计缓存计数器。 */
    StructuralCacheStats stats() const noexcept;

private:
    struct CacheEntry {
        std::shared_ptr<const CachedImageStructure> structure;
        std::list<std::string>::iterator lru_position;
    };

    /** @brief 真正调用 DLL 解码结构面的慢路径。 */
    StructuralCacheLookup Load(const std::vector<StructuralPathCandidate>& candidates);

    /** @brief 在锁内按预算从最近最少使用端淘汰已完成项。 */
    void EvictLocked();

    static constexpr std::size_t kEstimatedStructureBytes = 200U * 1024U;

    std::size_t maximum_bytes_ = 0;
    std::size_t cached_bytes_ = 0;
    std::uint32_t ffmpeg_threads_ = 1;
    std::uint32_t timeout_milliseconds_ = 0;
    const std::atomic_bool& cancel_requested_;
    mutable std::mutex mutex_;
    std::list<std::string> lru_;
    std::unordered_map<std::string, CacheEntry> cache_;
    std::unordered_map<std::string, std::shared_future<StructuralCacheLookup>> pending_;
    std::atomic<std::uint64_t> hits_{0};
    std::atomic<std::uint64_t> misses_{0};
    std::atomic<std::uint64_t> shared_waits_{0};
    std::atomic<std::uint64_t> evictions_{0};
    std::atomic<std::uint64_t> decodes_{0};
    std::atomic<std::uint64_t> path_retries_{0};
    std::atomic<std::uint64_t> timeouts_{0};
    std::atomic<std::uint64_t> decode_failures_{0};
};

}  // namespace videosc::dedup
