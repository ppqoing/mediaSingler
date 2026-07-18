<#
.SYNOPSIS
下载并还原 third_party/ffmpeg 的 Windows 运行库（bin 目录）。

.DESCRIPTION
仓库只保留 third_party/ffmpeg 的头文件（include）与导入库（lib），体积较大的
DLL 与可执行文件（bin）不入库。本脚本下载 BtbN FFmpeg-Builds 的 win64 GPL shared
压缩包，只把其中的 bin 目录还原到 third_party/ffmpeg\bin。

默认下载 master 滚动构建。仓库 include/lib 对应的 ABI 主版本为
avcodec-63 / avdevice-63 / avfilter-12 / avformat-63 / avutil-61 / swresample-7 / swscale-10；
下载完成后会校验该 DLL 集合，主版本漂移时中止并提示同步更新 include/lib，
可用 -IgnoreAbiCheck 强制继续。

下载中断后可重复运行，curl 支持续传。需要代理时显式传入，不写进项目配置。

.EXAMPLE
powershell -ExecutionPolicy Bypass -File .\scripts\Download-ThirdParty.ps1

.EXAMPLE
powershell -ExecutionPolicy Bypass -File .\scripts\Download-ThirdParty.ps1 -Proxy http://127.0.0.1:7890
#>
[CmdletBinding()]
param(
    [string]$Url = 'https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl-shared.zip',

    [string]$Proxy = '',

    [switch]$IgnoreAbiCheck
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$ffmpegRoot = Join-Path $projectRoot 'third_party\ffmpeg'
$binDirectory = Join-Path $ffmpegRoot 'bin'
$cacheDirectory = Join-Path $projectRoot '.downloads\third_party'
$archivePath = Join-Path $cacheDirectory 'ffmpeg-win64-gpl-shared.zip'
$partPath = "$archivePath.part"

# 仓库 include/lib 版本锁定的 DLL 集合；漂移说明上游 ABI 已升级。
$expectedBinaries = @(
    'avcodec-63.dll',
    'avdevice-63.dll',
    'avfilter-12.dll',
    'avformat-63.dll',
    'avutil-61.dll',
    'swresample-7.dll',
    'swscale-10.dll',
    'ffmpeg.exe',
    'ffprobe.exe'
)

New-Item -ItemType Directory -Force -Path $cacheDirectory | Out-Null

Write-Host "下载 FFmpeg shared 运行库：$Url"
$curlArgs = @('-fL', '--retry', '3', '-C', '-', '-o', $partPath)
if (-not [string]::IsNullOrWhiteSpace($Proxy)) {
    $curlArgs += @('--proxy', $Proxy)
}
$curlArgs += $Url
& curl.exe @curlArgs
if ($LASTEXITCODE -ne 0) {
    throw "FFmpeg 压缩包下载失败（curl 退出码 $LASTEXITCODE）。"
}
Move-Item -Force -LiteralPath $partPath -Destination $archivePath

$extractDirectory = Join-Path $cacheDirectory 'ffmpeg-extract'
if (Test-Path -LiteralPath $extractDirectory) {
    Remove-Item -Recurse -Force -LiteralPath $extractDirectory
}
Write-Host "解压压缩包..."
Expand-Archive -LiteralPath $archivePath -DestinationPath $extractDirectory

# 压缩包内唯一顶层目录（如 ffmpeg-master-latest-win64-gpl-shared）下应有 bin 目录。
$packageBin = Get-ChildItem -LiteralPath $extractDirectory -Directory |
    ForEach-Object { Join-Path $_.FullName 'bin' } |
    Where-Object { Test-Path -LiteralPath $_ -PathType Container } |
    Select-Object -First 1
if ($null -eq $packageBin) {
    throw "压缩包结构不符合预期：未找到 bin 目录。"
}

$missing = @($expectedBinaries | Where-Object {
    -not (Test-Path -LiteralPath (Join-Path $packageBin $_) -PathType Leaf)
})
if ($missing.Count -ne 0 -and -not $IgnoreAbiCheck) {
    throw ("下载包缺少仓库 include/lib 版本锁定的文件：{0}" -f ($missing -join '、')) +
        "`n上游 ABI 可能已升级；请同步更新 third_party/ffmpeg 的 include/lib，或用 -IgnoreAbiCheck 强制继续。"
}

Write-Host "还原 bin 目录到 $binDirectory ..."
New-Item -ItemType Directory -Force -Path $binDirectory | Out-Null
Copy-Item -Force -LiteralPath (Join-Path $packageBin '*') -Destination $binDirectory -Recurse

$version = & (Join-Path $binDirectory 'ffmpeg.exe') -version 2>$null |
    Select-Object -First 1
Write-Host "完成。$version"
Write-Host "include/lib 已在仓库内，无需下载；如上方版本与锁定 DLL 主版本不一致，请重新评估 include/lib。"
