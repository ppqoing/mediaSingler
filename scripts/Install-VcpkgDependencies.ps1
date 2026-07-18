<#
.SYNOPSIS
使用 E:\vcpkg 安装本项目 vcpkg.json 中缺少的依赖包。

.DESCRIPTION
以 manifest 模式运行 vcpkg install，自动解析 vcpkg.json 中的直接与传递依赖，
仅安装尚未就绪的包，结果输出到 E:\vcpkg\installed\ 下，供各 vcxproj 引用。

默认 triplet 为 x64-windows，与 DedupCore/VideoScGUI 等工程的 VcpkgTriplet 一致。

.EXAMPLE
powershell -ExecutionPolicy Bypass -File .\scripts\Install-VcpkgDependencies.ps1

.EXAMPLE
powershell -ExecutionPolicy Bypass -File .\scripts\Install-VcpkgDependencies.ps1 -Triplet x64-windows-static

.EXAMPLE
powershell -ExecutionPolicy Bypass -File .\scripts\Install-VcpkgDependencies.ps1 -Proxy http://127.0.0.1:7890
#>
[CmdletBinding()]
param(
    [string]$VcpkgPath = 'E:\vcpkg\vcpkg.exe',

    [ValidateSet('x64-windows', 'x64-windows-static', 'x64-windows-static-md')]
    [string]$Triplet = 'x64-windows',

    [string]$Proxy = '',

    [switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$projectRoot  = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$manifestPath = Join-Path $projectRoot 'vcpkg.json'
$installRoot  = 'E:\vcpkg\installed'

if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw "未找到 vcpkg.json：$manifestPath"
}
if (-not (Test-Path -LiteralPath $VcpkgPath -PathType Leaf)) {
    throw "未找到 vcpkg：$VcpkgPath。请通过 -VcpkgPath 指定实际路径。"
}

$installRoot = [IO.Path]::GetFullPath($installRoot)
[IO.Directory]::CreateDirectory($installRoot) | Out-Null

Write-Host "项目目录：$projectRoot"
Write-Host "vcpkg   ：$VcpkgPath"
Write-Host "Triplet ：$Triplet"
Write-Host "安装目录：$installRoot"

# 设置下载代理：vcpkg 内部用 curl/cmake 读取 HTTP(S)_PROXY。
# 注意：即使代理是 HTTPS 服务器，前缀也必须用 http://（除非代理本身是 HTTPS 服务器）。
if (-not [string]::IsNullOrWhiteSpace($Proxy)) {
    $env:HTTP_PROXY  = $Proxy
    $env:HTTPS_PROXY = $Proxy
    Write-Host "代理    ：$Proxy"
}

$arguments = @(
    'install'
    "--triplet=$Triplet"
    "--x-manifest-root=$projectRoot"
    "--x-install-root=$installRoot"
)

if ($DryRun) {
    Write-Host '将执行以下命令（DryRun，不实际安装）：'
    Write-Host "  $VcpkgPath $($arguments -join ' ')"
    return
}

& $VcpkgPath @arguments
if ($LASTEXITCODE -ne 0) {
    throw "vcpkg 安装失败，退出码：$LASTEXITCODE。"
}

Write-Host "依赖安装完成，输出目录：$installRoot"
