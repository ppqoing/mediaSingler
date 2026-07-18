# `ComputeImageDHash` 严重程序退出排查结论

> 排查日期：2026-07-15  
> 状态：已按最小修复方案完成修改并通过回归测试

## 1. 结论

`dllmain.cpp:1237` 的严重程序退出不是 GUI、D3D11 或线程析构问题，而是 `ComputeImageDHash` 把尚未完成首帧解码的 `AVCodecContext` 参数直接传给 `sws_getContext`。

当输入图片损坏、截断或无法从容器头确定尺寸/像素格式时，`cc->width`、`cc->height` 或 `cc->pix_fmt` 可能分别为 `0`、`0`、`AV_PIX_FMT_NONE`。当前代码没有校验这些值，也没有先解码首帧，导致 libswscale 内部断言触发 `abort()`。该退出不是 C++ 异常，调用方无法通过 `try/catch` 恢复，最终由 Visual Studio 显示为 `ucrtbase.dll` 中“请求了严重的程序退出”。

## 2. 稳定复现证据

使用当前 `x64\Debug\VideoSc.dll`，把每张图片放在独立 PowerShell 子进程中调用 `ComputeImageDHash`：

- 原始 `1.jpeg`、`1.jpg`、`1.png`、`1.webp`：返回成功。
- 生成的 JPEG、PNG、WebP、GIF 和水印图片：返回成功。
- `04_bmp_1280x720.bmp`：函数安全返回失败，子进程退出码为 `2`。
- `05_tiff_1280x720.tiff`：子进程以 `-1073740791` 退出，对应 `0xC0000409`，稳定复现严重程序终止。

触发文件：

`E:\Code\media\generated_test_media\image\formats\05_tiff_1280x720.tiff`

- 文件大小：262144 字节
- SHA-256：`ca3b3c2268115f6d3d60a1b07965d02af172202153c6e442e60e770688c6e8e7`
- FFprobe：`width=0`、`height=0`、`pix_fmt=unknown`
- TIFF 解码错误：`IFD offset is greater than image size`

另一个损坏文件：

`E:\Code\media\generated_test_media\image\formats\04_bmp_1280x720.bmp`

- 文件大小：1310720 字节
- SHA-256：`13db805b64b3749b5627c31653ec977896bc8f42cf0c4fbd43f9597ae70c3b2b`
- FFmpeg 报告文件数据不足，无法解码任何帧。

这两个文件来自媒体生成脚本首次运行被 PowerShell stderr 误判中断后的非零部分输出。它们不是完整的 BMP/TIFF 文件。

## 3. 代码证据链

1. `ScanCoordinator::AnalyzeMedia` 根据扩展名把 TIFF/BMP 分类为图片，并在 `ScanCoordinator.cpp:833` 调用 `AnalyzeMediaFile`。
2. `AnalyzeMediaFile` 的图片分支在 `dllmain.cpp:663-670` 调用 `ComputeImageDHash`。
3. `ComputeImageDHash` 只执行 `avcodec_open2`，尚未收到任何解码帧。
4. `dllmain.cpp:1234` 直接读取 `cc->width`、`cc->height`。
5. `dllmain.cpp:1237-1239` 直接把 `cc->pix_fmt` 和未校验尺寸传给 `sws_getContext`。
6. 对损坏 TIFF，流参数实际为宽高 `0x0`、像素格式未知，libswscale 内部断言终止整个进程。

项目中的视频分析路径已经采用更安全的模式：`dllmain.cpp:736-757` 在成功解码 `AVFrame` 后，使用 `frame->width`、`frame->height`、`frame->format` 创建缓存转换器。图片 dHash 应采用同一原则。

## 4. 触发条件

满足以下任一条件的图片都可能触发或放大该缺陷：

- 文件截断或尾部未写完。
- 容器头不能提供有效宽高。
- 解码器只有收到首个数据包后才能确定像素格式。
- `avcodec_parameters_to_context` 后 `cc->pix_fmt == AV_PIX_FMT_NONE`。
- 编码格式存在动态像素格式或损坏的元数据。

因此，删除当前 TIFF 只能绕过本次触发，不能消除生产代码处理其他坏图时的进程级崩溃风险。

## 5. 最小修复方案

只修改 `ComputeImageDHash`，不改 ABI、扫描器、数据库或 GUI：

1. 检查 `avcodec_alloc_context3`、`avcodec_parameters_to_context`、`av_frame_alloc`、`av_packet_alloc` 的返回值。
2. 先读取数据包并成功取得第一张 `AVFrame`。
3. 使用 `frame->width`、`frame->height`、`static_cast<AVPixelFormat>(frame->format)` 作为真实源参数。
4. 在调用 swscale 前校验宽高大于 0、像素格式不是 `AV_PIX_FMT_NONE`，并检查 `sws_isSupportedInput`。
5. 首帧成功后再调用 `sws_getCachedContext` 创建 9×8 灰度转换器。
6. 检查 `sws_scale` 返回值是否等于 `DHASH_H`；失败时释放资源并返回 `0`。
7. 所有失败路径把 `outHash[0]` 置为 `\0`，保证调用方不会读取旧结果。
8. 保持损坏图片返回失败，由现有 `AnalyzeMediaFile` 转换为 `VIDEOSC_ERR_DECODE_FAILED`，不得让 FFmpeg 断言退出进程。

## 6. 验证计划

1. 使用当前损坏 TIFF 回归：修复前子进程退出 `0xC0000409`，修复后函数返回 `0` 且进程正常退出。
2. 使用损坏 BMP 回归：继续安全返回 `0`。
3. 使用原始 JPG、JPEG、PNG、WebP 及生成的 GIF 回归：dHash 继续成功。
4. 使用完整重新生成的 BMP、TIFF 回归：应成功计算 dHash。
5. 运行现有媒体分析测试，确认图片 dHash、视频六帧 dHash 和 2×3 拼图没有回归。

## 7. 当前临时处理

- 若只需要继续扫描，可暂时排除上述损坏 BMP/TIFF。
- 若要重建完整测试集，应使用媒体生成脚本的 `-Overwrite` 重新生成，避免默认跳过这两个非零但损坏的文件。
- 即使重建测试文件，仍建议执行第 5 节代码修复，保证程序遇到未来的损坏图片时只记录失败而不退出整个进程。

## 8. 执行结果

已完成以下修改：

1. `ComputeImageDHash` 在所有有效调用开始时清空输出缓冲区。
2. 图片首帧解码成功后，才使用 `AVFrame` 的真实宽、高和像素格式创建 swscale 转换器。
3. 增加解码器、帧、数据包、像素格式支持和 `sws_scale` 返回值校验，所有失败路径统一释放资源并返回 `0`。
4. 新增截断 TIFF 回归用例，验证损坏图片返回失败、输出为空且测试进程继续运行。
5. 修正 `DedupTests` 的 Debug RocksDB 链接库映射：Debug 使用 `rocksdb-sharedd.lib`，非 Debug 使用 `rocksdb-shared.lib`。

验证结果：

- `Debug|x64` 的 `VideoSc.dll` 与 `VideoScGUI.exe` 重建成功。
- `DedupTests.exe` 构建成功，结果为 `29/29 passed`。
- 原始触发文件 `05_tiff_1280x720.tiff` 独立调用结果为 `RESULT=0`、`HASH=''`、`PROCESS_SURVIVED=true`，进程正常退出码为 `0`。
- 原异常退出码 `0xC0000409` 未再出现。
