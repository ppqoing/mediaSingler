// VideoScApp.cpp
//
// Implementation of the VideoScApp application class.
// Contains all application state and ImGui UI rendering logic, extracted
// from wWinMain in main.cpp. The D3D11/Win32 backend layer stays in main.cpp.

#include "VideoScApp.h"

#include "config/ConfigValidator.h"
#include "dedup/DuplicateReportService.h"
#include "deletion/DeletionService.h"
#include "persistence/DataVersionCoordinator.h"
#include "persistence/MySqlClient.h"
#include "persistence/MySqlBackup.h"
#include "persistence/MySqlSchema.h"
#include "persistence/MySqlSyncService.h"
#include "persistence/RocksStore.h"
#include "persistence/ScanCheckpointStore.h"
#include "persistence/SyncOperation.h"
#include "orchestration/ScanCoordinator.h"
#include "orchestration/ImageFeatureBackfillCoordinator.h"
#include "orchestration/ScanManifest.h"
#include "orchestration/ScanOptions.h"
#include "orchestration/ScanOptionsCodec.h"
#include "scheduling/FileHasher.h"
#include "diagnostics/CrashHandler.h"
#include "logging/ApplicationErrorLogger.h"
#include "logging/ExecutionLogger.h"
#include "logging/RuntimeLogFeed.h"

#include <windows.h>
#include <commdlg.h>
#include <shobjidl.h>     // IFileOpenDialog
#include <shellapi.h>     // ShellExecuteW
#include <string>
#include <vector>
#include <filesystem>
#include <cstdint>
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <wincodec.h>
#include <d3d11.h>        // for D3D11_TEXTURE2D_DESC / device calls in LoadImageToTexture
#pragma comment(lib, "windowscodecs.lib")

#include "imgui.h"
#include "imgui_internal.h"   // for DockBuilder API

// The D3D11 device is owned by RenderBackend and injected via SetD3DDevice().
// LoadImageToTexture creates textures on this device.

// -----------------------------------------------------------------------------
// UTF-8 helpers (ImGui expects UTF-8 strings; Win32 APIs use UTF-16)
// -----------------------------------------------------------------------------

/**
 * @brief 把以空字符结尾的 UTF-8 转换为 UTF-16。
 * @param text UTF-8 文本，允许为空指针。
 * @return 转换后的 UTF-16；非法输入返回空字符串。
 */
static std::wstring Utf8ToWide(const char* text) {
    if (!text || text[0] == '\0') return L"";
    const int sourceLength = static_cast<int>(std::strlen(text));
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, sourceLength, nullptr, 0);
    if (length <= 0) return L"";
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, sourceLength, wide.data(), length) != length) {
        return L"";
    }
    return wide;
}

/**
 * @brief 把以空字符结尾的 UTF-16 转换为 UTF-8。
 * @param text UTF-16 文本，允许为空指针。
 * @return 转换后的 UTF-8；非法输入返回空字符串。
 */
static std::string WideToUtf8(const wchar_t* text) {
    if (!text || text[0] == L'\0') return "";
    const int sourceLength = static_cast<int>(std::wcslen(text));
    const int length = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, text, sourceLength, nullptr, 0, nullptr, nullptr);
    if (length <= 0) return "";
    std::string utf8(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8,
                            WC_ERR_INVALID_CHARS,
                            text,
                            sourceLength,
                            utf8.data(),
                            length,
                            nullptr,
                            nullptr) != length) {
        return "";
    }
    return utf8;
}

static std::string DHashToHex(const std::uint64_t value) {
    std::ostringstream text;
    text << "0x" << std::hex << std::nouppercase << std::setfill('0') << std::setw(16) << value;
    return text.str();
}

/**
 * @brief 安全复制 UTF-8 字符串到固定 ImGui 编辑缓冲区。
 * @param destination 目标固定数组。
 * @param value 待复制文本，过长时截断并保证结尾空字符。
 */
template <std::size_t Size>
static void CopyToEditor(char (&destination)[Size], const std::string& value) {
    strncpy_s(destination, value.c_str(), _TRUNCATE);
}

/**
 * @brief 将配置加载告警合并成单条可换行 GUI 消息。
 * @param warnings UTF-16 告警列表。
 * @return UTF-8 告警文本。
 */
static std::string JoinWarnings(const std::vector<std::wstring>& warnings) {
    std::string result;
    for (const std::wstring& warning : warnings) {
        if (!result.empty()) {
            result += "\n";
        }
        result += WideToUtf8(warning.c_str());
    }
    return result;
}

/** @brief 把扫描阶段转换为面向用户的中文名称。 */
static const char* ScanPhaseName(const videosc::dedup::ScanPhase phase) {
    using videosc::dedup::ScanPhase;
    switch (phase) {
        case ScanPhase::Idle: return "空闲";
        case ScanPhase::Discovering: return "发现文件";
        case ScanPhase::Hashing: return "计算 SHA-512";
        case ScanPhase::ExtractingMedia: return "提取媒体特征";
        case ScanPhase::Syncing: return "同步 MySQL";
        case ScanPhase::Matching: return "生成重复报告";
        case ScanPhase::Deleting: return "永久删除";
        case ScanPhase::Paused: return "已暂停";
        case ScanPhase::CompletedLocal: return "本地扫描完成";
        case ScanPhase::CompletedSynchronized: return "本地与 MySQL 同步完成";
        case ScanPhase::Cancelled: return "已取消";
        case ScanPhase::Failed: return "失败";
        case ScanPhase::Interrupted: return "已中断，可恢复";
        case ScanPhase::Planning: return "联合本地与 MySQL 规划任务";
        case ScanPhase::FlushingSyncTail: return "发布 MySQL 同步尾批";
    }
    return "未知";
}

/** @brief 把单路径发现后端转换为界面名称。 */
static const char* DiscoveryBackendName(const videosc::dedup::DiscoveryBackend backend) {
    using videosc::dedup::DiscoveryBackend;
    switch (backend) {
        case DiscoveryBackend::Pending: return "等待";
        case DiscoveryBackend::Everything: return "Everything";
        case DiscoveryBackend::Native: return "系统遍历";
        case DiscoveryBackend::EverythingThenNative: return "Everything → 系统遍历";
    }
    return "未知";
}

/** @brief 把单路径文件发现阶段转换为界面名称。 */
static const char* DiscoveryRootPhaseName(const videosc::dedup::DiscoveryRootPhase phase) {
    using videosc::dedup::DiscoveryRootPhase;
    switch (phase) {
        case DiscoveryRootPhase::Waiting: return "等待";
        case DiscoveryRootPhase::PreparingEverything: return "准备 Everything";
        case DiscoveryRootPhase::QueryingEverything: return "查询 Everything 索引";
        case DiscoveryRootPhase::ProcessingEverythingResults: return "处理索引结果";
        case DiscoveryRootPhase::QueryingPhysicalLocation: return "查询物理位置";
        case DiscoveryRootPhase::ScanningNative: return "系统原生遍历";
        case DiscoveryRootPhase::NativeFallback: return "回退系统遍历";
        case DiscoveryRootPhase::Completed: return "完成";
        case DiscoveryRootPhase::Cancelling: return "正在取消";
        case DiscoveryRootPhase::Cancelled: return "已取消";
        case DiscoveryRootPhase::Failed: return "失败";
    }
    return "未知";
}

static std::string FormatElapsedMilliseconds(const std::uint64_t milliseconds) {
    std::ostringstream stream;
    if (milliseconds < 1000) {
        stream << milliseconds << " ms";
    } else {
        stream << std::fixed << std::setprecision(1)
               << static_cast<double>(milliseconds) / 1000.0 << " s";
    }
    return stream.str();
}

/**
 * @brief 返回扫描主流水线中的阶段序号。
 * @param phase 当前扫描阶段。
 * @return 发现、哈希、媒体、同步分别返回 1 至 4；非主流水线状态返回 0。
 */
static int ScanStageNumber(const videosc::dedup::ScanPhase phase) {
    using videosc::dedup::ScanPhase;
    switch (phase) {
        case ScanPhase::Discovering: return 1;
        case ScanPhase::Planning: return 1;
        case ScanPhase::Hashing: return 2;
        case ScanPhase::ExtractingMedia: return 3;
        case ScanPhase::Syncing:
        case ScanPhase::FlushingSyncTail:
        case ScanPhase::CompletedLocal:
        case ScanPhase::CompletedSynchronized: return 4;
        default: return 0;
    }
}

/**
 * @brief 计算并限制确定型阶段进度。
 * @param processed 已处理任务数。
 * @param total 本阶段任务总数；零表示该阶段无需处理任务。
 * @return 位于 `[0, 1]` 的进度值。
 */
static float CalculateStageProgress(const std::uint64_t processed, const std::uint64_t total) {
    if (total == 0) return 1.0f;
    const double fraction = static_cast<double>(processed) / static_cast<double>(total);
    return static_cast<float>(std::clamp(fraction, 0.0, 1.0));
}

/**
 * @brief 生成确定型进度条的计数与百分比覆盖文字。
 * @param processed 已处理任务数。
 * @param total 本阶段任务总数。
 * @return `已处理 P / T（X%）` 格式的 UTF-8 文本。
 */
static std::string FormatStageProgress(const std::uint64_t processed, const std::uint64_t total) {
    std::ostringstream stream;
    const double percentage = total == 0
                                  ? 100.0
                                  : std::clamp(static_cast<double>(processed) * 100.0 /
                                                   static_cast<double>(total),
                                               0.0,
                                               100.0);
    stream << "已处理 " << processed << " / " << total << "（" << std::fixed << std::setprecision(1)
           << percentage << "%）";
    return stream.str();
}

/**
 * @brief 计算不透明 RGB 反色，用于进度条文字与所在底色保持对比。
 * @param background 当前实际背景色。
 * @return RGB 逐通道取反且 alpha 固定为 1 的颜色。
 */
static ImVec4 InvertProgressColor(const ImVec4& background) {
    return ImVec4(1.0f - background.x, 1.0f - background.y, 1.0f - background.z, 1.0f);
}

/**
 * @brief 绘制覆盖文字按填充区和背景区分别取反色的业务进度条。
 *
 * 先使用 ImGui 原生控件绘制进度条本体，再按实际填充区裁剪绘制两种反色文字。
 * 负进度值沿用 ImGui 的不确定动画语义；传入固定负值可冻结动画色块。
 *
 * @param fraction `[0, 1]` 为确定进度，负数为不确定动画参数。
 * @param size 进度条尺寸，遵循 ImGui::ProgressBar 的尺寸规则。
 * @param overlay 居中显示的 UTF-8 覆盖文字，允许为空指针。
 */
static void DrawContrastProgressBar(const float fraction,
                                    const ImVec2& size,
                                    const char* overlay) {
    ImGui::ProgressBar(fraction, size, "");
    if (overlay == nullptr || overlay[0] == '\0') return;

    const ImVec2 barMin = ImGui::GetItemRectMin();
    const ImVec2 barMax = ImGui::GetItemRectMax();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float border = style.FrameBorderSize;
    const ImVec2 innerMin(barMin.x + border, barMin.y + border);
    const ImVec2 innerMax(barMax.x - border, barMax.y - border);
    if (innerMax.x <= innerMin.x || innerMax.y <= innerMin.y) return;

    const bool indeterminate = fraction < 0.0f;
    float fillStart = 0.0f;
    float fillEnd = std::clamp(fraction, 0.0f, 1.0f);
    if (indeterminate) {
        constexpr float fillWidth = 0.2f;
        fillStart = std::fmod(-fraction, 1.0f) * (1.0f + fillWidth) - fillWidth;
        fillEnd = std::clamp(fillStart + fillWidth, 0.0f, 1.0f);
        fillStart = std::clamp(fillStart, 0.0f, 1.0f);
    }

    const float fillX0 = innerMin.x + (innerMax.x - innerMin.x) * fillStart;
    const float fillX1 = innerMin.x + (innerMax.x - innerMin.x) * fillEnd;
    const ImVec2 textSize = ImGui::CalcTextSize(overlay);
    const ImVec2 textPosition(
        (barMin.x + barMax.x - textSize.x) * 0.5f,
        barMin.y + (barMax.y - barMin.y - textSize.y) * 0.5f);
    ImDrawList* const drawList = ImGui::GetWindowDrawList();
    const ImU32 backgroundTextColor =
        ImGui::GetColorU32(InvertProgressColor(ImGui::GetStyleColorVec4(ImGuiCol_FrameBg)));
    const ImU32 fillTextColor =
        ImGui::GetColorU32(InvertProgressColor(ImGui::GetStyleColorVec4(ImGuiCol_PlotHistogram)));

    const auto drawClippedText = [&](const float clipMinX,
                                     const float clipMaxX,
                                     const ImU32 color) {
        if (clipMaxX <= clipMinX) return;
        drawList->PushClipRect(ImVec2(clipMinX, innerMin.y),
                               ImVec2(clipMaxX, innerMax.y),
                               true);
        drawList->AddText(textPosition, color, overlay);
        drawList->PopClipRect();
    };

    drawClippedText(innerMin.x, fillX0, backgroundTextColor);
    drawClippedText(fillX1, innerMax.x, backgroundTextColor);
    drawClippedText(fillX0, fillX1, fillTextColor);
}

/** @brief 以适合表格的单位格式化字节数。 */
static std::string FormatBytes(const std::uint64_t bytes) {
    constexpr std::array<const char*, 5> units = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < units.size()) {
        value /= 1024.0;
        ++unit;
    }
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(unit == 0 ? 0 : 2) << value << ' ' << units[unit];
    return stream.str();
}

/**
 * @brief 将 Unix UTC 毫秒时间戳格式化为稳定的详情文本。
 * @param utc_milliseconds UTC 毫秒时间戳；非正数表示未知。
 * @return `YYYY-MM-DD HH:MM:SS UTC` 或“未知”。
 */
static std::string FormatUtcTimestamp(const std::int64_t utc_milliseconds) {
    if (utc_milliseconds <= 0) return "未知";
    const std::time_t seconds = static_cast<std::time_t>(utc_milliseconds / 1000);
    std::tm utc{};
    if (gmtime_s(&utc, &seconds) != 0) return "未知";
    std::ostringstream stream;
    stream << std::put_time(&utc, "%Y-%m-%d %H:%M:%S") << " UTC";
    return stream.str();
}

/**
 * @brief 返回实时日志级别的中文名称。
 * @param severity 日志级别。
 * @return 进程生命周期内有效的中文常量。
 */
static const char* RuntimeLogSeverityName(const videosc::dedup::RuntimeLogSeverity severity) {
    switch (severity) {
        case videosc::dedup::RuntimeLogSeverity::Info: return "信息";
        case videosc::dedup::RuntimeLogSeverity::Warning: return "警告";
        case videosc::dedup::RuntimeLogSeverity::Error: return "错误";
    }
    return "未知";
}

/**
 * @brief 返回实时日志级别对应的高对比度文本颜色。
 * @param severity 日志级别。
 * @return 当前主题可读的 RGBA 颜色。
 */
static ImVec4 RuntimeLogSeverityColor(const videosc::dedup::RuntimeLogSeverity severity) {
    switch (severity) {
        case videosc::dedup::RuntimeLogSeverity::Info:
            return ImVec4(0.65f, 0.89f, 0.63f, 1.0f);
        case videosc::dedup::RuntimeLogSeverity::Warning:
            return ImVec4(0.98f, 0.70f, 0.53f, 1.0f);
        case videosc::dedup::RuntimeLogSeverity::Error:
            return ImVec4(0.95f, 0.55f, 0.66f, 1.0f);
    }
    return ImGui::GetStyleColorVec4(ImGuiCol_Text);
}

/**
 * @brief 返回运行日志任务筛选的中文名称。
 * @param filter 任务筛选枚举。
 * @return 进程生命周期内有效的中文常量。
 */
static const char* RuntimeLogTaskFilterName(const RuntimeLogTaskFilter filter) {
    switch (filter) {
        case RuntimeLogTaskFilter::All: return "全部任务";
        case RuntimeLogTaskFilter::Scan: return "扫描";
        case RuntimeLogTaskFilter::VisualCompute: return "视觉计算";
        case RuntimeLogTaskFilter::MySql: return "MySQL";
        case RuntimeLogTaskFilter::Report: return "报告";
        case RuntimeLogTaskFilter::Deletion: return "删除";
        case RuntimeLogTaskFilter::Application: return "应用异常";
    }
    return "全部任务";
}

/**
 * @brief 对 ASCII 字母执行不区分大小写的子串匹配，中文和路径字符保持原值。
 * @param text 被搜索文本。
 * @param query 用户关键字。
 * @return 空关键字或匹配成功时返回 true。
 */
static bool ContainsRuntimeLogQuery(const std::string& text, const std::string& query) {
    if (query.empty()) return true;
    if (query.size() > text.size()) return false;
    return std::search(text.begin(), text.end(), query.begin(), query.end(), [](const char left, const char right) {
               const unsigned char leftByte = static_cast<unsigned char>(left);
               const unsigned char rightByte = static_cast<unsigned char>(right);
               return std::tolower(leftByte) == std::tolower(rightByte);
           }) != text.end();
}

/**
 * @brief 判断运行日志是否属于当前任务筛选。
 * @param entry 结构化日志。
 * @param filter 当前任务筛选。
 * @return 匹配时返回 true。
 */
static bool MatchesRuntimeLogTask(const videosc::dedup::RuntimeLogEntry& entry,
                                  const RuntimeLogTaskFilter filter) {
    if (filter == RuntimeLogTaskFilter::All) return true;
    const std::string context = entry.task + " " + entry.stage + " " + entry.operation;
    switch (filter) {
        case RuntimeLogTaskFilter::Scan:
            return entry.task == "scan" || context.find("scan") != std::string::npos;
        case RuntimeLogTaskFilter::VisualCompute:
            return context.find("image") != std::string::npos ||
                   context.find("visual") != std::string::npos ||
                   context.find("structure") != std::string::npos ||
                   context.find("dhash") != std::string::npos ||
                   context.find("phash") != std::string::npos ||
                   context.find("pdq") != std::string::npos;
        case RuntimeLogTaskFilter::MySql:
            return context.find("mysql") != std::string::npos ||
                   entry.task == "mysql_sync";
        case RuntimeLogTaskFilter::Report:
            return entry.task == "duplicate_report" || context.find("report") != std::string::npos;
        case RuntimeLogTaskFilter::Deletion:
            return context.find("delete") != std::string::npos ||
                   context.find("deletion") != std::string::npos ||
                   entry.task == "permanent_delete";
        case RuntimeLogTaskFilter::Application:
            return entry.task == "application" || entry.task == "logging";
        case RuntimeLogTaskFilter::All:
            return true;
    }
    return true;
}

/**
 * @brief 把一条结构化日志格式化为剪贴板单行文本。
 * @param entry 日志条目。
 * @return 与表格可见字段一致的 UTF-8 文本。
 */
static std::string FormatRuntimeLogLine(const videosc::dedup::RuntimeLogEntry& entry) {
    std::ostringstream stream;
    stream << '[' << FormatUtcTimestamp(entry.utc_ms) << "] ["
           << RuntimeLogSeverityName(entry.severity) << "] 任务=" << entry.task;
    if (entry.task_id != 0) stream << '#' << entry.task_id;
    stream << " 阶段=" << entry.stage << " 操作=" << entry.operation;
    if (entry.status_code != 0) stream << " 状态码=" << entry.status_code;
    if (entry.native_error != 0) stream << " 系统错误=" << entry.native_error;
    if (!entry.subject.empty()) stream << " 定位=" << entry.subject;
    stream << " 说明=" << entry.message;
    if (entry.repeat_count > 1) stream << " 重复=" << entry.repeat_count;
    return stream.str();
}

/** @brief 报告类别的中文标签。 */
static const char* ReportKindName(const videosc::dedup::DuplicateReportKind kind) {
    return kind == videosc::dedup::DuplicateReportKind::Exact ? "SHA-512 精确重复" : "视觉内容相似";
}

/**
 * @brief 返回重复组排序方式的桌面界面标签。
 * @param mode 当前排序方式。
 * @return 生命周期覆盖整个进程的只读中文标签。
 */
static const char* ReportSortModeName(const ReportSortMode mode) {
    switch (mode) {
    case ReportSortMode::Generated: return "生成顺序";
    case ReportSortMode::ReclaimableAscending: return "可释放大小（升序）";
    case ReportSortMode::ReclaimableDescending: return "可释放大小（降序）";
    case ReportSortMode::MemberCountAscending: return "组内数量（升序）";
    case ReportSortMode::MemberCountDescending: return "组内数量（降序）";
    case ReportSortMode::DHashAverageAscending: return "平均视觉距离（升序）";
    case ReportSortMode::DHashAverageDescending: return "平均视觉距离（降序）";
    }
    return "生成顺序";
}

/** @brief 把删除策略结果收窄为通过 dHash 严格小于安全上限的持久选择。 */
static std::vector<videosc::dedup::ReportSelectionMember> BuildSafeSelection(
    const videosc::dedup::DuplicateGroup& source,
    const videosc::dedup::DuplicateGroup& planned,
    const videosc::dedup::ReportSelectionConfig& config,
    const std::uint32_t imageReportMaximum,
    const std::uint32_t videoReportMaximum,
    const bool imageReportAlreadyThreeStageVerified,
    std::uint64_t& rejected) {
    std::vector<videosc::dedup::ReportSelectionMember> result;
    if (!planned.retained_path_id.has_value()) return result;
    const auto retained = std::find_if(source.members.begin(), source.members.end(), [&](const auto& member) {
        return member.path_id == *planned.retained_path_id;
    });
    if (retained == source.members.end()) return result;

    result.reserve(planned.selected_for_deletion.size());
    for (const std::uint64_t pathId : planned.selected_for_deletion) {
        const auto candidate = std::find_if(source.members.begin(), source.members.end(), [&](const auto& member) {
            return member.path_id == pathId;
        });
        if (candidate == source.members.end()) {
            ++rejected;
            continue;
        }
        const videosc::dedup::ReportSelectionDecision decision =
            videosc::dedup::ReportSelectionRules::Evaluate(
                source,
                *retained,
                *candidate,
                config,
                imageReportMaximum,
                videoReportMaximum,
                imageReportAlreadyThreeStageVerified);
        if (!decision.allowed) {
            ++rejected;
            continue;
        }
        result.push_back({candidate->path_id,
                          candidate->size_bytes,
                          retained->path_id,
                          decision.has_measured_distance,
                          decision.measured_distance,
                          decision.exclusive_limit});
    }
    return result;
}

/** @brief 将持久选择成员合并到可丢弃的报告组渲染模型。 */
static void ApplySelectionToGroup(
    videosc::dedup::DuplicateGroup& group,
    const std::vector<videosc::dedup::ReportSelectionMember>& selection) {
    group.selected_for_deletion.clear();
    group.retained_path_id.reset();
    for (const auto& member : selection) {
        group.selected_for_deletion.push_back(member.path_id);
        if (!group.retained_path_id.has_value()) group.retained_path_id = member.retained_path_id;
    }
}

/** @brief 删除前按选择记录冻结的严格上限重新校验，并构造唯一删除计划。 */
static std::optional<videosc::dedup::DuplicateGroup> BuildPersistedDeletionPlan(
    const videosc::dedup::DuplicateGroup& source,
    const std::vector<videosc::dedup::ReportSelectionMember>& selection,
    const bool trustedThreeStageReport,
    std::uint64_t& safetyRejected) {
    if (selection.empty()) return std::nullopt;
    const std::uint64_t retainedPathId = selection.front().retained_path_id;
    const auto retained = std::find_if(source.members.begin(), source.members.end(), [&](const auto& member) {
        return member.path_id == retainedPathId;
    });
    if (retained == source.members.end()) {
        safetyRejected += selection.size();
        return std::nullopt;
    }

    videosc::dedup::DuplicateGroup plan = source;
    plan.retained_path_id = retainedPathId;
    plan.selected_for_deletion.clear();
    for (const auto& selected : selection) {
        if (selected.retained_path_id != retainedPathId) {
            ++safetyRejected;
            continue;
        }
        const auto candidate = std::find_if(source.members.begin(), source.members.end(), [&](const auto& member) {
            return member.path_id == selected.path_id;
        });
        if (candidate == source.members.end()) {
            ++safetyRejected;
            continue;
        }
        videosc::dedup::ReportSelectionConfig frozen;
        if (source.kind == videosc::dedup::DuplicateGroupKind::SimilarImage) {
            frozen.image_dhash_distance_exclusive_limit =
                static_cast<std::uint32_t>(selected.exclusive_limit);
        } else if (source.kind == videosc::dedup::DuplicateGroupKind::SimilarVideo) {
            frozen.video_dhash_average_distance_exclusive_limit = selected.exclusive_limit;
        }
        const auto decision = source.kind == videosc::dedup::DuplicateGroupKind::SimilarImage &&
                                      !selected.has_measured_distance
                                  ? (trustedThreeStageReport
                                         ? videosc::dedup::ReportSelectionDecision{true, false, 0.0, 0.0, {}}
                                         : videosc::dedup::ReportSelectionDecision{
                                               false, false, 0.0, 0.0, "untrusted_three_stage_report"})
                                  : videosc::dedup::ReportSelectionRules::Evaluate(
                                        source, *retained, *candidate, frozen, 0, 0);
        if (!decision.allowed) {
            ++safetyRejected;
            continue;
        }
        plan.selected_for_deletion.push_back(selected.path_id);
    }
    if (plan.selected_for_deletion.empty() ||
        plan.selected_for_deletion.size() >= plan.members.size()) {
        return std::nullopt;
    }
    return plan;
}

/**
 * @brief 把视觉相似报告的稳定阶段键转换为中文名称。
 * @param stage 核心报告生成器发布的阶段键。
 * @return 面向用户的中文阶段名称。
 */
static const char* DHashReportStageName(const std::string& stage) {
    if (stage == "backfilling_image_features") return "审计并回填历史图片三级特征";
    if (stage == "streaming_mysql_visual_contents") return "读取 MySQL 视觉内容";
    if (stage == "preparing_visual_representatives") return "准备图片代表并压缩相同视频签名";
    if (stage == "enumerating_primary_candidates") return "PDQ 分桶与完整距离初筛";
    if (stage == "validating_secondary_phash") return "并行执行分区 pHash 二筛";
    if (stage == "loading_structure_paths") return "加载结构直验路径";
    if (stage == "validating_image_structure") return "并行执行图片结构三筛";
    if (stage == "grouping_strict_similarity") return "严格分组并整理相似关系";
    if (stage == "joining_active_paths") return "关联有效文件路径";
    if (stage == "writing_similarity_groups") return "写入相似分组与关系";
    if (stage == "completed") return "已完成";
    return "准备视觉相似计算";
}

/**
 * @brief 把跳过视觉内容原因转换为中文名称。
 * @param reason 跳过记录中的原因枚举。
 * @return 面向用户的中文原因名称。
 */
static const char* SkippedReasonName(const videosc::dedup::SkippedVisualContentReason reason) {
    using videosc::dedup::SkippedVisualContentReason;
    switch (reason) {
        case SkippedVisualContentReason::InvalidImage: return "图片三级特征未完成";
        case SkippedVisualContentReason::MissingVideoDHash: return "视频六帧 dHash 缺失";
        case SkippedVisualContentReason::ZeroVideoFrame: return "视频 dHash 含零帧";
        case SkippedVisualContentReason::UnsupportedMedia: return "不支持的媒体";
        case SkippedVisualContentReason::StructuralIoFailure: return "结构三筛读取/解码失败";
        case SkippedVisualContentReason::StructuralTimeout: return "结构三筛读取超时";
        case SkippedVisualContentReason::StructuralComputeFailure: return "结构三筛计算失败";
        case SkippedVisualContentReason::Count: break;
    }
    return "未知原因";
}

/**
 * @brief 生成图片特征回填未完整时的中文警告摘要。
 * @param result 回填最终状态及结构化失败统计。
 * @return 包含完整度、失败分类和继续策略的单行文本。
 */
static std::string FormatIncompleteImageBackfillWarning(
    const videosc::dedup::ImageFeatureBackfillResult& result) {
    const auto& completeness = result.completeness;
    const auto& progress = result.final_progress;
    const std::uint64_t categorized = progress.no_readable_path_images +
                                      progress.timeout_images +
                                      progress.decode_failed_images;
    std::string message =
        "图片三级特征回填未完成：完整 " + std::to_string(completeness.complete_images) + "/" +
        std::to_string(completeness.total_images) + "，未解决 " +
        std::to_string(completeness.incomplete_images) + "；无可读路径 " +
        std::to_string(progress.no_readable_path_images) + "，超时 " +
        std::to_string(progress.timeout_images) + "，解码或特征生成失败 " +
        std::to_string(progress.decode_failed_images) + "。";
    if (categorized != completeness.incomplete_images) {
        message += " 分类数与当前作用域不一致，可能存在并发路径变化或历史异常记录。";
    }
    message += " 报告将跳过仍不完整的图片并继续视觉三级相似计算；详情可在运行日志中筛选操作=image_perceptual_features。";
    return message;
}

/** @brief 汇总配置校验中的阻断错误。 */
static std::string JoinValidationErrors(const std::vector<videosc::dedup::ValidationIssue>& issues) {
    std::string result;
    for (const auto& issue : issues) {
        if (issue.severity != videosc::dedup::ValidationSeverity::Error) continue;
        if (!result.empty()) result += "\n";
        result += WideToUtf8(issue.message.c_str());
    }
    return result;
}

/**
 * @brief 将后台或 GUI 边界异常写入统一日志。
 * @param module 发生异常的模块。
 * @param operation 当前操作名。
 * @param message 不含敏感信息的错误说明。
 * @param exception_type 标准或未知异常类别。
 * @return 可供界面关联日志的异常 ID。
 */
static std::string LogApplicationException(const char* module,
                                           const char* operation,
                                           const std::string& message,
                                           const char* exception_type) noexcept {
    videosc::dedup::ApplicationErrorRecord record;
    record.module = module;
    record.operation = operation;
    record.message = message;
    record.exception_type = exception_type;
    return videosc::dedup::ApplicationErrorLogger::Write(record);
}

/** @brief 将 DLL 取消回调桥接到应用生命周期原子标志。 */
static int __cdecl AtomicCancelCallback(void* context) noexcept {
    const auto* requested = static_cast<const std::atomic_bool*>(context);
    return requested != nullptr && requested->load(std::memory_order_relaxed) ? 1 : 0;
}

/**
 * @brief 创建后台线程并将资源不足等创建失败转换为当前功能错误。
 * @tparam Function 可在线程中移动调用的函数对象。
 * @param destination 接收新线程的空闲线程对象。
 * @param function 线程入口。
 * @param operation 日志操作名。
 * @param error 接收带异常 ID 的中文错误。
 * @return 创建成功返回 true。
 */
template <typename Function>
static bool StartBackgroundThread(std::thread& destination,
                                  Function&& function,
                                  const char* operation,
                                  std::string& error) noexcept {
    try {
        destination = std::thread(std::forward<Function>(function));
        return true;
    } catch (const std::exception& exception) {
        error = std::string("无法创建后台线程：") + exception.what();
        error += "（" + LogApplicationException("VideoScGUI", operation, error, "std::exception") + "）";
    } catch (...) {
        error = "无法创建后台线程：未知异常";
        error += "（" + LogApplicationException("VideoScGUI", operation, error, "unknown_exception") + "）";
    }
    return false;
}

// -----------------------------------------------------------------------------
// Button color helpers: push tinted button colors for different action types.
// Each PushButtonStyle must be balanced with ImGui::PopStyleColor(3).
// Palette aligns with the soft dark theme in main.cpp::ApplyAppStyle.
//   - Primary:   blue (main actions: 开始截图 / 开始检索 / 查询物理磁盘)
//   - Danger:    red   (destructive: 取消)
//   - Accent:    peach (compute actions: SHA-512 / dHash / 汉明距离)
//   - Success:   green (quick-fill: 使用当前视频路径 / 使用输出目录)
//   - Neutral:   surface (secondary: 浏览... / 打开输出目录)
// -----------------------------------------------------------------------------
static const ImVec4 BtnPrimaryHover  = ImVec4(0.541f, 0.706f, 0.980f, 0.65f);  // #89b4fa blue
static const ImVec4 BtnPrimaryActive = ImVec4(0.541f, 0.706f, 0.980f, 0.85f);
static const ImVec4 BtnDangerHover  = ImVec4(0.953f, 0.545f, 0.659f, 0.65f);  // #f38ba8 red
static const ImVec4 BtnDangerActive = ImVec4(0.953f, 0.545f, 0.659f, 0.85f);
static const ImVec4 BtnAccentHover  = ImVec4(0.980f, 0.702f, 0.529f, 0.65f);  // #fab387 peach
static const ImVec4 BtnAccentActive = ImVec4(0.980f, 0.702f, 0.529f, 0.85f);
static const ImVec4 BtnSuccessHover = ImVec4(0.651f, 0.890f, 0.631f, 0.55f);  // #a6e3a1 green
static const ImVec4 BtnSuccessActive= ImVec4(0.651f, 0.890f, 0.631f, 0.80f);
static const ImVec4 BtnNeutralHover = ImVec4(0.706f, 0.745f, 0.878f, 0.30f);  // #b4befe lavender dim
static const ImVec4 BtnNeutralActive= ImVec4(0.706f, 0.745f, 0.878f, 0.50f);

static void PushButtonStylePrimary() {
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.541f, 0.706f, 0.980f, 0.40f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, BtnPrimaryHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  BtnPrimaryActive);
}
static void PushButtonStyleDanger() {
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.953f, 0.545f, 0.659f, 0.35f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, BtnDangerHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  BtnDangerActive);
}
static void PushButtonStyleAccent() {
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.980f, 0.702f, 0.529f, 0.35f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, BtnAccentHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  BtnAccentActive);
}
static void PushButtonStyleSuccess() {
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.651f, 0.890f, 0.631f, 0.30f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, BtnSuccessHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  BtnSuccessActive);
}
static void PushButtonStyleNeutral() {
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.192f, 0.196f, 0.267f, 1.0f));  // surface0
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, BtnNeutralHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  BtnNeutralActive);
}

// -----------------------------------------------------------------------------
// Constructor / destructor
// -----------------------------------------------------------------------------

VideoScApp::VideoScApp()
    : m_configStore(videosc::dedup::JsonConfigStore::ForApplicationDirectory()) {
    m_result.statusCode = -1; // not run yet
    InitializeReportPreviewDirectory();
    ReloadConfiguration();
    m_previousCrashMetadata = CrashHandler::LatestUnreviewedCrash();
    m_previousCrashPopupRequested = !m_previousCrashMetadata.empty();
}

void VideoScApp::InitializeReportPreviewDirectory() {
    std::error_code error;
    const std::filesystem::path root = std::filesystem::temp_directory_path(error) / L"VideoScGUI";
    if (error) return;
    std::filesystem::create_directories(root, error);
    if (error) return;
    for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
        if (error || !entry.is_directory(error)) continue;
        const std::wstring name = entry.path().filename().wstring();
        if (name.rfind(L"preview-", 0) != 0) continue;
        const std::size_t separator = name.find(L'-', 8);
        if (separator == std::wstring::npos) continue;
        const DWORD processId = static_cast<DWORD>(_wcstoui64(name.substr(8, separator - 8).c_str(), nullptr, 10));
        bool processAlive = false;
        if (processId != 0) {
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
            if (process) {
                DWORD exitCode = 0;
                processAlive = GetExitCodeProcess(process, &exitCode) != FALSE && exitCode == STILL_ACTIVE;
                CloseHandle(process);
            }
        }
        if (!processAlive) std::filesystem::remove_all(entry.path(), error);
        error.clear();
    }
    m_reportPreviewDirectory = root / (L"preview-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                       std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(m_reportPreviewDirectory, error);
    if (error) m_reportPreviewDirectory.clear();
}

void VideoScApp::CleanupReportPreviewDirectory() noexcept {
    for (auto& job : m_imagePreviewJobs) {
        try { job.second.wait(); } catch (...) {}
    }
    m_imagePreviewJobs.clear();
    for (auto& job : m_videoPreviewJobs) {
        try { job.second.wait(); } catch (...) {}
    }
    m_videoPreviewJobs.clear();
    m_generatedVideoPreviewFiles.clear();
    m_reportImagePreviewFiles.clear();
    m_imagePreviewFailures.clear();
    if (m_reportPreviewDirectory.empty()) return;
    std::error_code error;
    std::filesystem::remove_all(m_reportPreviewDirectory, error);
    m_reportPreviewDirectory.clear();
}

std::filesystem::path VideoScApp::AcquireImagePreview(
    const videosc::dedup::DuplicateMember& member,
    bool& pending,
    std::string& failure) {
    pending = false;
    failure.clear();
    if (member.path.empty() || m_reportPreviewDirectory.empty()) {
        failure = "图片路径或临时预览目录不可用";
        return {};
    }

    const std::string shaHex = videosc::dedup::Sha512ToHex(member.content_sha512);
    const wchar_t* extension = m_config.thumbnails.format == videosc::dedup::ThumbnailFormat::Png
                                   ? L".png"
                                   : L".jpg";
    const std::wstring key = Utf8ToWide(
        (shaHex + "-" + std::to_string(m_config.thumbnails.image_preview_long_edge) +
         (extension[1] == L'p' ? "-png" : "-jpg")).c_str());
    const std::wstring sourceFailureKey = key + L"|" + member.path.wstring();

    // 可见成员每帧都会经过此入口，因此顺便收取已经滚出可见区的任务，避免其永久占用并发槽位。
    for (auto iterator = m_imagePreviewJobs.begin(); iterator != m_imagePreviewJobs.end();) {
        if (iterator->second.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
            ++iterator;
            continue;
        }
        const std::wstring completedKey = iterator->first;
        ImagePreviewJobResult result;
        try {
            result = iterator->second.get();
        } catch (const std::exception& exception) {
            result.detail = exception.what();
            result.failure_logged = false;
        } catch (...) {
            result.detail = "图片缩略图后台任务发生未知异常";
            result.failure_logged = false;
        }
        iterator = m_imagePreviewJobs.erase(iterator);
        if (!result.output_path.empty() && std::filesystem::exists(result.output_path)) {
            m_reportImagePreviewFiles[completedKey] = result.output_path;
            continue;
        }
        const std::string detail = result.detail.empty() ? "图片缩略图生成失败" : result.detail;
        m_imagePreviewFailures[completedKey + L"|" + result.source_path.wstring()] = detail;
        m_reportMessage = result.failure_logged
                              ? "图片预览生成失败，详情已写入执行失败日志。"
                              : "图片预览生成失败，且执行失败日志写入失败。";
        m_reportMessageIsError = true;
    }

    const auto generated = m_reportImagePreviewFiles.find(key);
    if (generated != m_reportImagePreviewFiles.end()) {
        if (!generated->second.empty() && std::filesystem::exists(generated->second)) {
            return generated->second;
        }
        const auto failed = m_imagePreviewFailures.find(sourceFailureKey);
        failure = failed != m_imagePreviewFailures.end() ? failed->second : "图片缩略图生成失败";
        return {};
    }
    const auto failed = m_imagePreviewFailures.find(sourceFailureKey);
    if (failed != m_imagePreviewFailures.end()) {
        failure = failed->second;
        return {};
    }

    auto job = m_imagePreviewJobs.find(key);
    if (job != m_imagePreviewJobs.end()) {
        pending = true;
        return {};
    }

    constexpr std::size_t maximumConcurrentImageJobs = 2;
    if (m_imagePreviewJobs.size() >= maximumConcurrentImageJobs) {
        pending = true;
        return {};
    }

    const std::size_t filenameHash = std::hash<std::wstring>{}(key);
    const std::filesystem::path target =
        m_reportPreviewDirectory / (L"image-" + std::to_wstring(filenameHash) + extension);
    const std::string inputUtf8 = WideToUtf8(member.path.wstring().c_str());
    const std::string targetUtf8 = WideToUtf8(target.wstring().c_str());
    const std::uint32_t maximumLongEdge = m_config.thumbnails.image_preview_long_edge;
    const std::uint32_t ffmpegThreads = m_config.compute.ffmpeg_threads_per_task;
    const std::uint32_t timeoutMilliseconds = m_config.io.no_progress_timeout_seconds * 1000U;
    const videosc::dedup::LoggingConfig logging = m_config.logging;
    const std::uint64_t pathId = member.path_id;
    const std::wstring storageTarget = member.storage_target_key;
    std::atomic_bool* const previewCancel = &m_previewCancelRequested;
    m_imagePreviewJobs.emplace(
        key,
        std::async(std::launch::async,
                   [inputUtf8,
                    targetUtf8,
                    target,
                    maximumLongEdge,
                    ffmpegThreads,
                    timeoutMilliseconds,
                     logging,
                     pathId,
                     storageTarget,
                     previewCancel]() -> ImagePreviewJobResult {
                       ImagePreviewJobResult taskResult;
                       taskResult.source_path = Utf8ToWide(inputUtf8.c_str());
                       VideoScImagePreviewOptions options{};
                       options.structSize = sizeof(options);
                       options.maximumLongEdge = maximumLongEdge;
                       options.ffmpegThreadCount = ffmpegThreads;
                       options.noProgressTimeoutMilliseconds = timeoutMilliseconds;
                       options.shouldCancel = AtomicCancelCallback;
                       options.cancelContext = previewCancel;
                       VideoScImagePreviewResult preview{};
                       const bool succeeded = GenerateImagePreview(
                                                  inputUtf8.c_str(),
                                                  targetUtf8.c_str(),
                                                  &options,
                                                  &preview) != 0 &&
                                              std::filesystem::exists(target);
                       taskResult.status_code = preview.statusCode;
                       taskResult.native_error = preview.nativeError;
                       taskResult.detail = preview.errorMessage ? preview.errorMessage : "";
                       if (succeeded) {
                           taskResult.output_path = target;
                       } else {
                           videosc::dedup::ExecutionLogger logger(logging);
                           videosc::dedup::ExecutionFailureRecord record;
                           record.path_id = pathId;
                           record.task = "duplicate_report_detail";
                           record.stage = "preview";
                           record.operation = "image_preview_generate";
                           record.path = Utf8ToWide(inputUtf8.c_str());
                           record.storage_target_key = storageTarget;
                           record.media_kind = "image";
                           record.status = "failed";
                           record.status_code = static_cast<std::uint32_t>((std::max)(0, preview.statusCode));
                           record.native_error = preview.nativeError;
                           record.detail = taskResult.detail.empty()
                                               ? "image_preview_not_created"
                                               : taskResult.detail;
                           std::string logError;
                           taskResult.failure_logged =
                               logger.EnsureWritable(logError) && logger.WriteFailure(record, logError);
                       }
                       FreeVideoScImagePreviewResult(&preview);
                       return taskResult;
                   }));
    pending = true;
    return {};
}

std::filesystem::path VideoScApp::AcquireVideoContactSheet(
    const videosc::dedup::DuplicateMember& member) {
    if (member.path.empty() || m_reportPreviewDirectory.empty()) return {};
    const std::wstring key = member.path.wstring();
    const auto generated = m_generatedVideoPreviewFiles.find(key);
    if (generated != m_generatedVideoPreviewFiles.end()) {
        if (generated->second.empty()) m_reportThumbnailFailures.insert(key);
        return generated->second;
    }
    auto pending = m_videoPreviewJobs.find(key);
    if (pending != m_videoPreviewJobs.end()) {
        if (pending->second.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return {};
        std::filesystem::path result;
        try { result = pending->second.get(); } catch (...) { result.clear(); }
        m_videoPreviewJobs.erase(pending);
        const bool logWriteFailed = result.filename() == L"__execution_log_failure__";
        if (logWriteFailed) result.clear();
        m_generatedVideoPreviewFiles[key] = result;
        if (result.empty()) {
            m_reportThumbnailFailures.insert(key);
            m_reportMessage = logWriteFailed
                                  ? "视频六帧预览生成失败，且执行失败日志写入失败。"
                                  : "视频六帧预览生成失败，详情任务已写入执行失败日志。";
            m_reportMessageIsError = true;
        }
        return result;
    }

    const std::size_t nameHash = std::hash<std::wstring>{}(key);
    const wchar_t* extension = m_config.thumbnails.format == videosc::dedup::ThumbnailFormat::Png
                                   ? L".png"
                                   : L".jpg";
    const std::filesystem::path target =
        m_reportPreviewDirectory / (L"video-" + std::to_wstring(nameHash) + extension);
    const std::string inputUtf8 = WideToUtf8(member.path.wstring().c_str());
    const std::string targetUtf8 = WideToUtf8(target.wstring().c_str());
    const std::uint32_t cellEdge = m_config.thumbnails.video_cell_long_edge;
    const std::uint32_t ffmpegThreads = m_config.compute.ffmpeg_threads_per_task;
    const std::uint32_t timeoutMilliseconds = m_config.io.no_progress_timeout_seconds * 1000U;
    const videosc::dedup::LoggingConfig logging = m_config.logging;
    const std::uint64_t pathId = member.path_id;
    const std::wstring storageTarget = member.storage_target_key;
    std::atomic_bool* const previewCancel = &m_previewCancelRequested;
    m_videoPreviewJobs.emplace(
        key,
        std::async(std::launch::async,
                   [inputUtf8,
                    targetUtf8,
                    target,
                    cellEdge,
                    ffmpegThreads,
                    timeoutMilliseconds,
                     logging,
                     pathId,
                     storageTarget,
                     previewCancel]() -> std::filesystem::path {
                       VideoScMediaOptions options{};
                       options.structSize = sizeof(options);
                       options.mediaKindHint = VIDEOSC_MEDIA_VIDEO;
                       options.contactSheetCellLongEdge = cellEdge;
                       options.ffmpegThreadCount = ffmpegThreads;
                       options.noProgressTimeoutMilliseconds = timeoutMilliseconds;
                       options.shouldCancel = AtomicCancelCallback;
                       options.cancelContext = previewCancel;
                       options.contactSheetPath = targetUtf8.c_str();
                       VideoScMediaResult media{};
                       const bool succeeded = AnalyzeMediaFile(inputUtf8.c_str(), &options, &media) != 0 &&
                                              std::filesystem::exists(target);
                       bool failureLogged = true;
                       if (!succeeded) {
                           videosc::dedup::ExecutionLogger logger(logging);
                           videosc::dedup::ExecutionFailureRecord failure;
                           failure.path_id = pathId;
                           failure.task = "duplicate_report_detail";
                           failure.stage = "preview";
                           failure.operation = "video_contact_sheet_generate";
                           failure.path = Utf8ToWide(inputUtf8.c_str());
                           failure.storage_target_key = storageTarget;
                           failure.media_kind = "video";
                           failure.status = "failed";
                           failure.status_code = static_cast<std::uint32_t>((std::max)(0, media.statusCode));
                           failure.native_error = media.nativeError;
                           failure.detail = media.errorMessage ? media.errorMessage : "contact_sheet_not_created";
                           std::string error;
                           failureLogged = logger.EnsureWritable(error) && logger.WriteFailure(failure, error);
                       }
                       FreeVideoScMediaResult(&media);
                       if (succeeded) return target;
                       return failureLogged ? std::filesystem::path{}
                                            : target.parent_path() / L"__execution_log_failure__";
                   }));
    return {};
}

void VideoScApp::LoadConfigIntoEditors() {
    m_scanPathEditors.clear();
    std::string scanPathsText;
    for (const std::filesystem::path& path : m_config.paths.scan_roots) {
        const std::string utf8 = WideToUtf8(path.wstring().c_str());
        m_scanPathEditors.push_back(utf8);
        if (!scanPathsText.empty()) {
            scanPathsText += "\n";
        }
        scanPathsText += utf8;
    }
    CopyToEditor(m_pathsBuf, scanPathsText);

    CopyToEditor(m_mysqlHostBuf, WideToUtf8(m_config.database.host.c_str()));
    CopyToEditor(m_mysqlDatabaseBuf, WideToUtf8(m_config.database.database_name.c_str()));
    CopyToEditor(m_mysqlUserBuf, WideToUtf8(m_config.database.user_name.c_str()));
    CopyToEditor(m_mysqlPasswordBuf, WideToUtf8(m_config.database.password.c_str()));
    CopyToEditor(m_mysqlCaBuf, WideToUtf8(m_config.database.tls_ca_path.wstring().c_str()));
    CopyToEditor(m_mysqlCertificateBuf, WideToUtf8(m_config.database.tls_certificate_path.wstring().c_str()));
    CopyToEditor(m_mysqlPrivateKeyBuf, WideToUtf8(m_config.database.tls_private_key_path.wstring().c_str()));
    CopyToEditor(m_mysqldumpBuf, WideToUtf8(m_config.database.mysqldump_path.wstring().c_str()));
    CopyToEditor(m_backupDirectoryBuf, WideToUtf8(m_config.database.backup_directory.wstring().c_str()));
    CopyToEditor(m_thumbnailRootBuf, WideToUtf8(m_config.thumbnails.root_directory.wstring().c_str()));
    CopyToEditor(m_rocksDbDirectoryBuf, WideToUtf8(m_config.rocksdb.directory.wstring().c_str()));
    CopyToEditor(m_logDirectoryBuf, WideToUtf8(m_config.logging.directory.wstring().c_str()));
    CopyToEditor(m_executionLogDirectoryBuf,
                 WideToUtf8(m_config.logging.execution_directory.wstring().c_str()));
    CopyToEditor(m_imageSelectionDistanceBuf,
                 m_config.report_selection.image_dhash_distance_exclusive_limit.has_value()
                     ? std::to_string(*m_config.report_selection.image_dhash_distance_exclusive_limit)
                     : std::string{});
    if (m_config.report_selection.video_dhash_average_distance_exclusive_limit.has_value()) {
        std::ostringstream value;
        value << std::setprecision(12)
              << *m_config.report_selection.video_dhash_average_distance_exclusive_limit;
        CopyToEditor(m_videoSelectionDistanceBuf, value.str());
    } else {
        m_videoSelectionDistanceBuf[0] = '\0';
    }
    // 填充 Everything 路径缓冲区（参照现有 mysql 路径缓冲区填充模式）
    CopyToEditor(m_everythingDllBuf, WideToUtf8(m_config.discovery.everything_dll_path.wstring().c_str()));
    CopyToEditor(m_everythingExeBuf, WideToUtf8(m_config.discovery.everything_exe_path.wstring().c_str()));
    m_maxLongEdge = static_cast<int>(m_config.thumbnails.video_cell_long_edge);
}

void VideoScApp::UpdateConfigFromEditors() {
    m_config.paths.scan_roots.clear();
    for (const std::string& path : m_scanPathEditors) {
        const std::wstring widePath = Utf8ToWide(path.c_str());
        if (!widePath.empty()) {
            m_config.paths.scan_roots.emplace_back(widePath);
        }
    }
    m_config.database.host = Utf8ToWide(m_mysqlHostBuf);
    m_config.database.database_name = Utf8ToWide(m_mysqlDatabaseBuf);
    m_config.database.user_name = Utf8ToWide(m_mysqlUserBuf);
    m_config.database.password = Utf8ToWide(m_mysqlPasswordBuf);
    if (!m_config.database.password.empty()) {
        m_config.database.password_decryption_failed = false;
    }
    m_config.database.tls_ca_path = Utf8ToWide(m_mysqlCaBuf);
    m_config.database.tls_certificate_path = Utf8ToWide(m_mysqlCertificateBuf);
    m_config.database.tls_private_key_path = Utf8ToWide(m_mysqlPrivateKeyBuf);
    m_config.database.mysqldump_path = Utf8ToWide(m_mysqldumpBuf);
    m_config.database.backup_directory = Utf8ToWide(m_backupDirectoryBuf);
    m_config.thumbnails.root_directory = Utf8ToWide(m_thumbnailRootBuf);
    m_config.rocksdb.directory = Utf8ToWide(m_rocksDbDirectoryBuf);
    m_config.logging.directory = Utf8ToWide(m_logDirectoryBuf);
    m_config.logging.execution_directory = Utf8ToWide(m_executionLogDirectoryBuf);
    m_config.discovery.everything_dll_path = Utf8ToWide(m_everythingDllBuf);
    m_config.discovery.everything_exe_path = Utf8ToWide(m_everythingExeBuf);
    if (m_imageSelectionDistanceBuf[0] == '\0') {
        m_config.report_selection.image_dhash_distance_exclusive_limit.reset();
    } else {
        char* end = nullptr;
        const unsigned long value = std::strtoul(m_imageSelectionDistanceBuf, &end, 10);
        m_config.report_selection.image_dhash_distance_exclusive_limit =
            end != m_imageSelectionDistanceBuf && *end == '\0' &&
                    value <= (std::numeric_limits<std::uint32_t>::max)()
                ? static_cast<std::uint32_t>(value)
                : (std::numeric_limits<std::uint32_t>::max)();
    }
    if (m_videoSelectionDistanceBuf[0] == '\0') {
        m_config.report_selection.video_dhash_average_distance_exclusive_limit.reset();
    } else {
        char* end = nullptr;
        const double value = std::strtod(m_videoSelectionDistanceBuf, &end);
        m_config.report_selection.video_dhash_average_distance_exclusive_limit =
            end != m_videoSelectionDistanceBuf && *end == '\0'
                ? value
                : std::numeric_limits<double>::quiet_NaN();
    }
    m_maxLongEdge = static_cast<int>(m_config.thumbnails.video_cell_long_edge);
}

void VideoScApp::SaveConfiguration() {
    UpdateConfigFromEditors();
    const videosc::dedup::ConfigSaveResult saveResult = m_configStore.Save(m_config);
    if (!saveResult.succeeded) {
        m_configMessage = WideToUtf8(saveResult.error_message.c_str());
        m_configMessageIsError = true;
        return;
    }

    const videosc::dedup::ConfigLoadResult loadResult = m_configStore.Load();
    m_config = loadResult.config;
    videosc::dedup::ApplicationErrorLogger::Configure(m_config.logging);
    CrashHandler::SetLogDirectory(m_config.logging.directory);
    m_configCanSave = loadResult.can_save;
    LoadConfigIntoEditors();
    m_configMessage = "配置已原子保存：" + WideToUtf8(m_configStore.config_path().wstring().c_str());
    if (!loadResult.warnings.empty()) {
        m_configMessage += "\n" + JoinWarnings(loadResult.warnings);
    }
    m_configMessageIsError = false;
    if (m_runtimeStore) {
        m_runtimeConfigurationStale = true;
        m_configMessage += "\n新配置将在当前任务结束后的下一次运行操作中生效。";
    }
}

void VideoScApp::ReloadConfiguration() {
    const videosc::dedup::ConfigLoadResult loadResult = m_configStore.Load();
    m_config = loadResult.config;
    videosc::dedup::ApplicationErrorLogger::Configure(m_config.logging);
    CrashHandler::SetLogDirectory(m_config.logging.directory);
    m_configCanSave = loadResult.can_save;
    LoadConfigIntoEditors();
    m_configMessage = JoinWarnings(loadResult.warnings);
    if (m_configMessage.empty()) {
        m_configMessage = "配置已加载：" + WideToUtf8(m_configStore.config_path().wstring().c_str());
    }
    m_configMessageIsError = !loadResult.can_save ||
                             loadResult.status == videosc::dedup::ConfigLoadStatus::InvalidUsingDefaults;
    if (m_runtimeStore) m_runtimeConfigurationStale = true;
}

void VideoScApp::StartDatabaseInitialization() {
    if (m_databaseInitRunning.load()) return;
    if ((m_scanCoordinator && m_scanCoordinator->is_running()) || m_reportRunning.load() ||
        m_reportCleanupRunning.load() || m_deletionRunning.load() || m_selectionRunning.load()) {
        m_databaseInitMessage = "扫描、报告生成、报告删除或永久删除运行时不能初始化数据库表。";
        m_databaseInitMessageIsError = true;
        return;
    }
    UpdateConfigFromEditors();
    if (m_config.database.password_decryption_failed) {
        m_databaseInitMessage = "MySQL 密码无法解密，请重新输入并保存配置后再初始化。";
        m_databaseInitMessageIsError = true;
        return;
    }
    const auto issues = videosc::dedup::ConfigValidator::Validate(m_config);
    const auto databaseError = std::find_if(issues.begin(), issues.end(), [](const auto& issue) {
        return issue.severity == videosc::dedup::ValidationSeverity::Error &&
               issue.field.rfind("database.", 0) == 0;
    });
    if (databaseError != issues.end()) {
        m_databaseInitMessage = WideToUtf8(databaseError->message.c_str());
        m_databaseInitMessageIsError = true;
        return;
    }
    // 数据库维护与后台同步共享同一组表；先关闭空闲运行时，避免维护期间并发访问。
    if (m_runtimeStore) ShutdownRuntime();
    if (m_databaseInitThread.joinable()) m_databaseInitThread.join();
    const videosc::dedup::DatabaseConfig databaseConfig = m_config.database;
    {
        std::lock_guard<std::mutex> lock(m_databaseInitResultMutex);
        m_databaseInitResultReady = false;
        m_databaseInitResult.clear();
    }
    m_databaseInitMessage = "正在连接并初始化 MySQL 数据库表……";
    m_databaseInitMessageIsError = false;
    m_databaseInitRunning.store(true);
    if (!StartBackgroundThread(m_databaseInitThread, [this, databaseConfig] {
        bool succeeded = false;
        std::string message;
        const auto cancellationRequested = [this]() {
            return m_shutdownRequested.load(std::memory_order_relaxed);
        };
        try {
            videosc::dedup::MySqlClient client(databaseConfig);
            if (cancellationRequested()) {
                message = "数据库初始化已取消。";
            } else {
                const videosc::dedup::MySqlStatus connect = client.Connect();
                succeeded = connect.succeeded;
                if (!connect.succeeded) {
                    message = "MySQL 连接失败";
                    if (connect.native_error != 0) {
                        message += "（错误 " + std::to_string(connect.native_error) + "）";
                    }
                    if (!connect.message.empty()) message += "：" + connect.message;
                } else if (cancellationRequested()) {
                    succeeded = false;
                    message = "数据库初始化已在连接后取消。";
                } else {
                    bool databaseHasTables = false;
                    std::string databaseLiteral;
                    videosc::dedup::MySqlStatus inspected =
                        client.EscapeLiteral(WideToUtf8(databaseConfig.database_name.c_str()), databaseLiteral);
                    if (inspected.succeeded) {
                        inspected = client.Query(
                            "SELECT EXISTS(SELECT 1 FROM information_schema.tables WHERE table_schema=" +
                                databaseLiteral + " LIMIT 1)",
                            [&](const videosc::dedup::MySqlRow& row) {
                                databaseHasTables = !row.empty() && row.front().has_value() &&
                                                    *row.front() == "1";
                                return false;
                            });
                    }
                    if (!inspected.succeeded) {
                        succeeded = false;
                        message = "检查 MySQL 现有表失败：" + inspected.message;
                    } else if (cancellationRequested()) {
                        succeeded = false;
                        message = "数据库初始化已在表检查后取消。";
                    } else {
                        std::filesystem::path backupFile;
                        if (databaseHasTables) {
                            const videosc::dedup::MySqlBackupResult backup =
                                videosc::dedup::MySqlBackup::Create(databaseConfig, cancellationRequested);
                            if (!backup.succeeded) {
                                succeeded = false;
                                message = backup.cancelled
                                              ? "数据库初始化已在备份阶段取消。"
                                              : "初始化前 mysqldump 备份失败：" + backup.message;
                                if (backup.process_error != 0) {
                                    message += "（系统错误 " + std::to_string(backup.process_error) + "）";
                                }
                                if (backup.exit_code != 0) {
                                    message += "（退出码 " + std::to_string(backup.exit_code) + "）";
                                }
                            } else {
                                backupFile = backup.backup_file;
                            }
                        }
                        if ((!databaseHasTables || !backupFile.empty()) && !cancellationRequested()) {
                            const videosc::dedup::MySqlSchemaResult schema =
                                videosc::dedup::MySqlSchema::Initialize(client);
                            succeeded = schema.status.succeeded;
                            if (succeeded) {
                                message = schema.created_initial_schema
                                              ? "MySQL 数据库表初始化完成，模式版本 " +
                                                    std::to_string(schema.current_version) + "。"
                                              : "MySQL 数据库表检查完成，模式版本 " +
                                                    std::to_string(schema.current_version) + "，无需重建。";
                                if (!backupFile.empty()) {
                                    message += " 备份文件：" + WideToUtf8(backupFile.wstring().c_str());
                                }
                            } else {
                                message = "MySQL 数据库表初始化失败";
                                if (schema.status.native_error != 0) {
                                    message += "（错误 " + std::to_string(schema.status.native_error) + "）";
                                }
                                if (!schema.status.message.empty()) message += "：" + schema.status.message;
                            }
                        } else if (message.empty()) {
                            succeeded = false;
                            message = "数据库初始化已在建表前取消。";
                        }
                    }
                }
            }
            client.Disconnect();
        } catch (const std::bad_alloc&) {
            message = "数据库初始化内存不足";
            message += "（" + LogApplicationException("VideoScGUI", "database_init", message, "std::bad_alloc") + "）";
        } catch (const std::exception& exception) {
            message = exception.what();
            message += "（" + LogApplicationException("VideoScGUI", "database_init", message, "std::exception") + "）";
        } catch (...) {
            message = "数据库初始化发生未知异常";
            message += "（" + LogApplicationException("VideoScGUI", "database_init", message, "unknown_exception") + "）";
        }
        {
            std::lock_guard<std::mutex> lock(m_databaseInitResultMutex);
            m_databaseInitResultSucceeded = succeeded;
            m_databaseInitResult = std::move(message);
            m_databaseInitResultReady = true;
        }
        m_databaseInitRunning.store(false);
    }, "database_init_start", m_databaseInitMessage)) {
        m_databaseInitRunning.store(false);
        m_databaseInitMessageIsError = true;
    }
}

void VideoScApp::ConsumeDatabaseInitializationResult() {
    if (m_databaseInitRunning.load()) return;
    if (m_databaseInitThread.joinable()) m_databaseInitThread.join();
    std::lock_guard<std::mutex> lock(m_databaseInitResultMutex);
    if (!m_databaseInitResultReady) return;
    m_databaseInitMessage = m_databaseInitResult;
    m_databaseInitMessageIsError = !m_databaseInitResultSucceeded;
    m_databaseInitResultReady = false;
}

void VideoScApp::ApplyDataVersionResult(const videosc::dedup::DataVersionResult& result) {
    m_rocksDataVersion = result.rocksdb;
    m_mysqlDataVersion = result.mysql;
    m_dataVersionDecision = result.decision;
    m_dataVersionMessageIsError = !result.succeeded ||
                                  result.decision == videosc::dedup::DataVersionDecision::RejectNewerData;
    if (!result.message.empty()) {
        m_dataVersionMessage = result.message;
        return;
    }
    switch (result.decision) {
        case videosc::dedup::DataVersionDecision::ReuseReady:
            m_dataVersionMessage = "RocksDB 与 MySQL 数据版本一致，可以复用。";
            break;
        case videosc::dedup::DataVersionDecision::ResumeRebuild:
            m_dataVersionMessage = "当前数据 generation 正在重建；完成全量扫描和 MySQL 同步前不能生成报告。";
            break;
        case videosc::dedup::DataVersionDecision::ResetRequired:
            m_dataVersionMessage = "数据版本缺失或不一致，需要执行受控全量重建。";
            break;
        case videosc::dedup::DataVersionDecision::RejectNewerData:
            m_dataVersionMessage = "存储数据版本高于当前程序，请升级程序后重试。";
            break;
    }
}

bool VideoScApp::EnsureRuntime(std::string& error) {
    if (m_runtimeConfigurationStale) {
        if ((m_scanCoordinator && m_scanCoordinator->is_running()) || m_reportRunning.load() ||
            m_reportCleanupRunning.load() || m_deletionRunning.load() || m_selectionRunning.load()) {
            error = "当前任务仍使用启动时冻结的配置；任务结束后再开始下一项操作。";
            return false;
        }
        ShutdownRuntime();
        m_runtimeConfigurationStale = false;
    }
    if (m_runtimeStore && m_runtimeStore->is_open() && m_syncQueue && m_syncService) {
        error.clear();
        return true;
    }
    try {
        UpdateConfigFromEditors();
        const std::string validationErrors = JoinValidationErrors(videosc::dedup::ConfigValidator::Validate(m_config));
        if (!validationErrors.empty()) {
            error = validationErrors;
            return false;
        }

        videosc::dedup::ExecutionLogger dataVersionLogger(m_config.logging);
        std::string dataVersionLogError;
        if (!dataVersionLogger.EnsureWritable(dataVersionLogError)) {
            error = "数据版本执行日志不可写：" + dataVersionLogError;
            return false;
        }
        const auto writeDataVersionEvent = [&](const std::string& event,
                                               const std::string& stage,
                                               const std::string& message) {
            return dataVersionLogger.WriteEvent(
                {0, "data_version", event, stage, message, 0, 0}, dataVersionLogError);
        };

        auto store = std::make_unique<videosc::dedup::RocksStore>(m_config.rocksdb);
        const videosc::dedup::RocksStatus opened = store->Open();
        if (!opened.succeeded) {
            error = "RocksDB 打开失败：" + opened.message;
            return false;
        }

        auto mysql = std::make_unique<videosc::dedup::MySqlClient>(m_config.database);
        const videosc::dedup::MySqlStatus connected = mysql->Connect();
        if (!connected.succeeded) {
            error = "MySQL 连接失败";
            if (connected.native_error != 0) {
                error += "（错误 " + std::to_string(connected.native_error) + "）";
            }
            if (!connected.message.empty()) error += "：" + connected.message;
            return false;
        }

        std::uint32_t schemaVersion = 0;
        const videosc::dedup::MySqlStatus schemaVersionRead =
            videosc::dedup::MySqlSchema::ReadCurrentVersion(*mysql, schemaVersion);
        if (!schemaVersionRead.succeeded) {
            error = "读取 MySQL 模式版本失败：" + schemaVersionRead.message;
            return false;
        }
        if (!writeDataVersionEvent(
                "data_version_check",
                "schema_version",
                "程序 schema=" + std::to_string(videosc::dedup::kCurrentMySqlSchemaVersion) +
                    "，MySQL schema=" + std::to_string(schemaVersion))) {
            error = "写入数据版本检查日志失败：" + dataVersionLogError;
            return false;
        }
        if (schemaVersion > videosc::dedup::kCurrentMySqlSchemaVersion) {
            videosc::dedup::DataVersionResult rejected;
            rejected.succeeded = true;
            rejected.decision = videosc::dedup::DataVersionDecision::RejectNewerData;
            rejected.message = "MySQL 模式版本 " + std::to_string(schemaVersion) +
                               " 高于当前程序支持的版本 " +
                               std::to_string(videosc::dedup::kCurrentMySqlSchemaVersion) +
                               "；已拒绝运行，未清理业务数据。";
            ApplyDataVersionResult(rejected);
            error = rejected.message;
            writeDataVersionEvent("data_version_rejected", "schema_version", error);
            return false;
        }

        videosc::dedup::DataVersionCoordinator dataVersions(*store, *mysql);
        videosc::dedup::DataVersionResult versionResult = dataVersions.Inspect();
        if (versionResult.succeeded &&
            versionResult.decision == videosc::dedup::DataVersionDecision::RejectNewerData) {
            ApplyDataVersionResult(versionResult);
            const std::uint32_t rocksVersion = versionResult.rocksdb.has_value()
                                                   ? versionResult.rocksdb->data_version
                                                   : 0;
            const std::uint32_t mysqlVersion = versionResult.mysql.has_value()
                                                   ? versionResult.mysql->data_version
                                                   : 0;
            error = "存储数据版本高于当前程序版本 " +
                    std::to_string(videosc::dedup::kCurrentDataVersion) +
                    "（RocksDB=" + std::to_string(rocksVersion) +
                    "，MySQL=" + std::to_string(mysqlVersion) +
                    "）；已拒绝运行，未写入或清理数据。";
            m_dataVersionMessage = error;
            writeDataVersionEvent("data_version_rejected", "data_version_check", error);
            return false;
        }
        const bool missingDataVersionTable = !versionResult.succeeded &&
                                             versionResult.native_error == 1146;
        const bool invalidDataVersionRecord = !versionResult.succeeded &&
                                              (versionResult.message.find("invalid_data_version_record") !=
                                                   std::string::npos ||
                                               versionResult.message.find("invalid_mysql_data_version_record") !=
                                                   std::string::npos);
        if (!versionResult.succeeded && !missingDataVersionTable && !invalidDataVersionRecord) {
            ApplyDataVersionResult(versionResult);
            error = versionResult.message;
            return false;
        }

        bool resetRequired = schemaVersion < videosc::dedup::kCurrentMySqlSchemaVersion ||
                             missingDataVersionTable || invalidDataVersionRecord ||
                             (versionResult.succeeded &&
                              versionResult.decision == videosc::dedup::DataVersionDecision::ResetRequired);
        if (!resetRequired) {
            const videosc::dedup::MySqlSchemaResult schema =
                videosc::dedup::MySqlSchema::Initialize(*mysql);
            if (!schema.status.succeeded) {
                const bool contractDrift = schema.status.native_error == 0 &&
                                           schema.status.message.rfind("mysql_schema_contract_", 0) == 0;
                if (!contractDrift) {
                    error = "MySQL 模式初始化或校验失败：" + schema.status.message;
                    return false;
                }
                resetRequired = true;
            } else if (!writeDataVersionEvent(
                           "schema_contract_validation",
                           "mysql_schema",
                           "MySQL schema 物理字段契约校验通过")) {
                error = "写入 schema 校验日志失败：" + dataVersionLogError;
                return false;
            }
        }
        if (resetRequired) {
            if (!writeDataVersionEvent(
                    "data_reset_started",
                    "data_reset",
                    "数据版本缺失、较低、不一致或 schema 漂移，开始清理固定 VideoSc 派生数据")) {
                error = "写入数据重置日志失败：" + dataVersionLogError;
                return false;
            }
            versionResult = dataVersions.ResetForCurrentVersion();
            ApplyDataVersionResult(versionResult);
            if (!versionResult.succeeded) {
                error = versionResult.message;
                return false;
            }
            if (!writeDataVersionEvent(
                    "data_rebuild_started",
                    "data_rebuild",
                    "RocksDB 与 MySQL 已进入同 generation rebuilding 状态")) {
                error = "数据已重置，但写入重建日志失败：" + dataVersionLogError;
                return false;
            }
        } else {
            ApplyDataVersionResult(versionResult);
        }

        videosc::dedup::DuplicateReportStore reports(*store);
        const videosc::dedup::RocksStatus reportRecovered = reports.CleanupInterruptedWork();
        if (!reportRecovered.succeeded) {
            error = "清理异常退出遗留报告失败：" + reportRecovered.message;
            store->Close();
            return false;
        }
        videosc::dedup::ReportSelectionStore selections(*store);
        for (const auto kind : {videosc::dedup::DuplicateReportKind::Exact,
                                videosc::dedup::DuplicateReportKind::Similar}) {
            const auto activeReport = reports.ActiveGeneration(kind);
            if (!activeReport.has_value()) continue;
            const videosc::dedup::RocksStatus selectionRecovered =
                selections.CleanupInterruptedStaging(kind, *activeReport);
            if (!selectionRecovered.succeeded) {
                error = "清理异常退出遗留选择失败：" + selectionRecovered.message;
                store->Close();
                return false;
            }
        }
        auto queue = std::make_unique<videosc::dedup::MySqlSyncQueue>(*store);
        auto sync = std::make_unique<videosc::dedup::MySqlSyncService>(
            *queue,
            *mysql,
            m_config.database.sync_batch_size,
            m_config.database.retry_interval_seconds,
            m_config.logging);

        videosc::dedup::OperationLogger logger(m_config.logging);
        std::string logError;
        if (!logger.EnsureWritable(logError)) {
            error = "操作日志目录不可写：" + logError;
            store->Close();
            return false;
        }
        auto hasher = std::make_shared<videosc::dedup::VideoScFileHasher>(m_config.io);
        videosc::dedup::DeletionExecutor recovery(*store, *queue, std::move(hasher), logger);
        const videosc::dedup::RocksStatus recovered = recovery.RecoverPendingDeletes();
        if (!recovered.succeeded) {
            error = "恢复未完成删除事务失败：" + recovered.message;
            store->Close();
            return false;
        }

        std::size_t recoveredStagedOperations = 0;
        const videosc::dedup::RocksStatus recoveredStaged = queue->PublishAllStaged(
            std::max<std::uint32_t>(1, m_config.database.sync_batch_size),
            recoveredStagedOperations);
        if (!recoveredStaged.succeeded) {
            error = "恢复历史待同步数据失败：" + recoveredStaged.message;
            store->Close();
            return false;
        }

        sync->Start();
        m_runtimeStore = std::move(store);
        m_syncQueue = std::move(queue);
        m_mysqlClient = std::move(mysql);
        m_syncService = std::move(sync);
        m_runtimeConfigurationStale = false;
        RefreshResumableScans();
        error.clear();
        return true;
    } catch (const std::bad_alloc&) {
        error = "运行时初始化内存不足";
        error += "（" + LogApplicationException("VideoScGUI", "runtime_init", error, "std::bad_alloc") + "）";
        ShutdownRuntime();
        return false;
    } catch (const std::exception& exception) {
        error = exception.what();
        error += "（" + LogApplicationException("VideoScGUI", "runtime_init", error, "std::exception") + "）";
        ShutdownRuntime();
        return false;
    } catch (...) {
        error = "运行时初始化发生未知异常";
        error += "（" + LogApplicationException("VideoScGUI", "runtime_init", error, "unknown_exception") + "）";
        ShutdownRuntime();
        return false;
    }
}

void VideoScApp::ShutdownRuntime() {
    if (m_scanCoordinator) {
        m_scanCoordinator->Cancel();
        m_scanCoordinator->Wait();
        m_scanCoordinator.reset();
    }
    if (m_syncService) {
        m_syncService->Stop();
        m_syncService.reset();
    }
    m_mysqlClient.reset();
    m_syncQueue.reset();
    if (m_runtimeStore) {
        m_runtimeStore->Close();
        m_runtimeStore.reset();
    }
    m_scanSnapshot = {};
    m_syncSnapshot = {};
    m_rocksDataVersion.reset();
    m_mysqlDataVersion.reset();
    m_dataVersionDecision = videosc::dedup::DataVersionDecision::ResetRequired;
    m_dataVersionMessage.clear();
    m_dataVersionMessageIsError = false;
    m_resumableScans.clear();
    m_selectedReportGroupId.reset();
    m_selectedReportGroupOrdinal.reset();
    m_selectedReportGroup.reset();
    m_showReportDetailWindow = false;
    m_clearReportThumbnailsRequested = true;
}

bool VideoScApp::TryMarkDataVersionReady(std::string& error) {
    if (!m_runtimeStore || !m_mysqlClient || !m_syncQueue || !m_syncService) {
        error = "数据版本运行时尚未初始化。";
        return false;
    }

    videosc::dedup::DataVersionCoordinator coordinator(*m_runtimeStore, *m_mysqlClient);
    videosc::dedup::DataVersionResult inspected = coordinator.Inspect();
    ApplyDataVersionResult(inspected);
    if (!inspected.succeeded) {
        error = inspected.message;
        return false;
    }
    if (inspected.decision == videosc::dedup::DataVersionDecision::ReuseReady) {
        error.clear();
        return true;
    }
    if (inspected.decision != videosc::dedup::DataVersionDecision::ResumeRebuild ||
        !inspected.rocksdb.has_value() || !inspected.mysql.has_value() ||
        inspected.rocksdb->generation_id == 0 ||
        inspected.rocksdb->generation_id != inspected.mysql->generation_id) {
        error = "数据版本或 generation 不一致，不能提交 ready；请重新启动全量扫描。";
        return false;
    }

    m_syncSnapshot = m_syncService->snapshot();
    if (!m_syncSnapshot.connected || !m_syncSnapshot.last_error.empty()) {
        error = m_syncSnapshot.last_error.empty()
                    ? "MySQL 尚未连接，数据版本保持 rebuilding。"
                    : "MySQL 同步仍有错误：" + m_syncSnapshot.last_error;
        return false;
    }
    std::vector<videosc::dedup::SyncOperation> pending;
    const videosc::dedup::RocksStatus queueStatus = m_syncQueue->ReadBatch(1, pending);
    if (!queueStatus.succeeded || !pending.empty()) {
        error = queueStatus.succeeded ? "MySQL 全局同步队列尚未清空。"
                                      : "读取 MySQL 全局同步队列失败：" + queueStatus.message;
        return false;
    }
    const videosc::dedup::MySqlStatus ping = m_mysqlClient->Ping();
    if (!ping.succeeded) {
        error = "MySQL 连接复核失败：" + ping.message;
        return false;
    }
    const videosc::dedup::MySqlStatus schema =
        videosc::dedup::MySqlSchema::ValidateCurrentSchema(*m_mysqlClient);
    if (!schema.succeeded) {
        error = "MySQL schema 契约复核失败：" + schema.message;
        return false;
    }

    videosc::dedup::DataVersionResult ready =
        coordinator.MarkReady(inspected.rocksdb->generation_id);
    ApplyDataVersionResult(ready);
    if (!ready.succeeded) {
        error = ready.message;
        return false;
    }
    videosc::dedup::ExecutionLogger logger(m_config.logging);
    std::string logError;
    if (!logger.EnsureWritable(logError) ||
        !logger.WriteEvent(
             {0,
              "data_version",
              "data_rebuild_ready",
              "data_rebuild",
              "全量扫描、MySQL 同步和 schema 复核完成，双端数据版本已提交 ready",
              0,
              0},
             logError)) {
        m_dataVersionMessage += "；ready 日志写入失败：" + logError;
        m_dataVersionMessageIsError = true;
    }
    error.clear();
    return true;
}

bool VideoScApp::CheckReportReadiness(std::string& error) {
    if (!m_runtimeStore || !m_mysqlClient || !m_syncQueue || !m_syncService) {
        error = "报告运行时尚未初始化。";
        return false;
    }
    videosc::dedup::DataVersionCoordinator coordinator(*m_runtimeStore, *m_mysqlClient);
    videosc::dedup::DataVersionResult inspected = coordinator.Inspect();
    ApplyDataVersionResult(inspected);
    if (!inspected.succeeded) {
        error = inspected.message;
        return false;
    }
    if (inspected.decision != videosc::dedup::DataVersionDecision::ReuseReady ||
        !inspected.rocksdb.has_value() || !inspected.mysql.has_value() ||
        inspected.rocksdb->generation_id != inspected.mysql->generation_id) {
        error = inspected.decision == videosc::dedup::DataVersionDecision::RejectNewerData
                    ? "存储数据版本高于当前程序，必须升级程序后才能生成报告。"
                    : "数据仍在全量重建中；请开始或恢复扫描，并等待 MySQL 同步完成。";
        return false;
    }

    m_syncSnapshot = m_syncService->snapshot();
    if (!m_syncSnapshot.connected) {
        error = "MySQL 尚未连接，请等待后台同步重试。";
        return false;
    }
    if (!m_syncSnapshot.last_error.empty()) {
        error = "MySQL 最近同步失败，请等待重试成功：" + m_syncSnapshot.last_error;
        return false;
    }
    const videosc::dedup::MySqlStatus ping = m_mysqlClient->Ping();
    if (!ping.succeeded) {
        error = "MySQL 连接复核失败：" + ping.message;
        return false;
    }

    std::vector<videosc::dedup::SyncOperation> pending;
    const videosc::dedup::RocksStatus globalStatus = m_syncQueue->ReadBatch(1, pending);
    if (!globalStatus.succeeded || !pending.empty()) {
        error = globalStatus.succeeded ? "MySQL 全局同步队列尚未清空，请等待同步完成。"
                                       : "读取 MySQL 全局同步队列失败：" + globalStatus.message;
        return false;
    }

    videosc::dedup::ScanCheckpointStore checkpointStore(*m_runtimeStore);
    std::vector<videosc::dedup::ScanCheckpoint> checkpoints;
    const videosc::dedup::RocksStatus listed = checkpointStore.ListResumable(0, checkpoints);
    if (!listed.succeeded) {
        error = "读取扫描检查点失败：" + listed.message;
        return false;
    }
    for (const videosc::dedup::ScanCheckpoint& checkpoint : checkpoints) {
        std::uint64_t staged = 0;
        std::uint64_t scanPending = 0;
        const videosc::dedup::RocksStatus stagedStatus =
            m_syncQueue->StagedCount(checkpoint.scan_id, staged);
        const videosc::dedup::RocksStatus pendingStatus =
            m_syncQueue->PendingCount(checkpoint.scan_id, scanPending);
        if (!stagedStatus.succeeded || !pendingStatus.succeeded) {
            error = "读取扫描同步尾批状态失败。";
            return false;
        }
        if (staged != 0 || scanPending != 0) {
            error = "仍有扫描数据等待发布或同步，请等待后台同步完成。";
            return false;
        }
    }

    const videosc::dedup::MySqlStatus schema =
        videosc::dedup::MySqlSchema::ValidateCurrentSchema(*m_mysqlClient);
    if (!schema.succeeded) {
        error = "MySQL schema 契约不完整：" + schema.message + "；请初始化数据库表。";
        return false;
    }
    error.clear();
    return true;
}

void VideoScApp::StartScan(const std::optional<std::uint64_t> resume_scan_id) {
    if (m_reportRunning.load() || m_reportCleanupRunning.load() || m_deletionRunning.load() ||
        m_selectionRunning.load()) {
        m_scanMessage = "报告生成、报告删除或永久删除正在运行，当前不能启动扫描。";
        m_scanMessageIsError = true;
        return;
    }
    if (m_scanCoordinator && m_scanCoordinator->is_running()) return;

    UpdateConfigFromEditors();
    const std::string validationErrors = JoinValidationErrors(videosc::dedup::ConfigValidator::Validate(m_config));
    if (!validationErrors.empty()) {
        m_scanMessage = validationErrors;
        m_scanMessageIsError = true;
        return;
    }
    std::string runtimeError;
    if (!EnsureRuntime(runtimeError)) {
        m_scanMessage = runtimeError;
        m_scanMessageIsError = true;
        return;
    }
    if (m_scanCoordinator) {
        m_scanCoordinator->Wait();
        m_scanCoordinator.reset();
    }

    try {
        videosc::dedup::ScanOptions options =
            videosc::dedup::ScanOptions::Freeze(m_config, m_generateSimilarAfterScan);
        if (resume_scan_id.has_value()) {
            const auto found = std::find_if(m_resumableScans.begin(),
                                            m_resumableScans.end(),
                                            [&](const auto& checkpoint) {
                                                return checkpoint.scan_id == *resume_scan_id;
                                            });
            if (found == m_resumableScans.end()) {
                m_scanMessage = "所选断点已不存在，请刷新断点列表。";
                m_scanMessageIsError = true;
                return;
            }
            std::string decodeError;
            std::optional<videosc::dedup::ScanOptions> saved =
                videosc::dedup::ScanOptionsCodec::Deserialize(found->scan_options_json, decodeError);
            if (!saved.has_value()) {
                m_scanMessage = "断点配置损坏：" + decodeError;
                m_scanMessageIsError = true;
                return;
            }
            options = std::move(*saved);
        }
        videosc::dedup::MySqlSyncService* const syncService = m_syncService.get();
        m_scanCoordinator = std::make_unique<videosc::dedup::ScanCoordinator>(
            std::move(options),
            *m_runtimeStore,
            *m_syncQueue,
            *m_mysqlClient,
            [syncService] {
                if (syncService != nullptr) syncService->Wake();
            });
        if (!m_scanCoordinator->Start(resume_scan_id)) {
            m_scanMessage = "扫描任务已经在运行。";
            m_scanMessageIsError = true;
            return;
        }
        m_scanSnapshot = m_scanCoordinator->snapshot();
        m_shownDiscoveryWarning.clear();  // 重置已显示标记，新扫描的警告可再次弹窗
        m_discoveryWarningPopupPending = false;
        m_autoExactStartedForScanId = 0;
        m_scanMessage = resume_scan_id.has_value() ? "已从断点恢复扫描。" : "扫描任务已启动。";
        m_scanMessageIsError = false;
        if (m_syncService) m_syncService->Wake();
    } catch (const std::bad_alloc&) {
        m_scanMessage = "启动扫描失败：内存不足";
        m_scanMessage += "（" + LogApplicationException("VideoScGUI", "scan_start", m_scanMessage, "std::bad_alloc") + "）";
        m_scanMessageIsError = true;
    } catch (const std::exception& exception) {
        m_scanMessage = std::string("启动扫描失败：") + exception.what();
        m_scanMessage += "（" + LogApplicationException("VideoScGUI", "scan_start", m_scanMessage, "std::exception") + "）";
        m_scanMessageIsError = true;
    } catch (...) {
        m_scanMessage = "启动扫描失败：未知异常";
        m_scanMessage += "（" + LogApplicationException("VideoScGUI", "scan_start", m_scanMessage, "unknown_exception") + "）";
        m_scanMessageIsError = true;
    }
}

void VideoScApp::RefreshRuntimeSnapshots() {
    if (m_scanCoordinator) {
        m_scanSnapshot = m_scanCoordinator->snapshot();
    }
    if (m_syncService) {
        m_syncSnapshot = m_syncService->snapshot();
        m_scanSnapshot.mysql_connected = m_syncSnapshot.connected;
        m_scanSnapshot.mysql_last_success_utc_ms = m_syncSnapshot.last_success_utc_ms;
    }
    if (m_syncQueue) {
        std::uint64_t staged = 0;
        std::uint64_t scanPending = 0;
        std::vector<videosc::dedup::SyncOperation> globalPending;
        const videosc::dedup::RocksStatus stagedStatus =
            m_scanSnapshot.scan_id == 0
                ? videosc::dedup::RocksStatus{true, {}}
                : m_syncQueue->StagedCount(m_scanSnapshot.scan_id, staged);
        const videosc::dedup::RocksStatus pendingStatus =
            m_scanSnapshot.scan_id == 0
                ? videosc::dedup::RocksStatus{true, {}}
                : m_syncQueue->PendingCount(m_scanSnapshot.scan_id, scanPending);
        const videosc::dedup::RocksStatus globalStatus = m_syncQueue->ReadBatch(1, globalPending);
        if (stagedStatus.succeeded && pendingStatus.succeeded && globalStatus.succeeded) {
            m_scanSnapshot.mysql_staged_operations = staged;
            m_scanSnapshot.mysql_pending_operations = scanPending;
            m_scanSnapshot.shared_sync_complete = m_scanSnapshot.local_scan_complete &&
                                                  staged == 0 && scanPending == 0 &&
                                                  globalPending.empty();
            if (m_scanSnapshot.local_scan_complete && !m_scanSnapshot.shared_sync_complete) {
                m_scanSnapshot.phase = videosc::dedup::ScanPhase::Syncing;
            } else if (m_scanSnapshot.shared_sync_complete && m_scanCoordinator &&
                       m_scanSnapshot.phase != videosc::dedup::ScanPhase::CompletedSynchronized) {
                std::string cleanupError;
                if (!TryMarkDataVersionReady(cleanupError)) {
                    m_scanSnapshot.shared_sync_complete = false;
                    m_scanSnapshot.phase = videosc::dedup::ScanPhase::Syncing;
                    m_scanMessage = "同步队列已清空，但数据版本尚未就绪：" + cleanupError;
                    m_scanMessageIsError = true;
                } else if (!m_scanCoordinator->FinalizeSynchronized(cleanupError)) {
                    m_scanMessage = "同步完成，但扫描清单清理失败：" + cleanupError;
                    m_scanMessageIsError = true;
                }
                m_scanSnapshot = m_scanCoordinator->snapshot();
            }
            if (globalPending.empty() && m_syncSnapshot.connected) {
                const bool hasHistoricalCompletedScan = std::any_of(
                    m_resumableScans.begin(),
                    m_resumableScans.end(),
                    [](const videosc::dedup::ScanCheckpoint& checkpoint) {
                        return checkpoint.phase == videosc::dedup::ScanPhase::CompletedLocal;
                    });
                if (hasHistoricalCompletedScan) {
                    std::string readyError;
                    if (TryMarkDataVersionReady(readyError)) {
                        FinalizeHistoricalSynchronizedScans();
                    } else {
                        m_scanMessage = "历史扫描等待数据版本收尾：" + readyError;
                        m_scanMessageIsError = true;
                    }
                }
            }
        }
    }
}

void VideoScApp::FinalizeHistoricalSynchronizedScans() {
    if (!m_runtimeStore || !m_syncQueue) return;
    const std::uint64_t activeScanId = m_scanCoordinator ? m_scanCoordinator->snapshot().scan_id : 0;
    const bool hasHistoricalCompletedScan = std::any_of(
        m_resumableScans.begin(),
        m_resumableScans.end(),
        [&](const videosc::dedup::ScanCheckpoint& checkpoint) {
            return checkpoint.phase == videosc::dedup::ScanPhase::CompletedLocal &&
                   checkpoint.scan_id != activeScanId;
        });
    if (!hasHistoricalCompletedScan) return;
    videosc::dedup::ScanCheckpointStore checkpointStore(*m_runtimeStore);
    std::vector<videosc::dedup::ScanCheckpoint> checkpoints;
    const videosc::dedup::RocksStatus listed = checkpointStore.ListResumable(0, checkpoints);
    if (!listed.succeeded) {
        m_scanMessage = "读取历史扫描收尾状态失败：" + listed.message;
        m_scanMessageIsError = true;
        return;
    }

    const std::filesystem::path dataRoot = m_config.rocksdb.directory.parent_path().empty()
                                               ? m_config.rocksdb.directory
                                               : m_config.rocksdb.directory.parent_path();
    bool changed = false;
    for (videosc::dedup::ScanCheckpoint& checkpoint : checkpoints) {
        if (checkpoint.phase != videosc::dedup::ScanPhase::CompletedLocal ||
            checkpoint.scan_id == activeScanId) {
            continue;
        }
        std::uint64_t staged = 0;
        std::uint64_t pending = 0;
        const videosc::dedup::RocksStatus stagedStatus = m_syncQueue->StagedCount(checkpoint.scan_id, staged);
        const videosc::dedup::RocksStatus pendingStatus = m_syncQueue->PendingCount(checkpoint.scan_id, pending);
        if (!stagedStatus.succeeded || !pendingStatus.succeeded || staged != 0 || pending != 0) continue;

        videosc::dedup::ScanManifest manifest(dataRoot, checkpoint.scan_id);
        std::string cleanupError;
        if (!manifest.Cleanup(cleanupError)) {
            m_scanMessage = "历史扫描同步完成，但清单清理失败：" + cleanupError;
            m_scanMessageIsError = true;
            continue;
        }
        checkpoint.phase = videosc::dedup::ScanPhase::CompletedSynchronized;
        const videosc::dedup::RocksStatus saved = checkpointStore.Save(checkpoint);
        if (!saved.succeeded) {
            m_scanMessage = "历史扫描清单已清理，但检查点收尾失败：" + saved.message;
            m_scanMessageIsError = true;
            continue;
        }
        changed = true;
    }
    if (changed) RefreshResumableScans();
}

void VideoScApp::RefreshResumableScans() {
    m_resumableScans.clear();
    m_selectedResumableScan = -1;
    if (!m_runtimeStore || !m_runtimeStore->is_open()) return;
    videosc::dedup::ScanCheckpointStore checkpoints(*m_runtimeStore);
    const videosc::dedup::RocksStatus status = checkpoints.ListResumable(50, m_resumableScans);
    if (!status.succeeded) {
        m_scanMessage = "读取断点列表失败：" + status.message;
        m_scanMessageIsError = true;
        return;
    }
    std::sort(m_resumableScans.begin(), m_resumableScans.end(), [](const auto& left, const auto& right) {
        return left.updated_utc_ms > right.updated_utc_ms;
    });
    if (!m_resumableScans.empty()) m_selectedResumableScan = 0;
}

void VideoScApp::StartReportGeneration(const videosc::dedup::DuplicateReportKind kind,
                                       const bool automatic) {
    if (m_reportRunning.load() || m_reportCleanupRunning.load() || m_deletionRunning.load() ||
        m_selectionRunning.load() ||
        (m_scanCoordinator && m_scanCoordinator->is_running())) {
        if (!automatic) {
            m_reportMessage = "扫描、报告生成、报告删除或永久删除正在运行。";
            m_reportMessageIsError = true;
        }
        return;
    }
    std::string runtimeError;
    if (!EnsureRuntime(runtimeError)) {
        m_reportMessage = runtimeError;
        m_reportMessageIsError = true;
        return;
    }
    if (!CheckReportReadiness(runtimeError)) {
        m_reportMessage = runtimeError;
        m_reportMessageIsError = true;
        return;
    }
    if (m_reportThread.joinable()) m_reportThread.join();
    m_reportCancelRequested.store(false);
    m_reportRunning.store(true);
    m_reportResultReady = false;
    m_reportProgress = {};
    m_imageFeatureBackfillProgress = {};
    m_lastReportProgressFraction = 0.0f;
    m_lastReportIndeterminateProgress = -1.0f;
    m_reportWasAutomatic = automatic;
    m_visibleReportKind = kind;
    m_selectedReportGroupId.reset();
    m_selectedReportGroupOrdinal.reset();
    m_selectedReportGroup.reset();
    m_showReportDetailWindow = false;
    m_clearReportThumbnailsRequested = true;
    m_reportMessage = automatic
                          ? (kind == videosc::dedup::DuplicateReportKind::Exact
                                 ? "MySQL 已同步，正在自动生成 SHA-512 精确报告。"
                                 : "SHA-512 精确报告已完成，正在自动生成 MySQL 全量视觉相似报告。")
                          : std::string("正在生成") + ReportKindName(kind) + "报告。";
    m_reportMessageIsError = false;

    videosc::dedup::RocksStore* const store = m_runtimeStore.get();
    const videosc::dedup::DatabaseConfig database = m_config.database;
    const videosc::dedup::LoggingConfig logging = m_config.logging;
    const videosc::dedup::DHashSimilarityConfig dhashSimilarity = m_config.dhash_similarity;
    const videosc::dedup::ImageSimilarityConfig imageSimilarity = m_config.image_similarity;
    const videosc::dedup::ComputeConfig compute = m_config.compute;
    const videosc::dedup::IoConfig io = m_config.io;
    const videosc::dedup::StorageConfig storage = m_config.storage;
    const std::uint64_t reportTaskId = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    if (!StartBackgroundThread(m_reportThread,
                               [this, store, database, logging, dhashSimilarity, imageSimilarity, compute, io, storage,
                                reportTaskId, kind] {
        videosc::dedup::DuplicateReportResult generated;
        const auto logThreadFailure = [&](const std::string& detail) {
            videosc::dedup::ExecutionLogger logger(logging);
            videosc::dedup::ExecutionFailureRecord failure;
            failure.task_id = reportTaskId;
            failure.task = "duplicate_report";
            failure.stage = "thread";
            failure.operation = "unhandled_exception";
            failure.status = "failed";
            failure.detail = detail;
            std::string error;
            return logger.EnsureWritable(error) && logger.WriteFailure(failure, error) ? std::string{} : error;
        };
        try {
            videosc::dedup::ExecutionLogger executionLogger(logging);
            std::string executionLogError;
            if (!executionLogger.EnsureWritable(executionLogError) ||
                !executionLogger.WriteEvent(
                    {reportTaskId,
                     "duplicate_report",
                     "started",
                     kind == videosc::dedup::DuplicateReportKind::Exact ? "exact_sha512" : "similar_visual_v4",
                     "重复报告生成已启动",
                     0,
                     0},
                    executionLogError)) {
                throw std::runtime_error("执行日志不可写：" + executionLogError);
            }
            videosc::dedup::DuplicateReportGenerator generator(
                *store,
                "media-dhash-v2",
                dhashSimilarity,
                imageSimilarity,
                compute,
                io,
                storage);
            std::mutex reportStageLogMutex;
            std::string lastReportedStage;
            const auto callback = [this, reportTaskId, &reportStageLogMutex, &lastReportedStage](
                                      const videosc::dedup::DuplicateReportProgress& progress) {
                bool stageChanged = false;
                {
                    std::lock_guard<std::mutex> stageLock(reportStageLogMutex);
                    if (progress.stage != lastReportedStage) {
                        lastReportedStage = progress.stage;
                        stageChanged = true;
                    }
                }
                if (stageChanged && !progress.stage.empty()) {
                    videosc::dedup::RuntimeLogEntry entry;
                    entry.severity = videosc::dedup::RuntimeLogSeverity::Info;
                    entry.task_id = reportTaskId;
                    entry.task = "duplicate_report";
                    entry.stage = progress.stage;
                    entry.operation = "stage_changed";
                    entry.message = DHashReportStageName(progress.stage);
                    videosc::dedup::RuntimeLogFeed::Publish(std::move(entry));
                }
                std::lock_guard<std::mutex> lock(m_reportResultMutex);
                m_reportProgress = progress;
            };
            const auto diagnosticCallback = [reportTaskId](
                                                const videosc::dedup::DuplicateReportDiagnostic& diagnostic) {
                videosc::dedup::RuntimeLogEntry entry;
                switch (diagnostic.severity) {
                    case videosc::dedup::DuplicateReportDiagnosticSeverity::Info:
                        entry.severity = videosc::dedup::RuntimeLogSeverity::Info;
                        break;
                    case videosc::dedup::DuplicateReportDiagnosticSeverity::Warning:
                        entry.severity = videosc::dedup::RuntimeLogSeverity::Warning;
                        break;
                    case videosc::dedup::DuplicateReportDiagnosticSeverity::Error:
                        entry.severity = videosc::dedup::RuntimeLogSeverity::Error;
                        break;
                }
                entry.task_id = reportTaskId;
                entry.task = "duplicate_report";
                entry.stage = diagnostic.stage;
                entry.operation = diagnostic.operation;
                entry.status_code = diagnostic.status_code;
                entry.native_error = diagnostic.native_error;
                entry.subject = diagnostic.subject;
                entry.message = diagnostic.message;
                videosc::dedup::RuntimeLogFeed::Publish(std::move(entry));
            };
            videosc::dedup::MySqlClient client(database);
            const videosc::dedup::MySqlStatus connected = client.Connect();
            if (!connected.succeeded) {
                generated.message = "MySQL 连接失败：" + connected.message;
                if (!executionLogger.WriteFailure(
                        {reportTaskId,
                         0,
                         "duplicate_report",
                         "mysql_read",
                         "connect",
                         {},
                         {},
                         {},
                         "failed",
                         0,
                         connected.native_error,
                         0,
                         0,
                         connected.message},
                        executionLogError)) {
                    throw std::runtime_error("报告失败日志写入失败：" + executionLogError);
                }
            } else {
                if (kind == videosc::dedup::DuplicateReportKind::Exact) {
                    generated = generator.GenerateExact(client, m_reportCancelRequested, callback);
                } else {
                    videosc::dedup::RuntimeLogEntry backfillStarted;
                    backfillStarted.severity = videosc::dedup::RuntimeLogSeverity::Info;
                    backfillStarted.task_id = reportTaskId;
                    backfillStarted.task = "duplicate_report";
                    backfillStarted.stage = "backfilling_image_features";
                    backfillStarted.operation = "started";
                    backfillStarted.message = "开始审计并回填历史图片三级特征";
                    videosc::dedup::RuntimeLogFeed::Publish(std::move(backfillStarted));
                    videosc::dedup::ImageFeatureBackfillCoordinator backfill(
                        *store,
                        client,
                        "media-dhash-v2",
                        compute,
                        io,
                        database.sync_batch_size);
                    const auto backfillCallback = [this](
                        const videosc::dedup::ImageFeatureBackfillProgress& backfillProgress) {
                        videosc::dedup::DuplicateReportProgress progress;
                        progress.stage = "backfilling_image_features";
                        progress.stage_total_known = true;
                        progress.stage_total = backfillProgress.total_images;
                        progress.stage_processed = backfillProgress.completed_images +
                                                   backfillProgress.failed_images;
                        progress.processed_contents = backfillProgress.completed_images;
                        std::lock_guard<std::mutex> lock(m_reportResultMutex);
                        m_reportProgress = progress;
                        m_imageFeatureBackfillProgress = backfillProgress;
                    };
                    const videosc::dedup::ImageFeatureBackfillResult backfilled =
                        backfill.Run(m_reportCancelRequested, backfillCallback);
                    {
                        std::lock_guard<std::mutex> lock(m_reportResultMutex);
                        m_imageFeatureBackfillProgress = backfilled.final_progress;
                    }
                    if (!backfilled.succeeded) {
                        generated.cancelled = backfilled.cancelled;
                        generated.message = backfilled.message;
                    } else {
                        if (!backfilled.complete) {
                            videosc::dedup::RuntimeLogEntry warning;
                            warning.severity = videosc::dedup::RuntimeLogSeverity::Warning;
                            warning.task_id = reportTaskId;
                            warning.task = "duplicate_report";
                            warning.stage = "backfilling_image_features";
                            warning.operation = "completed_incomplete";
                            warning.message = FormatIncompleteImageBackfillWarning(backfilled);
                            videosc::dedup::RuntimeLogFeed::Publish(std::move(warning));
                        }
                        generated = generator.GenerateSimilar(
                            client, m_reportCancelRequested, callback, diagnosticCallback);
                    }
                }
                client.Disconnect();
            }
            if (!generated.succeeded && !generated.cancelled && !generated.message.empty()) {
                if (!executionLogger.WriteFailure(
                        {reportTaskId,
                         0,
                         "duplicate_report",
                         "generation",
                         "generate",
                         {},
                         {},
                         {},
                         "failed",
                         0,
                         0,
                         0,
                         0,
                         generated.message},
                        executionLogError)) {
                    throw std::runtime_error("报告失败日志写入失败：" + executionLogError);
                }
            }
            const bool completedWithWarnings =
                generated.succeeded &&
                (generated.skipped_invalid_visuals != 0 || generated.structural_io_failures != 0 ||
                 generated.structural_compute_failures != 0);
            if (!executionLogger.WriteEvent(
                    {reportTaskId,
                     "duplicate_report",
                     generated.succeeded
                         ? (completedWithWarnings ? "completed_with_warnings" : "completed")
                         : (generated.cancelled ? "cancelled" : "failed"),
                     kind == videosc::dedup::DuplicateReportKind::Exact ? "exact_sha512" : "similar_visual_v4",
                     generated.message,
                     generated.group_count,
                     generated.group_count},
                    executionLogError)) {
                throw std::runtime_error("报告结束日志写入失败：" + executionLogError);
            }
        } catch (const std::bad_alloc&) {
            generated.message = "报告生成内存不足";
            const std::string logError = logThreadFailure(generated.message);
            if (!logError.empty()) generated.message += "；执行失败日志写入失败：" + logError;
            generated.message += "（" + LogApplicationException("VideoScGUI", "duplicate_report", generated.message, "std::bad_alloc") + "）";
        } catch (const std::exception& exception) {
            generated.message = exception.what();
            const std::string logError = logThreadFailure(generated.message);
            if (!logError.empty()) generated.message += "；执行失败日志写入失败：" + logError;
            generated.message += "（" + LogApplicationException("VideoScGUI", "duplicate_report", generated.message, "std::exception") + "）";
        } catch (...) {
            generated.message = "报告生成发生未知异常";
            const std::string logError = logThreadFailure(generated.message);
            if (!logError.empty()) generated.message += "；执行失败日志写入失败：" + logError;
            generated.message += "（" + LogApplicationException("VideoScGUI", "duplicate_report", generated.message, "unknown_exception") + "）";
        }
        {
            std::lock_guard<std::mutex> lock(m_reportResultMutex);
            m_reportResult = std::move(generated);
            m_reportResultReady = true;
        }
        m_reportRunning.store(false);
    }, "duplicate_report_start", m_reportMessage)) {
        m_reportRunning.store(false);
        m_reportMessageIsError = true;
    }
}

void VideoScApp::ConsumeReportResult() {
    if (m_reportRunning.load()) return;
    if (m_reportThread.joinable()) m_reportThread.join();
    videosc::dedup::DuplicateReportResult result;
    {
        std::lock_guard<std::mutex> lock(m_reportResultMutex);
        if (!m_reportResultReady) return;
        result = m_reportResult;
        m_reportResultReady = false;
    }
    if (result.succeeded) {
        m_visibleReportGeneration = result.generation_id;
        m_visibleReportGroupCount = result.group_count;
        m_loadReportRequested = true;
        m_reportMessage = std::string(ReportKindName(m_visibleReportKind)) + "报告已生成，共 " +
                          std::to_string(result.group_count) + " 组。";
        if (result.skipped_invalid_visuals != 0) {
            m_reportMessage += " 跳过 " + std::to_string(result.skipped_invalid_visuals) +
                               " 条无效视觉记录（图片 " +
                               std::to_string(result.skipped_invalid_images) + "，视频 " +
                               std::to_string(result.skipped_invalid_videos) + "）。";
        }
        if (result.structural_io_failures != 0) {
            m_reportMessage += " 结构三筛跳过 " +
                               std::to_string(result.structural_io_failures) +
                               " 个读取或解码失败候选（其中超时 " +
                               std::to_string(result.structural_timeouts) + "）。";
        }
        if (result.structural_compute_failures != 0) {
            m_reportMessage += " 结构比较失败并跳过 " +
                               std::to_string(result.structural_compute_failures) + " 个候选。";
        }
        m_reportMessageIsError = false;
        if (!m_shutdownRequested && m_reportWasAutomatic &&
            m_visibleReportKind == videosc::dedup::DuplicateReportKind::Exact &&
            m_scanCoordinator && m_scanCoordinator->generate_similar_report()) {
            StartReportGeneration(videosc::dedup::DuplicateReportKind::Similar, true);
        }
    } else if (result.cancelled) {
        m_reportMessage = "报告生成已取消，未发布半成品。";
        m_reportMessageIsError = false;
    } else {
        m_reportMessage = "报告生成失败：" + result.message;
        m_reportMessageIsError = true;
    }
}

void VideoScApp::ClearLoadedReport() {
    m_visibleReportGeneration = 0;
    m_visibleReportGroupCount = 0;
    m_visibleSimilarReportMetadata.reset();
    m_reportSummaries.clear();
    m_reportRowStarts.clear();
    m_reportGroupCache.clear();
    m_reportSelectionSnapshot = {};
    m_selectionCoversEntireReport = false;
    m_selectedReportGroupId.reset();
    m_selectedReportGroupOrdinal.reset();
    m_selectedReportGroup.reset();
    m_reportDetailIndexGroupId.reset();
    m_reportDetailMemberNames.clear();
    m_reportDetailImageRelationCount = 0;
    m_reportDetailImageRelationCache.clear();
    m_selectedImageRelationOrdinal.reset();
    m_selectedImageRelation.reset();
    m_selectedImageRelationMemberCount = 0;
    m_selectedImageRelationMemberCache.clear();
    m_reportDetailRelationLoadError.clear();
    m_showReportDetailWindow = false;
    m_resetReportScroll = true;
    m_visibleReportTrusted = true;
    m_deleteOverrideUntrusted = false;
    m_visibleSkippedStats = {};
    m_visibleSkippedContents.clear();
    m_clearReportThumbnailsRequested = true;
}

void VideoScApp::StartReportCleanup(const videosc::dedup::DuplicateReportKind kind) {
    if (m_reportCleanupRunning.load()) return;
    if (m_reportRunning.load() || m_deletionRunning.load() || m_selectionRunning.load() ||
        (m_scanCoordinator && m_scanCoordinator->is_running())) {
        m_reportMessage = "扫描、报告生成、报告删除或永久删除正在运行。";
        m_reportMessageIsError = true;
        return;
    }

    std::string runtimeError;
    if (!EnsureRuntime(runtimeError)) {
        m_reportMessage = runtimeError;
        m_reportMessageIsError = true;
        return;
    }
    if (m_reportCleanupThread.joinable()) m_reportCleanupThread.join();

    {
        std::lock_guard<std::mutex> lock(m_reportCleanupResultMutex);
        m_reportCleanupResultReady = false;
        m_reportCleanupResultMessage.clear();
    }
    m_reportCleanupRunning.store(true);
    const std::string reportName = kind == videosc::dedup::DuplicateReportKind::Exact
                                       ? "SHA-512 重复报告"
                                       : "视觉相似报告";
    m_reportMessage = "正在删除" + reportName + "……";
    m_reportMessageIsError = false;

    videosc::dedup::RocksStore* const store = m_runtimeStore.get();
    const videosc::dedup::LoggingConfig logging = m_config.logging;
    const std::uint64_t taskId = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    if (!StartBackgroundThread(
            m_reportCleanupThread,
            [this, store, logging, taskId, kind, reportName] {
                bool succeeded = false;
                std::string message;
                const std::string stage = kind == videosc::dedup::DuplicateReportKind::Exact
                                              ? "exact_sha512"
                                              : "similar_visual_v4";
                const auto logThreadFailure = [&](const std::string& detail) {
                    videosc::dedup::ExecutionLogger logger(logging);
                    videosc::dedup::ExecutionFailureRecord failure;
                    failure.task_id = taskId;
                    failure.task = "duplicate_report_cleanup";
                    failure.stage = stage;
                    failure.operation = "delete_report_namespace";
                    failure.status = "failed";
                    failure.detail = detail;
                    std::string error;
                    return logger.EnsureWritable(error) && logger.WriteFailure(failure, error)
                               ? std::string{}
                               : error;
                };
                try {
                    videosc::dedup::ExecutionLogger logger(logging);
                    std::string logError;
                    if (!logger.EnsureWritable(logError) ||
                        !logger.WriteEvent(
                            {taskId,
                             "duplicate_report_cleanup",
                             "started",
                             stage,
                             "重复报告删除已启动",
                             0,
                             0},
                            logError)) {
                        throw std::runtime_error("执行日志不可写：" + logError);
                    }

                    videosc::dedup::DuplicateReportStore reports(*store);
                    const videosc::dedup::RocksStatus deleted = reports.DeleteAll(kind);
                    succeeded = deleted.succeeded;
                    message = succeeded ? reportName + "已删除。"
                                        : "RocksDB 清理失败：" + deleted.message;
                    if (!succeeded &&
                        !logger.WriteFailure(
                            {taskId,
                             0,
                             "duplicate_report_cleanup",
                             stage,
                             "delete_report_namespace",
                             {},
                             {},
                             {},
                             "failed",
                             0,
                             0,
                             0,
                             0,
                             deleted.message},
                            logError)) {
                        throw std::runtime_error("报告删除失败日志写入失败：" + logError);
                    }
                    if (!logger.WriteEvent(
                            {taskId,
                             "duplicate_report_cleanup",
                             succeeded ? "completed" : "failed",
                             stage,
                             message,
                             succeeded ? 1ULL : 0ULL,
                             1},
                            logError)) {
                        throw std::runtime_error("报告删除结束日志写入失败：" + logError);
                    }
                } catch (const std::bad_alloc&) {
                    succeeded = false;
                    message = "删除重复报告时内存不足";
                    const std::string logError = logThreadFailure(message);
                    if (!logError.empty()) message += "；执行失败日志写入失败：" + logError;
                    message += "（" + LogApplicationException(
                                           "VideoScGUI",
                                           "duplicate_report_cleanup",
                                           message,
                                           "std::bad_alloc") +
                               "）";
                } catch (const std::exception& exception) {
                    succeeded = false;
                    message = exception.what();
                    const std::string logError = logThreadFailure(message);
                    if (!logError.empty()) message += "；执行失败日志写入失败：" + logError;
                    message += "（" + LogApplicationException(
                                           "VideoScGUI",
                                           "duplicate_report_cleanup",
                                           message,
                                           "std::exception") +
                               "）";
                } catch (...) {
                    succeeded = false;
                    message = "删除重复报告时发生未知异常";
                    const std::string logError = logThreadFailure(message);
                    if (!logError.empty()) message += "；执行失败日志写入失败：" + logError;
                    message += "（" + LogApplicationException(
                                           "VideoScGUI",
                                           "duplicate_report_cleanup",
                                           message,
                                           "unknown_exception") +
                               "）";
                }
                {
                    std::lock_guard<std::mutex> lock(m_reportCleanupResultMutex);
                    m_reportCleanupResultSucceeded = succeeded;
                    m_reportCleanupResultKind = kind;
                    m_reportCleanupResultMessage = std::move(message);
                    m_reportCleanupResultReady = true;
                }
                m_reportCleanupRunning.store(false);
            },
            "duplicate_report_cleanup_start",
            m_reportMessage)) {
        m_reportCleanupRunning.store(false);
        m_reportMessageIsError = true;
    }
}

void VideoScApp::ConsumeReportCleanupResult() {
    if (m_reportCleanupRunning.load()) return;
    if (m_reportCleanupThread.joinable()) m_reportCleanupThread.join();

    bool succeeded = false;
    videosc::dedup::DuplicateReportKind kind = videosc::dedup::DuplicateReportKind::Exact;
    std::string message;
    {
        std::lock_guard<std::mutex> lock(m_reportCleanupResultMutex);
        if (!m_reportCleanupResultReady) return;
        succeeded = m_reportCleanupResultSucceeded;
        kind = m_reportCleanupResultKind;
        message = m_reportCleanupResultMessage;
        m_reportCleanupResultReady = false;
    }
    // 活动键可能已经先于后续批量清理失效，失败时也不能继续展示旧缓存。
    if (m_visibleReportKind == kind) ClearLoadedReport();
    m_reportMessage = std::move(message);
    m_reportMessageIsError = !succeeded;
}

void VideoScApp::LoadReport() {
    m_loadReportRequested = false;
    ClearLoadedReport();
    if (!m_runtimeStore || !m_runtimeStore->is_open()) {
        std::string runtimeError;
        if (!EnsureRuntime(runtimeError)) {
            m_reportMessage = runtimeError;
            m_reportMessageIsError = true;
            return;
        }
    }
    videosc::dedup::DuplicateReportStore reports(*m_runtimeStore);
    const std::optional<std::uint64_t> active = reports.ActiveGeneration(m_visibleReportKind);
    if (!active.has_value()) {
        m_visibleReportGeneration = 0;
        m_visibleReportGroupCount = 0;
        m_reportMessage = std::string("尚未生成") + ReportKindName(m_visibleReportKind) + "报告。";
        m_reportMessageIsError = false;
        return;
    }
    m_visibleReportGeneration = *active;
    m_visibleReportGroupCount = reports.GroupCount(m_visibleReportKind, *active).value_or(0);
    if (m_visibleReportKind == videosc::dedup::DuplicateReportKind::Exact &&
        (m_reportSortMode == ReportSortMode::DHashAverageAscending ||
         m_reportSortMode == ReportSortMode::DHashAverageDescending)) {
        m_reportSortMode = ReportSortMode::Generated;
    }
    if (m_visibleReportKind == videosc::dedup::DuplicateReportKind::Similar) {
        videosc::dedup::SimilarReportMetadata metadata;
        const videosc::dedup::RocksStatus metadataStatus =
            reports.LoadSimilarMetadata(*active, metadata);
        if (!metadataStatus.succeeded) {
            m_visibleReportGeneration = 0;
            m_visibleReportGroupCount = 0;
            m_reportMessage =
                "该视觉相似报告缺少严格分组规则元数据，请重新生成。";
            m_reportMessageIsError = true;
            return;
        }
        m_visibleSimilarReportMetadata = std::move(metadata);
        m_visibleReportTrusted =
            videosc::dedup::IsSimilarReportEligibleForPermanentDeletion(
                *m_visibleSimilarReportMetadata);
        const videosc::dedup::RocksStatus skippedStatsLoaded =
            reports.CountSkippedVisualContents(*active, m_visibleSkippedStats);
        const videosc::dedup::RocksStatus skippedLoaded =
            reports.LoadSkippedContents(*active, 0, 512, m_visibleSkippedContents);
        if (!skippedStatsLoaded.succeeded || !skippedLoaded.succeeded) {
            m_visibleSkippedStats = {};
            m_visibleSkippedContents.clear();
            m_reportMessage = "加载报告跳过内容失败。";
            m_reportMessageIsError = true;
            return;
        }
    }
    const videosc::dedup::RocksStatus loaded =
        reports.LoadSummaries(m_visibleReportKind, *active, m_reportSummaries);
    if (!loaded.succeeded) {
        m_reportMessage = "加载报告摘要失败：" + loaded.message;
        m_reportMessageIsError = true;
        return;
    }
    m_reportMessageIsError = false;
    RefreshReportSelectionSnapshot();
    if (m_reportMessageIsError) return;
    ApplyReportSort();
    m_reportMessage = "已加载全部 " + std::to_string(m_reportSummaries.size()) + " 个重复组摘要。";
    m_reportMessageIsError = false;
    if (m_visibleSimilarReportMetadata.has_value() &&
        m_visibleSimilarReportMetadata->image_uses_three_stage_verification &&
        m_visibleSimilarReportMetadata->image_similarity.standard_profile.pdq_max_hamming_distance !=
            m_config.image_similarity.standard_profile.pdq_max_hamming_distance) {
        m_reportMessage +=
            " 注意：报告 PDQ 阈值=" +
            std::to_string(m_visibleSimilarReportMetadata->image_similarity.standard_profile.pdq_max_hamming_distance) +
            "，当前配置=" +
            std::to_string(m_config.image_similarity.standard_profile.pdq_max_hamming_distance) +
            "；界面按报告实际规则解释结果。";
    }
}

void VideoScApp::ApplyReportSort() {
    const auto tie = [](const videosc::dedup::ReportGroupSummary& left,
                        const videosc::dedup::ReportGroupSummary& right) {
        return left.group_id < right.group_id;
    };
    std::sort(m_reportSummaries.begin(), m_reportSummaries.end(), [&](const auto& left, const auto& right) {
        switch (m_reportSortMode) {
        case ReportSortMode::ReclaimableAscending:
            if (left.reclaimable_bytes != right.reclaimable_bytes) {
                return left.reclaimable_bytes < right.reclaimable_bytes;
            }
            break;
        case ReportSortMode::ReclaimableDescending:
            if (left.reclaimable_bytes != right.reclaimable_bytes) {
                return left.reclaimable_bytes > right.reclaimable_bytes;
            }
            break;
        case ReportSortMode::MemberCountAscending:
            if (left.member_count != right.member_count) return left.member_count < right.member_count;
            break;
        case ReportSortMode::MemberCountDescending:
            if (left.member_count != right.member_count) return left.member_count > right.member_count;
            break;
        case ReportSortMode::DHashAverageAscending:
            if (left.has_average_hamming_distance != right.has_average_hamming_distance) {
                return left.has_average_hamming_distance;
            }
            if (left.average_hamming_distance != right.average_hamming_distance) {
                return left.average_hamming_distance < right.average_hamming_distance;
            }
            break;
        case ReportSortMode::DHashAverageDescending:
            if (left.has_average_hamming_distance != right.has_average_hamming_distance) {
                return left.has_average_hamming_distance;
            }
            if (left.average_hamming_distance != right.average_hamming_distance) {
                return left.average_hamming_distance > right.average_hamming_distance;
            }
            break;
        case ReportSortMode::Generated:
            return left.ordinal < right.ordinal;
        }
        return tie(left, right);
    });
    m_reportGroupCache.clear();
    m_resetReportScroll = true;
    RebuildReportRowStarts();
}

void VideoScApp::RebuildReportRowStarts() {
    m_reportRowStarts.clear();
    m_reportRowStarts.reserve(m_reportSummaries.size() + 1);
    std::uint64_t row = 0;
    for (const auto& summary : m_reportSummaries) {
        m_reportRowStarts.push_back(row);
        const std::uint64_t rows = summary.member_count + 2;
        row = row > (std::numeric_limits<std::uint64_t>::max)() - rows
                  ? (std::numeric_limits<std::uint64_t>::max)()
                  : row + rows;
    }
    m_reportRowStarts.push_back(row);
}

videosc::dedup::DuplicateGroup* VideoScApp::AcquireReportGroup(const std::uint64_t ordinal) {
    auto cached = m_reportGroupCache.find(ordinal);
    if (cached != m_reportGroupCache.end()) {
        cached->second.last_used_frame = m_reportTextureFrame;
        return &cached->second.group;
    }
    if (!m_runtimeStore) return nullptr;
    videosc::dedup::DuplicateGroup group;
    videosc::dedup::DuplicateReportStore reports(*m_runtimeStore);
    const videosc::dedup::RocksStatus loaded =
        reports.LoadGroup(m_visibleReportKind, m_visibleReportGeneration, ordinal, group);
    if (!loaded.succeeded) {
        m_reportMessage = "加载重复组失败：" + loaded.message;
        m_reportMessageIsError = true;
        return nullptr;
    }
    videosc::dedup::ReportSelectionStore selections(*m_runtimeStore);
    std::vector<videosc::dedup::ReportSelectionMember> selectedMembers;
    const videosc::dedup::RocksStatus selectionStatus = selections.LoadGroup(
        m_visibleReportKind, m_visibleReportGeneration, group.group_id, selectedMembers);
    if (!selectionStatus.succeeded) {
        m_reportMessage = "加载持久选择失败：" + selectionStatus.message;
        m_reportMessageIsError = true;
        return nullptr;
    }
    ApplySelectionToGroup(group, selectedMembers);
    CachedReportGroup value;
    value.group = std::move(group);
    value.last_used_frame = m_reportTextureFrame;
    return &m_reportGroupCache.emplace(ordinal, std::move(value)).first->second.group;
}

void VideoScApp::TrimReportGroupCache() {
    for (auto iterator = m_reportGroupCache.begin(); iterator != m_reportGroupCache.end();) {
        if (iterator->second.last_used_frame + 2 < m_reportTextureFrame) {
            iterator = m_reportGroupCache.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

void VideoScApp::RefreshReportSelectionSnapshot() {
    m_reportSelectionSnapshot = {};
    if (!m_runtimeStore || m_visibleReportGeneration == 0) return;
    videosc::dedup::ReportSelectionStore selections(*m_runtimeStore);
    const videosc::dedup::RocksStatus loaded = selections.LoadSnapshot(
        m_visibleReportKind, m_visibleReportGeneration, m_reportSelectionSnapshot);
    if (!loaded.succeeded) {
        m_reportMessage = "读取持久选择摘要失败：" + loaded.message;
        m_reportMessageIsError = true;
    }
}

void VideoScApp::ApplyDeletionSelection(const bool entire_report) {
    if (!m_runtimeStore || m_visibleReportGeneration == 0 || m_selectionRunning.load()) return;
    UpdateConfigFromEditors();
    const std::string validationErrors = JoinValidationErrors(
        videosc::dedup::ConfigValidator::Validate(m_config));
    if (!validationErrors.empty()) {
        m_deletionMessage = validationErrors;
        m_deletionMessageIsError = true;
        return;
    }
    if (!m_retentionPolicies.any()) {
        m_deletionMessage = "至少需要启用一个保留策略。";
        m_deletionMessageIsError = true;
        return;
    }
    const std::wstring target = Utf8ToWide(m_deleteTargetStorageBuf);
    const std::optional<std::wstring> targetStorage = target.empty() ? std::nullopt
                                                                     : std::optional<std::wstring>(target);
    const std::uint32_t imageMaximum = m_visibleSimilarReportMetadata.has_value()
                                           ? m_visibleSimilarReportMetadata->image_max_hamming_distance
                                           : m_config.dhash_similarity.image_max_hamming_distance;
    const std::uint32_t videoMaximum = m_visibleSimilarReportMetadata.has_value()
                                           ? m_visibleSimilarReportMetadata->video_max_average_hamming_distance
                                           : m_config.dhash_similarity.video_max_average_hamming_distance;
    const bool imageThreeStageVerified = m_visibleSimilarReportMetadata.has_value() &&
                                         m_visibleSimilarReportMetadata->image_uses_three_stage_verification;
    const videosc::dedup::ReportSelectionConfig selectionConfig = m_config.report_selection;
    const videosc::dedup::RetentionPolicySet policies = m_retentionPolicies;

    if (entire_report) {
        if (m_selectionThread.joinable()) m_selectionThread.join();
        m_selectionRunning.store(true);
        m_selectionCancelRequested.store(false);
        m_selectionProcessedGroups.store(0);
        m_selectionTotalGroups.store(m_reportSummaries.size());
        m_selectionResultReady = false;
        m_selectionCoversEntireReport = false;
        m_deletionMessage = "正在后台读取全部报告并应用持久化安全选择。";
        m_deletionMessageIsError = false;

        const auto kind = m_visibleReportKind;
        const std::uint64_t reportGeneration = m_visibleReportGeneration;
        const auto summaries = m_reportSummaries;
        videosc::dedup::RocksStore* const store = m_runtimeStore.get();
        static std::atomic_uint64_t selectionSequence{0};
        const std::uint64_t milliseconds = (std::max<std::uint64_t>)(
            1, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count()));
        const std::uint64_t selectionGeneration =
            milliseconds * 1000ULL + (selectionSequence.fetch_add(1) % 1000ULL);
        if (!StartBackgroundThread(
                m_selectionThread,
                [this,
                 store,
                 kind,
                 reportGeneration,
                 summaries,
                  policies,
                 targetStorage,
                 selectionConfig,
                 imageMaximum,
                 videoMaximum,
                 imageThreeStageVerified,
                 selectionGeneration] {
                    bool succeeded = false;
                    bool cancelled = false;
                    std::string message;
                    videosc::dedup::ReportSelectionSnapshot snapshot;
                    videosc::dedup::ReportSelectionStore selections(*store);
                    try {
                        videosc::dedup::RocksStatus status = selections.BeginReplacement(
                            kind, reportGeneration, selectionGeneration);
                        if (!status.succeeded) throw std::runtime_error(status.message);
                        videosc::dedup::DuplicateReportStore reports(*store);
                         std::uint64_t rejected = 0;
                         std::uint64_t targetSkipped = 0;
                         for (const auto& summary : summaries) {
                            if (m_selectionCancelRequested.load()) {
                                cancelled = true;
                                break;
                            }
                            videosc::dedup::DuplicateGroup group;
                            status = reports.LoadGroup(kind, reportGeneration, summary.ordinal, group);
                            if (!status.succeeded) throw std::runtime_error(status.message);
                             const auto planned = videosc::dedup::DeletionPlanner::SelectDetailed(
                                 group, policies, targetStorage);
                             if (planned.succeeded()) {
                                 const auto members = BuildSafeSelection(
                                     group,
                                     *planned.plan,
                                    selectionConfig,
                                    imageMaximum,
                                    videoMaximum,
                                    imageThreeStageVerified,
                                    rejected);
                                status = selections.SaveReplacementGroup(
                                    kind,
                                    reportGeneration,
                                    selectionGeneration,
                                    group.group_id,
                                    members);
                                 if (!status.succeeded) throw std::runtime_error(status.message);
                             } else if (planned.status ==
                                        videosc::dedup::DeletionPlanStatus::TargetStorageNotFound) {
                                 ++targetSkipped;
                             } else {
                                 rejected += group.members.size() > 1 ? group.members.size() - 1 : 1;
                             }
                            m_selectionProcessedGroups.fetch_add(1);
                        }
                        if (!cancelled) {
                            status = selections.PublishReplacement(
                                kind, reportGeneration, selectionGeneration, snapshot);
                            if (!status.succeeded) throw std::runtime_error(status.message);
                            succeeded = true;
                             message = "全部报告选择完成：选中 " +
                                       std::to_string(snapshot.selected_file_count) + " 个文件，共 " +
                                       FormatBytes(snapshot.selected_total_bytes) + "；安全规则拒绝 " +
                                       std::to_string(rejected) + " 个候选；指定磁盘无匹配跳过 " +
                                       std::to_string(targetSkipped) + " 组。";
                        } else {
                            message = "全报告选择已取消，继续保留上一份完整选择。";
                        }
                    } catch (const std::exception& exception) {
                        message = "全报告选择失败：" + std::string(exception.what());
                    } catch (...) {
                        message = "全报告选择发生未知异常。";
                    }
                    if (!succeeded) {
                        selections.DiscardReplacement(kind, reportGeneration, selectionGeneration);
                    }
                    {
                        std::lock_guard<std::mutex> lock(m_selectionResultMutex);
                        m_selectionResultSucceeded = succeeded;
                        m_selectionResultMessage = std::move(message);
                        m_selectionResultSnapshot = snapshot;
                        m_selectionResultReady = true;
                    }
                    m_selectionRunning.store(false);
                },
                "report_selection_start",
                m_deletionMessage)) {
            m_selectionRunning.store(false);
            m_deletionMessageIsError = true;
        }
        return;
    }

    if (!m_selectedReportGroupOrdinal.has_value() || !m_selectedReportGroupId.has_value()) {
        m_deletionMessage = "请先在重复组浏览窗口中点击选择一个当前组。";
        m_deletionMessageIsError = true;
        return;
    }

    videosc::dedup::DuplicateReportStore reports(*m_runtimeStore);
    videosc::dedup::DuplicateGroup group;
    const videosc::dedup::RocksStatus groupLoaded = reports.LoadGroup(
        m_visibleReportKind,
        m_visibleReportGeneration,
        *m_selectedReportGroupOrdinal,
        group);
    if (!groupLoaded.succeeded || group.group_id != *m_selectedReportGroupId) {
        m_deletionMessage = groupLoaded.succeeded
                                ? "当前组已随报告 generation 变化，请重新选择。"
                                : "读取当前重复组失败：" + groupLoaded.message;
        m_deletionMessageIsError = true;
        return;
    }

    std::uint64_t rejected = 0;
    videosc::dedup::ReportSelectionStore selections(*m_runtimeStore);
    const auto planned = videosc::dedup::DeletionPlanner::SelectDetailed(
        group, policies, targetStorage);
    if (!planned.succeeded()) {
        m_deletionMessage = planned.message;
        m_deletionMessageIsError = true;
        return;
    }
    const auto members = BuildSafeSelection(
        group,
        *planned.plan,
        selectionConfig,
        imageMaximum,
        videoMaximum,
        imageThreeStageVerified,
        rejected);
    videosc::dedup::ReportSelectionSnapshot snapshot;
    const videosc::dedup::RocksStatus saved = selections.SetGroup(
        m_visibleReportKind,
        m_visibleReportGeneration,
        group.group_id,
        members,
        snapshot);
    if (!saved.succeeded) {
        m_deletionMessage = "保存当前组选择失败：" + saved.message;
        m_deletionMessageIsError = true;
        return;
    }
    ApplySelectionToGroup(group, members);
    m_reportSelectionSnapshot = snapshot;
    m_selectedReportGroup = group;
    const auto cached = m_reportGroupCache.find(*m_selectedReportGroupOrdinal);
    if (cached != m_reportGroupCache.end()) cached->second.group = group;
    m_selectionCoversEntireReport = false;
    m_deletionMessage = "当前组已安全选中 " + std::to_string(members.size()) +
                        " 个文件；因距离上限拒绝 " + std::to_string(rejected) + " 个。";
    m_deletionMessageIsError = false;
}

void VideoScApp::ConsumeSelectionResult() {
    if (m_selectionRunning.load()) return;
    if (m_selectionThread.joinable()) m_selectionThread.join();
    std::lock_guard<std::mutex> lock(m_selectionResultMutex);
    if (!m_selectionResultReady) return;
    m_deletionMessage = m_selectionResultMessage;
    m_deletionMessageIsError = !m_selectionResultSucceeded;
    if (m_selectionResultSucceeded) {
        m_reportSelectionSnapshot = m_selectionResultSnapshot;
        m_selectionCoversEntireReport = true;
        m_reportGroupCache.clear();
    }
    m_selectionResultReady = false;
}

void VideoScApp::StartDeletion() {
    if (!m_runtimeStore || !m_syncQueue || m_deletionRunning.load()) return;
    if (m_selectionRunning.load()) {
        m_deletionMessage = "全报告选择尚未完成，不能开始永久删除。";
        m_deletionMessageIsError = true;
        return;
    }
    if (m_scanCoordinator && m_scanCoordinator->is_running()) {
        m_deletionMessage = "扫描运行时禁止永久删除。";
        m_deletionMessageIsError = true;
        return;
    }
    if (m_reportRunning.load()) {
        m_deletionMessage = "报告生成运行时禁止永久删除。";
        m_deletionMessageIsError = true;
        return;
    }
    if (m_reportCleanupRunning.load()) {
        m_deletionMessage = "报告删除运行时禁止永久删除文件。";
        m_deletionMessageIsError = true;
        return;
    }
    if (m_deletionThread.joinable()) m_deletionThread.join();

    RefreshReportSelectionSnapshot();
    if (m_reportSelectionSnapshot.selected_file_count == 0) {
        m_deletionMessage = "没有持久化的待删除选择。";
        m_deletionMessageIsError = true;
        return;
    }

    const bool overrideUntrusted = m_deleteOverrideUntrusted;
    m_deleteOverrideUntrusted = false;
    m_deletionStopRequested.store(false);
    m_deletionRunning.store(true);
    m_deletionTotalFiles.store(m_reportSelectionSnapshot.selected_file_count);
    m_deletionProcessedFiles.store(0);
    m_deletionProcessedBytes.store(0);
    m_deletionProgressReclaimedBytes.store(0);
    m_deletionProgressSucceeded.store(0);
    m_deletionProgressFailed.store(0);
    {
        std::lock_guard<std::mutex> lock(m_deletionProgressMutex);
        m_deletionProgressStage = "复核选择与文件内容";
        m_deletionCurrentPath.clear();
    }
    m_deletionResultReady = false;
    m_deletionMessage = "正在逐个复核完整 SHA-512 并永久删除选中文件。";
    m_deletionMessageIsError = false;
    const auto reportKind = m_visibleReportKind;
    const std::uint64_t generation = m_visibleReportGeneration;
    const std::uint64_t selectedGroupCount = m_reportSelectionSnapshot.selected_group_count;
    const videosc::dedup::IoConfig io = m_config.io;
    const videosc::dedup::LoggingConfig logging = m_config.logging;
    const std::uint64_t deletionTaskId = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    videosc::dedup::RocksStore* const store = m_runtimeStore.get();
    videosc::dedup::MySqlSyncQueue* const queue = m_syncQueue.get();
    videosc::dedup::MySqlSyncService* const sync = m_syncService.get();
    if (!StartBackgroundThread(m_deletionThread, [this,
                                    store,
                                    queue,
                                    sync,
                                    reportKind,
                                    generation,
                                    selectedGroupCount,
                                    io,
                                    logging,
                                    deletionTaskId,
                                    overrideUntrusted]() mutable {
        std::uint64_t deleted = 0;
        std::uint64_t reclaimed = 0;
        std::uint64_t failed = 0;
        std::string message;
        const auto logThreadFailure = [&](const std::string& detail) {
            videosc::dedup::ExecutionLogger logger(logging);
            videosc::dedup::ExecutionFailureRecord failure;
            failure.task_id = deletionTaskId;
            failure.task = "permanent_delete";
            failure.stage = "thread";
            failure.operation = "unhandled_exception";
            failure.status = "failed";
            failure.detail = detail;
            std::string error;
            return logger.EnsureWritable(error) && logger.WriteFailure(failure, error) ? std::string{} : error;
        };
        try {
            videosc::dedup::ExecutionLogger executionLogger(logging);
            std::string executionLogError;
            if (!executionLogger.EnsureWritable(executionLogError) ||
                !executionLogger.WriteEvent(
                    {deletionTaskId,
                     "permanent_delete",
                     "started",
                     "preflight",
                     "永久删除任务已启动",
                     0,
                     selectedGroupCount},
                    executionLogError)) {
                throw std::runtime_error("执行日志不可写：" + executionLogError);
            }
            videosc::dedup::OperationLogger logger(logging);
            std::string logError;
            if (!logger.EnsureWritable(logError)) throw std::runtime_error("操作日志不可写：" + logError);
            auto hasher = std::make_shared<videosc::dedup::VideoScFileHasher>(io);
            videosc::dedup::DeletionExecutor executor(*store, *queue, std::move(hasher), logger);
            videosc::dedup::ReportSelectionStore selections(*store);
            std::uint64_t safetyRejected = 0;
            const auto execute = [&](const videosc::dedup::DuplicateGroup& plan,
                                     const std::vector<videosc::dedup::ReportSelectionMember>& selectedMembers) {
                const videosc::dedup::DeletionBatchResult batch = executor.Execute(
                    plan,
                    [this, &plan](const videosc::dedup::DeletionItemResult& item,
                                  const videosc::dedup::DeletionBatchResult&) {
                        const auto member = std::find_if(
                            plan.members.begin(), plan.members.end(), [&](const auto& value) {
                                return value.path_id == item.path_id;
                            });
                        m_deletionProcessedFiles.fetch_add(1);
                        if (member != plan.members.end()) {
                            m_deletionProcessedBytes.fetch_add(member->size_bytes);
                        }
                        if (item.deleted) {
                            m_deletionProgressSucceeded.fetch_add(1);
                            if (member != plan.members.end()) {
                                m_deletionProgressReclaimedBytes.fetch_add(member->size_bytes);
                            }
                        } else {
                            m_deletionProgressFailed.fetch_add(1);
                        }
                        std::lock_guard<std::mutex> lock(m_deletionProgressMutex);
                        m_deletionProgressStage = "逐文件复核并永久删除";
                        m_deletionCurrentPath = item.path;
                    },
                    [this]() { return m_deletionStopRequested.load(std::memory_order_relaxed); });
                deleted += batch.deleted_files;
                reclaimed += batch.reclaimed_bytes;
                failed += batch.items.size() - batch.deleted_files;
                std::vector<videosc::dedup::ReportSelectionMember> remaining;
                for (const auto& selected : selectedMembers) {
                    const auto item = std::find_if(batch.items.begin(), batch.items.end(), [&](const auto& result) {
                        return result.path_id == selected.path_id;
                    });
                    if (item == batch.items.end() || !item->deleted) remaining.push_back(selected);
                }
                videosc::dedup::ReportSelectionSnapshot updatedSelection;
                const videosc::dedup::RocksStatus selectionUpdated = selections.SetGroup(
                    reportKind, generation, plan.group_id, remaining, updatedSelection);
                if (!selectionUpdated.succeeded) {
                    throw std::runtime_error("更新删除后的选择状态失败：" + selectionUpdated.message);
                }
                if (!executionLogger.WriteEvent(
                        {deletionTaskId,
                         "permanent_delete",
                         "batch_completed",
                         "delete",
                         "删除组处理完成，释放 " + FormatBytes(batch.reclaimed_bytes),
                         batch.deleted_files,
                         batch.items.size()},
                        executionLogError)) {
                    throw std::runtime_error("删除批次执行日志写入失败：" + executionLogError);
                }
            };
            videosc::dedup::DuplicateReportStore reports(*store);
            bool trustedThreeStageReport = reportKind == videosc::dedup::DuplicateReportKind::Exact;
            if (reportKind == videosc::dedup::DuplicateReportKind::Similar) {
                videosc::dedup::SimilarReportMetadata metadata;
                const videosc::dedup::RocksStatus metadataLoaded =
                    reports.LoadSimilarMetadata(generation, metadata);
                if (!metadataLoaded.succeeded) {
                    throw std::runtime_error("删除前报告规则校验失败：" + metadataLoaded.message);
                }
                trustedThreeStageReport =
                    videosc::dedup::IsSimilarReportEligibleForPermanentDeletion(metadata);
                if (!trustedThreeStageReport) {
                    if (!overrideUntrusted) {
                        throw std::runtime_error(
                            "当前相似报告为旧规则报告，缺少三筛删除证据；如确认风险，请在删除确认窗口勾选强制删除。");
                    }
                    if (!executionLogger.WriteEvent(
                            {deletionTaskId,
                             "permanent_delete",
                             "override_untrusted_report",
                             "preflight",
                             "用户确认风险，对旧规则相似报告（generation=" +
                                 std::to_string(generation) + "）强制执行永久删除",
                             0,
                             selectedGroupCount},
                            executionLogError)) {
                        throw std::runtime_error("强制删除审计日志写入失败：" + executionLogError);
                    }
                    trustedThreeStageReport = true;
                }
            }
            std::vector<videosc::dedup::ReportGroupSummary> reportSummaries;
            const videosc::dedup::RocksStatus summariesLoaded =
                reports.LoadSummaries(reportKind, generation, reportSummaries);
            if (!summariesLoaded.succeeded) {
                throw std::runtime_error("读取已发布报告摘要失败：" + summariesLoaded.message);
            }
            std::unordered_map<std::uint64_t, std::uint64_t> groupOrdinals;
            groupOrdinals.reserve(reportSummaries.size());
            for (const auto& summary : reportSummaries) {
                groupOrdinals.emplace(summary.group_id, summary.ordinal);
            }
            std::vector<std::uint64_t> selectedGroupIds;
            selectedGroupIds.reserve(static_cast<std::size_t>(selectedGroupCount));
            const videosc::dedup::RocksStatus selectedGroupsLoaded =
                selections.ForEachSelectedGroup(
                    reportKind,
                    generation,
                    [&](const std::uint64_t groupId) {
                        selectedGroupIds.push_back(groupId);
                        return true;
                    });
            if (!selectedGroupsLoaded.succeeded) {
                throw std::runtime_error("枚举持久选择失败：" + selectedGroupsLoaded.message);
            }
            if (selectedGroupIds.size() != selectedGroupCount) {
                throw std::runtime_error("持久选择摘要与组选中索引数量不一致");
            }
            for (const std::uint64_t groupId : selectedGroupIds) {
                if (m_deletionStopRequested.load(std::memory_order_relaxed)) break;
                std::vector<videosc::dedup::ReportSelectionMember> selectedMembers;
                videosc::dedup::RocksStatus loaded = selections.LoadGroup(
                    reportKind, generation, groupId, selectedMembers);
                if (!loaded.succeeded) {
                    throw std::runtime_error("读取持久选择失败：" + loaded.message);
                }
                if (selectedMembers.empty()) continue;
                const auto ordinal = groupOrdinals.find(groupId);
                if (ordinal == groupOrdinals.end()) {
                    throw std::runtime_error("持久选择引用的重复组不在当前已发布报告中：" +
                                             std::to_string(groupId));
                }
                videosc::dedup::DuplicateGroup group;
                loaded = reports.LoadGroup(reportKind, generation, ordinal->second, group);
                if (!loaded.succeeded) {
                    throw std::runtime_error("读取删除重复组失败：" + loaded.message);
                }
                const auto plan = BuildPersistedDeletionPlan(
                    group, selectedMembers, trustedThreeStageReport, safetyRejected);
                for (const auto& selected : selectedMembers) {
                    const bool accepted = plan.has_value() &&
                        std::find(plan->selected_for_deletion.begin(),
                                  plan->selected_for_deletion.end(),
                                  selected.path_id) != plan->selected_for_deletion.end();
                    if (accepted) continue;
                    videosc::dedup::ExecutionFailureRecord failure;
                    failure.task_id = deletionTaskId;
                    failure.path_id = selected.path_id;
                    failure.task = "permanent_delete";
                    failure.stage = "preflight";
                    failure.operation = "dhash_safety_recheck";
                    failure.status = "skipped";
                    const auto member = std::find_if(group.members.begin(), group.members.end(), [&](const auto& value) {
                        return value.path_id == selected.path_id;
                    });
                    if (member != group.members.end()) {
                        failure.path = member->path;
                        failure.storage_target_key = member->storage_target_key;
                    }
                    std::ostringstream detail;
                    detail << "删除前安全复核未通过；选择时距离=" << selected.measured_distance
                           << "，严格上限=" << selected.exclusive_limit
                           << "，保留路径ID=" << selected.retained_path_id;
                    failure.detail = detail.str();
                    if (!executionLogger.WriteFailure(failure, executionLogError)) {
                        throw std::runtime_error("安全复核逐文件日志写入失败：" + executionLogError);
                    }
                }
                if (plan.has_value()) execute(*plan, selectedMembers);
            }
            if (safetyRejected != 0) {
                m_deletionProcessedFiles.fetch_add(safetyRejected);
                m_deletionProgressFailed.fetch_add(safetyRejected);
                videosc::dedup::ExecutionFailureRecord failure;
                failure.task_id = deletionTaskId;
                failure.task = "permanent_delete";
                failure.stage = "preflight";
                failure.operation = "dhash_safety_recheck";
                failure.status = "skipped";
                failure.detail = "删除前 dHash 安全复核拒绝 " + std::to_string(safetyRejected) + " 个文件";
                if (!executionLogger.WriteFailure(failure, executionLogError)) {
                    throw std::runtime_error("安全复核跳过日志写入失败：" + executionLogError);
                }
                failed += safetyRejected;
            }
            const bool stopped = m_deletionStopRequested.load(std::memory_order_relaxed);
            if (sync && deleted != 0) sync->Wake();
            {
                std::lock_guard<std::mutex> lock(m_deletionProgressMutex);
                m_deletionProgressStage = stopped
                                              ? "已安全停止，未开始文件保留选择"
                                              : "写入本地映射并等待 MySQL 同步";
                m_deletionCurrentPath.clear();
            }
            message = stopped
                          ? "永久删除已安全停止，未开始文件继续保留选择。"
                          : "永久删除完成。";
            if (!executionLogger.WriteEvent(
                    {deletionTaskId,
                     "permanent_delete",
                     stopped ? "cancelled" : "completed",
                     "completed",
                     message,
                     deleted,
                     deleted + failed},
                    executionLogError)) {
                throw std::runtime_error("删除结束日志写入失败：" + executionLogError);
            }
        } catch (const std::bad_alloc&) {
            message = "永久删除任务内存不足";
            const std::string logError = logThreadFailure(message);
            if (!logError.empty()) message += "；执行失败日志写入失败：" + logError;
            message += "（" + LogApplicationException("VideoScGUI", "permanent_delete", message, "std::bad_alloc") + "）";
            if (logError.empty()) ++failed;
        } catch (const std::exception& exception) {
            message = exception.what();
            const std::string logError = logThreadFailure(message);
            if (!logError.empty()) message += "；执行失败日志写入失败：" + logError;
            message += "（" + LogApplicationException("VideoScGUI", "permanent_delete", message, "std::exception") + "）";
            if (logError.empty()) ++failed;
        } catch (...) {
            message = "永久删除任务发生未知异常";
            const std::string logError = logThreadFailure(message);
            if (!logError.empty()) message += "；执行失败日志写入失败：" + logError;
            message += "（" + LogApplicationException("VideoScGUI", "permanent_delete", message, "unknown_exception") + "）";
            if (logError.empty()) ++failed;
        }
        {
            std::lock_guard<std::mutex> lock(m_deletionResultMutex);
            m_deletedFiles = deleted;
            m_reclaimedBytes = reclaimed;
            m_failedDeletions = failed;
            m_deletionResultMessage = std::move(message);
            m_deletionResultReady = true;
        }
        m_deletionRunning.store(false);
    }, "permanent_delete_start", m_deletionMessage)) {
        m_deletionRunning.store(false);
        m_deletionMessageIsError = true;
    }
}

void VideoScApp::ConsumeDeletionResult() {
    if (m_deletionRunning.load()) return;
    if (m_deletionThread.joinable()) m_deletionThread.join();
    std::lock_guard<std::mutex> lock(m_deletionResultMutex);
    if (!m_deletionResultReady) return;
    m_deletionMessage = m_deletionResultMessage + " 已删除 " + std::to_string(m_deletedFiles) +
                        " 个文件，释放 " + FormatBytes(m_reclaimedBytes) +
                        "；失败/跳过 " + std::to_string(m_failedDeletions) + " 个。请重新生成报告。";
    m_deletionMessageIsError = m_failedDeletions != 0;
    m_deletionResultReady = false;
    m_selectionCoversEntireReport = false;
    RefreshReportSelectionSnapshot();
    m_reportGroupCache.clear();
}

ReportThumbnailTex* VideoScApp::AcquireReportThumbnail(const std::filesystem::path& path,
                                                       const videosc::dedup::MediaKind media_kind) {
    if (path.empty()) return nullptr;
    auto found = m_reportThumbnails.find(path.wstring());
    if (found != m_reportThumbnails.end()) {
        found->second.last_used_frame = m_reportTextureFrame;
        return &found->second;
    }
    const std::size_t entryLimit = (std::max<std::size_t>)(1, m_config.thumbnails.cache_entries);
    if (m_reportThumbnails.size() >= entryLimit) return nullptr;
    const std::uint64_t gpuLimit =
        static_cast<std::uint64_t>((std::max)(1U, m_config.thumbnails.gpu_memory_limit_mib)) * 1024ULL * 1024ULL;
    std::uint64_t currentGpuBytes = 0;
    for (const auto& item : m_reportThumbnails) {
        currentGpuBytes += static_cast<std::uint64_t>((std::max)(0, item.second.width)) *
                           static_cast<std::uint64_t>((std::max)(0, item.second.height)) * 4ULL;
    }
    if (currentGpuBytes >= gpuLimit) return nullptr;
    constexpr std::uint32_t maximumUploadsPerFrame = 2;
    if (m_reportTextureUploadsThisFrame >= maximumUploadsPerFrame) return nullptr;
    ++m_reportTextureUploadsThisFrame;
    ReportThumbnailTex texture;
    texture.srv = LoadImageToTexture(path.wstring(), &texture.width, &texture.height);
    texture.last_used_frame = m_reportTextureFrame;
    if (!texture.srv) {
        if (m_reportThumbnailFailures.insert(path.wstring()).second) {
            videosc::dedup::ExecutionLogger logger(m_config.logging);
            videosc::dedup::ExecutionFailureRecord failure;
            failure.task = "duplicate_report_detail";
            failure.stage = "preview";
            failure.operation = media_kind == videosc::dedup::MediaKind::Image
                                    ? "image_thumbnail_decode"
                                    : "video_contact_sheet_load";
            failure.path = path;
            failure.media_kind = media_kind == videosc::dedup::MediaKind::Image ? "image" : "video";
            failure.status = "failed";
            failure.detail = "preview_unavailable";
            std::string logError;
            if (!logger.EnsureWritable(logError) || !logger.WriteFailure(failure, logError)) {
                m_reportMessage = "预览失败且执行失败日志写入失败：" + logError;
                m_reportMessageIsError = true;
            }
        }
        return nullptr;
    }
    return &m_reportThumbnails.emplace(path.wstring(), texture).first->second;
}

void VideoScApp::TrimReportThumbnails() {
    const std::size_t configuredLimit = (std::max<std::size_t>)(1, m_config.thumbnails.cache_entries);
    const std::uint64_t configuredGpuBytes =
        static_cast<std::uint64_t>((std::max)(1U, m_config.thumbnails.gpu_memory_limit_mib)) * 1024ULL * 1024ULL;
    std::uint64_t estimatedGpuBytes = 0;
    for (const auto& item : m_reportThumbnails) {
        estimatedGpuBytes += static_cast<std::uint64_t>((std::max)(0, item.second.width)) *
                             static_cast<std::uint64_t>((std::max)(0, item.second.height)) * 4ULL;
    }
    for (auto iterator = m_reportThumbnails.begin(); iterator != m_reportThumbnails.end();) {
        const bool stale = iterator->second.last_used_frame + 2 < m_reportTextureFrame;
        const bool overLimit = (m_reportThumbnails.size() > configuredLimit ||
                                estimatedGpuBytes > configuredGpuBytes) &&
                               iterator->second.last_used_frame != m_reportTextureFrame;
        if (!stale && !overLimit) {
            ++iterator;
            continue;
        }
        estimatedGpuBytes -= static_cast<std::uint64_t>((std::max)(0, iterator->second.width)) *
                             static_cast<std::uint64_t>((std::max)(0, iterator->second.height)) * 4ULL;
        if (iterator->second.srv) iterator->second.srv->Release();
        iterator = m_reportThumbnails.erase(iterator);
    }
}

void VideoScApp::ClearReportThumbnails() {
    for (auto& item : m_reportThumbnails) {
        if (item.second.srv) item.second.srv->Release();
    }
    m_reportThumbnails.clear();
    m_reportThumbnailFailures.clear();
}

VideoScApp::~VideoScApp() {
    RequestShutdown();
    m_reportCancelRequested.store(true);
    m_selectionCancelRequested.store(true);
    if (m_reportThread.joinable()) m_reportThread.join();
    if (m_reportCleanupThread.joinable()) m_reportCleanupThread.join();
    if (m_selectionThread.joinable()) m_selectionThread.join();
    if (m_deletionThread.joinable()) m_deletionThread.join();
    ShutdownRuntime();
    ClearReportThumbnails();
    CleanupReportPreviewDirectory();
    if (m_databaseInitThread.joinable()) m_databaseInitThread.join();
    for (auto& t : m_thumbnails) {
        if (t.srv) t.srv->Release();
    }
    for (auto& t : m_retiredThumbnails) {
        if (t.srv) t.srv->Release();
    }
    FreeVideoScResult(&m_result);
}

void VideoScApp::RequestShutdown() noexcept {
    bool expected = false;
    if (!m_shutdownRequested.compare_exchange_strong(expected, true, std::memory_order_relaxed)) return;
    m_shutdownPhase = ShutdownPhase::CloseRequested;
    m_shutdownMessage = "正在请求后台任务停止";
    m_reportCancelRequested.store(true);
    m_selectionCancelRequested.store(true);
    m_deletionStopRequested.store(true);
    m_previewCancelRequested.store(true);
    m_fileQuery.Cancel();
    if (m_scanCoordinator) m_scanCoordinator->Cancel();
    if (m_syncService) m_syncService->RequestStop();
}

void VideoScApp::AdvanceShutdown() {
    if (!m_shutdownRequested.load(std::memory_order_relaxed) || IsShutdownComplete()) return;
    m_shutdownPhase = m_deletionRunning.load() ? ShutdownPhase::FinishingCurrentDeletion
                                               : ShutdownPhase::CancellingTasks;

    bool previewRunning = false;
    for (auto& job : m_imagePreviewJobs) {
        if (job.second.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
            previewRunning = true;
            break;
        }
    }
    if (!previewRunning) {
        for (auto& job : m_videoPreviewJobs) {
            if (job.second.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
                previewRunning = true;
                break;
            }
        }
    }

    const bool scanRunning = m_scanCoordinator && m_scanCoordinator->is_running();
    const bool syncRunning = m_syncService && m_syncService->IsRunning();
    const bool taskRunning = scanRunning || syncRunning || m_reportRunning.load() ||
                             m_reportCleanupRunning.load() || m_selectionRunning.load() ||
                             m_deletionRunning.load() || m_databaseInitRunning.load() ||
                             m_fileQuery.IsRunning() || previewRunning;
    if (taskRunning) {
        if (m_deletionRunning.load()) {
            m_shutdownMessage = "永久删除正在完成当前文件的安全记录，完成后不再领取新文件";
        } else if (scanRunning || m_reportRunning.load() || m_selectionRunning.load() || previewRunning) {
            m_shutdownMessage = "正在取消扫描、报告、选择或预览任务";
        } else if (syncRunning) {
            m_shutdownPhase = ShutdownPhase::StoppingSync;
            m_shutdownMessage = "正在停止 MySQL 后台同步";
        } else {
            m_shutdownMessage = "正在等待当前数据库或报告清理操作完成";
        }
        return;
    }

    // 所有运行标志都已归零，此时 join 不再阻塞窗口消息循环。
    if (m_reportThread.joinable()) m_reportThread.join();
    if (m_reportCleanupThread.joinable()) m_reportCleanupThread.join();
    if (m_selectionThread.joinable()) m_selectionThread.join();
    if (m_deletionThread.joinable()) m_deletionThread.join();
    if (m_databaseInitThread.joinable()) m_databaseInitThread.join();
    if (m_scanCoordinator) m_scanCoordinator->Wait();
    if (m_syncService) m_syncService->Wait();
    CleanupReportPreviewDirectory();
    m_shutdownPhase = ShutdownPhase::ClosingRocksDb;
    m_shutdownMessage = "正在关闭 RocksDB 并保存最终状态";
    ShutdownRuntime();
    m_shutdownPhase = ShutdownPhase::ReadyToDestroyWindow;
    m_shutdownMessage = "安全退出完成";
}

// -----------------------------------------------------------------------------
// Render(): renders all UI windows once per frame.
// -----------------------------------------------------------------------------

void VideoScApp::Render() {
    for (auto& texture : m_retiredThumbnails) {
        if (texture.srv) texture.srv->Release();
    }
    m_retiredThumbnails.clear();
    if (m_shutdownRequested) ImGui::BeginDisabled();
    RenderDockSpace();
    RenderScanWindow();
    RenderScanPathsWindow();
    if (m_clearReportThumbnailsRequested) {
        ClearReportThumbnails();
        m_clearReportThumbnailsRequested = false;
    }
    ++m_reportTextureFrame;
    m_reportTextureUploadsThisFrame = 0;
    RenderDuplicateReportWindow();
    RenderDuplicateGroupBrowserWindow();
    RenderDuplicateReportDetailWindow();
    RenderLogWindow();
    TrimReportGroupCache();
    TrimReportThumbnails();
    RenderScreenshotWindow();
    RenderResultsWindow();
    RenderDiskInfoWindow();
    RenderFileSearchWindow();
    RenderSettingsWindow();
    RenderPreviousCrashPopup();
    if (m_shutdownRequested) ImGui::EndDisabled();
    RenderShutdownOverlay();
}

void VideoScApp::RenderShutdownOverlay() {
    if (!m_shutdownRequested) return;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560.0f, 0.0f), ImGuiCond_Always);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking;
    if (ImGui::Begin("正在安全退出", nullptr, flags)) {
        ImGui::TextWrapped("%s", m_shutdownMessage.c_str());
        ImGui::Separator();
        ImGui::Text("扫描：%s", m_scanCoordinator && m_scanCoordinator->is_running() ? "收口中" : "已停止");
        ImGui::Text("报告：%s", m_reportRunning.load() || m_reportCleanupRunning.load() ? "收口中" : "已停止");
        ImGui::Text("选择：%s", m_selectionRunning.load() ? "收口中" : "已停止");
        ImGui::Text("永久删除：%s", m_deletionRunning.load() ? "完成当前文件后停止" : "已停止");
        ImGui::Text("数据库初始化：%s", m_databaseInitRunning.load() ? "等待当前阶段" : "已停止");
        ImGui::Text("MySQL 同步：%s", m_syncService && m_syncService->IsRunning() ? "收口中" : "已停止");
        DrawContrastProgressBar(-static_cast<float>(ImGui::GetTime()),
                                ImVec2(ImGui::GetContentRegionAvail().x, 0.0f),
                                "请勿强制结束进程");
    }
    ImGui::End();
}

void VideoScApp::RenderPreviousCrashPopup() {
    if (m_previousCrashPopupRequested) {
        ImGui::OpenPopup("上次运行异常退出");
        m_previousCrashPopupRequested = false;
    }
    if (!ImGui::BeginPopupModal("上次运行异常退出", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextWrapped("检测到上次运行留下的崩溃诊断文件。程序不会自动上传这些文件。");
    TextCopyable("##previous_crash", WideToUtf8(m_previousCrashMetadata.wstring().c_str()));
    PushButtonStylePrimary();
    if (ImGui::Button("打开诊断目录", ImVec2(150, 30))) {
        ShellExecuteW(nullptr, L"explore", CrashHandler::CrashDirectory().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::PopStyleColor(3);
    ImGui::SameLine();
    PushButtonStyleNeutral();
    if (ImGui::Button("已了解", ImVec2(100, 30))) {
        CrashHandler::MarkLatestCrashReviewed();
        m_previousCrashMetadata.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::PopStyleColor(3);
    ImGui::EndPopup();
}

// -----------------------------------------------------------------------------
// TextCopyable: display text as a read-only InputText so the user can
// select it with the mouse and copy via Ctrl+C (or right-click > Copy).
// Renders without border/frame so it looks like plain text.
// Uses a persistent internal buffer keyed by ImGui ID so selection state
// is preserved across frames.
// -----------------------------------------------------------------------------

void VideoScApp::TextCopyable(const char* label, const char* text) {
    ImGuiID id = ImGui::GetID(label);
    static std::unordered_map<ImGuiID, std::string> buffers;
    auto& buf = buffers[id];
    if (strcmp(buf.c_str(), text) != 0) {
        buf = text;
    }
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
    ImGui::PushItemWidth(-1);
    ImGui::InputText(label, buf.data(), (int)buf.size() + 1,
                     ImGuiInputTextFlags_ReadOnly);
    ImGui::PopItemWidth();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

// Convenience overload for std::string.
void VideoScApp::TextCopyable(const char* label, const std::string& text) {
    TextCopyable(label, text.c_str());
}

/**
 * @brief 在表格单元格内绘制单行文本，并在列宽不足时显示完整悬停提示。
 * @param text 要显示的 UTF-8 文本；空指针按空字符串处理。
 */
void VideoScApp::TableTextSingleLine(const char* text) {
    const char* value = text == nullptr ? "" : text;
    const bool clipped = ImGui::CalcTextSize(value).x > ImGui::GetContentRegionAvail().x;
    ImGui::TextUnformatted(value);
    if (clipped && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(value);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

/**
 * @brief `std::string` 版本的单行表格文本绘制。
 * @param text 要显示的 UTF-8 文本。
 */
void VideoScApp::TableTextSingleLine(const std::string& text) {
    TableTextSingleLine(text.c_str());
}

// -----------------------------------------------------------------------------
// File / folder picker dialogs
// -----------------------------------------------------------------------------

bool VideoScApp::OpenFileDialog(wchar_t* outBuf, size_t outBufLen,
                                const wchar_t* filter, bool isFolder) {
    if (isFolder) {
        IFileOpenDialog* pDlg = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&pDlg));
        if (FAILED(hr)) return false;
        pDlg->SetOptions(FOS_PICKFOLDERS);
        hr = pDlg->Show(nullptr);
        if (FAILED(hr)) { pDlg->Release(); return false; }
        IShellItem* pItem = nullptr;
        hr = pDlg->GetResult(&pItem);
        if (FAILED(hr)) { pDlg->Release(); return false; }
        PWSTR pPath = nullptr;
        hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &pPath);
        pItem->Release();
        pDlg->Release();
        if (FAILED(hr) || !pPath) return false;
        wcsncpy_s(outBuf, outBufLen, pPath, _TRUNCATE);
        CoTaskMemFree(pPath);
        return true;
    } else {
        OPENFILENAMEW ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.lpstrFile = outBuf;
        ofn.nMaxFile = (DWORD)outBufLen;
        outBuf[0] = 0;
        ofn.lpstrFilter = filter;
        ofn.nFilterIndex = 1;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        return GetOpenFileNameW(&ofn) == TRUE;
    }
}

// -----------------------------------------------------------------------------
// WIC image loading -> D3D11 texture
// -----------------------------------------------------------------------------

ID3D11ShaderResourceView* VideoScApp::LoadImageToTexture(const std::wstring& path,
                                                         int* outW,
                                                         int* outH,
                                                         const std::filesystem::path* scaled_output) {
    if (!m_d3dDevice) return nullptr;
    IWICImagingFactory* pFactory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&pFactory));
    if (FAILED(hr)) return nullptr;

    IWICBitmapDecoder* pDecoder = nullptr;
    hr = pFactory->CreateDecoderFromFilename(path.c_str(), nullptr,
                                             GENERIC_READ, WICDecodeMetadataCacheOnLoad, &pDecoder);
    if (FAILED(hr)) { pFactory->Release(); return nullptr; }

    IWICBitmapFrameDecode* pFrame = nullptr;
    hr = pDecoder->GetFrame(0, &pFrame);
    if (FAILED(hr)) { pDecoder->Release(); pFactory->Release(); return nullptr; }

    UINT sourceW = 0, sourceH = 0;
    pFrame->GetSize(&sourceW, &sourceH);
    if (sourceW == 0 || sourceH == 0) {
        pFrame->Release(); pDecoder->Release(); pFactory->Release();
        return nullptr;
    }

    UINT w = sourceW;
    UINT h = sourceH;
    IWICBitmapSource* bitmapSource = pFrame;
    IWICBitmapScaler* scaler = nullptr;
    const UINT maximumLongEdge = (std::max)(64U, m_config.thumbnails.image_preview_long_edge);
    if ((std::max)(sourceW, sourceH) > maximumLongEdge) {
        const double scale = static_cast<double>(maximumLongEdge) /
                             static_cast<double>((std::max)(sourceW, sourceH));
        w = (std::max)(1U, static_cast<UINT>(sourceW * scale));
        h = (std::max)(1U, static_cast<UINT>(sourceH * scale));
        hr = pFactory->CreateBitmapScaler(&scaler);
        if (FAILED(hr) || !scaler || FAILED(scaler->Initialize(pFrame, w, h, WICBitmapInterpolationModeFant))) {
            if (scaler) scaler->Release();
            pFrame->Release(); pDecoder->Release(); pFactory->Release();
            return nullptr;
        }
        bitmapSource = scaler;
    }

    IWICFormatConverter* pConv = nullptr;
    hr = pFactory->CreateFormatConverter(&pConv);
    if (FAILED(hr)) {
        if (scaler) scaler->Release();
        pFrame->Release(); pDecoder->Release(); pFactory->Release();
        return nullptr;
    }
    hr = pConv->Initialize(bitmapSource, GUID_WICPixelFormat32bppRGBA,
                           WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        pConv->Release();
        if (scaler) scaler->Release();
        pFrame->Release(); pDecoder->Release(); pFactory->Release();
        return nullptr;
    }

    const std::size_t pixelBytes = static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4ULL;
    if (pixelBytes > std::numeric_limits<UINT>::max()) {
        pConv->Release();
        if (scaler) scaler->Release();
        pFrame->Release(); pDecoder->Release(); pFactory->Release();
        return nullptr;
    }
    std::vector<uint8_t> pixels(pixelBytes);
    hr = pConv->CopyPixels(nullptr, w * 4, (UINT)pixels.size(), pixels.data());
    if (SUCCEEDED(hr) && scaled_output) {
        std::error_code fileError;
        std::filesystem::remove(*scaled_output, fileError);
        IWICStream* stream = nullptr;
        IWICBitmapEncoder* encoder = nullptr;
        IWICBitmapFrameEncode* encodedFrame = nullptr;
        IPropertyBag2* properties = nullptr;
        HRESULT encodeResult = pFactory->CreateStream(&stream);
        if (SUCCEEDED(encodeResult)) {
            encodeResult = stream->InitializeFromFilename(scaled_output->c_str(), GENERIC_WRITE);
        }
        if (SUCCEEDED(encodeResult)) {
            encodeResult = pFactory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
        }
        if (SUCCEEDED(encodeResult)) encodeResult = encoder->Initialize(stream, WICBitmapEncoderNoCache);
        if (SUCCEEDED(encodeResult)) encodeResult = encoder->CreateNewFrame(&encodedFrame, &properties);
        if (SUCCEEDED(encodeResult)) encodeResult = encodedFrame->Initialize(properties);
        if (SUCCEEDED(encodeResult)) encodeResult = encodedFrame->SetSize(w, h);
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppRGBA;
        if (SUCCEEDED(encodeResult)) encodeResult = encodedFrame->SetPixelFormat(&format);
        if (SUCCEEDED(encodeResult) && format != GUID_WICPixelFormat32bppRGBA) {
            encodeResult = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
        }
        if (SUCCEEDED(encodeResult)) {
            encodeResult = encodedFrame->WritePixels(h, w * 4, static_cast<UINT>(pixels.size()), pixels.data());
        }
        if (SUCCEEDED(encodeResult)) encodeResult = encodedFrame->Commit();
        if (SUCCEEDED(encodeResult)) encodeResult = encoder->Commit();
        if (properties) properties->Release();
        if (encodedFrame) encodedFrame->Release();
        if (encoder) encoder->Release();
        if (stream) stream->Release();
        if (FAILED(encodeResult)) hr = encodeResult;
    }
    pConv->Release();
    if (scaler) scaler->Release();
    pFrame->Release();
    pDecoder->Release();
    pFactory->Release();
    if (FAILED(hr)) return nullptr;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width            = w;
    desc.Height           = h;
    desc.MipLevels        = 1;
    desc.ArraySize        = 1;
    desc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage            = D3D11_USAGE_DEFAULT;
    desc.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem          = pixels.data();
    initData.SysMemPitch      = w * 4;
    initData.SysMemSlicePitch = (UINT)pixels.size();

    ID3D11Texture2D* pTex = nullptr;
    hr = m_d3dDevice->CreateTexture2D(&desc, &initData, &pTex);
    if (FAILED(hr)) return nullptr;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format                    = desc.Format;
    srvDesc.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels       = 1;
    srvDesc.Texture2D.MostDetailedMip = 0;

    ID3D11ShaderResourceView* pSRV = nullptr;
    hr = m_d3dDevice->CreateShaderResourceView(pTex, &srvDesc, &pSRV);
    pTex->Release();
    if (FAILED(hr)) return nullptr;

    *outW = (int)w;
    *outH = (int)h;
    return pSRV;
}

// -----------------------------------------------------------------------------
// RenderDockSpace: full-window dockspace host + menu bar + DockBuilder layout.
// -----------------------------------------------------------------------------

/** @brief 绘制主 DockSpace，并在首次启动或用户重置时构建默认布局。 */
void VideoScApp::RenderDockSpace() {
    // ------------- Full-window Dockspace -------------
    // PassthruCentralNode lets the host window background show through when
    // no window is docked in the central node. AutoHideTabBar hides tab bars
    // on nodes with a single window for a cleaner look.
    const ImGuiDockNodeFlags dockFlags =
        ImGuiDockNodeFlags_PassthruCentralNode | ImGuiDockNodeFlags_AutoHideTabBar;
    const ImGuiWindowFlags   hostFlags =
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_MenuBar;

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    ImGui::Begin("##DockSpaceWindow", nullptr, hostFlags);
    ImGui::PopStyleVar(3);

    // Menu bar: view toggles
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("视图")) {
            ImGui::MenuItem("扫描任务", nullptr, &m_showScanWindow);
            ImGui::MenuItem("扫描路径", nullptr, &m_showScanPathsWindow);
            ImGui::MenuItem("重复报告控制", nullptr, &m_showDuplicateReportWindow);
            ImGui::MenuItem("重复组浏览", nullptr, &m_showDuplicateGroupBrowserWindow);
            ImGui::MenuItem("运行日志", nullptr, &m_showLogWindow);
            ImGui::MenuItem("设置", nullptr, &m_showSettingsWindow);
            ImGui::Separator();
            if (ImGui::BeginMenu("诊断工具")) {
                ImGui::MenuItem("视频截图", nullptr, &m_showScreenshotWindow);
                ImGui::MenuItem("单文件诊断", nullptr, &m_showResultsWindow);
                ImGui::MenuItem("磁盘信息", nullptr, &m_showDiskInfoWindow);
                ImGui::MenuItem("文件检索", nullptr, &m_showFileSearchWindow);
                ImGui::Separator();
                if (ImGui::MenuItem("全部显示")) {
                    m_showScreenshotWindow = true;
                    m_showResultsWindow = true;
                    m_showDiskInfoWindow = true;
                    m_showFileSearchWindow = true;
                }
                if (ImGui::MenuItem("全部关闭")) {
                    m_showScreenshotWindow = false;
                    m_showResultsWindow = false;
                    m_showDiskInfoWindow = false;
                    m_showFileSearchWindow = false;
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    ImGuiID dockspaceId = ImGui::GetID("MainDockSpace");
    const bool resetLayout = m_resetDockLayoutRequested;
    if (resetLayout) {
        m_resetDockLayoutRequested = false;
        ImGui::ClearIniSettings();
        ImGui::DockBuilderRemoveNode(dockspaceId);
        m_showScanWindow = true;
        m_showSettingsWindow = true;
        m_showScanPathsWindow = true;
        m_showDuplicateReportWindow = true;
        m_showDuplicateGroupBrowserWindow = true;
        m_showLogWindow = true;
        m_showScreenshotWindow = false;
        m_showResultsWindow = false;
        m_showDiskInfoWindow = false;
        m_showFileSearchWindow = false;
        m_selectedReportGroupId.reset();
        m_selectedReportGroupOrdinal.reset();
        m_selectedReportGroup.reset();
        m_showReportDetailWindow = false;
        m_clearReportThumbnailsRequested = true;
    }
    if (ImGui::DockBuilderGetNode(dockspaceId) == nullptr) {
        // 首次启动或主动重置时构建“桌面工作区 + 底部日志”布局。
        ImGui::DockBuilderRemoveNode(dockspaceId);
        ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspaceId, vp->WorkSize);

        // 日志横跨底部，主工作区仍保持左侧辅助区和右侧报告区。
        ImGuiID dockWorkspace, dockLog;
        ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Down, 0.22f, &dockLog, &dockWorkspace);

        ImGuiID dockLeft, dockRight;
        ImGui::DockBuilderSplitNode(dockWorkspace, ImGuiDir_Left, 0.30f, &dockLeft, &dockRight);

        ImGuiID dockLeftTop, dockLeftBot;
        ImGui::DockBuilderSplitNode(dockLeft, ImGuiDir_Up, 0.55f, &dockLeftTop, &dockLeftBot);

        ImGuiID dockReportControls, dockReportBrowser;
        ImGui::DockBuilderSplitNode(
            dockRight, ImGuiDir_Left, 0.38f, &dockReportControls, &dockReportBrowser);

        ImGui::DockBuilderDockWindow("设置", dockLeftTop);
        ImGui::DockBuilderDockWindow("扫描路径", dockLeftTop);
        ImGui::DockBuilderDockWindow("扫描任务", dockLeftBot);
        ImGui::DockBuilderDockWindow("重复报告", dockReportControls);
        ImGui::DockBuilderDockWindow("重复组浏览", dockReportBrowser);
        ImGui::DockBuilderDockWindow("运行日志", dockLog);
        ImGui::DockBuilderFinish(dockspaceId);

        const char* layoutPath = ImGui::GetIO().IniFilename;
        if (layoutPath != nullptr && layoutPath[0] != '\0') {
            ImGui::SaveIniSettingsToDisk(layoutPath);
            if (resetLayout) {
                m_configMessage = "界面布局已恢复为初始状态：" + std::string(layoutPath);
                m_configMessageIsError = false;
            }
        } else if (resetLayout) {
            m_configMessage = "界面布局已重置，但布局文件路径不可用。";
            m_configMessageIsError = true;
        }
    }

    // Reserve space for the menu bar so the dockspace fills the remaining
    // viewport height (MenuBar height is known after BeginMenuBar above).
    float menuBarHeight = ImGui::GetFrameHeight();
    ImGui::DockSpace(dockspaceId,
                     ImVec2(0.0f, vp->WorkSize.y - menuBarHeight),
                     dockFlags);
    ImGui::End();
}

/** @brief 从进程实时日志源增量更新 GUI 本地有界缓存。 */
void VideoScApp::RefreshRuntimeLogs() {
    if (m_runtimeLogPaused) return;
    const videosc::dedup::RuntimeLogSnapshot snapshot =
        videosc::dedup::RuntimeLogFeed::SnapshotSince(m_runtimeLogCursor);
    if (snapshot.latest_sequence == 0 && m_runtimeLogCursor != 0) return;

    bool changed = false;
    for (const videosc::dedup::RuntimeLogEntry& entry : snapshot.entries) {
        const auto existing = m_runtimeLogEntryIndices.find(entry.entry_id);
        if (existing == m_runtimeLogEntryIndices.end()) {
            m_runtimeLogEntryIndices.emplace(entry.entry_id, m_runtimeLogEntries.size());
            m_runtimeLogEntries.push_back(entry);
        } else {
            m_runtimeLogEntries[existing->second] = entry;
        }
        changed = true;
    }
    m_runtimeLogCursor = (std::max)(m_runtimeLogCursor, snapshot.latest_sequence);
    m_runtimeLogDroppedEntries = snapshot.dropped_entries;

    if (m_runtimeLogEntries.size() > videosc::dedup::RuntimeLogFeed::Capacity()) {
        const std::size_t removeCount =
            m_runtimeLogEntries.size() - videosc::dedup::RuntimeLogFeed::Capacity();
        m_runtimeLogEntries.erase(
            m_runtimeLogEntries.begin(),
            m_runtimeLogEntries.begin() + static_cast<std::ptrdiff_t>(removeCount));
        m_runtimeLogEntryIndices.clear();
        for (std::size_t index = 0; index < m_runtimeLogEntries.size(); ++index) {
            m_runtimeLogEntryIndices.emplace(m_runtimeLogEntries[index].entry_id, index);
        }
        if (m_selectedRuntimeLogEntryId.has_value() &&
            m_runtimeLogEntryIndices.find(*m_selectedRuntimeLogEntryId) ==
                m_runtimeLogEntryIndices.end()) {
            m_selectedRuntimeLogEntryId.reset();
        }
    }
    m_runtimeLogHasUpdates = m_runtimeLogHasUpdates || changed;
}

/** @brief 绘制底部可停靠运行日志窗口。 */
void VideoScApp::RenderLogWindow() {
    RefreshRuntimeLogs();
    if (!m_showLogWindow) return;
    if (!ImGui::Begin("运行日志", &m_showLogWindow, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    ImGui::Checkbox("信息", &m_runtimeLogShowInfo);
    ImGui::SameLine();
    ImGui::Checkbox("警告", &m_runtimeLogShowWarning);
    ImGui::SameLine();
    ImGui::Checkbox("错误", &m_runtimeLogShowError);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150.0f);
    if (ImGui::BeginCombo("##runtime_log_task_filter",
                          RuntimeLogTaskFilterName(m_runtimeLogTaskFilter))) {
        for (int value = static_cast<int>(RuntimeLogTaskFilter::All);
             value <= static_cast<int>(RuntimeLogTaskFilter::Application);
             ++value) {
            const auto filter = static_cast<RuntimeLogTaskFilter>(value);
            const bool selected = filter == m_runtimeLogTaskFilter;
            if (ImGui::Selectable(RuntimeLogTaskFilterName(filter), selected)) {
                m_runtimeLogTaskFilter = filter;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth((std::max)(220.0f, ImGui::GetContentRegionAvail().x));
    ImGui::InputTextWithHint("##runtime_log_search",
                             "搜索任务、阶段、路径、SHA 或说明",
                             m_runtimeLogSearchBuf,
                             sizeof(m_runtimeLogSearchBuf));

    if (ImGui::Button(m_runtimeLogPaused ? "继续刷新" : "暂停刷新")) {
        m_runtimeLogPaused = !m_runtimeLogPaused;
    }
    ImGui::SameLine();
    ImGui::Checkbox("自动滚动", &m_runtimeLogAutoScroll);
    ImGui::SameLine();
    if (ImGui::Button("清空当前视图")) {
        m_runtimeLogEntries.clear();
        m_runtimeLogEntryIndices.clear();
        m_selectedRuntimeLogEntryId.reset();
        m_runtimeLogHasUpdates = false;
    }

    std::vector<std::size_t> filteredIndices;
    filteredIndices.reserve(m_runtimeLogEntries.size());
    const std::string query(m_runtimeLogSearchBuf);
    for (std::size_t index = 0; index < m_runtimeLogEntries.size(); ++index) {
        const auto& entry = m_runtimeLogEntries[index];
        const bool severityVisible =
            (entry.severity == videosc::dedup::RuntimeLogSeverity::Info && m_runtimeLogShowInfo) ||
            (entry.severity == videosc::dedup::RuntimeLogSeverity::Warning &&
             m_runtimeLogShowWarning) ||
            (entry.severity == videosc::dedup::RuntimeLogSeverity::Error && m_runtimeLogShowError);
        if (!severityVisible || !MatchesRuntimeLogTask(entry, m_runtimeLogTaskFilter)) continue;
        const bool queryMatches = query.empty() || ContainsRuntimeLogQuery(entry.task, query) ||
                                  ContainsRuntimeLogQuery(entry.stage, query) ||
                                  ContainsRuntimeLogQuery(entry.operation, query) ||
                                  ContainsRuntimeLogQuery(entry.subject, query) ||
                                  ContainsRuntimeLogQuery(entry.message, query);
        if (queryMatches) filteredIndices.push_back(index);
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(!m_selectedRuntimeLogEntryId.has_value());
    if (ImGui::Button("复制选中")) {
        const auto selected = m_runtimeLogEntryIndices.find(*m_selectedRuntimeLogEntryId);
        if (selected != m_runtimeLogEntryIndices.end()) {
            ImGui::SetClipboardText(
                FormatRuntimeLogLine(m_runtimeLogEntries[selected->second]).c_str());
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("复制筛选结果")) {
        std::string text;
        for (const std::size_t index : filteredIndices) {
            if (!text.empty()) text.push_back('\n');
            text += FormatRuntimeLogLine(m_runtimeLogEntries[index]);
        }
        ImGui::SetClipboardText(text.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("打开执行日志目录") && !m_config.logging.execution_directory.empty()) {
        ShellExecuteW(nullptr,
                      L"explore",
                      m_config.logging.execution_directory.c_str(),
                      nullptr,
                      nullptr,
                      SW_SHOWNORMAL);
    }
    if (m_config.logging.directory != m_config.logging.execution_directory) {
        ImGui::SameLine();
        if (ImGui::Button("打开扫描/应用日志目录") && !m_config.logging.directory.empty()) {
            ShellExecuteW(nullptr,
                          L"explore",
                          m_config.logging.directory.c_str(),
                          nullptr,
                          nullptr,
                          SW_SHOWNORMAL);
        }
    }

    ImGui::TextDisabled("显示 %llu/%llu 条 | 实时源已淘汰旧条目 %llu 条%s",
                        static_cast<unsigned long long>(filteredIndices.size()),
                        static_cast<unsigned long long>(m_runtimeLogEntries.size()),
                        static_cast<unsigned long long>(m_runtimeLogDroppedEntries),
                        m_runtimeLogPaused ? " | 已暂停" : "");

    const ImGuiTableFlags tableFlags = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
                                       ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable |
                                       ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY |
                                       ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##runtime_log_table", 7, tableFlags, ImVec2(0.0f, 0.0f))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("时间", ImGuiTableColumnFlags_WidthFixed, 165.0f);
        ImGui::TableSetupColumn("级别", ImGuiTableColumnFlags_WidthFixed, 58.0f);
        ImGui::TableSetupColumn("任务 / ID", ImGuiTableColumnFlags_WidthFixed, 145.0f);
        ImGui::TableSetupColumn("阶段 / 操作", ImGuiTableColumnFlags_WidthFixed, 205.0f);
        ImGui::TableSetupColumn("代码", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("路径 / 内容", ImGuiTableColumnFlags_WidthStretch, 0.8f);
        ImGui::TableSetupColumn("说明", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableHeadersRow();

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(filteredIndices.size()));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const auto& entry = m_runtimeLogEntries[filteredIndices[static_cast<std::size_t>(row)]];
                ImGui::PushID(reinterpret_cast<void*>(static_cast<std::uintptr_t>(entry.entry_id)));
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                const std::string time = FormatUtcTimestamp(entry.utc_ms);
                const bool selected = m_selectedRuntimeLogEntryId == entry.entry_id;
                if (ImGui::Selectable(time.c_str(),
                                      selected,
                                      ImGuiSelectableFlags_SpanAllColumns |
                                          ImGuiSelectableFlags_AllowOverlap)) {
                    m_selectedRuntimeLogEntryId = entry.entry_id;
                }

                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(RuntimeLogSeverityColor(entry.severity),
                                   "%s",
                                   RuntimeLogSeverityName(entry.severity));

                ImGui::TableSetColumnIndex(2);
                std::string task = entry.task.empty() ? "-" : entry.task;
                if (entry.task_id != 0) task += " #" + std::to_string(entry.task_id);
                TableTextSingleLine(task);

                ImGui::TableSetColumnIndex(3);
                std::string stage = entry.stage;
                if (!entry.operation.empty()) {
                    if (!stage.empty()) stage += " / ";
                    stage += entry.operation;
                }
                TableTextSingleLine(stage.empty() ? "-" : stage);

                ImGui::TableSetColumnIndex(4);
                std::string code = "-";
                if (entry.status_code != 0 || entry.native_error != 0) {
                    code = std::to_string(entry.status_code) + " / " +
                           std::to_string(entry.native_error);
                }
                TableTextSingleLine(code);

                ImGui::TableSetColumnIndex(5);
                TableTextSingleLine(entry.subject.empty() ? "-" : entry.subject);

                ImGui::TableSetColumnIndex(6);
                std::string message = entry.message.empty() ? "-" : entry.message;
                if (entry.repeat_count > 1) {
                    message += "（重复 " + std::to_string(entry.repeat_count) + " 次）";
                }
                TableTextSingleLine(message);
                ImGui::PopID();
            }
        }

        if (m_runtimeLogAutoScroll && m_runtimeLogHasUpdates && m_runtimeLogWasAtBottom) {
            ImGui::SetScrollY(ImGui::GetScrollMaxY());
        }
        m_runtimeLogWasAtBottom = ImGui::GetScrollMaxY() <= 0.0f ||
                                  ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2.0f;
        ImGui::EndTable();
    }
    m_runtimeLogHasUpdates = false;
    ImGui::End();
}

// -----------------------------------------------------------------------------
// RenderScreenshotWindow: 视频截图
// -----------------------------------------------------------------------------

void VideoScApp::RenderScreenshotWindow() {
    if (!m_showScreenshotWindow) return;

    // ------------- 视频截图窗口 -------------
    if (!ImGui::Begin("视频截图",
                      &m_showScreenshotWindow,
                      ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGui::End();
        return;
    }

    ImGui::Text("视频路径");
    ImGui::PushItemWidth(-120);
    ImGui::InputText("##vpath", m_videoPathBuf, sizeof(m_videoPathBuf));
    ImGui::PopItemWidth();
    ImGui::SameLine();
    PushButtonStyleNeutral();
    if (ImGui::Button("浏览...##vpath")) {
        wchar_t buf[1024] = {};
        if (OpenFileDialog(buf, 1024,
                L"视频文件\0*.mp4;*.mkv;*.avi;*.mov;*.flv;*.webm\0所有文件\0*.*\0",
                false)) {
            std::string utf8 = WideToUtf8(buf);
            strncpy_s(m_videoPathBuf, utf8.c_str(), _TRUNCATE);
        }
    }
    ImGui::PopStyleColor(3);

    ImGui::Text("输出目录");
    ImGui::PushItemWidth(-120);
    ImGui::InputText("##odir", m_outputDirBuf, sizeof(m_outputDirBuf));
    ImGui::PopItemWidth();
    ImGui::SameLine();
    PushButtonStyleNeutral();
    if (ImGui::Button("浏览...##odir")) {
        wchar_t buf[1024] = {};
        if (OpenFileDialog(buf, 1024, nullptr, true)) {
            std::string utf8 = WideToUtf8(buf);
            strncpy_s(m_outputDirBuf, utf8.c_str(), _TRUNCATE);
        }
    }
    ImGui::PopStyleColor(3);

    ImGui::Separator();

    PushButtonStylePrimary();
    if (ImGui::Button("开始截图", ImVec2(200, 36))) {
        m_runCapture = true;
    }
    ImGui::PopStyleColor(3);

    ImGui::SameLine();
    if (m_result.statusCode == VIDEOSC_OK) {
        ImGui::TextColored(ImVec4(0.65f, 0.89f, 0.63f, 1.0f), "成功");
    } else if (m_result.statusCode == -1) {
        ImGui::TextDisabled("尚未运行");
    } else {
        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.66f, 1.0f), "失败 (代码=%d)", m_result.statusCode);
    }

    if (m_result.statusCode == VIDEOSC_OK && m_result.duration > 0) {
        ImGui::Text("视频时长: %.3f 秒", m_result.duration);
        ImGui::Text("已生成: %d / %d (配置: %d 张, 长边 %d 像素)",
            m_result.thumbnailCount, m_screenshotCount, m_screenshotCount, m_maxLongEdge);
    }

    if (m_result.errorMessage && m_result.statusCode != VIDEOSC_OK) {
        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.66f, 1.0f), "错误: %s", m_result.errorMessage);
    }

    ImGui::Separator();
    ImGui::TextDisabled("提示: 截图数量/长边上限/文件名模式等参数请在「设置」窗口中配置");

    ImGui::End();
}

// -----------------------------------------------------------------------------
// RenderScanWindow: 扫描任务与多物理盘实时进度
// -----------------------------------------------------------------------------

void VideoScApp::RenderScanWindow() {
    if (!m_showScanWindow) return;
    if (!ImGui::Begin("扫描任务", &m_showScanWindow, ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGui::End();
        return;
    }

    const bool scanRunning = m_scanCoordinator && m_scanCoordinator->is_running();
    ImGui::BeginDisabled(scanRunning || m_reportRunning.load() ||
                         m_reportCleanupRunning.load() || m_deletionRunning.load() ||
                         m_selectionRunning.load());
    PushButtonStylePrimary();
    if (ImGui::Button("开始扫描", ImVec2(160, 32))) m_startScanRequested = true;
    ImGui::PopStyleColor(3);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!scanRunning || m_scanSnapshot.cancellation_requested);
    PushButtonStyleDanger();
    if (ImGui::Button("取消扫描", ImVec2(110, 32))) m_cancelScanRequested = true;
    ImGui::PopStyleColor(3);
    ImGui::EndDisabled();

    ImGui::BeginDisabled(scanRunning);
    ImGui::Checkbox("同步后生成视觉相似报告", &m_generateSimilarAfterScan);
    ImGui::EndDisabled();
    ImGui::TextDisabled("默认关闭；启动扫描后冻结本次选择，先生成 SHA-512 精确报告。" );

    if (m_runtimeStore && m_runtimeStore->is_open()) {
        ImGui::SameLine();
        PushButtonStyleNeutral();
        if (ImGui::Button("刷新断点")) RefreshResumableScans();
        ImGui::PopStyleColor(3);
    }

    if (!m_resumableScans.empty()) {
        std::string previewStorage = m_selectedResumableScan >= 0
                                         ? std::to_string(m_resumableScans[m_selectedResumableScan].scan_id)
                                         : "请选择断点";
        ImGui::SetNextItemWidth(250);
        if (ImGui::BeginCombo("可恢复任务", previewStorage.c_str())) {
            for (std::size_t index = 0; index < m_resumableScans.size(); ++index) {
                const auto& checkpoint = m_resumableScans[index];
                const std::string label = std::to_string(checkpoint.scan_id) + " | " +
                                          ScanPhaseName(checkpoint.phase) + " | 已完成 " +
                                          std::to_string(checkpoint.completed_files);
                const bool selected = m_selectedResumableScan == static_cast<int>(index);
                if (ImGui::Selectable(label.c_str(), selected)) m_selectedResumableScan = static_cast<int>(index);
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(scanRunning || m_selectedResumableScan < 0);
        if (ImGui::Button("从断点恢复")) m_resumeScanRequested = true;
        ImGui::EndDisabled();
    }

    ImGui::Separator();

    const ImVec4 okColor(0.65f, 0.89f, 0.63f, 1.0f);
    const ImVec4 warnColor(0.98f, 0.70f, 0.53f, 1.0f);
    const ImVec4 errorColor(0.95f, 0.55f, 0.66f, 1.0f);
    const ImVec4 activeProgressColor(0.35f, 0.68f, 0.94f, 1.0f);

    // 每个扫描任务和每个活动阶段都从零开始记录可确定进度，终态则保留最后现场。
    if (m_scanSnapshot.scan_id != m_lastScanProgressId) {
        m_lastScanProgressId = m_scanSnapshot.scan_id;
        m_lastScanProgressFraction = 0.0f;
        m_lastScanProgressPhase = videosc::dedup::ScanPhase::Idle;
    }
    const videosc::dedup::ScanPhase phase = m_scanSnapshot.phase;
    const bool activePhase = phase == videosc::dedup::ScanPhase::Discovering ||
                             phase == videosc::dedup::ScanPhase::Planning ||
                             phase == videosc::dedup::ScanPhase::Hashing ||
                             phase == videosc::dedup::ScanPhase::ExtractingMedia ||
                             phase == videosc::dedup::ScanPhase::Syncing ||
                             phase == videosc::dedup::ScanPhase::FlushingSyncTail;
    if (activePhase && phase != m_lastScanProgressPhase) {
        m_lastScanProgressFraction = 0.0f;
        m_lastScanProgressPhase = phase;
    }

    const int stageNumber = ScanStageNumber(phase);
    if (stageNumber > 0) {
        ImGui::Text("当前阶段：%d/4  %s", stageNumber, ScanPhaseName(phase));
    } else {
        ImGui::Text("当前状态：%s", ScanPhaseName(phase));
    }
    if (m_scanSnapshot.scan_id != 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("任务 ID: %llu", static_cast<unsigned long long>(m_scanSnapshot.scan_id));
    }
    if (m_scanSnapshot.cancellation_requested && scanRunning) {
        ImGui::TextColored(warnColor, "正在取消：等待当前文件操作安全退出……");
    }

    bool indeterminateProgress = false;
    float progressFraction = 0.0f;
    std::string progressOverlay;
    ImVec4 progressColor = activeProgressColor;
    switch (phase) {
        case videosc::dedup::ScanPhase::Discovering:
            indeterminateProgress = true;
            progressOverlay = "正在发现文件，已发现 " + std::to_string(m_scanSnapshot.discovered_files) + " 个";
            break;
        case videosc::dedup::ScanPhase::Planning:
            indeterminateProgress = true;
            progressOverlay = m_scanSnapshot.mysql_planning_degraded
                                  ? "MySQL 不可用，正在仅按本地数据规划"
                                  : "正在联合 RocksDB 与 MySQL 规划缺失任务";
            break;
        case videosc::dedup::ScanPhase::Hashing:
            progressFraction = CalculateStageProgress(m_scanSnapshot.hash_processed_files,
                                                      m_scanSnapshot.hash_total_files);
            progressOverlay = m_scanSnapshot.hash_total_files == 0
                                  ? "所有文件均已复用，无需重新计算 SHA-512"
                                  : FormatStageProgress(m_scanSnapshot.hash_processed_files,
                                                        m_scanSnapshot.hash_total_files);
            m_lastScanProgressFraction = progressFraction;
            break;
        case videosc::dedup::ScanPhase::ExtractingMedia:
            if (!m_scanSnapshot.media_total_known) {
                indeterminateProgress = true;
                progressOverlay = "正在统计待处理媒体文件";
            } else {
                progressFraction = CalculateStageProgress(m_scanSnapshot.media_processed_files,
                                                          m_scanSnapshot.media_total_files);
                progressOverlay = m_scanSnapshot.media_total_files == 0
                                      ? "没有需要提取特征的媒体文件"
                                      : FormatStageProgress(m_scanSnapshot.media_processed_files,
                                                            m_scanSnapshot.media_total_files);
                m_lastScanProgressFraction = progressFraction;
            }
            break;
        case videosc::dedup::ScanPhase::Syncing:
            indeterminateProgress = true;
            progressOverlay = "正在同步 MySQL，已同步 " +
                              std::to_string(m_syncSnapshot.synchronized_operations) + " 条操作";
            break;
        case videosc::dedup::ScanPhase::FlushingSyncTail:
            indeterminateProgress = true;
            progressOverlay = "正在发布不足同步阈值的尾批";
            break;
        case videosc::dedup::ScanPhase::CompletedLocal:
            progressFraction = 1.0f;
            progressOverlay = "本地扫描完成";
            progressColor = okColor;
            m_lastScanProgressFraction = 1.0f;
            break;
        case videosc::dedup::ScanPhase::CompletedSynchronized:
            progressFraction = 1.0f;
            progressOverlay = "扫描与同步全部完成";
            progressColor = okColor;
            m_lastScanProgressFraction = 1.0f;
            break;
        case videosc::dedup::ScanPhase::Cancelled:
            progressFraction = (std::min)(m_lastScanProgressFraction, 0.99f);
            progressOverlay = "扫描已取消";
            progressColor = warnColor;
            break;
        case videosc::dedup::ScanPhase::Failed:
            progressFraction = (std::min)(m_lastScanProgressFraction, 0.99f);
            progressOverlay = "扫描失败，请查看下方错误信息";
            progressColor = errorColor;
            break;
        case videosc::dedup::ScanPhase::Paused:
            progressFraction = (std::min)(m_lastScanProgressFraction, 0.99f);
            progressOverlay = "扫描已暂停";
            progressColor = warnColor;
            break;
        case videosc::dedup::ScanPhase::Interrupted:
            progressFraction = (std::min)(m_lastScanProgressFraction, 0.99f);
            progressOverlay = "扫描已中断，可从断点恢复";
            progressColor = warnColor;
            break;
        case videosc::dedup::ScanPhase::Idle:
            progressOverlay = "等待开始扫描";
            break;
        default:
            indeterminateProgress = true;
            progressOverlay = ScanPhaseName(phase);
            break;
    }
    if (m_scanSnapshot.cancellation_requested && scanRunning) {
        indeterminateProgress = false;
        progressFraction = (std::min)(m_lastScanProgressFraction, 0.99f);
        progressOverlay = "正在取消扫描";
        progressColor = warnColor;
    }
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, progressColor);
    const float renderedProgress = indeterminateProgress
                                       ? -static_cast<float>(ImGui::GetTime())
                                       : progressFraction;
    DrawContrastProgressBar(renderedProgress,
                            ImVec2(ImGui::GetContentRegionAvail().x, 0.0f),
                            progressOverlay.c_str());
    ImGui::PopStyleColor();

    ImGui::Text("发现 %llu | SHA-512 完成 %llu | 媒体特征完成 %llu | 图片特征失败 %llu | 失败/跳过 %llu",
                static_cast<unsigned long long>(m_scanSnapshot.discovered_files),
                static_cast<unsigned long long>(m_scanSnapshot.hashed_files),
                static_cast<unsigned long long>(m_scanSnapshot.media_processed_files),
                static_cast<unsigned long long>(m_scanSnapshot.image_feature_failed_contents),
                static_cast<unsigned long long>(m_scanSnapshot.failed_files));
    ImGui::Text("已读取：%s | 计算并发：%u / %u / 上限 %u | 系统 CPU：%.1f%% | 活动读取：%u",
                FormatBytes(m_scanSnapshot.bytes_read).c_str(),
                m_scanSnapshot.active_compute_threads,
                m_scanSnapshot.allowed_compute_threads,
                m_scanSnapshot.configured_compute_threads,
                m_scanSnapshot.system_cpu_percent,
                m_scanSnapshot.active_file_reads);

    ImGui::TextColored(m_scanSnapshot.local_scan_complete ? okColor : warnColor,
                       "本地 RocksDB：%s",
                       m_scanSnapshot.local_scan_complete ? "扫描完成" : "处理中");
    ImGui::SameLine();
    ImGui::TextColored(m_syncSnapshot.connected ? okColor : warnColor,
                       "MySQL：%s",
                       m_syncSnapshot.connected ? "已连接" : "未连接/重试中");
    ImGui::SameLine();
    ImGui::TextColored(m_scanSnapshot.shared_sync_complete ? okColor : warnColor,
                       "共享同步：%s",
                       m_scanSnapshot.shared_sync_complete ? "完成" : "仍有待同步操作");
    ImGui::TextDisabled("本轮暂存：%llu；本轮正式待同步：%llu；累计已同步：%llu；最近批次：%llu",
                        static_cast<unsigned long long>(m_scanSnapshot.mysql_staged_operations),
                        static_cast<unsigned long long>(m_scanSnapshot.mysql_pending_operations),
                        static_cast<unsigned long long>(m_syncSnapshot.synchronized_operations),
                        static_cast<unsigned long long>(m_syncSnapshot.last_batch_size));
    if (m_scanSnapshot.mysql_planning_degraded) {
        ImGui::TextColored(warnColor, "扫描规划已降级：MySQL 不可用，本轮仅依据本地数据排除。" );
    }
    if (m_scanSnapshot.image_feature_failed_contents != 0) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(
            warnColor,
            "本轮扫描阶段有 %llu 个唯一图片内容未生成完整三级特征。生成视觉报告时会自动尝试全部活动路径；可在 execution-failures.log 中查找 操作=image_perceptual_features。",
            static_cast<unsigned long long>(m_scanSnapshot.image_feature_failed_contents));
        ImGui::PopTextWrapPos();
    }
    if (!m_syncSnapshot.last_error.empty()) {
        ImGui::TextColored(warnColor, "MySQL 最近错误：%s", m_syncSnapshot.last_error.c_str());
    }

    if (!m_scanSnapshot.discovery_roots.empty() &&
        ImGui::BeginTable("root_discovery_progress",
                          6,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollX | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("扫描路径", ImGuiTableColumnFlags_WidthStretch, 2.4f);
        ImGui::TableSetupColumn("发现方式", ImGuiTableColumnFlags_WidthFixed, 145.0f);
        ImGui::TableSetupColumn("当前阶段", ImGuiTableColumnFlags_WidthFixed, 145.0f);
        ImGui::TableSetupColumn("已发现", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("耗时", ImGuiTableColumnFlags_WidthFixed, 75.0f);
        ImGui::TableSetupColumn("回退/说明", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableHeadersRow();
        for (const auto& root : m_scanSnapshot.discovery_roots) {
            const std::string rootPath = WideToUtf8(root.root_path.c_str());
            const std::string elapsed = FormatElapsedMilliseconds(root.elapsed_milliseconds);
            const std::string reason = WideToUtf8(root.fallback_reason.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); TableTextSingleLine(rootPath);
            ImGui::TableNextColumn(); TableTextSingleLine(DiscoveryBackendName(root.backend));
            ImGui::TableNextColumn(); TableTextSingleLine(DiscoveryRootPhaseName(root.phase));
            ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(root.discovered_files));
            ImGui::TableNextColumn(); TableTextSingleLine(elapsed);
            ImGui::TableNextColumn(); TableTextSingleLine(reason.empty() ? "-" : reason.c_str());
        }
        ImGui::EndTable();
    }

    if (!m_scanSnapshot.disks.empty() &&
        ImGui::BeginTable("disk_scan_progress",
                          10,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollX | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("物理存储");
        ImGui::TableSetupColumn("介质");
        ImGui::TableSetupColumn("线程上限");
        ImGui::TableSetupColumn("当前许可");
        ImGui::TableSetupColumn("活动");
        ImGui::TableSetupColumn("磁盘占用");
        ImGui::TableSetupColumn("队列");
        ImGui::TableSetupColumn("读取量");
        ImGui::TableSetupColumn("不可读");
        ImGui::TableSetupColumn("超时");
        ImGui::TableHeadersRow();
        for (const auto& disk : m_scanSnapshot.disks) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); TableTextSingleLine(WideToUtf8(disk.storage_target_key.c_str()));
            ImGui::TableNextColumn(); TableTextSingleLine(WideToUtf8(disk.media_type.c_str()));
            ImGui::TableNextColumn(); ImGui::Text("%u", disk.configured_read_threads);
            ImGui::TableNextColumn(); ImGui::Text("%u", disk.allowed_read_threads);
            ImGui::TableNextColumn(); ImGui::Text("%u", disk.active_read_threads);
            ImGui::TableNextColumn();
            if (disk.read_utilization_available) ImGui::Text("%.1f%%", disk.read_utilization_percent);
            else ImGui::TextDisabled("不可用/固定回退");
            ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(disk.queued_files));
            ImGui::TableNextColumn(); TableTextSingleLine(FormatBytes(disk.bytes_read));
            ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(disk.unreadable_files));
            ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(disk.timeout_files));
        }
        ImGui::EndTable();
    }

    if (!m_scanMessage.empty()) {
        ImGui::Separator();
        ImGui::TextColored(m_scanMessageIsError ? ImVec4(0.95f, 0.55f, 0.66f, 1.0f) : okColor,
                           "%s",
                           m_scanMessage.c_str());
    }

    // Everything 报错或返回0结果等非致命警告：检测到新警告时请求弹窗
    if (!m_scanSnapshot.discovery_warning.empty() &&
        m_scanSnapshot.discovery_warning != m_shownDiscoveryWarning) {
        m_shownDiscoveryWarning = m_scanSnapshot.discovery_warning;
        m_discoveryWarningPopupPending = true;
    }
    if (m_discoveryWarningPopupPending) {
        ImGui::OpenPopup("文件发现警告");
    }
    if (ImGui::BeginPopupModal("文件发现警告", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const std::string utf8Warning = WideToUtf8(m_scanSnapshot.discovery_warning.c_str());
        ImGui::TextUnformatted(utf8Warning.c_str());
        ImGui::Separator();
        if (ImGui::Button("确定", ImVec2(120, 0))) {
            m_discoveryWarningPopupPending = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::End();
}

// -----------------------------------------------------------------------------
// RenderScanPathsWindow: 扫描路径与保留优先级（独立窗口）
// 拖动整行排序，右键弹出删除；底部按钮追加新路径。
// -----------------------------------------------------------------------------

void VideoScApp::RenderScanPathsWindow() {
    if (!m_showScanPathsWindow) return;
    if (!ImGui::Begin("扫描路径",
                      &m_showScanPathsWindow,
                      ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGui::End();
        return;
    }

    ImGui::TextDisabled("从上到下为保留路径优先级；运行中的任务使用创建时快照。");
    ImGui::TextDisabled("拖动整行可排序，右键单击路径可删除。");
    ImGui::Separator();

    int removeIndex = -1;
    for (int index = 0; index < static_cast<int>(m_scanPathEditors.size()); ++index) {
        ImGui::PushID(index);

        const std::string label =
            std::to_string(index + 1) + ".  " + m_scanPathEditors[index];
        ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_SpanAllColumns);

        // 右键删除：弹出上下文菜单绑定到上一个 item（本行）。
        if (ImGui::BeginPopupContextItem("##scan_path_ctx")) {
            if (ImGui::MenuItem("删除")) {
                removeIndex = index;
            }
            ImGui::EndPopup();
        }

        // 拖动排序：当本行被按住(active)且鼠标已移到相邻行(hovered 的是别的行)时交换。
        if (ImGui::IsItemActive() && !ImGui::IsItemHovered()) {
            const float dragDeltaY = ImGui::GetMouseDragDelta(0).y;
            int target = index + (dragDeltaY < 0.0f ? -1 : +1);
            if (target >= 0 && target < static_cast<int>(m_scanPathEditors.size())) {
                std::swap(m_scanPathEditors[index], m_scanPathEditors[target]);
                ImGui::ResetMouseDragDelta(0);
            }
        }

        ImGui::PopID();
    }
    if (removeIndex >= 0) {
        m_scanPathEditors.erase(m_scanPathEditors.begin() + removeIndex);
    }

    ImGui::Separator();
    PushButtonStyleSuccess();
    if (ImGui::Button("添加扫描路径", ImVec2(160, 30))) {
        wchar_t selectedPath[4096] = {};
        if (OpenFileDialog(selectedPath, std::size(selectedPath), nullptr, true)) {
            const std::wstring selected(selectedPath);
            const bool duplicate = std::any_of(
                m_scanPathEditors.begin(), m_scanPathEditors.end(), [&selected](const std::string& existing) {
                    const std::wstring wideExisting = Utf8ToWide(existing.c_str());
                    return _wcsicmp(wideExisting.c_str(), selected.c_str()) == 0;
                });
            if (!duplicate) {
                m_scanPathEditors.push_back(WideToUtf8(selected.c_str()));
            }
        }
    }
    ImGui::PopStyleColor(3);

    ImGui::End();
}

// -----------------------------------------------------------------------------
// RenderDuplicateReportWindow: 重复报告控制与安全删除
// -----------------------------------------------------------------------------

/**
 * @brief 切换一个成员的待删除状态，并同步当前组的保留成员。
 * @param group 当前报告页中可修改的重复组。
 * @param path_id 被点击成员的路径 ID。
 */
void VideoScApp::ToggleReportMemberDeletion(videosc::dedup::DuplicateGroup& group,
                                            const std::uint64_t path_id) {
    if (!m_runtimeStore || m_visibleReportGeneration == 0) return;
    UpdateConfigFromEditors();
    const std::string validationErrors = JoinValidationErrors(
        videosc::dedup::ConfigValidator::Validate(m_config));
    if (!validationErrors.empty()) {
        m_deletionMessage = validationErrors;
        m_deletionMessageIsError = true;
        return;
    }
    if (!m_retentionPolicies.any()) {
        m_deletionMessage = "至少需要启用一个保留策略。";
        m_deletionMessageIsError = true;
        return;
    }
    m_selectionCoversEntireReport = false;
    videosc::dedup::DuplicateGroup requested = group;
    const auto selected = std::find(requested.selected_for_deletion.begin(),
                                    requested.selected_for_deletion.end(),
                                    path_id);
    if (selected == requested.selected_for_deletion.end()) {
        if (requested.selected_for_deletion.size() + 1 >= requested.members.size()) {
            m_deletionMessage = "同一重复组不能把所有副本都选中。";
            m_deletionMessageIsError = true;
            return;
        }
        requested.selected_for_deletion.push_back(path_id);
    } else {
        requested.selected_for_deletion.erase(selected);
    }

    // 手动点击不受批量“指定磁盘”过滤影响；保留成员只能从用户未选中的副本中产生。
    videosc::dedup::DuplicateGroup retentionCandidates = requested;
    retentionCandidates.members.erase(
        std::remove_if(
            retentionCandidates.members.begin(),
            retentionCandidates.members.end(),
            [&](const videosc::dedup::DuplicateMember& member) {
                return std::find(requested.selected_for_deletion.begin(),
                                 requested.selected_for_deletion.end(),
                                 member.path_id) != requested.selected_for_deletion.end();
            }),
        retentionCandidates.members.end());
    if (retentionCandidates.members.empty()) {
        m_deletionMessage = "同一重复组必须至少保留一个未选中的副本。";
        m_deletionMessageIsError = true;
        return;
    }
    std::uint64_t retainedPathId = retentionCandidates.members.front().path_id;
    if (retentionCandidates.members.size() > 1) {
        const auto policyPlan = videosc::dedup::DeletionPlanner::SelectDetailed(
            retentionCandidates, m_retentionPolicies);
        if (!policyPlan.succeeded() || !policyPlan.plan->retained_path_id.has_value()) {
            m_deletionMessage = policyPlan.message;
            m_deletionMessageIsError = true;
            return;
        }
        retainedPathId = *policyPlan.plan->retained_path_id;
    }
    videosc::dedup::DuplicateGroup planned = requested;
    planned.retained_path_id = retainedPathId;
    planned.selected_for_deletion = requested.selected_for_deletion;
    const std::uint32_t imageMaximum = m_visibleSimilarReportMetadata.has_value()
                                           ? m_visibleSimilarReportMetadata->image_max_hamming_distance
                                           : m_config.dhash_similarity.image_max_hamming_distance;
    const std::uint32_t videoMaximum = m_visibleSimilarReportMetadata.has_value()
                                           ? m_visibleSimilarReportMetadata->video_max_average_hamming_distance
                                           : m_config.dhash_similarity.video_max_average_hamming_distance;
    const bool imageThreeStageVerified = m_visibleSimilarReportMetadata.has_value() &&
                                         m_visibleSimilarReportMetadata->image_uses_three_stage_verification;
    std::uint64_t rejected = 0;
    const auto members = BuildSafeSelection(
        group,
        planned,
        m_config.report_selection,
        imageMaximum,
        videoMaximum,
        imageThreeStageVerified,
        rejected);
    videosc::dedup::ReportSelectionStore selections(*m_runtimeStore);
    videosc::dedup::ReportSelectionSnapshot snapshot;
    const videosc::dedup::RocksStatus saved = selections.SetGroup(
        m_visibleReportKind,
        m_visibleReportGeneration,
        group.group_id,
        members,
        snapshot);
    if (!saved.succeeded) {
        m_deletionMessage = "保存成员选择失败：" + saved.message;
        m_deletionMessageIsError = true;
        return;
    }
    ApplySelectionToGroup(requested, members);
    group = requested;
    m_reportSelectionSnapshot = snapshot;
    m_deletionMessage = rejected == 0
                            ? "选择已保存到本地数据库。"
                            : "该候选未通过严格小于 dHash 安全距离上限，未选中。";
    m_deletionMessageIsError = rejected != 0;
    for (auto& cached : m_reportGroupCache) {
        if (cached.second.group.group_id != group.group_id || &cached.second.group == &group) continue;
        cached.second.group.selected_for_deletion = group.selected_for_deletion;
        cached.second.group.retained_path_id = group.retained_path_id;
    }
}

/**
 * @brief 绘制重复报告控制窗口，集中承载报告类型、任务、筛选、排序和删除操作。
 *
 * 窗口只维护共享报告状态，不再绘制重复组列表；列表由独立浏览窗口消费同一状态。
 */
void VideoScApp::RenderDuplicateReportWindow() {
    if (!m_showDuplicateReportWindow) return;
    ImGui::SetNextWindowSize(ImVec2(560.0f, 760.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("重复报告",
                      &m_showDuplicateReportWindow,
                      ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGui::End();
        return;
    }

    const bool busy = m_reportRunning.load() || m_reportCleanupRunning.load() ||
                      m_deletionRunning.load() || m_selectionRunning.load() ||
                      (m_scanCoordinator && m_scanCoordinator->is_running());
    // 窄停靠区域改用单列命令，避免桌面缩放或小屏幕下按钮文字互相挤压。
    const float controlWidth = ImGui::GetContentRegionAvail().x;
    const int primaryCommandColumns = controlWidth >= 480.0f ? 3 : 1;
    const int dangerCommandColumns = controlWidth >= 380.0f ? 2 : 1;

    ImGui::TextDisabled("当前报告类型");
    if (ImGui::BeginTabBar("report_kind_tabs")) {
        if (ImGui::BeginTabItem("SHA-512 精确重复")) {
            if (m_visibleReportKind != videosc::dedup::DuplicateReportKind::Exact) {
                m_visibleReportKind = videosc::dedup::DuplicateReportKind::Exact;
                m_selectionCoversEntireReport = false;
                m_loadReportRequested = true;
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("视觉三级相似")) {
            if (m_visibleReportKind != videosc::dedup::DuplicateReportKind::Similar) {
                m_visibleReportKind = videosc::dedup::DuplicateReportKind::Similar;
                m_selectionCoversEntireReport = false;
                m_loadReportRequested = true;
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::Separator();
    ImGui::TextUnformatted("报告任务");
    ImGui::BeginDisabled(busy);
    if (ImGui::BeginTable(
            "report_primary_commands", primaryCommandColumns, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();
        PushButtonStylePrimary();
        if (ImGui::Button("生成精确报告", ImVec2(ImGui::GetContentRegionAvail().x, 0.0f))) {
            m_generateExactReportRequested = true;
        }
        ImGui::PopStyleColor(3);
        ImGui::TableNextColumn();
        PushButtonStyleAccent();
        if (ImGui::Button("生成视觉报告", ImVec2(ImGui::GetContentRegionAvail().x, 0.0f))) {
            m_generateSimilarReportRequested = true;
        }
        ImGui::PopStyleColor(3);
        ImGui::TableNextColumn();
        PushButtonStyleNeutral();
        if (ImGui::Button("加载已发布报告", ImVec2(ImGui::GetContentRegionAvail().x, 0.0f))) {
            m_loadReportRequested = true;
        }
        ImGui::PopStyleColor(3);
        ImGui::EndTable();
    }
    ImGui::EndDisabled();

    ImGui::TextDisabled("危险操作");
    ImGui::BeginDisabled(busy);
    if (ImGui::BeginTable(
            "report_danger_commands", dangerCommandColumns, ImGuiTableFlags_SizingStretchSame)) {
        PushButtonStyleDanger();
        ImGui::TableNextColumn();
        if (ImGui::Button("删除精确报告", ImVec2(ImGui::GetContentRegionAvail().x, 0.0f))) {
            m_reportCleanupConfirmationKind = videosc::dedup::DuplicateReportKind::Exact;
            m_openReportCleanupConfirmation = true;
        }
        ImGui::TableNextColumn();
        if (ImGui::Button("删除视觉报告", ImVec2(ImGui::GetContentRegionAvail().x, 0.0f))) {
            m_reportCleanupConfirmationKind = videosc::dedup::DuplicateReportKind::Similar;
            m_openReportCleanupConfirmation = true;
        }
        ImGui::PopStyleColor(3);
        ImGui::EndTable();
    }
    ImGui::EndDisabled();

    if (m_reportRunning.load()) {
        const bool cancellationRequested = m_reportCancelRequested.load(std::memory_order_relaxed);
        ImGui::BeginDisabled(cancellationRequested);
        PushButtonStyleDanger();
        if (ImGui::Button("取消报告生成")) m_cancelReportRequested = true;
        ImGui::PopStyleColor(3);
        ImGui::EndDisabled();
        videosc::dedup::DuplicateReportProgress progress;
        videosc::dedup::ImageFeatureBackfillProgress backfillProgress;
        {
            std::lock_guard<std::mutex> lock(m_reportResultMutex);
            progress = m_reportProgress;
            backfillProgress = m_imageFeatureBackfillProgress;
        }
        if (m_visibleReportKind == videosc::dedup::DuplicateReportKind::Similar) {
            const bool parallelValidation = progress.stage == "validating_secondary_phash" ||
                                            progress.stage == "validating_image_structure";
            if (progress.stage_index != 0 && progress.stage_count != 0) {
                ImGui::Text("当前阶段：%u/%u  %s",
                            progress.stage_index,
                            progress.stage_count,
                            DHashReportStageName(progress.stage));
            } else {
                ImGui::Text("当前阶段：%s", DHashReportStageName(progress.stage));
            }

            const bool indeterminate = !progress.stage_total_known;
            float renderedProgress = 0.0f;
            std::string progressOverlay;
            if (cancellationRequested) {
                renderedProgress = indeterminate
                                       ? m_lastReportIndeterminateProgress
                                       : (std::min)(m_lastReportProgressFraction, 0.99f);
                progressOverlay = parallelValidation
                                      ? "正在取消，等待视觉校验线程安全退出……"
                                      : "正在取消，等待当前操作安全退出……";
            } else if (indeterminate) {
                renderedProgress = -static_cast<float>(ImGui::GetTime());
                m_lastReportIndeterminateProgress = renderedProgress;
                progressOverlay = progress.stage == "joining_active_paths"
                                      ? "已读取 " + std::to_string(progress.stage_processed) + " 条有效路径"
                                      : "已处理 " + std::to_string(progress.stage_processed) + " 条视觉内容";
            } else {
                renderedProgress = CalculateStageProgress(progress.stage_processed,
                                                          progress.stage_total);
                m_lastReportProgressFraction = renderedProgress;
                progressOverlay = progress.stage_total == 0
                                      ? "本阶段无需处理"
                                      : FormatStageProgress(progress.stage_processed,
                                                            progress.stage_total);
                if (parallelValidation &&
                    progress.stage_total != 0) {
                    progressOverlay +=
                        " · 活动线程 " +
                        std::to_string(progress.active_validation_threads) +
                        " / " +
                        std::to_string(progress.configured_validation_threads);
                }
            }

            const ImVec4 reportProgressColor = cancellationRequested
                                                   ? ImVec4(0.980f, 0.702f, 0.529f, 0.85f)
                                                   : ImVec4(0.541f, 0.706f, 0.980f, 0.85f);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, reportProgressColor);
            DrawContrastProgressBar(renderedProgress,
                                    ImVec2(ImGui::GetContentRegionAvail().x, 0.0f),
                                    progressOverlay.c_str());
            ImGui::PopStyleColor();
            ImGui::Text("视觉内容 %llu | 有效路径 %llu | 相似边 %llu | 已写入分组 %llu",
                        static_cast<unsigned long long>(progress.processed_contents),
                        static_cast<unsigned long long>(progress.processed_paths),
                        static_cast<unsigned long long>(progress.matched_pairs),
                        static_cast<unsigned long long>(progress.emitted_groups));
            if (progress.stage == "backfilling_image_features") {
                const std::uint64_t repaired =
                    backfillProgress.completed_images >= backfillProgress.initially_complete_images
                        ? backfillProgress.completed_images - backfillProgress.initially_complete_images
                        : 0;
                ImGui::Text(
                    "初始完整 %llu | 本轮修复 %llu | 失败 %llu（无可读路径 %llu / 超时 %llu / 解码或特征失败 %llu）",
                    static_cast<unsigned long long>(backfillProgress.initially_complete_images),
                    static_cast<unsigned long long>(repaired),
                    static_cast<unsigned long long>(backfillProgress.failed_images),
                    static_cast<unsigned long long>(backfillProgress.no_readable_path_images),
                    static_cast<unsigned long long>(backfillProgress.timeout_images),
                    static_cast<unsigned long long>(backfillProgress.decode_failed_images));
            }
            if (progress.candidate_pairs_total != 0 ||
                parallelValidation) {
                ImGui::Text(
                    "初筛候选对 %llu | 已二筛 %llu | 最终通过 %llu | 二/三筛拒绝 %llu | 当前线程 %u",
                    static_cast<unsigned long long>(progress.candidate_pairs_total),
                    static_cast<unsigned long long>(progress.validated_candidate_pairs),
                    static_cast<unsigned long long>(progress.accepted_similarity_pairs),
                    static_cast<unsigned long long>(progress.rejected_bucket_collisions),
                    progress.configured_validation_threads);
            }
        } else {
            ImGui::Text("报告阶段：%s | 路径 %llu | 分组 %llu",
                        progress.stage.c_str(),
                        static_cast<unsigned long long>(progress.processed_paths),
                        static_cast<unsigned long long>(progress.emitted_groups));
            const float exactProgress = progress.stage_total_known
                                            ? CalculateStageProgress(progress.stage_processed,
                                                                     progress.stage_total)
                                            : -static_cast<float>(ImGui::GetTime());
            const std::string exactOverlay = progress.stage_total_known
                                                 ? FormatStageProgress(progress.stage_processed,
                                                                       progress.stage_total)
                                                 : "已处理 " + std::to_string(progress.stage_processed) +
                                                       " 个精确重复组";
            DrawContrastProgressBar(exactProgress,
                                    ImVec2(ImGui::GetContentRegionAvail().x, 0.0f),
                                    exactOverlay.c_str());
        }
    }

    if (!m_reportMessage.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(m_reportMessageIsError ? ImVec4(0.95f, 0.55f, 0.66f, 1.0f)
                                                  : ImVec4(0.65f, 0.89f, 0.63f, 1.0f),
                           "%s",
                           m_reportMessage.c_str());
        ImGui::PopTextWrapPos();
    }

    if (m_openReportCleanupConfirmation) {
        ImGui::OpenPopup("确认删除重复报告");
        m_openReportCleanupConfirmation = false;
    }
    if (ImGui::BeginPopupModal("确认删除重复报告", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const bool exact =
            m_reportCleanupConfirmationKind == videosc::dedup::DuplicateReportKind::Exact;
        const char* reportName = exact ? "SHA-512 重复报告" : "视觉相似报告";
        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.66f, 1.0f),
                           "即将删除：%s",
                           reportName);
        ImGui::TextWrapped("删除后需要重新生成报告才能再次查看重复组。");
        ImGui::TextWrapped("该操作不会删除媒体文件、SHA-512、dHash、扫描记录或 MySQL 数据。");
        PushButtonStyleDanger();
        const std::string confirmLabel = std::string("确认删除 ") + reportName;
        if (ImGui::Button(confirmLabel.c_str(), ImVec2(220, 30))) {
            m_reportCleanupRequestedKind = m_reportCleanupConfirmationKind;
            m_reportCleanupRequested = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(3);
        ImGui::SameLine();
        if (ImGui::Button("取消", ImVec2(100, 30))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::Separator();
    if (ImGui::CollapsingHeader("当前报告信息", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("已发布：%s | generation %llu | 共 %llu 组",
                    ReportKindName(m_visibleReportKind),
                    static_cast<unsigned long long>(m_visibleReportGeneration),
                    static_cast<unsigned long long>(m_visibleReportGroupCount));
        if (m_visibleReportKind == videosc::dedup::DuplicateReportKind::Similar &&
            m_visibleSimilarReportMetadata.has_value()) {
            const auto& metadata = *m_visibleSimilarReportMetadata;
            if (metadata.image_uses_three_stage_verification) {
                ImGui::TextDisabled(
                    "图片规则：PDQ ≤ %u 或 dHash 回退 ≤ %u | pHash 至少 %u/16 区 | 结构分 ≥ %u/%u | 完整链接 | 三筛线程 %u",
                    metadata.image_similarity.standard_profile.pdq_max_hamming_distance,
                    metadata.image_similarity.standard_profile.fallback_dhash_max_hamming_distance,
                    metadata.image_similarity.standard_profile.zoned_phash_min_passing_tiles,
                    metadata.image_similarity.standard_profile.structural_global_edge_min_millionths,
                    metadata.image_similarity.standard_profile.structural_trimmed_block_min_millionths,
                    metadata.structural_worker_threads);
                ImGui::TextDisabled(
                    "低质量严格规则：PDQ ≤ %u 或 dHash 回退 ≤ %u | pHash 至少 %u/16 区 | 结构分 ≥ %u/%u",
                    metadata.image_similarity.low_quality_profile.pdq_max_hamming_distance,
                    metadata.image_similarity.low_quality_profile.fallback_dhash_max_hamming_distance,
                    metadata.image_similarity.low_quality_profile.zoned_phash_min_passing_tiles,
                    metadata.image_similarity.low_quality_profile.structural_global_edge_min_millionths,
                    metadata.image_similarity.low_quality_profile.structural_trimmed_block_min_millionths);
                ImGui::TextDisabled(
                    "特征完整性：%llu/%llu | 缺失 %llu | 候选 %llu | 候选峰值 %s | 延迟热门签名 %llu",
                    static_cast<unsigned long long>(metadata.image_features_complete),
                    static_cast<unsigned long long>(metadata.image_scope_total),
                    static_cast<unsigned long long>(metadata.image_features_incomplete),
                    static_cast<unsigned long long>(metadata.candidate_pairs),
                    FormatBytes(metadata.candidate_peak_bytes).c_str(),
                    static_cast<unsigned long long>(metadata.deferred_hot_signatures));
                if (metadata.partial_scope_published) {
                    ImGui::TextColored(ImVec4(0.98f, 0.70f, 0.53f, 1.0f),
                                       "该报告自动跳过了无法完成特征或结构校验的资源；结果仅覆盖成功计算的视觉内容。");
                }
            } else {
                ImGui::TextDisabled(
                    "旧图片规则：dHash 距离 ≤ %u | 视频六帧平均距离 ≤ %u | 严格完整链接",
                    metadata.image_max_hamming_distance,
                    metadata.video_max_average_hamming_distance);
            }
            if (metadata.image_uses_three_stage_verification &&
                metadata.image_similarity.standard_profile.pdq_max_hamming_distance !=
                    m_config.image_similarity.standard_profile.pdq_max_hamming_distance) {
                ImGui::TextColored(
                    ImVec4(0.98f, 0.70f, 0.53f, 1.0f),
                    "当前 PDQ 阈值=%u，与报告生成时=%u 不同；以下结果按报告生成时规则显示。",
                    m_config.image_similarity.standard_profile.pdq_max_hamming_distance,
                    metadata.image_similarity.standard_profile.pdq_max_hamming_distance);
            }
        }
    }
    if (m_visibleReportKind == videosc::dedup::DuplicateReportKind::Similar &&
        m_visibleSimilarReportMetadata.has_value() && m_visibleSkippedStats.total != 0 &&
        ImGui::CollapsingHeader("跳过内容")) {
        const auto& counts = m_visibleSkippedStats.by_reason;
        using videosc::dedup::SkippedVisualContentReason;
        const auto countOf = [&](const SkippedVisualContentReason reason) {
            return counts[static_cast<std::size_t>(reason)];
        };
        ImGui::Text("共 %llu 条：无效图片 %llu | 视频 dHash 缺失 %llu | 视频零帧 %llu | 不支持 %llu | 结构读取失败 %llu | 结构超时 %llu | 结构计算失败 %llu",
                    static_cast<unsigned long long>(m_visibleSkippedStats.total),
                    static_cast<unsigned long long>(countOf(SkippedVisualContentReason::InvalidImage)),
                    static_cast<unsigned long long>(countOf(SkippedVisualContentReason::MissingVideoDHash)),
                    static_cast<unsigned long long>(countOf(SkippedVisualContentReason::ZeroVideoFrame)),
                    static_cast<unsigned long long>(countOf(SkippedVisualContentReason::UnsupportedMedia)),
                    static_cast<unsigned long long>(countOf(SkippedVisualContentReason::StructuralIoFailure)),
                    static_cast<unsigned long long>(countOf(SkippedVisualContentReason::StructuralTimeout)),
                    static_cast<unsigned long long>(countOf(SkippedVisualContentReason::StructuralComputeFailure)));
        ImGui::BeginChild("skipped_contents", ImVec2(0.0f, 160.0f), true);
        for (const auto& record : m_visibleSkippedContents) {
            ImGui::TextWrapped("%s | %s%s%s",
                               SkippedReasonName(record.reason),
                               record.primary_sha512.c_str(),
                               record.secondary_sha512.empty() ? "" : " <-> ",
                               record.secondary_sha512.c_str());
            for (const std::wstring& path : record.sample_paths) {
                ImGui::TextDisabled("  %s", WideToUtf8(path.c_str()).c_str());
            }
        }
        ImGui::EndChild();
        if (m_visibleSkippedStats.total > m_visibleSkippedContents.size()) {
            ImGui::TextDisabled("仅显示前 %llu 条。",
                                static_cast<unsigned long long>(m_visibleSkippedContents.size()));
        }
        if (ImGui::Button("重新生成相似报告（自动回填图片特征）")) {
            StartReportGeneration(videosc::dedup::DuplicateReportKind::Similar, false);
        }
    }
    ImGui::Separator();
    ImGui::TextUnformatted("排序与选择条件");
    constexpr std::array<ReportSortMode, 7> sortModes = {
        ReportSortMode::Generated,
        ReportSortMode::ReclaimableAscending,
        ReportSortMode::ReclaimableDescending,
        ReportSortMode::MemberCountAscending,
        ReportSortMode::MemberCountDescending,
        ReportSortMode::DHashAverageAscending,
        ReportSortMode::DHashAverageDescending,
    };
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("排序方式", ReportSortModeName(m_reportSortMode))) {
        for (const ReportSortMode mode : sortModes) {
            const bool visualOnly = mode == ReportSortMode::DHashAverageAscending ||
                                    mode == ReportSortMode::DHashAverageDescending;
            if (visualOnly && m_visibleReportKind == videosc::dedup::DuplicateReportKind::Exact) continue;
            const bool selected = m_reportSortMode == mode;
            if (ImGui::Selectable(ReportSortModeName(mode), selected)) {
                m_reportSortMode = mode;
                ApplyReportSort();
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::TextDisabled("以下条件只影响按条件选择，不改变浏览窗口展示范围。");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("指定磁盘", "留空=所有磁盘；例如 PhysicalDrive3", m_deleteTargetStorageBuf,
                             sizeof(m_deleteTargetStorageBuf));
    if (m_visibleReportKind == videosc::dedup::DuplicateReportKind::Similar) {
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("图片选择距离上限（严格小于）##report",
                                 "留空=沿用报告阈值",
                                 m_imageSelectionDistanceBuf,
                                 sizeof(m_imageSelectionDistanceBuf));
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("视频选择平均距离上限（严格小于）##report",
                                 "留空=沿用报告阈值",
                                 m_videoSelectionDistanceBuf,
                                 sizeof(m_videoSelectionDistanceBuf));
    }

    ImGui::Separator();
    ImGui::TextUnformatted("保留策略");
    ImGui::TextDisabled("固定优先级从上到下，可多选，至少保留一项。");
    const auto policyCheckbox = [&](const char* label, bool& value) {
        const bool previous = value;
        if (ImGui::Checkbox(label, &value) && !m_retentionPolicies.any()) {
            value = previous;
            m_deletionMessage = "至少需要启用一个保留策略。";
            m_deletionMessageIsError = true;
        }
    };
    policyCheckbox("1 路径：保留扫描路径优先级最高", m_retentionPolicies.path_priority);
    policyCheckbox("2 质量：保留最高质量", m_retentionPolicies.highest_quality);
    policyCheckbox("3 时间：保留最新", m_retentionPolicies.newest);
    policyCheckbox("4 时间：保留最旧", m_retentionPolicies.oldest);
    policyCheckbox("5 体积：保留最大", m_retentionPolicies.largest);
    policyCheckbox("6 体积：保留最小", m_retentionPolicies.smallest);

    ImGui::Separator();
    ImGui::TextUnformatted("选择与删除");

    const bool selectionConflict = m_reportRunning.load() || m_reportCleanupRunning.load() ||
                                   m_deletionRunning.load() || m_selectionRunning.load();
    const bool scanRunning = m_scanCoordinator && m_scanCoordinator->is_running();
    const bool currentSelectionEnabled = !selectionConflict &&
                                         m_visibleReportGeneration != 0 &&
                                         m_selectedReportGroupOrdinal.has_value() &&
                                         m_selectedReportGroupId.has_value() &&
                                         m_retentionPolicies.any();
    const bool allSelectionEnabled = !selectionConflict &&
                                     m_visibleReportGeneration != 0 &&
                                     !m_reportSummaries.empty() &&
                                     m_retentionPolicies.any();
    const bool selectionGenerationMatches =
        m_reportSelectionSnapshot.source_report_generation == m_visibleReportGeneration;
    const bool deletionEnabled = !selectionConflict && !scanRunning &&
                                 m_visibleReportGeneration != 0 &&
                                 selectionGenerationMatches &&
                                 m_reportSelectionSnapshot.selected_file_count != 0;
    if (ImGui::BeginTable(
            "report_selection_commands", primaryCommandColumns, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();
        ImGui::BeginDisabled(!currentSelectionEnabled);
        if (ImGui::Button("选择当前组", ImVec2(ImGui::GetContentRegionAvail().x, 0.0f))) {
            ApplyDeletionSelection(false);
        }
        ImGui::EndDisabled();
        if (!currentSelectionEnabled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("请先在重复组浏览窗口选择一个组，并等待冲突任务结束");
        }

        ImGui::TableNextColumn();
        ImGui::BeginDisabled(!allSelectionEnabled);
        if (ImGui::Button("选择全部报告", ImVec2(ImGui::GetContentRegionAvail().x, 0.0f))) {
            ApplyDeletionSelection(true);
        }
        ImGui::EndDisabled();
        if (!allSelectionEnabled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("当前没有可选择的已发布报告，或存在冲突任务");
        }

        ImGui::TableNextColumn();
        PushButtonStyleDanger();
        ImGui::BeginDisabled(!deletionEnabled);
        if (ImGui::Button("永久删除选中项", ImVec2(ImGui::GetContentRegionAvail().x, 0.0f))) {
            m_openDeleteConfirmation = true;
        }
        ImGui::EndDisabled();
        if (!deletionEnabled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(scanRunning ? "扫描运行时禁止永久删除"
                                          : "当前报告没有有效的持久化待删除选择");
        }
        ImGui::PopStyleColor(3);
        ImGui::EndTable();
    }
    if (m_selectionRunning.load()) {
        const std::uint64_t processed = m_selectionProcessedGroups.load();
        const std::uint64_t total = m_selectionTotalGroups.load();
        const std::string overlay = FormatStageProgress(processed, total);
        DrawContrastProgressBar(CalculateStageProgress(processed, total),
                                ImVec2(ImGui::GetContentRegionAvail().x, 0.0f),
                                overlay.c_str());
        ImGui::BeginDisabled(m_selectionCancelRequested.load());
        if (ImGui::Button("取消全报告选择")) m_selectionCancelRequested.store(true);
        ImGui::EndDisabled();
    }
    ImGui::Text("已选择 %llu 个文件，共 %s，涉及 %llu 个重复组",
                static_cast<unsigned long long>(m_reportSelectionSnapshot.selected_file_count),
                FormatBytes(m_reportSelectionSnapshot.selected_total_bytes).c_str(),
                static_cast<unsigned long long>(m_reportSelectionSnapshot.selected_group_count));
    if (m_selectionCoversEntireReport) {
        ImGui::TextColored(ImVec4(0.98f, 0.70f, 0.53f, 1.0f),
                           "当前选择范围：整个报告（删除时流式读取，不一次性装入内存）");
    }
    if (!m_deletionMessage.empty()) {
        ImGui::TextColored(m_deletionMessageIsError ? ImVec4(0.95f, 0.55f, 0.66f, 1.0f)
                                                    : ImVec4(0.65f, 0.89f, 0.63f, 1.0f),
                           "%s",
                           m_deletionMessage.c_str());
    }
    if (m_deletionRunning.load()) {
        const std::uint64_t processed = m_deletionProcessedFiles.load();
        const std::uint64_t total = m_deletionTotalFiles.load();
        std::string stage;
        std::filesystem::path currentPath;
        {
            std::lock_guard<std::mutex> lock(m_deletionProgressMutex);
            stage = m_deletionProgressStage;
            currentPath = m_deletionCurrentPath;
        }
        ImGui::Text("删除阶段：%s", stage.c_str());
        const std::string overlay = FormatStageProgress(processed, total);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.953f, 0.545f, 0.659f, 0.85f));
        DrawContrastProgressBar(CalculateStageProgress(processed, total),
                                ImVec2(ImGui::GetContentRegionAvail().x, 0.0f),
                                overlay.c_str());
        ImGui::PopStyleColor();
        ImGui::Text("已处理 %s | 已释放 %s | 成功 %llu | 失败/跳过 %llu",
                    FormatBytes(m_deletionProcessedBytes.load()).c_str(),
                    FormatBytes(m_deletionProgressReclaimedBytes.load()).c_str(),
                    static_cast<unsigned long long>(m_deletionProgressSucceeded.load()),
                    static_cast<unsigned long long>(m_deletionProgressFailed.load()));
        if (!currentPath.empty()) {
            ImGui::TextDisabled("当前文件：%s", WideToUtf8(currentPath.wstring().c_str()).c_str());
        }
    }

    if (m_openDeleteConfirmation) {
        ImGui::OpenPopup("确认永久删除");
        m_openDeleteConfirmation = false;
    }
    if (ImGui::BeginPopupModal("确认永久删除", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.66f, 1.0f), "这是永久删除，文件不会进入回收站。可用副本至少保留一个。");
        ImGui::TextWrapped("执行前会复核保留文件和每个待删文件的完整 SHA-512；不一致、不可读或超时的文件会跳过并写入日志。");
        if (m_visibleReportKind == videosc::dedup::DuplicateReportKind::Similar &&
            !m_visibleReportTrusted) {
            ImGui::TextColored(ImVec4(0.98f, 0.70f, 0.53f, 1.0f),
                               "当前相似报告为旧规则报告，缺少三筛删除证据，强制删除存在误删风险。");
            ImGui::Checkbox("我已了解风险，仍要强制删除", &m_deleteOverrideUntrusted);
        }
        if (m_selectionCoversEntireReport) ImGui::Text("范围：当前已发布报告的全部 %llu 组", static_cast<unsigned long long>(m_visibleReportGroupCount));
        else ImGui::Text("范围：当前手动或条件选中的文件");
        ImGui::Text("已选择：%llu 个文件，共 %s，涉及 %llu 个组",
                    static_cast<unsigned long long>(m_reportSelectionSnapshot.selected_file_count),
                    FormatBytes(m_reportSelectionSnapshot.selected_total_bytes).c_str(),
                    static_cast<unsigned long long>(m_reportSelectionSnapshot.selected_group_count));
        const bool overrideBlocked =
            m_visibleReportKind == videosc::dedup::DuplicateReportKind::Similar &&
            !m_visibleReportTrusted && !m_deleteOverrideUntrusted;
        PushButtonStyleDanger();
        ImGui::BeginDisabled(overrideBlocked);
        if (ImGui::Button("确认永久删除", ImVec2(150, 30))) {
            m_deleteConfirmed = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::PopStyleColor(3);
        ImGui::SameLine();
        if (ImGui::Button("取消", ImVec2(100, 30))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::End();
}

/**
 * @brief 绘制独立重复组浏览窗口，只加载当前滚动区域需要的分组。
 *
 * 排序、筛选和批量操作仍由重复报告控制窗口维护；本窗口仅显示共享状态，
 * 并保留打开详情和切换单个成员保留状态这两类行级交互。
 */
void VideoScApp::RenderDuplicateGroupBrowserWindow() {
    if (!m_showDuplicateGroupBrowserWindow) return;
    ImGui::SetNextWindowSize(ImVec2(1000.0f, 760.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("重复组浏览",
                      &m_showDuplicateGroupBrowserWindow,
                      ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGui::End();
        return;
    }

    ImGui::Text("%s | generation %llu | %llu 组 | %s",
                ReportKindName(m_visibleReportKind),
                static_cast<unsigned long long>(m_visibleReportGeneration),
                static_cast<unsigned long long>(m_visibleReportGroupCount),
                ReportSortModeName(m_reportSortMode));
    ImGui::TextDisabled("已选择 %llu 个文件，共 %s，涉及 %llu 个重复组",
                        static_cast<unsigned long long>(m_reportSelectionSnapshot.selected_file_count),
                        FormatBytes(m_reportSelectionSnapshot.selected_total_bytes).c_str(),
                        static_cast<unsigned long long>(m_reportSelectionSnapshot.selected_group_count));
    if (!m_showDuplicateReportWindow) {
        if (ImGui::Button("显示重复报告控制")) m_showDuplicateReportWindow = true;
    }
    ImGui::Separator();

    if (m_visibleReportGeneration == 0) {
        ImGui::TextDisabled("尚未加载已发布报告，请在“重复报告控制”窗口生成或加载报告。");
        ImGui::End();
        return;
    }
    if (m_reportSummaries.empty() || m_reportRowStarts.size() < 2) {
        ImGui::TextDisabled("当前报告没有重复组。");
        ImGui::End();
        return;
    }

    const bool childVisible = ImGui::BeginChild("all_report_groups",
                                                ImVec2(0.0f, 0.0f),
                                                false,
                                                ImGuiWindowFlags_HorizontalScrollbar);
    if (m_resetReportScroll) {
        ImGui::SetScrollY(0.0f);
        m_resetReportScroll = false;
    }
    if (childVisible) {
        const std::uint64_t totalRows64 = m_reportRowStarts.back();
        const int totalRows = static_cast<int>((std::min<std::uint64_t>)(
            totalRows64, static_cast<std::uint64_t>((std::numeric_limits<int>::max)())));
        ImGuiListClipper clipper;
        clipper.Begin(totalRows, ImGui::GetTextLineHeightWithSpacing());
        while (clipper.Step()) {
            for (int rowIndex = clipper.DisplayStart; rowIndex < clipper.DisplayEnd; ++rowIndex) {
                const std::uint64_t row = static_cast<std::uint64_t>(rowIndex);
                const auto upper = std::upper_bound(m_reportRowStarts.begin(), m_reportRowStarts.end(), row);
                if (upper == m_reportRowStarts.begin()) continue;
                const std::size_t summaryIndex = static_cast<std::size_t>(
                    upper - m_reportRowStarts.begin() - 1);
                if (summaryIndex >= m_reportSummaries.size()) continue;
                const auto& summary = m_reportSummaries[summaryIndex];
                const std::uint64_t localRow = row - m_reportRowStarts[summaryIndex];
                ImGui::PushID(static_cast<int>(summary.ordinal & 0x7fffffff));
                if (localRow == 0) {
                    videosc::dedup::DuplicateGroup* group = AcquireReportGroup(summary.ordinal);
                    std::ostringstream title;
                    title << "重复组 " << summary.group_id << " · " << summary.member_count
                          << " 个文件 · 可释放 " << FormatBytes(summary.reclaimable_bytes);
                    if (summary.has_average_hamming_distance) {
                        title << " · 平均距离 " << std::fixed << std::setprecision(2)
                              << summary.average_hamming_distance;
                    }
                    title << " · 点击查看详情";
                    const bool detailSelected = m_showReportDetailWindow &&
                                                m_selectedReportGroupId == summary.group_id;
                    if (detailSelected) {
                        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.541f, 0.706f, 0.980f, 0.34f));
                        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.541f, 0.706f, 0.980f, 0.48f));
                        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.541f, 0.706f, 0.980f, 0.62f));
                    }
                    if (ImGui::Selectable(title.str().c_str(), detailSelected) && group) {
                        if (m_selectedReportGroupId != group->group_id) ClearReportThumbnails();
                        m_selectedReportGroupId = group->group_id;
                        m_selectedReportGroupOrdinal = summary.ordinal;
                        m_selectedReportGroup = *group;
                        m_showReportDetailWindow = true;
                    }
                    if (detailSelected) ImGui::PopStyleColor(3);
                } else if (localRow <= summary.member_count) {
                    videosc::dedup::DuplicateGroup* group = AcquireReportGroup(summary.ordinal);
                    const std::size_t memberIndex = static_cast<std::size_t>(localRow - 1);
                    if (!group || memberIndex >= group->members.size()) {
                        ImGui::TextDisabled("  成员加载失败");
                    } else {
                        const auto& member = group->members[memberIndex];
                        const bool selected = std::find(group->selected_for_deletion.begin(),
                                                        group->selected_for_deletion.end(),
                                                        member.path_id) != group->selected_for_deletion.end();
                        const std::string path = WideToUtf8(member.path.wstring().c_str());
                        std::ostringstream text;
                        text << "    " << (selected ? "[删除] " : "[保留] ") << path
                             << " · " << member.width << "×" << member.height
                             << " · " << FormatBytes(member.size_bytes);
                        ImGui::PushID(static_cast<int>(member.path_id & 0x7fffffff));
                        if (selected) {
                            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.953f, 0.545f, 0.659f, 0.30f));
                            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.953f, 0.545f, 0.659f, 0.44f));
                            ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.953f, 0.545f, 0.659f, 0.58f));
                        }
                        if (ImGui::Selectable(text.str().c_str(), selected)) {
                            ToggleReportMemberDeletion(*group, member.path_id);
                            if (m_selectedReportGroupId == group->group_id &&
                                m_selectedReportGroup.has_value()) {
                                m_selectedReportGroup->selected_for_deletion = group->selected_for_deletion;
                                m_selectedReportGroup->retained_path_id = group->retained_path_id;
                            }
                        }
                        if (selected) ImGui::PopStyleColor(3);
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                            ImGui::SetTooltip("%s", path.c_str());
                        }
                        ImGui::PopID();
                    }
                } else {
                    ImGui::Separator();
                }
                ImGui::PopID();
            }
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

void VideoScApp::PrepareReportDetailIndexes(const videosc::dedup::DuplicateGroup& group) {
    if (m_reportDetailIndexGroupId == group.group_id) return;
    m_reportDetailMemberNames.clear();
    m_reportDetailMemberNames.reserve(group.members.size());
    for (const auto& member : group.members) {
        const std::filesystem::path filename = member.path.filename();
        m_reportDetailMemberNames.emplace(
            member.path_id,
            WideToUtf8((filename.empty() ? member.path : filename).wstring().c_str()));
    }
    m_reportDetailAverageDistance = 0.0;
    for (const auto& evidence : group.evidence) {
        m_reportDetailAverageDistance += evidence.average_hamming_distance;
    }
    if (!group.evidence.empty()) {
        m_reportDetailAverageDistance /= static_cast<double>(group.evidence.size());
    }
    m_reportDetailImageRelationCount = 0;
    m_reportDetailImageRelationCache.clear();
    m_selectedImageRelationOrdinal.reset();
    m_selectedImageRelation.reset();
    m_selectedImageRelationMemberCount = 0;
    m_selectedImageRelationMemberCache.clear();
    m_reportDetailRelationLoadError.clear();
    if (m_visibleReportKind == videosc::dedup::DuplicateReportKind::Similar &&
        group.kind == videosc::dedup::DuplicateGroupKind::SimilarImage &&
        m_visibleReportGeneration != 0 &&
        m_runtimeStore &&
        m_runtimeStore->is_open()) {
        videosc::dedup::DuplicateReportStore reports(*m_runtimeStore);
        m_reportDetailImageRelationCount =
            reports.ImageRelationCount(m_visibleReportGeneration, group.group_id);
    }
    m_reportDetailIndexGroupId = group.group_id;
}

/** @brief 绘制当前重复组的独立可停靠详情窗口。 */
void VideoScApp::RenderDuplicateReportDetailWindow() {
    if (!m_showReportDetailWindow || !m_selectedReportGroupId.has_value() || !m_selectedReportGroup.has_value()) {
        return;
    }
    if (m_selectedReportGroup->group_id != *m_selectedReportGroupId) {
        m_selectedReportGroupId.reset();
        m_selectedReportGroupOrdinal.reset();
        m_selectedReportGroup.reset();
        m_showReportDetailWindow = false;
        m_clearReportThumbnailsRequested = true;
        return;
    }

    videosc::dedup::DuplicateGroup& group = *m_selectedReportGroup;
    PrepareReportDetailIndexes(group);
    const std::string windowTitle = "重复组详情 · 组 " + std::to_string(group.group_id) +
                                    "###duplicate_report_detail";
    ImGui::SetNextWindowSize(ImVec2(1040.0f, 720.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(windowTitle.c_str(),
                      &m_showReportDetailWindow,
                      ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGui::End();
        if (!m_showReportDetailWindow) {
            m_selectedReportGroupId.reset();
            m_selectedReportGroupOrdinal.reset();
            m_selectedReportGroup.reset();
            m_clearReportThumbnailsRequested = true;
        }
        return;
    }

    const char* groupKind = "SHA-512 精确重复";
    if (group.kind == videosc::dedup::DuplicateGroupKind::SimilarImage) groupKind = "三级直验相似图片";
    if (group.kind == videosc::dedup::DuplicateGroupKind::SimilarVideo) groupKind = "dHash 相似视频";
    ImGui::Text("类型：%s | 成员：%zu | 可释放：%s",
                groupKind,
                group.members.size(),
                FormatBytes(group.reclaimable_bytes).c_str());
    if (!group.algorithm_version.empty()) {
        ImGui::TextDisabled("算法版本：%s", group.algorithm_version.c_str());
    }
    if (m_visibleSimilarReportMetadata.has_value() &&
        (group.kind == videosc::dedup::DuplicateGroupKind::SimilarImage ||
         group.kind == videosc::dedup::DuplicateGroupKind::SimilarVideo)) {
        const auto& metadata = *m_visibleSimilarReportMetadata;
        if (group.kind == videosc::dedup::DuplicateGroupKind::SimilarImage &&
            metadata.image_uses_three_stage_verification) {
            ImGui::TextDisabled(
                "报告生成规则：PDQ ≤ %u；分区 pHash + 结构直验；严格完整链接；三筛线程 %u。",
                metadata.image_similarity.standard_profile.pdq_max_hamming_distance,
                metadata.structural_worker_threads);
        } else {
            ImGui::TextDisabled("报告生成规则：视频六帧 dHash；严格完整链接分组。");
        }
    }
    ImGui::TextDisabled("点击成员的 [保留]/[删除] 文本可切换状态；同组始终至少保留一个文件。");

    if (!group.evidence.empty()) {
        ImGui::Separator();
        if (group.kind == videosc::dedup::DuplicateGroupKind::SimilarImage) {
            const std::uint32_t threshold =
                m_visibleSimilarReportMetadata.has_value()
                    ? m_visibleSimilarReportMetadata->image_max_hamming_distance
                    : m_config.image_similarity.standard_profile.pdq_max_hamming_distance;
            ImGui::Text("相似度证据 · %zu 条边 · 平均 PDQ 距离 %.2f（初筛阈值 ≤ %u）",
                        group.evidence.size(),
                        m_reportDetailAverageDistance,
                        threshold);
        } else {
            ImGui::Text("相似度证据 · %zu 条边 · 平均汉明距离 %.2f（视频六帧平均阈值严格 < 5）",
                        group.evidence.size(),
                        m_reportDetailAverageDistance);
        }
        const float evidenceHeight =
            (std::min)(240.0f, 28.0f * static_cast<float>(group.evidence.size()) + 30.0f);
        if (ImGui::BeginTable("similarity_evidence",
                              5,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_ScrollY,
                              ImVec2(0.0f, evidenceHeight))) {
            ImGui::TableSetupColumn("成员 A", ImGuiTableColumnFlags_WidthStretch, 1.5f);
            ImGui::TableSetupColumn("成员 B", ImGuiTableColumnFlags_WidthStretch, 1.5f);
            const bool imageEvidence = group.kind == videosc::dedup::DuplicateGroupKind::SimilarImage;
            ImGui::TableSetupColumn(imageEvidence ? "PDQ / 分区 pHash" : "逐帧汉明距离",
                                    ImGuiTableColumnFlags_WidthStretch,
                                    1.4f);
            ImGui::TableSetupColumn(imageEvidence ? "边缘 / 块" : "平均",
                                    ImGuiTableColumnFlags_WidthFixed,
                                    imageEvidence ? 120.0f : 72.0f);
            ImGui::TableSetupColumn(imageEvidence ? "通过块" : "时长差",
                                    ImGuiTableColumnFlags_WidthFixed,
                                    90.0f);
            ImGui::TableHeadersRow();

            const auto memberName = [&](const std::uint64_t pathId) {
                const auto member = m_reportDetailMemberNames.find(pathId);
                return member == m_reportDetailMemberNames.end()
                           ? std::string("路径 #") + std::to_string(pathId)
                           : member->second;
            };

            ImGuiListClipper evidenceClipper;
            evidenceClipper.Begin(static_cast<int>(group.evidence.size()), 26.0f);
            while (evidenceClipper.Step()) {
                for (int evidenceIndex = evidenceClipper.DisplayStart;
                     evidenceIndex < evidenceClipper.DisplayEnd;
                     ++evidenceIndex) {
                    const auto& evidence = group.evidence[static_cast<std::size_t>(evidenceIndex)];
                    std::ostringstream distances;
                    const std::size_t frameCount = (std::min)(
                        static_cast<std::size_t>(evidence.compared_frame_count), evidence.frame_distances.size());
                    for (std::size_t index = 0; index < frameCount; ++index) {
                        if (index != 0) distances << ", ";
                        distances << static_cast<unsigned>(evidence.frame_distances[index]);
                    }
                    if (imageEvidence) {
                        distances.str(std::string{});
                        distances.clear();
                        distances << "PDQ " << evidence.image_pdq_hamming_distance
                                  << " | pHash "
                                  << static_cast<unsigned>(evidence.image_zoned_passing_tiles)
                                  << "/16";
                    }
                    ImGui::TableNextRow(ImGuiTableRowFlags_None, 26.0f);
                    ImGui::TableNextColumn();
                    TableTextSingleLine(memberName(evidence.left_path_id));
                    ImGui::TableNextColumn();
                    TableTextSingleLine(memberName(evidence.right_path_id));
                    ImGui::TableNextColumn();
                    TableTextSingleLine(distances.str());
                    ImGui::TableNextColumn();
                    if (imageEvidence) {
                        ImGui::Text("%.3f / %.3f",
                                    evidence.image_global_edge_zncc_millionths / 1'000'000.0,
                                    evidence.image_trimmed_block_score_millionths / 1'000'000.0);
                    } else {
                        ImGui::Text("%.2f", evidence.average_hamming_distance);
                    }
                    ImGui::TableNextColumn();
                    if (imageEvidence) {
                        ImGui::Text("%.1f%%",
                                    evidence.image_passing_block_percent_millionths / 10'000.0);
                    } else {
                        ImGui::Text("%lld ms", static_cast<long long>(evidence.duration_difference_ms));
                    }
                }
            }
            ImGui::EndTable();
        }
    }

    if (group.kind == videosc::dedup::DuplicateGroupKind::SimilarImage) {
        ImGui::Separator();
        ImGui::Text("跨严格组直接相似 · %llu 条签名关系",
                    static_cast<unsigned long long>(m_reportDetailImageRelationCount));
        ImGui::TextDisabled(
            "这里保留因严格完整链接约束而分到其他组、但两端仍通过图片三级直验的关系；只读，不计入可释放空间。");
        if (!m_reportDetailRelationLoadError.empty()) {
            ImGui::TextColored(ImVec4(0.953f, 0.545f, 0.659f, 1.0f),
                               "%s",
                               m_reportDetailRelationLoadError.c_str());
        }
        if (m_reportDetailImageRelationCount == 0) {
            ImGui::TextDisabled("当前严格组没有跨组直接相似关系。");
        } else if (m_runtimeStore && m_runtimeStore->is_open()) {
            const float relationHeight =
                (std::min)(260.0f,
                           28.0f * static_cast<float>(
                                       (std::min<std::uint64_t>)(
                                           m_reportDetailImageRelationCount,
                                           8)) +
                               30.0f);
            if (ImGui::BeginTable(
                    "cross_group_image_relations",
                    7,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                        ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY |
                        ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable,
                    ImVec2(0.0f, relationHeight))) {
                ImGui::TableSetupColumn("序号", ImGuiTableColumnFlags_WidthFixed, 58.0f);
                ImGui::TableSetupColumn("本组兼容 dHash", ImGuiTableColumnFlags_WidthFixed, 170.0f);
                ImGui::TableSetupColumn("对方兼容 dHash", ImGuiTableColumnFlags_WidthFixed, 170.0f);
                ImGui::TableSetupColumn("PDQ 距离", ImGuiTableColumnFlags_WidthFixed, 72.0f);
                ImGui::TableSetupColumn("对方严格组", ImGuiTableColumnFlags_WidthFixed, 170.0f);
                ImGui::TableSetupColumn("活动文件", ImGuiTableColumnFlags_WidthFixed, 82.0f);
                ImGui::TableSetupColumn("操作", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                ImGui::TableHeadersRow();

                const int relationRows = static_cast<int>(
                    (std::min<std::uint64_t>)(
                        m_reportDetailImageRelationCount,
                        static_cast<std::uint64_t>((std::numeric_limits<int>::max)())));
                ImGuiListClipper relationClipper;
                relationClipper.Begin(relationRows, 26.0f);
                while (relationClipper.Step()) {
                    const std::uint64_t first =
                        static_cast<std::uint64_t>(relationClipper.DisplayStart);
                    const std::size_t count = static_cast<std::size_t>(
                        relationClipper.DisplayEnd - relationClipper.DisplayStart);
                    bool rangeMissing = false;
                    for (std::uint64_t ordinal = first;
                         ordinal < first + count;
                         ++ordinal) {
                        if (m_reportDetailImageRelationCache.find(ordinal) ==
                            m_reportDetailImageRelationCache.end()) {
                            rangeMissing = true;
                            break;
                        }
                    }
                    if (rangeMissing) {
                        videosc::dedup::DuplicateReportStore reports(*m_runtimeStore);
                        std::vector<videosc::dedup::SimilarImageRelationSummary> relations;
                        const videosc::dedup::RocksStatus loaded =
                            reports.LoadImageRelations(
                                m_visibleReportGeneration,
                                group.group_id,
                                first,
                                count,
                                relations);
                        if (!loaded.succeeded) {
                            m_reportDetailRelationLoadError =
                                "加载跨组相似关系失败：" + loaded.message;
                        } else {
                            for (auto& relation : relations) {
                                m_reportDetailImageRelationCache.insert_or_assign(
                                    relation.ordinal,
                                    std::move(relation));
                            }
                        }
                    }

                    for (int relationIndex = relationClipper.DisplayStart;
                         relationIndex < relationClipper.DisplayEnd;
                         ++relationIndex) {
                        const std::uint64_t ordinal =
                            static_cast<std::uint64_t>(relationIndex);
                        const auto cached =
                            m_reportDetailImageRelationCache.find(ordinal);
                        ImGui::TableNextRow(ImGuiTableRowFlags_None, 26.0f);
                        if (cached == m_reportDetailImageRelationCache.end()) {
                            ImGui::TableNextColumn();
                            ImGui::TextDisabled("%llu",
                                                static_cast<unsigned long long>(ordinal + 1));
                            ImGui::TableNextColumn();
                            ImGui::TextDisabled("加载失败");
                            continue;
                        }
                        const auto& relation = cached->second;
                        const bool selected =
                            m_selectedImageRelationOrdinal == relation.ordinal;
                        if (selected) {
                            ImGui::TableSetBgColor(
                                ImGuiTableBgTarget_RowBg0,
                                ImGui::ColorConvertFloat4ToU32(
                                    ImVec4(0.541f, 0.706f, 0.980f, 0.20f)));
                        }
                        ImGui::TableNextColumn();
                        ImGui::Text("%llu",
                                    static_cast<unsigned long long>(ordinal + 1));
                        ImGui::TableNextColumn();
                        TableTextSingleLine(DHashToHex(relation.current_image_dhash));
                        ImGui::TableNextColumn();
                        TableTextSingleLine(DHashToHex(relation.neighbor_image_dhash));
                        ImGui::TableNextColumn();
                        ImGui::Text("%u",
                                    static_cast<unsigned>(relation.hamming_distance));
                        ImGui::TableNextColumn();
                        std::string neighborGroup =
                            std::to_string(relation.neighbor_group_id);
                        neighborGroup += relation.neighbor_group_in_main_report
                                             ? "（主报告组）"
                                             : "（单签名端点）";
                        TableTextSingleLine(neighborGroup);
                        ImGui::TableNextColumn();
                        ImGui::Text("%llu",
                                    static_cast<unsigned long long>(
                                        relation.neighbor_active_member_count));
                        ImGui::TableNextColumn();
                        ImGui::PushID(
                            static_cast<int>((relation.ordinal ^ 0x49a3b2c1ULL) &
                                             0x7fffffff));
                        if (ImGui::SmallButton(selected ? "正在查看" : "查看对方文件")) {
                            m_selectedImageRelationOrdinal = relation.ordinal;
                            m_selectedImageRelation = relation;
                            m_selectedImageRelationMemberCount =
                                relation.neighbor_active_member_count;
                            m_selectedImageRelationMemberCache.clear();
                            m_reportDetailRelationLoadError.clear();
                        }
                        ImGui::PopID();
                    }
                }
                ImGui::EndTable();
            }
        }

        if (m_selectedImageRelation.has_value()) {
            const auto& relation = *m_selectedImageRelation;
            ImGui::Spacing();
            ImGui::Text(
                "对方签名活动文件 · %llu 个 · 汉明距离 %u",
                static_cast<unsigned long long>(m_selectedImageRelationMemberCount),
                static_cast<unsigned>(relation.hamming_distance));
            ImGui::TextDisabled("当前签名 SHA-512：");
            ImGui::SameLine();
            TextCopyable("##cross_relation_current_sha",
                         relation.current_representative_sha512);
            ImGui::TextDisabled("对方签名 SHA-512：");
            ImGui::SameLine();
            TextCopyable("##cross_relation_neighbor_sha",
                         relation.neighbor_representative_sha512);

            const float memberHeight =
                (std::min)(480.0f,
                           150.0f * static_cast<float>(
                                        (std::min<std::uint64_t>)(
                                            m_selectedImageRelationMemberCount,
                                            3)) +
                               30.0f);
            if (ImGui::BeginTable(
                    "cross_relation_members",
                    6,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                        ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY |
                        ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable,
                    ImVec2(0.0f, memberHeight))) {
                ImGui::TableSetupColumn("预览", ImGuiTableColumnFlags_WidthFixed, 180.0f);
                ImGui::TableSetupColumn("完整路径", ImGuiTableColumnFlags_WidthStretch, 320.0f);
                ImGui::TableSetupColumn("SHA-512", ImGuiTableColumnFlags_WidthFixed, 280.0f);
                ImGui::TableSetupColumn("dHash", ImGuiTableColumnFlags_WidthFixed, 170.0f);
                ImGui::TableSetupColumn("磁盘", ImGuiTableColumnFlags_WidthFixed, 130.0f);
                ImGui::TableSetupColumn("大小", ImGuiTableColumnFlags_WidthFixed, 90.0f);
                ImGui::TableHeadersRow();

                const int memberRows = static_cast<int>(
                    (std::min<std::uint64_t>)(
                        m_selectedImageRelationMemberCount,
                        static_cast<std::uint64_t>((std::numeric_limits<int>::max)())));
                ImGuiListClipper memberClipper;
                memberClipper.Begin(memberRows, 142.0f);
                while (memberClipper.Step()) {
                    const std::uint64_t first =
                        static_cast<std::uint64_t>(memberClipper.DisplayStart);
                    const std::size_t count = static_cast<std::size_t>(
                        memberClipper.DisplayEnd - memberClipper.DisplayStart);
                    bool rangeMissing = false;
                    for (std::uint64_t ordinal = first;
                         ordinal < first + count;
                         ++ordinal) {
                        if (m_selectedImageRelationMemberCache.find(ordinal) ==
                            m_selectedImageRelationMemberCache.end()) {
                            rangeMissing = true;
                            break;
                        }
                    }
                    if (rangeMissing && m_runtimeStore && m_runtimeStore->is_open()) {
                        videosc::dedup::DuplicateReportStore reports(*m_runtimeStore);
                        std::vector<videosc::dedup::DuplicateMember> members;
                        const videosc::dedup::RocksStatus loaded =
                            reports.LoadImageSignatureMembers(
                                m_visibleReportGeneration,
                                relation.neighbor_representative_sha512,
                                first,
                                count,
                                members);
                        if (!loaded.succeeded) {
                            m_reportDetailRelationLoadError =
                                "加载跨组签名活动文件失败：" + loaded.message;
                        } else {
                            for (std::size_t index = 0; index < members.size(); ++index) {
                                m_selectedImageRelationMemberCache.insert_or_assign(
                                    first + static_cast<std::uint64_t>(index),
                                    std::move(members[index]));
                            }
                        }
                    }

                    for (int memberIndex = memberClipper.DisplayStart;
                         memberIndex < memberClipper.DisplayEnd;
                         ++memberIndex) {
                        const std::uint64_t ordinal =
                            static_cast<std::uint64_t>(memberIndex);
                        const auto cached =
                            m_selectedImageRelationMemberCache.find(ordinal);
                        ImGui::TableNextRow(ImGuiTableRowFlags_None, 142.0f);
                        if (cached == m_selectedImageRelationMemberCache.end()) {
                            ImGui::TableNextColumn();
                            ImGui::TextDisabled("成员加载失败");
                            continue;
                        }
                        const auto& member = cached->second;
                        ImGui::TableNextColumn();
                        bool previewPending = false;
                        std::string previewFailure;
                        const std::filesystem::path previewPath =
                            AcquireImagePreview(member, previewPending, previewFailure);
                        ReportThumbnailTex* texture =
                            previewPath.empty()
                                ? nullptr
                                : AcquireReportThumbnail(
                                      previewPath,
                                      videosc::dedup::MediaKind::Image);
                        const bool texturePending =
                            !previewPath.empty() && texture == nullptr &&
                            m_reportThumbnailFailures.count(previewPath.wstring()) == 0;
                        if (texture && texture->srv) {
                            const float width = static_cast<float>(texture->width);
                            const float height = static_cast<float>(texture->height);
                            const float scale =
                                (std::min)(168.0f / (std::max)(1.0f, width),
                                           128.0f / (std::max)(1.0f, height));
                            ImGui::Image(
                                reinterpret_cast<ImTextureID>(texture->srv),
                                ImVec2(width * scale, height * scale));
                        } else if (previewPending) {
                            ImGui::TextDisabled("正在生成图片预览…");
                        } else if (texturePending) {
                            ImGui::TextDisabled("等待纹理上传…");
                        } else {
                            ImGui::TextDisabled(
                                "%s",
                                previewFailure.empty()
                                    ? "预览不可用"
                                    : previewFailure.c_str());
                        }

                        ImGui::TableNextColumn();
                        TableTextSingleLine(
                            WideToUtf8(member.path.wstring().c_str()));
                        ImGui::TableNextColumn();
                        ImGui::PushID(
                            static_cast<int>((member.path_id ^ 0x6c7d8e9fULL) &
                                             0x7fffffff));
                        TextCopyable(
                            "##cross_relation_member_sha",
                            videosc::dedup::Sha512ToHex(member.content_sha512));
                        ImGui::TableNextColumn();
                        TextCopyable(
                            "##cross_relation_member_dhash",
                            member.image_dhash.has_value()
                                ? DHashToHex(*member.image_dhash)
                                : "无（未计算）");
                        ImGui::PopID();
                        ImGui::TableNextColumn();
                        TableTextSingleLine(
                            WideToUtf8(member.storage_target_key.c_str()));
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(
                            FormatBytes(member.size_bytes).c_str());
                    }
                }
                ImGui::EndTable();
            }
        }
    }

    ImGui::Separator();
    ImGui::Text("成员详细对比");
    ImGui::TextDisabled("图片使用临时缩略图；视频把 2×3 拼图拆成六帧显示。SHA-512 与 dHash 均可复制。");
    const float tableHeight = (std::min)(620.0f, 178.0f * group.members.size() + 30.0f);
    if (ImGui::BeginTable("detail_members",
                          8,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable,
                          ImVec2(0.0f, tableHeight))) {
        ImGui::TableSetupColumn("状态", ImGuiTableColumnFlags_WidthFixed, 68.0f);
        ImGui::TableSetupColumn("预览", ImGuiTableColumnFlags_WidthFixed, 180.0f);
        ImGui::TableSetupColumn("完整路径", ImGuiTableColumnFlags_WidthStretch, 320.0f);
        ImGui::TableSetupColumn("SHA-512", ImGuiTableColumnFlags_WidthFixed, 280.0f);
        ImGui::TableSetupColumn("dHash", ImGuiTableColumnFlags_WidthFixed, 245.0f);
        ImGui::TableSetupColumn("磁盘", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn("大小", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("质量与时间", ImGuiTableColumnFlags_WidthFixed, 190.0f);
        ImGui::TableHeadersRow();

        ImGuiListClipper memberClipper;
        memberClipper.Begin(static_cast<int>(group.members.size()), 166.0f);
        while (memberClipper.Step()) {
            for (int memberIndex = memberClipper.DisplayStart;
                 memberIndex < memberClipper.DisplayEnd;
                 ++memberIndex) {
            const auto& member = group.members[static_cast<std::size_t>(memberIndex)];
            const bool selectedForDeletion =
                std::find(group.selected_for_deletion.begin(),
                          group.selected_for_deletion.end(),
                          member.path_id) != group.selected_for_deletion.end();
            ImGui::TableNextRow(ImGuiTableRowFlags_None, 166.0f);
            if (selectedForDeletion) {
                ImGui::TableSetBgColor(
                    ImGuiTableBgTarget_RowBg0,
                    ImGui::ColorConvertFloat4ToU32(ImVec4(0.953f, 0.545f, 0.659f, 0.20f)));
            }

            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(member.path_id & 0x7FFFFFFF));
            if (selectedForDeletion) {
                ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.953f, 0.545f, 0.659f, 0.34f));
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.953f, 0.545f, 0.659f, 0.48f));
                ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.953f, 0.545f, 0.659f, 0.62f));
            }
            if (ImGui::Selectable(selectedForDeletion ? "[删除]" : "[保留]", selectedForDeletion)) {
                ToggleReportMemberDeletion(group, member.path_id);
            }
            if (selectedForDeletion) ImGui::PopStyleColor(3);
            ImGui::PopID();

            ImGui::TableNextColumn();
            std::filesystem::path previewPath;
            bool previewPending = false;
            std::string previewFailure;
            if (member.media_kind == videosc::dedup::MediaKind::Image) {
                previewPath = AcquireImagePreview(member, previewPending, previewFailure);
            } else {
                previewPath = member.thumbnail_path.empty() ? member.path : member.thumbnail_path;
            }
            if (member.media_kind == videosc::dedup::MediaKind::Video &&
                (member.thumbnail_path.empty() || member.thumbnail_path == member.path ||
                 !std::filesystem::exists(member.thumbnail_path))) {
                previewPath = AcquireVideoContactSheet(member);
                previewPending = previewPath.empty() &&
                                 m_reportThumbnailFailures.count(member.path.wstring()) == 0;
            }
            ReportThumbnailTex* texture = previewPath.empty()
                                              ? nullptr
                                              : AcquireReportThumbnail(previewPath, member.media_kind);
            const bool texturePending =
                !previewPath.empty() && texture == nullptr &&
                m_reportThumbnailFailures.count(previewPath.wstring()) == 0;
            if (texture && texture->srv) {
                const float width = static_cast<float>(texture->width);
                const float height = static_cast<float>(texture->height);
                if (member.media_kind == videosc::dedup::MediaKind::Video) {
                    constexpr float frameWidth = 52.0f;
                    constexpr float frameHeight = 58.0f;
                    for (int frame = 0; frame < 6; ++frame) {
                        const int column = frame % 3;
                        const int row = frame / 3;
                        const ImVec2 uv0(static_cast<float>(column) / 3.0f,
                                         static_cast<float>(row) / 2.0f);
                        const ImVec2 uv1(static_cast<float>(column + 1) / 3.0f,
                                         static_cast<float>(row + 1) / 2.0f);
                        ImGui::Image(reinterpret_cast<ImTextureID>(texture->srv),
                                     ImVec2(frameWidth, frameHeight),
                                     uv0,
                                     uv1);
                        if (column != 2) ImGui::SameLine(0.0f, 3.0f);
                    }
                } else {
                    const float scale = (std::min)(168.0f / (std::max)(1.0f, width),
                                                   128.0f / (std::max)(1.0f, height));
                    ImGui::Image(reinterpret_cast<ImTextureID>(texture->srv),
                                 ImVec2(width * scale, height * scale));
                }
            } else {
                const char* status = "预览不可用";
                if (previewPending) {
                    status = member.media_kind == videosc::dedup::MediaKind::Image
                                 ? "正在生成图片预览…"
                                 : "正在后台生成六帧预览…";
                } else if (texturePending) {
                    status = "等待纹理上传…";
                } else if (!previewFailure.empty()) {
                    status = previewFailure.c_str();
                }
                ImGui::TextDisabled("%s", status);
            }

            ImGui::TableNextColumn();
            TableTextSingleLine(WideToUtf8(member.path.wstring().c_str()));
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>((member.path_id ^ 0x5a5a5a5aULL) & 0x7fffffff));
            TextCopyable("##report_sha", videosc::dedup::Sha512ToHex(member.content_sha512));
            ImGui::TableNextColumn();
            if (member.media_kind == videosc::dedup::MediaKind::Image) {
                std::string imageDHash = member.image_dhash.has_value()
                                             ? DHashToHex(*member.image_dhash)
                                             : "无（未计算）";
                if (member.image_dhash.has_value() && *member.image_dhash == 0) {
                    imageDHash += "（无效）";
                }
                TextCopyable("##report_image_dhash", imageDHash);
            } else if (member.media_kind == videosc::dedup::MediaKind::Video) {
                const bool videoDHashInvalid =
                    member.has_video_dhashes &&
                    std::any_of(member.video_dhashes.begin(),
                                member.video_dhashes.end(),
                                [](const std::uint64_t hash) { return hash == 0; });
                for (std::size_t frame = 0; frame < member.video_dhashes.size(); ++frame) {
                    const std::string caption = "帧 " + std::to_string(frame + 1) + ":";
                    const std::string inputId = "##report_video_dhash_" + std::to_string(frame);
                    ImGui::TextDisabled("%s", caption.c_str());
                    ImGui::SameLine();
                    std::string videoDHash = member.has_video_dhashes
                                                 ? DHashToHex(member.video_dhashes[frame])
                                                 : "无（未计算）";
                    if (member.has_video_dhashes && member.video_dhashes[frame] == 0) {
                        videoDHash += "（无效）";
                    }
                    TextCopyable(inputId.c_str(), videoDHash);
                }
                if (videoDHashInvalid) {
                    ImGui::TextColored(ImVec4(0.953f, 0.545f, 0.659f, 1.0f),
                                       "整体无效：存在 0 值帧");
                }
            } else {
                TextCopyable("##report_dhash_not_applicable", "不适用");
            }
            ImGui::PopID();
            ImGui::TableNextColumn();
            TableTextSingleLine(WideToUtf8(member.storage_target_key.c_str()));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(FormatBytes(member.size_bytes).c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%u × %u", member.width, member.height);
            if (member.bitrate != 0) {
                ImGui::Text("码率：%s/s", FormatBytes(member.bitrate / 8).c_str());
            }
            TableTextSingleLine("修改：" + FormatUtcTimestamp(member.last_write_time_utc_ms));
            }
        }
        ImGui::EndTable();
    }

    ImGui::End();
    if (!m_showReportDetailWindow) {
        m_selectedReportGroupId.reset();
        m_selectedReportGroupOrdinal.reset();
        m_selectedReportGroup.reset();
        m_clearReportThumbnailsRequested = true;
    }
}

// -----------------------------------------------------------------------------
// RenderResultsWindow: 单文件诊断结果
// -----------------------------------------------------------------------------

void VideoScApp::RenderResultsWindow() {
    if (!m_showResultsWindow) return;

    // ------------- 单文件诊断窗口 -------------
    if (!ImGui::Begin("单文件诊断",
                      &m_showResultsWindow,
                      ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGui::End();
        return;
    }

    if (m_result.statusCode != VIDEOSC_OK || m_result.thumbnailCount == 0) {
        ImGui::TextDisabled("暂无结果。请在「视频截图」窗口中点击「开始截图」。");
    } else {
        ImGui::Text("缩略图列表 (%d 张)", m_result.thumbnailCount);

        if (ImGui::BeginTable("thumb_table",
                              2,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_Resizable)) {
            for (int i = 0; i < m_result.thumbnailCount; ++i) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("路径:");
                ImGui::PushID(i);
                TextCopyable("##tpath", m_result.thumbnailPaths[i]);
                ImGui::TextDisabled("dHash:");
                TextCopyable("##tdhash", m_result.thumbnailDHashes[i]);
                ImGui::PopID();
                ImGui::TableNextColumn();
                if (i < (int)m_thumbnails.size() && m_thumbnails[i].srv) {
                    // Limit display size to 200px on long edge
                    float dispW = (float)m_thumbnails[i].width;
                    float dispH = (float)m_thumbnails[i].height;
                    float maxEdge = 200.0f;
                    if (dispW > maxEdge || dispH > maxEdge) {
                        float s = (dispW > dispH) ? maxEdge / dispW : maxEdge / dispH;
                        dispW *= s; dispH *= s;
                    }
                    ImGui::Image((ImTextureID)m_thumbnails[i].srv, ImVec2(dispW, dispH));
                } else {
                    ImGui::TextDisabled("(无图像)");
                }
            }
            ImGui::EndTable();
        }
    }

    ImGui::Separator();
    ImGui::Text("哈希工具");

    PushButtonStyleAccent();
    if (ImGui::Button("计算第1张的 SHA-512")) {
        if (m_result.thumbnailCount > 0) {
            if (!ComputeFileSHA512(m_result.thumbnailPaths[0], m_shaBuf, (int)sizeof(m_shaBuf))) {
                strncpy_s(m_shaBuf, "计算失败", _TRUNCATE);
            }
        }
    }
    ImGui::PopStyleColor(3);
    ImGui::SameLine();
    if (m_shaBuf[0]) {
        ImGui::TextDisabled("SHA-512:");
        ImGui::SameLine();
        TextCopyable("##sha512", m_shaBuf);
    }

    PushButtonStyleAccent();
    if (ImGui::Button("计算第1张的 dHash")) {
        if (m_result.thumbnailCount > 0) {
            if (!ComputeImageDHash(m_result.thumbnailPaths[0], m_dh1Buf, (int)sizeof(m_dh1Buf))) {
                strncpy_s(m_dh1Buf, "失败", _TRUNCATE);
            }
        }
    }
    ImGui::PopStyleColor(3);
    ImGui::SameLine();
    if (m_dh1Buf[0]) {
        ImGui::TextDisabled("dHash #1:");
        ImGui::SameLine();
        TextCopyable("##dhash1", m_dh1Buf);
    }

    PushButtonStyleAccent();
    if (ImGui::Button("计算第2张的 dHash")) {
        if (m_result.thumbnailCount > 1) {
            if (!ComputeImageDHash(m_result.thumbnailPaths[1], m_dh2Buf, (int)sizeof(m_dh2Buf))) {
                strncpy_s(m_dh2Buf, "失败", _TRUNCATE);
            }
        }
    }
    ImGui::PopStyleColor(3);
    ImGui::SameLine();
    if (m_dh2Buf[0]) {
        ImGui::TextDisabled("dHash #2:");
        ImGui::SameLine();
        TextCopyable("##dhash2", m_dh2Buf);
    }

    PushButtonStyleAccent();
    if (ImGui::Button("计算汉明距离 (dHash#1, dHash#2)")) {
        if (m_dh1Buf[0] && m_dh2Buf[0]) {
            m_hamming = ComputeHammingDistance(m_dh1Buf, m_dh2Buf);
        }
    }
    ImGui::PopStyleColor(3);
    ImGui::SameLine();
    if (m_hamming >= 0) {
        ImGui::Text("汉明距离 = %d", m_hamming);
        if (m_hamming < 5)       ImGui::SameLine(), ImGui::TextDisabled("(极相似)");
        else if (m_hamming < 10) ImGui::SameLine(), ImGui::TextDisabled("(相似)");
        else                   ImGui::SameLine(), ImGui::TextDisabled("(差异明显)");
    }

    ImGui::End();
}

// -----------------------------------------------------------------------------
// RenderDiskInfoWindow: 磁盘信息
// -----------------------------------------------------------------------------

void VideoScApp::RenderDiskInfoWindow() {
    if (!m_showDiskInfoWindow) return;

    // ------------- 磁盘信息窗口 -------------
    if (!ImGui::Begin("磁盘信息",
                      &m_showDiskInfoWindow,
                      ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGui::End();
        return;
    }

    ImGui::TextDisabled("查询文件/目录所在物理磁盘序号 (\\\\.\\PHYSICALDRIVE<n>)");

    ImGui::Text("查询路径");
    ImGui::PushItemWidth(-120);
    ImGui::InputText("##dpath", m_diskPathBuf, sizeof(m_diskPathBuf));
    ImGui::PopItemWidth();
    ImGui::SameLine();
    PushButtonStyleNeutral();
    if (ImGui::Button("浏览...##dpath")) {
        wchar_t buf[1024] = {};
        if (OpenFileDialog(buf, 1024,
                L"所有文件\0*.*\0视频文件\0*.mp4;*.mkv;*.avi;*.mov\0",
                false)) {
            std::string utf8 = WideToUtf8(buf);
            strncpy_s(m_diskPathBuf, utf8.c_str(), _TRUNCATE);
        }
    }
    ImGui::PopStyleColor(3);

    ImGui::Separator();

    PushButtonStylePrimary();
    if (ImGui::Button("查询物理磁盘", ImVec2(200, 32))) {
        m_runDiskQuery = true;
    }
    ImGui::PopStyleColor(3);

    ImGui::SameLine();
    PushButtonStyleSuccess();
    if (ImGui::Button("使用当前视频路径")) {
        if (m_videoPathBuf[0]) {
            strncpy_s(m_diskPathBuf, m_videoPathBuf, _TRUNCATE);
            m_runDiskQuery = true;
        }
    }
    ImGui::PopStyleColor(3);
    ImGui::SameLine();
    PushButtonStyleSuccess();
    if (ImGui::Button("使用输出目录")) {
        if (m_outputDirBuf[0]) {
            strncpy_s(m_diskPathBuf, m_outputDirBuf, _TRUNCATE);
            m_runDiskQuery = true;
        }
    }
    ImGui::PopStyleColor(3);

    ImGui::Separator();

    // Result display
    if (m_diskNumber == -2) {
        ImGui::TextDisabled("尚未查询");
    } else if (m_diskNumber >= 0) {
        ImGui::TextColored(ImVec4(0.65f, 0.89f, 0.63f, 1.0f), "查询成功");
        ImGui::Text("路径         :");
        ImGui::SameLine();
        TextCopyable("##disk_path", m_diskPathBuf);
        ImGui::Text("物理磁盘序号 : %d", m_diskNumber);
        ImGui::Text("设备名        :");
        ImGui::SameLine();
        char devName[64];
        snprintf(devName, sizeof(devName), "\\\\.\\PHYSICALDRIVE%d", m_diskNumber);
        TextCopyable("##disk_dev", devName);
    } else {
        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.66f, 1.0f), "查询失败 (代码=%d)", m_diskNumber);
        const char* reason = "";
        switch (m_diskNumber) {
            case -1: reason = "路径无效 (无盘符 / UNC 路径 / 空路径)"; break;
            case -2: reason = "无法打开卷 (访问被拒绝或驱动器无效)"; break;
            case -3: reason = "IOCTL_STORAGE_GET_DEVICE_NUMBER 失败"; break;
            default: reason = "未知错误"; break;
        }
        ImGui::TextWrapped("原因: %s", reason);
    }

    ImGui::Separator();
    ImGui::TextDisabled("提示: 拒绝 UNC 路径 (\\\\server\\share), 仅支持盘符路径 (X:\\...)");

    ImGui::End();
}

// -----------------------------------------------------------------------------
// RenderFileSearchWindow: 文件检索
// -----------------------------------------------------------------------------

void VideoScApp::RenderFileSearchWindow() {
    if (!m_showFileSearchWindow) return;

    // ------------- 文件检索窗口 -------------
    if (!ImGui::Begin("文件检索",
                      &m_showFileSearchWindow,
                      ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGui::End();
        return;
    }

    ImGui::TextDisabled("通过 Everything SDK 异步查询路径下所有文件, 按物理磁盘分组输出");

    ImGui::Text("输入路径 (每行一个)");
    ImGui::InputTextMultiline("##fpaths", m_pathsBuf, sizeof(m_pathsBuf),
                              ImVec2(-1, 100));

    ImGui::Text("输出目录");
    ImGui::PushItemWidth(-120);
    ImGui::InputText("##fout", m_outputBuf, sizeof(m_outputBuf));
    ImGui::PopItemWidth();
    ImGui::SameLine();
    PushButtonStyleNeutral();
    if (ImGui::Button("浏览...##fout")) {
        wchar_t buf[1024] = {};
        if (OpenFileDialog(buf, 1024, nullptr, true)) {
            std::string utf8 = WideToUtf8(buf);
            strncpy_s(m_outputBuf, utf8.c_str(), _TRUNCATE);
        }
    }
    ImGui::PopStyleColor(3);

    ImGui::Separator();

    // Control buttons
    if (!m_fileQuery.IsRunning()) {
        PushButtonStylePrimary();
        if (ImGui::Button("开始检索", ImVec2(200, 32))) {
            m_runFileQuery = true;
        }
        ImGui::PopStyleColor(3);
    } else {
        PushButtonStyleDanger();
        if (ImGui::Button("取消", ImVec2(200, 32))) {
            m_fileQuery.Cancel();
        }
        ImGui::PopStyleColor(3);
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.98f, 0.89f, 0.69f, 1.0f), "检索中...");
    }

    ImGui::SameLine();
    PushButtonStyleSuccess();
    if (ImGui::Button("使用当前视频路径")) {
        if (m_videoPathBuf[0]) {
            std::string s = m_videoPathBuf;
            strncpy_s(m_pathsBuf, s.c_str(), _TRUNCATE);
        }
    }
    ImGui::PopStyleColor(3);
    ImGui::SameLine();
    PushButtonStyleSuccess();
    if (ImGui::Button("使用输出目录")) {
        if (m_outputDirBuf[0]) {
            std::string s = m_outputDirBuf;
            strncpy_s(m_pathsBuf, s.c_str(), _TRUNCATE);
        }
    }
    ImGui::PopStyleColor(3);

    ImGui::Separator();

    // Status & results
    if (m_fileQuery.IsRunning()) {
        ImGui::TextDisabled("正在检索, 请稍候...");
    } else if (!m_fileResultsFetched && !m_fileQuery.IsDone()) {
        ImGui::TextDisabled("尚未开始检索");
    } else if (m_fileQuery.IsDone()) {
        if (!m_fileResultsFetched) {
            m_fileResults = m_fileQuery.GetResult();
            m_fileResultsFetched = true;
        }

        if (m_fileResults.empty()) {
            ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.66f, 1.0f), "无结果");
        } else {
            ImGui::Text("检索结果 (按磁盘分组, %d 组)", (int)m_fileResults.size());

            if (ImGui::BeginTable("file_results", 4,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
                ImGui::TableSetupColumn("磁盘号");
                ImGui::TableSetupColumn("状态");
                ImGui::TableSetupColumn("文件数");
                ImGui::TableSetupColumn("输出文件");
                ImGui::TableHeadersRow();

                for (int i = 0; i < (int)m_fileResults.size(); ++i) {
                    const auto& r = m_fileResults[i];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", r.diskNumber);
                    ImGui::TableNextColumn();
                    if (r.success) {
                            ImGui::TextColored(ImVec4(0.65f, 0.89f, 0.63f, 1.0f), "成功");
                        } else {
                            ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.66f, 1.0f), "失败");
                        }
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", r.fileCount);
                    ImGui::TableNextColumn();
                    if (!r.outputFile.empty()) {
                        ImGui::PushID(i);
                        TextCopyable("##outfile", r.outputFile);
                        ImGui::PopID();
                    } else {
                        ImGui::TextDisabled("-");
                    }
                    if (!r.error.empty()) {
                        std::string errStr = "错误: " + r.error;
                        ImGui::PushID(i * 1000 + 1);
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.55f, 0.66f, 1.0f));
                        TextCopyable("##err", errStr);
                        ImGui::PopStyleColor();
                        ImGui::PopID();
                    }
                }
                ImGui::EndTable();
            }

            PushButtonStyleNeutral();
            if (ImGui::Button("打开输出目录")) {
                if (m_outputBuf[0]) {
                    std::wstring w = Utf8ToWide(m_outputBuf);
                    ShellExecuteW(nullptr, L"explore", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
            }
            ImGui::PopStyleColor(3);
        }
    }

    ImGui::Separator();
    ImGui::TextDisabled("说明: SDK 路径等参数请在「设置」窗口中配置");
    ImGui::TextDisabled("前提: Everything64.dll 已安装; 服务未运行时将自动启动 Everything.exe 并等待就绪");

    ImGui::End();
}

// -----------------------------------------------------------------------------
// RenderSettingsWindow: 设置
// -----------------------------------------------------------------------------

void VideoScApp::RenderSettingsWindow() {
    if (!m_showSettingsWindow) {
        return;
    }

    if (!ImGui::Begin("设置",
                      &m_showSettingsWindow,
                      ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGui::End();
        return;
    }
    const std::string configPath = WideToUtf8(m_configStore.config_path().wstring().c_str());
    ImGui::Text("配置文件");
    ImGui::SameLine();
    TextCopyable("##config_path", configPath);
    ImGui::Text("schema_version: %u", m_config.schema_version);
    ImGui::SameLine();
    ImGui::TextDisabled("媒体特征算法版本：media-dhash-v2");

    ImGui::BeginDisabled(!m_configCanSave);
    PushButtonStylePrimary();
    if (ImGui::Button("保存配置", ImVec2(140, 30))) {
        SaveConfiguration();
    }
    ImGui::PopStyleColor(3);
    ImGui::EndDisabled();
    ImGui::SameLine();
    PushButtonStyleNeutral();
    if (ImGui::Button("重新加载", ImVec2(120, 30))) {
        ReloadConfiguration();
    }
    ImGui::PopStyleColor(3);

    ImGui::Spacing();
    ImGui::Text("界面布局");
    const char* layoutPath = ImGui::GetIO().IniFilename;
    TextCopyable("##layout_path", layoutPath == nullptr ? "布局持久化未启用" : layoutPath);
    PushButtonStyleNeutral();
    if (ImGui::Button("重置初始布局", ImVec2(150, 30))) {
        m_resetDockLayoutRequested = true;
    }
    ImGui::PopStyleColor(3);

    ImGui::Spacing();
    ImGui::Text("异常与诊断");
    const std::string crashDirectory = WideToUtf8(CrashHandler::CrashDirectory().wstring().c_str());
    TextCopyable("##crash_directory", crashDirectory);
    PushButtonStyleNeutral();
    if (ImGui::Button("打开诊断目录", ImVec2(150, 30))) {
        ShellExecuteW(nullptr, L"explore", CrashHandler::CrashDirectory().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::PopStyleColor(3);
    ImGui::SameLine();
    ImGui::TextDisabled("异常日志与小型转储不会自动上传");

    if (!m_configMessage.empty()) {
        const ImVec4 color = m_configMessageIsError
                                 ? ImVec4(0.95f, 0.55f, 0.66f, 1.0f)
                                 : ImVec4(0.65f, 0.89f, 0.63f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TextWrapped("%s", m_configMessage.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Separator();

    const std::uint32_t stepOne = 1;
    const std::uint64_t stepOne64 = 1;
    if (ImGui::CollapsingHeader("计算线程", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("自动分配计算线程", &m_config.compute.adaptive_worker_threads);
        ImGui::InputScalar(
            m_config.compute.adaptive_worker_threads ? "计算线程硬上限" : "固定计算线程数",
            ImGuiDataType_U32,
            &m_config.compute.worker_threads,
            &stepOne);
        ImGui::BeginDisabled(!m_config.compute.adaptive_worker_threads);
        ImGui::InputScalar(
            "系统 CPU 目标 (%)", ImGuiDataType_U32, &m_config.compute.cpu_target_percent, &stepOne);
        ImGui::EndDisabled();
        ImGui::InputScalar(
            "FFmpeg 单任务线程", ImGuiDataType_U32, &m_config.compute.ffmpeg_threads_per_task, &stepOne);
        ImGui::TextDisabled("自动模式每阶段从 1 线程开始，每秒按系统总 CPU 只增加 1；阶段内不回收。");
        ImGui::TextDisabled("自动分配关闭时，SHA-512 和媒体阶段严格使用固定线程数。");
    }

    if (ImGui::CollapsingHeader("SHA-512 与磁盘读取", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("智能分配每块磁盘读取线程", &m_config.storage.adaptive_read_threads);
        ImGui::BeginDisabled(!m_config.storage.adaptive_read_threads);
        ImGui::InputScalar("单盘读取占用目标 (%)",
                           ImGuiDataType_U32,
                           &m_config.storage.disk_read_target_percent,
                           &stepOne);
        ImGui::EndDisabled();
        ImGui::InputScalar("文件读取并发总上限",
                           ImGuiDataType_U32,
                           &m_config.storage.max_concurrent_file_reads,
                           &stepOne);
        ImGui::InputScalar("单块 HDD 读取线程",
                           ImGuiDataType_U32,
                           &m_config.storage.hdd_read_threads_per_disk,
                           &stepOne);
        ImGui::InputScalar("单块 SSD 读取线程",
                           ImGuiDataType_U32,
                           &m_config.storage.ssd_read_threads_per_disk,
                           &stepOne);
        ImGui::InputScalar(
            "SHA-512 读取块 (KiB)", ImGuiDataType_U32, &m_config.io.read_block_kib, &stepOne);
        ImGui::InputScalar(
            "每盘队列容量", ImGuiDataType_U32, &m_config.io.per_disk_queue_capacity, &stepOne);
        ImGui::Checkbox("HDD 物理区间优化", &m_config.io.hdd_extent_optimization);
        ImGui::InputScalar("HDD 排序窗口", ImGuiDataType_U32, &m_config.io.hdd_sort_window, &stepOne);
        ImGui::InputScalar("原块重试次数", ImGuiDataType_U32, &m_config.io.normal_block_retries, &stepOne);
        ImGui::InputScalar("小块重试次数", ImGuiDataType_U32, &m_config.io.small_block_retries, &stepOne);
        ImGui::InputScalar("坏块小块 (KiB)", ImGuiDataType_U32, &m_config.io.small_block_kib, &stepOne);
        ImGui::InputScalar("无进展超时 (秒)",
                           ImGuiDataType_U32,
                           &m_config.io.no_progress_timeout_seconds,
                           &stepOne);
        ImGui::TextDisabled("坏块或超时只跳过当前文件并写日志，不熔断整块物理盘。");
        ImGui::TextDisabled("程序按运行时物理盘和介质自动分配；未知介质按 HDD 配置处理。" );
        ImGui::TextDisabled("智能模式每块物理盘从 1 线程开始；占用低于目标且有积压时每秒只增加 1。" );
    }

    if (ImGui::CollapsingHeader("视觉相似报告", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextDisabled("图片：PDQ 初筛 → 4×4 分区 pHash 二筛 → 灰度/边缘结构三筛；视频继续使用六帧 dHash。");
        ImGui::InputScalar("图片长宽比差异上限 (%)",
                           ImGuiDataType_U32,
                           &m_config.image_similarity.aspect_ratio_tolerance_percent,
                           &stepOne);
        ImGui::TextDisabled("标准质量阈值");
        ImGui::InputScalar("PDQ 最大汉明距离",
                           ImGuiDataType_U32,
                           &m_config.image_similarity.standard_profile.pdq_max_hamming_distance,
                           &stepOne);
        ImGui::InputScalar("水印回退 dHash 最大距离",
                           ImGuiDataType_U32,
                           &m_config.image_similarity.standard_profile.fallback_dhash_max_hamming_distance,
                           &stepOne);
        ImGui::InputScalar("PDQ 最低质量分",
                           ImGuiDataType_U32,
                           &m_config.image_similarity.pdq_min_quality,
                           &stepOne);
        ImGui::InputScalar("pHash 单区最大距离",
                           ImGuiDataType_U32,
                           &m_config.image_similarity.standard_profile.zoned_phash_tile_max_distance,
                           &stepOne);
        ImGui::InputScalar("pHash 最少通过分区",
                           ImGuiDataType_U32,
                           &m_config.image_similarity.standard_profile.zoned_phash_min_passing_tiles,
                           &stepOne);
        ImGui::InputScalar("pHash 最多忽略分区",
                           ImGuiDataType_U32,
                           &m_config.image_similarity.standard_profile.zoned_phash_max_ignored_tiles,
                           &stepOne);
        ImGui::InputScalar("pHash 裁剪均值上限",
                           ImGuiDataType_U32,
                           &m_config.image_similarity.standard_profile.zoned_phash_trimmed_mean_max,
                           &stepOne);
        ImGui::InputScalar("结构全局边缘最低分（百万分）",
                           ImGuiDataType_U32,
                           &m_config.image_similarity.standard_profile.structural_global_edge_min_millionths,
                           &stepOne);
        ImGui::InputScalar("结构块裁剪最低分（百万分）",
                           ImGuiDataType_U32,
                           &m_config.image_similarity.standard_profile.structural_trimmed_block_min_millionths,
                           &stepOne);
        ImGui::InputScalar("结构块通过阈值（百万分）",
                           ImGuiDataType_U32,
                           &m_config.image_similarity.standard_profile.structural_block_pass_score_millionths,
                           &stepOne);
        ImGui::InputScalar("结构通过块最低占比（百万分）",
                           ImGuiDataType_U32,
                           &m_config.image_similarity.standard_profile.structural_min_passing_percent_millionths,
                           &stepOne);
        ImGui::TextDisabled("低质量图片严格阈值（每项不得弱于标准配置）");
        ImGui::InputScalar("低质量 PDQ 最大距离", ImGuiDataType_U32,
                           &m_config.image_similarity.low_quality_profile.pdq_max_hamming_distance, &stepOne);
        ImGui::InputScalar("低质量水印回退 dHash 距离", ImGuiDataType_U32,
                           &m_config.image_similarity.low_quality_profile.fallback_dhash_max_hamming_distance,
                           &stepOne);
        ImGui::InputScalar("低质量 pHash 单区距离", ImGuiDataType_U32,
                           &m_config.image_similarity.low_quality_profile.zoned_phash_tile_max_distance, &stepOne);
        ImGui::InputScalar("低质量 pHash 最少通过区", ImGuiDataType_U32,
                           &m_config.image_similarity.low_quality_profile.zoned_phash_min_passing_tiles, &stepOne);
        ImGui::InputScalar("低质量 pHash 最多忽略区", ImGuiDataType_U32,
                           &m_config.image_similarity.low_quality_profile.zoned_phash_max_ignored_tiles, &stepOne);
        ImGui::InputScalar("低质量 pHash 裁剪均值", ImGuiDataType_U32,
                           &m_config.image_similarity.low_quality_profile.zoned_phash_trimmed_mean_max, &stepOne);
        ImGui::InputScalar("低质量结构全局最低分", ImGuiDataType_U32,
                           &m_config.image_similarity.low_quality_profile.structural_global_edge_min_millionths, &stepOne);
        ImGui::InputScalar("低质量结构裁剪最低分", ImGuiDataType_U32,
                           &m_config.image_similarity.low_quality_profile.structural_trimmed_block_min_millionths, &stepOne);
        ImGui::InputScalar("低质量结构块通过阈值", ImGuiDataType_U32,
                           &m_config.image_similarity.low_quality_profile.structural_block_pass_score_millionths, &stepOne);
        ImGui::InputScalar("低质量结构通过块占比", ImGuiDataType_U32,
                           &m_config.image_similarity.low_quality_profile.structural_min_passing_percent_millionths, &stepOne);
        ImGui::InputScalar("图片一/二筛线程数",
                           ImGuiDataType_U32,
                           &m_config.image_similarity.report_validation_worker_threads,
                           &stepOne);
        ImGui::InputScalar("图片结构三筛线程数",
                           ImGuiDataType_U32,
                           &m_config.image_similarity.structural_worker_threads,
                           &stepOne);
        ImGui::InputScalar("图片结构缓存 (MiB)",
                           ImGuiDataType_U32,
                           &m_config.image_similarity.structural_cache_mib,
                           &stepOne);
        ImGui::InputScalar("候选内存预算 (MiB)", ImGuiDataType_U32,
                           &m_config.image_similarity.candidate_memory_mib, &stepOne);
        ImGui::InputScalar("候选临时空间预算 (MiB)", ImGuiDataType_U32,
                           &m_config.image_similarity.candidate_temp_mib, &stepOne);
        ImGui::InputScalar("候选对上限", ImGuiDataType_U64,
                           &m_config.image_similarity.candidate_max_pairs, &stepOne64);
        ImGui::InputScalar("热门签名成员上限", ImGuiDataType_U32,
                           &m_config.image_similarity.hot_signature_max_members, &stepOne);
        ImGui::InputScalar("热门签名直接对上限", ImGuiDataType_U64,
                           &m_config.image_similarity.hot_signature_max_pairs, &stepOne64);
        ImGui::TextDisabled("无法完成特征或结构校验的资源会自动跳过并计入报告统计；跳过明细见报告信息的跳过内容面板。");
        ImGui::Checkbox("强制使用标量位计数（诊断）",
                        &m_config.image_similarity.force_scalar_kernels);
        ImGui::InputScalar("视频六帧平均汉明距离最大值",
                           ImGuiDataType_U32,
                           &m_config.dhash_similarity.video_max_average_hamming_distance,
                           &stepOne);
        ImGui::TextDisabled("图片默认仅容忍水印、分辨率和色调差异；不做旋转、裁剪、局部特征或语义相似搜索。");
        ImGui::TextDisabled("线程、阈值与缓存预算在报告启动时冻结，修改只影响下一次报告。");
    }

    if (ImGui::CollapsingHeader("重复报告选择安全上限", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::InputTextWithHint("图片选择距离上限（严格小于）",
                                 "留空=沿用报告生成阈值",
                                 m_imageSelectionDistanceBuf,
                                 sizeof(m_imageSelectionDistanceBuf));
        ImGui::InputTextWithHint("视频选择平均距离上限（严格小于）",
                                 "留空=沿用报告生成阈值",
                                 m_videoSelectionDistanceBuf,
                                 sizeof(m_videoSelectionDistanceBuf));
        ImGui::TextDisabled("图片三级报告已完成结构直验，不再叠加旧 dHash 选择上限；该图片项只用于旧报告。");
        ImGui::TextDisabled("距离等于输入值时不会选中，输入值和留空状态随 config.json 保存。");
    }

    if (ImGui::CollapsingHeader("MySQL 8.0+", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextDisabled("本地连接默认 127.0.0.1:3306，纯密码验证（已启用 allowPublicKeyRetrieval）。");
        ImGui::InputText("主机", m_mysqlHostBuf, sizeof(m_mysqlHostBuf));
        const std::uint16_t portStep = 1;
        ImGui::InputScalar("端口", ImGuiDataType_U16, &m_config.database.port, &portStep);
        ImGui::InputText("数据库", m_mysqlDatabaseBuf, sizeof(m_mysqlDatabaseBuf));
        ImGui::InputText("用户名", m_mysqlUserBuf, sizeof(m_mysqlUserBuf));
        ImGui::InputText("密码",
                         m_mysqlPasswordBuf,
                         sizeof(m_mysqlPasswordBuf),
                         ImGuiInputTextFlags_Password);
        if (m_config.database.password_decryption_failed) {
            ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.66f, 1.0f),
                               "原密码无法由当前 Windows 用户解密，请重新输入后保存。");
        }
        const char* tlsModes[] = {"Disabled", "Preferred", "Required", "Verify CA", "Verify Identity"};
        int tlsMode = static_cast<int>(m_config.database.tls_mode);
        if (ImGui::Combo("TLS 模式", &tlsMode, tlsModes, IM_ARRAYSIZE(tlsModes))) {
            m_config.database.tls_mode = static_cast<videosc::dedup::MySqlTlsMode>(tlsMode);
        }
        ImGui::TextDisabled("本地连接建议 Disabled；MySQL 8.0 默认 caching_sha2_password，非 SSL 下用公钥检索完成密码认证。");
        ImGui::InputText("TLS CA", m_mysqlCaBuf, sizeof(m_mysqlCaBuf));
        ImGui::InputText("TLS 证书", m_mysqlCertificateBuf, sizeof(m_mysqlCertificateBuf));
        ImGui::InputText("TLS 私钥", m_mysqlPrivateKeyBuf, sizeof(m_mysqlPrivateKeyBuf));
        ImGui::InputScalar(
            "连接池大小", ImGuiDataType_U32, &m_config.database.connection_pool_size, &stepOne);
        ImGui::InputScalar("连接超时 (秒)",
                           ImGuiDataType_U32,
                           &m_config.database.connect_timeout_seconds,
                           &stepOne);
        ImGui::InputScalar("命令超时 (秒)",
                           ImGuiDataType_U32,
                           &m_config.database.command_timeout_seconds,
                           &stepOne);
        ImGui::InputScalar("重试间隔 (秒)",
                           ImGuiDataType_U32,
                           &m_config.database.retry_interval_seconds,
                           &stepOne);
        ImGui::InputScalar(
            "同步批量", ImGuiDataType_U32, &m_config.database.sync_batch_size, &stepOne);
        ImGui::InputText("mysqldump", m_mysqldumpBuf, sizeof(m_mysqldumpBuf));
        ImGui::InputText("备份目录", m_backupDirectoryBuf, sizeof(m_backupDirectoryBuf));
        ImGui::Separator();
        ImGui::Text("程序模式版本：%u；程序数据版本：%u",
                    videosc::dedup::kCurrentMySqlSchemaVersion,
                    videosc::dedup::kCurrentDataVersion);
        const auto renderDataVersion = [](const char* storage,
                                          const std::optional<videosc::dedup::DataVersionRecord>& record) {
            if (!record.has_value()) {
                ImGui::Text("%s：未加载或未登记", storage);
                return;
            }
            ImGui::Text("%s：版本 %u；状态 %s；generation %llu",
                        storage,
                        record->data_version,
                        videosc::dedup::DataVersionCoordinator::StateName(record->state),
                        static_cast<unsigned long long>(record->generation_id));
        };
        renderDataVersion("RocksDB", m_rocksDataVersion);
        renderDataVersion("MySQL", m_mysqlDataVersion);
        const bool generationMatches = m_rocksDataVersion.has_value() && m_mysqlDataVersion.has_value() &&
                                       m_rocksDataVersion->generation_id == m_mysqlDataVersion->generation_id;
        ImGui::Text("generation 一致性：%s", generationMatches ? "一致" : "未确认");
        if (!m_dataVersionMessage.empty()) {
            const ImVec4 color = m_dataVersionMessageIsError
                                     ? ImVec4(0.95f, 0.55f, 0.66f, 1.0f)
                                     : ImVec4(0.65f, 0.89f, 0.63f, 1.0f);
            ImGui::TextColored(color, "%s", m_dataVersionMessage.c_str());
        }
        ImGui::BeginDisabled(m_databaseInitRunning.load() ||
                             (m_scanCoordinator && m_scanCoordinator->is_running()) ||
                             m_reportRunning.load() || m_reportCleanupRunning.load() ||
                             m_deletionRunning.load() || m_selectionRunning.load());
        if (ImGui::Button("初始化数据库表")) {
            StartDatabaseInitialization();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("已有表时先备份，再幂等建表并校验 schema 3；不会把空库登记为 ready，也不删除数据库。密码不进入命令行。");
        if (!m_databaseInitMessage.empty()) {
            const ImVec4 color = m_databaseInitMessageIsError
                                     ? ImVec4(0.95f, 0.55f, 0.66f, 1.0f)
                                     : ImVec4(0.65f, 0.89f, 0.63f, 1.0f);
            ImGui::TextColored(color, "%s", m_databaseInitMessage.c_str());
        }
    }

    if (ImGui::CollapsingHeader("Everything 文件发现", ImGuiTreeNodeFlags_DefaultOpen)) {
        const char* methods[] = {"Native (FindFirstFile)", "Everything (索引查询)"};
        int method = static_cast<int>(m_config.discovery.method);
        if (ImGui::Combo("发现方式", &method, methods, IM_ARRAYSIZE(methods))) {
            m_config.discovery.method = static_cast<videosc::dedup::DiscoveryMethod>(method);
        }
        ImGui::TextDisabled("Everything 批量查询文件列表，按物理盘分组，HDD 按电梯排序后统一计算。不可用时回退 Native。");
        ImGui::InputText("Everything64.dll", m_everythingDllBuf, sizeof(m_everythingDllBuf));
        ImGui::InputText("Everything.exe", m_everythingExeBuf, sizeof(m_everythingExeBuf));
        ImGui::InputScalar("索引分页数量",
                           ImGuiDataType_U32,
                           &m_config.discovery.query_page_size,
                           &stepOne);
        ImGui::InputScalar("启动超时 (秒)",
                           ImGuiDataType_U32,
                           &m_config.discovery.launch_timeout_seconds,
                           &stepOne);
        ImGui::InputScalar("数据库加载超时 (秒)",
                           ImGuiDataType_U32,
                           &m_config.discovery.db_load_timeout_seconds,
                           &stepOne);
        ImGui::InputScalar("就绪轮询间隔 (毫秒)",
                           ImGuiDataType_U32,
                           &m_config.discovery.poll_interval_milliseconds,
                           &stepOne);
        ImGui::TextDisabled("分页范围 128–100000；启动 1–600 秒；数据库 1–3600 秒；轮询 10–5000 毫秒。");
        ImGui::TextDisabled("正式扫描与诊断文件列表查询共用上述参数。");
    }

    if (ImGui::CollapsingHeader("缩略图生成与缓存", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::InputText("缩略图目录", m_thumbnailRootBuf, sizeof(m_thumbnailRootBuf));
        int thumbnailFormat = static_cast<int>(m_config.thumbnails.format);
        const char* formats[] = {"JPEG", "PNG"};
        if (ImGui::Combo("输出格式", &thumbnailFormat, formats, IM_ARRAYSIZE(formats))) {
            m_config.thumbnails.format = static_cast<videosc::dedup::ThumbnailFormat>(thumbnailFormat);
        }
        ImGui::InputScalar("视频拼图单格长边 (像素)",
                           ImGuiDataType_U32,
                           &m_config.thumbnails.video_cell_long_edge,
                           &stepOne);
        ImGui::InputScalar("图片预览长边 (像素)",
                           ImGuiDataType_U32,
                           &m_config.thumbnails.image_preview_long_edge,
                           &stepOne);
        ImGui::InputScalar(
            "缓存条目", ImGuiDataType_U32, &m_config.thumbnails.cache_entries, &stepOne);
        ImGui::InputScalar(
            "内存上限 (MiB)", ImGuiDataType_U32, &m_config.thumbnails.memory_limit_mib, &stepOne);
        ImGui::InputScalar("显存上限 (MiB)",
                           ImGuiDataType_U32,
                           &m_config.thumbnails.gpu_memory_limit_mib,
                           &stepOne);
        m_maxLongEdge = static_cast<int>(m_config.thumbnails.video_cell_long_edge);
        ImGui::TextDisabled("媒体相似度算法固定采样 6 帧并生成 2×3 拼图；SHA-512 算法固定不变。");
        ImGui::TextDisabled("尺寸或格式变化只触发缩略图按需重建，不改变哈希算法版本。");
    }

    if (ImGui::CollapsingHeader("RocksDB 与日志", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::InputText("RocksDB 目录", m_rocksDbDirectoryBuf, sizeof(m_rocksDbDirectoryBuf));
        ImGui::InputScalar(
            "RocksDB 块缓存 (MiB)", ImGuiDataType_U32, &m_config.rocksdb.block_cache_mib, &stepOne);
        ImGui::InputScalar(
            "RocksDB 写缓冲 (MiB)", ImGuiDataType_U32, &m_config.rocksdb.write_buffer_mib, &stepOne);
        ImGui::InputText("异常/诊断日志目录", m_logDirectoryBuf, sizeof(m_logDirectoryBuf));
        ImGui::InputText("执行日志目录",
                         m_executionLogDirectoryBuf,
                         sizeof(m_executionLogDirectoryBuf));
        ImGui::InputScalar(
            "单日志文件 (MiB)", ImGuiDataType_U32, &m_config.logging.rotate_file_mib, &stepOne);
        ImGui::InputScalar(
            "滚动文件数量", ImGuiDataType_U32, &m_config.logging.rotate_count, &stepOne);
        ImGui::InputScalar(
            "日志保留天数", ImGuiDataType_U32, &m_config.logging.retention_days, &stepOne);
    }

    if (ImGui::CollapsingHeader("诊断工具（本次运行）")) {
        ImGui::TextDisabled("以下字段只服务于现有单文件诊断页，不参与去重任务快照。");
        ImGui::Text("视频截图固定 6 帧；当前拼图单格长边：%d 像素", m_maxLongEdge);
        ImGui::PushItemWidth(140);
        ImGui::InputText("截图前缀##prefix", m_namePrefixBuf, sizeof(m_namePrefixBuf));
        ImGui::SameLine();
        ImGui::InputText("截图扩展名##ext", m_nameExtBuf, sizeof(m_nameExtBuf));
        ImGui::PopItemWidth();
        ImGui::InputTextWithHint("Everything64.dll##esdll",
                                 "留空使用程序目录默认值",
                                 m_everythingDllBuf,
                                 sizeof(m_everythingDllBuf));
        ImGui::InputTextWithHint("Everything.exe##esexe",
                                 "留空自动查找",
                                 m_everythingExeBuf,
                                 sizeof(m_everythingExeBuf));
    }

    if (ImGui::CollapsingHeader("关于")) {
        ImGui::TextDisabled("VideoSc - 多磁盘媒体去重工具");
        ImGui::TextDisabled("用户配置保存到安装目录 UTF-8 config.json；密码使用 DPAPI CurrentUser。");
        ImGui::TextDisabled("扫描状态、断点、索引和待同步操作保存到 RocksDB，不写入 JSON。");
    }

    ImGui::End();
}

// -----------------------------------------------------------------------------
// ProcessDeferredActions: button-triggered work that was deferred from the
// render loop. Runs once per frame after Render().
// -----------------------------------------------------------------------------

void VideoScApp::ProcessDeferredActions() {
    ConsumeDatabaseInitializationResult();
    RefreshRuntimeSnapshots();
    ConsumeReportResult();
    ConsumeReportCleanupResult();
    ConsumeSelectionResult();
    ConsumeDeletionResult();
    if (m_shutdownRequested) return;

    if (m_startScanRequested) {
        m_startScanRequested = false;
        StartScan();
    }
    if (m_resumeScanRequested) {
        m_resumeScanRequested = false;
        if (m_selectedResumableScan >= 0 &&
            static_cast<std::size_t>(m_selectedResumableScan) < m_resumableScans.size()) {
            StartScan(m_resumableScans[static_cast<std::size_t>(m_selectedResumableScan)].scan_id);
        }
    }
    if (m_cancelScanRequested) {
        m_cancelScanRequested = false;
        if (m_scanCoordinator) {
            m_scanCoordinator->Cancel();
            m_scanMessage = "已请求取消；正在进行的文件读取和 FFmpeg 任务将尽快退出。";
            m_scanMessageIsError = false;
        }
    }
    if (m_generateExactReportRequested) {
        m_generateExactReportRequested = false;
        StartReportGeneration(videosc::dedup::DuplicateReportKind::Exact, false);
    }
    if (m_generateSimilarReportRequested) {
        m_generateSimilarReportRequested = false;
        StartReportGeneration(videosc::dedup::DuplicateReportKind::Similar, false);
    }
    if (m_cancelReportRequested) {
        m_cancelReportRequested = false;
        m_reportCancelRequested.store(true);
        m_reportMessage = "已请求取消报告生成。";
        m_reportMessageIsError = false;
    }
    if (m_reportCleanupRequested) {
        m_reportCleanupRequested = false;
        StartReportCleanup(m_reportCleanupRequestedKind);
    }
    if (m_deleteConfirmed) {
        m_deleteConfirmed = false;
        StartDeletion();
    }
    if (m_loadReportRequested && !m_reportCleanupRunning.load()) LoadReport();

    if (m_scanSnapshot.local_scan_complete && m_scanSnapshot.shared_sync_complete &&
        m_scanSnapshot.scan_id != 0 && m_autoExactStartedForScanId != m_scanSnapshot.scan_id &&
        !m_reportRunning.load() && !m_reportCleanupRunning.load() &&
        !m_deletionRunning.load() && !m_selectionRunning.load()) {
        m_autoExactStartedForScanId = m_scanSnapshot.scan_id;
        StartReportGeneration(videosc::dedup::DuplicateReportKind::Exact, true);
    }

    // ------------- Run capture (deferred) -------------
    if (m_runCapture) {
        m_runCapture = false;

        FreeVideoScResult(&m_result);
        m_result.statusCode = -1;
        for (auto& t : m_thumbnails) m_retiredThumbnails.push_back(std::move(t));
        m_thumbnails.clear();
        m_shaBuf[0] = 0;
        m_dh1Buf[0] = 0;
        m_dh2Buf[0] = 0;
        m_hamming = -1;

        std::vector<std::string> nameStorage(m_screenshotCount);
        std::vector<const char*> names(m_screenshotCount);
        for (int i = 0; i < m_screenshotCount; ++i) {
            nameStorage[i] = std::string(m_namePrefixBuf)
                           + std::to_string(i + 1)
                           + std::string(m_nameExtBuf);
            names[i] = nameStorage[i].c_str();
        }

        if (m_outputDirBuf[0]) {
            std::error_code ec;
            std::filesystem::create_directories(m_outputDirBuf, ec);
        }

        m_result = CaptureVideoScreenshots(
            m_videoPathBuf,
            m_outputDirBuf,
            m_screenshotCount,
            m_maxLongEdge,
            names.data());

        if (m_result.statusCode == VIDEOSC_OK) {
            for (int i = 0; i < m_result.thumbnailCount; ++i) {
                ThumbnailTex t;
                t.pathUtf8 = m_result.thumbnailPaths[i];
                std::wstring wpath = Utf8ToWide(t.pathUtf8.c_str());
                t.srv = LoadImageToTexture(wpath, &t.width, &t.height);
                m_thumbnails.push_back(t);
            }
        }
    }

    // ------------- Run DiskInfo query (deferred) -------------
    if (m_runDiskQuery) {
        m_runDiskQuery = false;
        m_diskNumber = GetPhysicalDiskNumber(m_diskPathBuf);
    }

    // ------------- Run Everything SDK query (deferred) -------------
    if (m_runFileQuery) {
        m_runFileQuery = false;
        m_fileResultsFetched = false;
        m_fileResults.clear();

        // Parse paths (one per line, trim whitespace)
        std::vector<std::string> paths;
        {
            std::string s(m_pathsBuf);
            std::string line;
            for (char c : s) {
                if (c == '\r') continue;
                if (c == '\n') {
                    // trim
                    size_t a = line.find_first_not_of(" \t");
                    size_t b = line.find_last_not_of(" \t");
                    if (a != std::string::npos) {
                        paths.push_back(line.substr(a, b - a + 1));
                    }
                    line.clear();
                } else {
                    line.push_back(c);
                }
            }
            if (!line.empty()) {
                size_t a = line.find_first_not_of(" \t");
                size_t b = line.find_last_not_of(" \t");
                if (a != std::string::npos) {
                    paths.push_back(line.substr(a, b - a + 1));
                }
            }
        }

        // 与正式扫描共用 Everything 分页和就绪等待参数；路径仍允许诊断页当前编辑值覆盖。
        m_fileQuery.SetDiscoveryConfig(m_config.discovery);

        // Everything64.dll path: user-configured or default relative to exe dir
        if (m_everythingDllBuf[0]) {
            m_fileQuery.SetEverythingDllPath(Utf8ToWide(m_everythingDllBuf));
        } else {
            wchar_t exeDir[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
            std::wstring exePath(exeDir);
            size_t pos = exePath.find_last_of(L"\\/");
            std::wstring base = (pos != std::string::npos) ? exePath.substr(0, pos + 1) : L"";
            m_fileQuery.SetEverythingDllPath(base + L"third_party\\everything_sdk\\Everything64.dll");
        }

        // Everything.exe path (service): user-configured or auto-detect
        m_fileQuery.SetEverythingExePath(Utf8ToWide(m_everythingExeBuf));

        if (!paths.empty() && m_outputBuf[0]) {
            m_fileQuery.Start(paths, m_outputBuf);
        }
    }
}
