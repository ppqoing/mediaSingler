#include "MySqlSchema.h"

#include <charconv>
#include <limits>
#include <map>
#include <sstream>

namespace videosc::dedup {
namespace {

/** @brief 将无符号十进制版本号解析为 uint32。 */
bool ParseVersion(const std::string& value, std::uint32_t& version) {
    std::uint64_t parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
        parsed > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    version = static_cast<std::uint32_t>(parsed);
    return true;
}

/** @brief 构造模式层错误，消息不包含 SQL。 */
MySqlStatus SchemaFailure(const std::string& message) {
    return {false, 0, message};
}

}  // namespace

const std::vector<std::string>& MySqlSchema::InitializationStatements() {
    static const std::vector<std::string> statements = {
        "CREATE TABLE IF NOT EXISTS videosc_schema_version ("
        "schema_version INT UNSIGNED NOT NULL PRIMARY KEY,"
        "applied_at_utc TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci",

        "CREATE TABLE IF NOT EXISTS videosc_data_version ("
        "singleton_id TINYINT UNSIGNED NOT NULL PRIMARY KEY,"
        "data_version INT UNSIGNED NOT NULL,"
        "generation_id BIGINT UNSIGNED NOT NULL,"
        "data_state TINYINT UNSIGNED NOT NULL,"
        "updated_at_utc TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci",

        "CREATE TABLE IF NOT EXISTS sha512_file_data ("
        "sha512 BINARY(64) NOT NULL PRIMARY KEY,"
        "content_size_bytes BIGINT UNSIGNED NOT NULL,"
        "media_kind TINYINT UNSIGNED NOT NULL,"
        "mime_type VARCHAR(255) NOT NULL DEFAULT '',"
        "container_name VARCHAR(255) NOT NULL DEFAULT '',"
        "width INT UNSIGNED NOT NULL DEFAULT 0,"
        "height INT UNSIGNED NOT NULL DEFAULT 0,"
        "image_dhash BIGINT UNSIGNED NULL,"
        "image_pdq_hash BINARY(32) NULL,"
        "image_pdq_quality TINYINT UNSIGNED NULL,"
        "image_zoned_phashes BINARY(128) NULL,"
        "image_perceptual_algorithm_version INT UNSIGNED NOT NULL DEFAULT 0,"
        "image_structural_algorithm_version INT UNSIGNED NOT NULL DEFAULT 0,"
        "video_duration_ms BIGINT NOT NULL DEFAULT 0,"
        "video_frame_rate DOUBLE NOT NULL DEFAULT 0,"
        "video_bitrate BIGINT UNSIGNED NOT NULL DEFAULT 0,"
        "video_codec VARCHAR(128) NOT NULL DEFAULT '',"
        "pixel_format VARCHAR(128) NOT NULL DEFAULT '',"
        "video_dhashes BINARY(48) NULL,"
        "has_video_dhashes TINYINT(1) NOT NULL DEFAULT 0,"
        "static_visual TINYINT(1) NOT NULL DEFAULT 0,"
        "contact_sheet_path LONGTEXT NOT NULL,"
        "media_algorithm_version VARCHAR(128) NOT NULL DEFAULT '',"
        "created_at_utc TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),"
        "updated_at_utc TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),"
        "KEY ix_media_kind_sha (media_kind, sha512),"
        "KEY ix_video_duration_sha (media_kind, static_visual, video_duration_ms, sha512)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci",

        "CREATE TABLE IF NOT EXISTS file_path_sha512 ("
        "path_id BIGINT UNSIGNED NOT NULL PRIMARY KEY,"
        "path_hash BINARY(32) NOT NULL,"
        "full_path LONGTEXT NOT NULL,"
        "normalized_path LONGTEXT NOT NULL,"
        "sha512 BINARY(64) NOT NULL,"
        "scan_id BIGINT UNSIGNED NOT NULL,"
        "volume_serial BIGINT UNSIGNED NOT NULL DEFAULT 0,"
        "file_id_high BIGINT UNSIGNED NOT NULL DEFAULT 0,"
        "file_id_low BIGINT UNSIGNED NOT NULL DEFAULT 0,"
        "volume_guid VARCHAR(128) NOT NULL DEFAULT '',"
        "storage_target_key VARCHAR(256) NOT NULL DEFAULT '',"
        "size_bytes BIGINT UNSIGNED NOT NULL,"
        "extension VARCHAR(64) NOT NULL DEFAULT '',"
        "creation_time_utc_ms BIGINT NOT NULL DEFAULT 0,"
        "last_write_time_utc_ms BIGINT NOT NULL DEFAULT 0,"
        "scan_root_priority INT UNSIGNED NOT NULL DEFAULT 0,"
        "path_state TINYINT UNSIGNED NOT NULL,"
        "active TINYINT(1) NOT NULL,"
        "updated_at_utc TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),"
        "KEY ix_path_hash (path_hash),"
        "KEY ix_exact_active_sha (active, sha512, path_id),"
        "KEY ix_scan_id_path (scan_id, path_id),"
        "CONSTRAINT fk_path_content FOREIGN KEY (sha512) REFERENCES sha512_file_data(sha512)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci",

        "CREATE TABLE IF NOT EXISTS duplicate_group ("
        "group_id BIGINT UNSIGNED NOT NULL PRIMARY KEY,"
        "group_kind TINYINT UNSIGNED NOT NULL,"
        "group_key BINARY(64) NULL,"
        "algorithm_version VARCHAR(128) NOT NULL DEFAULT '',"
        "member_count INT UNSIGNED NOT NULL,"
        "generated_at_utc TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),"
        "KEY ix_group_kind_generated (group_kind, generated_at_utc, group_id)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci",

        "CREATE TABLE IF NOT EXISTS duplicate_group_member ("
        "group_id BIGINT UNSIGNED NOT NULL,"
        "path_id BIGINT UNSIGNED NOT NULL,"
        "content_sha512 BINARY(64) NOT NULL,"
        "PRIMARY KEY (group_id, path_id),"
        "KEY ix_member_path (path_id, group_id)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci",
    };
    return statements;
}

const std::vector<std::string>& MySqlSchema::ResetStatements() {
    static const std::vector<std::string> statements = {
        "DROP TABLE IF EXISTS duplicate_group_member",
        "DROP TABLE IF EXISTS duplicate_group",
        "DROP TABLE IF EXISTS file_path_sha512",
        "DROP TABLE IF EXISTS sha512_file_data",
        "DROP TABLE IF EXISTS videosc_data_version",
    };
    return statements;
}

MySqlStatus MySqlSchema::EnsureMetadataTables(MySqlClient& client) {
    const auto& statements = InitializationStatements();
    MySqlStatus status = client.Execute(statements[0]);
    if (!status.succeeded) return status;
    return client.Execute(statements[1]);
}

MySqlStatus MySqlSchema::ReadCurrentVersion(MySqlClient& client, std::uint32_t& version) {
    version = 0;
    bool tableExists = false;
    const MySqlStatus inspected = client.Query(
        "SELECT 1 FROM information_schema.TABLES WHERE TABLE_SCHEMA=DATABASE() "
        "AND TABLE_NAME='videosc_schema_version' LIMIT 1",
        [&](const MySqlRow&) {
            tableExists = true;
            return false;
        });
    if (!inspected.succeeded || !tableExists) return inspected;

    bool receivedVersion = false;
    const MySqlStatus status = client.Query(
        "SELECT COALESCE(MAX(schema_version), 0) FROM videosc_schema_version",
        [&](const MySqlRow& row) {
            if (row.size() != 1 || !row.front().has_value() || !ParseVersion(*row.front(), version)) {
                return false;
            }
            receivedVersion = true;
            return false;
        });
    if (!status.succeeded) return status;
    return receivedVersion ? MySqlStatus{true, 0, {}}
                           : SchemaFailure("Cannot read VideoSc MySQL schema version");
}

MySqlSchemaResult MySqlSchema::Initialize(MySqlClient& client) {
    MySqlSchemaResult result;
    const auto& statements = InitializationStatements();
    result.status = EnsureMetadataTables(client);
    if (!result.status.succeeded) return result;

    std::uint32_t version = 0;
    result.status = ReadCurrentVersion(client, version);
    if (!result.status.succeeded) return result;
    result.previous_version = version;
    if (version > kCurrentMySqlSchemaVersion) {
        result.status = SchemaFailure("MySQL schema is newer than this application");
        return result;
    }
    if (version != 0 && version < 1) {
        result.status = SchemaFailure("Unsupported legacy MySQL schema version");
        return result;
    }

    for (std::size_t index = 2; index < statements.size(); ++index) {
        result.status = client.Execute(statements[index]);
        if (!result.status.succeeded) return result;
    }
    if (version == 1) {
        result.status = client.Execute(
            "ALTER TABLE sha512_file_data "
            "ADD COLUMN IF NOT EXISTS image_pdq_hash BINARY(32) NULL AFTER image_dhash,"
            "ADD COLUMN IF NOT EXISTS image_pdq_quality TINYINT UNSIGNED NULL AFTER image_pdq_hash,"
            "ADD COLUMN IF NOT EXISTS image_zoned_phashes BINARY(128) NULL AFTER image_pdq_quality,"
            "ADD COLUMN IF NOT EXISTS image_perceptual_algorithm_version INT UNSIGNED NOT NULL DEFAULT 0 "
            "AFTER image_zoned_phashes,"
            "ADD COLUMN IF NOT EXISTS image_structural_algorithm_version INT UNSIGNED NOT NULL DEFAULT 0 "
            "AFTER image_perceptual_algorithm_version");
        if (!result.status.succeeded) return result;
        result.migrated_schema = true;
    }
    result.status = ValidateCurrentSchema(client);
    if (!result.status.succeeded) return result;
    result.status = client.Execute(
        "INSERT IGNORE INTO videosc_schema_version(schema_version) VALUES (3)");
    if (!result.status.succeeded) return result;
    result.created_initial_schema = version == 0;
    result.migrated_schema = result.migrated_schema || (version != 0 && version < kCurrentMySqlSchemaVersion);
    result.current_version = kCurrentMySqlSchemaVersion;
    return result;
}

MySqlStatus MySqlSchema::ResetBusinessTables(MySqlClient& client) {
    for (const std::string& statement : ResetStatements()) {
        const MySqlStatus status = client.Execute(statement);
        if (!status.succeeded) return status;
    }
    return {true, 0, {}};
}

MySqlStatus MySqlSchema::ValidateCurrentSchema(MySqlClient& client) {
    struct ColumnContract {
        std::string data_type;
        bool nullable = false;
        bool unsigned_numeric = false;
    };
    static const std::map<std::string, ColumnContract> expected = {
        {"videosc_schema_version.schema_version", {"int", false, true}},
        {"videosc_schema_version.applied_at_utc", {"timestamp", false, false}},
        {"videosc_data_version.singleton_id", {"tinyint", false, true}},
        {"videosc_data_version.data_version", {"int", false, true}},
        {"videosc_data_version.generation_id", {"bigint", false, true}},
        {"videosc_data_version.data_state", {"tinyint", false, true}},
        {"videosc_data_version.updated_at_utc", {"timestamp", false, false}},
        {"sha512_file_data.sha512", {"binary", false, false}},
        {"sha512_file_data.content_size_bytes", {"bigint", false, true}},
        {"sha512_file_data.media_kind", {"tinyint", false, true}},
        {"sha512_file_data.mime_type", {"varchar", false, false}},
        {"sha512_file_data.container_name", {"varchar", false, false}},
        {"sha512_file_data.width", {"int", false, true}},
        {"sha512_file_data.height", {"int", false, true}},
        {"sha512_file_data.image_dhash", {"bigint", true, true}},
        {"sha512_file_data.image_pdq_hash", {"binary", true, false}},
        {"sha512_file_data.image_pdq_quality", {"tinyint", true, true}},
        {"sha512_file_data.image_zoned_phashes", {"binary", true, false}},
        {"sha512_file_data.image_perceptual_algorithm_version", {"int", false, true}},
        {"sha512_file_data.image_structural_algorithm_version", {"int", false, true}},
        {"sha512_file_data.video_duration_ms", {"bigint", false, false}},
        {"sha512_file_data.video_frame_rate", {"double", false, false}},
        {"sha512_file_data.video_bitrate", {"bigint", false, true}},
        {"sha512_file_data.video_codec", {"varchar", false, false}},
        {"sha512_file_data.pixel_format", {"varchar", false, false}},
        {"sha512_file_data.video_dhashes", {"binary", true, false}},
        {"sha512_file_data.has_video_dhashes", {"tinyint", false, false}},
        {"sha512_file_data.static_visual", {"tinyint", false, false}},
        {"sha512_file_data.contact_sheet_path", {"longtext", false, false}},
        {"sha512_file_data.media_algorithm_version", {"varchar", false, false}},
        {"sha512_file_data.created_at_utc", {"timestamp", false, false}},
        {"sha512_file_data.updated_at_utc", {"timestamp", false, false}},
        {"file_path_sha512.path_id", {"bigint", false, true}},
        {"file_path_sha512.path_hash", {"binary", false, false}},
        {"file_path_sha512.full_path", {"longtext", false, false}},
        {"file_path_sha512.normalized_path", {"longtext", false, false}},
        {"file_path_sha512.sha512", {"binary", false, false}},
        {"file_path_sha512.scan_id", {"bigint", false, true}},
        {"file_path_sha512.volume_serial", {"bigint", false, true}},
        {"file_path_sha512.file_id_high", {"bigint", false, true}},
        {"file_path_sha512.file_id_low", {"bigint", false, true}},
        {"file_path_sha512.volume_guid", {"varchar", false, false}},
        {"file_path_sha512.storage_target_key", {"varchar", false, false}},
        {"file_path_sha512.size_bytes", {"bigint", false, true}},
        {"file_path_sha512.extension", {"varchar", false, false}},
        {"file_path_sha512.creation_time_utc_ms", {"bigint", false, false}},
        {"file_path_sha512.last_write_time_utc_ms", {"bigint", false, false}},
        {"file_path_sha512.scan_root_priority", {"int", false, true}},
        {"file_path_sha512.path_state", {"tinyint", false, true}},
        {"file_path_sha512.active", {"tinyint", false, false}},
        {"file_path_sha512.updated_at_utc", {"timestamp", false, false}},
        {"duplicate_group.group_id", {"bigint", false, true}},
        {"duplicate_group.group_kind", {"tinyint", false, true}},
        {"duplicate_group.group_key", {"binary", true, false}},
        {"duplicate_group.algorithm_version", {"varchar", false, false}},
        {"duplicate_group.member_count", {"int", false, true}},
        {"duplicate_group.generated_at_utc", {"timestamp", false, false}},
        {"duplicate_group_member.group_id", {"bigint", false, true}},
        {"duplicate_group_member.path_id", {"bigint", false, true}},
        {"duplicate_group_member.content_sha512", {"binary", false, false}},
    };
    struct ObservedColumn {
        std::string data_type;
        bool nullable = false;
        bool unsigned_numeric = false;
    };
    std::map<std::string, ObservedColumn> actual;
    const MySqlStatus columns = client.Query(
        "SELECT TABLE_NAME,COLUMN_NAME,DATA_TYPE,IS_NULLABLE,COLUMN_TYPE "
        "FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() "
        "AND TABLE_NAME IN ('videosc_schema_version','videosc_data_version','sha512_file_data',"
        "'file_path_sha512','duplicate_group','duplicate_group_member')",
        [&](const MySqlRow& row) {
            if (row.size() == 5 && row[0].has_value() && row[1].has_value() &&
                row[2].has_value() && row[3].has_value() && row[4].has_value()) {
                actual[*row[0] + '.' + *row[1]] = {
                    *row[2],
                    *row[3] == "YES",
                    row[4]->find("unsigned") != std::string::npos,
                };
            }
            return true;
        });
    if (!columns.succeeded) return columns;

    std::vector<std::string> missing;
    std::vector<std::string> incompatible;
    for (const auto& [name, contract] : expected) {
        const auto found = actual.find(name);
        if (found == actual.end()) {
            missing.push_back(name);
            continue;
        }
        if (found->second.data_type != contract.data_type ||
            found->second.nullable != contract.nullable ||
            found->second.unsigned_numeric != contract.unsigned_numeric) {
            incompatible.push_back(name);
        }
    }
    if (!missing.empty()) {
        std::ostringstream message;
        message << "mysql_schema_contract_missing:";
        for (std::size_t index = 0; index < missing.size(); ++index) {
            if (index != 0) message << ',';
            message << missing[index];
        }
        return SchemaFailure(message.str());
    }
    if (!incompatible.empty()) {
        std::ostringstream message;
        message << "mysql_schema_contract_incompatible:";
        for (std::size_t index = 0; index < incompatible.size(); ++index) {
            if (index != 0) message << ',';
            message << incompatible[index];
        }
        return SchemaFailure(message.str());
    }

    bool hasExactIndex = false;
    const MySqlStatus index = client.Query(
        "SELECT 1 FROM information_schema.STATISTICS WHERE TABLE_SCHEMA=DATABASE() "
        "AND TABLE_NAME='file_path_sha512' AND INDEX_NAME='ix_exact_active_sha' LIMIT 1",
        [&](const MySqlRow&) {
            hasExactIndex = true;
            return false;
        });
    if (!index.succeeded) return index;
    return hasExactIndex ? MySqlStatus{true, 0, {}}
                         : SchemaFailure("mysql_schema_contract_missing_index:ix_exact_active_sha");
}

}  // namespace videosc::dedup
