#pragma once

#include "../models/CoreModels.h"
#include "../persistence/MySqlClient.h"

#include <functional>
#include <optional>

namespace videosc::dedup {

/**
 * @brief 对已按 SHA-512、path_id 排序的成员执行单遍精确分组。
 *
 * 内存只随当前单组成员数增长，与数据库总记录数无关。
 */
class ExactGroupAccumulator final {
public:
    using GroupVisitor = std::function<bool(DuplicateGroup&& group)>;

    explicit ExactGroupAccumulator(GroupVisitor visitor);

    /** @brief 消费一个有序成员；摘要倒序会抛出异常以防静默生成错误报告。 */
    bool Consume(const Sha512Digest& sha512, DuplicateMember member);

    /** @brief 刷出最后一组；只输出至少两个成员的组。 */
    bool Finish();

    std::uint64_t consumed_members() const noexcept;
    std::uint64_t emitted_groups() const noexcept;

private:
    bool FlushCurrent();

    GroupVisitor visitor_;
    std::optional<Sha512Digest> current_sha512_;
    std::vector<DuplicateMember> current_members_;
    std::uint64_t consumed_members_ = 0;
    std::uint64_t emitted_groups_ = 0;
    bool stopped_ = false;
};

/** @brief 从 MySQL 流式读取所有当前有效映射并生成 SHA-512 精确重复组。 */
class ExactDuplicateReader final {
public:
    explicit ExactDuplicateReader(MySqlClient& client);

    /**
     * @brief 生成精确重复报告。
     * @param visitor 每个重复组调用一次；返回 false 提前停止。
     */
    MySqlStatus Stream(const ExactGroupAccumulator::GroupVisitor& visitor);

private:
    MySqlClient& client_;
};

}  // namespace videosc::dedup
