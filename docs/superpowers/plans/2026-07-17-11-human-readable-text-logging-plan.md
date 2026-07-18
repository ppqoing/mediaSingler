# 11－易读文本执行日志修改计划

> 日期：2026-07-17  
> 状态：已执行，等待最终验收
> 编码：UTF-8 文本

## 1. 修改目标

人工日志不再使用 JSON 格式。读取、计算、报告、删除和持久化操作未完成时，记录可直接人工排查的路径、阶段、错误码和详细原因。

## 2. 当前实现

- `ExecutionLogger` 写 `execution.jsonl` 和 `execution-failures.jsonl`。
- `ApplicationErrorLogger` 写 `application-errors.jsonl`。
- `ScanErrorLogger` 写 `scan-errors.jsonl`。
- `OperationLogger` 写 `operations.jsonl`。
- 各记录写入后已经 flush，并有滚动配置。

## 3. 新日志文件

```text
execution.log
execution-failures.log
scan-errors.log
operations.log
application-errors.log
```

旧 JSONL 文件保留，不迁移、不删除；升级后不再向其追加。

## 4. 文本格式

每条事件占一行：

```text
2026-07-17T16:20:31.123+08:00 [ERROR] task=scan task_id=... stage=sha512 operation=read path="E:\\media\\a.mp4" disk="PhysicalDrive2" status=read_timeout native_error=121 offset=... bytes_read=... detail="读取 60 秒无进展"
```

要求：

- 使用带时区 ISO 8601 时间。
- 稳定字段名使用英文，说明文本使用中文。
- 路径、引号、反斜线、换行和控制字符统一转义。
- 一条事件严格一行，避免多线程交叉。
- 每条写入后立即 flush。
- 保留现有滚动大小、滚动数量和保留天数配置。

## 5. 公共格式化器

新增 `TextLogFormatter`，只负责：

- 时间格式化。
- 级别和稳定字段输出。
- UTF-8 字符串与路径转义。
- 单行构建。

它不负责文件滚动、锁、业务分类或异常决策，避免成为过度通用的大型日志框架。

## 6. 必须记录的失败

### 读取

- 文件打开失败、权限不足、共享冲突。
- 坏块、短读、无进展超时、设备离线、路径消失。
- 失败偏移、已读取字节、Win32 错误码。

### 计算

- SHA-512、图片 dHash、视频 6 帧、缩略图、FFmpeg。
- 无效图片尺寸、无效视频 dHash。
- 线程池任务异常、内存不足和取消。

### 报告

- MySQL 读取、候选索引、汉明校验、严格分组。
- metadata、正式组、关系、发布和半成品清理。

### 删除

- 保留文件复核、待删文件 SHA 不一致。
- `DeleteFileW`、tombstone、映射排队和恢复。

### 持久化与 GUI

- RocksDB 打开、锁占用、读写、批处理和关闭。
- 布局保存、预览生成和后台命令失败。

## 7. 失败记录最小字段

```text
task_id
path_id
task
stage
operation
path
storage_target_key
media_kind
status / status_code / native_error
failed_offset / bytes_read
detail
```

没有路径的数据库或报告错误改为记录键类别、generation ID 和相关摘要。

## 8. 预计修改文件

- 新增 `DedupCore/logging/TextLogFormatter.h/.cpp`
- `DedupCore/logging/ExecutionLogger.h/.cpp`
- `DedupCore/logging/ApplicationErrorLogger.h/.cpp`
- `DedupCore/logging/ScanErrorLogger.h/.cpp`
- `DedupCore/deletion/DeletionService.cpp`
- `VideoScGUI/diagnostics/CrashHandler.cpp`
- `DedupTests/main.cpp`

## 9. 日志安全

- 多线程写使用进程级互斥。
- 日志目录不可写时，危险删除和扫描任务在开始前失败。
- 应用异常记录器保持 `noexcept`，主日志失败时尝试应急 crash 文本文件。
- 日志不记录 MySQL 明文密码、DPAPI 密文或其他秘密配置。

## 10. 测试与验收

1. 记事本直接打开可读，无 JSON 对象结构。
2. 中文路径、引号和换行不会破坏单行结构。
3. 并发写入无行交叉。
4. 人工制造读取、计算、报告和删除失败，均包含详细原因。
5. 滚动和保留策略继续有效。
6. 旧 JSONL 文件不被删除。

## 11. 不在本条范围内

- `config.json`、checkpoint、tombstone、manifest 和 RocksDB value 是内部数据，不改成文本日志格式。
- 不引入外部日志框架。
- 不逐文件记录所有成功操作，避免日志 IO 影响扫描性能。
