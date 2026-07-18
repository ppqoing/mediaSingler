#include "ImageFeatureBackfillCoordinator.h"

#include "../models/CoreModelCodec.h"
#include "../persistence/MySqlSyncService.h"
#include "../persistence/SyncOperation.h"
#include "../../VideoSc/VideoSc.h"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <utility>
#include <vector>

namespace videosc::dedup {
namespace {

/** @brief 当前 UTC Unix 毫秒。 */
std::int64_t CurrentUtcMilliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/** @brief 严格 UTF-16 到 UTF-8 路径转换。 */
std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) throw std::runtime_error("Cannot encode backfill path as UTF-8");
    std::string result(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), length, nullptr, nullptr) != length) {
        throw std::runtime_error("Cannot encode backfill path as UTF-8");
    }
    return result;
}

/** @brief 把外部原子取消标志适配为 VideoSc C ABI 回调。 */
int __cdecl ShouldCancelImageBackfill(void* context) {
    const auto* cancelled = static_cast<const std::atomic_bool*>(context);
    return cancelled != nullptr && cancelled->load(std::memory_order_relaxed) ? 1 : 0;
}

/** @brief 将一次成功图片分析结果覆盖到既有内容记录。 */
void ApplyImageFeatures(const VideoScImageFeatureResultV1& result,
                        const std::string& algorithm_version,
                        ShaFileData& data) {
    data.media_kind = MediaKind::Image;
    data.mime_type = result.mimeType == nullptr ? "" : result.mimeType;
    data.container_name = result.containerName == nullptr ? "" : result.containerName;
    data.width = result.width;
    data.height = result.height;
    data.video_codec = result.imageCodec == nullptr ? "" : result.imageCodec;
    data.pixel_format = result.pixelFormat == nullptr ? "" : result.pixelFormat;
    if (result.hasImageDHash != 0) data.image_dhash = result.imageDHash;
    PdqHash256 pdq{};
    std::copy(std::begin(result.pdqHash), std::end(result.pdqHash), pdq.begin());
    data.image_pdq_hash = pdq;
    data.image_pdq_quality = result.pdqQuality;
    std::copy(std::begin(result.zonedPHashes), std::end(result.zonedPHashes),
              data.image_zoned_phashes.begin());
    data.has_image_zoned_phashes = true;
    data.image_perceptual_algorithm_version = result.perceptualAlgorithmVersion;
    data.image_structural_algorithm_version = VIDEOSC_IMAGE_STRUCTURAL_ALGORITHM_VERSION;
    data.media_algorithm_version = algorithm_version;
}

}  // namespace

ImageFeatureBackfillCoordinator::ImageFeatureBackfillCoordinator(
    RocksStore& store,
    MySqlClient& client,
    std::string algorithm_version,
    const ComputeConfig compute,
    const IoConfig io,
    const std::uint32_t sync_batch_size)
    : store_(store),
      client_(client),
      algorithm_version_(std::move(algorithm_version)),
      compute_(compute),
      io_(io),
      sync_batch_size_((std::max)(1U, sync_batch_size)) {}

ImageFeatureBackfillResult ImageFeatureBackfillCoordinator::Run(
    const std::atomic_bool& cancel_requested,
    const ProgressCallback& progress_callback) {
    ImageFeatureBackfillResult result;
    MySqlReadRepository repository(client_);
    ImageFeatureCompletenessSnapshot initial;
    const MySqlStatus counted = repository.CountImageFeatureCompleteness(algorithm_version_, initial);
    if (!counted.succeeded) {
        result.message = counted.message;
        return result;
    }
    if (initial.complete()) {
        result.succeeded = true;
        result.complete = true;
        result.completeness = initial;
        result.final_progress.total_images = initial.total_images;
        result.final_progress.initially_complete_images = initial.complete_images;
        result.final_progress.completed_images = initial.complete_images;
        result.message = "all image features are complete";
        return result;
    }

    ImageFeatureBackfillCheckpointStore checkpointStore(store_);
    ImageFeatureBackfillCheckpoint checkpoint;
    checkpoint.algorithm_version = algorithm_version_;
    checkpoint.total_images = initial.total_images;
    checkpoint.completed_images = initial.complete_images;
    checkpoint.started_utc_ms = CurrentUtcMilliseconds();
    checkpoint.updated_utc_ms = checkpoint.started_utc_ms;
    ImageFeatureBackfillProgress progress;
    progress.total_images = initial.total_images;
    progress.initially_complete_images = initial.complete_images;
    progress.completed_images = initial.complete_images;
    progress.remaining_images = initial.incomplete_images;
    MySqlSyncExecutor executor(client_);
    std::vector<SyncOperation> pendingOperations;
    std::vector<RocksMutation> pendingLocalWrites;
    std::string failure;

    /** 提交已经成功分析的内容；MySQL 先提交，避免本地检查点宣称未同步结果已完成。 */
    const auto flush = [&]() {
        if (pendingOperations.empty()) return true;
        const MySqlStatus applied = executor.Apply(pendingOperations);
        if (!applied.succeeded) {
            failure = applied.message;
            return false;
        }
        const RocksStatus saved = store_.WriteBatch(pendingLocalWrites, false);
        if (!saved.succeeded) {
            failure = saved.message;
            return false;
        }
        pendingOperations.clear();
        pendingLocalWrites.clear();
        checkpoint.updated_utc_ms = CurrentUtcMilliseconds();
        const RocksStatus checkpointed = checkpointStore.Save(checkpoint);
        if (!checkpointed.succeeded) {
            failure = checkpointed.message;
            return false;
        }
        return true;
    };

    const MySqlStatus streamed = repository.StreamIncompleteImageFeatureTargets(
        algorithm_version_,
        [&](ImageFeatureBackfillTarget&& target) {
            if (cancel_requested.load(std::memory_order_relaxed) || !failure.empty()) return false;
            progress.current_sha512 = Sha512ToHex(target.content.sha512);
            checkpoint.last_sha512 = progress.current_sha512;
            bool hasReadablePath = false;
            bool sawTimeout = false;
            bool succeeded = false;
            for (const FilePathRecord& path : target.active_paths) {
                if (cancel_requested.load(std::memory_order_relaxed)) return false;
                std::error_code fileError;
                if (!std::filesystem::is_regular_file(path.path, fileError)) continue;
                hasReadablePath = true;
                const std::string pathUtf8 = WideToUtf8(path.path.wstring());
                VideoScImageFeatureOptionsV1 options{};
                options.structSize = sizeof(options);
                options.ffmpegThreadCount = compute_.ffmpeg_threads_per_task;
                options.noProgressTimeoutMilliseconds = io_.no_progress_timeout_seconds * 1000U;
                options.shouldCancel = ShouldCancelImageBackfill;
                options.cancelContext = const_cast<std::atomic_bool*>(&cancel_requested);
                VideoScImageFeatureResultV1 image{};
                image.structSize = sizeof(image);
                const bool analyzed = AnalyzeImagePerceptualFeaturesV1(pathUtf8.c_str(), &options, &image) != 0;
                const bool valid = analyzed && image.hasPdqHash != 0 && image.hasZonedPHashes != 0 &&
                                   image.perceptualAlgorithmVersion == VIDEOSC_IMAGE_PERCEPTUAL_ALGORITHM_VERSION;
                if (valid) {
                    ApplyImageFeatures(image, algorithm_version_, target.content);
                    succeeded = true;
                    FreeVideoScImageFeatureResultV1(&image);
                    break;
                }
                sawTimeout = sawTimeout || image.statusCode == VIDEOSC_ERR_MEDIA_TIMEOUT;
                const bool cancelled = image.statusCode == VIDEOSC_ERR_MEDIA_CANCELLED;
                FreeVideoScImageFeatureResultV1(&image);
                if (cancelled || cancel_requested.load(std::memory_order_relaxed)) return false;
            }

            if (succeeded) {
                SyncOperation operation;
                operation.kind = SyncOperationKind::UpsertShaFileData;
                operation.sha_file_data = target.content;
                pendingOperations.push_back(std::move(operation));
                pendingLocalWrites.push_back(
                    {RocksColumnFamily::ShaFileData,
                     progress.current_sha512,
                     CoreModelCodec::SerializeShaFileData(target.content)});
                ++checkpoint.completed_images;
                ++progress.completed_images;
            } else {
                ++checkpoint.failed_images;
                ++progress.failed_images;
                if (!hasReadablePath) {
                    ++checkpoint.no_readable_path_images;
                    ++progress.no_readable_path_images;
                } else if (sawTimeout) {
                    ++checkpoint.timeout_images;
                    ++progress.timeout_images;
                } else {
                    ++checkpoint.decode_failed_images;
                    ++progress.decode_failed_images;
                }
            }
            const std::uint64_t processed = progress.completed_images + progress.failed_images;
            progress.remaining_images = processed >= progress.total_images
                                            ? 0
                                            : progress.total_images - processed;
            if (progress_callback) progress_callback(progress);
            if (pendingOperations.size() >= sync_batch_size_ && !flush()) return false;
            if (pendingOperations.empty() && (progress.failed_images % sync_batch_size_) == 0) {
                checkpoint.updated_utc_ms = CurrentUtcMilliseconds();
                const RocksStatus saved = checkpointStore.Save(checkpoint);
                if (!saved.succeeded) {
                    failure = saved.message;
                    return false;
                }
            }
            return true;
        });
    if (!streamed.succeeded && failure.empty()) failure = streamed.message;
    if (failure.empty() && !flush()) {
        result.final_progress = progress;
        result.message = failure;
        return result;
    }
    if (!failure.empty()) {
        result.final_progress = progress;
        result.message = failure;
        return result;
    }
    if (cancel_requested.load(std::memory_order_relaxed)) {
        checkpoint.updated_utc_ms = CurrentUtcMilliseconds();
        checkpointStore.Save(checkpoint);
        result.cancelled = true;
        result.final_progress = progress;
        result.message = "cancelled";
        return result;
    }

    const MySqlStatus recounted = repository.CountImageFeatureCompleteness(
        algorithm_version_, result.completeness);
    if (!recounted.succeeded) {
        result.final_progress = progress;
        result.message = recounted.message;
        return result;
    }
    checkpoint.finished = result.completeness.complete();
    checkpoint.updated_utc_ms = CurrentUtcMilliseconds();
    const RocksStatus finalCheckpoint = checkpointStore.Save(checkpoint);
    if (!finalCheckpoint.succeeded) {
        result.final_progress = progress;
        result.message = finalCheckpoint.message;
        return result;
    }
    result.succeeded = true;
    result.complete = result.completeness.complete();
    result.final_progress = progress;
    result.message = result.complete ? "ok" : "backfill finished with unresolved images";
    return result;
}

}  // namespace videosc::dedup
