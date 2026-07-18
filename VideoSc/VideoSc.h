// VideoSc.h : Public export API of VideoSc DLL
//
// Feature: Save n screenshots from a video. Screenshots are taken at intervals
//          of k = t/(n+1), where t is the video duration. The long edge of each
//          screenshot is scaled to not exceed maxLongEdge, preserving the
//          original aspect ratio. Key-frame seeking (av_seek_frame) is used to
//          reduce decoding cost. dHash is computed in-place for each frame.
//
// dHash spec:
//   - Image is scaled to 9x8 grayscale
//   - For each of 8 rows, compare 8 adjacent pixel pairs (left < right -> 1)
//   - 8 x 8 = 64 bits, output as 16 hex chars (big-endian, MSB first)

#ifndef VIDEOSC_H
#define VIDEOSC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef VIDEOSC_EXPORTS
#define VIDEOSC_API __declspec(dllexport)
#else
#define VIDEOSC_API __declspec(dllimport)
#endif

// Status codes
#define VIDEOSC_OK                 0
#define VIDEOSC_ERR_INVALID_ARG    1
#define VIDEOSC_ERR_OPEN_FAILED    2
#define VIDEOSC_ERR_NO_VIDEO_STREAM 3
#define VIDEOSC_ERR_NO_DECODER     4
#define VIDEOSC_ERR_NO_DURATION    5
#define VIDEOSC_ERR_SWS_INIT       6
#define VIDEOSC_ERR_MEDIA_TIMEOUT  7
#define VIDEOSC_ERR_MEDIA_CANCELLED 8
#define VIDEOSC_ERR_CONTACT_SHEET  9
#define VIDEOSC_ERR_DECODE_FAILED  10
#define VIDEOSC_ERR_OUT_OF_MEMORY  11
#define VIDEOSC_ERR_UNEXPECTED_FAILURE 12

// Result of CaptureVideoScreenshots.
// All strings (errorMessage, thumbnailPaths[i], thumbnailDHashes[i]) are
// heap-allocated by the DLL and must be released via FreeVideoScResult.
typedef struct {
    int          statusCode;       // 0 = success, see VIDEOSC_ERR_* otherwise
    const char*  errorMessage;     // UTF-8, nullptr when statusCode == 0
    double       duration;         // video duration in seconds (-1.0 if unknown)
    int          thumbnailCount;   // actual number of thumbnails generated
    const char** thumbnailPaths;   // array of full UTF-8 paths (length = thumbnailCount)
    const char** thumbnailDHashes; // array of dHash hex strings (length = thumbnailCount, 64 chars each)
} VideoScResult;

// Capture video screenshots and compute dHash for each.
//
// Parameters:
//   videoPath        Video file path (UTF-8)
//   outputDir        Output directory for screenshots (UTF-8; must exist)
//   screenshotCount  Number of screenshots n (>=1)
//   maxLongEdge      Max long-edge length in pixels (>0; <=0 keeps original)
//   screenshotNames  Array of screenshot file names (UTF-8, length =
//                    screenshotCount; file name only, e.g. "shot_1.jpg";
//                    will be joined with outputDir to form full path)
//
// Returns a VideoScResult. Caller MUST call FreeVideoScResult on it.
VIDEOSC_API VideoScResult __cdecl CaptureVideoScreenshots(
    const char*  videoPath,
    const char*  outputDir,
    int          screenshotCount,
    int          maxLongEdge,
    const char** screenshotNames
);

// Free a VideoScResult returned by CaptureVideoScreenshots.
// Safe to call on a zero-initialized struct.
VIDEOSC_API void __cdecl FreeVideoScResult(VideoScResult* result);

// -----------------------------------------------------------------------------
// Standalone hash APIs
// -----------------------------------------------------------------------------

// Compute SHA-512 of a file.
//
// Parameters:
//   filePath      UTF-8 file path.
//   outHash       Output buffer, must be at least 129 bytes (128 hex chars + NUL).
//   outHashSize   Size of outHash buffer in bytes (>= 129).
//
// Returns: 1 on success, 0 on failure.
VIDEOSC_API int __cdecl ComputeFileSHA512(
    const char* filePath,
    char*       outHash,
    int         outHashSize
);

/** @brief 流式 SHA-512 状态码；失败时不会返回部分摘要。 */
#define VIDEOSC_HASH_OK                 0
#define VIDEOSC_HASH_INVALID_ARGUMENT   1
#define VIDEOSC_HASH_OPEN_FAILED        2
#define VIDEOSC_HASH_READ_FAILED        3
#define VIDEOSC_HASH_READ_TIMEOUT       4
#define VIDEOSC_HASH_CANCELLED          5
#define VIDEOSC_HASH_FILE_CHANGED       6
#define VIDEOSC_HASH_CRYPTO_FAILED      7
#define VIDEOSC_HASH_OUT_OF_MEMORY      8
#define VIDEOSC_HASH_UNEXPECTED_FAILURE 9

/** @brief 流式读取块、坏块重试和无进展超时配置。 */
typedef struct VideoScHashOptions {
    uint32_t structSize;
    uint32_t readBlockBytes;
    uint32_t normalBlockRetries;
    uint32_t smallBlockBytes;
    uint32_t smallBlockRetries;
    uint32_t noProgressTimeoutMilliseconds;
} VideoScHashOptions;

/** @brief 流式 SHA-512 结果，failedOffset 可直接写入坏块日志。 */
typedef struct VideoScFileHashResult {
    int      statusCode;
    uint32_t win32Error;
    uint64_t fileSize;
    uint64_t bytesRead;
    uint64_t failedOffset;
    char     sha512Hex[129];
    uint64_t volumeSerial;
    uint64_t fileIdHigh;
    uint64_t fileIdLow;
    int64_t  creationTimeUtcMs;
    int64_t  lastWriteTimeUtcMs;
} VideoScFileHashResult;

/**
 * @brief 可选取消回调，由调用方使用线程安全状态实现。
 * @param context 调用 ComputeFileSHA512Ex 时传入的上下文。
 * @return 非零表示取消当前文件；零表示继续。
 */
typedef int(__cdecl* VideoScShouldCancel)(void* context);

/**
 * @brief 使用固定容量缓冲和可取消 Overlapped I/O 流式计算完整 SHA-512。
 * @param filePath UTF-8 文件路径。
 * @param options 读取与超时设置；为空时使用 1 MiB/64 KiB/60 秒默认值。
 * @param shouldCancel 可选线程安全取消回调。
 * @param cancelContext 原样传给 shouldCancel 的上下文。
 * @param outResult 必须非空；失败时包含状态、Win32 错误和失败偏移，摘要保持为空。
 * @return 成功完成完整文件并通过文件变化复核时返回 1，否则返回 0。
 */
VIDEOSC_API int __cdecl ComputeFileSHA512Ex(
    const char*               filePath,
    const VideoScHashOptions* options,
    VideoScShouldCancel       shouldCancel,
    void*                     cancelContext,
    VideoScFileHashResult*    outResult
);

/** @brief 媒体类型提示；调用方按扩展名分类，避免把音频封面误判为图片或视频。 */
#define VIDEOSC_MEDIA_OTHER 0
#define VIDEOSC_MEDIA_IMAGE 1
#define VIDEOSC_MEDIA_VIDEO 2
#define VIDEOSC_MEDIA_AUDIO 3

/** @brief 图片/视频内容分析选项。 */
typedef struct VideoScMediaOptions {
    uint32_t structSize;
    int      mediaKindHint;
    uint32_t contactSheetCellLongEdge;
    uint32_t ffmpegThreadCount;
    uint32_t noProgressTimeoutMilliseconds;
    VideoScShouldCancel shouldCancel;
    void*    cancelContext;
    const char* contactSheetPath;
} VideoScMediaOptions;

/**
 * @brief 媒体内容分析结果。
 *
 * 视频固定保存六个等间隔 dHash 和一张 2x3 拼图；图片只保存一个 dHash；音频不生成内容哈希。
 * 所有字符串由 DLL 分配，调用方必须使用 FreeVideoScMediaResult 释放。
 */
typedef struct VideoScMediaResult {
    int      statusCode;
    uint32_t nativeError;
    const char* errorMessage;
    int      mediaKind;
    int64_t  durationMilliseconds;
    uint32_t width;
    uint32_t height;
    double   frameRate;
    uint64_t bitrate;
    int      hasImageDHash;
    uint64_t imageDHash;
    int      hasVideoDHashes;
    uint64_t videoDHashes[6];
    int      staticVisual;
    const char* mimeType;
    const char* containerName;
    const char* videoCodec;
    const char* pixelFormat;
    const char* contactSheetPath;
} VideoScMediaResult;

/**
 * @brief 分析媒体元数据和内容指纹。
 * @return 成功返回 1；失败返回 0，并在 outResult 中提供状态。单文件失败不应中止扫描任务。
 */
VIDEOSC_API int __cdecl AnalyzeMediaFile(
    const char* videoPath,
    const VideoScMediaOptions* options,
    VideoScMediaResult* outResult
);

/** @brief 释放 AnalyzeMediaFile 返回的所有 DLL 字符串并清零结果。 */
VIDEOSC_API void __cdecl FreeVideoScMediaResult(VideoScMediaResult* result);

/** @brief 当前图片感知特征接口版本。 */
#define VIDEOSC_IMAGE_PERCEPTUAL_ALGORITHM_VERSION 1
/** @brief PDQ-256 的固定字节数。 */
#define VIDEOSC_PDQ_HASH_BYTES 32
/** @brief 4×4 分区 pHash 的固定数量。 */
#define VIDEOSC_ZONED_PHASH_COUNT 16

/**
 * @brief 图片感知特征 V1 选项。
 *
 * 调用方必须把 structSize 设置为 sizeof(VideoScImageFeatureOptionsV1)。
 */
typedef struct VideoScImageFeatureOptionsV1 {
    uint32_t structSize;
    uint32_t ffmpegThreadCount;
    uint32_t noProgressTimeoutMilliseconds;
    VideoScShouldCancel shouldCancel;
    void* cancelContext;
} VideoScImageFeatureOptionsV1;

/**
 * @brief 图片元数据、兼容 dHash、PDQ-256 与 4×4 分区 pHash 的单次解码结果。
 *
 * 调用前必须设置 structSize；所有字符串由 DLL 分配，调用方使用
 * FreeVideoScImageFeatureResultV1 统一释放。
 */
typedef struct VideoScImageFeatureResultV1 {
    uint32_t structSize;
    int statusCode;
    uint32_t nativeError;
    const char* errorMessage;
    uint32_t width;
    uint32_t height;
    int hasImageDHash;
    uint64_t imageDHash;
    int hasPdqHash;
    uint8_t pdqHash[VIDEOSC_PDQ_HASH_BYTES];
    uint8_t pdqQuality;
    int hasZonedPHashes;
    uint64_t zonedPHashes[VIDEOSC_ZONED_PHASH_COUNT];
    uint32_t perceptualAlgorithmVersion;
    const char* mimeType;
    const char* containerName;
    const char* imageCodec;
    const char* pixelFormat;
} VideoScImageFeatureResultV1;

/**
 * @brief 单次解码图片并生成元数据、dHash、PDQ 与 16 个分区 pHash。
 * @param imagePath UTF-8 图片路径。
 * @param options 带大小的 V1 选项。
 * @param outResult 调用前设置 structSize 的 V1 输出。
 * @return 全部特征成功返回 1；失败返回 0 并填写状态和错误消息。
 */
VIDEOSC_API int __cdecl AnalyzeImagePerceptualFeaturesV1(
    const char* imagePath,
    const VideoScImageFeatureOptionsV1* options,
    VideoScImageFeatureResultV1* outResult);

/** @brief 释放图片感知特征结果中的 DLL 字符串并清零结果。 */
VIDEOSC_API void __cdecl FreeVideoScImageFeatureResultV1(VideoScImageFeatureResultV1* result);

/** @brief 当前结构直验算法版本。 */
#define VIDEOSC_IMAGE_STRUCTURAL_ALGORITHM_VERSION 1

/** @brief 报告期图片结构面加载选项。 */
typedef struct VideoScImageStructureOptionsV1 {
    uint32_t structSize;
    uint32_t ffmpegThreadCount;
    uint32_t noProgressTimeoutMilliseconds;
    VideoScShouldCancel shouldCancel;
    void* cancelContext;
} VideoScImageStructureOptionsV1;

/**
 * @brief DLL 拥有的图片结构面句柄。
 *
 * structureHandle 只允许传给 CompareImageStructuresV1；调用方必须通过
 * FreeVideoScImageStructureResultV1 释放，不得直接 delete。
 */
typedef struct VideoScImageStructureResultV1 {
    uint32_t structSize;
    int statusCode;
    uint32_t nativeError;
    const char* errorMessage;
    uint32_t width;
    uint32_t height;
    uint32_t structuralAlgorithmVersion;
    void* structureHandle;
} VideoScImageStructureResultV1;

/** @brief 结构面比较选项；blockPassScore 取值必须在 0～1。 */
typedef struct VideoScImageStructureCompareOptionsV1 {
    uint32_t structSize;
    double blockPassScore;
} VideoScImageStructureCompareOptionsV1;

/** @brief 结构直验量化结果；百万分数范围固定为 0～1,000,000。 */
typedef struct VideoScImageStructureCompareResultV1 {
    uint32_t structSize;
    int statusCode;
    const char* errorMessage;
    uint32_t globalEdgeZnccMillionths;
    uint32_t trimmedBlockScoreMillionths;
    uint32_t passingBlockPercentMillionths;
    uint32_t comparedBlockCount;
} VideoScImageStructureCompareResultV1;

/**
 * @brief 解码图片并创建报告期可缓存的 256×256 灰度/Sobel 结构面。
 * @param imagePath UTF-8 图片路径。
 * @param options 带大小的结构面加载选项。
 * @param outResult 调用前设置 structSize 的结构面结果。
 * @return 成功返回 1；失败返回 0。
 */
VIDEOSC_API int __cdecl LoadImageStructureV1(
    const char* imagePath,
    const VideoScImageStructureOptionsV1* options,
    VideoScImageStructureResultV1* outResult);

/**
 * @brief 比较两个相同算法版本的结构面。
 * @param leftHandle 左侧非空结构句柄。
 * @param rightHandle 右侧非空结构句柄。
 * @param options 带大小的块阈值选项。
 * @param outResult 调用前设置 structSize 的比较结果。
 * @return 比较成功返回 1；失败返回 0。
 */
VIDEOSC_API int __cdecl CompareImageStructuresV1(
    const void* leftHandle,
    const void* rightHandle,
    const VideoScImageStructureCompareOptionsV1* options,
    VideoScImageStructureCompareResultV1* outResult);

/** @brief 释放结构面句柄与错误字符串并清零结果。 */
VIDEOSC_API void __cdecl FreeVideoScImageStructureResultV1(VideoScImageStructureResultV1* result);

/** @brief 释放结构比较错误字符串并清零结果。 */
VIDEOSC_API void __cdecl FreeVideoScImageStructureCompareResultV1(
    VideoScImageStructureCompareResultV1* result);

/** @brief 图片详情预览生成选项；输出文件扩展名决定 PNG 或 JPEG 编码。 */
typedef struct VideoScImagePreviewOptions {
    uint32_t structSize;
    uint32_t maximumLongEdge;
    uint32_t ffmpegThreadCount;
    uint32_t noProgressTimeoutMilliseconds;
    VideoScShouldCancel shouldCancel;
    void* cancelContext;
} VideoScImagePreviewOptions;

/** @brief 图片详情预览生成结果；字符串必须通过 FreeVideoScImagePreviewResult 释放。 */
typedef struct VideoScImagePreviewResult {
    int statusCode;
    uint32_t nativeError;
    uint32_t width;
    uint32_t height;
    const char* errorMessage;
    const char* outputPath;
} VideoScImagePreviewResult;

/**
 * @brief 解码图片首帧、按最长边等比例缩放并写入独立临时缩略图。
 * @param sourcePath UTF-8 原始图片路径。
 * @param outputPath UTF-8 输出缩略图路径；扩展名应为 .png、.jpg 或 .jpeg。
 * @param options 缩放、线程、超时与取消选项。
 * @param outResult 接收结构化状态、实际尺寸和输出路径。
 * @return 成功生成完整缩略图返回 1，否则返回 0。
 */
VIDEOSC_API int __cdecl GenerateImagePreview(
    const char* sourcePath,
    const char* outputPath,
    const VideoScImagePreviewOptions* options,
    VideoScImagePreviewResult* outResult
);

/** @brief 释放 GenerateImagePreview 返回的 DLL 字符串并清零结果。 */
VIDEOSC_API void __cdecl FreeVideoScImagePreviewResult(VideoScImagePreviewResult* result);

// Compute dHash (difference hash) of an image file.
//
// Spec:
//   - Scale image to 9x8 grayscale
//   - 8 rows x 8 comparisons = 64 bits
//   - Output as 16 hex chars (big-endian, MSB first)
//
// Parameters:
//   filePath      UTF-8 image file path.
//   outHash       Output buffer, must be at least 17 bytes (16 hex chars + NUL).
//   outHashSize   Size of outHash buffer in bytes (>= 17).
//
// Returns: 1 on success, 0 on failure.
VIDEOSC_API int __cdecl ComputeImageDHash(
    const char* filePath,
    char*       outHash,
    int         outHashSize
);

// Compute Hamming distance between two hex hash strings.
//
// Supports dHash (16 hex chars -> 64 bits) and SHA-512 (128 hex chars -> 512 bits).
// Hex string lengths must be equal and a multiple of 2 (i.e., whole bytes).
// Comparison is case-insensitive (uppercase / lowercase / mixed all accepted).
//
// Parameters:
//   hash1, hash2 - NUL-terminated hex strings.
//
// Returns: Hamming distance (bit count) on success, -1 on error
//          (null pointer, empty string, length mismatch, or invalid hex char).
VIDEOSC_API int __cdecl ComputeHammingDistance(
    const char* hash1,
    const char* hash2
);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // VIDEOSC_H
