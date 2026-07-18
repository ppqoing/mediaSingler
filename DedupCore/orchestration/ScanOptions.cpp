#include "ScanOptions.h"

#include "../config/ConfigValidator.h"

#include <stdexcept>

namespace videosc::dedup {

ScanOptions ScanOptions::Freeze(const AppConfig& config, const bool generate_similar_report) {
    const std::vector<ValidationIssue> issues = ConfigValidator::Validate(config);
    if (ConfigValidator::HasErrors(issues)) {
        throw std::invalid_argument("Cannot freeze invalid scan configuration");
    }

    ScanOptions options;
    options.scan_roots_ = config.paths.scan_roots;
    options.storage_ = config.storage;
    options.compute_ = config.compute;
    options.io_ = config.io;
    options.discovery_ = config.discovery;
    options.database_.host = config.database.host;
    options.database_.port = config.database.port;
    options.database_.database_name = config.database.database_name;
    options.database_.user_name = config.database.user_name;
    options.database_.tls_mode = config.database.tls_mode;
    options.database_.tls_ca_path = config.database.tls_ca_path;
    options.database_.tls_certificate_path = config.database.tls_certificate_path;
    options.database_.tls_private_key_path = config.database.tls_private_key_path;
    options.database_.connection_pool_size = config.database.connection_pool_size;
    options.database_.connect_timeout_seconds = config.database.connect_timeout_seconds;
    options.database_.command_timeout_seconds = config.database.command_timeout_seconds;
    options.database_.retry_interval_seconds = config.database.retry_interval_seconds;
    options.database_.sync_batch_size = config.database.sync_batch_size;
    options.thumbnails_ = config.thumbnails;
    options.dhash_similarity_ = config.dhash_similarity;
    options.rocksdb_ = config.rocksdb;
    options.logging_ = config.logging;
    options.generate_similar_report_ = generate_similar_report;
    return options;
}

const std::vector<std::filesystem::path>& ScanOptions::scan_roots() const noexcept {
    return scan_roots_;
}

const StorageConfig& ScanOptions::storage() const noexcept {
    return storage_;
}

const ComputeConfig& ScanOptions::compute() const noexcept {
    return compute_;
}

const IoConfig& ScanOptions::io() const noexcept {
    return io_;
}

const DiscoveryConfig& ScanOptions::discovery() const noexcept {
    return discovery_;
}

const DatabaseTaskOptions& ScanOptions::database() const noexcept {
    return database_;
}

const ThumbnailConfig& ScanOptions::thumbnails() const noexcept {
    return thumbnails_;
}

const DHashSimilarityConfig& ScanOptions::dhash_similarity() const noexcept {
    return dhash_similarity_;
}

const RocksDbConfig& ScanOptions::rocksdb() const noexcept {
    return rocksdb_;
}

const LoggingConfig& ScanOptions::logging() const noexcept {
    return logging_;
}

const std::string& ScanOptions::algorithm_version() const noexcept {
    return algorithm_version_;
}

bool ScanOptions::generate_similar_report() const noexcept {
    return generate_similar_report_;
}

}  // namespace videosc::dedup
