#include "RocksStore.h"

#include <Windows.h>
#include <rocksdb/c.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwctype>
#include <iomanip>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>

namespace videosc::dedup {
namespace {

constexpr std::array<const char*, 10> kColumnFamilyNames = {
    "default",
    "scan_tasks",
    "file_paths",
    "sha_file_data",
    "sync_queue",
    "exact_index",
    "image_dhash_index",
    "video_dhash_index",
    "checkpoints",
    "tombstones",
};

/**
 * @brief 把枚举转换成固定句柄数组索引。
 * @param column_family Column Family 枚举。
 * @return 0 到 9 的索引。
 */
std::size_t ColumnIndex(const RocksColumnFamily column_family) {
    return static_cast<std::size_t>(column_family);
}

/**
 * @brief 消费 RocksDB C API 分配的错误字符串。
 * @param error C API 错误指针；为空表示成功。
 * @return 公共状态。
 */
RocksStatus ConsumeError(char* error) {
    if (error == nullptr) return {true, {}};
    RocksStatus result{false, error};
    rocksdb_free(error);
    return result;
}

/**
 * @brief 判断二进制键是否以指定前缀开始。
 * @param key 完整键。
 * @param prefix 前缀。
 * @return 匹配返回 true。
 */
bool StartsWith(const std::string_view key, const std::string_view prefix) {
    return key.size() >= prefix.size() && key.compare(0, prefix.size(), prefix) == 0;
}

/** @brief 生成同一数据库目录跨进程稳定的 Windows named mutex 名称。 */
std::wstring DatabaseMutexName(const std::filesystem::path& directory) {
    std::error_code error;
    std::wstring normalized = std::filesystem::absolute(directory, error).lexically_normal().wstring();
    if (error) normalized = directory.lexically_normal().wstring();
    std::uint64_t hash = 1469598103934665603ULL;
    for (wchar_t character : normalized) {
        const wchar_t lower = static_cast<wchar_t>(std::towlower(character));
        hash ^= static_cast<std::uint16_t>(lower);
        hash *= 1099511628211ULL;
    }
    std::wostringstream name;
    name << L"Local\\VideoSc-RocksDB-" << std::hex << std::setw(16)
         << std::setfill(L'0') << hash;
    return name.str();
}

/**
 * @brief 同线程可重入的共享生命周期锁。
 *
 * RocksDB 前缀访问器允许在回调中继续点查同一 Store；标准 shared_mutex 不保证共享锁递归，
 * 因此只在该线程的最外层操作真正加锁，避免关闭竞态同时保留并发读写能力。
 */
class ReentrantSharedLock final {
public:
    explicit ReentrantSharedLock(std::shared_mutex& mutex) : mutex_(&mutex) {
        std::uint32_t& depth = depths_[mutex_];
        if (depth == 0) {
            mutex_->lock_shared();
            owns_ = true;
        }
        ++depth;
    }

    ~ReentrantSharedLock() {
        const auto found = depths_.find(mutex_);
        if (found == depths_.end()) return;
        if (--found->second == 0) {
            depths_.erase(found);
            if (owns_) mutex_->unlock_shared();
        }
    }

    ReentrantSharedLock(const ReentrantSharedLock&) = delete;
    ReentrantSharedLock& operator=(const ReentrantSharedLock&) = delete;

private:
    std::shared_mutex* mutex_ = nullptr;
    bool owns_ = false;
    static thread_local std::unordered_map<std::shared_mutex*, std::uint32_t> depths_;
};

thread_local std::unordered_map<std::shared_mutex*, std::uint32_t>
    ReentrantSharedLock::depths_;

}  // namespace

/** @brief RocksStore 的稳定 RocksDB C ABI 句柄实现。 */
class RocksStore::Impl final {
public:
    explicit Impl(RocksDbConfig value) : config(std::move(value)) {}

    /** @brief 获取已打开 Column Family 句柄。 */
    rocksdb_column_family_handle_t* Handle(const RocksColumnFamily column_family) const {
        const std::size_t index = ColumnIndex(column_family);
        return index < handles.size() ? handles[index] : nullptr;
    }

    /** @brief 在持有 lifecycle 独占锁时按依赖逆序释放全部句柄和进程互斥锁。 */
    void CloseResources() noexcept {
        for (rocksdb_column_family_handle_t* handle : handles) {
            if (handle != nullptr) rocksdb_column_family_handle_destroy(handle);
        }
        handles.clear();
        if (database != nullptr) {
            rocksdb_close(database);
            database = nullptr;
        }
        for (rocksdb_options_t* options : column_options) {
            rocksdb_options_destroy(options);
        }
        column_options.clear();
        if (database_options != nullptr) {
            rocksdb_options_destroy(database_options);
            database_options = nullptr;
        }
        if (process_mutex != nullptr) {
            if (owns_process_mutex) ReleaseMutex(process_mutex);
            CloseHandle(process_mutex);
            process_mutex = nullptr;
            owns_process_mutex = false;
        }
    }

    RocksDbConfig config;
    rocksdb_t* database = nullptr;
    rocksdb_options_t* database_options = nullptr;
    std::vector<rocksdb_options_t*> column_options;
    std::vector<rocksdb_column_family_handle_t*> handles;
    mutable std::shared_mutex lifecycle;
    HANDLE process_mutex = nullptr;
    bool owns_process_mutex = false;
};

RocksStore::RocksStore(RocksDbConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}

RocksStore::~RocksStore() {
    Close();
}

RocksStatus RocksStore::Open() {
    std::unique_lock<std::shared_mutex> lifecycleLock(impl_->lifecycle);
    if (impl_->database != nullptr) return {true, {}};
    std::error_code directory_error;
    std::filesystem::create_directories(impl_->config.directory, directory_error);
    if (directory_error) return {false, directory_error.message()};

    const std::wstring mutexName = DatabaseMutexName(impl_->config.directory);
    impl_->process_mutex = CreateMutexW(nullptr, FALSE, mutexName.c_str());
    if (impl_->process_mutex == nullptr) return {false, "database_process_mutex_create_failed"};
    const DWORD wait = WaitForSingleObject(impl_->process_mutex, 0);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
        impl_->CloseResources();
        return {false, wait == WAIT_TIMEOUT ? "database_in_use_by_another_process"
                                            : "database_process_mutex_wait_failed"};
    }
    impl_->owns_process_mutex = true;

    impl_->database_options = rocksdb_options_create();
    rocksdb_options_set_create_if_missing(impl_->database_options, 1);
    rocksdb_options_set_create_missing_column_families(impl_->database_options, 1);
    rocksdb_options_set_max_background_jobs(
        impl_->database_options,
        (std::max)(2, static_cast<int>(std::thread::hardware_concurrency() / 2)));
    rocksdb_options_set_bytes_per_sync(impl_->database_options, 4ULL * 1024ULL * 1024ULL);
    rocksdb_options_set_wal_bytes_per_sync(impl_->database_options, 1ULL * 1024ULL * 1024ULL);
    rocksdb_options_set_db_write_buffer_size(
        impl_->database_options,
        static_cast<std::size_t>(impl_->config.write_buffer_mib) * 1024ULL * 1024ULL);
    rocksdb_options_set_compression(impl_->database_options, rocksdb_no_compression);

    const std::uint64_t total_write_buffer =
        static_cast<std::uint64_t>(impl_->config.write_buffer_mib) * 1024ULL * 1024ULL;
    const std::size_t per_family_write_buffer = static_cast<std::size_t>(
        (std::max<std::uint64_t>)(4ULL * 1024ULL * 1024ULL, total_write_buffer / kColumnFamilyNames.size()));
    impl_->column_options.reserve(kColumnFamilyNames.size());
    std::vector<const rocksdb_options_t*> option_pointers;
    option_pointers.reserve(kColumnFamilyNames.size());
    for (std::size_t index = 0; index < kColumnFamilyNames.size(); ++index) {
        rocksdb_options_t* options = rocksdb_options_create_copy(impl_->database_options);
        rocksdb_options_set_write_buffer_size(options, per_family_write_buffer);
        rocksdb_options_set_level_compaction_dynamic_level_bytes(options, 1);
        // 让 RocksDB 内部持有点查缓存，避免 C API 表选项与外部缓存的重复所有权。
        const std::uint64_t per_family_cache_mib =
            (std::max<std::uint64_t>)(1, impl_->config.block_cache_mib / kColumnFamilyNames.size());
        rocksdb_options_optimize_for_point_lookup(options, per_family_cache_mib);
        impl_->column_options.push_back(options);
        option_pointers.push_back(options);
    }

    impl_->handles.resize(kColumnFamilyNames.size(), nullptr);
    char* error = nullptr;
    const std::string database_path = impl_->config.directory.u8string();
    impl_->database = rocksdb_open_column_families(impl_->database_options,
                                                   database_path.c_str(),
                                                   static_cast<int>(kColumnFamilyNames.size()),
                                                   kColumnFamilyNames.data(),
                                                   option_pointers.data(),
                                                   impl_->handles.data(),
                                                   &error);
    RocksStatus status = ConsumeError(error);
    if (!status.succeeded || impl_->database == nullptr) {
        if (status.succeeded) status = {false, "rocksdb_open_returned_null"};
        impl_->CloseResources();
        return status;
    }
    return {true, {}};
}

void RocksStore::Close() noexcept {
    std::unique_lock<std::shared_mutex> lifecycleLock(impl_->lifecycle);
    impl_->CloseResources();
}

bool RocksStore::is_open() const noexcept {
    try {
        ReentrantSharedLock lifecycleLock(impl_->lifecycle);
        return impl_->database != nullptr;
    } catch (...) {
        return false;
    }
}

RocksStatus RocksStore::Put(const RocksColumnFamily column_family,
                            const std::string_view key,
                            const std::string_view value,
                            const bool sync) {
    ReentrantSharedLock lifecycleLock(impl_->lifecycle);
    rocksdb_column_family_handle_t* handle = impl_->Handle(column_family);
    if (impl_->database == nullptr || handle == nullptr) return {false, "database_not_open"};
    rocksdb_writeoptions_t* options = rocksdb_writeoptions_create();
    rocksdb_writeoptions_set_sync(options, sync ? 1 : 0);
    char* error = nullptr;
    rocksdb_put_cf(impl_->database,
                   options,
                   handle,
                   key.data(),
                   key.size(),
                   value.data(),
                   value.size(),
                   &error);
    rocksdb_writeoptions_destroy(options);
    return ConsumeError(error);
}

RocksStatus RocksStore::Get(const RocksColumnFamily column_family,
                            const std::string_view key,
                            std::string& value) const {
    ReentrantSharedLock lifecycleLock(impl_->lifecycle);
    rocksdb_column_family_handle_t* handle = impl_->Handle(column_family);
    if (impl_->database == nullptr || handle == nullptr) return {false, "database_not_open"};
    rocksdb_readoptions_t* options = rocksdb_readoptions_create();
    std::size_t value_length = 0;
    char* error = nullptr;
    char* result = rocksdb_get_cf(
        impl_->database, options, handle, key.data(), key.size(), &value_length, &error);
    rocksdb_readoptions_destroy(options);
    RocksStatus status = ConsumeError(error);
    if (!status.succeeded) {
        if (result != nullptr) rocksdb_free(result);
        return status;
    }
    if (result == nullptr) return {false, "not_found"};
    value.assign(result, value_length);
    rocksdb_free(result);
    return {true, {}};
}

RocksStatus RocksStore::Delete(const RocksColumnFamily column_family,
                               const std::string_view key,
                               const bool sync) {
    ReentrantSharedLock lifecycleLock(impl_->lifecycle);
    rocksdb_column_family_handle_t* handle = impl_->Handle(column_family);
    if (impl_->database == nullptr || handle == nullptr) return {false, "database_not_open"};
    rocksdb_writeoptions_t* options = rocksdb_writeoptions_create();
    rocksdb_writeoptions_set_sync(options, sync ? 1 : 0);
    char* error = nullptr;
    rocksdb_delete_cf(impl_->database, options, handle, key.data(), key.size(), &error);
    rocksdb_writeoptions_destroy(options);
    return ConsumeError(error);
}

RocksStatus RocksStore::WriteBatch(const std::vector<RocksMutation>& mutations, const bool sync) {
    ReentrantSharedLock lifecycleLock(impl_->lifecycle);
    if (impl_->database == nullptr) return {false, "database_not_open"};
    rocksdb_writebatch_t* batch = rocksdb_writebatch_create();
    for (const RocksMutation& mutation : mutations) {
        rocksdb_column_family_handle_t* handle = impl_->Handle(mutation.column_family);
        if (handle == nullptr) {
            rocksdb_writebatch_destroy(batch);
            return {false, "invalid_column_family"};
        }
        if (mutation.value.has_value()) {
            rocksdb_writebatch_put_cf(batch,
                                      handle,
                                      mutation.key.data(),
                                      mutation.key.size(),
                                      mutation.value->data(),
                                      mutation.value->size());
        } else {
            rocksdb_writebatch_delete_cf(batch, handle, mutation.key.data(), mutation.key.size());
        }
    }
    rocksdb_writeoptions_t* options = rocksdb_writeoptions_create();
    rocksdb_writeoptions_set_sync(options, sync ? 1 : 0);
    char* error = nullptr;
    rocksdb_write(impl_->database, options, batch, &error);
    rocksdb_writeoptions_destroy(options);
    rocksdb_writebatch_destroy(batch);
    return ConsumeError(error);
}

RocksStatus RocksStore::ForEachPrefix(const RocksColumnFamily column_family,
                                      const std::string_view prefix,
                                      const std::size_t maximum_items,
                                      const PrefixVisitor& visitor) const {
    ReentrantSharedLock lifecycleLock(impl_->lifecycle);
    rocksdb_column_family_handle_t* handle = impl_->Handle(column_family);
    if (impl_->database == nullptr || handle == nullptr) return {false, "database_not_open"};
    rocksdb_readoptions_t* options = rocksdb_readoptions_create();
    rocksdb_readoptions_set_total_order_seek(options, 1);
    rocksdb_iterator_t* iterator = rocksdb_create_iterator_cf(impl_->database, options, handle);
    rocksdb_iter_seek(iterator, prefix.data(), prefix.size());
    std::size_t visited = 0;
    while (rocksdb_iter_valid(iterator)) {
        std::size_t key_length = 0;
        std::size_t value_length = 0;
        const char* key_data = rocksdb_iter_key(iterator, &key_length);
        const char* value_data = rocksdb_iter_value(iterator, &value_length);
        const std::string_view key(key_data, key_length);
        if (!StartsWith(key, prefix) || (maximum_items != 0 && visited >= maximum_items)) break;
        ++visited;
        if (visitor && !visitor(key, std::string_view(value_data, value_length))) break;
        rocksdb_iter_next(iterator);
    }
    char* error = nullptr;
    rocksdb_iter_get_error(iterator, &error);
    rocksdb_iter_destroy(iterator);
    rocksdb_readoptions_destroy(options);
    return ConsumeError(error);
}

RocksStatus RocksStore::DeletePrefix(const RocksColumnFamily column_family,
                                     const std::string_view prefix,
                                     const std::size_t batch_size,
                                     const bool sync) {
    if (batch_size == 0) return {false, "delete_prefix_batch_size_required"};
    while (true) {
        std::vector<std::string> keys;
        keys.reserve(batch_size);
        const RocksStatus listed = ForEachPrefix(
            column_family,
            prefix,
            batch_size,
            [&](const std::string_view key, const std::string_view) {
                keys.emplace_back(key);
                return true;
            });
        if (!listed.succeeded) return listed;
        if (keys.empty()) return {true, {}};
        std::vector<RocksMutation> deletions;
        deletions.reserve(keys.size());
        for (std::string& key : keys) {
            deletions.push_back({column_family, std::move(key), std::nullopt});
        }
        const RocksStatus deleted = WriteBatch(deletions, sync);
        if (!deleted.succeeded) return deleted;
    }
}

RocksStatus RocksStore::ClearAll(const std::size_t batch_size, const bool sync) {
    if (batch_size == 0) return {false, "batch_size_must_be_positive"};
    constexpr std::array<RocksColumnFamily, 10> families = {
        RocksColumnFamily::Default,
        RocksColumnFamily::ScanTasks,
        RocksColumnFamily::FilePaths,
        RocksColumnFamily::ShaFileData,
        RocksColumnFamily::SyncQueue,
        RocksColumnFamily::ExactIndex,
        RocksColumnFamily::ImageDhashIndex,
        RocksColumnFamily::VideoDhashIndex,
        RocksColumnFamily::Checkpoints,
        RocksColumnFamily::Tombstones,
    };
    for (const RocksColumnFamily family : families) {
        const RocksStatus cleared = DeletePrefix(family, {}, batch_size, sync);
        if (!cleared.succeeded) return cleared;
    }
    return {true, {}};
}

const std::filesystem::path& RocksStore::directory() const noexcept {
    return impl_->config.directory;
}

}  // namespace videosc::dedup
