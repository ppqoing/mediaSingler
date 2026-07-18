# 12－重复报告选择状态持久化修改计划

> 日期：2026-07-17  
> 状态：已执行，等待最终验收
> 状态存储：RocksDB，按报告类型和 generation 隔离

## 1. 修改目标

选择按钮直接修改持久化数据状态。可见行、离屏行、详情窗口、删除确认和删除执行器读取同一份选择结果。

## 2. 当前问题

- `ApplyDeletionSelection()` 遍历 `m_reportGroupCache`。
- “按条件选择全部报告”虽然设置整个报告标志，但没有立即物化离屏组选择。
- `m_reportSelections` 只在进程内保存，缓存清理或重启后丢失。
- 删除时可能重新按策略生成结果，与界面显示选择不是同一事实来源。

## 3. 持久化模型

新增 `ReportSelectionStore`：

```text
report-selection/<kind>/<report-generation>/active
report-selection/<kind>/<report-generation>/<selection-generation>/metadata
report-selection/<kind>/<report-generation>/<selection-generation>/group/<group-id>/member/<path-id>
report-selection/<kind>/<report-generation>/<selection-generation>/group-summary/<group-id>
```

metadata：

```text
selection_generation
source_report_generation
selected_file_count
selected_total_bytes
selected_group_count
updated_utc_ms
```

只保存选中成员；未出现的成员默认保留。

## 4. 单个成员选择

1. 读取当前组选择摘要。
2. 校验切换后仍至少保留一个活动成员。
3. 使用一个 `WriteBatch` 更新成员键、组摘要和全局摘要。
4. 提交成功后发布新的 `ReportSelectionSnapshot`。
5. GUI 不先乐观修改缓存；写入失败时保持旧状态并显示错误。

## 5. 批量选择

### 当前可见组

- 只对当前可见组应用策略。
- 每个组的结果仍写入持久化选择，而不是只修改缓存。

### 全部报告

1. 后台流式读取所有组。
2. 对每组运行 `DeletionPlanner::Select()`。
3. 写入新的 selection staging generation。
4. 累计文件数、总体积和组数。
5. 全部完成后原子切换 active selection 指针。
6. 强杀或失败时删除 staging，继续使用上一份 active selection。

## 6. GUI 数据来源

- `AcquireReportGroup()` 从 `ReportSelectionSnapshot` 合并选择状态。
- 虚拟列表渲染只读取成员是否选中，不根据可见缓存推断。
- 详情窗口和主列表共享同一个快照。
- 排序、滚动、缓存淘汰和缩略图释放不得改变选择。

## 7. 生命周期

- 应用重启后加载当前 active selection。
- 新报告 generation 发布时创建空选择并清理旧 generation 选择。
- 删除报告时同步删除其 selection namespace。
- 成功永久删除的成员从选择状态移除并更新摘要。

## 8. 预计修改文件

- 新增 `DedupCore/dedup/ReportSelectionStore.h/.cpp`
- `DedupCore/dedup/DuplicateReportService.h/.cpp`
- `DedupCore/deletion/DeletionService.cpp`
- `VideoScGUI/VideoScApp.h/.cpp`
- `DedupTests/main.cpp`

## 9. 异常与恢复

- 批量选择带 task ID、进度和恢复记录。
- staging 写入失败不切换 active 指针。
- metadata 与成员摘要不一致时拒绝危险删除，并提供流式重建摘要接口。
- 所有选择键均校验报告 generation，禁止旧状态污染新报告。

## 10. 测试与验收

1. 全选后滚动到从未显示的组，成员正确高亮。
2. 关闭并重启后选择仍存在。
3. 排序、切换详情和缓存清理不丢选择。
4. 批量选择中强杀后保留上一份完整选择。
5. 同组无法选中全部成员。
6. 新报告不会复用旧 generation 选择。
7. 删除执行器消费的路径集合与界面显示完全一致。

## 11. 不在本条范围内

- 不把选择同步到 MySQL。
- 不把跨严格组只读关系成员加入删除选择。
- 不依赖 GUI 可见范围决定“全部报告”的业务含义。
