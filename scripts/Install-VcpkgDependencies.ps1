<#
.SYNOPSIS
安装本项目 vcpkg.json 中缺少的依赖包；vcpkg 不存在时自动克隆并重建。

.DESCRIPTION
以 manifest 模式运行 vcpkg install，自动解析 vcpkg.json 中的直接与传递依赖，
仅安装尚未就绪的包，结果输出到 <vcpkg 根>\installed，供各 vcxproj 引用。

vcpkg 根目录按以下顺序确定：-VcpkgPath 参数 > 环境变量 VIDEOSC_VCPKG_ROOT > E:\vcpkg。
目标路径没有 vcpkg.exe 时自动执行：git clone microsoft/vcpkg、检出 manifest 的
builtin-baseline（如有）、运行 bootstrap-vcpkg.bat，然后再安装依赖。

默认 triplet 为 x64-windows，与 DedupCore/VideoScGUI 等工程的 VcpkgTriplet 一致。

.EXAMPLE
powershell -ExecutionPolicy Bypass -File .\scripts\Install-VcpkgDependencies.ps1

.EXAMPLE
powershell -ExecutionPolicy Bypass -File .\scripts\Install-VcpkgDependencies.ps1 -Triplet x64-windows-static

.EXAMPLE
powershell -ExecutionPolicy Bypass -File .\scripts\Install-VcpkgDependencies.ps1 -Proxy http://127.0.0.1:7890

.EXAMPLE
powershell -ExecutionPolicy Bypass -File .\scripts\Install-VcpkgDependencies.ps1 -BootstrapOnly
#>
[CmdletBinding()]
param(
    [string]$VcpkgPath = '',

    [ValidateSet('x64-windows', 'x64-windows-static', 'x64-windows-static-md')]
    [string]$Triplet = 'x64-windows',

    [string]$InstallRoot = '',

    [string]$Proxy = '',

    [switch]$BootstrapOnly,

    [switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$projectRoot  = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$manifestPath = Join-Path $projectRoot 'vcpkg.json'

if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw "未找到 vcpkg.json：$manifestPath"
}

# vcpkg 根目录解析顺序：-VcpkgPath > 环境变量 VIDEOSC_VCPKG_ROOT > E:\vcpkg。
if ([string]::IsNullOrWhiteSpace($VcpkgPath)) {
    $VcpkgPath = if (-not [string]::IsNullOrWhiteSpace($env:VIDEOSC_VCPKG_ROOT)) {
        Join-Path $env:VIDEOSC_VCPKG_ROOT 'vcpkg.exe'
    } else {
        'E:\vcpkg\vcpkg.exe'
    }
}
$vcpkgRoot = Split-Path -Parent $VcpkgPath
if ([string]::IsNullOrWhiteSpace($InstallRoot)) {
    $InstallRoot = Join-Path $vcpkgRoot 'installed'
}
$InstallRoot = [IO.Path]::GetFullPath($InstallRoot)

# 设置下载代理：vcpkg、git 和 bootstrap 内部通过 curl 等读取 HTTP(S)_PROXY。
# 注意：即使代理是 HTTPS 服务器，前缀也必须用 http://（除非代理本身是 HTTPS 服务器）。
if (-not [string]::IsNullOrWhiteSpace($Proxy)) {
    $env:HTTP_PROXY  = $Proxy
    $env:HTTPS_PROXY = $Proxy
}

Write-Host "项目目录：$projectRoot"
Write-Host "vcpkg   ：$VcpkgPath"
Write-Host "Triplet ：$Triplet"
Write-Host "安装目录：$InstallRoot"
if (-not [string]::IsNullOrWhiteSpace($Proxy)) {
    Write-Host "代理    ：$Proxy"
}

# 目标路径没有 vcpkg.exe 时自动克隆并重建 vcpkg 本体。
if (-not (Test-Path -LiteralPath $VcpkgPath -PathType Leaf)) {
    if ($DryRun) {
        Write-Host "vcpkg.exe 不存在，将自动重建（DryRun 不实际执行）："
        Write-Host "  git clone https://github.com/microsoft/vcpkg.git $vcpkgRoot"
        Write-Host "  git checkout <vcpkg.json 的 builtin-baseline>"
        Write-Host "  bootstrap-vcpkg.bat -disableMetrics"
    } else {
        $git = Get-Command git.exe -ErrorAction SilentlyContinue
        if ($null -eq $git) {
            throw "自动重建需要 git，但未找到 git.exe；请安装 git 或用 -VcpkgPath 指定已有 vcpkg。"
        }
        if ((Test-Path -LiteralPath $vcpkgRoot) -and
            (Get-ChildItem -LiteralPath $vcpkgRoot -Force -ErrorAction SilentlyContinue)) {
            throw "目录 $vcpkgRoot 已存在且非空但没有 vcpkg.exe；为避免覆盖，请人工处理后重试或改用 -VcpkgPath。"
        }
        Write-Host "克隆 vcpkg 仓库到 $vcpkgRoot ..."
        & git.exe clone https://github.com/microsoft/vcpkg.git $vcpkgRoot
        if ($LASTEXITCODE -ne 0) {
            throw "vcpkg 仓库克隆失败，退出码：$LASTEXITCODE。"
        }
        $manifest = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
        if ($manifest.'builtin-baseline') {
            Write-Host "检出 builtin-baseline：$($manifest.'builtin-baseline')"
            & git.exe -C $vcpkgRoot checkout $manifest.'builtin-baseline'
            if ($LASTEXITCODE -ne 0) {
                throw "检出 builtin-baseline 失败，退出码：$LASTEXITCODE。"
            }
        }
        Write-Host "执行 bootstrap-vcpkg ..."
        & (Join-Path $vcpkgRoot 'bootstrap-vcpkg.bat') -disableMetrics
        if ($LASTEXITCODE -ne 0) {
            throw "bootstrap-vcpkg 失败，退出码：$LASTEXITCODE。"
        }
        if (-not (Test-Path -LiteralPath $VcpkgPath -PathType Leaf)) {
            throw "bootstrap 结束后仍未找到 vcpkg.exe：$VcpkgPath"
        }
    }
}

if ($BootstrapOnly) {
    Write-Host "vcpkg 本体已就绪（-BootstrapOnly），跳过依赖安装。"
    return
}

[IO.Directory]::CreateDirectory($InstallRoot) | Out-Null

$arguments = @(
    'install'
    "--triplet=$Triplet"
    "--x-manifest-root=$projectRoot"
    "--x-install-root=$InstallRoot"
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

Write-Host "依赖安装完成，输出目录：$InstallRoot"
