# 永久删除可信判定重构实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 删除可信判定改为只看报告证据质量（schema v4 + 三筛），部分作用域 v4 报告允许永久删除；跳过内容持久化并在 GUI 可见可修复；旧规则报告保留按次确认的强制删除通道。

**Architecture:** 判定函数收窄 → 部分作用域 v4 报告自动放行（GUI 复用同一判定）；跳过记录在报告生成期收集、随元数据批量写入 RocksDB 报告代前缀，随换代自动清理；GUI 加载报告时读取跳过统计与前 512 条明细，删除确认弹窗对旧规则报告增加风险勾选，删除线程在绕过时写审计事件。

**Tech Stack:** C++20、MSBuild（VideoSc.sln）、RocksDB（RocksStore 封装）、Dear ImGui、自研无框架测试（DedupTests/main.cpp，`Require` 宏）。

**Spec:** `docs/superpowers/specs/2026-07-18-permanent-deletion-trust-redesign-design.md`

## Global Constraints

- 不修改候选生成、三筛校验、分组算法；不修改 V4 元数据编解码格式，不提升报告 schema 版本；不新增 MySQL 表或字段。
- 不提供全局/持久化绕过开关；绕过仅在删除确认弹窗按次勾选。
- Exact（完全重复）报告行为不变。
- 所有 git commit/push 操作必须先获得用户明确确认；计划中的提交步骤标注"（需用户确认）"。
- 构建命令（msbuild 不在 PATH 时先用 vswhere 定位）：
  - Debug：`msbuild VideoSc.sln -p:Configuration=Debug -p:Platform=x64 -m`
  - Release：`msbuild VideoSc.sln -p:Configuration=Release -p:Platform=x64 -m`
- 测试命令：`x64/Debug/DedupTests.exe`、`x64/Release/DedupTests.exe`，要求全部通过（当前基线 48/48，本计划新增后 49/49）。

---

### Task 1: 删除可信判定收窄为证据质量

**Files:**
- Modify: `DedupCore/dedup/DuplicateReportService.cpp:1074-1079`
- Modify: `DedupCore/dedup/DuplicateReportService.h:179-184`（注释）
- Test: `DedupTests/main.cpp:2178-2182`

**Interfaces:**
- Consumes: 现有 `SimilarReportMetadata`（`DuplicateReportService.h:141-177`）。
- Produces: `bool IsSimilarReportEligibleForPermanentDeletion(const SimilarReportMetadata& metadata) noexcept;`（签名不变，语义变为"报告证据质量可信"）。GUI 删除线程（`VideoScApp.cpp:3178`）与报告面板复用该判定；`partial_scope_published`、`image_features_incomplete`、`deferred_hot_signatures` 不再参与。

- [ ] **Step 1: 先改测试断言（TDD，此时应失败）**

`DedupTests/main.cpp:2178-2182` 的旧断言：

```cpp
    Require(!videosc::dedup::IsSimilarReportEligibleForPermanentDeletion(loadedMetadata),
            "Partial-scope similarity report was incorrectly trusted for permanent deletion");
    loadedMetadata.partial_scope_published = false;
    Require(videosc::dedup::IsSimilarReportEligibleForPermanentDeletion(loadedMetadata),
            "Complete V4 three-stage similarity report was not trusted for permanent deletion");
```

替换为：

```cpp
    // 部分作用域与特征不完整不再影响删除可信：被跳过内容不进入分组，已入组成员证据完整。
    Require(videosc::dedup::IsSimilarReportEligibleForPermanentDeletion(loadedMetadata),
            "Partial-scope V4 three-stage similarity report was incorrectly blocked from permanent deletion");
    loadedMetadata.image_features_incomplete = 3;
    Require(videosc::dedup::IsSimilarReportEligibleForPermanentDeletion(loadedMetadata),
            "V4 three-stage report with incomplete image features was incorrectly blocked");
    loadedMetadata.image_uses_three_stage_verification = false;
    Require(!videosc::dedup::IsSimilarReportEligibleForPermanentDeletion(loadedMetadata),
            "Legacy non-three-stage similarity report was incorrectly trusted for permanent deletion");
    loadedMetadata.image_uses_three_stage_verification = true;
    loadedMetadata.report_schema_version = 3;
    Require(!videosc::dedup::IsSimilarReportEligibleForPermanentDeletion(loadedMetadata),
            "Non-V4 similarity report was incorrectly trusted for permanent deletion");
```

- [ ] **Step 2: 构建并确认测试按预期失败**

Run: `msbuild VideoSc.sln -p:Configuration=Debug -p:Platform=x64 -m` 然后 `x64/Debug/DedupTests.exe`
Expected: `persistent similarity report` 用例 FAIL（断言 1 失败）

- [ ] **Step 3: 修改判定实现**

`DedupCore/dedup/DuplicateReportService.cpp:1074-1079` 替换为：

```cpp
bool IsSimilarReportEligibleForPermanentDeletion(
    const SimilarReportMetadata& metadata) noexcept {
    return metadata.report_schema_version == 4 && metadata.image_uses_three_stage_verification;
}
```

`DedupCore/dedup/DuplicateReportService.h:179-184` 注释替换为：

```cpp
/**
 * @brief 判断视觉相似报告是否具备执行永久删除所需的证据质量。
 * @param metadata 已成功加载并校验的报告规则快照。
 * @return V4 且启用三级校验的报告返回 true。被跳过的视觉内容不进入分组，
 *         不影响已入组成员的成对证据，因此不再参与删除可信判定。
 */
```

- [ ] **Step 4: 重新构建并跑通测试**

Run: `msbuild VideoSc.sln -p:Configuration=Debug -p:Platform=x64 -m` 然后 `x64/Debug/DedupTests.exe`
Expected: 全部通过（48/48）

- [ ] **Step 5: Commit（需用户确认）**

```bash
git add DedupCore/dedup/DuplicateReportService.cpp DedupCore/dedup/DuplicateReportService.h DedupTests/main.cpp
git commit -m "refactor: 删除可信判定只看报告证据质量，部分作用域不再整报告阻断"
```

---

### Task 2: 跳过视觉内容记录模型与存储接口

**Files:**
- Modify: `DedupCore/dedup/DuplicateReportService.h`（在 `SimilarReportMetadata` 之后、`DuplicateReportStore` 之前新增模型；在 `DuplicateReportStore` 类 public 区新增三个方法声明）
- Modify: `DedupCore/dedup/DuplicateReportService.cpp`（匿名命名空间新增编解码；类实现新增三个方法）
- Test: `DedupTests/main.cpp`（新增 `TestSkippedVisualContentRecords`，注册到 `main()` 列表 `"persistent similarity report"` 之后）

**Interfaces:**
- Consumes: `RocksStore::WriteBatch / ForEachPrefix`（`DedupCore/persistence/RocksStore.h:103/113`）；匿名命名空间已有助手 `Append`、`AppendString`、`WideToUtf8`、`Utf8ToWide`、`Reader`、`Hex64`、`GenerationPrefix`。
- Produces（Task 3 与 Task 4/5 依赖）：

```cpp
enum class SkippedVisualContentReason : std::uint8_t {
    InvalidImage = 0,
    MissingVideoDHash = 1,
    ZeroVideoFrame = 2,
    UnsupportedMedia = 3,
    StructuralIoFailure = 4,
    StructuralTimeout = 5,
    StructuralComputeFailure = 6,
    Count,  // 计数哨兵，必须保持最后
};

struct SkippedVisualContentRecord {
    std::string primary_sha512;
    std::string secondary_sha512;  // 仅结构三筛失败时有值
    MediaKind media_kind = MediaKind::Other;
    SkippedVisualContentReason reason = SkippedVisualContentReason::InvalidImage;
    std::uint64_t active_path_count = 0;
    std::vector<std::wstring> sample_paths;  // 最多 4 条
};

struct SkippedVisualContentStats {
    std::uint64_t total = 0;
    std::array<std::uint64_t,
               static_cast<std::size_t>(SkippedVisualContentReason::Count)> by_reason{};
};

// DuplicateReportStore:
RocksStatus SaveSkippedContents(std::uint64_t generation_id,
                                const std::vector<SkippedVisualContentRecord>& records);
RocksStatus LoadSkippedContents(std::uint64_t generation_id,
                                std::uint64_t offset,
                                std::size_t maximum_items,
                                std::vector<SkippedVisualContentRecord>& records) const;
RocksStatus CountSkippedVisualContents(std::uint64_t generation_id,
                                       SkippedVisualContentStats& stats) const;
```

- [ ] **Step 1: 写失败测试**

在 `DedupTests/main.cpp` 中 `TestPersistentSimilarityReport` 函数结束之后（`:2215` `}` 之后）新增：

```cpp
/** @brief 跳过视觉内容记录必须可持久化、分页读取、按原因计数，并随报告换代清理。 */
void TestSkippedVisualContentRecords() {
    const std::filesystem::path directory = CreateTestDirectory(L"skipped-visuals");
    RocksDbConfig config;
    config.directory = directory / L"rocks";
    config.block_cache_mib = 8;
    config.write_buffer_mib = 4;
    RocksStore store(config);
    Require(store.Open().succeeded, "Cannot open skipped-content RocksDB");

    DuplicateReportStore reportStore(store);
    constexpr std::uint64_t generation = 41;
    std::vector<videosc::dedup::SkippedVisualContentRecord> records;
    videosc::dedup::SkippedVisualContentRecord imageSkip;
    imageSkip.primary_sha512 = "aa";
    imageSkip.media_kind = videosc::dedup::MediaKind::Image;
    imageSkip.reason = videosc::dedup::SkippedVisualContentReason::InvalidImage;
    imageSkip.active_path_count = 2;
    imageSkip.sample_paths = {L"C:\\media\\a.jpg", L"D:\\media\\b.jpg"};
    records.push_back(imageSkip);
    videosc::dedup::SkippedVisualContentRecord pairSkip;
    pairSkip.primary_sha512 = "bb";
    pairSkip.secondary_sha512 = "cc";
    pairSkip.media_kind = videosc::dedup::MediaKind::Image;
    pairSkip.reason = videosc::dedup::SkippedVisualContentReason::StructuralTimeout;
    pairSkip.active_path_count = 1;
    pairSkip.sample_paths = {L"E:\\media\\c.jpg"};
    records.push_back(pairSkip);
    videosc::dedup::SkippedVisualContentRecord videoSkip;
    videoSkip.primary_sha512 = "dd";
    videoSkip.media_kind = videosc::dedup::MediaKind::Video;
    videoSkip.reason = videosc::dedup::SkippedVisualContentReason::ZeroVideoFrame;
    records.push_back(videoSkip);
    Require(reportStore.SaveSkippedContents(generation, records).succeeded,
            "Cannot save skipped visual content records");

    std::vector<videosc::dedup::SkippedVisualContentRecord> page;
    Require(reportStore.LoadSkippedContents(generation, 1, 1, page).succeeded &&
                page.size() == 1 && page.front().primary_sha512 == "bb" &&
                page.front().secondary_sha512 == "cc" &&
                page.front().reason ==
                    videosc::dedup::SkippedVisualContentReason::StructuralTimeout &&
                page.front().active_path_count == 1 &&
                page.front().sample_paths.size() == 1 &&
                page.front().sample_paths.front() == L"E:\\media\\c.jpg",
            "Skipped visual content paged load differs");
    videosc::dedup::SkippedVisualContentStats stats;
    Require(reportStore.CountSkippedVisualContents(generation, stats).succeeded &&
                stats.total == 3 &&
                stats.by_reason[static_cast<std::size_t>(
                    videosc::dedup::SkippedVisualContentReason::InvalidImage)] == 1 &&
                stats.by_reason[static_cast<std::size_t>(
                    videosc::dedup::SkippedVisualContentReason::StructuralTimeout)] == 1 &&
                stats.by_reason[static_cast<std::size_t>(
                    videosc::dedup::SkippedVisualContentReason::ZeroVideoFrame)] == 1,
            "Skipped visual content stats differ");

    // 发布新一代报告后，旧代跳过记录必须随前缀清理。
    Require(reportStore.Publish(DuplicateReportKind::Similar, generation, 0).succeeded,
            "Cannot publish first generation");
    Require(reportStore.Publish(DuplicateReportKind::Similar, generation + 1, 0).succeeded,
            "Cannot publish second generation");
    videosc::dedup::SkippedVisualContentStats cleaned;
    Require(reportStore.CountSkippedVisualContents(generation, cleaned).succeeded &&
                cleaned.total == 0,
            "Skipped visual content records survived generation rollover");
    store.Close();
    std::filesystem::remove_all(directory);
}
```

在 `main()` 测试列表 `{"persistent similarity report", TestPersistentSimilarityReport},` 之后注册：

```cpp
        {"skipped visual content records", TestSkippedVisualContentRecords},
```

- [ ] **Step 2: 构建确认编译失败**

Run: `msbuild VideoSc.sln -p:Configuration=Debug -p:Platform=x64 -m`
Expected: 编译错误（`SkippedVisualContentRecord` 等未定义）

- [ ] **Step 3: 在头文件声明模型与接口**

`DedupCore/dedup/DuplicateReportService.h`：确认顶部包含 `<array>`（无则添加）。在 `SimilarReportMetadata` 定义结束（`:177`）之后插入"Interfaces"一节中的枚举、两个结构体及其 Doxygen 注释（注释风格对齐上下文）。在 `DuplicateReportStore` 的 `LoadSimilarMetadata` 声明之后（`:276` 之后）插入三个方法声明：

```cpp
    /** @brief 批量保存指定代的跳过视觉内容记录；空列表直接成功。随报告代前缀清理。 */
    RocksStatus SaveSkippedContents(std::uint64_t generation_id,
                                    const std::vector<SkippedVisualContentRecord>& records);

    /** @brief 按写入序号分页加载跳过记录。 */
    RocksStatus LoadSkippedContents(std::uint64_t generation_id,
                                    std::uint64_t offset,
                                    std::size_t maximum_items,
                                    std::vector<SkippedVisualContentRecord>& records) const;

    /** @brief 统计指定代跳过记录总数与按原因计数。 */
    RocksStatus CountSkippedVisualContents(std::uint64_t generation_id,
                                           SkippedVisualContentStats& stats) const;
```

- [ ] **Step 4: 实现编解码与存取**

`DedupCore/dedup/DuplicateReportService.cpp` 匿名命名空间（`DeserializeSimilarMetadata` 之后、`:1055` 的 `}  // namespace` 之前）新增：

```cpp
/** @brief 跳过记录编解码版本。 */
constexpr std::uint8_t kSkippedContentCodecVersion = 1;

/** @brief 编码一条跳过视觉内容记录。 */
std::string SerializeSkippedContent(const SkippedVisualContentRecord& record) {
    std::string output;
    Append(output, kSkippedContentCodecVersion);
    Append(output, static_cast<std::uint8_t>(record.reason));
    Append(output, static_cast<std::uint8_t>(record.media_kind));
    Append(output, record.active_path_count);
    AppendString(output, record.primary_sha512);
    AppendString(output, record.secondary_sha512);
    Append(output, static_cast<std::uint32_t>(record.sample_paths.size()));
    for (const std::wstring& path : record.sample_paths) AppendString(output, WideToUtf8(path));
    return output;
}

/** @brief 解码跳过视觉内容记录并拒绝未知版本。 */
std::optional<SkippedVisualContentRecord> DeserializeSkippedContent(const std::string& value,
                                                                    std::string& error) {
    try {
        Reader reader(value);
        if (reader.Read<std::uint8_t>() != kSkippedContentCodecVersion) {
            throw std::runtime_error("Unsupported skipped content record");
        }
        const std::uint8_t reason = reader.Read<std::uint8_t>();
        if (reason >= static_cast<std::uint8_t>(SkippedVisualContentReason::Count)) {
            throw std::runtime_error("Invalid skipped content reason");
        }
        const std::uint8_t mediaKind = reader.Read<std::uint8_t>();
        if (mediaKind > static_cast<std::uint8_t>(MediaKind::Audio)) {
            throw std::runtime_error("Invalid skipped content media kind");
        }
        SkippedVisualContentRecord record;
        record.reason = static_cast<SkippedVisualContentReason>(reason);
        record.media_kind = static_cast<MediaKind>(mediaKind);
        record.active_path_count = reader.Read<std::uint64_t>();
        record.primary_sha512 = reader.ReadString();
        record.secondary_sha512 = reader.ReadString();
        const std::uint32_t pathCount = reader.Read<std::uint32_t>();
        if (pathCount > 4) throw std::runtime_error("Skipped content record has too many sample paths");
        record.sample_paths.reserve(pathCount);
        for (std::uint32_t index = 0; index < pathCount; ++index) {
            record.sample_paths.push_back(Utf8ToWide(reader.ReadString()));
        }
        reader.RequireEnd();
        return record;
    } catch (const std::exception& exception) {
        error = exception.what();
        return std::nullopt;
    }
}
```

在 `LoadSimilarMetadata` 实现（`:1314` `}`）之后新增三个方法：

```cpp
RocksStatus DuplicateReportStore::SaveSkippedContents(
    const std::uint64_t generation_id,
    const std::vector<SkippedVisualContentRecord>& records) {
    try {
        const std::string prefix =
            GenerationPrefix(DuplicateReportKind::Similar, generation_id) + "skipped/";
        std::vector<RocksMutation> batch;
        batch.reserve(4096);
        for (std::size_t index = 0; index < records.size(); ++index) {
            batch.push_back({RocksColumnFamily::ExactIndex,
                             prefix + Hex64(index),
                             SerializeSkippedContent(records[index])});
            if (batch.size() == 4096) {
                const RocksStatus written = store_.WriteBatch(batch, false);
                if (!written.succeeded) return written;
                batch.clear();
            }
        }
        if (!batch.empty()) return store_.WriteBatch(batch, false);
        return {true, {}};
    } catch (const std::exception& exception) {
        return {false, exception.what()};
    }
}

RocksStatus DuplicateReportStore::LoadSkippedContents(
    const std::uint64_t generation_id,
    const std::uint64_t offset,
    const std::size_t maximum_items,
    std::vector<SkippedVisualContentRecord>& records) const {
    const std::string prefix =
        GenerationPrefix(DuplicateReportKind::Similar, generation_id) + "skipped/";
    std::uint64_t index = 0;
    std::string decodeError;
    const RocksStatus status = store_.ForEachPrefix(
        RocksColumnFamily::ExactIndex,
        prefix,
        0,
        [&](const std::string_view, const std::string_view value) {
            if (index++ < offset) return true;
            if (records.size() >= maximum_items) return false;
            std::optional<SkippedVisualContentRecord> decoded =
                DeserializeSkippedContent(std::string(value), decodeError);
            if (!decoded.has_value()) return false;
            records.push_back(std::move(*decoded));
            return true;
        });
    if (!status.succeeded) return status;
    if (!decodeError.empty()) return {false, decodeError};
    return {true, {}};
}

RocksStatus DuplicateReportStore::CountSkippedVisualContents(
    const std::uint64_t generation_id,
    SkippedVisualContentStats& stats) const {
    const std::string prefix =
        GenerationPrefix(DuplicateReportKind::Similar, generation_id) + "skipped/";
    std::string decodeError;
    const RocksStatus status = store_.ForEachPrefix(
        RocksColumnFamily::ExactIndex,
        prefix,
        0,
        [&](const std::string_view, const std::string_view value) {
            std::optional<SkippedVisualContentRecord> decoded =
                DeserializeSkippedContent(std::string(value), decodeError);
            if (!decoded.has_value()) return false;
            ++stats.total;
            ++stats.by_reason[static_cast<std::size_t>(decoded->reason)];
            return true;
        });
    if (!status.succeeded) return status;
    if (!decodeError.empty()) return {false, decodeError};
    return {true, {}};
}
```

- [ ] **Step 5: 构建并跑通测试**

Run: `msbuild VideoSc.sln -p:Configuration=Debug -p:Platform=x64 -m` 然后 `x64/Debug/DedupTests.exe`
Expected: 全部通过（49/49）

- [ ] **Step 6: Commit（需用户确认）**

```bash
git add DedupCore/dedup/DuplicateReportService.h DedupCore/dedup/DuplicateReportService.cpp DedupTests/main.cpp
git commit -m "feat: 新增跳过视觉内容记录的持久化存储与按原因统计"
```

---

### Task 3: 报告生成期收集并写入跳过记录

**Files:**
- Modify: `DedupCore/dedup/DuplicateReportService.cpp`（生成函数内：`:1698` 前声明收集容器；`:1717-1724` 视觉内容跳过点；`:2355-2367`、`:2378-2404`、`:2416-2434` 结构三筛失败点；`:2828` 前构建索引；`:2839` 后路径解析；`:3007` 后批量写入）
- Test: 无新增单元测试（生成链路依赖 MySQL，由 Task 2 存储测试与最终现场验收覆盖）；既有 49 个测试必须保持通过。

**Interfaces:**
- Consumes: Task 2 的 `SkippedVisualContentRecord` / `SaveSkippedContents`；生成函数局部 `result`、计数器 `structuralIoFailureCount` 等。
- Produces: 已发布相似报告的 `skipped/` 前缀记录，供 Task 4/5 GUI 加载。

- [ ] **Step 1: 声明收集容器**

`DedupCore/dedup/DuplicateReportService.cpp` 生成函数中，`publisher.BeginStage("streaming_mysql_visual_contents", 1, 9, false, 0);`（`:1698`）之前插入（确认文件顶部已包含 `<unordered_map>` 与 `<mutex>`，缺则补）：

```cpp
    // 生成期收集的跳过视觉内容记录；随元数据在发布前批量写入。
    std::mutex skippedContentsMutex;
    std::vector<SkippedVisualContentRecord> skippedContents;
```

- [ ] **Step 2: 视觉内容跳过点收集**

`:1717-1724` 的计数块（`++result.skipped_invalid_visuals;` 等）之后、`DuplicateReportDiagnostic diagnostic;` 之前插入：

```cpp
                {
                    std::lock_guard<std::mutex> skippedLock(skippedContentsMutex);
                    SkippedVisualContentRecord skipped;
                    skipped.primary_sha512 = Sha512ToHex(data.sha512);
                    skipped.media_kind = data.media_kind;
                    skipped.reason =
                        visualStatus == VisualDHashStatus::InvalidImage
                            ? SkippedVisualContentReason::InvalidImage
                            : visualStatus == VisualDHashStatus::MissingVideo
                                  ? SkippedVisualContentReason::MissingVideoDHash
                                  : visualStatus == VisualDHashStatus::ZeroVideoFrame
                                        ? SkippedVisualContentReason::ZeroVideoFrame
                                        : SkippedVisualContentReason::UnsupportedMedia;
                    skippedContents.push_back(std::move(skipped));
                }
```

- [ ] **Step 3: 结构三筛失败点收集**

`:2355-2367`（`structure_missing_path` 分支，`structuralMissingPaths.fetch_add(...)` 之后）插入：

```cpp
                            {
                                std::lock_guard<std::mutex> skippedLock(skippedContentsMutex);
                                SkippedVisualContentRecord skipped;
                                skipped.primary_sha512 = pair.left;
                                skipped.secondary_sha512 = pair.right;
                                skipped.media_kind = MediaKind::Image;
                                skipped.reason = SkippedVisualContentReason::StructuralIoFailure;
                                skippedContents.push_back(std::move(skipped));
                            }
```

`:2378-2404`（结构读取/解码失败或超时分支，`structuralTimeouts` 计数之后）插入同样代码块，但 reason 改为：

```cpp
                                skipped.reason = timedOut
                                                     ? SkippedVisualContentReason::StructuralTimeout
                                                     : SkippedVisualContentReason::StructuralIoFailure;
```

`:2416-2434`（`structure_compare_failed` 分支，`structuralComputeFailures.fetch_add(...)` 之后）插入同样代码块，reason 为：

```cpp
                                skipped.reason = SkippedVisualContentReason::StructuralComputeFailure;
```

- [ ] **Step 4: 活动路径阶段解析样例路径**

`publisher.BeginStage("joining_active_paths", 8, 9, false, 0);`（`:2828`）之前插入：

```cpp
    // 跳过记录按 SHA 建索引，供活动路径流匹配样例路径。
    std::unordered_map<std::string, std::vector<std::size_t>> skippedContentIndex;
    for (std::size_t index = 0; index < skippedContents.size(); ++index) {
        skippedContentIndex[skippedContents[index].primary_sha512].push_back(index);
        if (!skippedContents[index].secondary_sha512.empty()) {
            skippedContentIndex[skippedContents[index].secondary_sha512].push_back(index);
        }
    }
```

在 `joining_active_paths` 回调内，`const std::string digestHex = Sha512ToHex(*path.sha512);`（`:2839`）之后、`resolveVisualRepresentative` 调用之前插入（必须在代表解析提前返回之前，被跳过的 SHA 无法解析为代表）：

```cpp
            const auto skippedMatch = skippedContentIndex.find(digestHex);
            if (skippedMatch != skippedContentIndex.end()) {
                for (const std::size_t skippedIndex : skippedMatch->second) {
                    SkippedVisualContentRecord& skipped = skippedContents[skippedIndex];
                    ++skipped.active_path_count;
                    if (skipped.sample_paths.size() < 4) {
                        skipped.sample_paths.push_back(path.path.wstring());
                    }
                }
            }
```

- [ ] **Step 5: 随元数据批量写入**

`SaveSimilarMetadata` 成功块（`:3001-3007`）之后插入：

```cpp
    const RocksStatus savedSkippedContents =
        reportStore.SaveSkippedContents(result.generation_id, skippedContents);
    if (!savedSkippedContents.succeeded) {
        result.message = savedSkippedContents.message;
        publisher.Publish(true);
        return result;
    }
```

- [ ] **Step 6: 构建并回归全部测试**

Run: `msbuild VideoSc.sln -p:Configuration=Debug -p:Platform=x64 -m` 然后 `x64/Debug/DedupTests.exe`
Expected: 编译 0 错误；测试 49/49 通过

- [ ] **Step 7: Commit（需用户确认）**

```bash
git add DedupCore/dedup/DuplicateReportService.cpp
git commit -m "feat: 报告生成期收集跳过视觉内容并随报告代持久化"
```

---

### Task 4: GUI 删除流程改造——旧规则报告绕过开关与审计

**Files:**
- Modify: `VideoScGUI/VideoScApp.h`（`:491` `m_visibleSimilarReportMetadata` 附近新增状态）
- Modify: `VideoScGUI/VideoScApp.cpp`（`ClearLoadedReport` `:2395-2419`；`LoadReport` `:2626-2639`；`StartDeletion` `:3037-3074` 与删除线程 `:3168-3183`；确认弹窗 `:5299-5317`；文案 `:5103-5106`、`:6718`）
- Test: 无新增单元测试（GUI 行为由现场验收覆盖）；既有 49 个测试必须保持通过。

**Interfaces:**
- Consumes: Task 1 的新判定语义；Task 2 的 `CountSkippedVisualContents` / `LoadSkippedContents`；`ExecutionLogger::WriteEvent`（事件结构 `ExecutionEventRecord{task_id, task, event, stage, message, processed_items, total_items}`，`DedupCore/logging/ExecutionLogger.h:13-21`）；帧循环中 `m_deleteConfirmed` → `StartDeletion()`（`:6982-6985`）。
- Produces: `m_visibleReportTrusted`（Task 5 面板与确认弹窗使用）；`m_visibleSkippedStats` / `m_visibleSkippedContents`（Task 5 使用）。

- [ ] **Step 1: 新增 GUI 状态**

`VideoScGUI/VideoScApp.h` 在 `m_visibleSimilarReportMetadata`（`:491`）之后新增：

```cpp
    /** @brief 当前可见报告是否具备删除可信证据；Exact 恒 true，Similar 按元数据判定。 */
    bool m_visibleReportTrusted = true;
    /** @brief 用户在删除确认弹窗勾选的对旧规则报告强制删除。 */
    bool m_deleteOverrideUntrusted = false;
    /** @brief 当前相似报告跳过内容统计与前 512 条明细。 */
    videosc::dedup::SkippedVisualContentStats m_visibleSkippedStats;
    std::vector<videosc::dedup::SkippedVisualContentRecord> m_visibleSkippedContents;
```

`ClearLoadedReport()`（`:2395-2419`）末尾、`m_clearReportThumbnailsRequested = true;` 之前新增：

```cpp
    m_visibleReportTrusted = true;
    m_deleteOverrideUntrusted = false;
    m_visibleSkippedStats = {};
    m_visibleSkippedContents.clear();
```

- [ ] **Step 2: LoadReport 加载可信判定与跳过内容**

`LoadReport()` 中 `m_visibleSimilarReportMetadata = std::move(metadata);`（`:2638`）之后插入：

```cpp
        m_visibleReportTrusted =
            videosc::dedup::IsSimilarReportEligibleForPermanentDeletion(
                *m_visibleSimilarReportMetadata);
        const videosc::dedup::RocksStatus skippedStatsLoaded =
            reports.CountSkippedVisualContents(*active, m_visibleSkippedStats);
        const videosc::dedup::RocksStatus skippedLoaded =
            reports.LoadSkippedContents(*active, 0, 512, m_visibleSkippedContents);
        if (!skippedStatsLoaded.succeeded || !skippedLoaded.succeeded) {
            m_reportMessage = "加载报告跳过内容失败。";
            m_reportMessageIsError = true;
            return;
        }
```

- [ ] **Step 3: StartDeletion 捕获绕过标记**

`StartDeletion()` 中 `m_deletionStopRequested.store(false);`（`:3038`）之前插入：

```cpp
    const bool overrideUntrusted = m_deleteOverrideUntrusted;
    m_deleteOverrideUntrusted = false;
```

`StartBackgroundThread` 的捕获列表（`:3065-3074`）中按值增加 `overrideUntrusted`。

- [ ] **Step 4: 删除线程信任逻辑与审计事件**

删除线程内 `:3168-3183` 整段（`DuplicateReportStore reports(*store);` 到不可信 `throw`）替换为：

```cpp
            videosc::dedup::DuplicateReportStore reports(*store);
            bool trustedThreeStageReport = reportKind == videosc::dedup::DuplicateReportKind::Exact;
            if (reportKind == videosc::dedup::DuplicateReportKind::Similar) {
                videosc::dedup::SimilarReportMetadata metadata;
                const videosc::dedup::RocksStatus metadataLoaded =
                    reports.LoadSimilarMetadata(generation, metadata);
                if (!metadataLoaded.succeeded) {
                    throw std::runtime_error("删除前报告规则校验失败：" + metadataLoaded.message);
                }
                trustedThreeStageReport =
                    videosc::dedup::IsSimilarReportEligibleForPermanentDeletion(metadata);
                if (!trustedThreeStageReport) {
                    if (!overrideUntrusted) {
                        throw std::runtime_error(
                            "当前相似报告为旧规则报告，缺少三筛删除证据；如确认风险，请在删除确认窗口勾选强制删除。");
                    }
                    if (!executionLogger.WriteEvent(
                            {deletionTaskId,
                             "permanent_delete",
                             "override_untrusted_report",
                             "preflight",
                             "用户确认风险，对旧规则相似报告强制执行永久删除",
                             0,
                             selectedGroupCount},
                            executionLogError)) {
                        throw std::runtime_error("强制删除审计日志写入失败：" + executionLogError);
                    }
                    trustedThreeStageReport = true;
                }
            }
```

- [ ] **Step 5: 确认弹窗增加风险勾选**

`VideoScApp.cpp:5299-5317` 的确认弹窗内，`:5301` 的 `TextWrapped` 之后插入：

```cpp
        if (m_visibleReportKind == videosc::dedup::DuplicateReportKind::Similar &&
            !m_visibleReportTrusted) {
            ImGui::TextColored(ImVec4(0.98f, 0.70f, 0.53f, 1.0f),
                               "当前相似报告为旧规则报告，缺少三筛删除证据，强制删除存在误删风险。");
            ImGui::Checkbox("我已了解风险，仍要强制删除", &m_deleteOverrideUntrusted);
        }
```

确认按钮（`:5309-5312`）改为勾选前禁用：

```cpp
        const bool overrideBlocked =
            m_visibleReportKind == videosc::dedup::DuplicateReportKind::Similar &&
            !m_visibleReportTrusted && !m_deleteOverrideUntrusted;
        PushButtonStyleDanger();
        ImGui::BeginDisabled(overrideBlocked);
        if (ImGui::Button("确认永久删除", ImVec2(150, 30))) {
            m_deleteConfirmed = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::PopStyleColor(3);
```

（删除原 `PushButtonStyleDanger();` + 按钮 + `ImGui::PopStyleColor(3);` 三行旧写法。）

- [ ] **Step 6: 文案调整**

`:5103-5106` 部分作用域警告文案改为：

```cpp
                    ImGui::TextColored(ImVec4(0.98f, 0.70f, 0.53f, 1.0f),
                                       "该报告自动跳过了无法完成特征或结构校验的资源；结果仅覆盖成功计算的视觉内容。");
```

`:6718` 只读说明改为：

```cpp
        ImGui::TextDisabled("无法完成特征或结构校验的资源会自动跳过并计入报告统计；跳过明细见报告信息的跳过内容面板。");
```

- [ ] **Step 7: 构建并回归全部测试**

Run: `msbuild VideoSc.sln -p:Configuration=Debug -p:Platform=x64 -m` 然后 `x64/Debug/DedupTests.exe`
Expected: 编译 0 错误；测试 49/49 通过

- [ ] **Step 8: Commit（需用户确认）**

```bash
git add VideoScGUI/VideoScApp.h VideoScGUI/VideoScApp.cpp
git commit -m "feat: 部分作用域报告放行永久删除，旧规则报告增加风险确认绕过与审计"
```

---

### Task 5: GUI 跳过内容面板与修复入口

**Files:**
- Modify: `VideoScGUI/VideoScApp.cpp`（`SkippedReasonName` 助手，`:626` `DHashReportStageName` 附近；面板插入到 `:5123` 与 `:5124` 的 `ImGui::Separator()` 之间）
- Test: 无新增单元测试；既有 49 个测试必须保持通过。

**Interfaces:**
- Consumes: Task 4 的 `m_visibleSkippedStats`、`m_visibleSkippedContents`、`m_visibleReportKind`、`m_visibleSimilarReportMetadata`；`StartReportGeneration(videosc::dedup::DuplicateReportKind, bool)`（`VideoScApp.h:227`）；文件级助手 `WideToUtf8`（`VideoScApp.cpp:5291` 已使用）。

- [ ] **Step 1: 新增原因名称助手**

`DHashReportStageName`（`:626-639`）之后新增：

```cpp
/**
 * @brief 把跳过视觉内容原因转换为中文名称。
 * @param reason 跳过记录中的原因枚举。
 * @return 面向用户的中文原因名称。
 */
static const char* SkippedReasonName(const videosc::dedup::SkippedVisualContentReason reason) {
    using videosc::dedup::SkippedVisualContentReason;
    switch (reason) {
        case SkippedVisualContentReason::InvalidImage: return "图片三级特征未完成";
        case SkippedVisualContentReason::MissingVideoDHash: return "视频六帧 dHash 缺失";
        case SkippedVisualContentReason::ZeroVideoFrame: return "视频 dHash 含零帧";
        case SkippedVisualContentReason::UnsupportedMedia: return "不支持的媒体";
        case SkippedVisualContentReason::StructuralIoFailure: return "结构三筛读取/解码失败";
        case SkippedVisualContentReason::StructuralTimeout: return "结构三筛读取超时";
        case SkippedVisualContentReason::StructuralComputeFailure: return "结构三筛计算失败";
        case SkippedVisualContentReason::Count: break;
    }
    return "未知原因";
}
```

- [ ] **Step 2: 插入跳过内容面板**

在"当前报告信息"折叠区结束（`:5123` `}`）之后、`ImGui::Separator();`（`:5124`）之前插入：

```cpp
    if (m_visibleReportKind == videosc::dedup::DuplicateReportKind::Similar &&
        m_visibleSimilarReportMetadata.has_value() && m_visibleSkippedStats.total != 0 &&
        ImGui::CollapsingHeader("跳过内容")) {
        const auto& counts = m_visibleSkippedStats.by_reason;
        using videosc::dedup::SkippedVisualContentReason;
        const auto countOf = [&](const SkippedVisualContentReason reason) {
            return counts[static_cast<std::size_t>(reason)];
        };
        ImGui::Text("共 %llu 条：无效图片 %llu | 视频 dHash 缺失 %llu | 视频零帧 %llu | 不支持 %llu | 结构读取失败 %llu | 结构超时 %llu | 结构计算失败 %llu",
                    static_cast<unsigned long long>(m_visibleSkippedStats.total),
                    static_cast<unsigned long long>(countOf(SkippedVisualContentReason::InvalidImage)),
                    static_cast<unsigned long long>(countOf(SkippedVisualContentReason::MissingVideoDHash)),
                    static_cast<unsigned long long>(countOf(SkippedVisualContentReason::ZeroVideoFrame)),
                    static_cast<unsigned long long>(countOf(SkippedVisualContentReason::UnsupportedMedia)),
                    static_cast<unsigned long long>(countOf(SkippedVisualContentReason::StructuralIoFailure)),
                    static_cast<unsigned long long>(countOf(SkippedVisualContentReason::StructuralTimeout)),
                    static_cast<unsigned long long>(countOf(SkippedVisualContentReason::StructuralComputeFailure)));
        ImGui::BeginChild("skipped_contents", ImVec2(0.0f, 160.0f), true);
        for (const auto& record : m_visibleSkippedContents) {
            ImGui::TextWrapped("%s | %s%s%s",
                               SkippedReasonName(record.reason),
                               record.primary_sha512.c_str(),
                               record.secondary_sha512.empty() ? "" : " <-> ",
                               record.secondary_sha512.c_str());
            for (const std::wstring& path : record.sample_paths) {
                ImGui::TextDisabled("  %s", WideToUtf8(path).c_str());
            }
        }
        ImGui::EndChild();
        if (m_visibleSkippedStats.total > m_visibleSkippedContents.size()) {
            ImGui::TextDisabled("仅显示前 %llu 条。",
                                static_cast<unsigned long long>(m_visibleSkippedContents.size()));
        }
        if (ImGui::Button("重新生成相似报告（自动回填图片特征）")) {
            StartReportGeneration(videosc::dedup::DuplicateReportKind::Similar, false);
        }
    }
```

- [ ] **Step 3: 全量构建与双配置测试**

Run:
```
msbuild VideoSc.sln -p:Configuration=Debug -p:Platform=x64 -m
x64/Debug/DedupTests.exe
msbuild VideoSc.sln -p:Configuration=Release -p:Platform=x64 -m
x64/Release/DedupTests.exe
```
Expected: 两个配置编译 0 错误；测试均 49/49 通过

- [ ] **Step 4: Commit（需用户确认）**

```bash
git add VideoScGUI/VideoScApp.cpp
git commit -m "feat: 报告信息新增跳过内容面板与重新生成入口"
```

---

## 最终验证清单

1. Debug 与 Release 全解决方案编译 0 错误。
2. 两个配置 `DedupTests.exe` 均 49/49 通过。
3. 现场验收（需真实数据，用户执行）：
   - 对含跳过内容的 v4 相似报告：不再出现"不具备完整删除证据"整报告阻断；报告信息可见"跳过内容"面板与样例路径；永久删除可执行且逐个复核 SHA-512。
   - 对旧规则相似报告：删除确认弹窗出现风险提示与强制勾选，未勾选时确认按钮禁用；勾选执行后执行日志含 `override_untrusted_report` 事件。
   - 修复坏文件后点"重新生成相似报告"，新报告跳过计数归零。

## Self-Review 记录

- Spec 覆盖：4.1→Task 1/4；4.2→Task 2/3/5；4.3→Task 4；4.4→Task 4 Step 6；4.6→Task 1/2 测试与最终清单。
- 类型一致性：`SkippedVisualContentRecord/Stats/Reason`、`Save/Load/CountSkippedVisualContents`、`m_visibleReportTrusted`、`m_deleteOverrideUntrusted` 在 Task 2/4/5 间签名一致。
- 已知留白：跳过面板只加载前 512 条明细（统计始终全量），分页浏览留给后续迭代；修复入口复用既有报告生成流程（首阶段自动回填图片特征），不新增独立回填任务。
