#pragma once

#include "AppConfig.h"

#include <filesystem>
#include <string>
#include <vector>

namespace videosc::dedup {

/** @brief 本次配置内容的来源。 */
enum class ConfigSource {
    BuiltInDefaults,
    PrimaryFile,
    BackupFile,
};

/** @brief 配置加载状态，GUI 据此显示缺失、恢复或拒绝覆盖提示。 */
enum class ConfigLoadStatus {
    Loaded,
    MissingUsingDefaults,
    RecoveredFromBackup,
    InvalidUsingDefaults,
    UnsupportedVersion,
};

/** @brief 配置加载结果；DPAPI 失败属于可恢复告警，不阻止其他设置加载。 */
struct ConfigLoadResult {
    AppConfig config;
    ConfigSource source = ConfigSource::BuiltInDefaults;
    ConfigLoadStatus status = ConfigLoadStatus::MissingUsingDefaults;
    bool can_save = true;
    std::vector<std::wstring> warnings;
};

/** @brief 原子保存结果，保留 Win32 错误码以便 GUI 给出可定位提示。 */
struct ConfigSaveResult {
    bool succeeded = false;
    std::filesystem::path path;
    std::wstring error_message;
    unsigned long system_error = 0;
};

/**
 * @brief 负责安装目录 config.json 的 UTF-8 读取、模式检查、DPAPI 和原子替换。
 *
 * 该类型不使用注册表或用户目录回退。调用方可传入测试路径，生产环境使用
 * ForApplicationDirectory() 固定到可执行文件目录。
 */
class JsonConfigStore final {
public:
    /** @brief 构造指定主配置路径的存储。 */
    explicit JsonConfigStore(std::filesystem::path config_path);

    /** @brief 创建固定使用“安装目录/config.json”的生产存储。 */
    static JsonConfigStore ForApplicationDirectory();

    /** @brief 获取主配置路径。 */
    const std::filesystem::path& config_path() const noexcept;

    /** @brief 获取上一有效配置备份路径 config.json.bak。 */
    std::filesystem::path backup_path() const;

    /**
     * @brief 加载主配置；损坏时尝试备份，缺失或均损坏时返回未保存默认值。
     * @return 带来源、可保存状态和中文告警的加载结果。
     */
    ConfigLoadResult Load() const;

    /**
     * @brief 校验并原子保存配置。
     *
     * 先写同目录临时文件并 FlushFileBuffers，再备份当前主文件并用 ReplaceFileW 原子替换。
     * 任一步失败都不改存其他目录，且尽可能保持原主文件可用。
     */
    ConfigSaveResult Save(const AppConfig& config) const;

private:
    std::filesystem::path config_path_;
};

}  // namespace videosc::dedup
