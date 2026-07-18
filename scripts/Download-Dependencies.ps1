<#
.SYNOPSIS
把 VideoSc 的 vcpkg 依赖手动下载到指定缓存目录。

.DESCRIPTION
默认只下载源文件，不编译、不安装。vcpkg 仍负责从固定 baseline 解析所有直接和传递依赖，
每个资产实际交给 Invoke-VcpkgAssetDownload.ps1 下载并校验 SHA-512。

安装 aria2 后可进行多连接下载；未安装时自动使用 Windows 自带 curl.exe。
下载中断后可重复运行，已通过哈希校验的缓存会直接复用。

.EXAMPLE
powershell -ExecutionPolicy Bypass -File .\scripts\Download-Dependencies.ps1

.EXAMPLE
powershell -ExecutionPolicy Bypass -File .\scripts\Download-Dependencies.ps1 `
  -Aria2Path D:\Tools\aria2\aria2c.exe -Connections 16 -Proxy http://127.0.0.1:7890

.EXAMPLE
powershell -ExecutionPolicy Bypass -File .\scripts\Download-Dependencies.ps1 -Install
#>
[CmdletBinding()]
param(
    [string]$VcpkgPath = 'E:\vcpkg\vcpkg.exe',

    [string]$DownloadsDirectory = '',

    [string]$InstallRoot = '',

    [string]$Proxy = '',

    [string]$Aria2Path = '',

    [ValidateRange(1, 64)]
    [int]$Connections = 16,

    [switch]$Install,

    [switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$manifestPath = Join-Path $projectRoot 'vcpkg.json'
$assetScript = Join-Path $PSScriptRoot 'Invoke-VcpkgAssetDownload.ps1'

if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw "未找到 vcpkg.json：$manifestPath"
}
if (-not (Test-Path -LiteralPath $VcpkgPath -PathType Leaf)) {
    throw "未找到 vcpkg：$VcpkgPath。请通过 -VcpkgPath 指定实际路径。"
}
if (-not (Test-Path -LiteralPath $assetScript -PathType Leaf)) {
    throw "未找到资产下载脚本：$assetScript"
}

if ([string]::IsNullOrWhiteSpace($DownloadsDirectory)) {
    $DownloadsDirectory = Join-Path $projectRoot '.downloads\vcpkg'
}
if ([string]::IsNullOrWhiteSpace($InstallRoot)) {
    $InstallRoot = 'E:\vcpkg\installed'
}
$DownloadsDirectory = [IO.Path]::GetFullPath($DownloadsDirectory)
$InstallRoot = [IO.Path]::GetFullPath($InstallRoot)
[IO.Directory]::CreateDirectory($DownloadsDirectory) | Out-Null
[IO.Directory]::CreateDirectory($InstallRoot) | Out-Null

$powerShellPath = Join-Path $PSHOME 'powershell.exe'
if (-not (Test-Path -LiteralPath $powerShellPath -PathType Leaf)) {
    throw "未找到 Windows PowerShell：$powerShellPath"
}

# vcpkg 会对 {url}/{sha512}/{dst} 做安全转义；固定脚本参数只在此处拼装一次。
$templateParts = @(
    "`"$powerShellPath`""
    '-NoLogo'
    '-NoProfile'
    '-NonInteractive'
    '-ExecutionPolicy Bypass'
    "-File `"$assetScript`""
    '-Url {url}'
    '-Sha512 {sha512}'
    '-Destination {dst}'
    "-Connections $Connections"
)
if (-not [string]::IsNullOrWhiteSpace($Proxy)) {
    $templateParts += "-Proxy `"$Proxy`""
}
if (-not [string]::IsNullOrWhiteSpace($Aria2Path)) {
    $resolvedAria2 = [IO.Path]::GetFullPath($Aria2Path)
    if (-not (Test-Path -LiteralPath $resolvedAria2 -PathType Leaf)) {
        throw "指定的 aria2c 不存在：$resolvedAria2"
    }
    $templateParts += "-Aria2Path `"$resolvedAria2`""
}
$assetSource = 'x-script,' + ($templateParts -join ' ')

$arguments = @(
    'install'
    '--triplet=x64-windows'
    "--x-manifest-root=$projectRoot"
    "--x-install-root=$InstallRoot"
    "--downloads-root=$DownloadsDirectory"
    "--x-asset-sources=$assetSource"
)
if (-not $Install) {
    $arguments += '--only-downloads'
}

Write-Host "项目目录：$projectRoot"
Write-Host "下载缓存：$DownloadsDirectory"
Write-Host "安装目录：$InstallRoot"
Write-Host ($(if ($DryRun) { '模式：仅显示命令，不联网' } elseif ($Install) { '模式：下载并安装' } else { '模式：仅下载，不编译' }))
if ([string]::IsNullOrWhiteSpace($Aria2Path) -and $null -eq (Get-Command 'aria2c.exe' -ErrorAction SilentlyContinue)) {
    Write-Warning '当前未找到 aria2c，将使用 curl。需要多连接时请安装 aria2 或传入 -Aria2Path。'
}

if ($DryRun) {
    Write-Host '将执行以下参数：'
    $arguments | ForEach-Object { Write-Host "  $_" }
    Write-Host '命令检查完成，未启动 vcpkg。'
    exit 0
}

& $VcpkgPath @arguments
if ($LASTEXITCODE -ne 0) {
    throw "vcpkg 依赖下载失败，退出码：$LASTEXITCODE。可直接重复运行以断点续传。"
}

if ($Install) {
    Write-Host '依赖下载与安装完成。'
} else {
    Write-Host '依赖源文件下载完成。需要编译安装时重新运行并添加 -Install。'
}
