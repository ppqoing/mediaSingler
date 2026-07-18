$ErrorActionPreference = 'Stop'

$proxy   = 'http://127.0.0.1:7890'
$dlDir   = Join-Path ([IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))) '.downloads'
[IO.Directory]::CreateDirectory($dlDir) | Out-Null
$msiName = 'mysql-installer-community-8.0.46.0.msi'
$msiPath = Join-Path $dlDir $msiName
$url     = "https://cdn.mysql.com/Downloads/MySQLInstaller/$msiName"

Write-Host "下载目录：$dlDir"
Write-Host "目标文件：$msiPath"
Write-Host "代理    ：$proxy"
Write-Host "URL     ：$url"

# 已存在且大于 100MB 视为完整，跳过下载
$skip = $false
if (Test-Path -LiteralPath $msiPath -PathType Leaf) {
    $sz = (Get-Item -LiteralPath $msiPath).Length
    if ($sz -gt 100MB) {
        Write-Host "已存在 $($sz) 字节，跳过下载。"
        $skip = $true
    }
}

if (-not $skip) {
    Write-Host '开始下载（curl，走代理，断点续传）...'
    $curl = (Get-Command curl.exe -ErrorAction Stop).Source
    for ($i = 1; $i -le 8; $i++) {
        $args = @('--location', '--fail', '--show-error',
                  '--retry', '5', '--retry-all-errors', '--retry-delay', '2',
                  '--proxy', $proxy,
                  '--output', $msiPath)
        if (Test-Path -LiteralPath $msiPath -PathType Leaf) {
            $args += @('--continue-at', '-')
        }
        $args += $url
        & $curl @args
        if ($LASTEXITCODE -eq 0) { break }
        Write-Host "第 $i 次下载失败（exit $LASTEXITCODE），重试..."
        Start-Sleep -Seconds ([Math]::Min(15, 2 * $i))
    }
    if ($LASTEXITCODE -ne 0) {
        throw "下载失败，curl exit=$LASTEXITCODE"
    }
    $sz = (Get-Item -LiteralPath $msiPath).Length
    Write-Host "下载完成，大小 $([Math]::Round($sz/1MB,1)) MB"
}

Write-Host ''
Write-Host '==================================================='
Write-Host '下载完成，即将启动 MSI 安装向导。'
Write-Host '请在向导中：'
Write-Host '  1) 选择 Custom（自定义）'
Write-Host '  2) 添加 MySQL Server 8.0.46 -> Server only 或 Developer Default'
Write-Host '  3) Execute 安装'
Write-Host '  4) 进入 Product Configuration：'
Write-Host '     - 端口默认 3306'
Write-Host '     - 设置 root 密码（请记牢）'
Write-Host '     - Windows Service Name 默认 MySQL80'
Write-Host '     - 启动服务'
Write-Host '  5) Finish'
Write-Host '==================================================='

Start-Sleep -Seconds 2
Write-Host "启动 MSI：$msiPath"
Start-Process -FilePath 'msiexec.exe' -ArgumentList @('/i', "`"$msiPath`"") -Verb RunAs
Write-Host 'MSI 已启动（如未弹出，请手动双击该 MSI）。'
'OK'