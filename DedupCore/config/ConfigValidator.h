#pragma once

#include "AppConfig.h"

#include <string>
#include <vector>

namespace videosc::dedup {

/** @brief 配置问题级别；错误阻止保存，警告仅用于 GUI 提示。 */
enum class ValidationSeverity {
    Warning,
    Error,
};

/** @brief 单个可定位到 JSON 字段的配置校验问题。 */
struct ValidationIssue {
    std::string field;
    std::wstring message;
    ValidationSeverity severity = ValidationSeverity::Error;
};

/** @brief 集中维护线程、队列、超时、缓存和路径的保存前校验规则。 */
class ConfigValidator final {
public:
    /**
     * @brief 校验完整配置，并估算并发缓冲是否超过用户配置的内存预算。
     * @param config 待保存或创建扫描任务的配置。
     * @return 所有错误和警告；调用方只需阻止存在 Error 的配置。
     */
    static std::vector<ValidationIssue> Validate(const AppConfig& config);

    /** @brief 判断校验结果中是否包含阻止保存的错误。 */
    static bool HasErrors(const std::vector<ValidationIssue>& issues);
};

}  // namespace videosc::dedup
