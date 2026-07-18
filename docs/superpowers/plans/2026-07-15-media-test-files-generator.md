# 多格式媒体测试文件生成脚本计划

> 状态：首次实际运行问题已修复，等待重新执行完整生成

## 1. 当前环境

- FFmpeg：`E:\Code\media\ffmpeg\ffmpeg.exe`
- FFprobe：`E:\Code\media\ffmpeg\ffprobe.exe`
- 原始媒体目录：`E:\Code\media`
- 已有样本：AVI、MKV、MP4、WMV、MP3、JPEG、JPG、PNG、WebP
- FFmpeg 版本：7.1.1，已确认支持 H.264、H.265、AV1、VP9、MPEG-2、MPEG-4、WMV2、ProRes、AAC、MP3、Opus、Vorbis、FLAC、WebP 等编码器，以及 `drawtext`、`overlay`、`scale` 等滤镜。

## 2. 已确认的默认生成方案

- 脚本位置：`scripts\Generate-MediaTestFiles.ps1`
- 输出目录：`E:\Code\media\generated_test_media`
- 视频测试片段默认截取前 8 秒，避免完整组合产生过大的测试数据。
- 采用精选矩阵而不是所有维度的笛卡尔积，实际规划生成 48 个文件。
- 原始媒体只读；输出目录默认不覆盖已有文件，显式传入 `-Overwrite` 后才允许覆盖。

## 3. 计划生成的测试集合

### 3.1 视频编码与封装

- H.264：MP4、MKV、MPEG-TS
- H.265：MP4、MKV
- VP9：WebM
- AV1：MKV
- MPEG-4 Part 2：AVI
- MPEG-2：MPG
- WMV2：WMV
- ProRes：MOV

### 3.2 视频分辨率

- 360p、480p、720p、1080p、1440p、2160p
- 统一保持源宽高比，必要时补黑边，确保输出尺寸固定且为偶数。

### 3.3 视频水印

- 左上角普通文字水印
- 居中半透明文字水印
- 右下角时间码水印
- 右上角半透明图片水印
- 平铺文字水印

### 3.4 音频格式

- MP3、AAC/M4A、Opus/OGG、Vorbis/OGG、FLAC、WAV

### 3.5 图片格式、尺寸与水印

- JPEG、PNG、WebP、BMP、TIFF、GIF
- 缩略图、标清、高清、4K 尺寸变体
- 普通文字、半透明文字、图片叠加水印变体

### 3.6 去重对照样本

- 保留少量原文件的字节级复制品，用于验证 SHA-512 精确重复检测。
- 编码、缩放和水印结果作为视觉近似但字节不同的对照样本。

## 4. 脚本设计

1. 参数化 FFmpeg、FFprobe、输入目录、输出目录、视频截取时长和覆盖策略。
2. 自动排除 `ffmpeg` 工具目录和输出目录，避免把生成结果再次当作输入。
3. 使用 FFprobe 识别候选视频、音频和图片，优先选择可稳定解码的现有样本。
4. 使用参数数组调用 FFmpeg，避免 PowerShell 字符串拼接导致路径和滤镜转义错误。
5. 每个生成任务输出清晰的进度、目标文件和失败原因；单项失败时记录汇总，最后以非零退出码报告失败。
6. 生成 `manifest.json`，记录文件名、来源、编码、封装、分辨率、水印类型和生成状态。

## 5. 验收方式

1. 使用 PowerShell 解析器检查脚本语法。
2. 使用 `-DryRun` 检查完整任务矩阵，不生成媒体文件。
3. 不在本次脚本编写阶段执行完整媒体生成；需要时再由用户运行脚本。

## 6. 确认结果

采用上述输出目录、8 秒时长和精选矩阵规模，最终 DryRun 规划 48 个生成任务。

## 7. 实施结果

- 已生成 `scripts\Generate-MediaTestFiles.ps1`。
- Windows PowerShell 5.1 语法检查通过。
- `-DryRun` 成功识别 `1.mp4`、`1.mp3`、`1.webp` 和 `1.png` 作为主视频、主音频、主图片和图片水印来源。
- DryRun 共生成 48 项任务，警告数为 0，退出码为 0。
- DryRun 未创建 `E:\Code\media\generated_test_media`，未执行完整媒体转码。

## 8. 首次实际运行问题修复计划

### 8.1 现场结果

- 首次实际运行后输出目录中存在 22 个文件，其中 9 个为 0 字节文件，且未生成 `manifest.json`。
- 图片和精确复制样本中已有部分非空输出；视频源转码和音频目录中存在失败残留空文件。

### 8.2 根因

1. Windows PowerShell 5.1 在 `$ErrorActionPreference = 'Stop'` 时，会把 FFmpeg 写入 stderr 的普通警告转换成终止错误。FFmpeg 按约定把大量诊断信息写入 stderr，因此脚本在 FFmpeg 退出码可为 0 的情况下提前进入 `catch`。
2. `manifest.json` 组装阶段直接把 `System.Collections.Generic.List[object]` 包进数组，在 Windows PowerShell 5.1 中触发“参数类型不匹配”。
3. JPEG、PNG、BMP、TIFF 等单图片输出未显式使用 image2 的 `-update 1`，FFmpeg 会输出“缺少序列模式”的非致命警告。
4. 失败任务会留下 0 字节目标文件，后续默认运行可能把这些文件误判为已有结果并跳过。

### 8.3 最小修复

1. 仅在调用 FFmpeg 期间临时把 `$ErrorActionPreference` 调整为 `Continue`，以真实 `$LASTEXITCODE` 判断成败，并在调用后立即恢复。
2. manifest 写入前显式枚举记录列表，避免 PowerShell 5.1 泛型列表数组化兼容问题。
3. 对 image2 单图片输出补充 `-update 1`。
4. 已有 0 字节目标文件自动重新生成；真正失败的 FFmpeg 任务删除本次残留目标文件，非空已有文件仍默认跳过。

### 8.4 检查方式

1. 重新执行 Windows PowerShell 5.1 语法检查和 `-DryRun`。
2. 使用工作区内临时输出验证“stderr 警告不等于失败”和 manifest 列表序列化，不自动覆盖用户当前媒体输出目录。
3. 修复完成后由用户使用原命令重新运行；现有 0 字节文件会自动重建，若需确保全部文件重新生成则显式使用 `-Overwrite`。

### 8.5 修复结果

- Windows PowerShell 5.1 语法检查通过。
- 48 项任务 DryRun 通过，警告数为 0，退出码为 0。
- MP3 定向测试成功：FFmpeg 向 stderr 输出 1 条诊断信息，但退出码为 0，生成文件为 25388 字节，脚本不再误判失败。
- PNG 单图片定向测试成功：退出码为 0，生成文件为 15522 字节，不再出现 image2 序列模式警告。
- manifest 泛型列表兼容测试成功，2 条记录可正常 JSON 往返序列化。
- 正式输出目录中的 13 个非空文件最初仅通过 FFprobe 元数据探测；后续逐帧解码确认其中 BMP、TIFF 是首次运行中断留下的截断文件。元数据探测不足以证明媒体可解码，必须使用 `-Overwrite` 重建这两个非零损坏文件；9 个 0 字节文件仍会在重新执行时自动删除并重建。
