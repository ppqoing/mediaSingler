# DedupBenchmarks

独立候选资源基准，不连接 MySQL，也不扫描媒体目录。

```powershell
.\x64\Release\DedupBenchmarks.exe 1000000
.\x64\Release\DedupBenchmarks.exe 10000000
```

- 100 万默认构造相同热门签名，验证在平方展开前转入人工复核。
- 1000 万在 512 MiB 配置下应于记录分配前拒绝，验证预算前置门禁。

退出码 0 表示保护行为符合预期；输出包含记录数、耗时、峰值估算、候选对和延迟签名数。
