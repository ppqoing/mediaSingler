#pragma once

#include "../config/AppConfig.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace videosc::dedup {

/** @brief 去重工具固定使用的 RocksDB Column Family。 */
enum class RocksColumnFamily {
    Default,
    ScanTasks,
    FilePaths,
    ShaFileData,
    SyncQueue,
    ExactIndex,
    ImageDhashIndex,
    VideoDhashIndex,
    Checkpoints,
    Tombstones,
};

/** @brief RocksDB 操作结果，保留原生状态文本用于日志。 */
struct RocksStatus {
    bool succeeded = false;
    std::string message;
};

/** @brief 一个 WriteBatch put/delete 变更。 */
struct RocksMutation {
    RocksColumnFamily column_family = RocksColumnFamily::Default;
    std::string key;
    std::optional<std::string> value;
};

/**
 * @brief 多线程安全的本地持久化 RocksDB 封装。
 *
 * 热路径先写本存储再异步同步 MySQL。打开与关闭必须在没有并发读写时执行；打开后的 Get/Put/
 * WriteBatch/ForEachPrefix 可由多个调度线程调用。所有 Column Family 在首次打开时幂等创建。
 */
class RocksStore final {
public:
    using PrefixVisitor = std::function<bool(std::string_view key, std::string_view value)>;

    /**
     * @brief 保存目录和总内存预算，不立即打开数据库。
     * @param config RocksDB 配置快照。
     */
    explicit RocksStore(RocksDbConfig config);
    ~RocksStore();

    RocksStore(const RocksStore&) = delete;
    RocksStore& operator=(const RocksStore&) = delete;

    /** @brief 创建目录并打开全部 Column Family。 */
    RocksStatus Open();

    /** @brief 销毁 Column Family 句柄并关闭数据库；可重复调用。 */
    void Close() noexcept;

    /** @return 数据库当前是否已打开。 */
    bool is_open() const noexcept;

    /**
     * @brief 写入一个键值。
     * @param column_family 目标 Column Family。
     * @param key 二进制安全键。
     * @param value 二进制安全值。
     * @param sync 是否要求 WAL 同步落盘。
     * @return RocksDB 状态。
     */
    RocksStatus Put(RocksColumnFamily column_family,
                    std::string_view key,
                    std::string_view value,
                    bool sync);

    /**
     * @brief 读取一个键。
     * @param column_family 目标 Column Family。
     * @param key 二进制安全键。
     * @param value 成功命中时写入。
     * @return 成功命中为 succeeded=true；不存在时 succeeded=false 且 message="not_found"。
     */
    RocksStatus Get(RocksColumnFamily column_family, std::string_view key, std::string& value) const;

    /** @brief 删除一个键；不存在也视为幂等成功。 */
    RocksStatus Delete(RocksColumnFamily column_family, std::string_view key, bool sync);

    /**
     * @brief 原子提交跨 Column Family 变更。
     * @param mutations value 有值表示 Put，空表示 Delete。
     * @param sync 是否同步 WAL。
     * @return 整批状态。
     */
    RocksStatus WriteBatch(const std::vector<RocksMutation>& mutations, bool sync);

    /**
     * @brief 按键前缀有界顺序迭代。
     * @param column_family 目标 Column Family。
     * @param prefix 二进制前缀。
     * @param maximum_items 最大回调条数，零表示不限制。
     * @param visitor 返回 false 可提前终止。
     * @return 迭代器状态。
     */
    RocksStatus ForEachPrefix(RocksColumnFamily column_family,
                              std::string_view prefix,
                              std::size_t maximum_items,
                              const PrefixVisitor& visitor) const;

    /**
     * @brief 以固定批量删除前缀下的全部键，避免报告代际清理占用全量内存。
     * @param batch_size 每批最多删除的键数，必须大于零。
     */
    RocksStatus DeletePrefix(RocksColumnFamily column_family,
                             std::string_view prefix,
                             std::size_t batch_size,
                             bool sync);

    /**
     * @brief 清空 VideoSc 固定使用的全部 Column Family，但保留数据库目录与结构。
     * @param batch_size 单批删除键数，必须大于零。
     * @param sync 是否要求每批 WAL 同步落盘。
     * @return 任一 Column Family 清理失败即返回失败。
     */
    RocksStatus ClearAll(std::size_t batch_size, bool sync);

    /** @return 当前数据库目录。 */
    const std::filesystem::path& directory() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace videosc::dedup
