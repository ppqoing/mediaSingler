#pragma once

#include "../config/AppConfig.h"
#include "../models/CoreModels.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>

namespace videosc::dedup {

/** @brief 单文件完整 SHA-512 的业务状态。 */
enum class FileHashStatus {
    Succeeded,
    InvalidArgument,
    OpenFailed,
    ReadFailed,
    ReadTimeout,
    Cancelled,
    FileChanged,
    CryptoFailed,
    UnexpectedFailure,
};

/** @brief 单文件哈希结果；非成功状态不包含摘要。 */
struct FileHashResult {
    FileHashStatus status = FileHashStatus::UnexpectedFailure;
    std::optional<Sha512Digest> sha512;
    std::uint64_t file_size = 0;
    std::uint64_t bytes_read = 0;
    std::uint64_t failed_offset = 0;
    std::uint32_t system_error = 0;
    FileIdentity identity;
    std::int64_t creation_time_utc_ms = 0;
    std::int64_t last_write_time_utc_ms = 0;
};

/**
 * @brief 可注入调度器的单文件哈希边界。
 *
 * 测试可提供内存实现，生产实现调用 VideoSc 流式 API；调度器不依赖 BCrypt 或 Win32 细节。
 */
class IFileHasher {
public:
    virtual ~IFileHasher() = default;

    /**
     * @brief 计算一个文件的完整 SHA-512。
     * @param path 文件路径。
     * @param cancel_requested 全局或任务取消状态。
     * @return 完整结果；取消或失败时不得包含部分摘要。
     */
    virtual FileHashResult Hash(const std::filesystem::path& path,
                                const std::atomic_bool& cancel_requested) = 0;
};

/** @brief 使用 VideoSc.dll Overlapped I/O 实现 IFileHasher。 */
class VideoScFileHasher final : public IFileHasher {
public:
    /**
     * @brief 固定当前任务的块大小、重试和超时设置。
     * @param io_options 已通过 ConfigValidator 校验的任务快照。
     */
    explicit VideoScFileHasher(IoConfig io_options);

    /** @copydoc IFileHasher::Hash */
    FileHashResult Hash(const std::filesystem::path& path,
                        const std::atomic_bool& cancel_requested) override;

private:
    IoConfig io_options_;
};

}  // namespace videosc::dedup
