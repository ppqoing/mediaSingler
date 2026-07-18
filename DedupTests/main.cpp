#include "config/ConfigValidator.h"
#include "config/DpapiSecretProtector.h"
#include "config/JsonConfigStore.h"
#include "concurrency/TaskThreadPool.h"
#include "dedup/ExactDuplicateReader.h"
#include "dedup/DHashSimilarity.h"
#include "dedup/ImageSimilarityRules.h"
#include "dedup/PdqCandidateBuilder.h"
#include "dedup/DuplicateReportService.h"
#include "dedup/ReportSelectionStore.h"
#include "dedup/StructuralVerificationCache.h"
#include "dedup/StructuralVerificationScheduler.h"
#include "discovery/NativeFileDiscovery.h"
#include "deletion/DeletionService.h"
#include "models/CoreModels.h"
#include "models/CoreModelCodec.h"
#include "logging/ScanErrorLogger.h"
#include "logging/ApplicationErrorLogger.h"
#include "logging/ExecutionLogger.h"
#include "logging/RuntimeLogFeed.h"
#include "orchestration/ScanOptions.h"
#include "orchestration/ScanOptionsCodec.h"
#include "orchestration/ScanCoordinator.h"
#include "orchestration/ImageFeatureBackfillCoordinator.h"
#include "persistence/RocksStore.h"
#include "persistence/DataVersionCoordinator.h"
#include "persistence/ImageFeatureBackfillCheckpointStore.h"
#include "persistence/ScanCheckpointStore.h"
#include "persistence/MySqlClient.h"
#include "persistence/MySqlSchema.h"
#include "persistence/SyncOperation.h"
#include "scheduling/DiskHashScheduler.h"
#include "VideoSc.h"
#include "DiskInfo.h"

#include <Windows.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <algorithm>
#include <array>
#include <fstream>
#include <functional>
#include <iostream>
#include <atomic>
#include <chrono>
#include <cwctype>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

using videosc::dedup::AppConfig;
using videosc::dedup::ApplicationErrorLogger;
using videosc::dedup::ApplicationErrorRecord;
using videosc::dedup::ConfigLoadStatus;
using videosc::dedup::ConfigValidator;
using videosc::dedup::CoreModelCodec;
using videosc::dedup::DiskChannelOptions;
using videosc::dedup::DiskHashScheduler;
using videosc::dedup::DpapiSecretProtector;
using videosc::dedup::DuplicateMember;
using videosc::dedup::DuplicateReportGenerator;
using videosc::dedup::DuplicateReportKind;
using videosc::dedup::DuplicateReportStore;
using videosc::dedup::ReportSelectionStore;
using videosc::dedup::ReportSelectionMember;
using videosc::dedup::ReportSelectionRules;
using videosc::dedup::ReportSelectionSnapshot;
using videosc::dedup::DHashCandidateIndex;
using videosc::dedup::DHashRules;
using videosc::dedup::DeletionExecutor;
using videosc::dedup::DeletionPlanner;
using videosc::dedup::DiscoveryRoot;
using videosc::dedup::DiscoveryBackend;
using videosc::dedup::DiscoveryMethod;
using videosc::dedup::DiscoveryRootPhase;
using videosc::dedup::ExactGroupAccumulator;
using videosc::dedup::ExecutionEventRecord;
using videosc::dedup::ExecutionFailureRecord;
using videosc::dedup::ExecutionLogger;
using videosc::dedup::RuntimeLogEntry;
using videosc::dedup::RuntimeLogFeed;
using videosc::dedup::RuntimeLogSeverity;
using videosc::dedup::FileHashJob;
using videosc::dedup::FileHashOutcome;
using videosc::dedup::FileHashResult;
using videosc::dedup::FileHashStatus;
using videosc::dedup::FilePathRecord;
using videosc::dedup::IFileHasher;
using videosc::dedup::JsonConfigStore;
using videosc::dedup::KeepPolicy;
using videosc::dedup::MySqlClient;
using videosc::dedup::MySqlSchema;
using videosc::dedup::MySqlSyncQueue;
using videosc::dedup::DataVersionCoordinator;
using videosc::dedup::DataVersionDecision;
using videosc::dedup::DataVersionRecord;
using videosc::dedup::DataVersionState;
using videosc::dedup::RocksColumnFamily;
using videosc::dedup::RocksDbConfig;
using videosc::dedup::RocksMutation;
using videosc::dedup::RocksStore;
using videosc::dedup::NativeFileDiscovery;
using videosc::dedup::OperationLogger;
using videosc::dedup::ScanOptions;
using videosc::dedup::ScanOptionsCodec;
using videosc::dedup::ScanCheckpoint;
using videosc::dedup::ScanCoordinator;
using videosc::dedup::ScanErrorLogger;
using videosc::dedup::TaskThreadPool;
using videosc::dedup::ScanCheckpointStore;
using videosc::dedup::ScanPhase;
using videosc::dedup::Sha512Digest;
using videosc::dedup::ShaFileData;
using videosc::dedup::Sha512FromHex;
using videosc::dedup::Sha512ToHex;
using videosc::dedup::StorageMediaType;
using videosc::dedup::SyncOperation;
using videosc::dedup::SyncOperationKind;

/** @brief 单个无外部框架测试用例。 */
struct TestCase {
    const char* name;
    std::function<void()> run;
};

/** @brief 条件不成立时终止当前测试并输出调用语义。 */
void Require(const bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

/** @brief 创建进程唯一临时目录，避免测试读写安装目录。 */
std::filesystem::path CreateTestDirectory(const wchar_t* case_name) {
    wchar_t temporary_root[MAX_PATH]{};
    const DWORD length = GetTempPathW(MAX_PATH, temporary_root);
    if (length == 0 || length >= MAX_PATH) {
        throw std::runtime_error("GetTempPathW failed");
    }
    const std::filesystem::path directory =
        std::filesystem::path(temporary_root) /
        (std::wstring(L"VideoSc-") + case_name + L"-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(directory);
    return directory;
}

/** @brief 严格读取小型测试文件。 */
std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Cannot read test file");
    }
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

/** @brief 覆盖小型测试文件，用于模拟配置损坏和未来版本。 */
void WriteFile(const std::filesystem::path& path, const std::string& content) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!stream) {
        throw std::runtime_error("Cannot write test file");
    }
}

/** @brief 测试夹具路径使用严格 UTF-8 传给 DLL。 */
std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8,
                                           WC_ERR_INVALID_CHARS,
                                           value.data(),
                                           static_cast<int>(value.size()),
                                           nullptr,
                                           0,
                                           nullptr,
                                           nullptr);
    Require(length > 0, "Cannot encode fixture path as UTF-8");
    std::string result(static_cast<std::size_t>(length), '\0');
    Require(WideCharToMultiByte(CP_UTF8,
                                WC_ERR_INVALID_CHARS,
                                value.data(),
                                static_cast<int>(value.size()),
                                result.data(),
                                length,
                                nullptr,
                                nullptr) == length,
            "Cannot encode fixture path as UTF-8");
    return result;
}

/** @brief 直接启动本地测试工具并等待完成，不经过 cmd.exe 拼接命令。 */
void RunProcess(const std::filesystem::path& executable,
                const std::wstring& arguments,
                const std::uint32_t timeoutMilliseconds) {
    std::wstring commandLine = L"\"" + executable.wstring() + L"\" " + arguments;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    Require(CreateProcessW(executable.c_str(),
                           commandLine.data(),
                           nullptr,
                           nullptr,
                           FALSE,
                           CREATE_NO_WINDOW,
                           nullptr,
                           nullptr,
                           &startup,
                           &process) != FALSE,
            "Cannot start test process");
    const DWORD wait = WaitForSingleObject(process.hProcess, timeoutMilliseconds);
    DWORD exitCode = 1;
    if (wait == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &exitCode);
    if (wait == WAIT_TIMEOUT) TerminateProcess(process.hProcess, ERROR_TIMEOUT);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    Require(wait == WAIT_OBJECT_0 && exitCode == 0, "Test process failed or timed out");
}

/** @brief DPAPI 必须能在当前用户下无损往返，并且密文不包含明文。 */
void TestDpapiRoundTrip() {
    const std::wstring plaintext = L"密钥-Secret-123";
    const std::string protected_value = DpapiSecretProtector::Protect(plaintext);
    Require(!protected_value.empty(), "DPAPI protected value is empty");
    Require(protected_value.find("Secret-123") == std::string::npos, "DPAPI output contains plaintext");
    Require(DpapiSecretProtector::Unprotect(protected_value) == plaintext, "DPAPI round trip differs");
}

/** @brief 默认配置有效，非法线程数和读取块必须被保存前校验拒绝。 */
void TestValidation() {
    AppConfig config = AppConfig::CreateDefault(L"C:\\VideoSc-Test");
    Require(!ConfigValidator::HasErrors(ConfigValidator::Validate(config)), "Default config should be valid");
    config.compute.worker_threads = 0;
    config.io.read_block_kib = 65;
    config.discovery.poll_interval_milliseconds = 0;
    config.dhash_similarity.image_max_hamming_distance = 16;
    config.storage.disk_read_target_percent = 9;
    config.dhash_similarity.video_max_average_hamming_distance = 16;
    config.dhash_similarity.image_aspect_ratio_tolerance_percent = 101;
    Require(ConfigValidator::HasErrors(ConfigValidator::Validate(config)), "Invalid config was accepted");

    config = AppConfig::CreateDefault(L"C:\\VideoSc-Test");
    config.dhash_similarity.validation_worker_threads = 0;
    Require(ConfigValidator::HasErrors(ConfigValidator::Validate(config)),
            "Zero dHash validation workers were accepted");
    config.dhash_similarity.validation_worker_threads = 257;
    Require(ConfigValidator::HasErrors(ConfigValidator::Validate(config)),
            "Too many dHash validation workers were accepted");
}

/** @brief JSON 保存/加载保留 Unicode、性能参数和 DPAPI 密码，文件中不得出现明文。 */
void TestSaveLoadRoundTrip() {
    const std::filesystem::path directory = CreateTestDirectory(L"roundtrip");
    const JsonConfigStore store(directory / L"config.json");
    AppConfig config = AppConfig::CreateDefault(directory);
    config.paths.scan_roots = {L"D:\\媒体", L"E:\\Archive"};
    config.storage.max_concurrent_file_reads = 7;
    config.storage.hdd_read_threads_per_disk = 1;
    config.storage.ssd_read_threads_per_disk = 3;
    config.storage.adaptive_read_threads = true;
    config.storage.disk_read_target_percent = 87;
    config.compute.worker_threads = 12;
    config.compute.ffmpeg_threads_per_task = 2;
    config.compute.adaptive_worker_threads = true;
    config.compute.cpu_target_percent = 82;
    config.io.no_progress_timeout_seconds = 75;
    config.database.password = L"Plaintext-Must-Not-Appear";
    config.thumbnails.video_cell_long_edge = 320;
    config.discovery.query_page_size = 8192;
    config.discovery.launch_timeout_seconds = 45;
    config.discovery.db_load_timeout_seconds = 240;
    config.discovery.poll_interval_milliseconds = 350;
    config.dhash_similarity.image_max_hamming_distance = 7;
    config.dhash_similarity.video_max_average_hamming_distance = 9;
    config.dhash_similarity.image_aspect_ratio_tolerance_percent = 12;
    config.dhash_similarity.validation_worker_threads = 13;
    config.image_similarity.standard_profile.pdq_max_hamming_distance = 27;
    config.image_similarity.standard_profile.fallback_dhash_max_hamming_distance = 3;
    config.image_similarity.low_quality_profile.pdq_max_hamming_distance = 23;
    config.image_similarity.low_quality_profile.fallback_dhash_max_hamming_distance = 2;
    config.image_similarity.pdq_min_quality = 61;
    config.image_similarity.structural_worker_threads = 3;
    config.image_similarity.structural_cache_mib = 384;
    config.report_selection.image_dhash_distance_exclusive_limit = 5;
    config.report_selection.video_dhash_average_distance_exclusive_limit = 4.5;
    config.logging.execution_directory = directory / L"custom-execution-logs";

    const auto save = store.Save(config);
    Require(save.succeeded, "Initial save failed");
    const std::string persisted = ReadFile(store.config_path());
    Require(persisted.find("Plaintext-Must-Not-Appear") == std::string::npos, "JSON contains MySQL plaintext");
    Require(persisted.find("dpapi-current-user") != std::string::npos, "JSON lacks DPAPI marker");

    const auto load = store.Load();
    Require(load.status == ConfigLoadStatus::Loaded, "Saved config did not load from primary");
    Require(load.config.paths.scan_roots.size() == 2, "Scan paths were not restored");
    Require(load.config.compute.worker_threads == 12, "Worker threads were not restored");
    Require(load.config.compute.adaptive_worker_threads && load.config.compute.cpu_target_percent == 82,
            "Adaptive compute settings were not restored");
    Require(load.config.storage.max_concurrent_file_reads == 7 &&
                load.config.storage.hdd_read_threads_per_disk == 1 &&
                load.config.storage.ssd_read_threads_per_disk == 3 &&
                load.config.storage.adaptive_read_threads &&
                load.config.storage.disk_read_target_percent == 87,
            "Storage read concurrency settings were not restored");
    Require(load.config.database.password == config.database.password, "MySQL password was not restored");
    Require(load.config.thumbnails.video_cell_long_edge == 320, "Thumbnail size was not restored");
    Require(load.config.discovery.query_page_size == 8192, "Everything page size was not restored");
    Require(load.config.discovery.launch_timeout_seconds == 45, "Everything launch timeout was not restored");
    Require(load.config.discovery.db_load_timeout_seconds == 240, "Everything DB timeout was not restored");
    Require(load.config.discovery.poll_interval_milliseconds == 350, "Everything poll interval was not restored");
    Require(load.config.dhash_similarity.image_max_hamming_distance == 7,
            "Image dHash maximum distance was not restored");
    Require(load.config.dhash_similarity.video_max_average_hamming_distance == 9 &&
                load.config.dhash_similarity.image_aspect_ratio_tolerance_percent == 12,
            "Video dHash distance or image aspect tolerance was not restored");
    Require(load.config.dhash_similarity.validation_worker_threads == 13,
            "dHash validation worker count was not restored");
    Require(load.config.image_similarity.standard_profile.pdq_max_hamming_distance == 27 &&
                load.config.image_similarity.standard_profile.fallback_dhash_max_hamming_distance == 3 &&
                load.config.image_similarity.pdq_min_quality == 61 &&
                load.config.image_similarity.structural_worker_threads == 3 &&
                load.config.image_similarity.structural_cache_mib == 384,
            "Image three-stage similarity settings were not restored");
    Require(load.config.report_selection.image_dhash_distance_exclusive_limit == 5U &&
                load.config.report_selection.video_dhash_average_distance_exclusive_limit == 4.5,
            "Report selection safety limits were not restored");
    Require(load.config.logging.execution_directory == config.logging.execution_directory,
            "Execution log directory was not restored");

    nlohmann::json legacyDocument = nlohmann::json::parse(persisted);
    legacyDocument["discovery"].erase("query_page_size");
    legacyDocument["discovery"].erase("launch_timeout_seconds");
    legacyDocument["discovery"].erase("db_load_timeout_seconds");
    legacyDocument["discovery"].erase("poll_interval_milliseconds");
    legacyDocument["schema_version"] = 1;
    legacyDocument.erase("dhash_similarity");
    legacyDocument.erase("image_similarity");
    legacyDocument.erase("report_selection");
    WriteFile(store.config_path(), legacyDocument.dump(2));
    const auto legacyLoad = store.Load();
    Require(legacyLoad.status == ConfigLoadStatus::Loaded, "Legacy config did not load");
    Require(legacyLoad.config.discovery.query_page_size == 4096, "Legacy page size default differs");
    Require(legacyLoad.config.discovery.launch_timeout_seconds == 30, "Legacy launch timeout default differs");
    Require(legacyLoad.config.discovery.db_load_timeout_seconds == 120, "Legacy DB timeout default differs");
    Require(legacyLoad.config.discovery.poll_interval_milliseconds == 200, "Legacy poll interval default differs");
    Require(legacyLoad.config.schema_version == videosc::dedup::kCurrentConfigSchemaVersion,
            "Legacy config schema was not migrated");
    Require(legacyLoad.config.dhash_similarity.image_max_hamming_distance == 4,
            "Legacy image dHash distance default differs");
    Require(legacyLoad.config.dhash_similarity.validation_worker_threads == 4,
            "Legacy dHash validation worker default differs");
    Require(legacyLoad.config.image_similarity.standard_profile.pdq_max_hamming_distance == 31 &&
                legacyLoad.config.image_similarity.structural_worker_threads == 2,
            "Legacy image similarity defaults differ");
    std::filesystem::remove_all(directory);
}

/** @brief 第二次保存产生上一有效备份，主文件损坏后必须从备份恢复。 */
void TestBackupRecovery() {
    const std::filesystem::path directory = CreateTestDirectory(L"backup");
    const JsonConfigStore store(directory / L"config.json");
    AppConfig config = AppConfig::CreateDefault(directory);
    config.compute.worker_threads = 7;
    Require(store.Save(config).succeeded, "First save failed");
    config.compute.worker_threads = 9;
    Require(store.Save(config).succeeded, "Second save failed");
    Require(std::filesystem::exists(store.backup_path()), "Backup was not created");

    WriteFile(store.config_path(), "{not-json");
    const auto load = store.Load();
    Require(load.status == ConfigLoadStatus::RecoveredFromBackup, "Corrupt primary did not use backup");
    Require(load.config.compute.worker_threads == 7, "Backup is not the previous valid config");
    std::filesystem::remove_all(directory);
}

/** @brief 未来模式版本必须进入只读拒绝状态，防止旧程序覆盖新字段。 */
void TestFutureVersionIsReadOnly() {
    const std::filesystem::path directory = CreateTestDirectory(L"future");
    const JsonConfigStore store(directory / L"config.json");
    AppConfig config = AppConfig::CreateDefault(directory);
    Require(store.Save(config).succeeded, "Cannot create future-version fixture");
    nlohmann::json document = nlohmann::json::parse(ReadFile(store.config_path()));
    Require(document.contains("schema_version"), "Cannot locate schema version");
    document["schema_version"] = 999;
    WriteFile(store.config_path(), document.dump());

    const auto load = store.Load();
    Require(load.status == ConfigLoadStatus::UnsupportedVersion, "Future schema was not rejected");
    Require(!load.can_save, "Future schema can be overwritten");
    std::filesystem::remove_all(directory);
}

/** @brief DPAPI 密文损坏只禁用数据库凭据，其他配置仍需正常加载。 */
void TestBrokenDpapiKeepsOtherSettings() {
    const std::filesystem::path directory = CreateTestDirectory(L"dpapi-failure");
    const JsonConfigStore store(directory / L"config.json");
    AppConfig config = AppConfig::CreateDefault(directory);
    config.compute.worker_threads = 11;
    config.database.password = L"valid-before-corruption";
    Require(store.Save(config).succeeded, "Cannot create DPAPI fixture");
    nlohmann::json document = nlohmann::json::parse(ReadFile(store.config_path()));
    Require(document.contains("database") && document["database"].contains("password_protected"),
            "Cannot locate protected password");
    document["database"]["password_protected"] = "QUJDRA==";
    WriteFile(store.config_path(), document.dump());

    const auto load = store.Load();
    Require(load.status == ConfigLoadStatus::Loaded, "DPAPI failure invalidated the whole config");
    Require(load.config.compute.worker_threads == 11, "Other settings were lost after DPAPI failure");
    Require(load.config.database.password_decryption_failed, "DPAPI failure flag was not set");
    Require(load.config.database.password.empty(), "Broken DPAPI returned a password");
    std::filesystem::remove_all(directory);
}

/** @brief SHA-512 二进制与十六进制转换必须稳定且严格拒绝非法输入。 */
void TestSha512HexCodec() {
    Sha512Digest digest{};
    for (std::size_t index = 0; index < digest.size(); ++index) {
        digest[index] = static_cast<std::uint8_t>(index);
    }
    const std::string hex = Sha512ToHex(digest);
    Require(hex.size() == 128, "SHA-512 hex length is not 128");
    const auto parsed = Sha512FromHex(hex);
    Require(parsed.has_value() && parsed.value() == digest, "SHA-512 hex round trip differs");
    Require(!Sha512FromHex(hex.substr(1)).has_value(), "Short SHA-512 hex was accepted");
    std::string invalid = hex;
    invalid[17] = 'x';
    Require(!Sha512FromHex(invalid).has_value(), "Invalid SHA-512 character was accepted");
}

/** @brief 扫描快照冻结性能参数和路径，但不能暴露或复制数据库密码。 */
void TestScanOptionsFreeze() {
    AppConfig config = AppConfig::CreateDefault(L"C:\\VideoSc-Test");
    config.paths.scan_roots = {L"D:\\Media"};
    config.compute.worker_threads = 18;
    config.io.no_progress_timeout_seconds = 90;
    config.dhash_similarity.image_max_hamming_distance = 6;
    config.dhash_similarity.validation_worker_threads = 11;
    config.database.password = L"must-stay-outside-snapshot";
    config.database.password_protected = "protected-material-must-also-stay-out";
    const ScanOptions options = ScanOptions::Freeze(config);
    config.compute.worker_threads = 1;
    config.paths.scan_roots.clear();

    Require(options.compute().worker_threads == 18, "Frozen compute threads changed with live config");
    Require(options.scan_roots().size() == 1, "Frozen scan roots changed with live config");
    Require(options.io().no_progress_timeout_seconds == 90, "Frozen timeout differs");
    Require(options.dhash_similarity().image_max_hamming_distance == 6,
            "Frozen image dHash distance differs");
    Require(options.dhash_similarity().validation_worker_threads == 11,
            "Frozen dHash validation workers differ");
    Require(options.database().host == L"127.0.0.1", "Database endpoint was not frozen");
}

/**
 * @brief 测试使用的取消回调。
 * @param context 指向 bool 的上下文。
 * @return bool 为 true 时返回 1。
 */
int __cdecl TestShouldCancel(void* context) {
    return *static_cast<const bool*>(context) ? 1 : 0;
}

/** @brief 流式 SHA-512 必须匹配标准向量，并报告完整读取进度。 */
void TestStreamingSha512() {
    const std::filesystem::path directory = CreateTestDirectory(L"streaming-sha512");
    const std::filesystem::path filePath = directory / L"abc.bin";
    WriteFile(filePath, "abc");
    const std::string utf8Path = filePath.u8string();
    VideoScHashOptions options{
        sizeof(VideoScHashOptions), 1, 1, 1, 1, 5000};
    VideoScFileHashResult result{};
    const int succeeded = ComputeFileSHA512Ex(utf8Path.c_str(), &options, nullptr, nullptr, &result);
    constexpr char expected[] =
        "ddaf35a193617abacc417349ae204131"
        "12e6fa4e89a97ea20a9eeee64b55d39a"
        "2192992a274fc1a836ba3c23a3feebbd"
        "454d4423643ce80e2a9ac94fa54ca49f";
    Require(succeeded == 1, "Streaming SHA-512 failed");
    Require(result.statusCode == VIDEOSC_HASH_OK, "Streaming SHA-512 status is not success");
    Require(result.fileSize == 3 && result.bytesRead == 3, "Streaming SHA-512 progress differs");
    Require(std::string(result.sha512Hex) == expected, "Streaming SHA-512 digest differs");

    char compatibilityHash[129]{};
    Require(ComputeFileSHA512(utf8Path.c_str(), compatibilityHash, sizeof(compatibilityHash)) == 1,
            "Compatibility SHA-512 API failed");
    Require(std::string(compatibilityHash) == expected, "Compatibility SHA-512 digest differs");
    std::filesystem::remove_all(directory);
}

/** @brief 取消流式 SHA-512 时不得返回部分摘要。 */
void TestStreamingSha512Cancellation() {
    const std::filesystem::path directory = CreateTestDirectory(L"streaming-cancel");
    const std::filesystem::path filePath = directory / L"cancel.bin";
    WriteFile(filePath, std::string(1024, 'x'));
    const std::string utf8Path = filePath.u8string();
    const bool cancel = true;
    VideoScFileHashResult result{};
    const int succeeded = ComputeFileSHA512Ex(utf8Path.c_str(), nullptr, TestShouldCancel, const_cast<bool*>(&cancel), &result);
    Require(succeeded == 0, "Cancelled SHA-512 reported success");
    Require(result.statusCode == VIDEOSC_HASH_CANCELLED, "Cancelled SHA-512 status differs");
    Require(result.sha512Hex[0] == '\0', "Cancelled SHA-512 exposed a partial digest");
    std::filesystem::remove_all(directory);
}

/** @brief 调度测试哈希器，用短暂停顿观测全局并发闸门。 */
class SchedulerTestHasher final : public IFileHasher {
public:
    /** @copydoc IFileHasher::Hash */
    FileHashResult Hash(const std::filesystem::path& path,
                        const std::atomic_bool& cancel_requested) override {
        const int active = active_calls_.fetch_add(1, std::memory_order_relaxed) + 1;
        int observed = maximum_active_calls_.load(std::memory_order_relaxed);
        while (active > observed &&
               !maximum_active_calls_.compare_exchange_weak(
                   observed, active, std::memory_order_relaxed)) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(4));

        FileHashResult result;
        if (cancel_requested.load(std::memory_order_relaxed)) {
            result.status = FileHashStatus::Cancelled;
        } else {
            result.status = FileHashStatus::Succeeded;
            Sha512Digest digest{};
            digest[0] = static_cast<std::uint8_t>(path.wstring().size());
            result.sha512 = digest;
            result.file_size = 100;
            result.bytes_read = 100;
        }
        active_calls_.fetch_sub(1, std::memory_order_relaxed);
        return result;
    }

    /** @return 测试期间同时进入 Hash 的最大线程数。 */
    int maximum_active_calls() const noexcept {
        return maximum_active_calls_.load(std::memory_order_relaxed);
    }

private:
    std::atomic_int active_calls_{0};
    std::atomic_int maximum_active_calls_{0};
};

/** @brief 多物理盘并行时全局计算并发不超限，所有有界队列任务必须完成。 */
void TestDiskHashSchedulerConcurrency() {
    auto hasher = std::make_shared<SchedulerTestHasher>();
    DiskHashScheduler scheduler(hasher, 2, 2, false, 90);
    std::mutex outcomeMutex;
    std::vector<FileHashOutcome> outcomes;
    scheduler.Start(
        {DiskChannelOptions{L"PhysicalDrive0", StorageMediaType::Hdd, 3, 32, true, 8},
         DiskChannelOptions{L"PhysicalDrive1", StorageMediaType::Ssd, 3, 32, false, 8}},
        [&](FileHashOutcome outcome) {
            std::lock_guard<std::mutex> lock(outcomeMutex);
            outcomes.push_back(std::move(outcome));
        });

    for (std::uint64_t index = 0; index < 20; ++index) {
        FileHashJob job;
        job.job_id = index + 1;
        job.scan_id = 99;
        job.path = L"file-" + std::to_wstring(index);
        job.storage_target_key = index % 2 == 0 ? L"PhysicalDrive0" : L"PhysicalDrive1";
        job.physical_start_byte = (20 - index) * 4096;
        Require(scheduler.Submit(std::move(job), 1000), "Scheduler rejected a valid job");
    }
    scheduler.CloseSubmissions();
    Require(scheduler.WaitUntilIdle(10000), "Scheduler did not become idle");
    scheduler.Join();

    Require(outcomes.size() == 20, "Scheduler lost completion outcomes");
    Require(hasher->maximum_active_calls() >= 2, "Independent disks did not run concurrently");
    Require(hasher->maximum_active_calls() <= 2, "Global computation gate was exceeded");
    std::uint64_t completed = 0;
    for (const auto& snapshot : scheduler.GetSnapshots()) {
        completed += snapshot.completed_files;
    }
    Require(completed == 20, "Disk snapshots lost completed counts");
}

/** @brief RocksDB 必须支持跨 Column Family 原子批次、前缀扫描和重启恢复。 */
void TestRocksStorePersistence() {
    const std::filesystem::path directory = CreateTestDirectory(L"rocks-store");
    RocksDbConfig config;
    config.directory = directory / L"database";
    config.block_cache_mib = 16;
    config.write_buffer_mib = 40;

    {
        RocksStore store(config);
        Require(store.Open().succeeded, "RocksDB open failed");
        Require(store.Put(RocksColumnFamily::ScanTasks, "scan/0001", "running", true).succeeded,
                "RocksDB put failed");
        const std::vector<RocksMutation> mutations = {
            {RocksColumnFamily::FilePaths, "path/a", std::string("sha-a")},
            {RocksColumnFamily::FilePaths, "path/b", std::string("sha-b")},
            {RocksColumnFamily::SyncQueue, "sync/0001", std::string("payload")},
        };
        Require(store.WriteBatch(mutations, true).succeeded, "RocksDB batch failed");
        Require(store.Put(RocksColumnFamily::Default, "temporary/a", "1", false).succeeded &&
                    store.Put(RocksColumnFamily::Default, "temporary/b", "2", false).succeeded,
                "RocksDB prefix cleanup fixture failed");
        Require(store.DeletePrefix(RocksColumnFamily::Default, "temporary/", 1, false).succeeded,
                "RocksDB bounded prefix cleanup failed");
        std::string removedValue;
        Require(!store.Get(RocksColumnFamily::Default, "temporary/a", removedValue).succeeded &&
                    !store.Get(RocksColumnFamily::Default, "temporary/b", removedValue).succeeded,
                "RocksDB prefix cleanup left keys behind");

        std::vector<std::string> keys;
        Require(store.ForEachPrefix(
                         RocksColumnFamily::FilePaths,
                         "path/",
                         10,
                         [&](const std::string_view key, const std::string_view) {
                             keys.emplace_back(key);
                             return true;
                         })
                    .succeeded,
                "RocksDB prefix iteration failed");
        Require(keys.size() == 2, "RocksDB prefix iteration count differs");
        store.Close();
    }

    {
        RocksStore reopened(config);
        Require(reopened.Open().succeeded, "RocksDB reopen failed");
        std::string value;
        Require(reopened.Get(RocksColumnFamily::ScanTasks, "scan/0001", value).succeeded,
                "RocksDB persisted value is missing");
        Require(value == "running", "RocksDB persisted value differs");
        Require(reopened.Delete(RocksColumnFamily::ScanTasks, "scan/0001", true).succeeded,
                "RocksDB delete failed");
        reopened.Close();
    }
    std::filesystem::remove_all(directory);
}

/** @brief 数据版本纯策略必须可靠区分复用、续建、重置与高版本拒绝。 */
void TestDataVersionDecisions() {
    const auto evaluate = [](const std::optional<DataVersionRecord>& rocks,
                             const std::optional<DataVersionRecord>& mysql) {
        return DataVersionCoordinator::Evaluate(rocks, mysql);
    };
    Require(evaluate(std::nullopt, std::nullopt) == DataVersionDecision::ResetRequired,
            "Missing data versions were reused");

    DataVersionRecord current;
    current.data_version = videosc::dedup::kCurrentDataVersion;
    current.generation_id = 17;
    current.state = DataVersionState::Ready;
    DataVersionRecord lower = current;
    lower.data_version = 0;
    Require(evaluate(lower, current) == DataVersionDecision::ResetRequired,
            "Lower RocksDB data version did not request reset");
    Require(evaluate(current, lower) == DataVersionDecision::ResetRequired,
            "Lower MySQL data version did not request reset");
    Require(evaluate(current, current) == DataVersionDecision::ReuseReady,
            "Matching ready generation was not reused");

    DataVersionRecord rebuilding = current;
    rebuilding.state = DataVersionState::Rebuilding;
    Require(evaluate(rebuilding, rebuilding) == DataVersionDecision::ResumeRebuild,
            "Matching rebuilding generation was not resumable");
    Require(evaluate(rebuilding, current) == DataVersionDecision::ResumeRebuild,
            "Two-step ready recovery was not resumable");

    DataVersionRecord differentGeneration = current;
    differentGeneration.generation_id = 18;
    Require(evaluate(current, differentGeneration) == DataVersionDecision::ResetRequired,
            "Mismatched generations were reused");

    DataVersionRecord newer = current;
    newer.data_version = videosc::dedup::kCurrentDataVersion + 1;
    Require(evaluate(newer, current) == DataVersionDecision::RejectNewerData,
            "Newer RocksDB data version was not rejected");
    Require(evaluate(current, newer) == DataVersionDecision::RejectNewerData,
            "Newer MySQL data version was not rejected");
}

/** @brief 全量清理必须覆盖固定十个 Column Family，且不能删除 RocksDB 目录外文件。 */
void TestRocksStoreClearAll() {
    const std::filesystem::path directory = CreateTestDirectory(L"rocks-clear-all");
    const std::filesystem::path sentinel = directory / L"keep-me.txt";
    {
        std::ofstream output(sentinel, std::ios::binary | std::ios::trunc);
        output << "outside-rocksdb";
    }
    RocksDbConfig config;
    config.directory = directory / L"database";
    config.block_cache_mib = 16;
    config.write_buffer_mib = 40;
    RocksStore store(config);
    Require(store.Open().succeeded, "ClearAll RocksDB open failed");

    const std::array<RocksColumnFamily, 10> families = {
        RocksColumnFamily::Default,
        RocksColumnFamily::ScanTasks,
        RocksColumnFamily::FilePaths,
        RocksColumnFamily::ShaFileData,
        RocksColumnFamily::SyncQueue,
        RocksColumnFamily::ExactIndex,
        RocksColumnFamily::ImageDhashIndex,
        RocksColumnFamily::VideoDhashIndex,
        RocksColumnFamily::Checkpoints,
        RocksColumnFamily::Tombstones,
    };
    for (std::size_t index = 0; index < families.size(); ++index) {
        Require(store.Put(families[index], "legacy/" + std::to_string(index), "value", true).succeeded,
                "Cannot seed a RocksDB Column Family");
    }
    Require(store.ClearAll(2, true).succeeded, "RocksDB ClearAll failed");
    for (std::size_t index = 0; index < families.size(); ++index) {
        std::string value;
        const auto status = store.Get(families[index], "legacy/" + std::to_string(index), value);
        Require(!status.succeeded && status.message == "not_found",
                "RocksDB ClearAll left a Column Family key behind");
    }
    Require(std::filesystem::exists(sentinel), "RocksDB ClearAll removed an external file");
    store.Close();
    std::filesystem::remove_all(directory);
}

/** @brief ScanOptions 快照必须完整恢复资源语义且 JSON 中不存在密码材料。 */
void TestScanOptionsCodec() {
    AppConfig config = AppConfig::CreateDefault(L"C:\\VideoSc-Test");
    config.paths.scan_roots = {L"D:\\媒体", L"E:\\Archive"};
    config.storage.max_concurrent_file_reads = 9;
    config.storage.hdd_read_threads_per_disk = 1;
    config.storage.ssd_read_threads_per_disk = 4;
    config.compute.worker_threads = 16;
    config.compute.adaptive_worker_threads = true;
    config.compute.cpu_target_percent = 87;
    config.io.no_progress_timeout_seconds = 123;
    config.discovery.method = DiscoveryMethod::Everything;
    config.discovery.everything_dll_path = L"C:\\Everything\\Everything64.dll";
    config.discovery.everything_exe_path = L"C:\\Everything\\Everything.exe";
    config.discovery.query_page_size = 16384;
    config.discovery.launch_timeout_seconds = 55;
    config.discovery.db_load_timeout_seconds = 300;
    config.discovery.poll_interval_milliseconds = 425;
    config.database.password = L"snapshot-plaintext";
    config.database.password_protected = "snapshot-protected";
    config.dhash_similarity.image_max_hamming_distance = 9;
    config.dhash_similarity.validation_worker_threads = 17;
    const ScanOptions frozen = ScanOptions::Freeze(config);
    const std::string json = ScanOptionsCodec::Serialize(frozen);
    Require(json.find("snapshot-plaintext") == std::string::npos, "Snapshot contains plaintext password");
    Require(json.find("snapshot-protected") == std::string::npos, "Snapshot contains protected password material");

    std::string error;
    const auto restored = ScanOptionsCodec::Deserialize(json, error);
    Require(restored.has_value(), "ScanOptions snapshot did not deserialize");
    Require(restored->scan_roots().size() == 2, "Snapshot scan roots differ");
    Require(restored->storage().max_concurrent_file_reads == 9 &&
                restored->storage().hdd_read_threads_per_disk == 1 &&
                restored->storage().ssd_read_threads_per_disk == 4,
            "Snapshot storage read concurrency differs");
    Require(restored->compute().worker_threads == 16, "Snapshot compute threads differ");
    Require(restored->compute().adaptive_worker_threads && restored->compute().cpu_target_percent == 87,
            "Snapshot adaptive compute settings differ");
    Require(restored->io().no_progress_timeout_seconds == 123, "Snapshot timeout differs");
    Require(restored->discovery().method == DiscoveryMethod::Everything, "Snapshot discovery method differs");
    Require(restored->discovery().everything_dll_path == config.discovery.everything_dll_path,
            "Snapshot Everything DLL path differs");
    Require(restored->discovery().everything_exe_path == config.discovery.everything_exe_path,
            "Snapshot Everything EXE path differs");
    Require(restored->discovery().query_page_size == 16384, "Snapshot page size differs");
    Require(restored->discovery().launch_timeout_seconds == 55, "Snapshot launch timeout differs");
    Require(restored->discovery().db_load_timeout_seconds == 300, "Snapshot DB timeout differs");
    Require(restored->discovery().poll_interval_milliseconds == 425, "Snapshot poll interval differs");
    Require(restored->dhash_similarity().image_max_hamming_distance == 9,
            "Snapshot image dHash distance differs");
    Require(restored->dhash_similarity().validation_worker_threads == 17,
            "Snapshot dHash validation workers differ");

    nlohmann::ordered_json legacySnapshot = nlohmann::ordered_json::parse(json);
    legacySnapshot["snapshot_version"] = 1;
    legacySnapshot.erase("discovery");
    legacySnapshot.erase("dhash_similarity");
    const auto legacyRestored = ScanOptionsCodec::Deserialize(legacySnapshot.dump(), error);
    Require(legacyRestored.has_value(), "Legacy ScanOptions snapshot did not deserialize");
    Require(legacyRestored->discovery().query_page_size == 4096,
            "Legacy snapshot page size default differs");
    Require(legacyRestored->discovery().poll_interval_milliseconds == 200,
            "Legacy snapshot poll interval default differs");
    Require(legacyRestored->dhash_similarity().image_max_hamming_distance == 4,
            "Legacy snapshot image dHash distance default differs");
    Require(legacyRestored->dhash_similarity().validation_worker_threads == 4,
            "Legacy snapshot dHash validation worker default differs");
}

/** @brief 检查点与单文件业务变更必须原子写入，并可在重启后标记为 Interrupted。 */
void TestScanCheckpointRecovery() {
    const std::filesystem::path directory = CreateTestDirectory(L"checkpoint");
    RocksDbConfig rocksConfig;
    rocksConfig.directory = directory / L"rocks";
    rocksConfig.block_cache_mib = 16;
    rocksConfig.write_buffer_mib = 40;
    RocksStore store(rocksConfig);
    Require(store.Open().succeeded, "Checkpoint RocksDB open failed");
    ScanCheckpointStore checkpoints(store);

    AppConfig config = AppConfig::CreateDefault(directory);
    config.paths.scan_roots = {directory};
    ScanCheckpoint checkpoint;
    checkpoint.scan_id = 42;
    checkpoint.phase = ScanPhase::Hashing;
    checkpoint.scan_options_json = ScanOptionsCodec::Serialize(ScanOptions::Freeze(config));
    checkpoint.discovery_cursor = "everything-page-8";
    checkpoint.discovered_files = 1000;
    checkpoint.completed_files = 600;
    checkpoint.image_feature_failed_contents = 7;
    Require(checkpoints.SaveWithMutations(
                           checkpoint,
                           {{RocksColumnFamily::FilePaths,
                             "scan/42/path/600",
                             std::string("completed")}})
                .succeeded,
            "Atomic checkpoint batch failed");

    const auto loaded = checkpoints.Load(42);
    Require(loaded.status.succeeded && loaded.checkpoint.has_value(), "Checkpoint load failed");
    Require(loaded.checkpoint->completed_files == 600, "Checkpoint progress differs");
    Require(loaded.checkpoint->image_feature_failed_contents == 7,
            "Checkpoint image feature failure count differs");
    std::string fileValue;
    Require(store.Get(RocksColumnFamily::FilePaths, "scan/42/path/600", fileValue).succeeded,
            "Atomic checkpoint business mutation is missing");

    nlohmann::ordered_json legacy = {
        {"record_version", 1},
        {"scan_id", 43},
        {"phase", static_cast<int>(ScanPhase::CompletedSynchronized)},
        {"scan_options_json", checkpoint.scan_options_json},
        {"discovery_cursor", ""},
        {"discovered_files", 1},
        {"completed_files", 1},
        {"failed_files", 0},
        {"media_completed_files", 1},
        {"next_sync_sequence", 0},
        {"updated_utc_ms", 0},
    };
    Require(store.Put(RocksColumnFamily::Checkpoints,
                      "scan/00000000000000000043",
                      legacy.dump(),
                      true).succeeded,
            "Legacy checkpoint write failed");
    const auto legacyLoaded = checkpoints.Load(43);
    Require(legacyLoaded.status.succeeded && legacyLoaded.checkpoint.has_value() &&
                legacyLoaded.checkpoint->image_feature_failed_contents == 0,
            "Legacy checkpoint did not default image feature failures to zero");

    Require(checkpoints.MarkInterrupted(42).succeeded, "MarkInterrupted failed");
    std::vector<ScanCheckpoint> resumable;
    Require(checkpoints.ListResumable(10, resumable).succeeded, "ListResumable failed");
    Require(resumable.size() == 1 && resumable.front().phase == ScanPhase::Interrupted,
            "Interrupted checkpoint is not resumable");
    store.Close();
    std::filesystem::remove_all(directory);
}

/** @brief 图片部分报告默认策略和扫描超时重试必须保持新契约与有限边界。 */
void TestImageFeatureCompletenessAndRetryPolicies() {
    videosc::dedup::ImageSimilarityConfig config;
    Require(!config.require_complete_features && config.allow_partial_reports,
            "New reports did not default to automatic partial-scope publication");
    config.require_complete_features = true;
    config.allow_partial_reports = false;
    Require(config.require_complete_features && !config.allow_partial_reports,
            "Legacy completeness flags were not retained as readable compatibility fields");

    Require(videosc::dedup::ShouldRetryImageFeatureAnalysis(
                VIDEOSC_ERR_MEDIA_TIMEOUT, 1, false),
            "First image timeout was not retried");
    Require(!videosc::dedup::ShouldRetryImageFeatureAnalysis(
                VIDEOSC_ERR_MEDIA_TIMEOUT, 2, false),
            "Second image timeout incorrectly requested another retry");
    Require(!videosc::dedup::ShouldRetryImageFeatureAnalysis(
                VIDEOSC_ERR_MEDIA_TIMEOUT, 1, true),
            "Cancelled image analysis incorrectly requested a retry");
    Require(!videosc::dedup::ShouldRetryImageFeatureAnalysis(
                VIDEOSC_ERR_DECODE_FAILED, 1, false),
            "Deterministic image decode failure incorrectly requested a retry");

    videosc::dedup::ImageFeatureBackfillResult result;
    result.succeeded = true;
    result.complete = false;
    result.completeness.total_images = 4;
    result.completeness.complete_images = 1;
    result.completeness.incomplete_images = 3;
    result.final_progress.total_images = 4;
    result.final_progress.completed_images = 1;
    result.final_progress.failed_images = 3;
    result.final_progress.no_readable_path_images = 1;
    result.final_progress.timeout_images = 1;
    result.final_progress.decode_failed_images = 1;
    Require(result.final_progress.completed_images + result.final_progress.failed_images <=
                result.final_progress.total_images,
            "Image backfill final statistics are not conservative");
    Require(result.succeeded && !result.cancelled && !result.complete,
            "Incomplete backfill did not remain an operational success that can continue to reporting");
}

/** @brief 待同步消息必须与本地记录原子落盘，并在确认后只删除已提交项。 */
void TestPersistentSyncQueue() {
    const std::filesystem::path directory = CreateTestDirectory(L"sync-queue");
    RocksDbConfig config;
    config.directory = directory / L"rocks";
    config.block_cache_mib = 8;
    config.write_buffer_mib = 4;
    RocksStore store(config);
    Require(store.Open().succeeded, "Cannot open sync queue RocksDB");
    MySqlSyncQueue queue(store);

    ShaFileData content;
    for (std::size_t index = 0; index < content.sha512.size(); ++index) {
        content.sha512[index] = static_cast<std::uint8_t>(index + 3);
    }
    content.content_size_bytes = 123456;
    content.media_kind = videosc::dedup::MediaKind::Video;
    content.video_duration_ms = 9876;
    content.video_dhashes = {1, 2, 3, 4, 5, 6};
    content.has_video_dhashes = true;
    content.image_pdq_hash = videosc::dedup::PdqHash256{};
    (*content.image_pdq_hash)[3] = 0x5a;
    content.image_pdq_quality = static_cast<std::uint8_t>(91);
    content.has_image_zoned_phashes = true;
    content.image_zoned_phashes[5] = 0x12345678ULL;
    content.image_perceptual_algorithm_version = 1;
    content.image_structural_algorithm_version = 1;
    content.contact_sheet_path = directory / L"thumbs" / L"sheet.jpg";
    content.media_algorithm_version = "video-dhash-v1";

    SyncOperation contentOperation;
    contentOperation.kind = SyncOperationKind::UpsertShaFileData;
    contentOperation.sha_file_data = content;
    const std::vector<RocksMutation> local = {
        {RocksColumnFamily::ShaFileData, Sha512ToHex(content.sha512), "local-content"},
    };
    Require(queue.Enqueue(local, contentOperation).succeeded, "Cannot atomically enqueue content");
    Require(contentOperation.sequence == 1, "First sync sequence is not one");

    FilePathRecord path;
    path.path_id = 55;
    path.scan_id = 9;
    path.path = directory / L"媒体" / L"sample.mp4";
    path.normalized_path_key = L"normalized-sample";
    path.sha512 = content.sha512;
    path.size_bytes = content.content_size_bytes;
    path.state = videosc::dedup::FilePathState::Available;
    SyncOperation pathOperation;
    pathOperation.kind = SyncOperationKind::UpsertFilePath;
    pathOperation.file_path = path;
    Require(queue.Enqueue({}, pathOperation).succeeded, "Cannot enqueue path mapping");
    Require(pathOperation.sequence == 2, "Sync sequence did not advance");

    std::string localValue;
    Require(store.Get(RocksColumnFamily::ShaFileData, Sha512ToHex(content.sha512), localValue).succeeded,
            "Local mutation was not committed with sync message");
    std::vector<SyncOperation> operations;
    Require(queue.ReadBatch(10, operations).succeeded, "Cannot read sync queue");
    Require(operations.size() == 2, "Unexpected sync queue size");
    Require(operations[0].sequence == 1 && operations[1].sequence == 2, "Sync queue is not ordered");
    Require(operations[0].sha_file_data.has_value() &&
                operations[0].sha_file_data->video_dhashes == content.video_dhashes &&
                operations[0].sha_file_data->image_pdq_hash == content.image_pdq_hash &&
                operations[0].sha_file_data->image_zoned_phashes == content.image_zoned_phashes,
            "Content sync payload did not round trip");
    Require(operations[1].file_path.has_value() && operations[1].file_path->path == path.path,
            "UTF-8 path sync payload did not round trip");

    Require(queue.Acknowledge({1}).succeeded, "Cannot acknowledge first sync message");
    operations.clear();
    Require(queue.ReadBatch(10, operations).succeeded, "Cannot reread sync queue");
    Require(operations.size() == 1 && operations.front().sequence == 2,
            "Acknowledgement removed the wrong sync messages");

    SyncOperation stagedOperation;
    stagedOperation.kind = SyncOperationKind::DeleteFilePath;
    stagedOperation.delete_path_id = 999;
    bool insertedNewKey = false;
    Require(queue.Stage(77, {}, stagedOperation, insertedNewKey).succeeded && insertedNewKey,
            "Cannot stage current-scan sync operation");
    std::uint64_t stagedCount = 0;
    std::uint64_t pendingCount = 0;
    Require(queue.StagedCount(77, stagedCount).succeeded && stagedCount == 1,
            "Staged sync count differs");
    Require(queue.PendingCount(77, pendingCount).succeeded && pendingCount == 0,
            "Staged operation was published before threshold");
    operations.clear();
    Require(queue.ReadBatch(10, operations).succeeded && operations.size() == 1,
            "Staged operation leaked into formal retry queue");
    std::size_t publishedItems = 0;
    Require(queue.PublishStaged(77, 10, publishedItems).succeeded && publishedItems == 1,
            "Cannot publish staged sync operation");
    Require(queue.StagedCount(77, stagedCount).succeeded && stagedCount == 0 &&
                queue.PendingCount(77, pendingCount).succeeded && pendingCount == 1,
            "Published scan counters differ");
    operations.clear();
    Require(queue.ReadBatch(10, operations).succeeded && operations.size() == 2 &&
                operations.back().sequence == 3 && operations.back().batch_scan_id == 77,
            "Published staged operation is missing scan identity");
    Require(queue.Acknowledge({3}).succeeded &&
                queue.PendingCount(77, pendingCount).succeeded && pendingCount == 0,
            "Acknowledging staged operation did not clear scan pending count");
    SyncOperation historicalStaged;
    historicalStaged.kind = SyncOperationKind::DeleteFilePath;
    historicalStaged.delete_path_id = 888;
    Require(queue.Stage(88, {}, historicalStaged, insertedNewKey).succeeded && insertedNewKey,
            "Cannot stage historical recovery operation");
    store.Close();

    RocksStore reopened(config);
    Require(reopened.Open().succeeded, "Cannot reopen sync queue RocksDB");
    MySqlSyncQueue reopenedQueue(reopened);
    std::size_t recoveredStaged = 0;
    Require(reopenedQueue.PublishAllStaged(10, recoveredStaged).succeeded && recoveredStaged == 1,
            "Historical staged operation was not published on restart");
    SyncOperation deleteOperation;
    deleteOperation.kind = SyncOperationKind::DeleteFilePath;
    deleteOperation.delete_path_id = path.path_id;
    deleteOperation.expected_sha512 = content.sha512;
    Require(reopenedQueue.Enqueue({}, deleteOperation).succeeded, "Cannot enqueue after restart");
    Require(deleteOperation.sequence == 5, "Sync sequence was not persistent");
    reopened.Close();
    std::filesystem::remove_all(directory);
}

/** @brief 初始化 SQL 只能幂等创建计划内表，禁止出现清空或重建指令。 */
void TestSafeMySqlSchemaStatements() {
    const auto& statements = MySqlSchema::InitializationStatements();
    Require(statements.size() >= 6, "MySQL initialization statements are incomplete");
    Require(videosc::dedup::kCurrentMySqlSchemaVersion == 3,
            "MySQL schema version was not advanced to three");
    std::string combined;
    for (const std::string& statement : statements) {
        combined += statement;
        combined.push_back('\n');
    }
    Require(combined.find("file_path_sha512") != std::string::npos, "Path mapping table is missing");
    Require(combined.find("sha512_file_data") != std::string::npos, "Content table is missing");
    Require(combined.find("videosc_data_version") != std::string::npos,
            "Data version table is missing");
    Require(combined.find("image_pdq_hash BINARY(32)") != std::string::npos &&
                combined.find("image_zoned_phashes BINARY(128)") != std::string::npos,
            "Image perceptual feature columns are missing");
    Require(combined.find("ix_exact_active_sha") != std::string::npos, "Exact grouping index is missing");
    Require(combined.find("DROP ") == std::string::npos, "Initialization contains DROP");
    Require(combined.find("TRUNCATE ") == std::string::npos, "Initialization contains TRUNCATE");
    Require(combined.find("DELETE ") == std::string::npos, "Initialization contains DELETE");
    Require(combined.find("INSERT INTO videosc_data_version") == std::string::npos,
            "Initialization incorrectly marks an empty data set ready");

    const auto& resetStatements = MySqlSchema::ResetStatements();
    const std::set<std::string> expectedReset = {
        "DROP TABLE IF EXISTS duplicate_group_member",
        "DROP TABLE IF EXISTS duplicate_group",
        "DROP TABLE IF EXISTS file_path_sha512",
        "DROP TABLE IF EXISTS sha512_file_data",
        "DROP TABLE IF EXISTS videosc_data_version",
    };
    Require(std::set<std::string>(resetStatements.begin(), resetStatements.end()) == expectedReset,
            "MySQL reset statements escaped the fixed business-table whitelist");
    std::string resetCombined;
    for (const std::string& statement : resetStatements) resetCombined += statement + '\n';
    Require(resetCombined.find("DROP DATABASE") == std::string::npos,
            "MySQL reset contains DROP DATABASE");
    Require(resetCombined.find("videosc_schema_version") == std::string::npos,
            "MySQL reset deletes the schema version table");
    Require(resetCombined.find("TRUNCATE ") == std::string::npos &&
                resetCombined.find("DELETE ") == std::string::npos,
            "MySQL reset contains unbounded data deletion");
}

/** @brief 千万级精确分组的核心必须按摘要单遍输出且忽略单成员内容。 */
void TestExactGroupAccumulator() {
    Sha512Digest first{};
    Sha512Digest duplicate{};
    Sha512Digest triple{};
    first[0] = 1;
    duplicate[0] = 2;
    triple[0] = 3;
    std::vector<std::size_t> groupSizes;
    std::vector<std::uint64_t> reclaimable;
    ExactGroupAccumulator accumulator([&](videosc::dedup::DuplicateGroup&& group) {
        groupSizes.push_back(group.members.size());
        reclaimable.push_back(group.reclaimable_bytes);
        return true;
    });
    DuplicateMember member;
    member.size_bytes = 100;
    Require(accumulator.Consume(first, member), "Cannot consume singleton");
    member.path_id = 2;
    Require(accumulator.Consume(duplicate, member), "Cannot consume duplicate member one");
    member.path_id = 3;
    Require(accumulator.Consume(duplicate, member), "Cannot consume duplicate member two");
    member.size_bytes = 250;
    member.path_id = 4;
    Require(accumulator.Consume(triple, member), "Cannot consume triple member one");
    member.path_id = 5;
    Require(accumulator.Consume(triple, member), "Cannot consume triple member two");
    member.path_id = 6;
    Require(accumulator.Consume(triple, member), "Cannot consume triple member three");
    Require(accumulator.Finish(), "Cannot flush final exact group");
    Require(groupSizes == std::vector<std::size_t>({2, 3}), "Exact group sizes are incorrect");
    Require(reclaimable == std::vector<std::uint64_t>({100, 500}), "Reclaimable bytes are incorrect");
    Require(accumulator.consumed_members() == 6, "Exact group consumed count is incorrect");
    Require(accumulator.emitted_groups() == 2, "Exact group emitted count is incorrect");
}

/** @brief 视频必须固定生成六帧 dHash 和一张 2x3 拼图，并正确排除静态画面。 */
void TestMediaAnalysisAndContactSheet() {
    const std::filesystem::path directory = CreateTestDirectory(L"media-analysis");
    const std::filesystem::path ffmpeg =
        std::filesystem::current_path() / L"third_party" / L"ffmpeg" / L"bin" / L"ffmpeg.exe";
    Require(std::filesystem::exists(ffmpeg), "FFmpeg test executable is missing");
    const std::filesystem::path dynamicVideo = directory / L"dynamic.mp4";
    const std::filesystem::path staticVideo = directory / L"static.mp4";
    RunProcess(ffmpeg,
               L"-hide_banner -loglevel error -y -f lavfi -i "
               L"\"nullsrc=size=320x180:rate=10:duration=7,geq=lum='random(1)*255':cb=128:cr=128\" "
               L"-c:v libx264 -pix_fmt yuv420p \"" +
                   dynamicVideo.wstring() + L"\"",
               30000);
    RunProcess(ffmpeg,
               L"-hide_banner -loglevel error -y -f lavfi -i "
               L"\"color=c=red:size=320x180:rate=10:duration=7\" -c:v libx264 -pix_fmt yuv420p \"" +
                   staticVideo.wstring() + L"\"",
               30000);

    const std::filesystem::path dynamicSheet = directory / L"dynamic-sheet.jpg";
    const std::filesystem::path staticSheet = directory / L"static-sheet.jpg";
    const std::string dynamicUtf8 = WideToUtf8(dynamicVideo.wstring());
    const std::string staticUtf8 = WideToUtf8(staticVideo.wstring());
    const std::string dynamicSheetUtf8 = WideToUtf8(dynamicSheet.wstring());
    const std::string staticSheetUtf8 = WideToUtf8(staticSheet.wstring());
    VideoScMediaOptions options{};
    options.structSize = sizeof(options);
    options.mediaKindHint = VIDEOSC_MEDIA_VIDEO;
    options.contactSheetCellLongEdge = 160;
    options.ffmpegThreadCount = 1;
    options.noProgressTimeoutMilliseconds = 10000;
    options.contactSheetPath = dynamicSheetUtf8.c_str();
    VideoScMediaResult dynamicResult{};
    Require(AnalyzeMediaFile(dynamicUtf8.c_str(), &options, &dynamicResult) != 0,
            dynamicResult.errorMessage == nullptr ? "Dynamic media analysis failed" : dynamicResult.errorMessage);
    Require(dynamicResult.hasVideoDHashes != 0, "Dynamic video has no six-frame dHash");
    Require(dynamicResult.staticVisual == 0, "Dynamic video was marked static");
    Require(dynamicResult.durationMilliseconds >= 6900 && dynamicResult.durationMilliseconds <= 7100,
            "Dynamic video duration is incorrect");
    Require(std::filesystem::exists(dynamicSheet) && std::filesystem::file_size(dynamicSheet) > 0,
            "Dynamic contact sheet was not generated");
    FreeVideoScMediaResult(&dynamicResult);

    options.contactSheetPath = staticSheetUtf8.c_str();
    VideoScMediaResult staticResult{};
    Require(AnalyzeMediaFile(staticUtf8.c_str(), &options, &staticResult) != 0,
            staticResult.errorMessage == nullptr ? "Static media analysis failed" : staticResult.errorMessage);
    Require(staticResult.hasVideoDHashes != 0, "Static video has no six-frame dHash");
    Require(staticResult.staticVisual != 0, "Static video was not marked static");
    Require(std::filesystem::exists(staticSheet) && std::filesystem::file_size(staticSheet) > 0,
            "Static contact sheet was not generated");
    FreeVideoScMediaResult(&staticResult);
    std::filesystem::remove_all(directory);
}

/** @brief 截断 TIFF 必须安全返回失败，不能触发 FFmpeg 进程级终止。 */
void TestCorruptImageDHashFailsSafely() {
    const std::filesystem::path directory = CreateTestDirectory(L"corrupt-image-dhash");
    const std::filesystem::path ffmpeg =
        std::filesystem::current_path() / L"third_party" / L"ffmpeg" / L"bin" / L"ffmpeg.exe";
    Require(std::filesystem::exists(ffmpeg), "FFmpeg test executable is missing");

    const std::filesystem::path validTiff = directory / L"valid.tiff";
    const std::filesystem::path corruptTiff = directory / L"truncated.tiff";
    RunProcess(ffmpeg,
               L"-hide_banner -loglevel error -y -f lavfi -i "
               L"\"color=c=blue:size=1280x720\" -frames:v 1 -c:v tiff "
               L"-compression_algo raw -update 1 \"" +
                   validTiff.wstring() + L"\"",
               30000);

    char validHash[17]{};
    const std::string validUtf8 = WideToUtf8(validTiff.wstring());
    Require(ComputeImageDHash(validUtf8.c_str(), validHash, sizeof(validHash)) != 0,
            "Valid TIFF did not produce a dHash");
    Require(std::string(validHash).size() == 16, "Valid TIFF dHash length is incorrect");

    std::string truncatedBytes = ReadFile(validTiff);
    Require(truncatedBytes.size() > 262144, "Generated TIFF is too small for truncation fixture");
    truncatedBytes.resize(262144);
    WriteFile(corruptTiff, truncatedBytes);

    char hash[17] = "xxxxxxxxxxxxxxxx";
    const std::string corruptUtf8 = WideToUtf8(corruptTiff.wstring());
    Require(ComputeImageDHash(corruptUtf8.c_str(), hash, sizeof(hash)) == 0,
            "Truncated TIFF unexpectedly produced a dHash");
    Require(hash[0] == '\0', "Failed dHash left stale output data");
    std::filesystem::remove_all(directory);
}

/** @brief 图片预览接口必须使用 FFmpeg 兼容 WebP，并输出不超过配置最长边的临时缩略图。 */
void TestImagePreviewGeneration() {
    const std::filesystem::path directory = CreateTestDirectory(L"image-preview");
    const std::filesystem::path ffmpeg =
        std::filesystem::current_path() / L"third_party" / L"ffmpeg" / L"bin" / L"ffmpeg.exe";
    Require(std::filesystem::exists(ffmpeg), "FFmpeg test executable is missing");
    const std::filesystem::path source = directory / L"source.webp";
    const std::filesystem::path preview = directory / L"preview.png";
    RunProcess(ffmpeg,
               L"-hide_banner -loglevel error -y -f lavfi -i "
               L"\"testsrc2=size=1280x720:rate=1\" -frames:v 1 \"" +
                   source.wstring() + L"\"",
               30000);

    const std::string sourceUtf8 = WideToUtf8(source.wstring());
    const std::string previewUtf8 = WideToUtf8(preview.wstring());
    VideoScImagePreviewOptions options{};
    options.structSize = sizeof(options);
    options.maximumLongEdge = 192;
    options.ffmpegThreadCount = 1;
    options.noProgressTimeoutMilliseconds = 10000;
    VideoScImagePreviewResult result{};
    Require(GenerateImagePreview(sourceUtf8.c_str(), previewUtf8.c_str(), &options, &result) != 0,
            result.errorMessage == nullptr ? "Image preview generation failed" : result.errorMessage);
    Require(result.statusCode == VIDEOSC_OK, "Image preview returned a non-success status");
    Require((std::max)(result.width, result.height) <= options.maximumLongEdge,
            "Image preview exceeded configured long edge");
    Require(std::filesystem::exists(preview) && std::filesystem::file_size(preview) > 0,
            "Image preview file was not created");
    FreeVideoScImagePreviewResult(&result);

    VideoScImagePreviewResult invalid{};
    Require(GenerateImagePreview(nullptr, previewUtf8.c_str(), &options, &invalid) == 0 &&
                invalid.statusCode == VIDEOSC_ERR_INVALID_ARG,
            "Image preview invalid argument boundary changed");
    FreeVideoScImagePreviewResult(&invalid);
    std::filesystem::remove_all(directory);
}

/** @brief 图片 V1 接口必须单次返回完整感知特征，并能对相同内容得到满分结构证据。 */
void TestImagePerceptualAndStructuralApis() {
    const std::filesystem::path directory = CreateTestDirectory(L"image-three-stage-api");
    const std::filesystem::path ffmpeg =
        std::filesystem::current_path() / L"third_party" / L"ffmpeg" / L"bin" / L"ffmpeg.exe";
    Require(std::filesystem::exists(ffmpeg), "FFmpeg test executable is missing");
    const std::filesystem::path source = directory / L"source.png";
    RunProcess(ffmpeg,
               L"-hide_banner -loglevel error -y -f lavfi -i "
               L"\"testsrc2=size=640x360:rate=1\" -frames:v 1 \"" +
                   source.wstring() + L"\"",
               30000);
    const std::string sourceUtf8 = WideToUtf8(source.wstring());

    VideoScImageFeatureOptionsV1 featureOptions{};
    featureOptions.structSize = sizeof(featureOptions);
    featureOptions.ffmpegThreadCount = 1;
    featureOptions.noProgressTimeoutMilliseconds = 10000;
    VideoScImageFeatureResultV1 features{};
    features.structSize = sizeof(features);
    Require(AnalyzeImagePerceptualFeaturesV1(sourceUtf8.c_str(), &featureOptions, &features) != 0,
            features.errorMessage == nullptr ? "Image perceptual analysis failed" : features.errorMessage);
    Require(features.width == 640 && features.height == 360 && features.hasPdqHash != 0 &&
                features.hasZonedPHashes != 0 &&
                features.perceptualAlgorithmVersion == VIDEOSC_IMAGE_PERCEPTUAL_ALGORITHM_VERSION,
            "Image perceptual API returned incomplete features");
    FreeVideoScImageFeatureResultV1(&features);

    VideoScImageStructureOptionsV1 structureOptions{};
    structureOptions.structSize = sizeof(structureOptions);
    structureOptions.ffmpegThreadCount = 1;
    structureOptions.noProgressTimeoutMilliseconds = 10000;
    VideoScImageStructureResultV1 left{};
    left.structSize = sizeof(left);
    VideoScImageStructureResultV1 right{};
    right.structSize = sizeof(right);
    Require(LoadImageStructureV1(sourceUtf8.c_str(), &structureOptions, &left) != 0 &&
                LoadImageStructureV1(sourceUtf8.c_str(), &structureOptions, &right) != 0,
            "Cannot load identical image structures");
    VideoScImageStructureCompareOptionsV1 compareOptions{};
    compareOptions.structSize = sizeof(compareOptions);
    compareOptions.blockPassScore = 0.9;
    VideoScImageStructureCompareResultV1 compared{};
    compared.structSize = sizeof(compared);
    Require(CompareImageStructuresV1(
                left.structureHandle, right.structureHandle, &compareOptions, &compared) != 0,
            compared.errorMessage == nullptr ? "Image structure comparison failed" : compared.errorMessage);
    Require(compared.globalEdgeZnccMillionths == 1'000'000 &&
                compared.trimmedBlockScoreMillionths == 1'000'000 &&
                compared.passingBlockPercentMillionths == 1'000'000,
            "Identical image structures did not produce full scores");
    FreeVideoScImageStructureCompareResultV1(&compared);
    FreeVideoScImageStructureResultV1(&left);
    FreeVideoScImageStructureResultV1(&right);

    std::atomic_bool cancelRequested{false};
    videosc::dedup::StructuralVerificationCache structureCache(
        8U * 1024U * 1024U, 1, 10000, cancelRequested);
    const std::vector<videosc::dedup::StructuralPathCandidate> paths{
        {WideToUtf8((directory / L"missing-first-path.png").wstring()), "disk-0"},
        {sourceUtf8, "disk-0"}};
    const auto cached = structureCache.Get(std::string(128, 'a'), paths);
    Require(cached.structure != nullptr && cached.selected_path_utf8 == sourceUtf8,
            "Structural cache did not retry the readable alternate path");
    Require(structureCache.stats().path_retries == 1 && structureCache.stats().decodes == 2,
            "Structural cache retry counters differ");
    std::filesystem::remove_all(directory);
}

/** @brief PDQ、长宽比和 4×4 pHash 规则必须在边界值上稳定接受或拒绝。 */
void TestImageSimilarityRules() {
    videosc::dedup::ImageSimilarityConfig config;
    ShaFileData left;
    left.media_kind = videosc::dedup::MediaKind::Image;
    left.width = 1000;
    left.height = 1000;
    left.image_pdq_hash = videosc::dedup::PdqHash256{};
    left.image_pdq_quality = static_cast<std::uint8_t>(100);
    left.has_image_zoned_phashes = true;
    left.image_perceptual_algorithm_version = 1;
    left.image_structural_algorithm_version = 1;
    ShaFileData right = left;
    right.width = 1009;
    Require(videosc::dedup::ImageSimilarityRules::HasCompleteFeatures(left),
            "Complete image features were rejected");
    Require(videosc::dedup::ImageSimilarityRules::AspectRatiosCompatible(left, right, 1),
            "One-percent aspect ratio boundary was rejected");
    right.width = 1020;
    Require(!videosc::dedup::ImageSimilarityRules::AspectRatiosCompatible(left, right, 1),
            "Aspect ratio outside one percent was accepted");

    right = left;
    for (std::size_t bit = 0; bit < 31; ++bit) {
        (*right.image_pdq_hash)[bit / 8] ^= static_cast<std::uint8_t>(1U << (bit % 8));
    }
    Require(videosc::dedup::ImageSimilarityRules::PdqHammingDistance(
                *left.image_pdq_hash, *right.image_pdq_hash) == 31,
            "PDQ distance 31 was calculated incorrectly");
    (*right.image_pdq_hash)[31 / 8] ^= static_cast<std::uint8_t>(1U << (31 % 8));
    Require(videosc::dedup::ImageSimilarityRules::PdqHammingDistance(
                *left.image_pdq_hash, *right.image_pdq_hash) == 32,
            "PDQ distance 32 was calculated incorrectly");
    Require(videosc::dedup::ImageSimilarityRules::PdqHammingDistance(
                *left.image_pdq_hash, *right.image_pdq_hash, true) == 32,
            "Scalar PDQ distance path differs from runtime-optimized path");

    right = left;
    for (std::size_t tile = 0; tile < 12; ++tile) right.image_zoned_phashes[tile] = 0xffULL;
    for (std::size_t tile = 12; tile < 16; ++tile) right.image_zoned_phashes[tile] = 0xfffffULL;
    const auto accepted = videosc::dedup::ImageSimilarityRules::CompareZonedPHashes(
        left, right, config.standard_profile, config.force_scalar_kernels);
    Require(accepted.accepted && accepted.passing_tiles == 12 &&
                accepted.trimmed_distance_sum == 96,
            "Zoned pHash 12-of-16 boundary was rejected");
    right.image_zoned_phashes[11] = 0x7ffULL;
    Require(!videosc::dedup::ImageSimilarityRules::CompareZonedPHashes(
                 left, right, config.standard_profile, config.force_scalar_kernels).accepted,
            "Zoned pHash with only 11 passing tiles was accepted");

    right = left;
    right.image_pdq_quality = static_cast<std::uint8_t>(config.pdq_min_quality - 1);
    videosc::dedup::ImageQualityClass qualityClass =
        videosc::dedup::ImageQualityClass::Standard;
    const auto& lowQualityProfile = videosc::dedup::ImageSimilarityRules::SelectThresholdProfile(
        left, right, config, qualityClass);
    Require(qualityClass == videosc::dedup::ImageQualityClass::LowQuality &&
                &lowQualityProfile == &config.low_quality_profile,
            "Low-PDQ-quality image did not select the strict threshold profile");

    AppConfig appConfig = AppConfig::CreateDefault(L"C:\\VideoSc-Test");
    appConfig.image_similarity.low_quality_profile.pdq_max_hamming_distance =
        appConfig.image_similarity.standard_profile.pdq_max_hamming_distance + 1;
    Require(ConfigValidator::HasErrors(ConfigValidator::Validate(appConfig)),
            "A weaker low-quality threshold profile was accepted");
    appConfig = AppConfig::CreateDefault(L"C:\\VideoSc-Test");
    appConfig.image_similarity.low_quality_profile.fallback_dhash_max_hamming_distance =
        appConfig.image_similarity.standard_profile.fallback_dhash_max_hamming_distance + 1;
    Require(ConfigValidator::HasErrors(ConfigValidator::Validate(appConfig)),
            "A weaker low-quality dHash fallback threshold was accepted");
}

/** @brief 三级验收结果，用于稳定图片语料的逐级断言与诊断。 */
struct ImageFixtureComparison {
    bool primary_accepted = false;
    bool secondary_accepted = false;
    bool structural_accepted = false;
    std::uint32_t pdq_distance = 0;
    std::uint32_t dhash_distance = 0;
    std::uint32_t zoned_passing_tiles = 0;
    std::uint32_t structural_global = 0;
    std::uint32_t structural_trimmed = 0;
    std::uint32_t structural_passing_percent = 0;
};

/** @brief 读取已纳入仓库的固定图片，不允许测试运行时生成验收输入。 */
std::filesystem::path ImageSimilarityAsset(const wchar_t* fileName) {
    std::filesystem::path directory =
        std::filesystem::path(__FILE__).parent_path() / L"assets" / L"image_similarity";
    if (!std::filesystem::exists(directory)) {
        directory = std::filesystem::current_path() / L"DedupTests" / L"assets" /
                    L"image_similarity";
    }
    const std::filesystem::path path = directory / fileName;
    Require(std::filesystem::is_regular_file(path), "Image similarity fixture is missing");
    return path;
}

/** @brief 使用生产图片入口读取一份固定语料的 PDQ 与分区 pHash。 */
ShaFileData AnalyzeImageFixture(const std::filesystem::path& path) {
    VideoScImageFeatureOptionsV1 options{};
    options.structSize = sizeof(options);
    options.ffmpegThreadCount = 1;
    options.noProgressTimeoutMilliseconds = 10000;
    VideoScImageFeatureResultV1 features{};
    features.structSize = sizeof(features);
    const std::string pathUtf8 = WideToUtf8(path.wstring());
    if (AnalyzeImagePerceptualFeaturesV1(pathUtf8.c_str(), &options, &features) == 0) {
        const std::string error = features.errorMessage == nullptr
                                      ? "Cannot analyze image similarity fixture"
                                      : features.errorMessage;
        FreeVideoScImageFeatureResultV1(&features);
        throw std::runtime_error(error);
    }
    ShaFileData result;
    result.media_kind = videosc::dedup::MediaKind::Image;
    result.width = features.width;
    result.height = features.height;
    if (features.hasImageDHash != 0) result.image_dhash = features.imageDHash;
    videosc::dedup::PdqHash256 pdq{};
    std::copy(std::begin(features.pdqHash), std::end(features.pdqHash), pdq.begin());
    result.image_pdq_hash = pdq;
    result.image_pdq_quality = features.pdqQuality;
    std::copy(std::begin(features.zonedPHashes),
              std::end(features.zonedPHashes),
              result.image_zoned_phashes.begin());
    result.has_image_zoned_phashes = features.hasZonedPHashes != 0;
    result.image_perceptual_algorithm_version = features.perceptualAlgorithmVersion;
    result.image_structural_algorithm_version = VIDEOSC_IMAGE_STRUCTURAL_ALGORITHM_VERSION;
    FreeVideoScImageFeatureResultV1(&features);
    Require(videosc::dedup::ImageSimilarityRules::HasCompleteFeatures(result),
            "Image similarity fixture produced incomplete features");
    return result;
}

/** @brief 对固定语料执行与报告相同的 PDQ、pHash 和结构三筛。 */
ImageFixtureComparison CompareImageFixtures(const std::filesystem::path& leftPath,
                                            const std::filesystem::path& rightPath,
                                            const videosc::dedup::ImageSimilarityConfig& config) {
    const ShaFileData left = AnalyzeImageFixture(leftPath);
    const ShaFileData right = AnalyzeImageFixture(rightPath);
    videosc::dedup::ImageQualityClass qualityClass{};
    const auto& profile = videosc::dedup::ImageSimilarityRules::SelectThresholdProfile(
        left, right, config, qualityClass);
    ImageFixtureComparison comparison;
    comparison.pdq_distance = videosc::dedup::ImageSimilarityRules::PdqHammingDistance(
        *left.image_pdq_hash, *right.image_pdq_hash, config.force_scalar_kernels);
    comparison.dhash_distance = videosc::dedup::DHashRules::HammingDistance(
        *left.image_dhash, *right.image_dhash);
    comparison.primary_accepted =
        videosc::dedup::ImageSimilarityRules::AspectRatiosCompatible(
            left, right, config.aspect_ratio_tolerance_percent) &&
        (comparison.pdq_distance <= profile.pdq_max_hamming_distance ||
         comparison.dhash_distance <= profile.fallback_dhash_max_hamming_distance);
    const auto zoned = videosc::dedup::ImageSimilarityRules::CompareZonedPHashes(
        left, right, profile, config.force_scalar_kernels);
    comparison.zoned_passing_tiles = zoned.passing_tiles;
    comparison.secondary_accepted = comparison.primary_accepted && zoned.accepted;

    VideoScImageStructureOptionsV1 structureOptions{};
    structureOptions.structSize = sizeof(structureOptions);
    structureOptions.ffmpegThreadCount = 1;
    structureOptions.noProgressTimeoutMilliseconds = 10000;
    VideoScImageStructureResultV1 leftStructure{};
    leftStructure.structSize = sizeof(leftStructure);
    VideoScImageStructureResultV1 rightStructure{};
    rightStructure.structSize = sizeof(rightStructure);
    const std::string leftUtf8 = WideToUtf8(leftPath.wstring());
    const std::string rightUtf8 = WideToUtf8(rightPath.wstring());
    if (LoadImageStructureV1(leftUtf8.c_str(), &structureOptions, &leftStructure) == 0 ||
        LoadImageStructureV1(rightUtf8.c_str(), &structureOptions, &rightStructure) == 0) {
        FreeVideoScImageStructureResultV1(&leftStructure);
        FreeVideoScImageStructureResultV1(&rightStructure);
        throw std::runtime_error("Cannot load image similarity fixture structure");
    }
    VideoScImageStructureCompareOptionsV1 compareOptions{};
    compareOptions.structSize = sizeof(compareOptions);
    compareOptions.blockPassScore =
        static_cast<double>(profile.structural_block_pass_score_millionths) / 1'000'000.0;
    VideoScImageStructureCompareResultV1 structural{};
    structural.structSize = sizeof(structural);
    if (CompareImageStructuresV1(leftStructure.structureHandle,
                                 rightStructure.structureHandle,
                                 &compareOptions,
                                 &structural) == 0) {
        FreeVideoScImageStructureResultV1(&leftStructure);
        FreeVideoScImageStructureResultV1(&rightStructure);
        const std::string error = structural.errorMessage == nullptr
                                      ? "Cannot compare image similarity fixture structures"
                                      : structural.errorMessage;
        FreeVideoScImageStructureCompareResultV1(&structural);
        throw std::runtime_error(error);
    }
    comparison.structural_global = structural.globalEdgeZnccMillionths;
    comparison.structural_trimmed = structural.trimmedBlockScoreMillionths;
    comparison.structural_passing_percent = structural.passingBlockPercentMillionths;
    comparison.structural_accepted =
        comparison.secondary_accepted &&
        structural.globalEdgeZnccMillionths >= profile.structural_global_edge_min_millionths &&
        structural.trimmedBlockScoreMillionths >=
            profile.structural_trimmed_block_min_millionths &&
        structural.passingBlockPercentMillionths >=
            profile.structural_min_passing_percent_millionths;
    FreeVideoScImageStructureCompareResultV1(&structural);
    FreeVideoScImageStructureResultV1(&leftStructure);
    FreeVideoScImageStructureResultV1(&rightStructure);
    return comparison;
}

/** @brief 固定正变体必须三级全过，固定难负样本必须至少在一级被拒绝。 */
void TestStableImageSimilarityFixtures() {
    const videosc::dedup::ImageSimilarityConfig config;
    const std::filesystem::path base = ImageSimilarityAsset(L"base.ppm");
    const ShaFileData baseGolden = AnalyzeImageFixture(base);
    const videosc::dedup::PdqHash256 expectedPdq{
        0xf8, 0x00, 0xfc, 0x04, 0xfe, 0x00, 0xaf, 0x15,
        0x93, 0xff, 0x00, 0xff, 0x40, 0x7f, 0x00, 0x3f,
        0x08, 0x1f, 0x2c, 0x86, 0xa2, 0x24, 0xf7, 0xf0,
        0xfb, 0xf8, 0x1d, 0x7c, 0xd4, 0xeb, 0x07, 0x27};
    const std::array<std::uint64_t, 16> expectedZoned{
        16158642715656190982ULL, 5960807020880049236ULL,
        2225093711851761232ULL, 15597030351634156548ULL,
        13084947423771085132ULL, 16281668270034581178ULL,
        503841300284375280ULL, 15913712054341245986ULL,
        13138595183671798852ULL, 11936128586728428628ULL,
        13898373402254510048ULL, 2216191584067612710ULL,
        17357137881511030814ULL, 17361641481138397214ULL,
        17361641481138401294ULL, 5363405243192422420ULL};
    Require(*baseGolden.image_pdq_hash == expectedPdq &&
                *baseGolden.image_pdq_quality == 86 &&
                baseGolden.image_zoned_phashes == expectedZoned,
            "Stable PDQ or zoned-pHash golden changed");
    const std::array<const wchar_t*, 4> positiveNames{
        L"resolution-128x96.ppm", L"brightness.ppm", L"warm-tone.ppm", L"watermark.ppm"};
    std::string positiveFailures;
    for (const wchar_t* name : positiveNames) {
        const ImageFixtureComparison comparison = CompareImageFixtures(
            base, ImageSimilarityAsset(name), config);
        if (!comparison.structural_accepted) {
            if (!positiveFailures.empty()) positiveFailures += "; ";
            positiveFailures += WideToUtf8(std::wstring(name)) +
                                ":pdq=" + std::to_string(comparison.pdq_distance) +
                                ",zoned=" + std::to_string(comparison.zoned_passing_tiles) +
                                ",structure=" + std::to_string(comparison.structural_global) + "/" +
                                std::to_string(comparison.structural_trimmed) + "/" +
                                std::to_string(comparison.structural_passing_percent);
        }
    }
    if (!positiveFailures.empty()) {
        throw std::runtime_error("Positive image fixtures rejected: " + positiveFailures);
    }
    const ImageFixtureComparison negative = CompareImageFixtures(
        base, ImageSimilarityAsset(L"hard-negative.ppm"), config);
    Require(!(negative.primary_accepted && negative.secondary_accepted &&
              negative.structural_accepted),
            "Hard-negative fixture passed all three image similarity stages");
}

/** @brief PDQ 扁平倒排候选必须与小样本全量两两判断完全一致，并严格执行资源上限。 */
void TestPdqCandidateBuilderAgainstBruteForce() {
    videosc::dedup::ImageSimilarityConfig config;
    config.candidate_memory_mib = 16;
    config.candidate_max_pairs = 100000;
    config.hot_signature_max_members = 100;
    config.hot_signature_max_pairs = 10000;
    config.candidate_cancel_check_stride = 1;

    std::vector<videosc::dedup::PdqCandidateRecord> records;
    records.reserve(48);
    for (std::uint32_t index = 0; index < 48; ++index) {
        videosc::dedup::PdqCandidateRecord record;
        record.digest.fill(0);
        record.digest[0] = static_cast<std::uint8_t>(index + 1);
        const std::uint8_t base = index < 16 ? 0x00 : (index < 32 ? 0xff : 0xaa);
        record.pdq.fill(base);
        const std::uint32_t changedBits = (index % 16) * 2;
        for (std::uint32_t bit = 0; bit < changedBits; ++bit) {
            record.pdq[bit / 8] ^= static_cast<std::uint8_t>(1U << (bit % 8));
        }
        record.width = 1600 + (index % 3) * 8;
        record.height = 900;
        record.quality = static_cast<std::uint8_t>(index % 5 == 0 ? 20 : 100);
        records.push_back(record);
    }

    const auto pairKey = [](const Sha512Digest& left, const Sha512Digest& right) {
        std::string leftHex = Sha512ToHex(left);
        std::string rightHex = Sha512ToHex(right);
        if (rightHex < leftHex) std::swap(leftHex, rightHex);
        return leftHex + "/" + rightHex;
    };
    std::set<std::string> actual;
    std::atomic_bool cancelRequested{false};
    const videosc::dedup::PdqCandidateBuildResult built =
        videosc::dedup::PdqCandidateBuilder(config).Build(
            records,
            cancelRequested,
            [&](const Sha512Digest& left, const Sha512Digest& right) {
                return actual.insert(pairKey(left, right)).second;
            });
    Require(built.succeeded && built.candidate_pairs == actual.size(),
            "PDQ candidate builder failed or emitted a duplicate pair");

    std::set<std::string> expected;
    for (std::size_t left = 0; left < records.size(); ++left) {
        for (std::size_t right = left + 1; right < records.size(); ++right) {
            ShaFileData leftData;
            leftData.width = records[left].width;
            leftData.height = records[left].height;
            leftData.image_pdq_quality = records[left].quality;
            ShaFileData rightData;
            rightData.width = records[right].width;
            rightData.height = records[right].height;
            rightData.image_pdq_quality = records[right].quality;
            videosc::dedup::ImageQualityClass qualityClass{};
            const auto& profile = videosc::dedup::ImageSimilarityRules::SelectThresholdProfile(
                leftData, rightData, config, qualityClass);
            if (videosc::dedup::ImageSimilarityRules::AspectRatiosCompatible(
                    leftData, rightData, config.aspect_ratio_tolerance_percent) &&
                videosc::dedup::ImageSimilarityRules::PdqHammingDistance(
                    records[left].pdq, records[right].pdq, config.force_scalar_kernels) <=
                    profile.pdq_max_hamming_distance) {
                expected.insert(pairKey(records[left].digest, records[right].digest));
            }
        }
    }
    Require(actual == expected, "PDQ flat-postings candidates differ from brute force");

    std::vector<videosc::dedup::PdqCandidateRecord> watermarkFallback(2);
    watermarkFallback[0].digest.fill(1);
    watermarkFallback[1].digest.fill(2);
    watermarkFallback[0].pdq.fill(0);
    watermarkFallback[1].pdq.fill(0);
    for (std::uint32_t bit = 0; bit < 44; ++bit) {
        watermarkFallback[1].pdq[bit / 8] ^=
            static_cast<std::uint8_t>(1U << (bit % 8));
    }
    watermarkFallback[0].dhash = 0;
    watermarkFallback[1].dhash = 0b111;
    watermarkFallback[0].has_dhash = true;
    watermarkFallback[1].has_dhash = true;
    watermarkFallback[0].width = watermarkFallback[1].width = 1600;
    watermarkFallback[0].height = watermarkFallback[1].height = 900;
    watermarkFallback[0].quality = watermarkFallback[1].quality = 100;
    std::uint64_t fallbackPairs = 0;
    const auto fallback = videosc::dedup::PdqCandidateBuilder(config).Build(
        watermarkFallback,
        cancelRequested,
        [&](const Sha512Digest&, const Sha512Digest&) {
            ++fallbackPairs;
            return true;
        });
    Require(fallback.succeeded && fallbackPairs == 1,
            "dHash watermark fallback did not recall the PDQ-outside-radius pair");

    watermarkFallback[0].quality = watermarkFallback[1].quality = 20;
    fallbackPairs = 0;
    const auto lowQualityFallback = videosc::dedup::PdqCandidateBuilder(config).Build(
        watermarkFallback,
        cancelRequested,
        [&](const Sha512Digest&, const Sha512Digest&) {
            ++fallbackPairs;
            return true;
        });
    Require(lowQualityFallback.succeeded && fallbackPairs == 0,
            "Low-quality dHash fallback did not apply the stricter distance");

    std::vector<videosc::dedup::PdqCandidateRecord> identical(4, records.front());
    for (std::size_t index = 0; index < identical.size(); ++index) {
        identical[index].digest.fill(static_cast<std::uint8_t>(index + 1));
    }
    auto hotConfig = config;
    hotConfig.hot_signature_max_members = 3;
    const auto hot = videosc::dedup::PdqCandidateBuilder(hotConfig).Build(
        identical, cancelRequested, [](const Sha512Digest&, const Sha512Digest&) { return true; });
    Require(!hot.succeeded && hot.deferred_hot_signatures == 1,
            "Oversized identical PDQ signature was not deferred safely");

    auto pairBudgetConfig = config;
    pairBudgetConfig.candidate_max_pairs = 1;
    const auto budget = videosc::dedup::PdqCandidateBuilder(pairBudgetConfig).Build(
        identical, cancelRequested, [](const Sha512Digest&, const Sha512Digest&) { return true; });
    Require(!budget.succeeded && budget.candidate_pairs == 1,
            "PDQ candidate pair budget was not enforced");

    cancelRequested.store(true, std::memory_order_relaxed);
    const auto cancelled = videosc::dedup::PdqCandidateBuilder(config).Build(
        records, cancelRequested, [](const Sha512Digest&, const Sha512Digest&) { return true; });
    Require(cancelled.cancelled && !cancelled.succeeded,
            "PDQ candidate cancellation was not propagated");
}

/** @brief 图片特征回填检查点必须可恢复、校验并幂等清除。 */
void TestImageFeatureBackfillCheckpoint() {
    const std::filesystem::path directory = CreateTestDirectory(L"image-backfill-checkpoint");
    RocksDbConfig config;
    config.directory = directory / L"rocks";
    config.block_cache_mib = 8;
    config.write_buffer_mib = 4;
    RocksStore store(config);
    Require(store.Open().succeeded, "Cannot open backfill checkpoint RocksDB");
    videosc::dedup::ImageFeatureBackfillCheckpointStore checkpointStore(store);
    videosc::dedup::ImageFeatureBackfillCheckpoint checkpoint;
    checkpoint.algorithm_version = "media-dhash-v2";
    checkpoint.last_sha512 = std::string(128, 'a');
    checkpoint.total_images = 12;
    checkpoint.completed_images = 9;
    checkpoint.failed_images = 3;
    checkpoint.no_readable_path_images = 1;
    checkpoint.timeout_images = 1;
    checkpoint.decode_failed_images = 1;
    checkpoint.started_utc_ms = 100;
    checkpoint.updated_utc_ms = 200;
    Require(checkpointStore.Save(checkpoint).succeeded, "Cannot save image backfill checkpoint");
    std::string error;
    const auto loaded = checkpointStore.Load(error);
    Require(error.empty() && loaded.has_value() &&
                loaded->algorithm_version == checkpoint.algorithm_version &&
                loaded->last_sha512 == checkpoint.last_sha512 &&
                loaded->completed_images == 9 && loaded->failed_images == 3 &&
                loaded->decode_failed_images == 1 && !loaded->finished,
            "Image backfill checkpoint round trip differs");
    Require(checkpointStore.Clear().succeeded, "Cannot clear image backfill checkpoint");
    Require(!checkpointStore.Load(error).has_value() && error.empty(),
            "Cleared image backfill checkpoint is still visible");
    store.Close();
    std::filesystem::remove_all(directory);
}

/** @brief 三筛调度器必须同时收敛 CPU/全局读取线程并限制同物理盘并发。 */
void TestStructuralVerificationSchedulerBudgets() {
    videosc::dedup::ComputeConfig compute;
    compute.worker_threads = 4;
    videosc::dedup::StorageConfig storage;
    storage.max_concurrent_file_reads = 3;
    storage.hdd_read_threads_per_disk = 1;
    std::atomic_bool cancelled{false};
    videosc::dedup::StructuralVerificationScheduler scheduler(
        8, compute, storage, cancelled);
    Require(scheduler.effective_worker_count() == 3,
            "Structural scheduler did not apply the minimum global worker budget");

    std::atomic<std::uint32_t> active{0};
    std::atomic<std::uint32_t> maximumActive{0};
    std::atomic_bool runFailed{false};
    std::vector<std::thread> workers;
    for (std::uint32_t index = 0; index < 6; ++index) {
        workers.emplace_back([&] {
            if (!scheduler.RunWithReadBudget("PhysicalDrive7", [&] {
                        const std::uint32_t current = active.fetch_add(1) + 1;
                        std::uint32_t observed = maximumActive.load();
                        while (current > observed &&
                               !maximumActive.compare_exchange_weak(observed, current)) {
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        active.fetch_sub(1);
                    })) {
                runFailed.store(true);
            }
        });
    }
    for (std::thread& worker : workers) worker.join();
    Require(!runFailed.load(), "Structural scheduler unexpectedly cancelled a disk task");
    Require(maximumActive.load() == 1,
            "Structural scheduler exceeded the per-disk HDD read budget");
    cancelled.store(true);
    Require(!scheduler.RunWithReadBudget("PhysicalDrive7", [] {}),
            "Structural scheduler accepted work after cancellation");
}

/** @brief 动态 n+1 段必须无遗漏召回所有汉明距离不超过 n 的哈希。 */
void TestDHashSegmentationGuarantee() {
    std::uint64_t state = 0x9E3779B97F4A7C15ULL;
    for (std::uint32_t maximumDistance = 0; maximumDistance <= 15; ++maximumDistance) {
        for (std::uint32_t sample = 0; sample < 1000; ++sample) {
            state ^= state << 7;
            state ^= state >> 9;
            const std::uint64_t left = state;
            std::uint64_t right = left;
            const std::uint32_t changes = maximumDistance == 0 ? 0 : sample % (maximumDistance + 1);
            for (std::uint32_t bit = 0; bit < changes; ++bit) {
                const std::uint32_t position = (sample + bit * 17) % 64;
                right ^= 1ULL << position;
            }
            const auto leftSegments = DHashRules::Split(left, maximumDistance);
            const auto rightSegments = DHashRules::Split(right, maximumDistance);
            Require(leftSegments.size() == maximumDistance + 1 &&
                        rightSegments.size() == maximumDistance + 1,
                    "Dynamic MIH segment count differs");
            std::uint32_t totalBits = 0;
            bool shared = false;
            for (std::size_t index = 0; index < leftSegments.size(); ++index) {
                totalBits += leftSegments[index].bit_count;
                Require(leftSegments[index].index == index &&
                            leftSegments[index].bit_count == rightSegments[index].bit_count,
                        "Dynamic MIH segment layout is unstable");
                if (leftSegments[index].value == rightSegments[index].value) shared = true;
            }
            Require(totalBits == 64, "Dynamic MIH segments do not cover 64 bits");
            Require(shared, "Dynamic MIH segmentation missed a distance <= n pair");
            Require(DHashRules::ImagesAreSimilar(left, right, maximumDistance),
                    "Configured image distance boundary is incorrect");
        }
    }
    Require(!DHashRules::ImagesAreSimilar(0, 0x1FULL), "Image distance 5 was accepted");
    bool rejected = false;
    try {
        (void)DHashRules::Split(0, 64);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    Require(rejected, "Dynamic MIH accepted an empty-segment layout");
}

/** @brief 完整链接分组必须拒绝仅通过中间成员连通的图片内容。 */
void TestStrictImageCompleteLinkGrouping() {
    const std::vector<std::uint64_t> hashes = {0x0ULL, 0xFULL, 0xFFULL};
    std::vector<std::pair<std::size_t, std::size_t>> acceptedEdges;
    for (std::size_t left = 0; left < hashes.size(); ++left) {
        for (std::size_t right = left + 1; right < hashes.size(); ++right) {
            if (DHashRules::ImagesAreSimilar(hashes[left], hashes[right], 4)) {
                acceptedEdges.emplace_back(left, right);
            }
        }
    }
    std::vector<std::vector<std::uint64_t>> groups;
    for (const std::uint64_t hash : hashes) {
        bool assigned = false;
        for (std::vector<std::uint64_t>& group : groups) {
            const bool accepted = std::all_of(
                group.begin(),
                group.end(),
                [&](const std::uint64_t member) {
                    return DHashRules::ImagesAreSimilar(hash, member, 4);
                });
            if (!accepted) continue;
            group.push_back(hash);
            assigned = true;
            break;
        }
        if (!assigned) groups.push_back({hash});
    }
    Require(groups.size() == 2 && groups.front().size() == 2 && groups.back().size() == 1,
            "Complete-link grouping accepted a transitive-only image chain");
    Require(acceptedEdges.size() == 2 &&
                acceptedEdges[0] == std::pair<std::size_t, std::size_t>{0, 1} &&
                acceptedEdges[1] == std::pair<std::size_t, std::size_t>{1, 2},
            "Direct image relations were lost while enforcing strict groups");
    Require(DHashRules::ImagesAreSimilar(groups.front().back(), groups.back().front(), 4),
            "Expected cross-group direct similarity relation is missing");

    const std::array<std::uint64_t, 4> reported = {
        0x68e0e0e0f0f0f0f0ULL,
        0xe0e0f2b0a0e0e0e0ULL,
        0xe4e0e0f0f0f0f8e8ULL,
        0xf8fcd4f8f0f0f8f8ULL,
    };
    for (std::size_t left = 0; left < reported.size(); ++left) {
        for (std::size_t right = left + 1; right < reported.size(); ++right) {
            Require(!DHashRules::ImagesAreSimilar(reported[left], reported[right], 4),
                    "User-reported non-similar dHash pair was accepted");
        }
    }
}

/** @brief 视频真实比较必须同时执行时长、静态画面和六组平均距离规则。 */
void TestVideoSimilarityRules() {
    ShaFileData left;
    left.media_kind = videosc::dedup::MediaKind::Video;
    left.has_video_dhashes = true;
    left.video_duration_ms = 10000;
    left.video_dhashes = {0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0};
    Require(videosc::dedup::ClassifyVisualDHash(left) == videosc::dedup::VisualDHashStatus::Valid,
            "Complete video dHash was classified invalid");
    ShaFileData missingVideoDHash = left;
    missingVideoDHash.has_video_dhashes = false;
    Require(videosc::dedup::ClassifyVisualDHash(missingVideoDHash) ==
                videosc::dedup::VisualDHashStatus::MissingVideo,
            "Missing video dHash flag was not classified");
    ShaFileData zeroFrameVideo = left;
    zeroFrameVideo.video_dhashes[3] = 0;
    Require(videosc::dedup::ClassifyVisualDHash(zeroFrameVideo) ==
                videosc::dedup::VisualDHashStatus::ZeroVideoFrame,
            "Zero video dHash frame was not classified");
    ShaFileData invalidImage;
    invalidImage.media_kind = videosc::dedup::MediaKind::Image;
    invalidImage.image_dhash = 0;
    Require(videosc::dedup::ClassifyVisualDHash(invalidImage) ==
                videosc::dedup::VisualDHashStatus::InvalidImage,
            "Zero image dHash was not classified");
    ShaFileData right = left;
    right.video_duration_ms = 12000;
    right.video_dhashes = {0xFFULL, 0xFFULL, 0xFFULL, 0xFFULL, 0xFFULL, 0xFFULL};
    const auto evidence = DHashRules::CompareVideos(left, right, 1, 2);
    Require(evidence.has_value(), "Average video distance 4 was rejected");
    Require(evidence->average_hamming_distance == 4.0 && evidence->duration_difference_ms == 2000,
            "Video similarity evidence is incorrect");
    right.video_duration_ms = 12001;
    Require(!DHashRules::CompareVideos(left, right).has_value(), "Duration difference above 2 seconds was accepted");
    right.video_duration_ms = 10000;
    right.video_dhashes = {0x1FULL, 0x1FULL, 0x1FULL, 0x1FULL, 0x1FULL, 0x1FULL};
    Require(!DHashRules::CompareVideos(left, right).has_value(), "Average video distance 5 was accepted");
    right = left;
    right.video_dhashes[3] = 0;
    Require(!DHashRules::CompareVideos(left, right).has_value(), "Video with one zero dHash frame was accepted");
    left.static_visual = true;
    Require(!DHashRules::CompareVideos(left, right).has_value(), "Static video entered content comparison");
    Require(DHashRules::IsStaticVisual({1, 1, 1, 1, 1, 1}), "Identical video frames were not static");
}

/** @brief RocksDB 图片/视频候选索引必须召回真实相似内容并去重多段命中。 */
void TestDHashCandidateIndex() {
    const std::filesystem::path directory = CreateTestDirectory(L"dhash-index");
    RocksDbConfig config;
    config.directory = directory / L"rocks";
    config.block_cache_mib = 8;
    config.write_buffer_mib = 4;
    RocksStore store(config);
    Require(store.Open().succeeded, "Cannot open dHash index RocksDB");
    DHashCandidateIndex index(store, "media-dhash-v2", "n7", 7);

    ShaFileData image;
    image.media_kind = videosc::dedup::MediaKind::Image;
    image.image_dhash = 0x123456789ABCDEF0ULL;
    image.sha512[0] = 1;
    Require(index.AddImage(image).succeeded, "Cannot add image dHash index");
    Sha512Digest imageQuerySha{};
    imageQuerySha[0] = 2;
    std::unordered_set<std::string> imageCandidates;
    Require(index.FindImageCandidates(*image.image_dhash ^ 0x7FULL,
                                      imageQuerySha,
                                      [&](const Sha512Digest& digest) {
                                          imageCandidates.insert(Sha512ToHex(digest));
                                          return true;
                                      }).succeeded,
            "Cannot query image dHash index");
    Require(imageCandidates.size() == 1 && imageCandidates.count(Sha512ToHex(image.sha512)) == 1,
            "Image candidate was missed or duplicated");
    DHashCandidateIndex isolatedIndex(store, "media-dhash-v2", "n7", 4);
    std::uint32_t isolatedCallbacks = 0;
    Require(isolatedIndex.FindImageCandidates(*image.image_dhash,
                                              imageQuerySha,
                                              [&](const Sha512Digest&) {
                                                  ++isolatedCallbacks;
                                                  return true;
                                              }).succeeded,
            "Cannot query isolated image dHash index");
    Require(isolatedCallbacks == 0, "Different n values shared the same image bucket namespace");
    ShaFileData duplicateSignature = image;
    duplicateSignature.sha512[0] = 9;
    Require(index.AddImage(duplicateSignature).succeeded, "Cannot overwrite identical visual signature index");
    Sha512Digest thirdQuerySha{};
    thirdQuerySha[0] = 10;
    std::uint32_t identicalSignatureCallbacks = 0;
    Sha512Digest identicalSignatureCandidate{};
    Require(index.FindImageCandidates(*image.image_dhash,
                                      thirdQuerySha,
                                      [&](const Sha512Digest& digest) {
                                          ++identicalSignatureCallbacks;
                                          identicalSignatureCandidate = digest;
                                          return true;
                                      }).succeeded,
            "Cannot query compressed identical visual signature");
    Require(identicalSignatureCallbacks == 1 && identicalSignatureCandidate == duplicateSignature.sha512,
            "Popular identical dHash was not compressed to one candidate signature");

    ShaFileData video;
    video.media_kind = videosc::dedup::MediaKind::Video;
    video.has_video_dhashes = true;
    video.video_duration_ms = 10000;
    video.video_dhashes = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
    video.sha512[0] = 3;
    Require(index.AddVideo(video).succeeded, "Cannot add video dHash index");
    ShaFileData invalidVideo = video;
    invalidVideo.video_dhashes[2] = 0;
    Require(!index.AddVideo(invalidVideo).succeeded,
            "Video candidate index accepted a zero dHash frame");
    ShaFileData videoQuery = video;
    videoQuery.sha512[0] = 4;
    videoQuery.video_duration_ms = 12000;
    for (std::uint64_t& hash : videoQuery.video_dhashes) hash ^= 0x0FULL;
    std::unordered_set<std::string> videoCandidates;
    Require(index.FindVideoCandidates(videoQuery, [&](const Sha512Digest& digest) {
                videoCandidates.insert(Sha512ToHex(digest));
                return true;
            }).succeeded,
            "Cannot query video dHash index");
    Require(videoCandidates.size() == 1 && videoCandidates.count(Sha512ToHex(video.sha512)) == 1,
            "Video candidate was missed or duplicated");
    store.Close();
    std::filesystem::remove_all(directory);
}

/** @brief 手动相似报告必须用磁盘候选索引分组，并只在完整生成后发布分页结果。 */
void TestPersistentSimilarityReport() {
    const std::filesystem::path directory = CreateTestDirectory(L"similar-report");
    RocksDbConfig config;
    config.directory = directory / L"rocks";
    config.block_cache_mib = 8;
    config.write_buffer_mib = 4;
    RocksStore store(config);
    Require(store.Open().succeeded, "Cannot open similarity report RocksDB");

    DuplicateReportStore reportStore(store);
    const std::uint64_t generation = 77;
    videosc::dedup::DuplicateGroup group;
    group.group_id = 1;
    group.kind = videosc::dedup::DuplicateGroupKind::SimilarImage;
    group.algorithm_version = "media-dhash-v2";
    DuplicateMember left;
    left.path_id = 10;
    left.path = directory / L"left.jpg";
    left.storage_target_key = L"PhysicalDrive1";
    left.size_bytes = 100;
    left.media_kind = videosc::dedup::MediaKind::Image;
    left.image_dhash = 0x0123456789abcdefULL;
    left.content_sha512.fill(0x11);
    DuplicateMember right = left;
    right.path_id = 11;
    right.path = directory / L"right.jpg";
    right.storage_target_key = L"PhysicalDrive2";
    right.size_bytes = 120;
    right.content_sha512.fill(0x22);
    right.image_dhash = 0x0123456789abcdeeULL;
    group.members = {left, right};
    group.reclaimable_bytes = 100;
    videosc::dedup::SimilarityEvidence evidence;
    evidence.left_path_id = 10;
    evidence.right_path_id = 11;
    evidence.compared_frame_count = 1;
    evidence.average_hamming_distance = 2.0;
    evidence.image_pdq_hamming_distance = 2;
    evidence.image_dhash_hamming_distance = 3;
    evidence.image_used_dhash_fallback = true;
    evidence.image_zoned_passing_tiles = 14;
    evidence.image_zoned_ignored_tiles = 2;
    evidence.image_zoned_trimmed_distance_sum = 56;
    evidence.image_zoned_retained_tiles = 14;
    evidence.image_global_edge_zncc_millionths = 950000;
    evidence.image_trimmed_block_score_millionths = 960000;
    evidence.image_passing_block_percent_millionths = 875000;
    group.evidence.push_back(evidence);

    Require(reportStore.SaveGroup(DuplicateReportKind::Similar, generation, 0, group).succeeded,
            "Cannot save unpublished similarity report group");

    videosc::dedup::DuplicateGroup videoGroup;
    videoGroup.group_id = 2;
    videoGroup.kind = videosc::dedup::DuplicateGroupKind::SimilarVideo;
    videoGroup.algorithm_version = "media-dhash-v2";
    DuplicateMember videoLeft;
    videoLeft.path_id = 20;
    videoLeft.path = directory / L"left.mp4";
    videoLeft.storage_target_key = L"PhysicalDrive1";
    videoLeft.size_bytes = 200;
    videoLeft.media_kind = videosc::dedup::MediaKind::Video;
    videoLeft.has_video_dhashes = true;
    videoLeft.video_dhashes = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
    DuplicateMember videoRight = videoLeft;
    videoRight.path_id = 21;
    videoRight.path = directory / L"right.mp4";
    videoRight.storage_target_key = L"PhysicalDrive2";
    videoRight.size_bytes = 240;
    videoGroup.members = {videoLeft, videoRight};
    videoGroup.reclaimable_bytes = 200;
    Require(reportStore.SaveGroup(DuplicateReportKind::Similar, generation, 1, videoGroup).succeeded,
            "Cannot save video similarity report group");

    videosc::dedup::SimilarReportMetadata metadata;
    metadata.image_max_hamming_distance = 4;
    metadata.validation_worker_threads = 8;
    metadata.image_similarity.standard_profile.pdq_max_hamming_distance = 29;
    metadata.image_similarity.standard_profile.structural_global_edge_min_millionths = 940000;
    metadata.image_similarity.low_quality_profile.pdq_max_hamming_distance = 24;
    metadata.image_similarity.low_quality_profile.structural_global_edge_min_millionths = 950000;
    metadata.structural_worker_threads = 3;
    metadata.structural_cache_mib = 384;
    metadata.image_scope_total = 11;
    metadata.image_features_complete = 11;
    metadata.image_features_incomplete = 0;
    metadata.partial_scope_published = true;
    metadata.candidate_peak_bytes = 123456;
    metadata.candidate_pairs = 9;
    metadata.popcount_path = "portable-swar";
    metadata.generated_utc_ms = 123456789;
    metadata.media_algorithm_version = "media-dhash-v2";
    Require(reportStore.SaveSimilarMetadata(generation, metadata).succeeded,
            "Cannot save similarity report metadata");

    const std::string leftSignature = Sha512ToHex(left.content_sha512);
    const std::string rightSignature = Sha512ToHex(right.content_sha512);
    Require(reportStore.SaveImageSignatureMember(generation, leftSignature, left).succeeded &&
                reportStore.SaveImageSignatureMember(generation, rightSignature, right).succeeded,
            "Cannot save image signature members");
    videosc::dedup::SimilarImageRelationSummary relation;
    relation.current_group_id = group.group_id;
    relation.neighbor_group_id = 99;
    relation.current_representative_sha512 = leftSignature;
    relation.neighbor_representative_sha512 = rightSignature;
    relation.current_image_dhash = *left.image_dhash;
    relation.neighbor_image_dhash = *right.image_dhash;
    relation.hamming_distance = 1;
    relation.neighbor_active_member_count = 1;
    relation.neighbor_group_in_main_report = false;
    Require(reportStore.SaveImageRelation(generation, group.group_id, relation).succeeded,
            "Cannot save cross-group image relation");

    Require(!reportStore.ActiveGeneration(DuplicateReportKind::Similar).has_value(),
            "Incomplete report generation was published");
    Require(reportStore.Publish(DuplicateReportKind::Similar, generation, 2).succeeded,
            "Cannot publish similarity report");
    const auto active = reportStore.ActiveGeneration(DuplicateReportKind::Similar);
    Require(active.has_value() && *active == generation, "Published report generation differs");
    std::vector<videosc::dedup::DuplicateGroup> groups;
    Require(reportStore.LoadPage(DuplicateReportKind::Similar, *active, 0, 20, groups).succeeded,
            "Cannot load similarity report page");
    Require(groups.size() == 2 && groups.front().members.size() == 2 &&
                groups.front().evidence.size() == 1 &&
                groups.front().evidence.front().average_hamming_distance == 2.0 &&
                groups.front().evidence.front().image_pdq_hamming_distance == 2 &&
                groups.front().evidence.front().image_dhash_hamming_distance == 3 &&
                groups.front().evidence.front().image_used_dhash_fallback &&
                groups.front().evidence.front().image_trimmed_block_score_millionths == 960000 &&
                groups.front().members.front().media_kind == videosc::dedup::MediaKind::Image &&
                groups.front().members.front().image_dhash == left.image_dhash &&
                groups.back().members.front().media_kind == videosc::dedup::MediaKind::Video &&
                groups.back().members.front().has_video_dhashes &&
                groups.back().members.front().video_dhashes == videoLeft.video_dhashes,
            "Similarity report members or distance evidence differ");
    std::vector<videosc::dedup::ReportGroupSummary> summaries;
    Require(reportStore.LoadSummaries(DuplicateReportKind::Similar, *active, summaries).succeeded &&
                summaries.size() == 2 && summaries.front().ordinal == 0 &&
                summaries.front().member_count == 2 && summaries.front().reclaimable_bytes == 100 &&
                summaries.back().ordinal == 1 && summaries.back().member_count == 2 &&
                summaries.back().reclaimable_bytes == 200,
            "Similarity report lightweight summary differs");
    videosc::dedup::DuplicateGroup direct;
    Require(reportStore.LoadGroup(DuplicateReportKind::Similar, *active, 1, direct).succeeded &&
                direct.group_id == videoGroup.group_id && direct.members.size() == 2 &&
                direct.members.front().has_video_dhashes &&
                direct.members.front().video_dhashes == videoLeft.video_dhashes,
            "Video similarity report direct group load failed");
    videosc::dedup::SimilarReportMetadata loadedMetadata;
    Require(reportStore.LoadSimilarMetadata(*active, loadedMetadata).succeeded &&
                loadedMetadata.image_max_hamming_distance == 4 &&
                loadedMetadata.validation_worker_threads == 8 &&
                loadedMetadata.image_similarity.standard_profile.pdq_max_hamming_distance == 29 &&
                loadedMetadata.structural_worker_threads == 3 &&
                loadedMetadata.structural_cache_mib == 384 &&
                loadedMetadata.image_scope_total == 11 &&
                loadedMetadata.image_features_complete == 11 &&
                loadedMetadata.image_features_incomplete == 0 &&
                loadedMetadata.partial_scope_published &&
                loadedMetadata.candidate_peak_bytes == 123456 &&
                loadedMetadata.candidate_pairs == 9 &&
                loadedMetadata.popcount_path == "portable-swar" &&
                loadedMetadata.media_algorithm_version == "media-dhash-v2",
            "Similarity report metadata differs");
    // 部分作用域与特征不完整不再影响删除可信：被跳过内容不进入分组，已入组成员证据完整。
    Require(videosc::dedup::IsSimilarReportEligibleForPermanentDeletion(loadedMetadata),
            "Partial-scope V4 three-stage similarity report was incorrectly blocked from permanent deletion");
    loadedMetadata.image_features_incomplete = 3;
    Require(videosc::dedup::IsSimilarReportEligibleForPermanentDeletion(loadedMetadata),
            "V4 three-stage report with incomplete image features was incorrectly blocked");
    loadedMetadata.image_uses_three_stage_verification = false;
    Require(!videosc::dedup::IsSimilarReportEligibleForPermanentDeletion(loadedMetadata),
            "Legacy non-three-stage similarity report was incorrectly trusted for permanent deletion");
    loadedMetadata.image_uses_three_stage_verification = true;
    loadedMetadata.report_schema_version = 3;
    Require(!videosc::dedup::IsSimilarReportEligibleForPermanentDeletion(loadedMetadata),
            "Non-V4 similarity report was incorrectly trusted for permanent deletion");
    Require(reportStore.ImageRelationCount(*active, group.group_id) == 1,
            "Cross-group relation count differs");
    std::vector<videosc::dedup::SimilarImageRelationSummary> relations;
    Require(reportStore.LoadImageRelations(*active, group.group_id, 0, 8, relations).succeeded &&
                relations.size() == 1 && relations.front().ordinal == 0 &&
                relations.front().neighbor_group_id == 99 &&
                relations.front().neighbor_representative_sha512 == rightSignature &&
                relations.front().hamming_distance == 1,
            "Cross-group image relation differs");
    Require(reportStore.ImageSignatureMemberCount(*active, rightSignature) == 1,
            "Image signature member count differs");
    std::vector<DuplicateMember> relationMembers;
    Require(reportStore.LoadImageSignatureMembers(
                *active,
                rightSignature,
                0,
                8,
                relationMembers).succeeded &&
                relationMembers.size() == 1 &&
                relationMembers.front().path_id == right.path_id &&
                relationMembers.front().image_dhash == right.image_dhash,
            "Cross-group image signature members differ");

    videosc::dedup::SimilarReportMetadata unsupportedMetadata = metadata;
    unsupportedMetadata.image_grouping_rule = "future-grouping-rule";
    Require(reportStore.SaveSimilarMetadata(generation + 1, unsupportedMetadata).succeeded,
            "Cannot save unsupported metadata fixture");
    videosc::dedup::SimilarReportMetadata rejectedMetadata;
    Require(!reportStore.LoadSimilarMetadata(generation + 1, rejectedMetadata).succeeded,
            "Unsupported strict report rules were accepted");
    store.Close();
    std::filesystem::remove_all(directory);
}

/** @brief 跳过视觉内容记录必须可持久化、分页读取、按原因计数，并随报告换代清理。 */
void TestSkippedVisualContentRecords() {
    const std::filesystem::path directory = CreateTestDirectory(L"skipped-visuals");
    RocksDbConfig config;
    config.directory = directory / L"rocks";
    config.block_cache_mib = 8;
    config.write_buffer_mib = 4;
    RocksStore store(config);
    Require(store.Open().succeeded, "Cannot open skipped-content RocksDB");

    DuplicateReportStore reportStore(store);
    constexpr std::uint64_t generation = 41;
    std::vector<videosc::dedup::SkippedVisualContentRecord> records;
    videosc::dedup::SkippedVisualContentRecord imageSkip;
    imageSkip.primary_sha512 = "aa";
    imageSkip.media_kind = videosc::dedup::MediaKind::Image;
    imageSkip.reason = videosc::dedup::SkippedVisualContentReason::InvalidImage;
    imageSkip.active_path_count = 2;
    imageSkip.sample_paths = {L"C:\\media\\a.jpg", L"D:\\media\\b.jpg"};
    records.push_back(imageSkip);
    videosc::dedup::SkippedVisualContentRecord pairSkip;
    pairSkip.primary_sha512 = "bb";
    pairSkip.secondary_sha512 = "cc";
    pairSkip.media_kind = videosc::dedup::MediaKind::Image;
    pairSkip.reason = videosc::dedup::SkippedVisualContentReason::StructuralTimeout;
    pairSkip.active_path_count = 1;
    pairSkip.sample_paths = {L"E:\\media\\c.jpg"};
    records.push_back(pairSkip);
    videosc::dedup::SkippedVisualContentRecord videoSkip;
    videoSkip.primary_sha512 = "dd";
    videoSkip.media_kind = videosc::dedup::MediaKind::Video;
    videoSkip.reason = videosc::dedup::SkippedVisualContentReason::ZeroVideoFrame;
    records.push_back(videoSkip);
    Require(reportStore.SaveSkippedContents(generation, records).succeeded,
            "Cannot save skipped visual content records");

    std::vector<videosc::dedup::SkippedVisualContentRecord> page;
    Require(reportStore.LoadSkippedContents(generation, 1, 1, page).succeeded &&
                page.size() == 1 && page.front().primary_sha512 == "bb" &&
                page.front().secondary_sha512 == "cc" &&
                page.front().reason ==
                    videosc::dedup::SkippedVisualContentReason::StructuralTimeout &&
                page.front().active_path_count == 1 &&
                page.front().sample_paths.size() == 1 &&
                page.front().sample_paths.front() == L"E:\\media\\c.jpg",
            "Skipped visual content paged load differs");
    videosc::dedup::SkippedVisualContentStats stats;
    Require(reportStore.CountSkippedVisualContents(generation, stats).succeeded &&
                stats.total == 3 &&
                stats.by_reason[static_cast<std::size_t>(
                    videosc::dedup::SkippedVisualContentReason::InvalidImage)] == 1 &&
                stats.by_reason[static_cast<std::size_t>(
                    videosc::dedup::SkippedVisualContentReason::StructuralTimeout)] == 1 &&
                stats.by_reason[static_cast<std::size_t>(
                    videosc::dedup::SkippedVisualContentReason::ZeroVideoFrame)] == 1,
            "Skipped visual content stats differ");

    // 发布新一代报告后，旧代跳过记录必须随前缀清理。
    Require(reportStore.Publish(DuplicateReportKind::Similar, generation, 0).succeeded,
            "Cannot publish first generation");
    Require(reportStore.Publish(DuplicateReportKind::Similar, generation + 1, 0).succeeded,
            "Cannot publish second generation");
    videosc::dedup::SkippedVisualContentStats cleaned;
    Require(reportStore.CountSkippedVisualContents(generation, cleaned).succeeded &&
                cleaned.total == 0,
            "Skipped visual content records survived generation rollover");
    store.Close();
    std::filesystem::remove_all(directory);
}

/** @brief 按类型删除报告必须清理对应命名空间，并保留另一类报告和原始内容数据。 */
void TestDeleteDuplicateReportsByKind() {
    const std::filesystem::path directory = CreateTestDirectory(L"delete-reports");
    RocksDbConfig config;
    config.directory = directory / L"rocks";
    config.block_cache_mib = 8;
    config.write_buffer_mib = 4;
    RocksStore store(config);
    Require(store.Open().succeeded, "Cannot open report deletion RocksDB");

    DuplicateReportStore reportStore(store);
    videosc::dedup::DuplicateGroup group;
    group.group_id = 1;
    group.kind = videosc::dedup::DuplicateGroupKind::ExactSha512;
    group.algorithm_version = "media-dhash-v2";
    DuplicateMember left;
    left.path_id = 1;
    left.path = directory / L"left.bin";
    left.size_bytes = 100;
    DuplicateMember right = left;
    right.path_id = 2;
    right.path = directory / L"right.bin";
    group.members = {left, right};
    group.reclaimable_bytes = 100;

    constexpr std::uint64_t exactGeneration = 10;
    constexpr std::uint64_t similarGeneration = 20;
    Require(reportStore.SaveGroup(DuplicateReportKind::Exact, exactGeneration, 0, group).succeeded &&
                reportStore.Publish(DuplicateReportKind::Exact, exactGeneration, 1).succeeded,
            "Cannot publish exact report fixture");
    group.kind = videosc::dedup::DuplicateGroupKind::SimilarImage;
    Require(reportStore.SaveGroup(DuplicateReportKind::Similar, similarGeneration, 0, group).succeeded &&
                reportStore.Publish(DuplicateReportKind::Similar, similarGeneration, 1).succeeded,
            "Cannot publish similar report fixture");

    Require(store.Put(RocksColumnFamily::ExactIndex,
                      "report/exact/0000000000000063/group/orphan",
                      "orphan",
                      false).succeeded &&
                store.Put(RocksColumnFamily::ExactIndex,
                          "report/similar/work/0000000000000058/content/orphan",
                          "orphan",
                          false).succeeded &&
                store.Put(RocksColumnFamily::ShaFileData, "content-preserved", "value", false).succeeded,
            "Cannot save report deletion fixtures");

    Require(reportStore.DeleteAll(DuplicateReportKind::Exact).succeeded,
            "Cannot delete exact report namespace");
    Require(!reportStore.ActiveGeneration(DuplicateReportKind::Exact).has_value(),
            "Exact report active generation survived deletion");
    std::size_t exactKeys = 0;
    Require(store.ForEachPrefix(
                      RocksColumnFamily::ExactIndex,
                      "report/exact/",
                      0,
                      [&](const std::string_view, const std::string_view) {
                          ++exactKeys;
                          return true;
                      }).succeeded &&
                exactKeys == 0,
            "Exact report namespace was not fully deleted");
    Require(reportStore.ActiveGeneration(DuplicateReportKind::Similar) == similarGeneration,
            "Deleting exact report changed similar report");

    std::string preserved;
    Require(store.Get(RocksColumnFamily::ShaFileData, "content-preserved", preserved).succeeded &&
                preserved == "value",
            "Deleting report changed original content data");

    Require(reportStore.DeleteAll(DuplicateReportKind::Similar).succeeded,
            "Cannot delete similar report namespace");
    Require(!reportStore.ActiveGeneration(DuplicateReportKind::Similar).has_value(),
            "Similar report active generation survived deletion");
    std::size_t similarKeys = 0;
    Require(store.ForEachPrefix(
                      RocksColumnFamily::ExactIndex,
                      "report/similar/",
                      0,
                      [&](const std::string_view, const std::string_view) {
                          ++similarKeys;
                          return true;
                      }).succeeded &&
                similarKeys == 0,
            "Similar report namespace was not fully deleted");
    Require(reportStore.DeleteAll(DuplicateReportKind::Similar).succeeded,
            "Deleting a missing report was not idempotent");

    store.Close();
    std::filesystem::remove_all(directory);
}

/** @brief 缺少媒体类型和 dHash 的第 1 版报告必须要求用户重新生成。 */
void TestLegacyReportRequiresRegeneration() {
    const std::filesystem::path directory = CreateTestDirectory(L"legacy-report");
    RocksDbConfig config;
    config.directory = directory / L"rocks";
    config.block_cache_mib = 8;
    config.write_buffer_mib = 4;
    RocksStore store(config);
    Require(store.Open().succeeded, "Cannot open legacy report RocksDB");

    const std::string legacyGroup(1, static_cast<char>(1));
    Require(store.Put(RocksColumnFamily::ExactIndex,
                      "report/exact/0000000000000058/group/0000000000000000",
                      legacyGroup,
                      false).succeeded,
            "Cannot save legacy report fixture");

    DuplicateReportStore reportStore(store);
    videosc::dedup::SimilarReportMetadata missingMetadata;
    Require(!reportStore.LoadSimilarMetadata(88, missingMetadata).succeeded,
            "Legacy similarity report without metadata was accepted");
    videosc::dedup::DuplicateGroup group;
    const videosc::dedup::RocksStatus loaded =
        reportStore.LoadGroup(DuplicateReportKind::Exact, 88, 0, group);
    Require(!loaded.succeeded && loaded.message.find("dHash") != std::string::npos &&
                loaded.message.find("重新生成") != std::string::npos,
            "Legacy report did not request regeneration");

    store.Close();
    std::filesystem::remove_all(directory);
}

/** @brief 实时日志源必须支持并发聚合、增量读取和固定容量淘汰。 */
void TestRuntimeLogFeed() {
    RuntimeLogFeed::ResetForTests();
    constexpr std::size_t threadCount = 8;
    constexpr std::size_t recordsPerThread = 100;
    std::vector<std::thread> writers;
    writers.reserve(threadCount);
    for (std::size_t threadIndex = 0; threadIndex < threadCount; ++threadIndex) {
        writers.emplace_back([] {
            for (std::size_t index = 0; index < recordsPerThread; ++index) {
                RuntimeLogEntry entry;
                entry.severity = RuntimeLogSeverity::Warning;
                entry.task_id = 42;
                entry.task = "duplicate_report";
                entry.stage = "validating_image_structure";
                entry.operation = "structure_timeout";
                entry.status_code = 1;
                entry.subject = "fixture-pair";
                entry.message = "fixture timeout";
                RuntimeLogFeed::Publish(std::move(entry));
            }
        });
    }
    for (std::thread& writer : writers) writer.join();

    const auto aggregated = RuntimeLogFeed::SnapshotSince(0);
    Require(aggregated.entries.size() == 1 &&
                aggregated.entries.front().repeat_count == threadCount * recordsPerThread &&
                aggregated.latest_sequence == threadCount * recordsPerThread,
            "Concurrent runtime logs were not aggregated with a monotonic update sequence");
    Require(RuntimeLogFeed::SnapshotSince(aggregated.latest_sequence).entries.empty(),
            "Incremental runtime log snapshot replayed consumed entries");

    RuntimeLogFeed::ResetForTests();
    const std::size_t overflow = 64;
    for (std::size_t index = 0; index < RuntimeLogFeed::Capacity() + overflow; ++index) {
        RuntimeLogEntry entry;
        entry.severity = RuntimeLogSeverity::Error;
        entry.task_id = static_cast<std::uint64_t>(index + 1);
        entry.task = "capacity_test";
        entry.stage = "unique";
        entry.operation = "record_" + std::to_string(index);
        entry.message = "unique fixture";
        RuntimeLogFeed::Publish(std::move(entry));
    }
    const auto bounded = RuntimeLogFeed::SnapshotSince(0);
    Require(bounded.entries.size() == RuntimeLogFeed::Capacity() &&
                bounded.dropped_entries == overflow,
            "Runtime log feed did not enforce its fixed capacity");
    RuntimeLogFeed::ResetForTests();
}

/** @brief 执行轨迹与失败记录必须分文件立即刷盘并镜像到实时日志。 */
void TestExecutionLogger() {
    RuntimeLogFeed::ResetForTests();
    const std::filesystem::path directory = CreateTestDirectory(L"execution-log");
    videosc::dedup::LoggingConfig config;
    config.execution_directory = directory / L"execution";
    config.rotate_file_mib = 1;
    config.rotate_count = 2;
    ExecutionLogger logger(config);
    std::string error;
    Require(logger.EnsureWritable(error), "Cannot initialize execution logs");
    Require(logger.WriteEvent({42, "scan", "started", "sha512", "started", 0, 10}, error),
            "Cannot append execution event");
    for (std::uint64_t index = 0; index < 54; ++index) {
        ExecutionFailureRecord failure;
        failure.task_id = 42;
        failure.path_id = index + 1;
        failure.task = "scan";
        failure.stage = "media_features";
        failure.operation = "video_six_frame_dhash";
        failure.path = directory / (L"broken-" + std::to_wstring(index) + L".mp4");
        failure.status = "failed";
        failure.status_code = 9;
        failure.detail = "fixture_failure";
        Require(logger.WriteFailure(failure, error), "Cannot append execution failure");
    }
    const std::string events = ReadFile(config.execution_directory / L"execution.log");
    const std::string failures = ReadFile(config.execution_directory / L"execution-failures.log");
    Require(events.find("sha512") != std::string::npos && events.find("started") != std::string::npos,
            "Execution event fields are missing");
    Require(failures.find("broken-0.mp4") != std::string::npos &&
                failures.find("video_six_frame_dhash") != std::string::npos &&
                failures.find("fixture_failure") != std::string::npos,
            "Execution failure fields are missing");
    Require(static_cast<std::size_t>(std::count(failures.begin(), failures.end(), '\n')) == 54,
            "Execution failure log did not retain all 54 flushed records");
    const auto realtime = RuntimeLogFeed::SnapshotSince(0);
    const auto mirroredFailure = std::find_if(
        realtime.entries.begin(), realtime.entries.end(), [](const RuntimeLogEntry& entry) {
            return entry.operation == "video_six_frame_dhash";
        });
    Require(mirroredFailure != realtime.entries.end() && mirroredFailure->repeat_count == 54,
            "Execution failures were not mirrored and aggregated in the runtime log feed");
    RuntimeLogFeed::ResetForTests();
    std::filesystem::remove_all(directory);
}

/** @brief 坏块和超时信息必须带定位字段写入文件并镜像到实时日志。 */
void TestScanErrorLog() {
    RuntimeLogFeed::ResetForTests();
    const std::filesystem::path directory = CreateTestDirectory(L"scan-error-log");
    videosc::dedup::LoggingConfig config;
    config.directory = directory / L"logs";
    config.rotate_file_mib = 1;
    config.rotate_count = 2;
    ScanErrorLogger logger(config);
    std::string error;
    Require(logger.EnsureWritable(error), "Cannot initialize scan error log");
    FilePathRecord record;
    record.scan_id = 42;
    record.path_id = 7;
    record.path = directory / L"bad-sector.mp4";
    record.storage_target_key = L"PhysicalDrive8";
    record.size_bytes = 8192;
    FileHashResult result;
    result.status = FileHashStatus::ReadTimeout;
    result.system_error = ERROR_SEM_TIMEOUT;
    result.failed_offset = 4096;
    result.bytes_read = 4096;
    Require(logger.Write(record, result, error), "Cannot append scan error log");
    const std::string log = ReadFile(config.directory / L"scan-errors.log");
    Require(log.find("read_timeout") != std::string::npos &&
                log.find("bad-sector.mp4") != std::string::npos &&
                log.find("4096") != std::string::npos,
            "Scan error log omitted timeout, path or failed offset");
    const auto realtime = RuntimeLogFeed::SnapshotSince(0);
    Require(realtime.entries.size() == 1 && realtime.entries.front().task == "scan" &&
                realtime.entries.front().operation == "read_timeout" &&
                realtime.entries.front().subject.find("bad-sector.mp4") != std::string::npos,
            "Scan error was not mirrored to the runtime log feed");
    RuntimeLogFeed::ResetForTests();
    std::filesystem::remove_all(directory);
}

/** @brief RocksDB 热表二进制记录必须无损保留 Unicode 路径和媒体字段。 */
void TestCoreModelBinaryCodec() {
    FilePathRecord path;
    path.path_id = 88;
    path.scan_id = 99;
    path.path = L"D:\\媒体\\电影.mp4";
    path.normalized_path_key = L"d:\\媒体\\电影.mp4";
    path.sha512 = Sha512Digest{};
    (*path.sha512)[7] = 42;
    path.identity = {123, 456, 789};
    path.volume_guid = L"\\\\?\\Volume{test}\\";
    path.storage_target_key = L"PhysicalDrive4";
    path.size_bytes = 987654321;
    path.extension = L".mp4";
    path.creation_time_utc_ms = 1001;
    path.last_write_time_utc_ms = 1002;
    path.scan_root_priority = 3;
    path.state = videosc::dedup::FilePathState::Available;
    path.sync_state = videosc::dedup::SyncState::Pending;
    std::string error;
    const auto decodedPath = CoreModelCodec::DeserializeFilePath(CoreModelCodec::SerializeFilePath(path), error);
    Require(decodedPath.has_value(), "Cannot decode binary file path record");
    Require(decodedPath->path == path.path && decodedPath->normalized_path_key == path.normalized_path_key &&
                decodedPath->identity.file_id_low == 789 && decodedPath->sha512 == path.sha512,
            "Binary file path record differs");

    ShaFileData data;
    data.sha512 = *path.sha512;
    data.content_size_bytes = path.size_bytes;
    data.media_kind = videosc::dedup::MediaKind::Video;
    data.mime_type = "video/mp4";
    data.container_name = "mov,mp4";
    data.width = 3840;
    data.height = 2160;
    data.video_duration_ms = 12345;
    data.video_frame_rate = 29.97;
    data.video_bitrate = 8000000;
    data.video_dhashes = {1, 2, 3, 4, 5, 6};
    data.has_video_dhashes = true;
    data.contact_sheet_path = L"D:\\缩略图\\电影.jpg";
    data.media_algorithm_version = "media-dhash-v2";
    const auto decodedData = CoreModelCodec::DeserializeShaFileData(CoreModelCodec::SerializeShaFileData(data), error);
    Require(decodedData.has_value(), "Cannot decode binary content record");
    Require(decodedData->video_dhashes == data.video_dhashes &&
                decodedData->contact_sheet_path == data.contact_sheet_path && decodedData->width == data.width,
            "Binary content record differs");

    ShaFileData image;
    image.sha512 = data.sha512;
    image.media_kind = videosc::dedup::MediaKind::Image;
    image.width = 1920;
    image.height = 1080;
    image.image_pdq_hash = videosc::dedup::PdqHash256{};
    (*image.image_pdq_hash)[7] = 0xa5;
    image.image_pdq_quality = static_cast<std::uint8_t>(87);
    image.has_image_zoned_phashes = true;
    for (std::size_t index = 0; index < image.image_zoned_phashes.size(); ++index) {
        image.image_zoned_phashes[index] = 0x1000ULL + index;
    }
    image.image_perceptual_algorithm_version = 1;
    image.image_structural_algorithm_version = 1;
    const auto decodedImage = CoreModelCodec::DeserializeShaFileData(
        CoreModelCodec::SerializeShaFileData(image), error);
    Require(decodedImage.has_value() && decodedImage->image_pdq_hash == image.image_pdq_hash &&
                decodedImage->image_pdq_quality == image.image_pdq_quality &&
                decodedImage->image_zoned_phashes == image.image_zoned_phashes &&
                decodedImage->image_perceptual_algorithm_version == 1 &&
                decodedImage->image_structural_algorithm_version == 1,
            "Binary image perceptual features differ");
}

/** @brief 原生发现器必须流式遍历嵌套目录并正确分类媒体扩展名。 */
void TestNativeFileDiscovery() {
    const std::filesystem::path directory = CreateTestDirectory(L"discovery");
    std::filesystem::create_directories(directory / L"nested");
    WriteFile(directory / L"photo.JPG", "image");
    WriteFile(directory / L"nested" / L"movie.MP4", "video");
    WriteFile(directory / L"nested" / L"sound.FLAC", "audio");
    WriteFile(directory / L"data.bin", "other");
    DiscoveryRoot root;
    root.path = directory;
    root.priority = 2;
    root.volume_guid = L"test-volume";
    root.storage_target_key = L"PhysicalDriveTest";
    std::atomic_bool cancelled{false};
    std::unordered_set<int> kinds;
    std::uint64_t visited = 0;
    const auto stats = NativeFileDiscovery::Enumerate(root, 77, cancelled, [&](videosc::dedup::DiscoveredFile&& file) {
        ++visited;
        kinds.insert(static_cast<int>(file.media_kind));
        Require(file.record.scan_id == 77 && file.record.scan_root_priority == 2,
                "Discovered file lost scan metadata");
        Require(!file.record.normalized_path_key.empty(), "Discovered file has no normalized key");
        return true;
    });
    Require(stats.error.empty() && stats.discovered_files == 4 && visited == 4,
            "Native discovery file count is incorrect");
    Require(kinds.size() == 4, "Native discovery media classification is incomplete");
    std::filesystem::remove_all(directory);
}

/** @brief 首次扫描必须完整落盘，第二次扫描必须复用未变化文件且不重复同步。 */
void TestEndToEndIncrementalScan() {
    const std::filesystem::path directory = CreateTestDirectory(L"coordinator");
    const std::filesystem::path scanRoot = directory / L"input";
    std::filesystem::create_directories(scanRoot);
    WriteFile(scanRoot / L"copy-a.txt", "same-content");
    WriteFile(scanRoot / L"copy-b.txt", "same-content");
    const std::filesystem::path ffmpeg =
        std::filesystem::current_path() / L"third_party" / L"ffmpeg" / L"bin" / L"ffmpeg.exe";
    const std::filesystem::path image = scanRoot / L"picture.png";
    RunProcess(ffmpeg,
               L"-hide_banner -loglevel error -y -f lavfi -i \"testsrc2=size=64x64:rate=1\" "
               L"-frames:v 1 \"" + image.wstring() + L"\"",
               30000);

    AppConfig appConfig = AppConfig::CreateDefault(directory);
    appConfig.paths.scan_roots = {scanRoot};
    appConfig.discovery.method = DiscoveryMethod::Native;
    appConfig.compute.worker_threads = 2;
    appConfig.compute.ffmpeg_threads_per_task = 1;
    appConfig.database.port = 1;
    appConfig.database.connect_timeout_seconds = 1;
    appConfig.io.no_progress_timeout_seconds = 10;
    appConfig.rocksdb.directory = directory / L"rocks";
    appConfig.rocksdb.block_cache_mib = 16;
    appConfig.rocksdb.write_buffer_mib = 4;
    appConfig.thumbnails.root_directory = directory / L"thumbs";
    const ScanOptions options = ScanOptions::Freeze(appConfig);
    RocksStore store(appConfig.rocksdb);
    Require(store.Open().succeeded, "Cannot open coordinator RocksDB");
    MySqlSyncQueue syncQueue(store);
    MySqlClient mysqlClient(appConfig.database);

    ScanCoordinator first(options, store, syncQueue, mysqlClient, [] {});
    Require(first.Start(), "Cannot start first scan");
    const auto firstDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (first.is_running() && std::chrono::steady_clock::now() < firstDeadline) Sleep(20);
    if (first.is_running()) first.Cancel();
    first.Wait();
    const auto firstSnapshot = first.snapshot();
    Require(firstSnapshot.phase == ScanPhase::CompletedLocal, "First scan did not complete locally");
    Require(firstSnapshot.discovery_roots.size() == 1 &&
                firstSnapshot.discovery_roots.front().backend == DiscoveryBackend::Native &&
                firstSnapshot.discovery_roots.front().phase == DiscoveryRootPhase::Completed &&
                firstSnapshot.discovery_roots.front().discovered_files == 3,
            "Per-root discovery progress is incorrect");
    Require(firstSnapshot.discovered_files == 3 && firstSnapshot.hashed_files == 3,
            "First scan counts are incorrect");
    Require(firstSnapshot.hash_total_files == 3 && firstSnapshot.hash_processed_files == 3,
            "First scan hash progress is incorrect");
    Require(firstSnapshot.media_total_known && firstSnapshot.media_total_files == 1 &&
                firstSnapshot.media_processed_files == 1,
            "First scan media progress is incorrect");

    std::size_t pathRecords = 0;
    Require(store.ForEachPrefix(RocksColumnFamily::FilePaths, "path/", 0, [&](const auto, const auto) {
                ++pathRecords;
                return true;
            }).succeeded,
            "Cannot count scanned path records");
    Require(pathRecords == 3, "First scan did not persist all paths");
    std::vector<SyncOperation> beforeSecond;
    Require(syncQueue.ReadBatch(100, beforeSecond).succeeded && beforeSecond.size() == 5,
            "First scan sync operation count is incorrect");

    ScanCoordinator second(options, store, syncQueue, mysqlClient, [] {});
    Require(second.Start(), "Cannot start incremental scan");
    const auto secondDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (second.is_running() && std::chrono::steady_clock::now() < secondDeadline) Sleep(20);
    if (second.is_running()) second.Cancel();
    second.Wait();
    const auto secondSnapshot = second.snapshot();
    Require(secondSnapshot.phase == ScanPhase::CompletedLocal, "Incremental scan did not complete locally");
    Require(secondSnapshot.discovered_files == 3 && secondSnapshot.hashed_files == 0,
            "Incremental scan did not reuse unchanged hashes");
    Require(secondSnapshot.hash_total_files == 0 && secondSnapshot.hash_processed_files == 0,
            "Incremental scan hash progress did not exclude reused files");
    Require(secondSnapshot.media_total_known && secondSnapshot.media_total_files == 0 &&
                secondSnapshot.media_processed_files == 0,
            "Incremental scan media progress is incorrect");
    std::vector<SyncOperation> afterSecond;
    Require(syncQueue.ReadBatch(100, afterSecond).succeeded && afterSecond.size() == beforeSecond.size(),
            "Incremental scan duplicated MySQL sync operations");

    std::filesystem::remove(scanRoot / L"copy-b.txt");
    ScanCoordinator third(options, store, syncQueue, mysqlClient, [] {});
    Require(third.Start(), "Cannot start missing-file scan");
    const auto thirdDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (third.is_running() && std::chrono::steady_clock::now() < thirdDeadline) Sleep(20);
    if (third.is_running()) third.Cancel();
    third.Wait();
    const auto thirdSnapshot = third.snapshot();
    Require(thirdSnapshot.phase == ScanPhase::CompletedLocal && thirdSnapshot.discovered_files == 2 &&
                thirdSnapshot.hashed_files == 0,
            "Missing-file incremental scan is incorrect");
    Require(thirdSnapshot.hash_total_files == 0 && thirdSnapshot.hash_processed_files == 0 &&
                thirdSnapshot.media_total_known && thirdSnapshot.media_total_files == 0,
            "Missing-file scan stage progress is incorrect");
    std::size_t missingRecords = 0;
    Require(store.ForEachPrefix(RocksColumnFamily::FilePaths, "path/", 0, [&](const auto, const auto value) {
                std::string decodeError;
                const auto record = CoreModelCodec::DeserializeFilePath(std::string(value), decodeError);
                Require(record.has_value(), "Cannot decode missing-file record");
                if (record->state == videosc::dedup::FilePathState::Missing) ++missingRecords;
                return true;
            }).succeeded,
            "Cannot inspect missing-file records");
    Require(missingRecords == 1, "Removed external file was not marked Missing");
    std::vector<SyncOperation> afterThird;
    Require(syncQueue.ReadBatch(100, afterThird).succeeded && afterThird.size() == afterSecond.size() + 1,
            "Missing-file state did not enqueue exactly one MySQL update");
    store.Close();
    std::filesystem::remove_all(directory);
}

/** @brief 取消请求必须立即发布快照，并在协调线程退出后进入稳定取消终态。 */
void TestScanCancellationSnapshot() {
    const std::filesystem::path directory = CreateTestDirectory(L"cancel_snapshot");
    const std::filesystem::path scanRoot = directory / L"input";
    std::filesystem::create_directories(scanRoot);

    AppConfig appConfig = AppConfig::CreateDefault(directory);
    appConfig.paths.scan_roots = {scanRoot};
    appConfig.discovery.method = DiscoveryMethod::Native;
    appConfig.database.port = 1;
    appConfig.database.connect_timeout_seconds = 1;
    appConfig.rocksdb.directory = directory / L"rocks";
    appConfig.rocksdb.block_cache_mib = 16;
    appConfig.rocksdb.write_buffer_mib = 4;
    appConfig.thumbnails.root_directory = directory / L"thumbs";

    RocksStore store(appConfig.rocksdb);
    Require(store.Open().succeeded, "Cannot open cancellation RocksDB");
    MySqlSyncQueue syncQueue(store);
    MySqlClient mysqlClient(appConfig.database);
    ScanCoordinator coordinator(ScanOptions::Freeze(appConfig), store, syncQueue, mysqlClient, [] {});
    Require(coordinator.Start(), "Cannot start cancellation scan");
    coordinator.Cancel();
    Require(coordinator.snapshot().cancellation_requested,
            "Cancellation request was not published immediately");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (coordinator.is_running() && std::chrono::steady_clock::now() < deadline) Sleep(10);
    coordinator.Wait();
    const auto snapshot = coordinator.snapshot();
    Require(snapshot.phase == ScanPhase::Cancelled && snapshot.cancellation_requested,
            "Cancelled scan did not reach a stable terminal snapshot");

    store.Close();
    std::filesystem::remove_all(directory);
}

/** @brief 删除策略必须确定性保留一项，指定磁盘时优先保留其他磁盘副本。 */
void TestDeletionPlannerSafety() {
    videosc::dedup::DuplicateGroup group;
    group.group_id = 100;
    for (std::uint64_t index = 1; index <= 3; ++index) {
        DuplicateMember member;
        member.path_id = index;
        member.path = L"D:\\file-" + std::to_wstring(index);
        member.storage_target_key = index == 3 ? L"PhysicalDrive2" : L"PhysicalDrive1";
        member.size_bytes = index * 100;
        member.width = static_cast<std::uint32_t>(index * 100);
        member.height = 100;
        member.bitrate = index * 1000;
        member.last_write_time_utc_ms = static_cast<std::int64_t>(index);
        member.scan_root_priority = static_cast<std::uint32_t>(3 - index);
        group.members.push_back(std::move(member));
    }
    const auto smallest = DeletionPlanner::Select(group, KeepPolicy::Smallest);
    Require(smallest.has_value() && smallest->retained_path_id == 1 &&
                smallest->selected_for_deletion.size() == 2,
            "Keep-smallest deletion plan is incorrect");
    const auto targetDisk = DeletionPlanner::Select(group, KeepPolicy::Largest, L"PhysicalDrive1");
    Require(targetDisk.has_value() && targetDisk->retained_path_id == 3 &&
                targetDisk->selected_for_deletion == std::vector<std::uint64_t>({1, 2}),
            "Target-disk deletion did not preserve an off-disk copy");
    const auto highestQuality = DeletionPlanner::Select(group, KeepPolicy::HighestQuality);
    Require(highestQuality.has_value() && highestQuality->retained_path_id == 3,
            "Highest-quality retention is incorrect");

    videosc::dedup::RetentionPolicySet pathThenQuality;
    pathThenQuality.path_priority = true;
    const auto prioritized = DeletionPlanner::SelectDetailed(group, pathThenQuality);
    Require(prioritized.succeeded() && prioritized.plan->retained_path_id == 3,
            "Fixed path-priority retention is incorrect");

    videosc::dedup::RetentionPolicySet newestThenOldest;
    newestThenOldest.highest_quality = false;
    newestThenOldest.newest = true;
    newestThenOldest.oldest = true;
    const auto timeConflict = DeletionPlanner::SelectDetailed(group, newestThenOldest);
    Require(timeConflict.succeeded() && timeConflict.plan->retained_path_id == 3,
            "Newest did not win the fixed priority over oldest");

    videosc::dedup::RetentionPolicySet largestThenSmallest;
    largestThenSmallest.highest_quality = false;
    largestThenSmallest.largest = true;
    largestThenSmallest.smallest = true;
    const auto sizeConflict = DeletionPlanner::SelectDetailed(group, largestThenSmallest);
    Require(sizeConflict.succeeded() && sizeConflict.plan->retained_path_id == 3,
            "Largest did not win the fixed priority over smallest");

    videosc::dedup::RetentionPolicySet none;
    none.highest_quality = false;
    Require(DeletionPlanner::SelectDetailed(group, none).status ==
                videosc::dedup::DeletionPlanStatus::InvalidPolicy,
            "Empty retention policy set was accepted");
    const auto normalizedTarget = DeletionPlanner::SelectDetailed(
        group, pathThenQuality, L"  physicaldrive1  ");
    Require(normalizedTarget.succeeded() && normalizedTarget.matched_target_members == 2,
            "Target storage normalization did not ignore spaces and case");
    Require(DeletionPlanner::SelectDetailed(group, pathThenQuality, L"PhysicalDrive99").status ==
                videosc::dedup::DeletionPlanStatus::TargetStorageNotFound,
            "Missing target storage did not return a diagnostic status");

    videosc::dedup::DuplicateGroup stable = group;
    for (auto& member : stable.members) {
        member.storage_target_key = L"PhysicalDrive1";
        member.size_bytes = 100;
        member.width = 100;
        member.height = 100;
        member.bitrate = 1000;
        member.last_write_time_utc_ms = 1;
        member.scan_root_priority = 0;
    }
    const auto stableTie = DeletionPlanner::SelectDetailed(stable, pathThenQuality);
    Require(stableTie.succeeded() && stableTie.plan->retained_path_id == 1,
            "path_id did not provide a stable final tie-breaker");
}

/** @brief 永久删除必须逐文件复核和提交，并在停止请求后保留尚未开始的文件。 */
void TestSafePermanentDeletion() {
    const std::filesystem::path directory = CreateTestDirectory(L"deletion");
    const std::filesystem::path retainedPath = directory / L"keep.bin";
    const std::filesystem::path deletedPath = directory / L"delete.bin";
    const std::filesystem::path pendingPath = directory / L"pending.bin";
    WriteFile(retainedPath, "identical-delete-test");
    WriteFile(deletedPath, "identical-delete-test");
    WriteFile(pendingPath, "identical-delete-test");
    videosc::dedup::IoConfig io;
    io.no_progress_timeout_seconds = 10;
    auto hasher = std::make_shared<videosc::dedup::VideoScFileHasher>(io);
    std::atomic_bool cancel{false};
    const auto retainedHash = hasher->Hash(retainedPath, cancel);
    const auto deletedHash = hasher->Hash(deletedPath, cancel);
    const auto pendingHash = hasher->Hash(pendingPath, cancel);
    Require(retainedHash.sha512.has_value() && deletedHash.sha512 == retainedHash.sha512 &&
                pendingHash.sha512 == retainedHash.sha512,
            "Deletion fixture SHA-512 differs");

    RocksDbConfig rocks;
    rocks.directory = directory / L"rocks";
    rocks.block_cache_mib = 8;
    rocks.write_buffer_mib = 4;
    RocksStore store(rocks);
    Require(store.Open().succeeded, "Cannot open deletion RocksDB");
    MySqlSyncQueue queue(store);
    auto makeRecord = [&](const std::uint64_t pathId,
                          const std::filesystem::path& path,
                          const videosc::dedup::FileHashResult& hash) {
        FilePathRecord record;
        record.path_id = pathId;
        record.path = std::filesystem::absolute(path).lexically_normal();
        record.normalized_path_key = record.path.wstring();
        std::replace(record.normalized_path_key.begin(), record.normalized_path_key.end(), L'/', L'\\');
        std::transform(record.normalized_path_key.begin(), record.normalized_path_key.end(),
                       record.normalized_path_key.begin(), [](const wchar_t value) {
                           return static_cast<wchar_t>(std::towlower(value));
                       });
        record.sha512 = hash.sha512;
        record.identity = hash.identity;
        record.size_bytes = hash.file_size;
        record.last_write_time_utc_ms = hash.last_write_time_utc_ms;
        record.state = videosc::dedup::FilePathState::Available;
        const std::string key = "path/" + WideToUtf8(record.normalized_path_key);
        Require(store.Put(RocksColumnFamily::FilePaths,
                          key,
                          CoreModelCodec::SerializeFilePath(record),
                          true).succeeded,
                "Cannot persist deletion path record");
        return record;
    };
    const FilePathRecord retainedRecord = makeRecord(1, retainedPath, retainedHash);
    const FilePathRecord deletedRecord = makeRecord(2, deletedPath, deletedHash);
    const FilePathRecord pendingRecord = makeRecord(3, pendingPath, pendingHash);

    videosc::dedup::DuplicateGroup group;
    group.group_id = 200;
    for (const FilePathRecord* record : {&retainedRecord, &deletedRecord, &pendingRecord}) {
        DuplicateMember member;
        member.path_id = record->path_id;
        member.path = record->path;
        member.content_sha512 = *record->sha512;
        member.size_bytes = record->size_bytes;
        group.members.push_back(std::move(member));
    }
    group.retained_path_id = 1;
    group.selected_for_deletion = {2, 3};
    videosc::dedup::LoggingConfig logging;
    logging.directory = directory / L"logs";
    logging.execution_directory = directory / L"execution-logs";
    logging.rotate_file_mib = 1;
    logging.rotate_count = 3;
    OperationLogger logger(logging);
    DeletionExecutor executor(store, queue, hasher, logger);
    std::atomic_uint64_t completedFiles{0};
    const auto result = executor.Execute(
        group,
        [&](const videosc::dedup::DeletionItemResult&,
            const videosc::dedup::DeletionBatchResult&) {
            completedFiles.fetch_add(1);
        },
        [&]() { return completedFiles.load() != 0; });
    Require(result.preflight_succeeded && result.deleted_files == 1 &&
                result.items.size() == 1 && result.items.front().mapping_delete_queued,
            "Safe deletion batch did not stop between files");
    Require(std::filesystem::exists(retainedPath) && !std::filesystem::exists(deletedPath) &&
                std::filesystem::exists(pendingPath),
            "Deletion did not finish the current file and preserve the unstarted file");
    std::string ignored;
    Require(store.Get(RocksColumnFamily::FilePaths,
                      "path/" + WideToUtf8(deletedRecord.normalized_path_key),
                      ignored).message == "not_found",
            "Deleted path mapping remains in RocksDB");
    Require(store.Get(RocksColumnFamily::FilePaths,
                      "path/" + WideToUtf8(retainedRecord.normalized_path_key),
                      ignored).succeeded,
            "Retained path mapping was removed");
    Require(store.Get(RocksColumnFamily::FilePaths,
                      "path/" + WideToUtf8(pendingRecord.normalized_path_key),
                      ignored).succeeded,
            "Unstarted path mapping was removed after stop request");
    std::vector<SyncOperation> operations;
    Require(queue.ReadBatch(10, operations).succeeded && operations.size() == 1 &&
                operations.front().kind == SyncOperationKind::DeleteFilePath &&
                operations.front().expected_sha512 == retainedHash.sha512,
            "Conditional MySQL mapping delete was not queued");
    Require(std::filesystem::exists(logging.directory / L"operations.log") &&
                std::filesystem::file_size(logging.directory / L"operations.log") > 0,
            "Deletion operation log was not written");
    store.Close();
    std::filesystem::remove_all(directory);
}

/** @brief 应用异常日志必须支持并发写入、唯一 ID 和实时聚合镜像。 */
void TestApplicationErrorLog() {
    RuntimeLogFeed::ResetForTests();
    const std::filesystem::path directory = CreateTestDirectory(L"application-errors");
    videosc::dedup::LoggingConfig logging;
    logging.directory = directory;
    logging.rotate_file_mib = 4;
    logging.rotate_count = 3;
    ApplicationErrorLogger::Configure(logging);

    constexpr int threadCount = 4;
    constexpr int recordsPerThread = 16;
    std::vector<std::thread> writers;
    std::atomic_bool writeFailed{false};
    for (int threadIndex = 0; threadIndex < threadCount; ++threadIndex) {
        writers.emplace_back([threadIndex, &writeFailed] {
            for (int recordIndex = 0; recordIndex < recordsPerThread; ++recordIndex) {
                ApplicationErrorRecord record;
                record.category = "test_exception";
                record.module = "DedupTests";
                record.operation = "concurrent_write";
                record.message = "fixture-" + std::to_string(threadIndex) + "-" + std::to_string(recordIndex);
                record.exception_type = "test";
                if (ApplicationErrorLogger::Write(record).empty()) writeFailed.store(true);
            }
        });
    }
    for (std::thread& writer : writers) writer.join();
    Require(!writeFailed.load(), "Application error ID is empty");

    std::ifstream stream(directory / L"application-errors.log", std::ios::binary);
    Require(static_cast<bool>(stream), "Application error log was not created");
    std::unordered_set<std::string> ids;
    std::string line;
    std::size_t lineCount = 0;
    while (std::getline(stream, line)) {
        Require(line.find("模块=DedupTests") != std::string::npos,
                "Application error module changed");
        Require(line.find("操作=concurrent_write") != std::string::npos,
                "Application error operation changed");
        const std::string marker = "异常ID=";
        const std::size_t begin = line.find(marker);
        const std::size_t end = begin == std::string::npos
                                    ? std::string::npos
                                    : line.find(' ', begin + marker.size());
        Require(begin != std::string::npos && end != std::string::npos,
                "Application error ID is missing");
        Require(ids.insert(line.substr(begin + marker.size(), end - begin - marker.size())).second,
                "Application error ID is not unique");
        ++lineCount;
    }
    Require(lineCount == threadCount * recordsPerThread, "Concurrent application error records were lost");
    const auto realtime = RuntimeLogFeed::SnapshotSince(0);
    Require(realtime.entries.size() == 1 && realtime.entries.front().task == "application" &&
                realtime.entries.front().repeat_count == threadCount * recordsPerThread,
            "Application errors were not mirrored and aggregated in the runtime log feed");
    RuntimeLogFeed::ResetForTests();
    stream.close();
    std::filesystem::remove_all(directory);
}

/** @brief 报告选择必须跨缓存持久化、原子发布并严格执行距离小于上限。 */
void TestPersistentReportSelection() {
    const std::filesystem::path directory = CreateTestDirectory(L"report-selection");
    videosc::dedup::RocksDbConfig config;
    config.directory = directory / L"rocks";
    RocksStore store(config);
    Require(store.Open().succeeded, "Cannot open report selection store");

    videosc::dedup::DuplicateGroup group;
    group.group_id = 77;
    group.kind = videosc::dedup::DuplicateGroupKind::SimilarImage;
    DuplicateMember retained;
    retained.path_id = 1;
    retained.size_bytes = 100;
    retained.image_dhash = 0xf0f0f0f0f0f0f0f0ULL;
    DuplicateMember candidate = retained;
    candidate.path_id = 2;
    candidate.size_bytes = 250;
    candidate.image_dhash = *retained.image_dhash ^ 0x3ULL;
    group.members = {retained, candidate};

    videosc::dedup::ReportSelectionConfig limits;
    limits.image_dhash_distance_exclusive_limit = 2;
    Require(!ReportSelectionRules::Evaluate(group, retained, candidate, limits, 4, 5).allowed,
            "Distance equal to exclusive image limit was selected");
    limits.image_dhash_distance_exclusive_limit = 3;
    const auto accepted = ReportSelectionRules::Evaluate(group, retained, candidate, limits, 4, 5);
    Require(accepted.allowed && accepted.measured_distance == 2.0,
            "Distance below exclusive image limit was rejected");
    candidate.image_dhash.reset();
    const auto threeStageAccepted =
        ReportSelectionRules::Evaluate(group, retained, candidate, limits, 31, 5, true);
    Require(threeStageAccepted.allowed && !threeStageAccepted.has_measured_distance,
            "Three-stage verified image group was incorrectly rechecked with legacy dHash");
    candidate.image_dhash = *retained.image_dhash ^ 0x3ULL;

    ReportSelectionStore selections(store);
    ReportSelectionSnapshot snapshot;
    const ReportSelectionMember selected{2, 250, 1, true, 2.0, 3.0};
    Require(selections.SetGroup(DuplicateReportKind::Similar, 1234, 77, {selected}, snapshot).succeeded,
            "Cannot persist report selection");
    Require(snapshot.selected_file_count == 1 && snapshot.selected_total_bytes == 250 &&
                snapshot.selected_group_count == 1,
            "Report selection summary is incorrect");
    std::vector<ReportSelectionMember> loaded;
    Require(selections.LoadGroup(DuplicateReportKind::Similar, 1234, 77, loaded).succeeded &&
                loaded.size() == 1 && loaded.front().path_id == 2,
            "Persistent report selection cannot be loaded");
    std::vector<std::uint64_t> selectedGroups;
    Require(selections.ForEachSelectedGroup(
                DuplicateReportKind::Similar,
                1234,
                [&](const std::uint64_t groupId) {
                    selectedGroups.push_back(groupId);
                    return true;
                }).succeeded &&
                selectedGroups == std::vector<std::uint64_t>{77},
            "Persistent selected groups cannot be enumerated independently from GUI summaries");

    constexpr std::uint64_t replacement = 987654;
    Require(selections.BeginReplacement(DuplicateReportKind::Similar, 1234, replacement).succeeded &&
                selections.SaveReplacementGroup(
                    DuplicateReportKind::Similar, 1234, replacement, 88,
                    {ReportSelectionMember{3, 400, 4, true, 1.0, 3.0}}).succeeded,
            "Cannot write selection replacement staging");
    ReportSelectionSnapshot beforePublish;
    Require(selections.LoadSnapshot(DuplicateReportKind::Similar, 1234, beforePublish).succeeded &&
                beforePublish.selection_generation == snapshot.selection_generation,
            "Unpublished selection staging became visible");
    ReportSelectionSnapshot published;
    Require(selections.PublishReplacement(
                DuplicateReportKind::Similar, 1234, replacement, published).succeeded &&
                published.selected_total_bytes == 400,
            "Cannot atomically publish selection replacement");
    store.Close();
    Require(store.Open().succeeded, "Cannot reopen persistent report selection store");
    ReportSelectionSnapshot reopened;
    Require(selections.LoadSnapshot(DuplicateReportKind::Similar, 1234, reopened).succeeded &&
                reopened.selection_generation == replacement && reopened.selected_file_count == 1,
            "Published report selection did not survive reopen");
    selectedGroups.clear();
    Require(selections.ForEachSelectedGroup(
                DuplicateReportKind::Similar,
                1234,
                [&](const std::uint64_t groupId) {
                    selectedGroups.push_back(groupId);
                    return true;
                }).succeeded &&
                selectedGroups == std::vector<std::uint64_t>{88},
            "Selected group enumeration did not follow the atomically published generation");
    store.Close();
    std::filesystem::remove_all(directory);
}

/** @brief 公共线程池必须排空任务、收集异常并提供稳定快照。 */
void TestTaskThreadPoolLifecycle() {
    TaskThreadPool pool("test-pool", 2, 2);
    std::atomic_uint64_t completed{0};
    Require(pool.Submit([&] { completed.fetch_add(1); }), "Cannot submit first pool task");
    Require(pool.Submit([&] { completed.fetch_add(1); }), "Cannot submit second pool task");
    Require(pool.Submit([] { throw std::runtime_error("fixture task failure"); }),
            "Cannot submit failing pool task");
    pool.CloseSubmissions();
    pool.Join();
    const auto snapshot = pool.snapshot();
    Require(completed.load() == 2 && snapshot.completed_tasks == 3 && snapshot.failed_tasks == 1 &&
                pool.failure_message() == "fixture task failure",
            "Task thread pool did not drain or collect failure");
}

/** @brief 同一 RocksDB 目录被占用时第二个实例必须失败且不得删除 LOCK。 */
void TestRocksStoreExclusiveOpen() {
    const std::filesystem::path directory = CreateTestDirectory(L"rocks-exclusive-open");
    videosc::dedup::RocksDbConfig config;
    config.directory = directory / L"rocks";
    RocksStore first(config);
    RocksStore second(config);
    Require(first.Open().succeeded, "Cannot open first RocksDB instance");
    Require(!second.Open().succeeded, "Second RocksDB instance opened the same directory");
    first.Close();
    Require(second.Open().succeeded, "RocksDB lock was not released by normal close");
    second.Close();
    std::filesystem::remove_all(directory);
}

/** @brief 自研 DLL 的公开失败路径必须初始化输出且不传播异常。 */
void TestDllExceptionBoundaryContracts() {
    VideoScFileHashResult hash{};
    Require(ComputeFileSHA512Ex(nullptr, nullptr, nullptr, nullptr, &hash) == 0 &&
                hash.statusCode == VIDEOSC_HASH_INVALID_ARGUMENT,
            "VideoSc hash boundary did not initialize invalid result");

    VideoScMediaResult media{};
    Require(AnalyzeMediaFile(nullptr, nullptr, &media) == 0 &&
                media.statusCode == VIDEOSC_ERR_INVALID_ARG,
            "VideoSc media boundary did not initialize invalid result");
    FreeVideoScMediaResult(&media);

    DiskTopologyInfo topology{};
    Require(QueryDiskTopology(nullptr, &topology) == 0 &&
                topology.statusCode == DISKINFO_TOPOLOGY_INVALID_ARGUMENT &&
                topology.physicalDiskNumber == -1,
            "DiskInfo topology boundary did not initialize invalid result");
    FilePhysicalLocation location{};
    Require(QueryFilePhysicalLocation(nullptr, &location) == 0 &&
                location.statusCode == DISKINFO_FILE_LOCATION_INVALID_ARGUMENT &&
                location.physicalDiskNumber == -1,
            "DiskInfo location boundary did not initialize invalid result");
    Require(GetPhysicalDiskNumber(nullptr) == -1, "DiskInfo compatibility boundary changed invalid status");
}

}  // namespace

/** @brief 顺序运行核心配置测试并返回标准进程退出码。 */
int main() {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    const std::vector<TestCase> tests = {
        {"DPAPI round trip", TestDpapiRoundTrip},
        {"configuration validation", TestValidation},
        {"JSON save/load round trip", TestSaveLoadRoundTrip},
        {"backup recovery", TestBackupRecovery},
        {"future schema read-only", TestFutureVersionIsReadOnly},
        {"broken DPAPI isolation", TestBrokenDpapiKeepsOtherSettings},
        {"SHA-512 hex codec", TestSha512HexCodec},
        {"scan options freeze", TestScanOptionsFreeze},
        {"streaming SHA-512", TestStreamingSha512},
        {"streaming SHA-512 cancellation", TestStreamingSha512Cancellation},
        {"disk hash scheduler concurrency", TestDiskHashSchedulerConcurrency},
        {"RocksDB persistence", TestRocksStorePersistence},
        {"data version decisions", TestDataVersionDecisions},
        {"RocksDB full Column Family reset", TestRocksStoreClearAll},
        {"scan options codec", TestScanOptionsCodec},
        {"scan checkpoint recovery", TestScanCheckpointRecovery},
        {"image completeness and retry policies", TestImageFeatureCompletenessAndRetryPolicies},
        {"persistent MySQL sync queue", TestPersistentSyncQueue},
        {"safe MySQL schema statements", TestSafeMySqlSchemaStatements},
        {"streaming exact duplicate grouping", TestExactGroupAccumulator},
        {"media dHash and 2x3 contact sheet", TestMediaAnalysisAndContactSheet},
        {"corrupt image dHash safety", TestCorruptImageDHashFailsSafely},
        {"FFmpeg image preview generation", TestImagePreviewGeneration},
        {"image perceptual and structural APIs", TestImagePerceptualAndStructuralApis},
        {"image three-stage similarity rules", TestImageSimilarityRules},
        {"stable image similarity fixtures", TestStableImageSimilarityFixtures},
        {"PDQ candidates match brute force", TestPdqCandidateBuilderAgainstBruteForce},
        {"image feature backfill checkpoint", TestImageFeatureBackfillCheckpoint},
        {"structural scheduler budgets", TestStructuralVerificationSchedulerBudgets},
        {"dHash exact-radius index recall", TestDHashSegmentationGuarantee},
        {"strict image complete-link grouping", TestStrictImageCompleteLinkGrouping},
        {"video similarity rules", TestVideoSimilarityRules},
        {"RocksDB dHash candidate index", TestDHashCandidateIndex},
        {"persistent similarity report", TestPersistentSimilarityReport},
        {"skipped visual content records", TestSkippedVisualContentRecords},
        {"duplicate report deletion by kind", TestDeleteDuplicateReportsByKind},
        {"legacy report regeneration requirement", TestLegacyReportRequiresRegeneration},
        {"bounded runtime log feed", TestRuntimeLogFeed},
        {"execution and failure logs", TestExecutionLogger},
        {"scan bad-block error log", TestScanErrorLog},
        {"binary RocksDB model codec", TestCoreModelBinaryCodec},
        {"streaming native file discovery", TestNativeFileDiscovery},
        {"end-to-end incremental scan", TestEndToEndIncrementalScan},
        {"scan cancellation snapshot", TestScanCancellationSnapshot},
        {"deletion planner safety", TestDeletionPlannerSafety},
        {"safe permanent deletion", TestSafePermanentDeletion},
        {"application exception log", TestApplicationErrorLog},
        {"persistent report selection", TestPersistentReportSelection},
        {"task thread pool lifecycle", TestTaskThreadPoolLifecycle},
        {"exclusive RocksDB open", TestRocksStoreExclusiveOpen},
        {"DLL exception boundary contracts", TestDllExceptionBoundaryContracts},
    };
    int failures = 0;
    for (const TestCase& test : tests) {
        try {
            test.run();
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& exception) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << exception.what() << '\n';
        }
    }
    std::cout << tests.size() - static_cast<std::size_t>(failures) << "/" << tests.size() << " passed\n";
    return failures == 0 ? 0 : 1;
}
