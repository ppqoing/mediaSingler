#include "JsonConfigStore.h"

#include "ConfigValidator.h"
#include "DpapiSecretProtector.h"

#include <Windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace videosc::dedup {
namespace {

using Json = nlohmann::ordered_json;

/** @brief 内部解析结果，用于区分损坏配置和未来版本配置。 */
struct ParseResult {
    bool succeeded = false;
    bool unsupported_version = false;
    AppConfig config;
    std::vector<std::wstring> warnings;
    std::wstring error;
};

/** @brief 把 UTF-16 文本严格转换为 UTF-8 JSON 字符串。 */
std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int count = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) {
        throw std::runtime_error("Failed to encode UTF-16 configuration value");
    }
    std::string result(static_cast<std::size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8,
                            WC_ERR_INVALID_CHARS,
                            value.data(),
                            static_cast<int>(value.size()),
                            result.data(),
                            count,
                            nullptr,
                            nullptr) != count) {
        throw std::runtime_error("Failed to encode UTF-16 configuration value");
    }
    return result;
}

/** @brief 把 UTF-8 JSON 字符串严格转换为 UTF-16。 */
std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int count = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) {
        throw std::runtime_error("Invalid UTF-8 configuration value");
    }
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count) != count) {
        throw std::runtime_error("Invalid UTF-8 configuration value");
    }
    return result;
}

/** @brief 将 std::exception 文本转成可在中文 GUI 中附带显示的 UTF-16。 */
std::wstring ExceptionText(const std::exception& exception) {
    try {
        return Utf8ToWide(exception.what());
    } catch (...) {
        return L"未知解析错误";
    }
}

/** @brief 严格读取完整文件，拒绝大于 64 MiB 的异常配置。 */
std::string ReadUtf8File(const std::filesystem::path& path) {
    HANDLE file = CreateFileW(path.c_str(),
                              GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "CreateFileW");
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size)) {
        const DWORD error = GetLastError();
        CloseHandle(file);
        throw std::system_error(static_cast<int>(error), std::system_category(), "GetFileSizeEx");
    }
    constexpr LONGLONG kMaximumConfigBytes = 64LL * 1024LL * 1024LL;
    if (size.QuadPart < 0 || size.QuadPart > kMaximumConfigBytes) {
        CloseHandle(file);
        throw std::runtime_error("Configuration file size is invalid");
    }

    std::string content(static_cast<std::size_t>(size.QuadPart), '\0');
    std::size_t offset = 0;
    while (offset < content.size()) {
        const DWORD requested = static_cast<DWORD>(
            std::min<std::size_t>(content.size() - offset, std::numeric_limits<DWORD>::max()));
        DWORD read = 0;
        if (!ReadFile(file, content.data() + offset, requested, &read, nullptr)) {
            const DWORD error = GetLastError();
            CloseHandle(file);
            throw std::system_error(static_cast<int>(error), std::system_category(), "ReadFile");
        }
        if (read == 0) {
            CloseHandle(file);
            throw std::runtime_error("Configuration file ended unexpectedly");
        }
        offset += read;
    }
    CloseHandle(file);

    constexpr unsigned char kUtf8Bom[] = {0xEF, 0xBB, 0xBF};
    if (content.size() >= 3 && static_cast<unsigned char>(content[0]) == kUtf8Bom[0] &&
        static_cast<unsigned char>(content[1]) == kUtf8Bom[1] &&
        static_cast<unsigned char>(content[2]) == kUtf8Bom[2]) {
        content.erase(0, 3);
    }
    return content;
}

/** @brief 将枚举转换为稳定的 JSON 字符串。 */
std::string ToString(const StorageMediaType value) {
    switch (value) {
        case StorageMediaType::Hdd:
            return "hdd";
        case StorageMediaType::Ssd:
            return "ssd";
        default:
            return "unknown";
    }
}

/** @brief 将 JSON 字符串解析为存储介质枚举。 */
StorageMediaType ParseStorageMediaType(const std::string& value) {
    if (value == "hdd") {
        return StorageMediaType::Hdd;
    }
    if (value == "ssd") {
        return StorageMediaType::Ssd;
    }
    if (value == "unknown") {
        return StorageMediaType::Unknown;
    }
    throw std::runtime_error("Unknown storage media type");
}

/** @brief 将 MySQL TLS 枚举转换为稳定的 JSON 字符串。 */
std::string ToString(const MySqlTlsMode value) {
    switch (value) {
        case MySqlTlsMode::Disabled:
            return "disabled";
        case MySqlTlsMode::Required:
            return "required";
        case MySqlTlsMode::VerifyCa:
            return "verify_ca";
        case MySqlTlsMode::VerifyIdentity:
            return "verify_identity";
        default:
            return "preferred";
    }
}

/** @brief 将 JSON 字符串解析为 MySQL TLS 枚举。 */
MySqlTlsMode ParseTlsMode(const std::string& value) {
    if (value == "disabled") {
        return MySqlTlsMode::Disabled;
    }
    if (value == "preferred") {
        return MySqlTlsMode::Preferred;
    }
    if (value == "required") {
        return MySqlTlsMode::Required;
    }
    if (value == "verify_ca") {
        return MySqlTlsMode::VerifyCa;
    }
    if (value == "verify_identity") {
        return MySqlTlsMode::VerifyIdentity;
    }
    throw std::runtime_error("Unknown MySQL TLS mode");
}

/** @brief 将缩略图格式枚举转换为 JSON 字符串。 */
std::string ToString(const ThumbnailFormat value) {
    return value == ThumbnailFormat::Png ? "png" : "jpeg";
}

/** @brief 将 JSON 字符串解析为缩略图格式。 */
ThumbnailFormat ParseThumbnailFormat(const std::string& value) {
    if (value == "jpeg") {
        return ThumbnailFormat::Jpeg;
    }
    if (value == "png") {
        return ThumbnailFormat::Png;
    }
    throw std::runtime_error("Unknown thumbnail format");
}

const char* ToString(DiscoveryMethod method) {
    switch (method) {
        case DiscoveryMethod::Native:     return "native";
        case DiscoveryMethod::Everything: return "everything";
    }
    return "everything";
}

DiscoveryMethod ParseDiscoveryMethod(const std::string& value) {
    if (value == "native")     return DiscoveryMethod::Native;
    if (value == "everything") return DiscoveryMethod::Everything;
    return DiscoveryMethod::Everything;
}

/** @brief 读取必需对象，避免错误类型被 value() 静默掩盖。 */
const Json& RequiredObject(const Json& parent, const char* key) {
    const auto iterator = parent.find(key);
    if (iterator == parent.end() || !iterator->is_object()) {
        throw std::runtime_error(std::string("Missing object: ") + key);
    }
    return *iterator;
}

/** @brief 读取 UTF-8 字符串字段并转为 UTF-16。 */
std::wstring ReadWide(const Json& object, const char* key, const std::wstring& fallback = {}) {
    const auto iterator = object.find(key);
    if (iterator == object.end()) {
        return fallback;
    }
    return Utf8ToWide(iterator->get<std::string>());
}

/**
 * @brief 把一套图片三级阈值编码为稳定 JSON 对象。
 * @param profile 待编码阈值。
 * @return 不包含运行控制项的阈值对象。
 */
Json SerializeImageProfile(const ImageSimilarityThresholdProfile& profile) {
    return {{"pdq_max_hamming_distance", profile.pdq_max_hamming_distance},
            {"fallback_dhash_max_hamming_distance", profile.fallback_dhash_max_hamming_distance},
            {"zoned_phash_tile_max_distance", profile.zoned_phash_tile_max_distance},
            {"zoned_phash_min_passing_tiles", profile.zoned_phash_min_passing_tiles},
            {"zoned_phash_max_ignored_tiles", profile.zoned_phash_max_ignored_tiles},
            {"zoned_phash_trimmed_mean_max", profile.zoned_phash_trimmed_mean_max},
            {"structural_global_edge_min_millionths", profile.structural_global_edge_min_millionths},
            {"structural_trimmed_block_min_millionths", profile.structural_trimmed_block_min_millionths},
            {"structural_block_pass_score_millionths", profile.structural_block_pass_score_millionths},
            {"structural_min_passing_percent_millionths",
             profile.structural_min_passing_percent_millionths}};
}

/**
 * @brief 从 JSON 对象覆盖一套图片三级阈值。
 * @param object 配置对象；缺失字段保留现有默认值。
 * @param profile 输出阈值。
 */
void DeserializeImageProfile(const Json& object, ImageSimilarityThresholdProfile& profile) {
    if (!object.is_object()) return;
    profile.pdq_max_hamming_distance =
        object.value("pdq_max_hamming_distance", profile.pdq_max_hamming_distance);
    profile.fallback_dhash_max_hamming_distance = object.value(
        "fallback_dhash_max_hamming_distance", profile.fallback_dhash_max_hamming_distance);
    profile.zoned_phash_tile_max_distance =
        object.value("zoned_phash_tile_max_distance", profile.zoned_phash_tile_max_distance);
    profile.zoned_phash_min_passing_tiles =
        object.value("zoned_phash_min_passing_tiles", profile.zoned_phash_min_passing_tiles);
    profile.zoned_phash_max_ignored_tiles =
        object.value("zoned_phash_max_ignored_tiles", profile.zoned_phash_max_ignored_tiles);
    profile.zoned_phash_trimmed_mean_max =
        object.value("zoned_phash_trimmed_mean_max", profile.zoned_phash_trimmed_mean_max);
    profile.structural_global_edge_min_millionths =
        object.value("structural_global_edge_min_millionths",
                     profile.structural_global_edge_min_millionths);
    profile.structural_trimmed_block_min_millionths =
        object.value("structural_trimmed_block_min_millionths",
                     profile.structural_trimmed_block_min_millionths);
    profile.structural_block_pass_score_millionths =
        object.value("structural_block_pass_score_millionths",
                     profile.structural_block_pass_score_millionths);
    profile.structural_min_passing_percent_millionths =
        object.value("structural_min_passing_percent_millionths",
                     profile.structural_min_passing_percent_millionths);
}

/** @brief 将 AppConfig 序列化为稳定分区的 JSON；绝不写入 database.password 明文。 */
Json SerializeConfig(const AppConfig& config, const std::string& protected_password) {
    Json root;
    root["schema_version"] = config.schema_version;

    root["paths"]["scan_roots"] = Json::array();
    for (const std::filesystem::path& path : config.paths.scan_roots) {
        root["paths"]["scan_roots"].push_back(WideToUtf8(path.wstring()));
    }

    root["storage"] = {{"max_concurrent_file_reads", config.storage.max_concurrent_file_reads},
                       {"hdd_read_threads_per_disk", config.storage.hdd_read_threads_per_disk},
                       {"ssd_read_threads_per_disk", config.storage.ssd_read_threads_per_disk},
                       {"adaptive_read_threads", config.storage.adaptive_read_threads},
                       {"disk_read_target_percent", config.storage.disk_read_target_percent}};

    root["compute"] = {{"worker_threads", config.compute.worker_threads},
                       {"ffmpeg_threads_per_task", config.compute.ffmpeg_threads_per_task},
                       {"adaptive_worker_threads", config.compute.adaptive_worker_threads},
                       {"cpu_target_percent", config.compute.cpu_target_percent}};
    root["io"] = {{"read_block_kib", config.io.read_block_kib},
                  {"per_disk_queue_capacity", config.io.per_disk_queue_capacity},
                  {"hdd_extent_optimization", config.io.hdd_extent_optimization},
                  {"hdd_sort_window", config.io.hdd_sort_window},
                  {"normal_block_retries", config.io.normal_block_retries},
                  {"small_block_retries", config.io.small_block_retries},
                  {"small_block_kib", config.io.small_block_kib},
                  {"no_progress_timeout_seconds", config.io.no_progress_timeout_seconds}};

    {
        Json discovery;
        discovery["method"] = ToString(config.discovery.method);
        if (!config.discovery.everything_dll_path.empty())
            discovery["everything_dll_path"] = WideToUtf8(config.discovery.everything_dll_path.wstring());
        if (!config.discovery.everything_exe_path.empty())
            discovery["everything_exe_path"] = WideToUtf8(config.discovery.everything_exe_path.wstring());
        discovery["query_page_size"] = config.discovery.query_page_size;
        discovery["launch_timeout_seconds"] = config.discovery.launch_timeout_seconds;
        discovery["db_load_timeout_seconds"] = config.discovery.db_load_timeout_seconds;
        discovery["poll_interval_milliseconds"] = config.discovery.poll_interval_milliseconds;
        root["discovery"] = std::move(discovery);
    }

    root["database"] = {{"host", WideToUtf8(config.database.host)},
                        {"port", config.database.port},
                        {"database_name", WideToUtf8(config.database.database_name)},
                        {"user_name", WideToUtf8(config.database.user_name)},
                        {"password_protection", "dpapi-current-user"},
                        {"password_protected", protected_password},
                        {"tls_mode", ToString(config.database.tls_mode)},
                        {"tls_ca_path", WideToUtf8(config.database.tls_ca_path.wstring())},
                        {"tls_certificate_path", WideToUtf8(config.database.tls_certificate_path.wstring())},
                        {"tls_private_key_path", WideToUtf8(config.database.tls_private_key_path.wstring())},
                        {"connection_pool_size", config.database.connection_pool_size},
                        {"connect_timeout_seconds", config.database.connect_timeout_seconds},
                        {"command_timeout_seconds", config.database.command_timeout_seconds},
                        {"retry_interval_seconds", config.database.retry_interval_seconds},
                        {"sync_batch_size", config.database.sync_batch_size},
                        {"mysqldump_path", WideToUtf8(config.database.mysqldump_path.wstring())},
                        {"backup_directory", WideToUtf8(config.database.backup_directory.wstring())}};

    root["thumbnails"] = {{"root_directory", WideToUtf8(config.thumbnails.root_directory.wstring())},
                          {"format", ToString(config.thumbnails.format)},
                          {"video_cell_long_edge", config.thumbnails.video_cell_long_edge},
                          {"image_preview_long_edge", config.thumbnails.image_preview_long_edge},
                          {"cache_entries", config.thumbnails.cache_entries},
                          {"memory_limit_mib", config.thumbnails.memory_limit_mib},
                          {"gpu_memory_limit_mib", config.thumbnails.gpu_memory_limit_mib}};
    root["dhash_similarity"] = {
        {"image_max_hamming_distance", config.dhash_similarity.image_max_hamming_distance},
        {"video_max_average_hamming_distance",
         config.dhash_similarity.video_max_average_hamming_distance},
        {"image_aspect_ratio_tolerance_percent",
         config.dhash_similarity.image_aspect_ratio_tolerance_percent},
        {"validation_worker_threads", config.dhash_similarity.validation_worker_threads}};
    root["image_similarity"] = {
        {"aspect_ratio_tolerance_percent", config.image_similarity.aspect_ratio_tolerance_percent},
        {"pdq_min_quality", config.image_similarity.pdq_min_quality},
        {"standard_profile", SerializeImageProfile(config.image_similarity.standard_profile)},
        {"low_quality_profile", SerializeImageProfile(config.image_similarity.low_quality_profile)},
        {"report_validation_worker_threads", config.image_similarity.report_validation_worker_threads},
        {"structural_worker_threads", config.image_similarity.structural_worker_threads},
        {"structural_cache_mib", config.image_similarity.structural_cache_mib},
        {"candidate_memory_mib", config.image_similarity.candidate_memory_mib},
        {"candidate_temp_mib", config.image_similarity.candidate_temp_mib},
        {"candidate_max_pairs", config.image_similarity.candidate_max_pairs},
        {"hot_signature_max_members", config.image_similarity.hot_signature_max_members},
        {"hot_signature_max_pairs", config.image_similarity.hot_signature_max_pairs},
        {"candidate_write_batch_size", config.image_similarity.candidate_write_batch_size},
        {"candidate_cancel_check_stride", config.image_similarity.candidate_cancel_check_stride},
        {"require_complete_features", config.image_similarity.require_complete_features},
        {"allow_partial_reports", config.image_similarity.allow_partial_reports},
        {"force_scalar_kernels", config.image_similarity.force_scalar_kernels}};
    root["report_selection"]["image_dhash_distance_exclusive_limit"] =
        config.report_selection.image_dhash_distance_exclusive_limit.has_value()
            ? Json(*config.report_selection.image_dhash_distance_exclusive_limit)
            : Json(nullptr);
    root["report_selection"]["video_dhash_average_distance_exclusive_limit"] =
        config.report_selection.video_dhash_average_distance_exclusive_limit.has_value()
            ? Json(*config.report_selection.video_dhash_average_distance_exclusive_limit)
            : Json(nullptr);
    root["rocksdb"] = {{"directory", WideToUtf8(config.rocksdb.directory.wstring())},
                       {"block_cache_mib", config.rocksdb.block_cache_mib},
                       {"write_buffer_mib", config.rocksdb.write_buffer_mib}};
    root["logging"] = {{"directory", WideToUtf8(config.logging.directory.wstring())},
                       {"execution_directory", WideToUtf8(config.logging.execution_directory.wstring())},
                       {"rotate_file_mib", config.logging.rotate_file_mib},
                       {"rotate_count", config.logging.rotate_count},
                       {"retention_days", config.logging.retention_days}};
    return root;
}

/** @brief 将已确认版本的 JSON 完整反序列化为 AppConfig。 */
AppConfig DeserializeConfig(const Json& root, const std::filesystem::path& install_directory) {
    AppConfig config = AppConfig::CreateDefault(install_directory);
    config.schema_version = kCurrentConfigSchemaVersion;

    const Json& paths = RequiredObject(root, "paths");
    config.paths.scan_roots.clear();
    for (const Json& path : paths.at("scan_roots")) {
        config.paths.scan_roots.emplace_back(Utf8ToWide(path.get<std::string>()));
    }

    const Json& storage = RequiredObject(root, "storage");
    config.storage.max_concurrent_file_reads =
        storage.value("max_concurrent_file_reads", config.storage.max_concurrent_file_reads);
    config.storage.hdd_read_threads_per_disk =
        storage.value("hdd_read_threads_per_disk", config.storage.hdd_read_threads_per_disk);
    config.storage.ssd_read_threads_per_disk =
        storage.value("ssd_read_threads_per_disk", config.storage.ssd_read_threads_per_disk);
    config.storage.adaptive_read_threads =
        storage.value("adaptive_read_threads", config.storage.adaptive_read_threads);
    config.storage.disk_read_target_percent =
        storage.value("disk_read_target_percent", config.storage.disk_read_target_percent);

    const Json& compute = RequiredObject(root, "compute");
    config.compute.worker_threads = compute.at("worker_threads").get<std::uint32_t>();
    config.compute.ffmpeg_threads_per_task = compute.at("ffmpeg_threads_per_task").get<std::uint32_t>();
    config.compute.adaptive_worker_threads =
        compute.value("adaptive_worker_threads", config.compute.adaptive_worker_threads);
    config.compute.cpu_target_percent =
        compute.value("cpu_target_percent", config.compute.cpu_target_percent);

    const Json& io = RequiredObject(root, "io");
    config.io.read_block_kib = io.at("read_block_kib").get<std::uint32_t>();
    config.io.per_disk_queue_capacity = io.at("per_disk_queue_capacity").get<std::uint32_t>();
    config.io.hdd_extent_optimization = io.at("hdd_extent_optimization").get<bool>();
    config.io.hdd_sort_window = io.at("hdd_sort_window").get<std::uint32_t>();
    config.io.normal_block_retries = io.at("normal_block_retries").get<std::uint32_t>();
    config.io.small_block_retries = io.at("small_block_retries").get<std::uint32_t>();
    config.io.small_block_kib = io.at("small_block_kib").get<std::uint32_t>();
    config.io.no_progress_timeout_seconds = io.at("no_progress_timeout_seconds").get<std::uint32_t>();

    if (root.contains("discovery") && root["discovery"].is_object()) {
        const Json& discovery = root["discovery"];
        if (discovery.contains("method") && discovery["method"].is_string())
            config.discovery.method = ParseDiscoveryMethod(discovery["method"].get<std::string>());
        if (discovery.contains("everything_dll_path") && discovery["everything_dll_path"].is_string())
            config.discovery.everything_dll_path = ReadWide(discovery, "everything_dll_path");
        if (discovery.contains("everything_exe_path") && discovery["everything_exe_path"].is_string())
            config.discovery.everything_exe_path = ReadWide(discovery, "everything_exe_path");
        config.discovery.query_page_size =
            discovery.value("query_page_size", config.discovery.query_page_size);
        config.discovery.launch_timeout_seconds =
            discovery.value("launch_timeout_seconds", config.discovery.launch_timeout_seconds);
        config.discovery.db_load_timeout_seconds =
            discovery.value("db_load_timeout_seconds", config.discovery.db_load_timeout_seconds);
        config.discovery.poll_interval_milliseconds =
            discovery.value("poll_interval_milliseconds", config.discovery.poll_interval_milliseconds);
    }

    const Json& database = RequiredObject(root, "database");
    config.database.host = ReadWide(database, "host");
    config.database.port = database.at("port").get<std::uint16_t>();
    config.database.database_name = ReadWide(database, "database_name");
    config.database.user_name = ReadWide(database, "user_name");
    if (database.value("password_protection", std::string("dpapi-current-user")) != "dpapi-current-user") {
        throw std::runtime_error("Unsupported password protection method");
    }
    config.database.password_protected = database.value("password_protected", std::string{});
    config.database.tls_mode = ParseTlsMode(database.at("tls_mode").get<std::string>());
    config.database.tls_ca_path = ReadWide(database, "tls_ca_path");
    config.database.tls_certificate_path = ReadWide(database, "tls_certificate_path");
    config.database.tls_private_key_path = ReadWide(database, "tls_private_key_path");
    config.database.connection_pool_size = database.at("connection_pool_size").get<std::uint32_t>();
    config.database.connect_timeout_seconds = database.at("connect_timeout_seconds").get<std::uint32_t>();
    config.database.command_timeout_seconds = database.at("command_timeout_seconds").get<std::uint32_t>();
    config.database.retry_interval_seconds = database.at("retry_interval_seconds").get<std::uint32_t>();
    config.database.sync_batch_size = database.at("sync_batch_size").get<std::uint32_t>();
    config.database.mysqldump_path = ReadWide(database, "mysqldump_path");
    config.database.backup_directory = ReadWide(database, "backup_directory");

    const Json& thumbnails = RequiredObject(root, "thumbnails");
    config.thumbnails.root_directory = ReadWide(thumbnails, "root_directory");
    config.thumbnails.format = ParseThumbnailFormat(thumbnails.at("format").get<std::string>());
    config.thumbnails.video_cell_long_edge = thumbnails.at("video_cell_long_edge").get<std::uint32_t>();
    config.thumbnails.image_preview_long_edge = thumbnails.at("image_preview_long_edge").get<std::uint32_t>();
    config.thumbnails.cache_entries = thumbnails.at("cache_entries").get<std::uint32_t>();
    config.thumbnails.memory_limit_mib = thumbnails.at("memory_limit_mib").get<std::uint32_t>();
    config.thumbnails.gpu_memory_limit_mib = thumbnails.at("gpu_memory_limit_mib").get<std::uint32_t>();

    if (root.contains("dhash_similarity") && root["dhash_similarity"].is_object()) {
        const Json& dhashSimilarity = root["dhash_similarity"];
        config.dhash_similarity.image_max_hamming_distance =
            dhashSimilarity.value("image_max_hamming_distance",
                                  config.dhash_similarity.image_max_hamming_distance);
        config.dhash_similarity.video_max_average_hamming_distance =
            dhashSimilarity.value("video_max_average_hamming_distance",
                                  config.dhash_similarity.video_max_average_hamming_distance);
        config.dhash_similarity.image_aspect_ratio_tolerance_percent =
            dhashSimilarity.value("image_aspect_ratio_tolerance_percent",
                                  config.dhash_similarity.image_aspect_ratio_tolerance_percent);
        config.dhash_similarity.validation_worker_threads =
            dhashSimilarity.value("validation_worker_threads",
                                  config.dhash_similarity.validation_worker_threads);
    }

    if (root.contains("image_similarity") && root["image_similarity"].is_object()) {
        const Json& imageSimilarity = root["image_similarity"];
        config.image_similarity.aspect_ratio_tolerance_percent =
            imageSimilarity.value("aspect_ratio_tolerance_percent",
                                  config.image_similarity.aspect_ratio_tolerance_percent);
        config.image_similarity.pdq_min_quality =
            imageSimilarity.value("pdq_min_quality", config.image_similarity.pdq_min_quality);
        if (imageSimilarity.contains("standard_profile")) {
            DeserializeImageProfile(imageSimilarity["standard_profile"],
                                    config.image_similarity.standard_profile);
        } else {
            // v4 以前的扁平阈值等价迁入标准配置。
            DeserializeImageProfile(imageSimilarity, config.image_similarity.standard_profile);
        }
        if (imageSimilarity.contains("low_quality_profile")) {
            DeserializeImageProfile(imageSimilarity["low_quality_profile"],
                                    config.image_similarity.low_quality_profile);
        } else {
            // 旧配置迁移时把保守默认值继续收紧到不弱于用户原标准阈值。
            auto& low = config.image_similarity.low_quality_profile;
            const auto& standard = config.image_similarity.standard_profile;
            low.pdq_max_hamming_distance = (std::min)(low.pdq_max_hamming_distance,
                                                       standard.pdq_max_hamming_distance);
            low.fallback_dhash_max_hamming_distance = (std::min)(
                low.fallback_dhash_max_hamming_distance,
                standard.fallback_dhash_max_hamming_distance);
            low.zoned_phash_tile_max_distance = (std::min)(low.zoned_phash_tile_max_distance,
                                                            standard.zoned_phash_tile_max_distance);
            low.zoned_phash_min_passing_tiles = (std::max)(low.zoned_phash_min_passing_tiles,
                                                            standard.zoned_phash_min_passing_tiles);
            low.zoned_phash_max_ignored_tiles = (std::min)(low.zoned_phash_max_ignored_tiles,
                                                            standard.zoned_phash_max_ignored_tiles);
            low.zoned_phash_trimmed_mean_max = (std::min)(low.zoned_phash_trimmed_mean_max,
                                                           standard.zoned_phash_trimmed_mean_max);
            low.structural_global_edge_min_millionths =
                (std::max)(low.structural_global_edge_min_millionths,
                           standard.structural_global_edge_min_millionths);
            low.structural_trimmed_block_min_millionths =
                (std::max)(low.structural_trimmed_block_min_millionths,
                           standard.structural_trimmed_block_min_millionths);
            low.structural_block_pass_score_millionths =
                (std::max)(low.structural_block_pass_score_millionths,
                           standard.structural_block_pass_score_millionths);
            low.structural_min_passing_percent_millionths =
                (std::max)(low.structural_min_passing_percent_millionths,
                           standard.structural_min_passing_percent_millionths);
        }
        config.image_similarity.report_validation_worker_threads =
            imageSimilarity.value("report_validation_worker_threads",
                                  config.image_similarity.report_validation_worker_threads);
        config.image_similarity.structural_worker_threads =
            imageSimilarity.value("structural_worker_threads",
                                  config.image_similarity.structural_worker_threads);
        config.image_similarity.structural_cache_mib =
            imageSimilarity.value("structural_cache_mib",
                                  config.image_similarity.structural_cache_mib);
        config.image_similarity.candidate_memory_mib =
            imageSimilarity.value("candidate_memory_mib", config.image_similarity.candidate_memory_mib);
        config.image_similarity.candidate_temp_mib =
            imageSimilarity.value("candidate_temp_mib", config.image_similarity.candidate_temp_mib);
        config.image_similarity.candidate_max_pairs =
            imageSimilarity.value("candidate_max_pairs", config.image_similarity.candidate_max_pairs);
        config.image_similarity.hot_signature_max_members =
            imageSimilarity.value("hot_signature_max_members",
                                  config.image_similarity.hot_signature_max_members);
        config.image_similarity.hot_signature_max_pairs =
            imageSimilarity.value("hot_signature_max_pairs", config.image_similarity.hot_signature_max_pairs);
        config.image_similarity.candidate_write_batch_size =
            imageSimilarity.value("candidate_write_batch_size",
                                  config.image_similarity.candidate_write_batch_size);
        config.image_similarity.candidate_cancel_check_stride =
            imageSimilarity.value("candidate_cancel_check_stride",
                                  config.image_similarity.candidate_cancel_check_stride);
        config.image_similarity.require_complete_features =
            imageSimilarity.value("require_complete_features",
                                  config.image_similarity.require_complete_features);
        config.image_similarity.allow_partial_reports =
            imageSimilarity.value("allow_partial_reports", config.image_similarity.allow_partial_reports);
        config.image_similarity.force_scalar_kernels =
            imageSimilarity.value("force_scalar_kernels", config.image_similarity.force_scalar_kernels);
    }

    if (root.contains("report_selection") && root["report_selection"].is_object()) {
        const Json& selection = root["report_selection"];
        const auto image = selection.find("image_dhash_distance_exclusive_limit");
        if (image != selection.end() && !image->is_null()) {
            config.report_selection.image_dhash_distance_exclusive_limit = image->get<std::uint32_t>();
        }
        const auto video = selection.find("video_dhash_average_distance_exclusive_limit");
        if (video != selection.end() && !video->is_null()) {
            config.report_selection.video_dhash_average_distance_exclusive_limit = video->get<double>();
        }
    }

    const Json& rocksdb = RequiredObject(root, "rocksdb");
    config.rocksdb.directory = ReadWide(rocksdb, "directory");
    config.rocksdb.block_cache_mib = rocksdb.at("block_cache_mib").get<std::uint32_t>();
    config.rocksdb.write_buffer_mib = rocksdb.at("write_buffer_mib").get<std::uint32_t>();

    const Json& logging = RequiredObject(root, "logging");
    config.logging.directory = ReadWide(logging, "directory");
    config.logging.execution_directory = logging.contains("execution_directory")
                                             ? std::filesystem::path(ReadWide(logging, "execution_directory"))
                                             : config.logging.directory / L"execution";
    config.logging.rotate_file_mib = logging.at("rotate_file_mib").get<std::uint32_t>();
    config.logging.rotate_count = logging.at("rotate_count").get<std::uint32_t>();
    config.logging.retention_days = logging.at("retention_days").get<std::uint32_t>();
    return config;
}

/** @brief 解析一个候选文件，并把 DPAPI 失败降级为凭据待重输告警。 */
ParseResult ParseConfigFile(const std::filesystem::path& path, const std::filesystem::path& install_directory) {
    ParseResult result;
    try {
        const Json root = Json::parse(ReadUtf8File(path));
        if (!root.is_object() || !root.contains("schema_version") || !root["schema_version"].is_number_unsigned()) {
            throw std::runtime_error("Missing unsigned schema_version");
        }
        const std::uint32_t version = root["schema_version"].get<std::uint32_t>();
        if (version > kCurrentConfigSchemaVersion) {
            result.unsupported_version = true;
            result.error = L"配置文件版本高于当前程序支持版本，已禁止覆盖。";
            return result;
        }
        if (version < 1) {
            result.error = L"配置文件版本过旧，无法迁移。";
            return result;
        }

        result.config = DeserializeConfig(root, install_directory);
        const auto issues = ConfigValidator::Validate(result.config);
        if (ConfigValidator::HasErrors(issues)) {
            const auto first_error = std::find_if(issues.begin(), issues.end(), [](const ValidationIssue& issue) {
                return issue.severity == ValidationSeverity::Error;
            });
            result.error = first_error == issues.end() ? L"配置校验失败。" : first_error->message;
            return result;
        }
        for (const ValidationIssue& issue : issues) {
            if (issue.severity == ValidationSeverity::Warning) {
                result.warnings.push_back(issue.message);
            }
        }

        if (!result.config.database.password_protected.empty()) {
            try {
                result.config.database.password =
                    DpapiSecretProtector::Unprotect(result.config.database.password_protected);
            } catch (const std::exception&) {
                result.config.database.password.clear();
                result.config.database.password_decryption_failed = true;
                result.warnings.push_back(L"MySQL 密码无法由当前 Windows 用户解密，请重新输入并保存。数据库操作已禁用。");
            }
        }
        result.succeeded = true;
    } catch (const std::exception& exception) {
        result.error = L"配置文件解析失败：" + ExceptionText(exception);
    }
    return result;
}

/** @brief 把 UTF-8 JSON 写入同目录临时文件并强制刷新到存储设备。 */
DWORD WriteAndFlush(const std::filesystem::path& path, const std::string& content) {
    HANDLE file = CreateFileW(path.c_str(),
                              GENERIC_WRITE,
                              0,
                              nullptr,
                              CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return GetLastError();
    }
    std::size_t offset = 0;
    while (offset < content.size()) {
        const DWORD requested = static_cast<DWORD>(
            std::min<std::size_t>(content.size() - offset, std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (!WriteFile(file, content.data() + offset, requested, &written, nullptr) || written == 0) {
            const DWORD error = GetLastError();
            CloseHandle(file);
            return error == ERROR_SUCCESS ? ERROR_WRITE_FAULT : error;
        }
        offset += written;
    }
    if (!FlushFileBuffers(file)) {
        const DWORD error = GetLastError();
        CloseHandle(file);
        return error;
    }
    if (!CloseHandle(file)) {
        return GetLastError();
    }
    return ERROR_SUCCESS;
}

/** @brief 对已复制的备份文件执行 FlushFileBuffers，确保替换前备份可恢复。 */
DWORD FlushExistingFile(const std::filesystem::path& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return GetLastError();
    }
    const BOOL flushed = FlushFileBuffers(file);
    const DWORD error = flushed ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    return error;
}

/** @brief 生成带 Win32 错误码的中文保存失败结果。 */
ConfigSaveResult SaveFailure(const std::filesystem::path& path,
                             const std::wstring& operation,
                             const DWORD system_error) {
    ConfigSaveResult result;
    result.path = path;
    result.system_error = system_error;
    result.error_message = operation + L"失败，路径：" + path.wstring();
    if (system_error != ERROR_SUCCESS) {
        result.error_message += L"，Win32 错误码：" + std::to_wstring(system_error);
    }
    return result;
}

}  // namespace

JsonConfigStore::JsonConfigStore(std::filesystem::path config_path) : config_path_(std::move(config_path)) {}

JsonConfigStore JsonConfigStore::ForApplicationDirectory() {
    return JsonConfigStore(GetApplicationDirectory() / L"config.json");
}

const std::filesystem::path& JsonConfigStore::config_path() const noexcept {
    return config_path_;
}

std::filesystem::path JsonConfigStore::backup_path() const {
    return config_path_.wstring() + L".bak";
}

ConfigLoadResult JsonConfigStore::Load() const {
    ConfigLoadResult result;
    const std::filesystem::path install_directory = config_path_.parent_path();
    result.config = AppConfig::CreateDefault(install_directory);

    std::error_code exists_error;
    const bool primary_exists = std::filesystem::exists(config_path_, exists_error);
    if (!primary_exists && !exists_error) {
        result.status = ConfigLoadStatus::MissingUsingDefaults;
        result.warnings.push_back(L"配置文件不存在，当前使用内置默认值；保存后将创建 config.json。");
        return result;
    }

    ParseResult primary = ParseConfigFile(config_path_, install_directory);
    if (primary.succeeded) {
        result.config = std::move(primary.config);
        result.source = ConfigSource::PrimaryFile;
        result.status = ConfigLoadStatus::Loaded;
        result.warnings = std::move(primary.warnings);
        return result;
    }
    if (primary.unsupported_version) {
        result.status = ConfigLoadStatus::UnsupportedVersion;
        result.can_save = false;
        result.warnings.push_back(primary.error);
        return result;
    }

    ParseResult backup = ParseConfigFile(backup_path(), install_directory);
    if (backup.succeeded) {
        result.config = std::move(backup.config);
        result.source = ConfigSource::BackupFile;
        result.status = ConfigLoadStatus::RecoveredFromBackup;
        result.warnings.push_back(L"主配置不可用，已从 config.json.bak 恢复到内存；保存前请确认设置。原因：" + primary.error);
        result.warnings.insert(result.warnings.end(), backup.warnings.begin(), backup.warnings.end());
        return result;
    }
    if (backup.unsupported_version) {
        result.status = ConfigLoadStatus::UnsupportedVersion;
        result.can_save = false;
        result.warnings.push_back(L"主配置损坏，备份版本高于当前程序支持版本，已禁止覆盖。");
        return result;
    }

    result.status = ConfigLoadStatus::InvalidUsingDefaults;
    result.warnings.push_back(L"主配置和备份都不可用，当前使用未保存默认值。主配置原因：" + primary.error +
                              L"；备份原因：" + backup.error);
    return result;
}

ConfigSaveResult JsonConfigStore::Save(const AppConfig& config) const {
    ConfigSaveResult result;
    result.path = config_path_;
    const std::vector<ValidationIssue> issues = ConfigValidator::Validate(config);
    const auto first_error = std::find_if(issues.begin(), issues.end(), [](const ValidationIssue& issue) {
        return issue.severity == ValidationSeverity::Error;
    });
    if (first_error != issues.end()) {
        result.error_message = L"配置校验失败：" + first_error->message;
        return result;
    }

    try {
        std::string protected_password = config.database.password_protected;
        if (!config.database.password.empty()) {
            protected_password = DpapiSecretProtector::Protect(config.database.password);
        } else if (!config.database.password_decryption_failed) {
            protected_password.clear();
        }
        const std::string content = SerializeConfig(config, protected_password).dump(2) + "\n";
        const std::filesystem::path temporary = config_path_.wstring() + L".tmp";
        const DWORD write_error = WriteAndFlush(temporary, content);
        if (write_error != ERROR_SUCCESS) {
            DeleteFileW(temporary.c_str());
            return SaveFailure(config_path_, L"写入并刷新临时配置", write_error);
        }

        if (GetFileAttributesW(config_path_.c_str()) != INVALID_FILE_ATTRIBUTES) {
            if (!CopyFileW(config_path_.c_str(), backup_path().c_str(), FALSE)) {
                const DWORD error = GetLastError();
                DeleteFileW(temporary.c_str());
                return SaveFailure(config_path_, L"创建上一有效配置备份", error);
            }
            const DWORD flush_backup_error = FlushExistingFile(backup_path());
            if (flush_backup_error != ERROR_SUCCESS) {
                DeleteFileW(temporary.c_str());
                return SaveFailure(config_path_, L"刷新上一有效配置备份", flush_backup_error);
            }
            if (!ReplaceFileW(config_path_.c_str(),
                              temporary.c_str(),
                              nullptr,
                              REPLACEFILE_WRITE_THROUGH,
                              nullptr,
                              nullptr)) {
                const DWORD error = GetLastError();
                DeleteFileW(temporary.c_str());
                return SaveFailure(config_path_, L"原子替换主配置", error);
            }
        } else if (!MoveFileExW(temporary.c_str(),
                                config_path_.c_str(),
                                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            const DWORD error = GetLastError();
            DeleteFileW(temporary.c_str());
            return SaveFailure(config_path_, L"创建主配置", error);
        }
        result.succeeded = true;
        return result;
    } catch (const std::exception& exception) {
        result.error_message = L"序列化或加密配置失败：" + ExceptionText(exception);
        return result;
    }
}

}  // namespace videosc::dedup
