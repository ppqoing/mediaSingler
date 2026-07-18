// VideoScApp.h
//
// Application-level state and UI rendering for VideoScGUI.
// Extracts all GUI state and ImGui rendering logic out of wWinMain so that
// main.cpp only retains the Win32 + DirectX 11 backend layer.
//
// Lifetime: one VideoScApp instance is created after ImGui/D3D11 setup and
// destroyed before ImGui/D3D11 shutdown. Render() and ProcessDeferredActions()
// are called once per frame between ImGui::NewFrame() and ImGui::Render().

#pragma once

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "VideoSc.h"
#include "DiskInfo.h"
#include "EverythingFileListQuery.h"
#include "config/AppConfig.h"
#include "config/JsonConfigStore.h"
#include "dedup/DuplicateReportService.h"
#include "dedup/ReportSelectionStore.h"
#include "deletion/DeletionService.h"
#include "logging/RuntimeLogFeed.h"
#include "orchestration/ImageFeatureBackfillCoordinator.h"
#include "orchestration/ProgressSnapshot.h"
#include "persistence/DataVersionCoordinator.h"
#include "persistence/ScanCheckpointStore.h"
#include "persistence/MySqlSyncService.h"

// Forward declarations to avoid pulling D3D headers into the header
struct ID3D11Device;
struct ID3D11ShaderResourceView;

// ---------------------------------------------------------------------------
// ThumbnailTex: a loaded screenshot texture + its source path.
// ---------------------------------------------------------------------------
struct ThumbnailTex {
    std::string     pathUtf8;
    ID3D11ShaderResourceView* srv = nullptr;
    int             width  = 0;
    int             height = 0;
};

/** @brief 报告页按需加载的 GPU 缩略图；离开可见区域后及时释放。 */
struct ReportThumbnailTex {
    ID3D11ShaderResourceView* srv = nullptr;
    int width = 0;
    int height = 0;
    std::uint64_t last_used_frame = 0;
};

/** @brief 后台图片缩略图任务的完整结果；由 UI 线程收取并决定是否上传纹理。 */
struct ImagePreviewJobResult {
    std::filesystem::path output_path;
    std::filesystem::path source_path;
    int status_code = VIDEOSC_ERR_UNEXPECTED_FAILURE;
    std::uint32_t native_error = 0;
    std::string detail;
    bool failure_logged = true;
};

enum class ReportSortMode {
    Generated,
    ReclaimableAscending,
    ReclaimableDescending,
    MemberCountAscending,
    MemberCountDescending,
    DHashAverageAscending,
    DHashAverageDescending,
};

/** @brief 运行日志窗口的任务类别筛选。 */
enum class RuntimeLogTaskFilter {
    All,
    Scan,
    VisualCompute,
    MySql,
    Report,
    Deletion,
    Application,
};

/** @brief 主窗口关闭后的可观察安全收口阶段。 */
enum class ShutdownPhase {
    Running,
    CloseRequested,
    CancellingTasks,
    FinishingCurrentDeletion,
    StoppingSync,
    ClosingRocksDb,
    ReadyToDestroyWindow,
};

struct CachedReportGroup {
    videosc::dedup::DuplicateGroup group;
    std::uint64_t last_used_frame = 0;
};

namespace videosc::dedup {
class MySqlClient;
class MySqlSyncQueue;
class MySqlSyncService;
class RocksStore;
class ScanCoordinator;
}  // namespace videosc::dedup

// ---------------------------------------------------------------------------
// VideoScApp: holds all application state and renders the ImGui UI.
// ---------------------------------------------------------------------------
class VideoScApp {
public:
    VideoScApp();
    ~VideoScApp();

    // Called once per frame to render all UI windows.
    // Must be called between ImGui::NewFrame() and ImGui::Render().
    void Render();

    // Called once per frame to process deferred actions (button-triggered work).
    void ProcessDeferredActions();

    /** @brief 发布一次幂等的安全退出请求，不阻塞 GUI 线程。 */
    void RequestShutdown() noexcept;

    /** @brief 每帧推进后台任务收口；仅在所有任务结束后关闭 RocksDB。 */
    void AdvanceShutdown();

    /** @brief 是否已经收到主窗口关闭请求。 */
    bool IsShutdownRequested() const noexcept {
        return m_shutdownRequested.load(std::memory_order_relaxed);
    }

    /** @brief 所有业务资源已经安全收口、允许销毁 Win32 窗口时返回 true。 */
    bool IsShutdownComplete() const noexcept {
        return m_shutdownPhase == ShutdownPhase::ReadyToDestroyWindow;
    }

    // Inject the D3D11 device (owned by RenderBackend) so LoadImageToTexture
    // can create textures. Must be called before Render().
    void SetD3DDevice(ID3D11Device* dev) { m_d3dDevice = dev; }

private:
    // --- UI rendering methods ---
    void RenderDockSpace();
    void RenderScreenshotWindow();
    void RenderResultsWindow();
    void RenderScanWindow();
    void RenderScanPathsWindow();
    /** @brief 绘制报告类型、排序、筛选、批量选择和删除等控制区域。 */
    void RenderDuplicateReportWindow();
    /** @brief 绘制独立的虚拟化重复组列表，并复用当前报告共享状态。 */
    void RenderDuplicateGroupBrowserWindow();
    /** @brief 绘制主窗口仍可见期间的安全退出遮罩和活动任务状态。 */
    void RenderShutdownOverlay();
    /** @brief 显示当前选中重复组的缩略图、质量参数和相似度证据。 */
    void RenderDuplicateReportDetailWindow();
    void RenderDiskInfoWindow();
    void RenderFileSearchWindow();
    void RenderSettingsWindow();
    /** @brief 绘制可停靠运行日志窗口，并增量消费后台结构化日志。 */
    void RenderLogWindow();

    /** @brief 从进程实时日志源增量更新本地有界视图缓存。 */
    void RefreshRuntimeLogs();

    /** @brief 显示上次异常退出提示，并允许打开或忽略诊断记录。 */
    void RenderPreviousCrashPopup();

    /** @brief 把当前已加载配置同步到 ImGui 文本编辑缓冲区。 */
    void LoadConfigIntoEditors();

    /** @brief 把 ImGui 编辑值写回配置模型，不执行文件 I/O。 */
    void UpdateConfigFromEditors();

    /** @brief 校验、DPAPI 加密并原子保存安装目录 config.json。 */
    void SaveConfiguration();

    /** @brief 丢弃未保存编辑并重新加载主配置或备份。 */
    void ReloadConfiguration();

    /** @brief 使用页面当前配置在后台连接 MySQL 并执行幂等初始化。 */
    void StartDatabaseInitialization();

    /** @brief 收取后台初始化结果，禁止后台线程直接修改 ImGui 状态。 */
    void ConsumeDatabaseInitializationResult();

    /** @brief 打开 RocksDB、持久化同步队列并启动 MySQL 后台补同步服务。 */
    bool EnsureRuntime(std::string& error);

    /** @brief 按安全生命周期顺序停止任务、同步线程并关闭 RocksDB。 */
    void ShutdownRuntime();

    /** @brief 使用当前配置启动新扫描或恢复指定检查点。 */
    void StartScan(std::optional<std::uint64_t> resume_scan_id = std::nullopt);

    /** @brief 刷新扫描器与 MySQL 补同步的 GUI 快照。 */
    void RefreshRuntimeSnapshots();

    /** @brief 正式队列清空后收尾历史已完成扫描，删除其磁盘清单并关闭检查点。 */
    void FinalizeHistoricalSynchronizedScans();

    /** @brief 在同 generation 全量扫描和 MySQL 同步完成后提交双端 ready。 */
    bool TryMarkDataVersionReady(std::string& error);

    /** @brief 统一校验自动/手动报告所需的数据版本、队列、连接与 schema 契约。 */
    bool CheckReportReadiness(std::string& error);

    /** @brief 把协调器结果保存为 GUI 可观察状态。 */
    void ApplyDataVersionResult(const videosc::dedup::DataVersionResult& result);

    /** @brief 从 RocksDB 读取有限数量的可恢复检查点。 */
    void RefreshResumableScans();

    /** @brief 后台生成精确或相似报告，禁止阻塞 ImGui 帧循环。 */
    void StartReportGeneration(videosc::dedup::DuplicateReportKind kind, bool automatic);

    /** @brief 收取报告线程结果并加载已发布报告摘要。 */
    void ConsumeReportResult();

    /** @brief 从 RocksDB 加载当前报告的全部轻量摘要。 */
    void LoadReport();

    /** @brief 清空当前已加载报告的 GUI 缓存和详情状态，不修改持久化数据。 */
    void ClearLoadedReport();

    /**
     * @brief 后台删除指定类型的全部重复报告数据。
     * @param kind 要清理的固定报告类型。
     */
    void StartReportCleanup(videosc::dedup::DuplicateReportKind kind);

    /** @brief 收取报告清理线程结果，并在需要时清空当前可见报告。 */
    void ConsumeReportCleanupResult();

    void ApplyReportSort();
    void RebuildReportRowStarts();
    videosc::dedup::DuplicateGroup* AcquireReportGroup(std::uint64_t ordinal);
    void TrimReportGroupCache();

    /** @brief 按当前保留策略对当前选中组或整个报告建立删除选择。 */
    void ApplyDeletionSelection(bool entire_report);

    /** @brief 收取后台全报告选择结果并发布新的选择快照。 */
    void ConsumeSelectionResult();

    /** @brief 从 RocksDB 重新加载当前报告的持久选择汇总。 */
    void RefreshReportSelectionSnapshot();

    /**
     * @brief 切换组内成员的待删除状态，并保证至少保留一个成员。
     * @param group 当前报告页中可修改的重复组。
     * @param path_id 被点击成员的路径 ID。
     */
    void ToggleReportMemberDeletion(videosc::dedup::DuplicateGroup& group, std::uint64_t path_id);

    /** @brief 后台执行已经确认的永久删除批次。 */
    void StartDeletion();

    /** @brief 收取永久删除线程的汇总结果。 */
    void ConsumeDeletionResult();

    /** @brief 仅在报告项实际展开时加载纹理。 */
    ReportThumbnailTex* AcquireReportThumbnail(
        const std::filesystem::path& path,
        videosc::dedup::MediaKind media_kind = videosc::dedup::MediaKind::Other);

    /** @brief 释放离开可见范围或超过配置上限的报告纹理。 */
    void TrimReportThumbnails();

    /** @brief 立即释放所有报告纹理，例如换页或关闭运行时。 */
    void ClearReportThumbnails();
    void InitializeReportPreviewDirectory();
    void CleanupReportPreviewDirectory() noexcept;
    std::filesystem::path AcquireVideoContactSheet(const videosc::dedup::DuplicateMember& member);
    /**
     * @brief 为图片成员请求或复用后台临时缩略图。
     * @param member 当前图片成员。
     * @param pending 返回是否仍在排队或生成。
     * @param failure 返回已确认的失败原因。
     * @return 已生成的临时缩略图路径；尚未完成或失败时为空。
     */
    std::filesystem::path AcquireImagePreview(
        const videosc::dedup::DuplicateMember& member,
        bool& pending,
        std::string& failure);

    /** @brief 当前组切换时一次性建立证据表文件名索引和平均距离。 */
    void PrepareReportDetailIndexes(const videosc::dedup::DuplicateGroup& group);

    // --- Static helpers ---
    static void TextCopyable(const char* label, const char* text);
    static void TextCopyable(const char* label, const std::string& text);
    /**
     * @brief 在当前表格单元格内绘制不换行文本，内容被裁剪时提供完整悬停提示。
     * @param text 要显示的 UTF-8 文本；空指针按空字符串处理。
     */
    static void TableTextSingleLine(const char* text);

    /**
     * @brief `std::string` 版本的单行表格文本绘制。
     * @param text 要显示的 UTF-8 文本。
     */
    static void TableTextSingleLine(const std::string& text);
    static bool OpenFileDialog(wchar_t* outBuf, size_t outBufLen,
                               const wchar_t* filter, bool folderPicker);
    ID3D11ShaderResourceView* LoadImageToTexture(const std::wstring& path,
                                                 int* outW,
                                                 int* outH,
                                                 const std::filesystem::path* scaled_output = nullptr);

    // --- D3D11 device (injected, not owned) ---
    ID3D11Device* m_d3dDevice = nullptr;

    // --- Screenshot state ---
    char  m_videoPathBuf[1024] = "";
    char  m_outputDirBuf[1024] = "";
    int   m_screenshotCount    = 6;
    int   m_maxLongEdge        = 256;
    char  m_namePrefixBuf[64]  = "shot_";
    char  m_nameExtBuf[16]     = ".jpg";
    VideoScResult m_result     = {};
    std::vector<ThumbnailTex>  m_thumbnails;
    std::vector<ThumbnailTex>  m_retiredThumbnails;
    char  m_shaBuf[129]        = {};
    char  m_dh1Buf[17]         = {};
    char  m_dh2Buf[17]         = {};
    int   m_hamming            = -1;
    bool  m_runCapture         = false;

    // --- Disk info state ---
    char  m_diskPathBuf[1024]  = "C:\\";
    int   m_diskNumber         = -2;
    bool  m_runDiskQuery       = false;

    // --- File search state ---
    EverythingFileListQuery m_fileQuery;
    char  m_pathsBuf[16384]       = "D:\\\nE:\\";
    char  m_outputBuf[1024]       = "file_output";
    char  m_everythingDllBuf[1024] = "";
    char  m_everythingExeBuf[1024] = "";
    bool  m_runFileQuery          = false;
    std::vector<EverythingFileListQuery::DiskResult> m_fileResults;
    bool  m_fileResultsFetched    = false;

    // --- Function window visibility ---
    /** @brief 普通功能窗口的可见状态；原生辅助窗口关闭按钮通过这些字段隐藏窗口。 */
    bool m_showScanWindow = true;
    bool m_showScanPathsWindow = true;
    bool m_showDuplicateReportWindow = true;
    /** @brief 独立重复组浏览窗口的可见状态；关闭后停止加载可见组。 */
    bool m_showDuplicateGroupBrowserWindow = true;
    /** @brief 底部运行日志窗口可见状态。 */
    bool m_showLogWindow = true;
    bool m_showSettingsWindow = true;
    bool m_showScreenshotWindow = false;
    bool m_showResultsWindow = false;
    bool m_showDiskInfoWindow = false;
    bool m_showFileSearchWindow = false;
    /** @brief 设置页提交的默认 Dock 布局重建请求，由下一帧 RenderDockSpace 消费。 */
    bool  m_resetDockLayoutRequested = false;
    /** @brief 启动时检测到的未确认崩溃元数据路径。 */
    std::filesystem::path m_previousCrashMetadata;
    /** @brief 只在 ImGui Context 可用后提交一次上次崩溃弹窗。 */
    bool m_previousCrashPopupRequested = false;

    // --- Bounded runtime log viewer ---
    /** @brief GUI 当前持有的有界日志条目；聚合更新按 entry_id 原位替换。 */
    std::vector<videosc::dedup::RuntimeLogEntry> m_runtimeLogEntries;
    std::unordered_map<std::uint64_t, std::size_t> m_runtimeLogEntryIndices;
    std::uint64_t m_runtimeLogCursor = 0;
    std::uint64_t m_runtimeLogDroppedEntries = 0;
    std::optional<std::uint64_t> m_selectedRuntimeLogEntryId;
    RuntimeLogTaskFilter m_runtimeLogTaskFilter = RuntimeLogTaskFilter::All;
    char m_runtimeLogSearchBuf[256] = {};
    bool m_runtimeLogShowInfo = true;
    bool m_runtimeLogShowWarning = true;
    bool m_runtimeLogShowError = true;
    bool m_runtimeLogPaused = false;
    bool m_runtimeLogAutoScroll = true;
    bool m_runtimeLogWasAtBottom = true;
    bool m_runtimeLogHasUpdates = false;

    /** @brief 固定指向安装目录 config.json 的配置存储。 */
    videosc::dedup::JsonConfigStore m_configStore;

    /** @brief GUI 当前编辑的完整配置模型。 */
    videosc::dedup::AppConfig m_config;

    /** @brief 扫描目录编辑列表；顺序同时表示保留优先级。 */
    std::vector<std::string> m_scanPathEditors;

    /** @brief 最近一次加载或保存的中文状态消息。 */
    std::string m_configMessage;
    bool m_configMessageIsError = false;
    bool m_configCanSave = true;

    char m_mysqlHostBuf[256] = {};
    char m_mysqlDatabaseBuf[256] = {};
    char m_mysqlUserBuf[256] = {};
    char m_mysqlPasswordBuf[512] = {};
    char m_mysqlCaBuf[1024] = {};
    char m_mysqlCertificateBuf[1024] = {};
    char m_mysqlPrivateKeyBuf[1024] = {};
    char m_mysqldumpBuf[1024] = {};
    char m_backupDirectoryBuf[1024] = {};
    char m_thumbnailRootBuf[1024] = {};
    char m_rocksDbDirectoryBuf[1024] = {};
    char m_logDirectoryBuf[1024] = {};
    /** 独立执行轨迹和失败记录目录编辑缓冲。 */
    char m_executionLogDirectoryBuf[1024] = {};
    char m_imageSelectionDistanceBuf[32] = {};
    char m_videoSelectionDistanceBuf[32] = {};

    /** @brief MySQL 初始化只在后台线程执行，避免网络超时阻塞 UI。 */
    std::thread m_databaseInitThread;
    std::atomic<bool> m_databaseInitRunning{false};
    std::mutex m_databaseInitResultMutex;
    bool m_databaseInitResultReady = false;
    bool m_databaseInitResultSucceeded = false;
    std::string m_databaseInitResult;
    std::string m_databaseInitMessage;
    bool m_databaseInitMessageIsError = false;

    // --- Runtime scan and synchronization ---
    std::unique_ptr<videosc::dedup::RocksStore> m_runtimeStore;
    std::unique_ptr<videosc::dedup::MySqlSyncQueue> m_syncQueue;
    std::unique_ptr<videosc::dedup::MySqlClient> m_mysqlClient;
    std::unique_ptr<videosc::dedup::MySqlSyncService> m_syncService;
    std::unique_ptr<videosc::dedup::ScanCoordinator> m_scanCoordinator;
    bool m_runtimeConfigurationStale = false;
    videosc::dedup::ProgressSnapshot m_scanSnapshot;
    videosc::dedup::MySqlSyncSnapshot m_syncSnapshot;
    std::optional<videosc::dedup::DataVersionRecord> m_rocksDataVersion;
    std::optional<videosc::dedup::DataVersionRecord> m_mysqlDataVersion;
    videosc::dedup::DataVersionDecision m_dataVersionDecision =
        videosc::dedup::DataVersionDecision::ResetRequired;
    std::string m_dataVersionMessage;
    bool m_dataVersionMessageIsError = false;
    std::vector<videosc::dedup::ScanCheckpoint> m_resumableScans;
    int m_selectedResumableScan = -1;
    bool m_startScanRequested = false;
    bool m_resumeScanRequested = false;
    bool m_cancelScanRequested = false;
    /** @brief 新扫描是否在精确报告后继续自动生成 MySQL 全量 dHash 报告。 */
    bool m_generateSimilarAfterScan = false;
    std::string m_scanMessage;
    bool m_scanMessageIsError = false;
    /** @brief 最后一次渲染的扫描 ID，用于在新任务开始时清空旧进度。 */
    std::uint64_t m_lastScanProgressId = 0;
    /** @brief 最近一次可确定计算的阶段进度；取消或失败后用于保留现场。 */
    float m_lastScanProgressFraction = 0.0f;
    /** @brief 最近一次活动阶段；阶段切换时用于重置阶段内百分比。 */
    videosc::dedup::ScanPhase m_lastScanProgressPhase = videosc::dedup::ScanPhase::Idle;
    /** @brief 已弹窗显示过的 discovery_warning 文本，用于避免同一警告重复弹窗。 */
    std::wstring m_shownDiscoveryWarning;
    /** @brief 待显示的 discovery_warning 弹窗请求标志，每帧检查直到弹窗显示。 */
    bool m_discoveryWarningPopupPending = false;
    std::uint64_t m_autoExactStartedForScanId = 0;

    // --- Persistent duplicate reports ---
    std::thread m_reportThread;
    std::atomic_bool m_reportRunning{false};
    std::atomic_bool m_reportCancelRequested{false};
    std::mutex m_reportResultMutex;
    videosc::dedup::DuplicateReportProgress m_reportProgress;
    /** @brief 当前视觉报告前图片特征回填的详细进度，与报告结果共用同一互斥锁。 */
    videosc::dedup::ImageFeatureBackfillProgress m_imageFeatureBackfillProgress;
    videosc::dedup::DuplicateReportResult m_reportResult;
    bool m_reportResultReady = false;
    bool m_reportWasAutomatic = false;
    /** @brief dHash 报告最近一次确定型阶段进度，取消时保留现场。 */
    float m_lastReportProgressFraction = 0.0f;
    /** @brief dHash 报告最近一次不确定动画参数，取消后固定动画色块位置。 */
    float m_lastReportIndeterminateProgress = -1.0f;
    videosc::dedup::DuplicateReportKind m_visibleReportKind = videosc::dedup::DuplicateReportKind::Exact;
    std::uint64_t m_visibleReportGeneration = 0;
    std::uint64_t m_visibleReportGroupCount = 0;
    /** @brief 当前已加载 dHash 报告的冻结规则；缺失时拒绝把旧报告解释为当前算法结果。 */
    std::optional<videosc::dedup::SimilarReportMetadata> m_visibleSimilarReportMetadata;
    /** @brief 当前可见报告是否具备删除可信证据；Exact 恒 true，Similar 按元数据判定。 */
    bool m_visibleReportTrusted = true;
    /** @brief 用户在删除确认弹窗勾选的对旧规则报告强制删除。 */
    bool m_deleteOverrideUntrusted = false;
    /** @brief 当前相似报告跳过内容统计与前 512 条明细。 */
    videosc::dedup::SkippedVisualContentStats m_visibleSkippedStats;
    std::vector<videosc::dedup::SkippedVisualContentRecord> m_visibleSkippedContents;
    ReportSortMode m_reportSortMode = ReportSortMode::Generated;
    std::vector<videosc::dedup::ReportGroupSummary> m_reportSummaries;
    std::vector<std::uint64_t> m_reportRowStarts;
    std::unordered_map<std::uint64_t, CachedReportGroup> m_reportGroupCache;
    videosc::dedup::ReportSelectionSnapshot m_reportSelectionSnapshot;
    bool m_loadReportRequested = false;
    bool m_generateExactReportRequested = false;
    bool m_generateSimilarReportRequested = false;
    bool m_cancelReportRequested = false;
    /** @brief 报告删除确认弹窗中固定展示的类型，避免受页签切换影响。 */
    videosc::dedup::DuplicateReportKind m_reportCleanupConfirmationKind =
        videosc::dedup::DuplicateReportKind::Exact;
    bool m_openReportCleanupConfirmation = false;
    videosc::dedup::DuplicateReportKind m_reportCleanupRequestedKind =
        videosc::dedup::DuplicateReportKind::Exact;
    bool m_reportCleanupRequested = false;
    std::thread m_reportCleanupThread;
    std::atomic_bool m_reportCleanupRunning{false};
    std::mutex m_reportCleanupResultMutex;
    bool m_reportCleanupResultReady = false;
    bool m_reportCleanupResultSucceeded = false;
    videosc::dedup::DuplicateReportKind m_reportCleanupResultKind =
        videosc::dedup::DuplicateReportKind::Exact;
    std::string m_reportCleanupResultMessage;
    std::string m_reportMessage;
    bool m_reportMessageIsError = false;
    std::unordered_map<std::wstring, ReportThumbnailTex> m_reportThumbnails;
    std::unordered_set<std::wstring> m_reportThumbnailFailures;
    std::unordered_map<std::wstring, std::filesystem::path> m_reportImagePreviewFiles;
    std::unordered_map<std::wstring, std::future<ImagePreviewJobResult>> m_imagePreviewJobs;
    std::unordered_map<std::wstring, std::string> m_imagePreviewFailures;
    std::unordered_map<std::wstring, std::filesystem::path> m_generatedVideoPreviewFiles;
    std::unordered_map<std::wstring, std::future<std::filesystem::path>> m_videoPreviewJobs;
    std::filesystem::path m_reportPreviewDirectory;
    std::uint64_t m_reportTextureFrame = 0;
    std::uint32_t m_reportTextureUploadsThisFrame = 0;
    bool m_resetReportScroll = false;
    bool m_clearReportThumbnailsRequested = false;
    /** @brief 当前主列表高亮并由详情窗口展示的组 ID；换页或换报告时清空。 */
    std::optional<std::uint64_t> m_selectedReportGroupId;
    /** @brief 当前组在报告 generation 中的固定序号，用于绕过虚拟列表缓存重新加载。 */
    std::optional<std::uint64_t> m_selectedReportGroupOrdinal;
    std::optional<videosc::dedup::DuplicateGroup> m_selectedReportGroup;
    std::optional<std::uint64_t> m_reportDetailIndexGroupId;
    std::unordered_map<std::uint64_t, std::string> m_reportDetailMemberNames;
    double m_reportDetailAverageDistance = 0.0;
    /** @brief 当前详情组的跨严格组图片关系总数和按需加载缓存。 */
    std::uint64_t m_reportDetailImageRelationCount = 0;
    std::unordered_map<std::uint64_t, videosc::dedup::SimilarImageRelationSummary>
        m_reportDetailImageRelationCache;
    std::optional<std::uint64_t> m_selectedImageRelationOrdinal;
    std::optional<videosc::dedup::SimilarImageRelationSummary> m_selectedImageRelation;
    /** @brief 当前展开的跨组关系对方签名成员，键为签名内绝对序号。 */
    std::uint64_t m_selectedImageRelationMemberCount = 0;
    std::unordered_map<std::uint64_t, videosc::dedup::DuplicateMember>
        m_selectedImageRelationMemberCache;
    std::string m_reportDetailRelationLoadError;
    /** @brief 同一主程序内可停靠的重复组详情窗口显示状态。 */
    bool m_showReportDetailWindow = false;

    // --- Selection and permanent deletion ---
    /** @brief 非互斥保留策略快照；默认仅保留最高质量。 */
    videosc::dedup::RetentionPolicySet m_retentionPolicies;
    char m_deleteTargetStorageBuf[256] = {};
    bool m_selectionCoversEntireReport = false;
    std::thread m_selectionThread;
    std::atomic_bool m_selectionRunning{false};
    std::atomic_bool m_selectionCancelRequested{false};
    std::atomic<std::uint64_t> m_selectionProcessedGroups{0};
    std::atomic<std::uint64_t> m_selectionTotalGroups{0};
    std::mutex m_selectionResultMutex;
    bool m_selectionResultReady = false;
    bool m_selectionResultSucceeded = false;
    std::string m_selectionResultMessage;
    videosc::dedup::ReportSelectionSnapshot m_selectionResultSnapshot;
    bool m_openDeleteConfirmation = false;
    bool m_deleteConfirmed = false;
    std::thread m_deletionThread;
    std::atomic_bool m_deletionRunning{false};
    /** @brief 安全退出时只阻止领取下一个文件，不中断当前文件一致性边界。 */
    std::atomic_bool m_deletionStopRequested{false};
    std::mutex m_deletionResultMutex;
    bool m_deletionResultReady = false;
    std::uint64_t m_deletedFiles = 0;
    std::uint64_t m_reclaimedBytes = 0;
    std::uint64_t m_failedDeletions = 0;
    std::atomic<std::uint64_t> m_deletionTotalFiles{0};
    std::atomic<std::uint64_t> m_deletionProcessedFiles{0};
    std::atomic<std::uint64_t> m_deletionProcessedBytes{0};
    std::atomic<std::uint64_t> m_deletionProgressReclaimedBytes{0};
    std::atomic<std::uint64_t> m_deletionProgressSucceeded{0};
    std::atomic<std::uint64_t> m_deletionProgressFailed{0};
    std::mutex m_deletionProgressMutex;
    std::string m_deletionProgressStage;
    std::filesystem::path m_deletionCurrentPath;
    std::string m_deletionResultMessage;
    std::string m_deletionMessage;
    bool m_deletionMessageIsError = false;

    // --- Safe application shutdown ---
    std::atomic_bool m_shutdownRequested{false};
    ShutdownPhase m_shutdownPhase = ShutdownPhase::Running;
    std::atomic_bool m_previewCancelRequested{false};
    std::string m_shutdownMessage;
};
