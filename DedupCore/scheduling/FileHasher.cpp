#include "FileHasher.h"

#include "VideoSc.h"

#include <limits>

namespace videosc::dedup {
namespace {

/**
 * @brief 将调度器原子取消状态适配为 VideoSc C 回调。
 * @param context 指向 std::atomic_bool。
 * @return 已请求取消返回 1，否则返回 0。
 */
int __cdecl ShouldCancelHash(void* context) {
    return static_cast<const std::atomic_bool*>(context)->load(std::memory_order_relaxed) ? 1 : 0;
}

/**
 * @brief 把 VideoSc C 状态码映射为核心业务状态。
 * @param status_code VIDEOSC_HASH_* 常量。
 * @return FileHashStatus。
 */
FileHashStatus MapHashStatus(const int status_code) {
    switch (status_code) {
        case VIDEOSC_HASH_OK:
            return FileHashStatus::Succeeded;
        case VIDEOSC_HASH_INVALID_ARGUMENT:
            return FileHashStatus::InvalidArgument;
        case VIDEOSC_HASH_OPEN_FAILED:
            return FileHashStatus::OpenFailed;
        case VIDEOSC_HASH_READ_FAILED:
            return FileHashStatus::ReadFailed;
        case VIDEOSC_HASH_READ_TIMEOUT:
            return FileHashStatus::ReadTimeout;
        case VIDEOSC_HASH_CANCELLED:
            return FileHashStatus::Cancelled;
        case VIDEOSC_HASH_FILE_CHANGED:
            return FileHashStatus::FileChanged;
        case VIDEOSC_HASH_CRYPTO_FAILED:
            return FileHashStatus::CryptoFailed;
        default:
            return FileHashStatus::UnexpectedFailure;
    }
}

}  // namespace

VideoScFileHasher::VideoScFileHasher(IoConfig io_options) : io_options_(std::move(io_options)) {}

FileHashResult VideoScFileHasher::Hash(const std::filesystem::path& path,
                                       const std::atomic_bool& cancel_requested) {
    const std::string utf8_path = path.u8string();
    VideoScHashOptions options{};
    options.structSize = sizeof(options);
    options.readBlockBytes = io_options_.read_block_kib * 1024U;
    options.normalBlockRetries = io_options_.normal_block_retries;
    options.smallBlockBytes = io_options_.small_block_kib * 1024U;
    options.smallBlockRetries = io_options_.small_block_retries;
    options.noProgressTimeoutMilliseconds = io_options_.no_progress_timeout_seconds * 1000U;

    VideoScFileHashResult native_result{};
    ComputeFileSHA512Ex(utf8_path.c_str(),
                        &options,
                        ShouldCancelHash,
                        const_cast<std::atomic_bool*>(&cancel_requested),
                        &native_result);

    FileHashResult result;
    result.status = MapHashStatus(native_result.statusCode);
    result.file_size = native_result.fileSize;
    result.bytes_read = native_result.bytesRead;
    result.failed_offset = native_result.failedOffset;
    result.system_error = native_result.win32Error;
    result.identity.volume_serial = native_result.volumeSerial;
    result.identity.file_id_high = native_result.fileIdHigh;
    result.identity.file_id_low = native_result.fileIdLow;
    result.creation_time_utc_ms = native_result.creationTimeUtcMs;
    result.last_write_time_utc_ms = native_result.lastWriteTimeUtcMs;
    if (result.status == FileHashStatus::Succeeded) {
        result.sha512 = Sha512FromHex(native_result.sha512Hex);
        if (!result.sha512.has_value()) {
            result.status = FileHashStatus::CryptoFailed;
        }
    }
    return result;
}

}  // namespace videosc::dedup
