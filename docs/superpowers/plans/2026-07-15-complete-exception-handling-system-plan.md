# 完整异常捕获与异常处理系统计划

> 日期：2026-07-15  
> 状态：已按推荐范围执行完成

## 1. 目标

为 `VideoScGUI.exe`、后台任务和两个原生 DLL 建立分层异常处理，达到以下目标：

1. 可恢复的业务异常在当前任务边界内结束，向用户显示明确错误，不导致整个进程退出。
2. 所有 `std::thread` 入口均有最终 `catch (...)` 边界，任何 C++ 异常都不能逃逸到 C++ 运行库并触发 `std::terminate`。
3. `VideoSc.dll`、`DiskInfo.dll` 的 C ABI 导出函数不允许 C++ 异常跨 DLL 边界传播。
4. 无法恢复的访问冲突、栈损坏、纯虚函数调用、非法参数、`std::terminate` 等故障尽可能记录诊断信息和转储，然后安全终止进程。
5. 异常日志具备时间、线程、模块、操作、错误码和调用栈等上下文，且不记录数据库密码等敏感配置。
6. 异常处理本身失败时有可执行文件目录下的兜底记录路径，避免二次异常覆盖原始故障。

## 2. 当前代码盘点

### 2.1 已有能力

- `DedupCore/logging/ScanErrorLogger` 已将单文件读取失败写入 `scan-errors.jsonl`，支持互斥和滚动。
- `DedupCore/deletion/OperationLogger` 已记录永久删除操作。
- 报告生成、永久删除和部分运行时初始化路径已经捕获 `std::exception` 并将错误回传到 GUI。
- `ScanCoordinator::WorkerMain` 和 `MySqlSyncExecutor::Apply` 等少数工作入口已有局部异常捕获。
- 各工程均生成调试信息，具备输出 PDB 和解析转储的基础。

### 2.2 主要缺口

1. `VideoScGUI/main.cpp` 的初始化、消息循环、`VideoScApp::Render`、延迟操作和退出清理没有顶层 `try/catch`。
2. 工程中未安装 `SetUnhandledExceptionFilter`、`std::set_terminate`、纯虚函数/非法参数处理器，也未调用 `MiniDumpWriteDump`。
3. `VideoScApp` 的数据库初始化线程没有异常捕获；报告和删除线程只捕获 `std::exception`，未知异常仍会逃逸。
4. `EverythingFileListQuery::WorkerMain`、`MySqlSyncService::WorkerLoop`、扫描发现子线程和媒体处理子线程没有统一的最终异常边界。
5. `ScanCoordinator::WorkerMain` 只捕获 `std::exception` 且丢失 `what()` 内容，无法形成完整诊断链。
6. `VideoSc.dll` 暴露 8 个 C 接口、`DiskInfo.dll` 暴露 3 个 C 接口；当前没有统一 ABI 异常屏障。即使函数体未显式 `throw`，`std::string`、`std::vector`、`std::filesystem` 等仍可能抛出异常。
7. 现有日志分别面向扫描错误和删除审计，缺少应用级异常日志，也没有异常 ID 用于关联 UI 提示、日志和转储。
8. 异步任务通过多个布尔值维护 `running/resultReady`；异常路径若提前退出，可能导致界面永久显示“运行中”。

## 3. 异常分层与处理原则

| 层级 | 典型故障 | 处理方式 | 是否继续运行 |
| --- | --- | --- | --- |
| 业务失败 | 文件打不开、媒体不支持、数据库暂时不可用 | 返回现有状态码或任务结果，记录 warning/error | 是 |
| 可恢复 C++ 异常 | `std::runtime_error`、`std::bad_alloc` 之外的一般标准异常 | 在任务边界捕获、清理任务状态、记录上下文并提示用户 | 仅停止当前任务 |
| 未知 C++ 异常 | `catch (...)` 捕获到的非标准异常 | 记录为 unknown_exception，停止当前任务，不猜测异常内容 | 视边界状态决定 |
| 资源耗尽 | `std::bad_alloc`、句柄/磁盘空间耗尽 | 使用低分配路径记录；停止新任务，必要时退出 | 通常不继续重负载任务 |
| 致命进程异常 | 访问冲突、非法指令、堆/栈损坏、`std::terminate` | 写入最小诊断文件和 dump，立即结束进程 | 否 |

以下故障禁止“捕获后继续运行”：访问冲突、堆损坏、栈缓冲区越界、安全检查失败、非法指令。此时进程内状态已不可信，继续保存配置、关闭数据库或释放复杂对象可能造成二次损坏。

## 4. 总体设计

### 4.1 应用级异常日志

在 `DedupCore/logging` 新增 `ApplicationErrorLogger`，复用 `LoggingConfig` 的目录、大小、数量和保留期配置，写入：

```text
<logging.directory>\application-errors.jsonl
```

每条记录至少包含：

- `exception_id`：一次异常的唯一标识，用于关联 UI、日志和 dump。
- `utc_ms`、`process_id`、`thread_id`。
- `severity`、`category`、`module`、`operation`。
- 标准异常类型和 `what()`；未知异常只写固定标记。
- Win32/HRESULT/FFmpeg/MySQL 等原生错误码（有值时）。
- 当前扫描 ID、报告类型、文件路径等允许记录的业务上下文。
- 普通运行上下文中采集的调用栈地址。

日志必须显式排除 MySQL 密码、DPAPI 密文、完整配置 JSON 和内存内容。路径属于诊断数据，可以保留，但需要在设置界面说明。

### 4.2 GUI 进程致命故障处理器

在 `VideoScGUI/diagnostics` 新增 `CrashHandler.h/.cpp`：

1. `wWinMain` 的第一段可执行逻辑安装处理器，早于 COM、窗口、D3D、ImGui 和业务配置初始化。
2. 注册 `SetUnhandledExceptionFilter` 与 `std::set_terminate`。
3. 注册 `_set_purecall_handler`、`_set_invalid_parameter_handler`；根据最终范围决定是否处理 `SIGABRT`。
4. 致命路径只使用预先准备的路径缓冲、Win32 文件 API 和 `MiniDumpWriteDump`，避免使用 JSON、iostream、锁和大规模堆分配。
5. 生成同名的 `.dmp` 与 `.txt` 元数据，例如：

```text
<logging.directory>\crash\VideoScGUI-20260715-154131-p1234-t5678.dmp
<logging.directory>\crash\VideoScGUI-20260715-154131-p1234-t5678.txt
```

6. 配置加载前使用 `<VideoScGUI.exe目录>\logs\crash`；配置加载后更新为 `logging.directory\crash`。若目标目录不可写，再回退到 `%TEMP%\VideoScGUI\crash`。
7. `.txt` 仅写异常码、地址、线程、进程、版本/构建、当前操作和 dump 路径；不在已损坏进程中做符号解析。
8. dump 写完后恢复默认处理并终止，不弹出依赖 ImGui 的窗口。下次启动时由正常 UI 检测上次崩溃并提示文件位置。

`VideoScGUI.vcxproj` 增加 `dbghelp.lib`，并将新源文件纳入工程。

### 4.3 GUI 主线程边界

重构 `wWinMain` 的生命周期，使资源清理由 RAII 对象负责：

- COM 初始化结果与 `CoUninitialize` 成对。
- 窗口类、HWND、`RenderBackend` 和 `VideoScApp` 在正常返回与标准异常返回时均按固定顺序释放。
- 初始化和消息循环外层捕获 `std::bad_alloc`、`std::exception`、`catch (...)`。
- 可恢复的标准异常写入 `application-errors.jsonl`，显示 Win32 `MessageBoxW` 并返回非零退出码。
- 不在 `WndProc` 内吞掉致命异常；Win32 回调边界仅防止 C++ 异常跨系统回调传播，并将异常升级给进程致命处理路径。

### 4.4 后台线程最终边界

所有线程入口遵循同一模板：

1. 最外层 `try` 包住线程全部逻辑。
2. `catch (const std::bad_alloc&)`、`catch (const std::exception&)`、`catch (...)` 分级处理。
3. 使用作用域结束器保证 `running=false`、`resultReady=true`、条件变量通知和必要的状态清理始终执行。
4. 将异常转换为该任务现有结果结构；不得从工作线程直接操作 ImGui 或弹 Win32 窗口。
5. 记录线程名/操作名，例如 `database_init`、`duplicate_report`、`permanent_delete`、`everything_query`、`scan_discovery`、`media_analysis`、`mysql_sync`。
6. 多工作线程中的第一个异常写入共享失败状态并请求取消，其他线程只负责退出，避免同一故障产生大量重复日志。

首批必须覆盖：

- `VideoScApp` 的数据库初始化、报告生成、永久删除线程。
- `EverythingFileListQuery::WorkerMain`。
- `ScanCoordinator::WorkerMain`、发现线程和媒体处理线程。
- `DiskHashScheduler` 的磁盘工作线程和完成回调。
- `MySqlSyncService::WorkerLoop`。

### 4.5 自研 DLL C ABI 异常屏障

`VideoSc.dll` 和 `DiskInfo.dll` 的每个导出函数必须满足：

1. 输出结构或输出缓冲先初始化为失败状态，再进入可能抛异常的逻辑。
2. 捕获 `std::bad_alloc`、`std::exception` 和 `catch (...)`，映射为稳定的 API 错误码。
3. 释放函数保证空指针安全，且具有不抛异常语义。
4. 不使用 `/EHa` 和 `catch (...)` 恢复访问冲突；C++ ABI 屏障只处理 C++ 异常，SEH 仍交由进程级致命处理器。
5. 在公共头文件中补充 `UNEXPECTED_FAILURE`/`OUT_OF_MEMORY` 状态码和注释，不改变现有结构体布局，保持二进制兼容。
6. GUI/DedupCore 调用方收到新增状态码时，将模块名、操作名和输入上下文写入应用异常日志。

本解决方案中可修改源码的动态库只有以下两个；`DedupCore` 的 `ConfigurationType` 为 `StaticLibrary`，其异常由最终链接进程的线程/任务边界处理。

#### `VideoSc.dll`

覆盖全部 8 个导出函数：

- `CaptureVideoScreenshots`
- `FreeVideoScResult`
- `ComputeFileSHA512`
- `ComputeFileSHA512Ex`
- `AnalyzeMediaFile`
- `FreeVideoScMediaResult`
- `ComputeImageDHash`
- `ComputeHammingDistance`

具体处理：

1. 所有结果结构在进入实现前清零并写入默认失败状态，保证中途异常也不会把未初始化内存交给调用方。
2. 新增统一的内部错误结果构造函数，处理错误消息分配失败的情况，避免异常处理路径再次抛出。
3. `std::bad_alloc` 映射为内存不足；其他标准/未知异常映射为 `VIDEOSC_ERR_UNEXPECTED_FAILURE`。
4. SHA-512、媒体分析和 dHash 分别补充与现有状态码体系一致的异常状态，不复用“文件打不开”等不准确状态。
5. `FreeVideoScResult`、`FreeVideoScMediaResult` 按不抛异常契约实现，处理空指针、部分初始化结果和重复清理后的零状态。
6. FFmpeg 的错误返回值继续按现有业务状态处理；FFmpeg 内部访问冲突等原生致命故障不在 DLL 内恢复，交给宿主进程崩溃处理器。

#### `DiskInfo.dll`

覆盖全部 3 个导出函数：

- `GetPhysicalDiskNumber`
- `QueryDiskTopology`
- `QueryFilePhysicalLocation`

具体处理：

1. `DiskTopologyInfo`、`FilePhysicalLocation` 在任何参数转换或 Win32 调用前初始化。
2. 标准/未知 C++ 异常映射为新增的 unexpected failure 状态，同时保留已有 Win32 错误字段。
3. 路径编码失败、卷不存在、句柄打开失败、IOCTL 查询失败继续使用各自现有错误码，不被笼统异常覆盖。
4. 所有 Win32 `HANDLE` 改由现有或最小 RAII 封装持有，异常路径也能关闭；不在 `DllMain` 中执行日志、加锁、创建线程或写 dump。

### 4.6 第三方 DLL 调用边界

以下 DLL/组件没有本项目源码，不修改其内部异常处理；处理范围是加载验证、调用返回值、回调边界、超时/取消和宿主进程致命故障记录。

| DLL/组件 | 本项目处理位置 | 异常与错误策略 |
| --- | --- | --- |
| FFmpeg：`avformat`、`avcodec`、`avutil`、`swscale` 等 | `VideoSc.dll` | 检查所有指针和负返回码；保存 FFmpeg 阶段/错误码；超时、取消和不支持格式返回稳定状态；访问冲突只生成宿主 dump，不尝试继续解码 |
| `Everything64.dll` | `EverythingFileDiscovery`、`EverythingFileListQuery` | 校验 `LoadLibraryW`/`GetProcAddress`、SDK 版本、查询状态和结果数量；动态调用外层捕获 C++ 异常；DLL 崩溃交给进程级处理器 |
| MySQL 客户端 DLL | `MySqlClient`、`MySqlSyncService` | 检查 native error、断线和超时；工作线程最终异常边界；失败批次保留重试状态，不记录密码 |
| RocksDB DLL/静态依赖 | `RocksStore`、报告/扫描/同步服务 | 将 `Status` 转为业务结果；调用点可能抛出的 C++ 异常在事务/任务边界捕获；失败时不发布半成品报告或确认未同步队列 |
| D3D11/DXGI | `RenderBackend` | 检查 `HRESULT`、设备丢失和交换链失败；可重建资源时重建，不可恢复时记录并退出 GUI |
| DbgHelp | `CrashHandler` | 动态或静态加载失败时仍写 `.txt` 元数据；不得因 dump 生成失败覆盖原始异常 |

第三方 DLL 的 `DllMain`、内部线程和内部内存错误无法由本项目改写。若第三方 DLL 触发 SEH/fail-fast，只能由宿主进程或独立崩溃报告进程记录，不能安全恢复。

### 4.7 用户提示与再次启动

- 当前任务可恢复失败：沿用各功能区域现有错误文本，并附加短异常 ID，例如 `ERR-20260715-...`。
- 主线程标准异常：使用 `MessageBoxW` 显示“程序遇到错误并将退出”、异常 ID 和日志目录。
- 致命崩溃：当前进程不尝试复杂 UI；下次启动检测未读 crash 元数据，弹出“上次运行异常退出”窗口，提供“打开诊断目录”和“忽略”按钮。
- 配置界面增加只读的诊断目录显示和“打开诊断目录”按钮；是否增加“生成测试异常”按钮仅限 Debug 构建。

## 5. 明确不做的错误处理

1. 不把 `catch (...)` 添加到每个普通函数；只在进程、线程、第三方回调和 C ABI 边界捕获。
2. 不将正常的“不支持格式”“文件被占用”“网络暂时断开”升级为崩溃。
3. 不在致命异常处理器中保存 `config.json`、关闭 RocksDB/MySQL、刷新 ImGui 或执行复杂析构。
4. 不记录数据库密码、DPAPI 密文或完整进程内存到文本日志。
5. 不声称进程内处理器能捕获所有终止场景。`TerminateProcess`、断电、内核崩溃以及某些 fail-fast/安全检查失败可能绕过普通未处理异常过滤器。

## 6. 预计修改文件

### 新增

- `VideoScGUI/diagnostics/CrashHandler.h`
- `VideoScGUI/diagnostics/CrashHandler.cpp`
- `DedupCore/logging/ApplicationErrorLogger.h`
- `DedupCore/logging/ApplicationErrorLogger.cpp`
- `DedupTests` 下的异常日志测试；如确认外部崩溃捕获，再新增独立辅助进程工程

### 修改

- `VideoScGUI/main.cpp`
- `VideoScGUI/VideoScApp.h/.cpp`
- `VideoScGUI/EverythingFileListQuery.cpp`
- `VideoScGUI/RenderBackend.cpp`（仅 Win32 回调边界和错误传播）
- `DedupCore/orchestration/ScanCoordinator.cpp`
- `DedupCore/scheduling/DiskHashScheduler.cpp`
- `DedupCore/persistence/MySqlSyncService.cpp`
- `VideoSc/VideoSc.h`、`VideoSc/dllmain.cpp`
- `DiskInfo/DiskInfo.h`、`DiskInfo/DiskInfo.cpp`
- `DedupCore/discovery/EverythingFileDiscovery.cpp`（第三方 DLL 加载及调用边界）
- `DedupCore/persistence/MySqlClient.cpp`、`DedupCore/persistence/RocksStore.cpp`（第三方返回码与上下文记录）
- `VideoScGUI/VideoScGUI.vcxproj`
- `DedupCore/DedupCore.vcxproj`
- `VideoSc/VideoSc.vcxproj`、`DiskInfo/DiskInfo.vcxproj`（若测试或共享屏障需要）
- `DedupTests/DedupTests.vcxproj`、`DedupTests/main.cpp`

## 7. 实施顺序

1. 增加应用异常日志及滚动测试。
2. 增加 GUI 进程崩溃处理器、dump 元数据和日志目录回退。
3. 改造 `wWinMain` 生命周期和主线程异常边界。
4. 补齐全部后台线程入口、异步状态结束器和错误回传。
5. 为 `VideoSc.dll`、`DiskInfo.dll` 全部导出函数增加 C ABI 屏障和稳定错误码。
6. 补齐 FFmpeg、Everything、MySQL、RocksDB、D3D11 等第三方 DLL 的加载、返回码、回调和上下文记录边界。
7. 增加上次崩溃提示及诊断目录入口。
8. 通过子进程故障注入验证，不让测试进程自身崩溃。

## 8. 测试与验收

### 8.1 自动测试

1. 并发写入 `application-errors.jsonl` 时记录不交叉、不丢字段。
2. 达到大小阈值后按配置滚动，目录不可写时返回明确错误且不抛异常。
3. 标准异常、未知异常和内存不足分别映射到正确任务状态/API 状态码。
4. 所有 DLL 导出函数对空参数、非法 UTF-8 和异常注入均不让异常跨 ABI。
5. 子进程触发未处理 C++ 异常、`std::terminate` 和访问冲突后，父测试进程验证非零退出码、`.txt` 和 `.dmp` 是否生成。
6. 日志内容不包含注入的测试密码或 DPAPI 密文。

### 8.2 手工验收

1. 数据库初始化、报告、删除、Everything 查询、扫描子线程任一位置抛出标准异常时，仅当前任务失败，GUI 仍可继续操作。
2. 异步失败后所有“运行中”状态均能复位，可再次发起任务。
3. 主线程标准异常会写日志、显示异常 ID 并正常退出。
4. 访问冲突不会被当作可恢复异常继续运行；能生成 dump 时生成后退出。
5. 再次启动能发现上次崩溃，并可打开对应诊断目录。
6. `Debug|x64` 和 `Release|x64` 解决方案构建成功，现有 `DedupTests` 不回归。

## 9. 能力边界与可选增强

仅使用进程内 `SetUnhandledExceptionFilter + MiniDumpWriteDump` 时，普通未处理 SEH 和 `std::terminate` 可覆盖，但某些 `0xC0000409` fail-fast、严重堆损坏和进程被强制终止的场景不保证进入处理器。

若要求尽可能捕获这类“请求了严重的程序退出”，需要额外选择一种进程外方案：

- 新增独立 `VideoScCrashReporter.exe` 监视主进程并在异常终止时保存转储；工程和发布物会增加。
- 使用 Windows Error Reporting LocalDumps；实现较少，但需要安装/注册表部署权限，转储路径也受系统策略影响。

两种方案都只能记录故障，不能让已发生安全检查失败的进程继续运行。

## 10. 已执行口径

1. 已确认：覆盖 `VideoScGUI.exe`、`VideoSc.dll`、`DiskInfo.dll`，并补齐 FFmpeg、Everything、MySQL、RocksDB 等第三方 DLL 的调用方边界；不修改第三方二进制内部实现。
2. 已采用小型转储，不生成可能包含更多敏感内存数据的完整内存转储。
3. 已新增独立崩溃报告进程，用于补充覆盖可能绕过进程内过滤器的 fail-fast。
4. 已在设置界面增加“打开诊断目录”，并在启动时显示未确认的上次异常提示。

## 11. 执行结果

已按推荐口径完成：

1. 新增 `ApplicationErrorLogger`，统一写入 `application-errors.jsonl`，记录异常 ID、进程、线程、模块、操作、异常类型、原生错误码和有限调用栈地址，并复用现有滚动配置。
2. 新增 GUI 进程内 `CrashHandler`，覆盖未处理 SEH、`std::terminate`、纯虚函数调用、CRT 非法参数和 `SIGABRT`，生成小型 `.dmp` 与 UTF-16 `.txt` 元数据。
3. 新增独立 `VideoScCrashReporter.exe`，以调试事件监视主进程并在二次异常时生成外部转储，用于补充捕获 fail-fast。
4. `wWinMain` 已改为 RAII 生命周期，COM、窗口、D3D/ImGui 和业务对象在标准异常路径按顺序清理；主线程异常写日志、显示异常 ID并返回非零退出码。
5. 数据库初始化、报告生成、永久删除、Everything 查询、扫描协调、扫描发现、媒体分析、磁盘哈希和 MySQL 同步线程均增加最终异常边界与运行状态复位。
6. `VideoSc.dll` 全部 8 个导出接口增加 C ABI 包装层，新增内存不足/未知异常状态，并补齐 FFmpeg 分配返回值检查。
7. `DiskInfo.dll` 全部 3 个导出接口增加 C ABI 包装层和 unexpected failure 状态；关键 Win32 句柄改为 RAII，异常路径不会泄漏句柄。
8. 设置界面增加诊断目录和“打开诊断目录”；下次启动检测到未确认崩溃时显示提示弹窗，可打开目录或标记已查看。
9. 新增并发应用异常日志测试与 DLL 输出初始化/错误契约测试，测试数量由 29 增至 31。

验证结果：

- `Debug|x64` 全量解决方案构建成功。
- `Release|x64` 全量解决方案构建成功。
- Debug `DedupTests.exe`：`31/31 passed`。
- Release `DedupTests.exe`：`31/31 passed`。
- Debug 隔离故障探针调用 `RaiseFailFastException` 后，目标子进程以 `0xC0000602` 终止；外部报告进程正常退出并生成约 26 KiB `.dmp` 和配套 `.txt`。
- 故障探针输出目录已在验证后清理。
- 未自动启动桌面 GUI；未主动让 `VideoScGUI.exe` 发生真实崩溃。
