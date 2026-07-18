#include "ScanOptionsCodec.h"

#include <Windows.h>

#include <nlohmann/json.hpp>

#include <stdexcept>

namespace videosc::dedup {
namespace {

using Json = nlohmann::ordered_json;
constexpr std::uint32_t kScanOptionsSnapshotVersion = 3;

/** @brief 严格把 UTF-16 转换为 UTF-8。 */
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
    if (length <= 0) throw std::runtime_error("snapshot UTF-16 encode failed");
    std::string result(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8,
                            WC_ERR_INVALID_CHARS,
                            value.data(),
                            static_cast<int>(value.size()),
                            result.data(),
                            length,
                            nullptr,
                            nullptr) != length) {
        throw std::runtime_error("snapshot UTF-16 encode failed");
    }
    return result;
}

/** @brief 严格把 UTF-8 转换为 UTF-16。 */
std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) throw std::runtime_error("snapshot UTF-8 decode failed");
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8,
                            MB_ERR_INVALID_CHARS,
                            value.data(),
                            static_cast<int>(value.size()),
                            result.data(),
                            length) != length) {
        throw std::runtime_error("snapshot UTF-8 decode failed");
    }
    return result;
}

}  // namespace

std::string ScanOptionsCodec::Serialize(const ScanOptions& options) {
    Json root;
    root["snapshot_version"] = kScanOptionsSnapshotVersion;
    root["algorithm_version"] = options.algorithm_version();
    root["scan_roots"] = Json::array();
    for (const std::filesystem::path& path : options.scan_roots()) {
        root["scan_roots"].push_back(WideToUtf8(path.wstring()));
    }
    root["storage"] = {{"max_concurrent_file_reads", options.storage().max_concurrent_file_reads},
                       {"hdd_read_threads_per_disk", options.storage().hdd_read_threads_per_disk},
                       {"ssd_read_threads_per_disk", options.storage().ssd_read_threads_per_disk},
                       {"adaptive_read_threads", options.storage().adaptive_read_threads},
                       {"disk_read_target_percent", options.storage().disk_read_target_percent}};
    root["compute"] = {{"worker_threads", options.compute().worker_threads},
                       {"ffmpeg_threads_per_task", options.compute().ffmpeg_threads_per_task},
                       {"adaptive_worker_threads", options.compute().adaptive_worker_threads},
                       {"cpu_target_percent", options.compute().cpu_target_percent}};
    root["generate_similar_report"] = options.generate_similar_report();
    root["discovery"] = {
        {"method", static_cast<int>(options.discovery().method)},
        {"everything_dll_path", WideToUtf8(options.discovery().everything_dll_path.wstring())},
        {"everything_exe_path", WideToUtf8(options.discovery().everything_exe_path.wstring())},
        {"query_page_size", options.discovery().query_page_size},
        {"launch_timeout_seconds", options.discovery().launch_timeout_seconds},
        {"db_load_timeout_seconds", options.discovery().db_load_timeout_seconds},
        {"poll_interval_milliseconds", options.discovery().poll_interval_milliseconds}};
    root["io"] = {{"read_block_kib", options.io().read_block_kib},
                  {"per_disk_queue_capacity", options.io().per_disk_queue_capacity},
                  {"hdd_extent_optimization", options.io().hdd_extent_optimization},
                  {"hdd_sort_window", options.io().hdd_sort_window},
                  {"normal_block_retries", options.io().normal_block_retries},
                  {"small_block_retries", options.io().small_block_retries},
                  {"small_block_kib", options.io().small_block_kib},
                  {"no_progress_timeout_seconds", options.io().no_progress_timeout_seconds}};
    root["database"] = {{"profile_name", WideToUtf8(options.database().profile_name)},
                        {"host", WideToUtf8(options.database().host)},
                        {"port", options.database().port},
                        {"database_name", WideToUtf8(options.database().database_name)},
                        {"user_name", WideToUtf8(options.database().user_name)},
                        {"tls_mode", static_cast<int>(options.database().tls_mode)},
                        {"tls_ca_path", WideToUtf8(options.database().tls_ca_path.wstring())},
                        {"tls_certificate_path", WideToUtf8(options.database().tls_certificate_path.wstring())},
                        {"tls_private_key_path", WideToUtf8(options.database().tls_private_key_path.wstring())},
                        {"connection_pool_size", options.database().connection_pool_size},
                        {"connect_timeout_seconds", options.database().connect_timeout_seconds},
                        {"command_timeout_seconds", options.database().command_timeout_seconds},
                        {"retry_interval_seconds", options.database().retry_interval_seconds},
                        {"sync_batch_size", options.database().sync_batch_size}};
    root["thumbnails"] = {{"root_directory", WideToUtf8(options.thumbnails().root_directory.wstring())},
                          {"format", static_cast<int>(options.thumbnails().format)},
                          {"video_cell_long_edge", options.thumbnails().video_cell_long_edge},
                          {"image_preview_long_edge", options.thumbnails().image_preview_long_edge},
                          {"cache_entries", options.thumbnails().cache_entries},
                          {"memory_limit_mib", options.thumbnails().memory_limit_mib},
                          {"gpu_memory_limit_mib", options.thumbnails().gpu_memory_limit_mib}};
    root["dhash_similarity"] = {
        {"image_max_hamming_distance", options.dhash_similarity().image_max_hamming_distance},
        {"video_max_average_hamming_distance",
         options.dhash_similarity().video_max_average_hamming_distance},
        {"image_aspect_ratio_tolerance_percent",
         options.dhash_similarity().image_aspect_ratio_tolerance_percent},
        {"validation_worker_threads", options.dhash_similarity().validation_worker_threads}};
    root["rocksdb"] = {{"directory", WideToUtf8(options.rocksdb().directory.wstring())},
                       {"block_cache_mib", options.rocksdb().block_cache_mib},
                       {"write_buffer_mib", options.rocksdb().write_buffer_mib}};
    root["logging"] = {{"directory", WideToUtf8(options.logging().directory.wstring())},
                       {"execution_directory", WideToUtf8(options.logging().execution_directory.wstring())},
                       {"rotate_file_mib", options.logging().rotate_file_mib},
                       {"rotate_count", options.logging().rotate_count},
                       {"retention_days", options.logging().retention_days}};
    return root.dump();
}

std::optional<ScanOptions> ScanOptionsCodec::Deserialize(const std::string& json, std::string& error) {
    try {
        const Json root = Json::parse(json);
        const std::uint32_t snapshotVersion = root.at("snapshot_version").get<std::uint32_t>();
        if (snapshotVersion == 0 || snapshotVersion > kScanOptionsSnapshotVersion) {
            throw std::runtime_error("unsupported scan options snapshot version");
        }
        if (root.at("algorithm_version").get<std::string>() != "media-dhash-v2") {
            throw std::runtime_error("unsupported media algorithm version");
        }

        AppConfig config = AppConfig::CreateDefault({});
        config.paths.scan_roots.clear();
        for (const Json& path : root.at("scan_roots")) {
            config.paths.scan_roots.emplace_back(Utf8ToWide(path.get<std::string>()));
        }
        if (const auto storageIt = root.find("storage"); storageIt != root.end()) {
            const Json& storage = *storageIt;
            config.storage.max_concurrent_file_reads = storage.value(
                "max_concurrent_file_reads", config.storage.max_concurrent_file_reads);
            config.storage.hdd_read_threads_per_disk = storage.value(
                "hdd_read_threads_per_disk", config.storage.hdd_read_threads_per_disk);
            config.storage.ssd_read_threads_per_disk = storage.value(
                "ssd_read_threads_per_disk", config.storage.ssd_read_threads_per_disk);
            config.storage.adaptive_read_threads = storage.value(
                "adaptive_read_threads", config.storage.adaptive_read_threads);
            config.storage.disk_read_target_percent = storage.value(
                "disk_read_target_percent", config.storage.disk_read_target_percent);
        }
        const Json& compute = root.at("compute");
        config.compute.worker_threads = compute.at("worker_threads").get<std::uint32_t>();
        config.compute.ffmpeg_threads_per_task = compute.at("ffmpeg_threads_per_task").get<std::uint32_t>();
        config.compute.adaptive_worker_threads = compute.value(
            "adaptive_worker_threads", config.compute.adaptive_worker_threads);
        config.compute.cpu_target_percent = compute.value(
            "cpu_target_percent", config.compute.cpu_target_percent);
        if (const auto discoveryIt = root.find("discovery"); discoveryIt != root.end()) {
            const Json& discovery = *discoveryIt;
            const int method = discovery.value("method", static_cast<int>(config.discovery.method));
            if (method < static_cast<int>(DiscoveryMethod::Native) ||
                method > static_cast<int>(DiscoveryMethod::Everything)) {
                throw std::runtime_error("invalid snapshot discovery method");
            }
            config.discovery.method = static_cast<DiscoveryMethod>(method);
            config.discovery.everything_dll_path = Utf8ToWide(
                discovery.value("everything_dll_path", std::string{}));
            config.discovery.everything_exe_path = Utf8ToWide(
                discovery.value("everything_exe_path", std::string{}));
            config.discovery.query_page_size = discovery.value(
                "query_page_size", config.discovery.query_page_size);
            config.discovery.launch_timeout_seconds = discovery.value(
                "launch_timeout_seconds", config.discovery.launch_timeout_seconds);
            config.discovery.db_load_timeout_seconds = discovery.value(
                "db_load_timeout_seconds", config.discovery.db_load_timeout_seconds);
            config.discovery.poll_interval_milliseconds = discovery.value(
                "poll_interval_milliseconds", config.discovery.poll_interval_milliseconds);
        }
        const Json& io = root.at("io");
        config.io.read_block_kib = io.at("read_block_kib").get<std::uint32_t>();
        config.io.per_disk_queue_capacity = io.at("per_disk_queue_capacity").get<std::uint32_t>();
        config.io.hdd_extent_optimization = io.at("hdd_extent_optimization").get<bool>();
        config.io.hdd_sort_window = io.at("hdd_sort_window").get<std::uint32_t>();
        config.io.normal_block_retries = io.at("normal_block_retries").get<std::uint32_t>();
        config.io.small_block_retries = io.at("small_block_retries").get<std::uint32_t>();
        config.io.small_block_kib = io.at("small_block_kib").get<std::uint32_t>();
        config.io.no_progress_timeout_seconds = io.at("no_progress_timeout_seconds").get<std::uint32_t>();

        const Json& database = root.at("database");
        config.database.host = Utf8ToWide(database.at("host").get<std::string>());
        config.database.port = database.at("port").get<std::uint16_t>();
        config.database.database_name = Utf8ToWide(database.at("database_name").get<std::string>());
        config.database.user_name = Utf8ToWide(database.at("user_name").get<std::string>());
        const int tls_mode = database.at("tls_mode").get<int>();
        if (tls_mode < static_cast<int>(MySqlTlsMode::Disabled) ||
            tls_mode > static_cast<int>(MySqlTlsMode::VerifyIdentity)) {
            throw std::runtime_error("invalid snapshot TLS mode");
        }
        config.database.tls_mode = static_cast<MySqlTlsMode>(tls_mode);
        config.database.tls_ca_path = Utf8ToWide(database.at("tls_ca_path").get<std::string>());
        config.database.tls_certificate_path =
            Utf8ToWide(database.at("tls_certificate_path").get<std::string>());
        config.database.tls_private_key_path =
            Utf8ToWide(database.at("tls_private_key_path").get<std::string>());
        config.database.connection_pool_size = database.at("connection_pool_size").get<std::uint32_t>();
        config.database.connect_timeout_seconds = database.at("connect_timeout_seconds").get<std::uint32_t>();
        config.database.command_timeout_seconds = database.at("command_timeout_seconds").get<std::uint32_t>();
        config.database.retry_interval_seconds = database.at("retry_interval_seconds").get<std::uint32_t>();
        config.database.sync_batch_size = database.at("sync_batch_size").get<std::uint32_t>();

        const Json& thumbnails = root.at("thumbnails");
        config.thumbnails.root_directory = Utf8ToWide(thumbnails.at("root_directory").get<std::string>());
        const int format = thumbnails.at("format").get<int>();
        if (format < static_cast<int>(ThumbnailFormat::Jpeg) || format > static_cast<int>(ThumbnailFormat::Png)) {
            throw std::runtime_error("invalid snapshot thumbnail format");
        }
        config.thumbnails.format = static_cast<ThumbnailFormat>(format);
        config.thumbnails.video_cell_long_edge = thumbnails.at("video_cell_long_edge").get<std::uint32_t>();
        config.thumbnails.image_preview_long_edge = thumbnails.at("image_preview_long_edge").get<std::uint32_t>();
        config.thumbnails.cache_entries = thumbnails.at("cache_entries").get<std::uint32_t>();
        config.thumbnails.memory_limit_mib = thumbnails.at("memory_limit_mib").get<std::uint32_t>();
        config.thumbnails.gpu_memory_limit_mib = thumbnails.at("gpu_memory_limit_mib").get<std::uint32_t>();

        if (const auto dhashIt = root.find("dhash_similarity"); dhashIt != root.end() && dhashIt->is_object()) {
            config.dhash_similarity.image_max_hamming_distance =
                dhashIt->value("image_max_hamming_distance",
                               config.dhash_similarity.image_max_hamming_distance);
            config.dhash_similarity.video_max_average_hamming_distance =
                dhashIt->value("video_max_average_hamming_distance",
                               config.dhash_similarity.video_max_average_hamming_distance);
            config.dhash_similarity.image_aspect_ratio_tolerance_percent =
                dhashIt->value("image_aspect_ratio_tolerance_percent",
                               config.dhash_similarity.image_aspect_ratio_tolerance_percent);
            config.dhash_similarity.validation_worker_threads =
                dhashIt->value("validation_worker_threads",
                               config.dhash_similarity.validation_worker_threads);
        }

        const Json& rocksdb = root.at("rocksdb");
        config.rocksdb.directory = Utf8ToWide(rocksdb.at("directory").get<std::string>());
        config.rocksdb.block_cache_mib = rocksdb.at("block_cache_mib").get<std::uint32_t>();
        config.rocksdb.write_buffer_mib = rocksdb.at("write_buffer_mib").get<std::uint32_t>();
        const Json& logging = root.at("logging");
        config.logging.directory = Utf8ToWide(logging.at("directory").get<std::string>());
        config.logging.execution_directory = logging.contains("execution_directory")
                                                 ? std::filesystem::path(Utf8ToWide(
                                                       logging.at("execution_directory").get<std::string>()))
                                                 : config.logging.directory / L"execution";
        config.logging.rotate_file_mib = logging.at("rotate_file_mib").get<std::uint32_t>();
        config.logging.rotate_count = logging.at("rotate_count").get<std::uint32_t>();
        config.logging.retention_days = logging.at("retention_days").get<std::uint32_t>();
        error.clear();
        return ScanOptions::Freeze(config, root.value("generate_similar_report", false));
    } catch (const std::exception& exception) {
        error = exception.what();
        return std::nullopt;
    }
}

}  // namespace videosc::dedup
