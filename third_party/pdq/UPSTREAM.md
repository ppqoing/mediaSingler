# Meta PDQ 上游说明

- 上游仓库：`https://github.com/facebook/ThreatExchange`
- 固定提交：`4b98d786c3b9c40e62cc31fe20f7e6d9fe729757`
- 上游目录：`pdq/cpp`
- 许可证：BSD License，见同目录 `LICENSE`

本项目只引入计算 PDQ-256 所需的基础类型、downscaling 与 hashing 源码，不引入 `CImg.h`、命令行工具和图片 IO。图片解码与固定尺寸灰度预处理继续使用项目已有 FFmpeg。

