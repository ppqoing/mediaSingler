#include "StructuralVerificationCache.h"

#include <algorithm>
#include <exception>
#include <utility>

namespace videosc::dedup {
namespace {

/** @brief 把报告的原子取消状态适配为 VideoSc C 回调。 */
int __cdecl ShouldCancelStructure(void* context) {
    const auto* cancelled = static_cast<const std::atomic_bool*>(context);
    return cancelled != nullptr && cancelled->load(std::memory_order_relaxed) ? 1 : 0;
}

}  // namespace

CachedImageStructure::CachedImageStructure(VideoScImageStructureResultV1 result) noexcept
    : result_(result) {}

CachedImageStructure::~CachedImageStructure() {
    FreeVideoScImageStructureResultV1(&result_);
}

const void* CachedImageStructure::native_handle() const noexcept {
    return result_.structureHandle;
}

StructuralVerificationCache::StructuralVerificationCache(
    const std::size_t maximum_bytes,
    const std::uint32_t ffmpeg_threads,
    const std::uint32_t timeout_milliseconds,
    const std::atomic_bool& cancel_requested)
    : maximum_bytes_((std::max)(maximum_bytes, kEstimatedStructureBytes)),
      ffmpeg_threads_((std::max)(1U, ffmpeg_threads)),
      timeout_milliseconds_(timeout_milliseconds),
      cancel_requested_(cancel_requested) {}

StructuralCacheLookup StructuralVerificationCache::Get(const std::string& sha512_hex,
                                                        const std::string& image_path_utf8) {
    return Get(sha512_hex, {{image_path_utf8, {}}});
}

StructuralCacheLookup StructuralVerificationCache::Get(
    const std::string& sha512_hex,
    const std::vector<StructuralPathCandidate>& candidates) {
    std::shared_future<StructuralCacheLookup> sharedLoad;
    std::shared_ptr<std::promise<StructuralCacheLookup>> ownerPromise;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto cached = cache_.find(sha512_hex);
        if (cached != cache_.end()) {
            lru_.splice(lru_.begin(), lru_, cached->second.lru_position);
            cached->second.lru_position = lru_.begin();
            hits_.fetch_add(1, std::memory_order_relaxed);
            return {cached->second.structure, {}, StructuralLoadStatus::Succeeded, {}};
        }
        const auto pending = pending_.find(sha512_hex);
        if (pending != pending_.end()) {
            sharedLoad = pending->second;
            shared_waits_.fetch_add(1, std::memory_order_relaxed);
        } else {
            ownerPromise = std::make_shared<std::promise<StructuralCacheLookup>>();
            sharedLoad = ownerPromise->get_future().share();
            pending_.emplace(sha512_hex, sharedLoad);
            misses_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    if (!ownerPromise) return sharedLoad.get();

    StructuralCacheLookup loaded;
    try {
        loaded = Load(candidates);
    } catch (const std::exception& exception) {
        loaded.error = exception.what();
    } catch (...) {
        loaded.error = "Unknown image structure cache failure";
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.erase(sha512_hex);
        if (loaded.structure) {
            lru_.push_front(sha512_hex);
            cache_.insert_or_assign(sha512_hex, CacheEntry{loaded.structure, lru_.begin()});
            cached_bytes_ += kEstimatedStructureBytes;
            EvictLocked();
        }
    }
    ownerPromise->set_value(loaded);
    return loaded;
}

StructuralCacheStats StructuralVerificationCache::stats() const noexcept {
    return {hits_.load(std::memory_order_relaxed),
            misses_.load(std::memory_order_relaxed),
            shared_waits_.load(std::memory_order_relaxed),
            evictions_.load(std::memory_order_relaxed),
            decodes_.load(std::memory_order_relaxed),
            path_retries_.load(std::memory_order_relaxed),
            timeouts_.load(std::memory_order_relaxed),
            decode_failures_.load(std::memory_order_relaxed)};
}

StructuralCacheLookup StructuralVerificationCache::Load(
    const std::vector<StructuralPathCandidate>& candidates) {
    if (cancel_requested_.load(std::memory_order_relaxed)) {
        return {{}, "cancelled", StructuralLoadStatus::Cancelled, {}};
    }
    if (candidates.empty()) {
        return {{}, "no active image path", StructuralLoadStatus::NoReadablePath, {}};
    }
    StructuralCacheLookup terminal{{}, "no readable image path", StructuralLoadStatus::NoReadablePath, {}};
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        if (cancel_requested_.load(std::memory_order_relaxed)) {
            return {{}, "cancelled", StructuralLoadStatus::Cancelled, {}};
        }
        if (index != 0) path_retries_.fetch_add(1, std::memory_order_relaxed);
        VideoScImageStructureOptionsV1 options{};
        options.structSize = sizeof(options);
        options.ffmpegThreadCount = ffmpeg_threads_;
        options.noProgressTimeoutMilliseconds = timeout_milliseconds_;
        options.shouldCancel = ShouldCancelStructure;
        options.cancelContext = const_cast<std::atomic_bool*>(&cancel_requested_);
        VideoScImageStructureResultV1 result{};
        result.structSize = sizeof(result);
        decodes_.fetch_add(1, std::memory_order_relaxed);
        if (LoadImageStructureV1(candidates[index].image_path_utf8.c_str(), &options, &result) != 0) {
            return {std::make_shared<CachedImageStructure>(result),
                    {},
                    StructuralLoadStatus::Succeeded,
                    candidates[index].image_path_utf8};
        }
        const std::string error = result.errorMessage == nullptr ? "Cannot load image structure" :
                                                                    result.errorMessage;
        StructuralLoadStatus status = StructuralLoadStatus::DecodeFailed;
        if (result.statusCode == VIDEOSC_ERR_MEDIA_CANCELLED) {
            status = StructuralLoadStatus::Cancelled;
        } else if (result.statusCode == VIDEOSC_ERR_MEDIA_TIMEOUT) {
            status = StructuralLoadStatus::TimedOut;
            timeouts_.fetch_add(1, std::memory_order_relaxed);
        } else if (result.statusCode == VIDEOSC_ERR_OPEN_FAILED) {
            status = StructuralLoadStatus::OpenFailed;
        } else {
            decode_failures_.fetch_add(1, std::memory_order_relaxed);
        }
        terminal = {{}, error, status, candidates[index].image_path_utf8};
        FreeVideoScImageStructureResultV1(&result);
        if (status == StructuralLoadStatus::Cancelled) return terminal;
    }
    return terminal;
}

void StructuralVerificationCache::EvictLocked() {
    while (cached_bytes_ > maximum_bytes_ && cache_.size() > 1 && !lru_.empty()) {
        const std::string key = lru_.back();
        lru_.pop_back();
        if (cache_.erase(key) != 0) {
            cached_bytes_ -= kEstimatedStructureBytes;
            evictions_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

}  // namespace videosc::dedup
