#pragma once

#include <filesystem>

/**
 * @brief 管理 GUI 宿主进程的致命异常记录、最小转储和外部报告进程。
 *
 * 致命处理器只负责尽量保存诊断信息并终止进程，不承担业务恢复或复杂资源清理。
 */
class CrashHandler final {
public:
    /**
     * @brief 安装未处理异常、terminate、纯虚函数和 CRT 非法参数处理器。
     * @param logging_directory 应用日志根目录，崩溃文件写入其 crash 子目录。
     * @return 目录准备和处理器安装完成返回 true。
     */
    static bool Install(const std::filesystem::path& logging_directory) noexcept;

    /**
     * @brief 配置加载后更新崩溃文件目录。
     * @param logging_directory 用户配置的日志根目录。
     */
    static void SetLogDirectory(const std::filesystem::path& logging_directory) noexcept;

    /**
     * @brief 启动独立 VideoScCrashReporter 以补充捕获 fail-fast。
     * @return 报告进程成功创建返回 true；调试器已附加或文件不存在时返回 false。
     */
    static bool LaunchExternalReporter() noexcept;

    /**
     * @brief 返回当前崩溃文件目录。
     * @return crash 目录绝对路径。
     */
    static std::filesystem::path CrashDirectory() noexcept;

    /**
     * @brief 返回最近一次尚未确认的崩溃元数据。
     * @return 没有未确认记录时返回空路径。
     */
    static std::filesystem::path LatestUnreviewedCrash() noexcept;

    /**
     * @brief 将当前最近一次崩溃标记为已查看。
     */
    static void MarkLatestCrashReviewed() noexcept;
};
