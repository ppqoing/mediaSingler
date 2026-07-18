# vcpkg 依赖路径调整计划

## 1. 修改目标

将本项目使用的 vcpkg 根目录统一为 `E:\vcpkg`：

- vcpkg 可执行文件：`E:\vcpkg\vcpkg.exe`
- 已安装依赖目录：`E:\vcpkg\installed`

## 2. 修改范围

1. 新增解决方案级 MSBuild 属性，集中定义 vcpkg 根目录和依赖安装目录。
2. 将 `DedupCore`、`VideoScGUI`、`DedupTests` 的头文件、库文件、运行库和工具查找路径切换到统一属性，并匹配目标目录中的 Debug/Release 运行库文件名。
3. 将依赖安装脚本和依赖下载脚本的默认 vcpkg 可执行文件、安装目录切换到 `E:\vcpkg`。
4. 同步更新依赖下载说明中的默认目录。

## 3. 保持不变

- 不修改 `vcpkg.json` 中的依赖清单和版本信息。
- 不修改默认 triplet，继续使用 `x64-windows`。
- 不修改项目内 FFmpeg、Everything SDK 等非 vcpkg 依赖路径。
- 不执行依赖下载、安装或项目编译。

## 4. 完成检查

1. 静态扫描项目，确认可执行配置不再引用原有项目内安装目录或旧 vcpkg 工具目录。
2. 确认工程依赖路径最终解析到 `E:\vcpkg\installed\x64-windows`。
3. 检查修改后的 MSBuild XML 和 PowerShell 脚本语法。
