#include "pch.h"
// dllmain.cpp : DLL entry point and video screenshot implementation.
#include "VideoSc.h"
#include "CpuDispatch.h"
#include "ImagePerceptualHash.h"
#include "ImageStructuralPlane.h"

// FFmpeg headers (C interface, wrapped with extern "C" in C++)
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>
}

#include <string>
#include <cstring>
#include <vector>
#include <cstdint>
#include <cstdlib>
#include <algorithm>
#include <array>
#include <limits>
#include <chrono>
#include <filesystem>
#include <exception>
#include <new>
#include <intrin.h>

// Windows BCrypt for SHA-512
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

// Link FFmpeg import libraries
#pragma comment(lib, "avformat.lib")
#pragma comment(lib, "avcodec.lib")
#pragma comment(lib, "swscale.lib")
#pragma comment(lib, "avutil.lib")

// dHash dimensions: 9 columns x 8 rows -> 64 bits -> 16 hex chars
#define DHASH_W 9
#define DHASH_H 8
#define DHASH_HEX_LEN 16  // 64 / 4

// ---------------------------------------------------------------------------
// Helper: UTF-8 string -> UTF-16 wstring
// ---------------------------------------------------------------------------
static std::wstring Utf8ToWide(const char* utf8) {
    if (!utf8 || !*utf8) return L"";
    const int sourceLength = static_cast<int>(strlen(utf8));
    int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, sourceLength, nullptr, 0);
    if (len <= 0) return L"";
    std::wstring wide(len, L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, sourceLength, wide.data(), len) != len) {
        return L"";
    }
    return wide;
}

// ---------------------------------------------------------------------------
// Helper: strdup a UTF-8 string with DLL-local malloc (for cross-DLL free)
// ---------------------------------------------------------------------------
static char* DupString(const char* s) {
    if (!s) return nullptr;
    size_t n = strlen(s) + 1;
    char* p = (char*)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

// ---------------------------------------------------------------------------
// Helper: configure sws color range to silence YUVJ warnings
// ---------------------------------------------------------------------------
static void SwsFixColorRange(struct SwsContext* sws, AVCodecContext* cc) {
    const int* coeffs = sws_getCoefficients(SWS_CS_DEFAULT);
    int srcRange = (cc->color_range == AVCOL_RANGE_JPEG ||
                    cc->pix_fmt == AV_PIX_FMT_YUVJ420P ||
                    cc->pix_fmt == AV_PIX_FMT_YUVJ422P ||
                    cc->pix_fmt == AV_PIX_FMT_YUVJ444P ||
                    cc->pix_fmt == AV_PIX_FMT_YUVJ411P ||
                    cc->pix_fmt == AV_PIX_FMT_YUVJ440P) ? 1 : 0;
    sws_setColorspaceDetails(sws, coeffs, srcRange, coeffs, 1, 0, 1 << 16, 1 << 16);
}

/**
 * @brief 根据已解码帧的真实像素格式设置 swscale 色彩范围。
 * @param sws 已创建的像素格式转换上下文。
 * @param frame 已成功解码且仍然有效的源帧。
 */
static void SwsFixColorRange(struct SwsContext* sws, const AVFrame* frame) {
    const AVPixelFormat pixelFormat = static_cast<AVPixelFormat>(frame->format);
    const int* coeffs = sws_getCoefficients(SWS_CS_DEFAULT);
    const int srcRange = (frame->color_range == AVCOL_RANGE_JPEG ||
                          pixelFormat == AV_PIX_FMT_YUVJ420P ||
                          pixelFormat == AV_PIX_FMT_YUVJ422P ||
                          pixelFormat == AV_PIX_FMT_YUVJ444P ||
                          pixelFormat == AV_PIX_FMT_YUVJ411P ||
                          pixelFormat == AV_PIX_FMT_YUVJ440P) ? 1 : 0;
    sws_setColorspaceDetails(sws, coeffs, srcRange, coeffs, 1, 0, 1 << 16, 1 << 16);
}

// ---------------------------------------------------------------------------
// Helper: pick image codec by file extension
// ---------------------------------------------------------------------------
static AVCodecID DetectImageCodec(const char* filename) {
    const char* dot = strrchr(filename, '.');
    if (!dot) return AV_CODEC_ID_MJPEG;
    if (_stricmp(dot, ".png") == 0)  return AV_CODEC_ID_PNG;
    if (_stricmp(dot, ".bmp") == 0)  return AV_CODEC_ID_BMP;
    if (_stricmp(dot, ".jpg") == 0 || _stricmp(dot, ".jpeg") == 0)
        return AV_CODEC_ID_MJPEG;
    return AV_CODEC_ID_MJPEG;
}

// ---------------------------------------------------------------------------
// Helper: encode RGB24 data and save as an image file
// ---------------------------------------------------------------------------
static bool SaveImageFile(const char* path,
                          const uint8_t* rgbData,
                          int width, int height, int linesize) {
    AVCodecID codecId = DetectImageCodec(path);
    const AVCodec* codec = avcodec_find_encoder(codecId);
    if (!codec) return false;

    AVCodecContext* enc = avcodec_alloc_context3(codec);
    if (!enc) return false;
    enc->width      = width;
    enc->height     = height;
    enc->time_base  = AVRational{ 1, 1 };

    AVPixelFormat encFmt;
    if (codecId == AV_CODEC_ID_PNG) {
        encFmt = AV_PIX_FMT_RGBA;
    } else if (codecId == AV_CODEC_ID_BMP) {
        encFmt = AV_PIX_FMT_BGR24;
    } else {
        encFmt = AV_PIX_FMT_YUV420P;
        enc->color_range = AVCOL_RANGE_JPEG;
        enc->qmin = 2;
        enc->qmax = 10;
    }
    enc->pix_fmt = encFmt;

    if (avcodec_open2(enc, codec, nullptr) < 0) {
        avcodec_free_context(&enc);
        return false;
    }

    struct SwsContext* sws = sws_getContext(
        width, height, AV_PIX_FMT_RGB24,
        width, height, encFmt,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws) {
        avcodec_free_context(&enc);
        return false;
    }

    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        sws_freeContext(sws);
        avcodec_free_context(&enc);
        return false;
    }
    frame->width  = width;
    frame->height = height;
    frame->format = encFmt;
    if (av_frame_get_buffer(frame, 0) < 0) {
        sws_freeContext(sws);
        av_frame_free(&frame);
        avcodec_free_context(&enc);
        return false;
    }

    const uint8_t* srcData[1]    = { rgbData };
    int            srcLinesize[1] = { linesize };
    sws_scale(sws, srcData, srcLinesize, 0, height,
              frame->data, frame->linesize);
    frame->pts = 0;

    std::wstring wpath = Utf8ToWide(path);
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, wpath.c_str(), L"wb") != 0 || !fp) {
        sws_freeContext(sws);
        av_frame_free(&frame);
        avcodec_free_context(&enc);
        return false;
    }

    bool ok = false;
    if (avcodec_send_frame(enc, frame) >= 0) {
        AVPacket* pkt = av_packet_alloc();
        if (pkt != nullptr) {
            while (avcodec_receive_packet(enc, pkt) >= 0) {
                fwrite(pkt->data, 1, pkt->size, fp);
                av_packet_unref(pkt);
                ok = true;
            }
            av_packet_free(&pkt);
        }
    }

    fclose(fp);
    sws_freeContext(sws);
    av_frame_free(&frame);
    avcodec_free_context(&enc);
    return ok;
}

// ---------------------------------------------------------------------------
// Helper: compute 64-bit dHash from a 9x8 grayscale buffer -> 16 hex chars
// ---------------------------------------------------------------------------
static void DHashFromGray17x16(const uint8_t* gray, int linesize, char* outHex17) {
    // 8 rows x 8 comparisons = 64 bits
    // Pack into 8 bytes (big-endian: first comparison is MSB of byte 0)
    uint8_t bytes[8];
    memset(bytes, 0, sizeof(bytes));
    int bitPos = 0;
    for (int y = 0; y < DHASH_H; ++y) {
        const uint8_t* row = gray + y * linesize;
        for (int x = 0; x < DHASH_W - 1; ++x) {
            if (row[x] < row[x + 1]) {
                bytes[bitPos / 8] |= (1 << (7 - (bitPos % 8)));
            }
            ++bitPos;
        }
    }
    static const char kHex[] = "0123456789abcdef";
    for (int i = 0; i < 8; ++i) {
        outHex17[i * 2]     = kHex[(bytes[i] >> 4) & 0xF];
        outHex17[i * 2 + 1] = kHex[bytes[i] & 0xF];
    }
    outHex17[DHASH_HEX_LEN] = '\0';
}

/** @brief 从 9x8 灰度图计算原生 64 位 dHash。 */
static uint64_t DHashValueFromGray(const uint8_t* gray, const int linesize) {
    uint64_t value = 0;
    for (int y = 0; y < DHASH_H; ++y) {
        const uint8_t* row = gray + y * linesize;
        for (int x = 0; x < DHASH_W - 1; ++x) {
            value = (value << 1) | (row[x] < row[x + 1] ? 1ULL : 0ULL);
        }
    }
    return value;
}

/** @brief FFmpeg 中断上下文；每次读取到数据后刷新 lastProgress。 */
struct MediaInterruptContext {
    VideoScShouldCancel shouldCancel = nullptr;
    void* cancelContext = nullptr;
    uint32_t timeoutMilliseconds = 0;
    std::chrono::steady_clock::time_point lastProgress = std::chrono::steady_clock::now();
};

/** @brief FFmpeg 在打开、探测、读取和 seek 时轮询此回调。 */
static int MediaInterruptCallback(void* opaque) {
    auto* context = static_cast<MediaInterruptContext*>(opaque);
    if (context == nullptr) return 0;
    if (context->shouldCancel != nullptr && context->shouldCancel(context->cancelContext) != 0) return 1;
    if (context->timeoutMilliseconds == 0) return 0;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - context->lastProgress);
    return elapsed.count() > context->timeoutMilliseconds ? 1 : 0;
}

/** @brief 成功读取或 seek 后刷新无进展超时。 */
static void MarkMediaProgress(MediaInterruptContext& context) {
    context.lastProgress = std::chrono::steady_clock::now();
}

/**
 * @brief 单次图片解码后生成感知 Hash 和结构直验需要的固定灰度面。
 *
 * 该对象只在一次 DLL 调用内存在。PDQ 和结构面使用同一张 256×256 灰度图来源，
 * 避免同一文件为了不同算法重复打开和解码。
 */
struct DecodedImagePlanes {
    uint32_t width = 0;
    uint32_t height = 0;
    std::string mimeType;
    std::string containerName;
    std::string codecName;
    std::string pixelFormat;
    std::array<uint8_t, DHASH_W * DHASH_H> dhashGray{};
    std::array<uint8_t, 64 * 64> pdqGray{};
    std::array<uint8_t, 256 * 256> normalizedGray{};
};

/**
 * @brief 把一个已解码帧缩放为固定尺寸 GRAY8 平面。
 * @param frame 有效的 FFmpeg 解码帧。
 * @param width 目标宽度。
 * @param height 目标高度。
 * @param flags swscale 缩放算法标志。
 * @param destination 连续目标缓冲区。
 * @param destinationSize 目标缓冲区字节数。
 * @return 完整输出目标高度时返回 true。
 */
static bool ScaleFrameToGray(const AVFrame* frame,
                             const int width,
                             const int height,
                             const int flags,
                             uint8_t* destination,
                             const std::size_t destinationSize) {
    if (frame == nullptr || destination == nullptr || width <= 0 || height <= 0 ||
        destinationSize < static_cast<std::size_t>(width) * height || frame->format < 0) {
        return false;
    }
    const AVPixelFormat sourceFormat = static_cast<AVPixelFormat>(frame->format);
    if (sws_isSupportedInput(sourceFormat) <= 0 || sws_isSupportedOutput(AV_PIX_FMT_GRAY8) <= 0) {
        return false;
    }
    SwsContext* scaler = sws_getContext(frame->width,
                                        frame->height,
                                        sourceFormat,
                                        width,
                                        height,
                                        AV_PIX_FMT_GRAY8,
                                        flags,
                                        nullptr,
                                        nullptr,
                                        nullptr);
    if (scaler == nullptr) return false;
    SwsFixColorRange(scaler, frame);
    uint8_t* outputData[4] = {destination, nullptr, nullptr, nullptr};
    int outputStride[4] = {width, 0, 0, 0};
    const int convertedHeight = sws_scale(scaler,
                                          frame->data,
                                          frame->linesize,
                                          0,
                                          frame->height,
                                          outputData,
                                          outputStride);
    sws_freeContext(scaler);
    return convertedHeight == height;
}

/**
 * @brief 打开图片、解码第一帧并一次生成三种固定灰度面。
 * @param imagePath UTF-8 图片路径。
 * @param ffmpegThreadCount 解码器线程数，零按一处理。
 * @param shouldCancel 可选取消回调。
 * @param cancelContext 取消回调上下文。
 * @param timeoutMilliseconds 无进展超时，零表示不启用。
 * @param output 接收元数据与固定灰度面。
 * @param statusCode 失败时接收 VideoSc 状态码。
 * @param nativeError 失败时接收 FFmpeg/Win32 原生错误。
 * @param error 失败时接收不含路径的错误说明。
 * @return 全部灰度面成功生成时返回 true。
 */
static bool DecodeImagePlanes(const char* imagePath,
                              const uint32_t ffmpegThreadCount,
                              VideoScShouldCancel shouldCancel,
                              void* cancelContext,
                              const uint32_t timeoutMilliseconds,
                              DecodedImagePlanes& output,
                              int& statusCode,
                              uint32_t& nativeError,
                              std::string& error) {
    output = {};
    statusCode = VIDEOSC_ERR_DECODE_FAILED;
    nativeError = 0;
    error.clear();
    if (imagePath == nullptr || imagePath[0] == '\0') {
        statusCode = VIDEOSC_ERR_INVALID_ARG;
        nativeError = ERROR_INVALID_PARAMETER;
        error = "Image path is empty";
        return false;
    }

    MediaInterruptContext interrupt;
    interrupt.shouldCancel = shouldCancel;
    interrupt.cancelContext = cancelContext;
    interrupt.timeoutMilliseconds = timeoutMilliseconds;
    MarkMediaProgress(interrupt);

    AVFormatContext* format = avformat_alloc_context();
    AVCodecContext* decoder = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    auto cleanup = [&]() {
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&decoder);
        if (format != nullptr) avformat_close_input(&format);
    };
    if (format == nullptr) {
        statusCode = VIDEOSC_ERR_OUT_OF_MEMORY;
        nativeError = ERROR_OUTOFMEMORY;
        error = "Cannot allocate image format context";
        return false;
    }
    format->interrupt_callback.callback = MediaInterruptCallback;
    format->interrupt_callback.opaque = &interrupt;
    int ffmpegError = avformat_open_input(&format, imagePath, nullptr, nullptr);
    if (ffmpegError < 0) {
        const bool cancelled = shouldCancel != nullptr && shouldCancel(cancelContext) != 0;
        const bool interrupted = cancelled || MediaInterruptCallback(&interrupt) != 0;
        statusCode = interrupted ? (cancelled ? VIDEOSC_ERR_MEDIA_CANCELLED
                                              : VIDEOSC_ERR_MEDIA_TIMEOUT)
                                 : VIDEOSC_ERR_OPEN_FAILED;
        nativeError = static_cast<uint32_t>(-ffmpegError);
        error = interrupted ? (cancelled ? "Image open was cancelled" : "Image open timed out")
                            : "Cannot open image file";
        cleanup();
        return false;
    }
    MarkMediaProgress(interrupt);
    ffmpegError = avformat_find_stream_info(format, nullptr);
    if (ffmpegError < 0) {
        const bool cancelled = shouldCancel != nullptr && shouldCancel(cancelContext) != 0;
        const bool interrupted = cancelled || MediaInterruptCallback(&interrupt) != 0;
        statusCode = interrupted ? (cancelled ? VIDEOSC_ERR_MEDIA_CANCELLED
                                              : VIDEOSC_ERR_MEDIA_TIMEOUT)
                                 : VIDEOSC_ERR_OPEN_FAILED;
        nativeError = static_cast<uint32_t>(-ffmpegError);
        error = interrupted ? (cancelled ? "Image stream probing was cancelled"
                                         : "Image stream probing timed out")
                            : "Cannot read image stream information";
        cleanup();
        return false;
    }
    MarkMediaProgress(interrupt);

    int visualStream = -1;
    for (unsigned int index = 0; index < format->nb_streams; ++index) {
        if (format->streams[index]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            visualStream = static_cast<int>(index);
            break;
        }
    }
    if (visualStream < 0) {
        statusCode = VIDEOSC_ERR_NO_VIDEO_STREAM;
        error = "No visual stream found in image";
        cleanup();
        return false;
    }

    AVCodecParameters* parameters = format->streams[visualStream]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(parameters->codec_id);
    if (codec == nullptr) {
        statusCode = VIDEOSC_ERR_NO_DECODER;
        error = "Image decoder was not found";
        cleanup();
        return false;
    }
    decoder = avcodec_alloc_context3(codec);
    if (decoder == nullptr || avcodec_parameters_to_context(decoder, parameters) < 0) {
        statusCode = VIDEOSC_ERR_NO_DECODER;
        nativeError = ERROR_OUTOFMEMORY;
        error = "Cannot allocate image decoder";
        cleanup();
        return false;
    }
    decoder->thread_count = static_cast<int>(ffmpegThreadCount == 0 ? 1 : ffmpegThreadCount);
    decoder->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    ffmpegError = avcodec_open2(decoder, codec, nullptr);
    if (ffmpegError < 0) {
        statusCode = VIDEOSC_ERR_NO_DECODER;
        nativeError = static_cast<uint32_t>(-ffmpegError);
        error = "Cannot open image decoder";
        cleanup();
        return false;
    }

    packet = av_packet_alloc();
    frame = av_frame_alloc();
    if (packet == nullptr || frame == nullptr) {
        statusCode = VIDEOSC_ERR_OUT_OF_MEMORY;
        nativeError = ERROR_OUTOFMEMORY;
        error = "Cannot allocate image decode buffers";
        cleanup();
        return false;
    }
    bool decoded = false;
    while (!decoded && av_read_frame(format, packet) >= 0) {
        MarkMediaProgress(interrupt);
        if (packet->stream_index == visualStream && avcodec_send_packet(decoder, packet) >= 0) {
            while (avcodec_receive_frame(decoder, frame) >= 0) {
                decoded = true;
                break;
            }
        }
        av_packet_unref(packet);
        if (MediaInterruptCallback(&interrupt) != 0) break;
    }
    if (!decoded && avcodec_send_packet(decoder, nullptr) >= 0) {
        decoded = avcodec_receive_frame(decoder, frame) >= 0;
    }
    if (!decoded || frame->width <= 0 || frame->height <= 0) {
        const bool cancelled = shouldCancel != nullptr && shouldCancel(cancelContext) != 0;
        statusCode = cancelled ? VIDEOSC_ERR_MEDIA_CANCELLED :
                     MediaInterruptCallback(&interrupt) != 0 ? VIDEOSC_ERR_MEDIA_TIMEOUT :
                                                               VIDEOSC_ERR_DECODE_FAILED;
        error = cancelled ? "Image decode was cancelled" : "Cannot decode image frame";
        cleanup();
        return false;
    }

    output.width = static_cast<uint32_t>(frame->width);
    output.height = static_cast<uint32_t>(frame->height);
    output.mimeType = format->iformat != nullptr && format->iformat->mime_type != nullptr
                          ? format->iformat->mime_type
                          : "";
    output.containerName = format->iformat != nullptr && format->iformat->name != nullptr
                               ? format->iformat->name
                               : "";
    output.codecName = avcodec_get_name(parameters->codec_id);
    const char* pixelFormatName = av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format));
    output.pixelFormat = pixelFormatName == nullptr ? "" : pixelFormatName;

    // dHash 沿用既有双线性缩放；PDQ 与结构面使用面积缩放以稳定压缩高分辨率输入。
    const bool scaled = ScaleFrameToGray(frame,
                                         DHASH_W,
                                         DHASH_H,
                                         SWS_BILINEAR,
                                         output.dhashGray.data(),
                                         output.dhashGray.size()) &&
                        ScaleFrameToGray(frame,
                                         64,
                                         64,
                                         SWS_AREA,
                                         output.pdqGray.data(),
                                         output.pdqGray.size()) &&
                        ScaleFrameToGray(frame,
                                         256,
                                         256,
                                         SWS_AREA,
                                         output.normalizedGray.data(),
                                         output.normalizedGray.size());
    if (!scaled) {
        statusCode = VIDEOSC_ERR_DECODE_FAILED;
        error = "Cannot normalize image feature planes";
        cleanup();
        return false;
    }

    cleanup();
    statusCode = VIDEOSC_OK;
    nativeError = 0;
    error.clear();
    return true;
}

/** @brief 在目标时间之前 seek，并解码第一张时间戳不早于目标的画面。 */
static bool DecodeFrameAt(AVFormatContext* format,
                          AVCodecContext* decoder,
                          const int videoStream,
                          const int64_t targetTimestamp,
                          AVFrame* frame,
                          MediaInterruptContext& interrupt) {
    if (av_seek_frame(format, videoStream, targetTimestamp, AVSEEK_FLAG_BACKWARD) < 0) return false;
    MarkMediaProgress(interrupt);
    avcodec_flush_buffers(decoder);
    AVPacket* packet = av_packet_alloc();
    if (packet == nullptr) return false;
    bool found = false;
    while (!found && av_read_frame(format, packet) >= 0) {
        MarkMediaProgress(interrupt);
        if (packet->stream_index == videoStream && avcodec_send_packet(decoder, packet) >= 0) {
            while (avcodec_receive_frame(decoder, frame) >= 0) {
                MarkMediaProgress(interrupt);
                const int64_t timestamp = frame->best_effort_timestamp;
                if (timestamp == AV_NOPTS_VALUE || timestamp >= targetTimestamp) {
                    found = true;
                    break;
                }
                av_frame_unref(frame);
            }
        }
        av_packet_unref(packet);
        if (MediaInterruptCallback(&interrupt) != 0) break;
    }
    av_packet_free(&packet);
    return found;
}

/** @brief 六帧两两汉明距离平均值严格小于 5 时识别为静态画面。 */
static bool IsStaticVisual(const uint64_t hashes[6]) {
    unsigned int total = 0;
    unsigned int pairs = 0;
    for (int left = 0; left < 6; ++left) {
        for (int right = left + 1; right < 6; ++right) {
            total += videosc::CpuDispatch::Popcount64(hashes[left] ^ hashes[right]);
            ++pairs;
        }
    }
    return pairs != 0 && static_cast<double>(total) / static_cast<double>(pairs) < 5.0;
}

// ===========================================================================
// CaptureVideoScreenshots
// ===========================================================================
static VideoScResult CaptureVideoScreenshotsImpl(
    const char*  videoPath,
    const char*  outputDir,
    int          screenshotCount,
    int          maxLongEdge,
    const char** screenshotNames)
{
    VideoScResult result;
    result.statusCode       = VIDEOSC_OK;
    result.errorMessage     = nullptr;
    result.duration         = -1.0;
    result.thumbnailCount   = 0;
    result.thumbnailPaths   = nullptr;
    result.thumbnailDHashes = nullptr;

    if (!videoPath || !outputDir || !screenshotNames || screenshotCount <= 0) {
        result.statusCode   = VIDEOSC_ERR_INVALID_ARG;
        result.errorMessage = DupString("Invalid argument");
        return result;
    }

    // Pre-allocate output arrays (so we can still report partial results on error)
    char** pathArr  = (char**)calloc((size_t)screenshotCount, sizeof(char*));
    char** dhashArr = (char**)calloc((size_t)screenshotCount, sizeof(char*));
    if (pathArr == nullptr || dhashArr == nullptr) {
        free(pathArr);
        free(dhashArr);
        result.statusCode = VIDEOSC_ERR_OUT_OF_MEMORY;
        result.errorMessage = DupString("Out of memory");
        return result;
    }
    result.thumbnailPaths   = (const char**)pathArr;
    result.thumbnailDHashes = (const char**)dhashArr;

    AVFormatContext* fmtCtx = nullptr;
    if (avformat_open_input(&fmtCtx, videoPath, nullptr, nullptr) < 0) {
        result.statusCode   = VIDEOSC_ERR_OPEN_FAILED;
        result.errorMessage = DupString("avformat_open_input failed");
        return result;
    }
    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        avformat_close_input(&fmtCtx);
        result.statusCode   = VIDEOSC_ERR_OPEN_FAILED;
        result.errorMessage = DupString("avformat_find_stream_info failed");
        return result;
    }

    // Duration
    double duration = -1.0;
    if (fmtCtx->duration > 0) {
        duration = (double)fmtCtx->duration / AV_TIME_BASE;
    } else {
        for (unsigned i = 0; i < fmtCtx->nb_streams; ++i) {
            int64_t d = fmtCtx->streams[i]->duration;
            if (d > 0) {
                double ds = (double)d * av_q2d(fmtCtx->streams[i]->time_base);
                if (ds > duration) duration = ds;
            }
        }
    }
    result.duration = duration;
    if (duration <= 0.0) {
        avformat_close_input(&fmtCtx);
        result.statusCode   = VIDEOSC_ERR_NO_DURATION;
        result.errorMessage = DupString("Cannot determine video duration");
        return result;
    }

    // Find video stream
    int videoIdx = -1;
    for (unsigned i = 0; i < fmtCtx->nb_streams; ++i) {
        if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoIdx = (int)i; break;
        }
    }
    if (videoIdx < 0) {
        avformat_close_input(&fmtCtx);
        result.statusCode   = VIDEOSC_ERR_NO_VIDEO_STREAM;
        result.errorMessage = DupString("No video stream found");
        return result;
    }

    AVCodecParameters* codecPar = fmtCtx->streams[videoIdx]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(codecPar->codec_id);
    if (!codec) {
        avformat_close_input(&fmtCtx);
        result.statusCode   = VIDEOSC_ERR_NO_DECODER;
        result.errorMessage = DupString("Decoder not found");
        return result;
    }
    AVCodecContext* codecCtx = avcodec_alloc_context3(codec);
    const bool codecContextMissing = codecCtx == nullptr;
    if (codecContextMissing || avcodec_parameters_to_context(codecCtx, codecPar) < 0) {
        avcodec_free_context(&codecCtx);
        avformat_close_input(&fmtCtx);
        result.statusCode = codecContextMissing ? VIDEOSC_ERR_OUT_OF_MEMORY : VIDEOSC_ERR_NO_DECODER;
        result.errorMessage = DupString(codecContextMissing ? "Out of memory" : "Cannot copy codec parameters");
        return result;
    }
    if (avcodec_open2(codecCtx, codec, nullptr) < 0) {
        avcodec_free_context(&codecCtx);
        avformat_close_input(&fmtCtx);
        result.statusCode   = VIDEOSC_ERR_NO_DECODER;
        result.errorMessage = DupString("avcodec_open2 failed");
        return result;
    }

    int origW = codecCtx->width;
    int origH = codecCtx->height;

    // Target size for screenshot
    int targetW = origW, targetH = origH;
    if (maxLongEdge > 0) {
        int longEdge = (origW > origH) ? origW : origH;
        if (longEdge > maxLongEdge) {
            double scale = (double)maxLongEdge / (double)longEdge;
            targetW = (int)(origW * scale);
            targetH = (int)(origH * scale);
            targetW -= (targetW & 1);
            targetH -= (targetH & 1);
            if (targetW < 2) targetW = 2;
            if (targetH < 2) targetH = 2;
        }
    }

    // sws #1: decoder fmt -> RGB24 (for screenshot saving)
    struct SwsContext* swsDec = sws_getContext(
        origW, origH, codecCtx->pix_fmt,
        targetW, targetH, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!swsDec) {
        avcodec_free_context(&codecCtx);
        avformat_close_input(&fmtCtx);
        result.statusCode   = VIDEOSC_ERR_SWS_INIT;
        result.errorMessage = DupString("sws_getContext (RGB24) failed");
        return result;
    }
    SwsFixColorRange(swsDec, codecCtx);

    // sws #2: decoder fmt -> GRAY8 17x16 (for dHash)
    struct SwsContext* swsDHash = sws_getContext(
        origW, origH, codecCtx->pix_fmt,
        DHASH_W, DHASH_H, AV_PIX_FMT_GRAY8,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!swsDHash) {
        sws_freeContext(swsDec);
        avcodec_free_context(&codecCtx);
        avformat_close_input(&fmtCtx);
        result.statusCode   = VIDEOSC_ERR_SWS_INIT;
        result.errorMessage = DupString("sws_getContext (GRAY8) failed");
        return result;
    }
    SwsFixColorRange(swsDHash, codecCtx);

    AVFrame* frame    = av_frame_alloc();
    AVFrame* rgbFrame = av_frame_alloc();
    AVFrame* grayFrame = av_frame_alloc();
    int  rgbBufSize = av_image_get_buffer_size(AV_PIX_FMT_RGB24, targetW, targetH, 1);
    uint8_t* rgbBuf = rgbBufSize > 0 ? (uint8_t*)av_malloc((size_t)rgbBufSize) : nullptr;
    if (frame == nullptr || rgbFrame == nullptr || grayFrame == nullptr || rgbBuf == nullptr ||
        av_image_fill_arrays(rgbFrame->data, rgbFrame->linesize, rgbBuf,
                             AV_PIX_FMT_RGB24, targetW, targetH, 1) < 0) {
        av_free(rgbBuf);
        av_frame_free(&frame);
        av_frame_free(&rgbFrame);
        av_frame_free(&grayFrame);
        sws_freeContext(swsDec);
        sws_freeContext(swsDHash);
        avcodec_free_context(&codecCtx);
        avformat_close_input(&fmtCtx);
        result.statusCode = VIDEOSC_ERR_OUT_OF_MEMORY;
        result.errorMessage = DupString("Cannot allocate screenshot buffers");
        return result;
    }

    grayFrame->width  = DHASH_W;
    grayFrame->height = DHASH_H;
    grayFrame->format = AV_PIX_FMT_GRAY8;
    if (av_frame_get_buffer(grayFrame, 0) < 0) {
        av_free(rgbBuf);
        av_frame_free(&frame);
        av_frame_free(&rgbFrame);
        av_frame_free(&grayFrame);
        sws_freeContext(swsDec);
        sws_freeContext(swsDHash);
        avcodec_free_context(&codecCtx);
        avformat_close_input(&fmtCtx);
        result.statusCode = VIDEOSC_ERR_OUT_OF_MEMORY;
        result.errorMessage = DupString("Cannot allocate dHash frame");
        return result;
    }

    // Interval k = t/(n+1); capture times: k, 2k, ..., n*k
    double k = duration / (double)(screenshotCount + 1);
    AVRational streamTb = fmtCtx->streams[videoIdx]->time_base;
    double streamTick = av_q2d(streamTb);

    for (int i = 0; i < screenshotCount; ++i) {
        double targetTime = (double)(i + 1) * k;
        int64_t targetTs  = (int64_t)(targetTime / streamTick);

        if (av_seek_frame(fmtCtx, videoIdx, targetTs, AVSEEK_FLAG_BACKWARD) < 0) {
            continue;
        }
        avcodec_flush_buffers(codecCtx);

        AVPacket* pkt = av_packet_alloc();
        if (pkt == nullptr) {
            result.statusCode = VIDEOSC_ERR_OUT_OF_MEMORY;
            result.errorMessage = DupString("Cannot allocate decode packet");
            break;
        }
        bool gotFrame = false;
        while (!gotFrame) {
            if (av_read_frame(fmtCtx, pkt) < 0) break;
            if (pkt->stream_index == videoIdx) {
                if (avcodec_send_packet(codecCtx, pkt) >= 0) {
                    while (avcodec_receive_frame(codecCtx, frame) >= 0) {
                        gotFrame = true;
                        break;
                    }
                }
            }
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);

        if (!gotFrame) continue;

        // Scale to RGB24 (for saving) and GRAY8 17x16 (for dHash) in parallel
        sws_scale(swsDec, frame->data, frame->linesize, 0, origH,
                  rgbFrame->data, rgbFrame->linesize);
        sws_scale(swsDHash, frame->data, frame->linesize, 0, origH,
                  grayFrame->data, grayFrame->linesize);

        // Build output path
        std::string fullPath = outputDir;
        if (!fullPath.empty()) {
            char last = fullPath.back();
            if (last != '\\' && last != '/') {
                fullPath += '\\';
            }
        }
        fullPath += screenshotNames[i];

        if (SaveImageFile(fullPath.c_str(),
                          rgbFrame->data[0],
                          targetW, targetH,
                          rgbFrame->linesize[0])) {
            pathArr[result.thumbnailCount] = DupString(fullPath.c_str());
            // Compute dHash
            char dhash[DHASH_HEX_LEN + 1];
            DHashFromGray17x16(grayFrame->data[0], grayFrame->linesize[0], dhash);
            dhashArr[result.thumbnailCount] = DupString(dhash);
            result.thumbnailCount++;
        }
    }

    // Cleanup FFmpeg resources
    av_free(rgbBuf);
    av_frame_free(&frame);
    av_frame_free(&rgbFrame);
    av_frame_free(&grayFrame);
    sws_freeContext(swsDec);
    sws_freeContext(swsDHash);
    avcodec_free_context(&codecCtx);
    avformat_close_input(&fmtCtx);

    return result;
}

// ===========================================================================
// FreeVideoScResult
// ===========================================================================
static void FreeVideoScResultImpl(VideoScResult* result) {
    if (!result) return;
    if (result->errorMessage) {
        free((void*)result->errorMessage);
        result->errorMessage = nullptr;
    }
    if (result->thumbnailPaths) {
        for (int i = 0; i < result->thumbnailCount; ++i) {
            if (result->thumbnailPaths[i]) free((void*)result->thumbnailPaths[i]);
        }
        free((void*)result->thumbnailPaths);
        result->thumbnailPaths = nullptr;
    }
    if (result->thumbnailDHashes) {
        for (int i = 0; i < result->thumbnailCount; ++i) {
            if (result->thumbnailDHashes[i]) free((void*)result->thumbnailDHashes[i]);
        }
        free((void*)result->thumbnailDHashes);
        result->thumbnailDHashes = nullptr;
    }
    result->thumbnailCount = 0;
    result->statusCode     = VIDEOSC_OK;
    result->duration       = -1.0;
}

/** @brief 设置媒体分析错误；消息不包含输入路径。 */
static int SetMediaError(VideoScMediaResult* result,
                         const int statusCode,
                         const uint32_t nativeError,
                         const char* message) {
    result->statusCode = statusCode;
    result->nativeError = nativeError;
    result->errorMessage = DupString(message);
    return 0;
}

/** @brief 把 FFmpeg 格式元数据复制到 DLL 所有权字符串。 */
static void SetMediaText(VideoScMediaResult* result,
                         const std::string& mimeType,
                         const std::string& containerName,
                         const std::string& videoCodec,
                         const std::string& pixelFormat) {
    result->mimeType = DupString(mimeType.c_str());
    result->containerName = DupString(containerName.c_str());
    result->videoCodec = DupString(videoCodec.c_str());
    result->pixelFormat = DupString(pixelFormat.c_str());
}

static int AnalyzeMediaFileImpl(const char* mediaPath,
                                const VideoScMediaOptions* options,
                                VideoScMediaResult* outResult) {
    if (outResult == nullptr) return 0;
    *outResult = {};
    if (mediaPath == nullptr || mediaPath[0] == '\0' || options == nullptr ||
        options->structSize < sizeof(VideoScMediaOptions) ||
        options->mediaKindHint < VIDEOSC_MEDIA_IMAGE || options->mediaKindHint > VIDEOSC_MEDIA_AUDIO) {
        return SetMediaError(outResult, VIDEOSC_ERR_INVALID_ARG, ERROR_INVALID_PARAMETER, "Invalid media options");
    }
    outResult->mediaKind = options->mediaKindHint;
    if (options->mediaKindHint == VIDEOSC_MEDIA_VIDEO &&
        (options->contactSheetPath == nullptr || options->contactSheetPath[0] == '\0' ||
         options->contactSheetCellLongEdge < 16 || options->contactSheetCellLongEdge > 4096)) {
        return SetMediaError(outResult,
                             VIDEOSC_ERR_INVALID_ARG,
                             ERROR_INVALID_PARAMETER,
                             "Video contact sheet options are invalid");
    }

    MediaInterruptContext interrupt;
    interrupt.shouldCancel = options->shouldCancel;
    interrupt.cancelContext = options->cancelContext;
    interrupt.timeoutMilliseconds = options->noProgressTimeoutMilliseconds;
    MarkMediaProgress(interrupt);
    AVFormatContext* format = avformat_alloc_context();
    if (format == nullptr) {
        return SetMediaError(outResult, VIDEOSC_ERR_OPEN_FAILED, ERROR_OUTOFMEMORY, "Cannot allocate format context");
    }
    format->interrupt_callback.callback = MediaInterruptCallback;
    format->interrupt_callback.opaque = &interrupt;
    int ffmpegError = avformat_open_input(&format, mediaPath, nullptr, nullptr);
    if (ffmpegError < 0) {
        if (format != nullptr) avformat_close_input(&format);
        const bool interrupted = MediaInterruptCallback(&interrupt) != 0;
        const bool cancelled = options->shouldCancel != nullptr && options->shouldCancel(options->cancelContext) != 0;
        return SetMediaError(outResult,
                             interrupted ? (cancelled ? VIDEOSC_ERR_MEDIA_CANCELLED : VIDEOSC_ERR_MEDIA_TIMEOUT)
                                         : VIDEOSC_ERR_OPEN_FAILED,
                             static_cast<uint32_t>(-ffmpegError),
                             interrupted ? "Media open was interrupted" : "Cannot open media file");
    }
    MarkMediaProgress(interrupt);
    ffmpegError = avformat_find_stream_info(format, nullptr);
    if (ffmpegError < 0) {
        avformat_close_input(&format);
        const bool cancelled = options->shouldCancel != nullptr && options->shouldCancel(options->cancelContext) != 0;
        const bool interrupted = cancelled || MediaInterruptCallback(&interrupt) != 0;
        return SetMediaError(outResult,
                             interrupted ? (cancelled ? VIDEOSC_ERR_MEDIA_CANCELLED
                                                      : VIDEOSC_ERR_MEDIA_TIMEOUT)
                                         : VIDEOSC_ERR_OPEN_FAILED,
                             static_cast<uint32_t>(-ffmpegError),
                             interrupted ? "Media stream probing was interrupted"
                                         : "Cannot read media stream information");
    }
    MarkMediaProgress(interrupt);

    const std::string containerName =
        format->iformat != nullptr && format->iformat->name != nullptr ? format->iformat->name : "";
    const std::string mimeType =
        format->iformat != nullptr && format->iformat->mime_type != nullptr ? format->iformat->mime_type : "";
    if (format->duration > 0) {
        outResult->durationMilliseconds = static_cast<int64_t>(
            static_cast<long double>(format->duration) * 1000.0L / static_cast<long double>(AV_TIME_BASE));
    }
    outResult->bitrate = format->bit_rate > 0 ? static_cast<uint64_t>(format->bit_rate) : 0;

    if (options->mediaKindHint == VIDEOSC_MEDIA_AUDIO) {
        SetMediaText(outResult, mimeType, containerName, "", "");
        avformat_close_input(&format);
        outResult->statusCode = VIDEOSC_OK;
        return 1;
    }

    int videoStream = -1;
    for (unsigned int index = 0; index < format->nb_streams; ++index) {
        if (format->streams[index]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStream = static_cast<int>(index);
            break;
        }
    }
    if (videoStream < 0) {
        avformat_close_input(&format);
        return SetMediaError(outResult, VIDEOSC_ERR_NO_VIDEO_STREAM, 0, "No visual stream found");
    }
    AVStream* stream = format->streams[videoStream];
    AVCodecParameters* parameters = stream->codecpar;
    outResult->width = parameters->width > 0 ? static_cast<uint32_t>(parameters->width) : 0;
    outResult->height = parameters->height > 0 ? static_cast<uint32_t>(parameters->height) : 0;
    const AVRational guessedRate = av_guess_frame_rate(format, stream, nullptr);
    outResult->frameRate = guessedRate.den != 0 ? av_q2d(guessedRate) : 0.0;
    if (parameters->bit_rate > 0) outResult->bitrate = static_cast<uint64_t>(parameters->bit_rate);
    const std::string codecName = avcodec_get_name(parameters->codec_id);

    if (options->mediaKindHint == VIDEOSC_MEDIA_IMAGE) {
        avformat_close_input(&format);
        char hash[DHASH_HEX_LEN + 1]{};
        if (!ComputeImageDHash(mediaPath, hash, sizeof(hash))) {
            return SetMediaError(outResult, VIDEOSC_ERR_DECODE_FAILED, 0, "Cannot decode image for dHash");
        }
        outResult->imageDHash = _strtoui64(hash, nullptr, 16);
        outResult->hasImageDHash = 1;
        SetMediaText(outResult, mimeType, containerName, codecName, "");
        outResult->statusCode = VIDEOSC_OK;
        return 1;
    }

    if (outResult->durationMilliseconds <= 0) {
        avformat_close_input(&format);
        return SetMediaError(outResult, VIDEOSC_ERR_NO_DURATION, 0, "Cannot determine video duration");
    }
    const AVCodec* decoderCodec = avcodec_find_decoder(parameters->codec_id);
    if (decoderCodec == nullptr) {
        avformat_close_input(&format);
        return SetMediaError(outResult, VIDEOSC_ERR_NO_DECODER, 0, "Video decoder was not found");
    }
    AVCodecContext* decoder = avcodec_alloc_context3(decoderCodec);
    if (decoder == nullptr || avcodec_parameters_to_context(decoder, parameters) < 0) {
        if (decoder != nullptr) avcodec_free_context(&decoder);
        avformat_close_input(&format);
        return SetMediaError(outResult, VIDEOSC_ERR_NO_DECODER, 0, "Cannot allocate video decoder");
    }
    decoder->thread_count = static_cast<int>(options->ffmpegThreadCount == 0 ? 1 : options->ffmpegThreadCount);
    decoder->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    ffmpegError = avcodec_open2(decoder, decoderCodec, nullptr);
    if (ffmpegError < 0) {
        avcodec_free_context(&decoder);
        avformat_close_input(&format);
        return SetMediaError(outResult,
                             VIDEOSC_ERR_NO_DECODER,
                             static_cast<uint32_t>(-ffmpegError),
                             "Cannot open video decoder");
    }

    const int originalWidth = decoder->width;
    const int originalHeight = decoder->height;
    if (originalWidth <= 0 || originalHeight <= 0) {
        avcodec_free_context(&decoder);
        avformat_close_input(&format);
        return SetMediaError(outResult, VIDEOSC_ERR_DECODE_FAILED, 0, "Video dimensions are invalid");
    }
    double scale = static_cast<double>(options->contactSheetCellLongEdge) /
                   static_cast<double>((std::max)(originalWidth, originalHeight));
    if (scale > 1.0) scale = 1.0;
    int cellWidth = (std::max)(2, static_cast<int>(originalWidth * scale));
    int cellHeight = (std::max)(2, static_cast<int>(originalHeight * scale));
    cellWidth -= cellWidth & 1;
    cellHeight -= cellHeight & 1;
    const int sheetWidth = cellWidth * 3;
    const int sheetHeight = cellHeight * 2;
    const std::size_t sheetBytes = static_cast<std::size_t>(sheetWidth) *
                                   static_cast<std::size_t>(sheetHeight) * 3;
    std::vector<uint8_t> contactSheet(sheetBytes, 0);
    std::array<uint8_t, DHASH_W * DHASH_H> gray{};
    AVFrame* frame = av_frame_alloc();
    SwsContext* rgbScaler = nullptr;
    SwsContext* grayScaler = nullptr;
    bool decodedAll = frame != nullptr;
    const double streamTick = av_q2d(stream->time_base);
    const double durationSeconds = static_cast<double>(outResult->durationMilliseconds) / 1000.0;
    for (int index = 0; decodedAll && index < 6; ++index) {
        const double targetSeconds = durationSeconds * static_cast<double>(index + 1) / 7.0;
        const int64_t targetTimestamp = static_cast<int64_t>(targetSeconds / streamTick);
        if (!DecodeFrameAt(format, decoder, videoStream, targetTimestamp, frame, interrupt)) {
            decodedAll = false;
            break;
        }
        rgbScaler = sws_getCachedContext(rgbScaler,
                                         frame->width,
                                         frame->height,
                                         static_cast<AVPixelFormat>(frame->format),
                                         cellWidth,
                                         cellHeight,
                                         AV_PIX_FMT_RGB24,
                                         SWS_BILINEAR,
                                         nullptr,
                                         nullptr,
                                         nullptr);
        grayScaler = sws_getCachedContext(grayScaler,
                                          frame->width,
                                          frame->height,
                                          static_cast<AVPixelFormat>(frame->format),
                                          DHASH_W,
                                          DHASH_H,
                                          AV_PIX_FMT_GRAY8,
                                          SWS_BILINEAR,
                                          nullptr,
                                          nullptr,
                                          nullptr);
        if (rgbScaler == nullptr || grayScaler == nullptr) {
            decodedAll = false;
            break;
        }
        SwsFixColorRange(rgbScaler, decoder);
        SwsFixColorRange(grayScaler, decoder);
        uint8_t* rgbDestination[4] = {
            contactSheet.data() + static_cast<std::size_t>(index / 3 * cellHeight) * sheetWidth * 3 +
                static_cast<std::size_t>(index % 3 * cellWidth) * 3,
            nullptr,
            nullptr,
            nullptr,
        };
        int rgbLines[4] = {sheetWidth * 3, 0, 0, 0};
        uint8_t* grayDestination[4] = {gray.data(), nullptr, nullptr, nullptr};
        int grayLines[4] = {DHASH_W, 0, 0, 0};
        if (sws_scale(rgbScaler,
                      frame->data,
                      frame->linesize,
                      0,
                      frame->height,
                      rgbDestination,
                      rgbLines) != cellHeight ||
            sws_scale(grayScaler,
                      frame->data,
                      frame->linesize,
                      0,
                      frame->height,
                      grayDestination,
                      grayLines) != DHASH_H) {
            decodedAll = false;
            break;
        }
        outResult->videoDHashes[index] = DHashValueFromGray(gray.data(), DHASH_W);
        av_frame_unref(frame);
    }
    const std::string pixelFormat =
        decoder->pix_fmt != AV_PIX_FMT_NONE && av_get_pix_fmt_name(decoder->pix_fmt) != nullptr
            ? av_get_pix_fmt_name(decoder->pix_fmt)
            : "";
    if (rgbScaler != nullptr) sws_freeContext(rgbScaler);
    if (grayScaler != nullptr) sws_freeContext(grayScaler);
    av_frame_free(&frame);
    avcodec_free_context(&decoder);
    avformat_close_input(&format);
    if (!decodedAll) {
        const bool cancelled = options->shouldCancel != nullptr && options->shouldCancel(options->cancelContext) != 0;
        const bool timedOut = !cancelled && MediaInterruptCallback(&interrupt) != 0;
        return SetMediaError(outResult,
                             cancelled ? VIDEOSC_ERR_MEDIA_CANCELLED
                                       : (timedOut ? VIDEOSC_ERR_MEDIA_TIMEOUT : VIDEOSC_ERR_DECODE_FAILED),
                             0,
                             "Cannot decode all six video samples");
    }

    const std::wstring contactSheetWide = Utf8ToWide(options->contactSheetPath);
    std::error_code directoryError;
    const std::filesystem::path parent = std::filesystem::path(contactSheetWide).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, directoryError);
    if (directoryError || !SaveImageFile(options->contactSheetPath,
                                         contactSheet.data(),
                                         sheetWidth,
                                         sheetHeight,
                                         sheetWidth * 3)) {
        return SetMediaError(outResult,
                             VIDEOSC_ERR_CONTACT_SHEET,
                             directoryError.value(),
                             "Cannot save video contact sheet");
    }
    outResult->hasVideoDHashes = 1;
    outResult->staticVisual = IsStaticVisual(outResult->videoDHashes) ? 1 : 0;
    outResult->contactSheetPath = DupString(options->contactSheetPath);
    SetMediaText(outResult, mimeType, containerName, codecName, pixelFormat);
    outResult->statusCode = VIDEOSC_OK;
    return 1;
}

static void FreeVideoScMediaResultImpl(VideoScMediaResult* result) {
    if (result == nullptr) return;
    free(const_cast<char*>(result->errorMessage));
    free(const_cast<char*>(result->mimeType));
    free(const_cast<char*>(result->containerName));
    free(const_cast<char*>(result->videoCodec));
    free(const_cast<char*>(result->pixelFormat));
    free(const_cast<char*>(result->contactSheetPath));
    *result = {};
}

/** @brief 释放图片感知特征结果中的 DLL 字符串并保留可再次使用的结构大小。 */
static void FreeVideoScImageFeatureResultV1Impl(VideoScImageFeatureResultV1* result) {
    if (result == nullptr) return;
    const uint32_t structSize = result->structSize;
    free(const_cast<char*>(result->errorMessage));
    free(const_cast<char*>(result->mimeType));
    free(const_cast<char*>(result->containerName));
    free(const_cast<char*>(result->imageCodec));
    free(const_cast<char*>(result->pixelFormat));
    *result = {};
    result->structSize = structSize;
}

/** @brief 设置图片感知特征错误并返回 C 接口失败值。 */
static int SetImageFeatureError(VideoScImageFeatureResultV1* result,
                                const int statusCode,
                                const uint32_t nativeError,
                                const char* message) {
    result->statusCode = statusCode;
    result->nativeError = nativeError;
    result->errorMessage = DupString(message);
    return 0;
}

/**
 * @brief 单次解码图片并填充全部持久化感知特征。
 * @param imagePath UTF-8 图片路径。
 * @param options V1 调用选项。
 * @param outResult 调用方提供的 V1 结果。
 * @return 全部特征成功返回 1，否则返回 0。
 */
static int AnalyzeImagePerceptualFeaturesV1Impl(
    const char* imagePath,
    const VideoScImageFeatureOptionsV1* options,
    VideoScImageFeatureResultV1* outResult) {
    if (outResult == nullptr || options == nullptr ||
        outResult->structSize < sizeof(VideoScImageFeatureResultV1) ||
        options->structSize < sizeof(VideoScImageFeatureOptionsV1)) {
        return 0;
    }
    *outResult = {};
    outResult->structSize = sizeof(VideoScImageFeatureResultV1);

    DecodedImagePlanes decoded;
    int statusCode = VIDEOSC_ERR_DECODE_FAILED;
    uint32_t nativeError = 0;
    std::string error;
    if (!DecodeImagePlanes(imagePath,
                           options->ffmpegThreadCount,
                           options->shouldCancel,
                           options->cancelContext,
                           options->noProgressTimeoutMilliseconds,
                           decoded,
                           statusCode,
                           nativeError,
                           error)) {
        return SetImageFeatureError(outResult, statusCode, nativeError, error.c_str());
    }

    videosc::ImagePerceptualFeatures features;
    if (!videosc::ComputeImagePerceptualFeatures(decoded.pdqGray.data(),
                                                 64,
                                                 decoded.normalizedGray.data(),
                                                 256,
                                                 features,
                                                 error)) {
        return SetImageFeatureError(outResult,
                                    VIDEOSC_ERR_DECODE_FAILED,
                                    0,
                                    error.empty() ? "Cannot compute image perceptual features" : error.c_str());
    }

    outResult->width = decoded.width;
    outResult->height = decoded.height;
    outResult->hasImageDHash = 1;
    outResult->imageDHash = DHashValueFromGray(decoded.dhashGray.data(), DHASH_W);
    outResult->hasPdqHash = 1;
    memcpy(outResult->pdqHash, features.pdq_hash.data(), features.pdq_hash.size());
    outResult->pdqQuality = features.pdq_quality;
    outResult->hasZonedPHashes = 1;
    memcpy(outResult->zonedPHashes,
           features.zoned_phashes.data(),
           sizeof(outResult->zonedPHashes));
    outResult->perceptualAlgorithmVersion = videosc::kImagePerceptualAlgorithmVersion;
    outResult->mimeType = DupString(decoded.mimeType.c_str());
    outResult->containerName = DupString(decoded.containerName.c_str());
    outResult->imageCodec = DupString(decoded.codecName.c_str());
    outResult->pixelFormat = DupString(decoded.pixelFormat.c_str());
    outResult->statusCode = VIDEOSC_OK;
    return 1;
}

/** @brief 释放 DLL 结构面句柄与错误字符串并保留结构大小。 */
static void FreeVideoScImageStructureResultV1Impl(VideoScImageStructureResultV1* result) {
    if (result == nullptr) return;
    const uint32_t structSize = result->structSize;
    free(const_cast<char*>(result->errorMessage));
    delete static_cast<videosc::ImageStructuralPlane*>(result->structureHandle);
    *result = {};
    result->structSize = structSize;
}

/** @brief 设置结构面加载错误并返回 C 接口失败值。 */
static int SetImageStructureError(VideoScImageStructureResultV1* result,
                                  const int statusCode,
                                  const uint32_t nativeError,
                                  const char* message) {
    result->statusCode = statusCode;
    result->nativeError = nativeError;
    result->errorMessage = DupString(message);
    return 0;
}

/**
 * @brief 解码并创建可由报告级缓存持有的结构面句柄。
 * @param imagePath UTF-8 图片路径。
 * @param options V1 结构加载选项。
 * @param outResult 调用方提供的 V1 结果。
 * @return 创建成功返回 1，否则返回 0。
 */
static int LoadImageStructureV1Impl(const char* imagePath,
                                    const VideoScImageStructureOptionsV1* options,
                                    VideoScImageStructureResultV1* outResult) {
    if (outResult == nullptr || options == nullptr ||
        outResult->structSize < sizeof(VideoScImageStructureResultV1) ||
        options->structSize < sizeof(VideoScImageStructureOptionsV1)) {
        return 0;
    }
    *outResult = {};
    outResult->structSize = sizeof(VideoScImageStructureResultV1);

    DecodedImagePlanes decoded;
    int statusCode = VIDEOSC_ERR_DECODE_FAILED;
    uint32_t nativeError = 0;
    std::string error;
    if (!DecodeImagePlanes(imagePath,
                           options->ffmpegThreadCount,
                           options->shouldCancel,
                           options->cancelContext,
                           options->noProgressTimeoutMilliseconds,
                           decoded,
                           statusCode,
                           nativeError,
                           error)) {
        return SetImageStructureError(outResult, statusCode, nativeError, error.c_str());
    }
    auto structure = videosc::BuildImageStructuralPlane(decoded.normalizedGray.data(), 256, error);
    if (!structure) {
        return SetImageStructureError(outResult,
                                      VIDEOSC_ERR_DECODE_FAILED,
                                      0,
                                      error.empty() ? "Cannot build image structure" : error.c_str());
    }
    outResult->statusCode = VIDEOSC_OK;
    outResult->width = decoded.width;
    outResult->height = decoded.height;
    outResult->structuralAlgorithmVersion = videosc::kImageStructuralAlgorithmVersion;
    outResult->structureHandle = structure.release();
    return 1;
}

/** @brief 释放结构比较错误字符串并保留结构大小。 */
static void FreeVideoScImageStructureCompareResultV1Impl(
    VideoScImageStructureCompareResultV1* result) {
    if (result == nullptr) return;
    const uint32_t structSize = result->structSize;
    free(const_cast<char*>(result->errorMessage));
    *result = {};
    result->structSize = structSize;
}

/**
 * @brief 对两个 DLL 结构句柄执行固定坐标整图直验。
 * @param leftHandle 左结构句柄。
 * @param rightHandle 右结构句柄。
 * @param options 块通过阈值。
 * @param outResult 量化输出。
 * @return 比较成功返回 1，否则返回 0。
 */
static int CompareImageStructuresV1Impl(
    const void* leftHandle,
    const void* rightHandle,
    const VideoScImageStructureCompareOptionsV1* options,
    VideoScImageStructureCompareResultV1* outResult) {
    if (outResult == nullptr || options == nullptr ||
        outResult->structSize < sizeof(VideoScImageStructureCompareResultV1) ||
        options->structSize < sizeof(VideoScImageStructureCompareOptionsV1)) {
        return 0;
    }
    *outResult = {};
    outResult->structSize = sizeof(VideoScImageStructureCompareResultV1);
    if (leftHandle == nullptr || rightHandle == nullptr) {
        outResult->statusCode = VIDEOSC_ERR_INVALID_ARG;
        outResult->errorMessage = DupString("Image structure handle is null");
        return 0;
    }

    videosc::ImageStructuralCompareOptions nativeOptions;
    nativeOptions.block_pass_score = options->blockPassScore;
    videosc::ImageStructuralCompareResult nativeResult;
    std::string error;
    if (!videosc::CompareImageStructuralPlanes(
            *static_cast<const videosc::ImageStructuralPlane*>(leftHandle),
            *static_cast<const videosc::ImageStructuralPlane*>(rightHandle),
            nativeOptions,
            nativeResult,
            error)) {
        outResult->statusCode = VIDEOSC_ERR_INVALID_ARG;
        outResult->errorMessage = DupString(error.c_str());
        return 0;
    }
    outResult->statusCode = VIDEOSC_OK;
    outResult->globalEdgeZnccMillionths = nativeResult.global_edge_zncc_millionths;
    outResult->trimmedBlockScoreMillionths = nativeResult.trimmed_block_score_millionths;
    outResult->passingBlockPercentMillionths = nativeResult.passing_block_percent_millionths;
    outResult->comparedBlockCount = nativeResult.compared_block_count;
    return 1;
}

/** @brief 设置图片预览失败结果并保持 C ABI 返回值语义。 */
static int SetImagePreviewError(VideoScImagePreviewResult* result,
                                const int statusCode,
                                const uint32_t nativeError,
                                const char* message) {
    result->statusCode = statusCode;
    result->nativeError = nativeError;
    result->errorMessage = DupString(message);
    return 0;
}

/** @brief 释放图片预览结果内部由 DLL 分配的字符串。 */
static void FreeVideoScImagePreviewResultImpl(VideoScImagePreviewResult* result) {
    if (result == nullptr) return;
    free(const_cast<char*>(result->errorMessage));
    free(const_cast<char*>(result->outputPath));
    *result = {};
}

/**
 * @brief 使用 FFmpeg 解码任意受支持图片的首帧并输出有界缩略图。
 * @param sourcePath UTF-8 原始图片路径。
 * @param outputPath UTF-8 临时缩略图路径。
 * @param options 线程、尺寸、超时和取消选项。
 * @param outResult 接收结构化结果。
 * @return 成功返回 1，失败返回 0。
 */
static int GenerateImagePreviewImpl(const char* sourcePath,
                                    const char* outputPath,
                                    const VideoScImagePreviewOptions* options,
                                    VideoScImagePreviewResult* outResult) {
    if (outResult == nullptr) return 0;
    *outResult = {};
    if (sourcePath == nullptr || sourcePath[0] == '\0' ||
        outputPath == nullptr || outputPath[0] == '\0' ||
        options == nullptr || options->structSize < sizeof(VideoScImagePreviewOptions) ||
        options->maximumLongEdge < 16 || options->maximumLongEdge > 16384) {
        return SetImagePreviewError(outResult,
                                    VIDEOSC_ERR_INVALID_ARG,
                                    ERROR_INVALID_PARAMETER,
                                    "Invalid image preview options");
    }

    MediaInterruptContext interrupt;
    interrupt.shouldCancel = options->shouldCancel;
    interrupt.cancelContext = options->cancelContext;
    interrupt.timeoutMilliseconds = options->noProgressTimeoutMilliseconds;
    MarkMediaProgress(interrupt);

    AVFormatContext* format = avformat_alloc_context();
    AVCodecContext* decoder = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    SwsContext* scaler = nullptr;
    auto cleanup = [&]() {
        sws_freeContext(scaler);
        av_packet_free(&packet);
        av_frame_free(&frame);
        avcodec_free_context(&decoder);
        if (format != nullptr) avformat_close_input(&format);
    };
    if (format == nullptr) {
        return SetImagePreviewError(outResult,
                                    VIDEOSC_ERR_OUT_OF_MEMORY,
                                    ERROR_OUTOFMEMORY,
                                    "Cannot allocate image format context");
    }
    format->interrupt_callback.callback = MediaInterruptCallback;
    format->interrupt_callback.opaque = &interrupt;

    int ffmpegError = avformat_open_input(&format, sourcePath, nullptr, nullptr);
    if (ffmpegError < 0) {
        cleanup();
        const bool cancelled = options->shouldCancel != nullptr &&
                               options->shouldCancel(options->cancelContext) != 0;
        const bool interrupted = cancelled || MediaInterruptCallback(&interrupt) != 0;
        return SetImagePreviewError(outResult,
                                    interrupted ? (cancelled ? VIDEOSC_ERR_MEDIA_CANCELLED
                                                             : VIDEOSC_ERR_MEDIA_TIMEOUT)
                                                : VIDEOSC_ERR_OPEN_FAILED,
                                    static_cast<uint32_t>(-ffmpegError),
                                    interrupted ? "Image open was interrupted" : "Cannot open image source");
    }
    MarkMediaProgress(interrupt);
    ffmpegError = avformat_find_stream_info(format, nullptr);
    if (ffmpegError < 0) {
        cleanup();
        const bool cancelled = options->shouldCancel != nullptr &&
                               options->shouldCancel(options->cancelContext) != 0;
        const bool interrupted = cancelled || MediaInterruptCallback(&interrupt) != 0;
        return SetImagePreviewError(outResult,
                                    interrupted ? (cancelled ? VIDEOSC_ERR_MEDIA_CANCELLED
                                                             : VIDEOSC_ERR_MEDIA_TIMEOUT)
                                                : VIDEOSC_ERR_OPEN_FAILED,
                                    static_cast<uint32_t>(-ffmpegError),
                                    interrupted ? "Image stream probing was interrupted"
                                                : "Cannot read image stream information");
    }

    int streamIndex = -1;
    for (unsigned int index = 0; index < format->nb_streams; ++index) {
        if (format->streams[index]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            streamIndex = static_cast<int>(index);
            break;
        }
    }
    if (streamIndex < 0) {
        cleanup();
        return SetImagePreviewError(outResult, VIDEOSC_ERR_NO_VIDEO_STREAM, 0, "No image stream found");
    }
    AVCodecParameters* parameters = format->streams[streamIndex]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(parameters->codec_id);
    if (codec == nullptr) {
        cleanup();
        return SetImagePreviewError(outResult, VIDEOSC_ERR_NO_DECODER, 0, "Image decoder was not found");
    }
    decoder = avcodec_alloc_context3(codec);
    if (decoder == nullptr || avcodec_parameters_to_context(decoder, parameters) < 0) {
        cleanup();
        return SetImagePreviewError(outResult,
                                    VIDEOSC_ERR_NO_DECODER,
                                    ERROR_OUTOFMEMORY,
                                    "Cannot allocate image decoder");
    }
    decoder->thread_count = static_cast<int>(options->ffmpegThreadCount == 0
                                                 ? 1
                                                 : options->ffmpegThreadCount);
    decoder->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    ffmpegError = avcodec_open2(decoder, codec, nullptr);
    if (ffmpegError < 0) {
        cleanup();
        return SetImagePreviewError(outResult,
                                    VIDEOSC_ERR_NO_DECODER,
                                    static_cast<uint32_t>(-ffmpegError),
                                    "Cannot open image decoder");
    }

    frame = av_frame_alloc();
    packet = av_packet_alloc();
    if (frame == nullptr || packet == nullptr) {
        cleanup();
        return SetImagePreviewError(outResult,
                                    VIDEOSC_ERR_OUT_OF_MEMORY,
                                    ERROR_OUTOFMEMORY,
                                    "Cannot allocate image decode buffers");
    }
    bool decoded = false;
    while (!decoded && (ffmpegError = av_read_frame(format, packet)) >= 0) {
        MarkMediaProgress(interrupt);
        if (packet->stream_index == streamIndex && avcodec_send_packet(decoder, packet) >= 0) {
            while (avcodec_receive_frame(decoder, frame) >= 0) {
                decoded = true;
                break;
            }
        }
        av_packet_unref(packet);
    }
    if (!decoded && avcodec_send_packet(decoder, nullptr) >= 0) {
        decoded = avcodec_receive_frame(decoder, frame) >= 0;
    }
    if (!decoded || frame->width <= 0 || frame->height <= 0 || frame->format < 0) {
        const bool cancelled = options->shouldCancel != nullptr &&
                               options->shouldCancel(options->cancelContext) != 0;
        const bool timedOut = !cancelled && MediaInterruptCallback(&interrupt) != 0;
        cleanup();
        return SetImagePreviewError(outResult,
                                    cancelled ? VIDEOSC_ERR_MEDIA_CANCELLED
                                              : (timedOut ? VIDEOSC_ERR_MEDIA_TIMEOUT
                                                          : VIDEOSC_ERR_DECODE_FAILED),
                                    ffmpegError < 0 ? static_cast<uint32_t>(-ffmpegError) : 0,
                                    cancelled ? "Image preview generation was cancelled"
                                              : (timedOut ? "Image preview generation timed out"
                                                          : "Cannot decode image first frame"));
    }

    double scale = static_cast<double>(options->maximumLongEdge) /
                   static_cast<double>((std::max)(frame->width, frame->height));
    if (scale > 1.0) scale = 1.0;
    int targetWidth = (std::max)(1, static_cast<int>(frame->width * scale));
    int targetHeight = (std::max)(1, static_cast<int>(frame->height * scale));
    const AVCodecID outputCodec = DetectImageCodec(outputPath);
    if (outputCodec == AV_CODEC_ID_MJPEG) {
        targetWidth = (std::max)(2, targetWidth - (targetWidth & 1));
        targetHeight = (std::max)(2, targetHeight - (targetHeight & 1));
    }
    const std::size_t pixelBytes = static_cast<std::size_t>(targetWidth) *
                                   static_cast<std::size_t>(targetHeight) * 3ULL;
    if (pixelBytes > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        cleanup();
        return SetImagePreviewError(outResult,
                                    VIDEOSC_ERR_OUT_OF_MEMORY,
                                    ERROR_NOT_ENOUGH_MEMORY,
                                    "Image preview pixel buffer is too large");
    }
    std::vector<uint8_t> rgb(pixelBytes);
    scaler = sws_getContext(frame->width,
                            frame->height,
                            static_cast<AVPixelFormat>(frame->format),
                            targetWidth,
                            targetHeight,
                            AV_PIX_FMT_RGB24,
                            SWS_BILINEAR,
                            nullptr,
                            nullptr,
                            nullptr);
    if (scaler == nullptr) {
        cleanup();
        return SetImagePreviewError(outResult, VIDEOSC_ERR_SWS_INIT, 0, "Cannot initialize image scaler");
    }
    SwsFixColorRange(scaler, frame);
    uint8_t* destinations[4] = {rgb.data(), nullptr, nullptr, nullptr};
    int destinationLines[4] = {targetWidth * 3, 0, 0, 0};
    if (sws_scale(scaler,
                  frame->data,
                  frame->linesize,
                  0,
                  frame->height,
                  destinations,
                  destinationLines) != targetHeight) {
        cleanup();
        return SetImagePreviewError(outResult, VIDEOSC_ERR_DECODE_FAILED, 0, "Cannot scale image preview");
    }

    const std::wstring outputWide = Utf8ToWide(outputPath);
    std::error_code directoryError;
    const std::filesystem::path parent = std::filesystem::path(outputWide).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, directoryError);
    if (directoryError || !SaveImageFile(outputPath,
                                         rgb.data(),
                                         targetWidth,
                                         targetHeight,
                                         targetWidth * 3)) {
        cleanup();
        return SetImagePreviewError(outResult,
                                    VIDEOSC_ERR_CONTACT_SHEET,
                                    directoryError.value(),
                                    "Cannot write image preview file");
    }
    cleanup();
    outResult->statusCode = VIDEOSC_OK;
    outResult->width = static_cast<uint32_t>(targetWidth);
    outResult->height = static_cast<uint32_t>(targetHeight);
    outResult->outputPath = DupString(outputPath);
    if (outResult->outputPath == nullptr) {
        return SetImagePreviewError(outResult,
                                    VIDEOSC_ERR_OUT_OF_MEMORY,
                                    ERROR_OUTOFMEMORY,
                                    "Cannot allocate image preview result");
    }
    return 1;
}

// ===========================================================================
// ComputeFileSHA512 / ComputeFileSHA512Ex
// ===========================================================================
namespace {

/** @brief 单次 Overlapped 读取的内部状态。 */
enum class BlockReadStatus {
    Success,
    Failed,
    Timeout,
    Cancelled,
};

/** @brief 单次 Overlapped 读取结果，error 保留原始 Win32 错误。 */
struct BlockReadResult {
    BlockReadStatus status = BlockReadStatus::Failed;
    DWORD bytesRead = 0;
    DWORD error = ERROR_SUCCESS;
};

/**
 * @brief 判断调用方是否请求取消当前文件。
 * @param callback 可选回调。
 * @param context 回调上下文。
 * @return 回调存在且返回非零时为 true。
 */
bool IsHashCancelled(VideoScShouldCancel callback, void* context) {
    return callback != nullptr && callback(context) != 0;
}

/**
 * @brief 发起一个指定偏移的可取消异步读取，并执行无进展超时。
 * @param file 以 FILE_FLAG_OVERLAPPED 打开的文件。
 * @param offset 文件字节偏移。
 * @param buffer 目标固定容量缓冲。
 * @param requestedBytes 本次读取字节数。
 * @param timeoutMilliseconds 无进展超时。
 * @param shouldCancel 可选取消回调。
 * @param cancelContext 回调上下文。
 * @return 读取状态、实际字节和 Win32 错误。
 */
BlockReadResult ReadOverlappedBlock(HANDLE file,
                                    const std::uint64_t offset,
                                    std::uint8_t* buffer,
                                    const DWORD requestedBytes,
                                    const DWORD timeoutMilliseconds,
                                    VideoScShouldCancel shouldCancel,
                                    void* cancelContext) {
    BlockReadResult result;
    HANDLE eventHandle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (eventHandle == nullptr) {
        result.error = GetLastError();
        return result;
    }

    OVERLAPPED overlapped{};
    overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFULL);
    overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
    overlapped.hEvent = eventHandle;

    if (IsHashCancelled(shouldCancel, cancelContext)) {
        CloseHandle(eventHandle);
        result.status = BlockReadStatus::Cancelled;
        result.error = ERROR_OPERATION_ABORTED;
        return result;
    }

    DWORD immediateBytes = 0;
    const BOOL immediate = ReadFile(file, buffer, requestedBytes, &immediateBytes, &overlapped);
    if (immediate) {
        CloseHandle(eventHandle);
        result.status = BlockReadStatus::Success;
        result.bytesRead = immediateBytes;
        return result;
    }
    const DWORD readError = GetLastError();
    if (readError != ERROR_IO_PENDING) {
        CloseHandle(eventHandle);
        result.error = readError;
        return result;
    }

    const ULONGLONG startedAt = GetTickCount64();
    while (true) {
        if (IsHashCancelled(shouldCancel, cancelContext)) {
            CancelIoEx(file, &overlapped);
            DWORD ignored = 0;
            GetOverlappedResult(file, &overlapped, &ignored, TRUE);
            CloseHandle(eventHandle);
            result.status = BlockReadStatus::Cancelled;
            result.error = ERROR_OPERATION_ABORTED;
            return result;
        }

        const ULONGLONG elapsed = GetTickCount64() - startedAt;
        if (elapsed >= timeoutMilliseconds) {
            CancelIoEx(file, &overlapped);
            DWORD ignored = 0;
            GetOverlappedResult(file, &overlapped, &ignored, TRUE);
            CloseHandle(eventHandle);
            result.status = BlockReadStatus::Timeout;
            result.error = WAIT_TIMEOUT;
            return result;
        }

        const DWORD remaining = static_cast<DWORD>(timeoutMilliseconds - elapsed);
        const DWORD waitSlice = (std::min)(remaining, static_cast<DWORD>(100));
        const DWORD waitResult = WaitForSingleObject(eventHandle, waitSlice);
        if (waitResult == WAIT_TIMEOUT) {
            continue;
        }
        if (waitResult != WAIT_OBJECT_0) {
            result.error = GetLastError();
            CancelIoEx(file, &overlapped);
            DWORD ignored = 0;
            GetOverlappedResult(file, &overlapped, &ignored, TRUE);
            CloseHandle(eventHandle);
            return result;
        }

        DWORD completedBytes = 0;
        if (!GetOverlappedResult(file, &overlapped, &completedBytes, FALSE)) {
            result.error = GetLastError();
            CloseHandle(eventHandle);
            return result;
        }
        CloseHandle(eventHandle);
        result.status = BlockReadStatus::Success;
        result.bytesRead = completedBytes;
        return result;
    }
}

/**
 * @brief 比较散列前后的文件大小、时间和稳定文件身份。
 * @param before 散列前信息。
 * @param after 散列后信息。
 * @return 会使摘要失效的字段发生变化时返回 true。
 */
bool FileChanged(const BY_HANDLE_FILE_INFORMATION& before, const BY_HANDLE_FILE_INFORMATION& after) {
    return before.dwVolumeSerialNumber != after.dwVolumeSerialNumber ||
           before.nFileIndexHigh != after.nFileIndexHigh || before.nFileIndexLow != after.nFileIndexLow ||
           before.nFileSizeHigh != after.nFileSizeHigh || before.nFileSizeLow != after.nFileSizeLow ||
           CompareFileTime(&before.ftLastWriteTime, &after.ftLastWriteTime) != 0;
}

/** @brief Windows FILETIME 转 Unix UTC 毫秒。 */
std::int64_t FileTimeToUnixMilliseconds(const FILETIME& value) {
    ULARGE_INTEGER ticks{};
    ticks.LowPart = value.dwLowDateTime;
    ticks.HighPart = value.dwHighDateTime;
    constexpr std::uint64_t kUnixEpochTicks = 116444736000000000ULL;
    if (ticks.QuadPart < kUnixEpochTicks) return 0;
    return static_cast<std::int64_t>((ticks.QuadPart - kUnixEpochTicks) / 10000ULL);
}

/**
 * @brief 把 64 字节 SHA-512 写成小写十六进制。
 * @param digest 二进制摘要。
 * @param destination 至少 129 字节的输出缓冲。
 */
void WriteSha512Hex(const std::uint8_t* digest, char* destination) {
    constexpr char kHex[] = "0123456789abcdef";
    for (int index = 0; index < 64; ++index) {
        destination[index * 2] = kHex[(digest[index] >> 4) & 0x0F];
        destination[index * 2 + 1] = kHex[digest[index] & 0x0F];
    }
    destination[128] = '\0';
}

}  // namespace

static int ComputeFileSHA512ExImpl(
    const char* filePath,
    const VideoScHashOptions* options,
    VideoScShouldCancel shouldCancel,
    void* cancelContext,
    VideoScFileHashResult* outResult) {
    if (outResult == nullptr) {
        return 0;
    }
    *outResult = {};
    outResult->statusCode = VIDEOSC_HASH_INVALID_ARGUMENT;

    const VideoScHashOptions defaults = {
        sizeof(VideoScHashOptions), 1024U * 1024U, 2U, 64U * 1024U, 2U, 60U * 1000U};
    const VideoScHashOptions& activeOptions = options == nullptr ? defaults : *options;
    constexpr std::uint32_t kMaximumBlockBytes = 64U * 1024U * 1024U;
    if (filePath == nullptr || filePath[0] == '\0' ||
        (options != nullptr && options->structSize < sizeof(VideoScHashOptions)) ||
        activeOptions.readBlockBytes == 0 || activeOptions.readBlockBytes > kMaximumBlockBytes ||
        activeOptions.smallBlockBytes == 0 || activeOptions.smallBlockBytes > activeOptions.readBlockBytes ||
        activeOptions.noProgressTimeoutMilliseconds == 0) {
        return 0;
    }

    const std::wstring widePath = Utf8ToWide(filePath);
    if (widePath.empty()) {
        return 0;
    }
    HANDLE file = CreateFileW(widePath.c_str(),
                              GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OVERLAPPED,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        outResult->statusCode = VIDEOSC_HASH_OPEN_FAILED;
        outResult->win32Error = GetLastError();
        return 0;
    }

    BY_HANDLE_FILE_INFORMATION beforeInfo{};
    LARGE_INTEGER fileSize{};
    if (!GetFileInformationByHandle(file, &beforeInfo) || !GetFileSizeEx(file, &fileSize) || fileSize.QuadPart < 0) {
        outResult->statusCode = VIDEOSC_HASH_OPEN_FAILED;
        outResult->win32Error = GetLastError();
        CloseHandle(file);
        return 0;
    }
    outResult->fileSize = static_cast<std::uint64_t>(fileSize.QuadPart);
    outResult->volumeSerial = beforeInfo.dwVolumeSerialNumber;
    outResult->fileIdHigh = 0;
    outResult->fileIdLow = (static_cast<std::uint64_t>(beforeInfo.nFileIndexHigh) << 32) |
                           static_cast<std::uint64_t>(beforeInfo.nFileIndexLow);
    outResult->creationTimeUtcMs = FileTimeToUnixMilliseconds(beforeInfo.ftCreationTime);
    outResult->lastWriteTimeUtcMs = FileTimeToUnixMilliseconds(beforeInfo.ftLastWriteTime);

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hashHandle = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA512_ALGORITHM, nullptr, 0) != 0 ||
        BCryptCreateHash(algorithm, &hashHandle, nullptr, 0, nullptr, 0, 0) != 0) {
        outResult->statusCode = VIDEOSC_HASH_CRYPTO_FAILED;
        if (hashHandle != nullptr) BCryptDestroyHash(hashHandle);
        if (algorithm != nullptr) BCryptCloseAlgorithmProvider(algorithm, 0);
        CloseHandle(file);
        return 0;
    }

    std::vector<std::uint8_t> buffer(activeOptions.readBlockBytes);
    std::uint64_t offset = 0;
    bool completed = true;
    while (offset < outResult->fileSize) {
        if (IsHashCancelled(shouldCancel, cancelContext)) {
            outResult->statusCode = VIDEOSC_HASH_CANCELLED;
            outResult->win32Error = ERROR_OPERATION_ABORTED;
            outResult->failedOffset = offset;
            completed = false;
            break;
        }

        const std::uint64_t remaining = outResult->fileSize - offset;
        DWORD requested = static_cast<DWORD>((std::min<std::uint64_t>)(remaining, activeOptions.readBlockBytes));
        BlockReadResult readResult;
        for (std::uint32_t attempt = 0; attempt <= activeOptions.normalBlockRetries; ++attempt) {
            readResult = ReadOverlappedBlock(file,
                                             offset,
                                             buffer.data(),
                                             requested,
                                             activeOptions.noProgressTimeoutMilliseconds,
                                             shouldCancel,
                                             cancelContext);
            if (readResult.status != BlockReadStatus::Failed) {
                break;
            }
        }

        if (readResult.status == BlockReadStatus::Failed) {
            requested = static_cast<DWORD>(
                (std::min<std::uint64_t>)(remaining, activeOptions.smallBlockBytes));
            for (std::uint32_t attempt = 0; attempt <= activeOptions.smallBlockRetries; ++attempt) {
                readResult = ReadOverlappedBlock(file,
                                                 offset,
                                                 buffer.data(),
                                                 requested,
                                                 activeOptions.noProgressTimeoutMilliseconds,
                                                 shouldCancel,
                                                 cancelContext);
                if (readResult.status != BlockReadStatus::Failed) {
                    break;
                }
            }
        }

        if (readResult.status != BlockReadStatus::Success || readResult.bytesRead == 0) {
            outResult->failedOffset = offset;
            outResult->win32Error = readResult.error;
            if (readResult.status == BlockReadStatus::Timeout) {
                outResult->statusCode = VIDEOSC_HASH_READ_TIMEOUT;
            } else if (readResult.status == BlockReadStatus::Cancelled) {
                outResult->statusCode = VIDEOSC_HASH_CANCELLED;
            } else if (readResult.bytesRead == 0) {
                outResult->statusCode = VIDEOSC_HASH_FILE_CHANGED;
            } else {
                outResult->statusCode = VIDEOSC_HASH_READ_FAILED;
            }
            completed = false;
            break;
        }

        if (BCryptHashData(hashHandle, buffer.data(), readResult.bytesRead, 0) != 0) {
            outResult->statusCode = VIDEOSC_HASH_CRYPTO_FAILED;
            outResult->failedOffset = offset;
            completed = false;
            break;
        }
        offset += readResult.bytesRead;
        outResult->bytesRead = offset;
    }

    std::uint8_t digest[64]{};
    if (completed && BCryptFinishHash(hashHandle, digest, sizeof(digest), 0) != 0) {
        outResult->statusCode = VIDEOSC_HASH_CRYPTO_FAILED;
        completed = false;
    }

    BY_HANDLE_FILE_INFORMATION afterInfo{};
    if (completed && (!GetFileInformationByHandle(file, &afterInfo) || FileChanged(beforeInfo, afterInfo))) {
        outResult->statusCode = VIDEOSC_HASH_FILE_CHANGED;
        outResult->win32Error = GetLastError();
        outResult->failedOffset = outResult->bytesRead;
        completed = false;
    }

    BCryptDestroyHash(hashHandle);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    if (!completed) {
        SecureZeroMemory(digest, sizeof(digest));
        outResult->sha512Hex[0] = '\0';
        return 0;
    }

    WriteSha512Hex(digest, outResult->sha512Hex);
    SecureZeroMemory(digest, sizeof(digest));
    outResult->statusCode = VIDEOSC_HASH_OK;
    return 1;
}

static int ComputeFileSHA512Impl(
    const char* filePath,
    char* outHash,
    int outHashSize) {
    if (outHash == nullptr || outHashSize < 129) {
        return 0;
    }
    VideoScFileHashResult result{};
    if (!ComputeFileSHA512Ex(filePath, nullptr, nullptr, nullptr, &result)) {
        outHash[0] = '\0';
        return 0;
    }
    memcpy(outHash, result.sha512Hex, sizeof(result.sha512Hex));
    return 1;
}

// ===========================================================================
// ComputeImageDHash (standalone, optimized: decode directly to 17x16 GRAY8)
// ===========================================================================
/**
 * @brief 解码图片首帧并计算 64 位 dHash。
 * @param filePath UTF-8 编码的图片路径。
 * @param outHash 接收 16 位十六进制哈希和结尾空字符的缓冲区。
 * @param outHashSize 输出缓冲区大小。
 * @return 成功返回 1；输入无效、解码失败或格式不受支持时返回 0。
 */
static int ComputeImageDHashImpl(
    const char* filePath,
    char*       outHash,
    int         outHashSize)
{
    if (!filePath || !outHash || outHashSize < DHASH_HEX_LEN + 1) return 0;
    outHash[0] = '\0';

    AVFormatContext* fmtCtx = nullptr;
    AVCodecContext* cc = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* grayFrame = nullptr;
    AVPacket* packet = nullptr;
    struct SwsContext* sws = nullptr;
    auto cleanup = [&]() {
        sws_freeContext(sws);
        av_packet_free(&packet);
        av_frame_free(&grayFrame);
        av_frame_free(&frame);
        avcodec_free_context(&cc);
        avformat_close_input(&fmtCtx);
    };

    if (avformat_open_input(&fmtCtx, filePath, nullptr, nullptr) < 0) {
        cleanup();
        return 0;
    }
    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        cleanup();
        return 0;
    }

    int videoIdx = -1;
    for (unsigned i = 0; i < fmtCtx->nb_streams; ++i) {
        if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoIdx = static_cast<int>(i);
            break;
        }
    }
    if (videoIdx < 0) {
        cleanup();
        return 0;
    }

    AVCodecParameters* par = fmtCtx->streams[videoIdx]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(par->codec_id);
    if (!codec) {
        cleanup();
        return 0;
    }

    cc = avcodec_alloc_context3(codec);
    if (!cc || avcodec_parameters_to_context(cc, par) < 0) {
        cleanup();
        return 0;
    }
    if (avcodec_open2(cc, codec, nullptr) < 0) {
        cleanup();
        return 0;
    }

    frame = av_frame_alloc();
    packet = av_packet_alloc();
    if (!frame || !packet) {
        cleanup();
        return 0;
    }

    bool decoded = false;
    while (av_read_frame(fmtCtx, packet) >= 0 && !decoded) {
        if (packet->stream_index == videoIdx && avcodec_send_packet(cc, packet) >= 0) {
            while (avcodec_receive_frame(cc, frame) >= 0) {
                decoded = true;
                break;
            }
        }
        av_packet_unref(packet);
    }

    if (!decoded && avcodec_send_packet(cc, nullptr) >= 0) {
        while (avcodec_receive_frame(cc, frame) >= 0) {
            decoded = true;
            break;
        }
    }

    if (!decoded || frame->width <= 0 || frame->height <= 0 || frame->format < 0) {
        cleanup();
        return 0;
    }

    const AVPixelFormat sourceFormat = static_cast<AVPixelFormat>(frame->format);
    if (sws_isSupportedInput(sourceFormat) <= 0 || sws_isSupportedOutput(AV_PIX_FMT_GRAY8) <= 0) {
        cleanup();
        return 0;
    }

    // 直接把解码帧缩放为 9x8 灰度图，不创建中间 RGB 帧。
    sws = sws_getCachedContext(nullptr,
                               frame->width,
                               frame->height,
                               sourceFormat,
                               DHASH_W,
                               DHASH_H,
                               AV_PIX_FMT_GRAY8,
                               SWS_BILINEAR,
                               nullptr,
                               nullptr,
                               nullptr);
    if (!sws) {
        cleanup();
        return 0;
    }
    SwsFixColorRange(sws, frame);

    grayFrame = av_frame_alloc();
    if (!grayFrame) {
        cleanup();
        return 0;
    }
    grayFrame->width = DHASH_W;
    grayFrame->height = DHASH_H;
    grayFrame->format = AV_PIX_FMT_GRAY8;
    if (av_frame_get_buffer(grayFrame, 0) < 0) {
        cleanup();
        return 0;
    }

    const int convertedHeight = sws_scale(sws,
                                          frame->data,
                                          frame->linesize,
                                          0,
                                          frame->height,
                                          grayFrame->data,
                                          grayFrame->linesize);
    if (convertedHeight != DHASH_H) {
        cleanup();
        return 0;
    }

    DHashFromGray17x16(grayFrame->data[0], grayFrame->linesize[0], outHash);
    cleanup();
    return 1;
}

// ===========================================================================
// ComputeHammingDistance
//
// High-performance implementation:
//   - Operates byte-parallel: 2 hex chars -> 1 byte -> XOR -> popcount
//   - Uses runtime POPCNT/SWAR dispatch, so old CPUs do not execute unsupported instructions
//   - No heap allocation, no system calls
//
// Time complexity:  O(n/2) where n = hex string length
// Space complexity: O(1)
// ===========================================================================
static int ComputeHammingDistanceImpl(const char* hash1, const char* hash2) {
    if (!hash1 || !hash2) return -1;

    // Fast length check + length equality check
    size_t len1 = 0, len2 = 0;
    while (hash1[len1]) ++len1;
    while (hash2[len2]) ++len2;
    if (len1 != len2 || len1 == 0 || (len1 & 1) != 0) return -1;

    // Hex char -> 4-bit value (case-insensitive); returns -1 on invalid char
    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };

    int distance = 0;
    const size_t nBytes = len1 / 2;
    for (size_t i = 0; i < nBytes; ++i) {
        int h1a = hexVal(hash1[i * 2]);
        int h1b = hexVal(hash1[i * 2 + 1]);
        int h2a = hexVal(hash2[i * 2]);
        int h2b = hexVal(hash2[i * 2 + 1]);
        if (h1a < 0 || h1b < 0 || h2a < 0 || h2b < 0) return -1;

        uint8_t b1 = (uint8_t)((h1a << 4) | h1b);
        uint8_t b2 = (uint8_t)((h2a << 4) | h2b);
        uint8_t x  = (uint8_t)(b1 ^ b2);

        distance += static_cast<int>(videosc::CpuDispatch::Popcount32(x));
    }

    return distance;
}

// ---------------------------------------------------------------------------
// C ABI exception barriers
// ---------------------------------------------------------------------------

/**
 * @brief 捕获截图实现抛出的 C++ 异常并转换为稳定结果。
 * @return 始终可由 FreeVideoScResult 释放的结果结构。
 */
VIDEOSC_API VideoScResult __cdecl CaptureVideoScreenshots(const char* videoPath,
                                                           const char* outputDir,
                                                           int screenshotCount,
                                                           int maxLongEdge,
                                                           const char** screenshotNames) {
    try {
        return CaptureVideoScreenshotsImpl(videoPath, outputDir, screenshotCount, maxLongEdge, screenshotNames);
    } catch (const std::bad_alloc&) {
        return {VIDEOSC_ERR_OUT_OF_MEMORY, DupString("Out of memory"), -1.0, 0, nullptr, nullptr};
    } catch (const std::exception&) {
        return {VIDEOSC_ERR_UNEXPECTED_FAILURE, DupString("Unexpected C++ exception"), -1.0, 0, nullptr, nullptr};
    } catch (...) {
        return {VIDEOSC_ERR_UNEXPECTED_FAILURE, DupString("Unknown exception"), -1.0, 0, nullptr, nullptr};
    }
}

/** @brief 保证释放函数不会把异常传播到调用方。 */
VIDEOSC_API void __cdecl FreeVideoScResult(VideoScResult* result) {
    try { FreeVideoScResultImpl(result); } catch (...) {}
}

/**
 * @brief 捕获媒体分析 C++ 异常并初始化稳定失败结果。
 * @return 成功返回 1，任何失败返回 0。
 */
VIDEOSC_API int __cdecl AnalyzeMediaFile(const char* mediaPath,
                                         const VideoScMediaOptions* options,
                                         VideoScMediaResult* outResult) {
    if (outResult == nullptr) return 0;
    *outResult = {};
    try {
        return AnalyzeMediaFileImpl(mediaPath, options, outResult);
    } catch (const std::bad_alloc&) {
        FreeVideoScMediaResultImpl(outResult);
        return SetMediaError(outResult, VIDEOSC_ERR_OUT_OF_MEMORY, ERROR_NOT_ENOUGH_MEMORY, "Out of memory");
    } catch (const std::exception&) {
        FreeVideoScMediaResultImpl(outResult);
        return SetMediaError(outResult, VIDEOSC_ERR_UNEXPECTED_FAILURE, ERROR_UNHANDLED_EXCEPTION,
                             "Unexpected C++ exception");
    } catch (...) {
        FreeVideoScMediaResultImpl(outResult);
        return SetMediaError(outResult, VIDEOSC_ERR_UNEXPECTED_FAILURE, ERROR_UNHANDLED_EXCEPTION,
                             "Unknown exception");
    }
}

/** @brief 保证媒体结果释放函数不会把异常传播到调用方。 */
VIDEOSC_API void __cdecl FreeVideoScMediaResult(VideoScMediaResult* result) {
    try { FreeVideoScMediaResultImpl(result); } catch (...) {}
}

/** @brief 捕获图片感知特征实现异常，并保持版本化 C ABI 的结构大小。 */
VIDEOSC_API int __cdecl AnalyzeImagePerceptualFeaturesV1(
    const char* imagePath,
    const VideoScImageFeatureOptionsV1* options,
    VideoScImageFeatureResultV1* outResult) {
    if (outResult == nullptr || outResult->structSize < sizeof(VideoScImageFeatureResultV1)) return 0;
    try {
        return AnalyzeImagePerceptualFeaturesV1Impl(imagePath, options, outResult);
    } catch (const std::bad_alloc&) {
        FreeVideoScImageFeatureResultV1Impl(outResult);
        outResult->structSize = sizeof(VideoScImageFeatureResultV1);
        return SetImageFeatureError(outResult,
                                    VIDEOSC_ERR_OUT_OF_MEMORY,
                                    ERROR_NOT_ENOUGH_MEMORY,
                                    "Out of memory");
    } catch (const std::exception&) {
        FreeVideoScImageFeatureResultV1Impl(outResult);
        outResult->structSize = sizeof(VideoScImageFeatureResultV1);
        return SetImageFeatureError(outResult,
                                    VIDEOSC_ERR_UNEXPECTED_FAILURE,
                                    ERROR_UNHANDLED_EXCEPTION,
                                    "Unexpected C++ exception");
    } catch (...) {
        FreeVideoScImageFeatureResultV1Impl(outResult);
        outResult->structSize = sizeof(VideoScImageFeatureResultV1);
        return SetImageFeatureError(outResult,
                                    VIDEOSC_ERR_UNEXPECTED_FAILURE,
                                    ERROR_UNHANDLED_EXCEPTION,
                                    "Unknown exception");
    }
}

/** @brief 保证图片感知结果释放不会向调用方传播异常。 */
VIDEOSC_API void __cdecl FreeVideoScImageFeatureResultV1(VideoScImageFeatureResultV1* result) {
    try { FreeVideoScImageFeatureResultV1Impl(result); } catch (...) {}
}

/** @brief 捕获结构面解码/构建异常并返回稳定错误结构。 */
VIDEOSC_API int __cdecl LoadImageStructureV1(
    const char* imagePath,
    const VideoScImageStructureOptionsV1* options,
    VideoScImageStructureResultV1* outResult) {
    if (outResult == nullptr || outResult->structSize < sizeof(VideoScImageStructureResultV1)) return 0;
    try {
        return LoadImageStructureV1Impl(imagePath, options, outResult);
    } catch (const std::bad_alloc&) {
        FreeVideoScImageStructureResultV1Impl(outResult);
        outResult->structSize = sizeof(VideoScImageStructureResultV1);
        return SetImageStructureError(outResult,
                                      VIDEOSC_ERR_OUT_OF_MEMORY,
                                      ERROR_NOT_ENOUGH_MEMORY,
                                      "Out of memory");
    } catch (const std::exception&) {
        FreeVideoScImageStructureResultV1Impl(outResult);
        outResult->structSize = sizeof(VideoScImageStructureResultV1);
        return SetImageStructureError(outResult,
                                      VIDEOSC_ERR_UNEXPECTED_FAILURE,
                                      ERROR_UNHANDLED_EXCEPTION,
                                      "Unexpected C++ exception");
    } catch (...) {
        FreeVideoScImageStructureResultV1Impl(outResult);
        outResult->structSize = sizeof(VideoScImageStructureResultV1);
        return SetImageStructureError(outResult,
                                      VIDEOSC_ERR_UNEXPECTED_FAILURE,
                                      ERROR_UNHANDLED_EXCEPTION,
                                      "Unknown exception");
    }
}

/** @brief 捕获结构比较异常并返回稳定错误结构。 */
VIDEOSC_API int __cdecl CompareImageStructuresV1(
    const void* leftHandle,
    const void* rightHandle,
    const VideoScImageStructureCompareOptionsV1* options,
    VideoScImageStructureCompareResultV1* outResult) {
    if (outResult == nullptr || outResult->structSize < sizeof(VideoScImageStructureCompareResultV1)) return 0;
    try {
        return CompareImageStructuresV1Impl(leftHandle, rightHandle, options, outResult);
    } catch (const std::bad_alloc&) {
        FreeVideoScImageStructureCompareResultV1Impl(outResult);
        outResult->structSize = sizeof(VideoScImageStructureCompareResultV1);
        outResult->statusCode = VIDEOSC_ERR_OUT_OF_MEMORY;
        outResult->errorMessage = DupString("Out of memory");
    } catch (const std::exception&) {
        FreeVideoScImageStructureCompareResultV1Impl(outResult);
        outResult->structSize = sizeof(VideoScImageStructureCompareResultV1);
        outResult->statusCode = VIDEOSC_ERR_UNEXPECTED_FAILURE;
        outResult->errorMessage = DupString("Unexpected C++ exception");
    } catch (...) {
        FreeVideoScImageStructureCompareResultV1Impl(outResult);
        outResult->structSize = sizeof(VideoScImageStructureCompareResultV1);
        outResult->statusCode = VIDEOSC_ERR_UNEXPECTED_FAILURE;
        outResult->errorMessage = DupString("Unknown exception");
    }
    return 0;
}

/** @brief 保证结构面句柄释放不会向调用方传播异常。 */
VIDEOSC_API void __cdecl FreeVideoScImageStructureResultV1(VideoScImageStructureResultV1* result) {
    try { FreeVideoScImageStructureResultV1Impl(result); } catch (...) {}
}

/** @brief 保证结构比较结果释放不会向调用方传播异常。 */
VIDEOSC_API void __cdecl FreeVideoScImageStructureCompareResultV1(
    VideoScImageStructureCompareResultV1* result) {
    try { FreeVideoScImageStructureCompareResultV1Impl(result); } catch (...) {}
}

/**
 * @brief 捕获图片预览生成异常并返回稳定结构化错误。
 * @return 成功返回 1，任何失败返回 0。
 */
VIDEOSC_API int __cdecl GenerateImagePreview(const char* sourcePath,
                                             const char* outputPath,
                                             const VideoScImagePreviewOptions* options,
                                             VideoScImagePreviewResult* outResult) {
    if (outResult == nullptr) return 0;
    *outResult = {};
    try {
        return GenerateImagePreviewImpl(sourcePath, outputPath, options, outResult);
    } catch (const std::bad_alloc&) {
        FreeVideoScImagePreviewResultImpl(outResult);
        return SetImagePreviewError(outResult,
                                    VIDEOSC_ERR_OUT_OF_MEMORY,
                                    ERROR_NOT_ENOUGH_MEMORY,
                                    "Out of memory");
    } catch (const std::exception&) {
        FreeVideoScImagePreviewResultImpl(outResult);
        return SetImagePreviewError(outResult,
                                    VIDEOSC_ERR_UNEXPECTED_FAILURE,
                                    ERROR_UNHANDLED_EXCEPTION,
                                    "Unexpected C++ exception");
    } catch (...) {
        FreeVideoScImagePreviewResultImpl(outResult);
        return SetImagePreviewError(outResult,
                                    VIDEOSC_ERR_UNEXPECTED_FAILURE,
                                    ERROR_UNHANDLED_EXCEPTION,
                                    "Unknown exception");
    }
}

/** @brief 保证图片预览结果释放函数不会把异常传播到调用方。 */
VIDEOSC_API void __cdecl FreeVideoScImagePreviewResult(VideoScImagePreviewResult* result) {
    try { FreeVideoScImagePreviewResultImpl(result); } catch (...) {}
}

/**
 * @brief 捕获流式哈希实现的 C++ 异常并保留结构化状态。
 * @return 成功返回 1，任何失败返回 0。
 */
VIDEOSC_API int __cdecl ComputeFileSHA512Ex(const char* filePath,
                                            const VideoScHashOptions* options,
                                            VideoScShouldCancel shouldCancel,
                                            void* cancelContext,
                                            VideoScFileHashResult* outResult) {
    if (outResult == nullptr) return 0;
    *outResult = {};
    try {
        return ComputeFileSHA512ExImpl(filePath, options, shouldCancel, cancelContext, outResult);
    } catch (const std::bad_alloc&) {
        *outResult = {};
        outResult->statusCode = VIDEOSC_HASH_OUT_OF_MEMORY;
    } catch (const std::exception&) {
        *outResult = {};
        outResult->statusCode = VIDEOSC_HASH_UNEXPECTED_FAILURE;
    } catch (...) {
        *outResult = {};
        outResult->statusCode = VIDEOSC_HASH_UNEXPECTED_FAILURE;
    }
    return 0;
}

/** @brief 捕获兼容 SHA-512 接口异常并清空输出。 */
VIDEOSC_API int __cdecl ComputeFileSHA512(const char* filePath, char* outHash, int outHashSize) {
    if (outHash != nullptr && outHashSize > 0) outHash[0] = '\0';
    try { return ComputeFileSHA512Impl(filePath, outHash, outHashSize); } catch (...) { return 0; }
}

/** @brief 捕获图片 dHash 接口异常并清空输出。 */
VIDEOSC_API int __cdecl ComputeImageDHash(const char* filePath, char* outHash, int outHashSize) {
    if (outHash != nullptr && outHashSize > 0) outHash[0] = '\0';
    try { return ComputeImageDHashImpl(filePath, outHash, outHashSize); } catch (...) { return 0; }
}

/** @brief 捕获汉明距离接口异常并返回原有非法值。 */
VIDEOSC_API int __cdecl ComputeHammingDistance(const char* hash1, const char* hash2) {
    try { return ComputeHammingDistanceImpl(hash1, hash2); } catch (...) { return -1; }
}

// ---------------------------------------------------------------------------
// DLL entry
// ---------------------------------------------------------------------------
BOOL APIENTRY DllMain(HMODULE hModule,
                      DWORD  ul_reason_for_call,
                      LPVOID lpReserved)
{
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
