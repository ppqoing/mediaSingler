#pragma once

#include "../config/AppConfig.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace videosc::dedup {

/**
 * @brief 一条应用级异常记录，负责描述模块、操作和可公开的诊断上下文。
 *
 * 调用方不得把密码、DPAPI 密文或完整配置内容写入 message/context。
 */
struct ApplicationErrorRecord {
    std::string severity = "error";
    std::string category = "exception";
    std::string module;
    std::string operation;
    std::string message;
    std::string exception_type;
    std::string context;
    std::int64_t native_error = 0;
};

/**
 * @brief 进程级、线程安全的 UTF-8 易读文本异常记录器。
 *
 * 该类型只处理可恢复异常；进程已损坏时必须使用 GUI 的 CrashHandler，
 * 不得从未处理异常过滤器调用本记录器。
 */
class ApplicationErrorLogger final {
public:
    /**
     * @brief 更新当前进程使用的日志目录和滚动规则。
     * @param config 日志目录、单文件大小和滚动数量。
     */
    static void Configure(const LoggingConfig& config) noexcept;

    /**
     * @brief 写入一条异常记录并返回可供 UI 展示的异常 ID。
     * @param record 不含敏感信息的异常上下文。
     * @return 成功或失败时都尽量返回稳定异常 ID；极端内存不足时可能为空。
     */
    static std::string Write(const ApplicationErrorRecord& record) noexcept;

    /**
     * @brief 获取当前配置的日志目录快照。
     * @return 未配置时返回空路径。
     */
    static std::filesystem::path Directory() noexcept;
};

}  // namespace videosc::dedup
