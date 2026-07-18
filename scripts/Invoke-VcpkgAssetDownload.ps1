<#
.SYNOPSIS
下载一个由 vcpkg 请求的源文件，并在写入缓存前校验 SHA-512。

.DESCRIPTION
本脚本由 Download-Dependencies.ps1 通过 vcpkg 的 x-script 资产源调用。
优先使用 aria2 多连接断点续传；未安装 aria2 时退化到 Windows 自带 curl.exe。
所有数据先写入 .part 文件，哈希正确后才移动到 vcpkg 指定的最终缓存路径。
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Url,

    [Parameter(Mandatory = $true)]
    [string]$Sha512,

    [Parameter(Mandatory = $true)]
    [string]$Destination,

    [string]$Proxy = '',

    [string]$Aria2Path = '',

    [ValidateRange(1, 64)]
    [int]$Connections = 16,

    [ValidateRange(1, 20)]
    [int]$RetryCount = 8
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Test-AssetHash {
    <#
    .SYNOPSIS
    判断文件是否存在且 SHA-512 与 vcpkg 端口清单一致。
    #>
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$ExpectedSha512
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return $false
    }
    $actual = (Get-FileHash -LiteralPath $Path -Algorithm SHA512).Hash
    return [string]::Equals($actual, $ExpectedSha512, [StringComparison]::OrdinalIgnoreCase)
}

function Invoke-Aria2Download {
    <#
    .SYNOPSIS
    使用 aria2 对单个大文件执行多连接断点续传。
    #>
    param(
        [Parameter(Mandatory = $true)]
        [string]$Executable,

        [Parameter(Mandatory = $true)]
        [string]$SourceUrl,

        [Parameter(Mandatory = $true)]
        [string]$PartPath
    )

    $directory = Split-Path -Parent $PartPath
    $fileName = Split-Path -Leaf $PartPath
    $arguments = @(
        '--allow-overwrite=true'
        '--auto-file-renaming=false'
        '--continue=true'
        '--file-allocation=none'
        '--max-tries=8'
        '--retry-wait=3'
        "--max-connection-per-server=$Connections"
        "--split=$Connections"
        '--min-split-size=1M'
        "--dir=$directory"
        "--out=$fileName"
    )
    if (-not [string]::IsNullOrWhiteSpace($Proxy)) {
        $arguments += "--all-proxy=$Proxy"
    }
    $arguments += $SourceUrl

    & $Executable @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "aria2 下载失败，退出码：$LASTEXITCODE，URL：$SourceUrl"
    }
}

function Invoke-CurlDownload {
    <#
    .SYNOPSIS
    使用系统 curl 执行可重试的断点续传；服务端不支持续传时自动重新下载。
    #>
    param(
        [Parameter(Mandatory = $true)]
        [string]$SourceUrl,

        [Parameter(Mandatory = $true)]
        [string]$PartPath
    )

    $curl = (Get-Command 'curl.exe' -ErrorAction Stop).Source
    for ($attempt = 1; $attempt -le $RetryCount; $attempt++) {
        $arguments = @(
            '--location'
            '--fail'
            '--show-error'
            '--retry', '5'
            '--retry-all-errors'
            '--retry-delay', '2'
            '--output', $PartPath
        )
        if (Test-Path -LiteralPath $PartPath -PathType Leaf) {
            $arguments += @('--continue-at', '-')
        }
        if (-not [string]::IsNullOrWhiteSpace($Proxy)) {
            $arguments += @('--proxy', $Proxy)
        }
        $arguments += $SourceUrl

        & $curl @arguments
        if ($LASTEXITCODE -eq 0) {
            return
        }

        # curl 33/36 常见于源站不支持 Range；删除半文件后下一轮从头开始。
        if ($LASTEXITCODE -eq 33 -or $LASTEXITCODE -eq 36) {
            Remove-Item -LiteralPath $PartPath -Force -ErrorAction SilentlyContinue
        }
        if ($attempt -lt $RetryCount) {
            Start-Sleep -Seconds ([Math]::Min(15, 2 * $attempt))
        }
    }
    throw "curl 下载失败，已重试 $RetryCount 次，URL：$SourceUrl"
}

$destinationPath = [IO.Path]::GetFullPath($Destination)
$destinationDirectory = Split-Path -Parent $destinationPath
[IO.Directory]::CreateDirectory($destinationDirectory) | Out-Null

if (Test-AssetHash -Path $destinationPath -ExpectedSha512 $Sha512) {
    Write-Host "缓存已存在并通过校验：$destinationPath"
    exit 0
}

$partPath = "$destinationPath.part"
$resolvedAria2 = $null
if (-not [string]::IsNullOrWhiteSpace($Aria2Path)) {
    if (Test-Path -LiteralPath $Aria2Path -PathType Leaf) {
        $resolvedAria2 = (Resolve-Path -LiteralPath $Aria2Path).Path
    } else {
        throw "指定的 aria2c 不存在：$Aria2Path"
    }
} else {
    $aria2Command = Get-Command 'aria2c.exe' -ErrorAction SilentlyContinue
    if ($null -ne $aria2Command) {
        $resolvedAria2 = $aria2Command.Source
    }
}

Write-Host "下载：$Url"
Write-Host "目标：$destinationPath"
if ($null -ne $resolvedAria2) {
    Write-Host "下载器：aria2，连接数：$Connections"
    Invoke-Aria2Download -Executable $resolvedAria2 -SourceUrl $Url -PartPath $partPath
} else {
    Write-Host '下载器：curl（如需多连接，请安装 aria2 或传入 -Aria2Path）'
    Invoke-CurlDownload -SourceUrl $Url -PartPath $partPath
}

if (-not (Test-AssetHash -Path $partPath -ExpectedSha512 $Sha512)) {
    $actualHash = if (Test-Path -LiteralPath $partPath -PathType Leaf) {
        (Get-FileHash -LiteralPath $partPath -Algorithm SHA512).Hash
    } else {
        '<文件不存在>'
    }
    Remove-Item -LiteralPath $partPath -Force -ErrorAction SilentlyContinue
    throw "SHA-512 校验失败。期望：$Sha512，实际：$actualHash，URL：$Url"
}

Move-Item -LiteralPath $partPath -Destination $destinationPath -Force
Remove-Item -LiteralPath "$partPath.aria2" -Force -ErrorAction SilentlyContinue
Write-Host "下载完成并通过 SHA-512 校验：$destinationPath"
