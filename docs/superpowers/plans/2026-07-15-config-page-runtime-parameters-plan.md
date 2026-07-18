# 配置页面运行参数暴露与分组调整计划

> 日期：2026-07-15  
> 状态：已按确认边界执行完成

## 1. 需求目标

在“设置”窗口中集中展示并持久化以下四类运行参数：

1. 缩略图生成相关参数。
2. Everything 文件发现相关参数。
3. SHA-512 哈希计算与文件读取相关参数。
4. 计算线程与磁盘读取线程相关参数。

## 2. 当前代码现状

### 2.1 已经存在并已显示在设置页面的参数

当前 `VideoScApp::RenderSettingsWindow()` 已经显示并保存下列参数，只是分散在不同折叠区域：

| 分类 | 已有参数 |
| --- | --- |
| 缩略图 | 输出目录、JPEG/PNG、视频拼图单格长边、图片预览长边、缓存条目、内存上限、显存上限 |
| Everything | Native/Everything 发现方式、`Everything64.dll` 路径、`Everything.exe` 路径 |
| 哈希读取 | 正常读取块、每盘队列容量、HDD 物理位置优化、HDD 排序窗口、正常块重试、小块重试、小块尺寸、无进展超时 |
| 线程 | 总计算线程、FFmpeg 单任务线程、每物理盘读取线程 |

这些字段已经由 `AppConfig`、`JsonConfigStore`、`ConfigValidator` 和 `ScanOptions` 串联，不需要重复增加第二套配置。

### 2.2 尚未配置化的 Everything 参数

扫描发现器和诊断查询中仍有以下硬编码：

| 参数 | 当前值 | 推荐配置字段 |
| --- | ---: | --- |
| 索引分页数量 | 4096 | `discovery.query_page_size` |
| Everything 启动等待 | 30 秒 | `discovery.launch_timeout_seconds` |
| Everything 数据库加载等待 | 120 秒 | `discovery.db_load_timeout_seconds` |
| 就绪轮询间隔 | 200 毫秒 | `discovery.poll_interval_milliseconds` |

推荐让扫描发现器与“文件列表检索”诊断工具共同使用这一组参数，避免两个入口行为不一致。

### 2.3 不建议直接配置化的内部常量

以下参数不是普通性能调节项，不建议在本次直接暴露：

1. Everything 最大路径缓冲 `32768`：这是 Windows/SDK 安全边界，不是用户性能参数。
2. SHA-512 算法类型：当前持久化、去重和同步协议均以完整 SHA-512 为身份，不能在设置页随意切换。
3. 视频固定六段 dHash 与 2×3 拼图：`VideoScMediaResult`、RocksDB 模型、相似度规则和算法版本 `media-dhash-v2` 都固定为六段。改变数量属于算法与数据模型迁移，不是单纯 UI 参数。
4. 哈希调度器内部 250 毫秒提交等待、媒体队列容量倍数：属于线程协调细节，暴露后容易造成忙等或吞吐退化。

## 3. 推荐执行范围

### 3.1 重新整理设置页面

将现有配置重新组织为四个清晰折叠区：

1. `计算线程`
   - 总计算线程。
   - FFmpeg 单任务线程。
   - 每物理盘读取线程表。
2. `SHA-512 与磁盘读取`
   - 读取块、队列容量、重试、小块、无进展超时。
   - HDD 物理位置优化和排序窗口。
3. `Everything 文件发现`
   - 发现方式、DLL、EXE。
   - 新增分页数量、启动超时、数据库加载超时、轮询间隔。
4. `缩略图生成与缓存`
   - 保留现有全部缩略图字段。
   - 明确提示“扫描媒体指纹固定六段和 2×3 拼图，不随诊断截图参数变化”。

### 3.2 增加 Everything 高级配置

在 `DiscoveryConfig` 增加四个带默认值的字段，并同步修改：

- `AppConfig.h` 默认值。
- `JsonConfigStore.cpp` 保存与兼容读取。
- `ConfigValidator.cpp` 范围校验。
- `ScanOptionsCodec.cpp` 任务快照保存与恢复。
- `EverythingFileDiscovery` 扫描入口消费配置。
- `EverythingFileListQuery` 诊断入口消费同一配置。
- `VideoScApp` 编辑缓冲同步与设置控件。

建议校验范围：

| 字段 | 最小值 | 最大值 |
| --- | ---: | ---: |
| `query_page_size` | 128 | 100000 |
| `launch_timeout_seconds` | 1 | 600 |
| `db_load_timeout_seconds` | 1 | 3600 |
| `poll_interval_milliseconds` | 10 | 5000 |

新增字段采用可选读取和默认值回退，现有 `schema_version: 1` 配置文件可直接加载，不因缺少字段损坏。

### 3.3 参数生效语义

1. 保存设置只写入 `config.json`；正在运行的扫描继续使用启动时冻结的 `ScanOptions`。
2. 下一次新扫描使用新参数。
3. 可恢复扫描从检查点读取当时冻结的参数，避免恢复后使用当前页面的新值。
4. Everything 分页和等待参数同时影响去重扫描与诊断文件列表查询。

## 4. 测试计划

1. JSON 旧配置缺少新增字段时使用默认值。
2. JSON 保存/加载后四个 Everything 字段保持一致。
3. 越界值被 `ConfigValidator` 拒绝。
4. `ScanOptionsCodec` 恢复后参数与扫描启动时一致。
5. 设置页面保存后重新加载，全部参数保持一致。
6. 既有增量扫描、取消、DLL 异常边界测试继续通过。
7. `Debug|x64`、`Release|x64` 全解决方案构建成功，`DedupTests` 全部通过。

## 5. 已确认的执行边界

推荐按以下边界执行：

1. 重新分组并保留当前已经存在的缩略图、哈希和线程参数。
2. 新增 Everything 的分页数量、启动超时、数据库加载超时、轮询间隔四项。
3. 不修改固定六段 dHash、2×3 拼图和 SHA-512 算法。
4. 本次不新增 JPEG 质量、PNG 压缩级别或可变截图数量；这些需要扩展 DLL ABI 或媒体算法模型，另立需求处理。

## 6. 执行结果

已于 2026-07-15 完成以下修改：

1. `DiscoveryConfig` 新增 Everything 分页数量、启动超时、数据库加载超时和轮询间隔，并加入 JSON 持久化与范围校验。
2. 正式去重扫描和诊断文件列表查询已共同使用上述参数；运行前额外做安全范围收敛，避免未保存的非法编辑值造成零间隔死循环。
3. `ScanOptionsCodec` 已记录发现方式、Everything 路径和四个运行参数；旧扫描快照缺少 `discovery` 对象时仍使用默认值恢复。
4. 设置页面已重组为“计算线程”“SHA-512 与磁盘读取”“Everything 文件发现”“缩略图生成与缓存”四个区域。
5. 固定六帧 dHash、2×3 拼图、SHA-512 算法和 `media-dhash-v2` 算法版本均未修改。
6. 补充了配置保存/加载、旧配置缺字段、扫描快照保存/恢复及旧快照兼容测试。

验证结果：

- `Debug|x64` 全解决方案构建成功，`DedupTests`：`32/32 passed`。
- `Release|x64` 全解决方案构建成功，`DedupTests`：`32/32 passed`。
- 构建仅保留项目原有的 vcpkg manifest 未启用提示，无编译错误或测试失败。
