<#
.SYNOPSIS
根据已有媒体样本生成多编码、多封装、多分辨率和多水印的测试文件集合。

.DESCRIPTION
脚本使用 FFprobe 识别输入目录中的视频、音频和图片，再按固定精选矩阵调用 FFmpeg。
默认只读取 E:\Code\media 中的原始样本，输出到 generated_test_media 子目录。
脚本不会清空输出目录；目标文件已存在时默认跳过，只有显式传入 -Overwrite 才会覆盖。

.PARAMETER FfmpegPath
ffmpeg.exe 的完整路径。

.PARAMETER FfprobePath
ffprobe.exe 的完整路径。

.PARAMETER SourceDirectory
已有媒体样本目录。会递归查找媒体文件，并排除 FFmpeg 工具目录和输出目录。

.PARAMETER OutputDirectory
测试文件输出目录。不得与输入目录或 FFmpeg 工具目录相同。

.PARAMETER DurationSeconds
视频测试片段的最长时长，单位为秒。

.PARAMETER Overwrite
允许覆盖已存在的目标媒体文件。脚本不会删除输出目录中的其他文件。

.PARAMETER DryRun
仅检查环境、分析输入并显示任务矩阵，不创建目录或媒体文件。

.EXAMPLE
powershell -ExecutionPolicy Bypass -File .\scripts\Generate-MediaTestFiles.ps1 -DryRun

.EXAMPLE
powershell -ExecutionPolicy Bypass -File .\scripts\Generate-MediaTestFiles.ps1

.EXAMPLE
powershell -ExecutionPolicy Bypass -File .\scripts\Generate-MediaTestFiles.ps1 -DurationSeconds 5 -Overwrite
#>
[CmdletBinding()]
param(
    [string]$FfmpegPath = 'E:\Code\media\ffmpeg\ffmpeg.exe',

    [string]$FfprobePath = 'E:\Code\media\ffmpeg\ffprobe.exe',

    [string]$SourceDirectory = 'E:\Code\media',

    [string]$OutputDirectory = 'E:\Code\media\generated_test_media',

    [ValidateRange(1, 60)]
    [int]$DurationSeconds = 8,

    [switch]$Overwrite,

    [switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

<#
.SYNOPSIS
判断文件是否位于指定目录内部。

.PARAMETER Path
待判断文件或子目录的路径。

.PARAMETER Directory
作为边界的目录路径。

.OUTPUTS
System.Boolean。路径位于目录内部时返回 true，不把目录自身视为内部路径。
#>
function Test-PathInsideDirectory {
    param(
        [Parameter(Mandatory)]
        [string]$Path,

        [Parameter(Mandatory)]
        [string]$Directory
    )

    $fullPath = [IO.Path]::GetFullPath($Path)
    $directoryPrefix = [IO.Path]::GetFullPath($Directory).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    return $fullPath.StartsWith($directoryPrefix, [StringComparison]::OrdinalIgnoreCase)
}

<#
.SYNOPSIS
使用 FFprobe 读取单个媒体文件的关键流信息。

.PARAMETER Path
媒体文件完整路径。

.PARAMETER Kind
由扩展名确定的 Video、Audio 或 Image 分类。

.PARAMETER ProbePath
ffprobe.exe 的完整路径。

.OUTPUTS
包含路径、类型、编码、尺寸、时长和文件大小的对象。

.NOTES
FFprobe 失败或文件与声明分类不匹配时抛出异常，不静默使用不完整信息。
#>
function Get-MediaProbeInfo {
    param(
        [Parameter(Mandatory)]
        [string]$Path,

        [Parameter(Mandatory)]
        [ValidateSet('Video', 'Audio', 'Image')]
        [string]$Kind,

        [Parameter(Mandatory)]
        [string]$ProbePath
    )

    $probeOutput = & $ProbePath -v error `
        -show_entries 'format=duration,format_name' `
        -show_entries 'stream=codec_type,codec_name,width,height' `
        -of json -- $Path 2>&1
    $probeExitCode = $LASTEXITCODE
    if ($probeExitCode -ne 0) {
        throw "FFprobe 读取失败，退出码 $probeExitCode：$Path`n$($probeOutput -join [Environment]::NewLine)"
    }

    $probeData = ($probeOutput -join [Environment]::NewLine) | ConvertFrom-Json
    $streams = @($probeData.streams)
    $videoStream = $streams | Where-Object { $_.codec_type -eq 'video' } | Select-Object -First 1
    $audioStream = $streams | Where-Object { $_.codec_type -eq 'audio' } | Select-Object -First 1

    if (($Kind -eq 'Video' -or $Kind -eq 'Image') -and $null -eq $videoStream) {
        throw "未检测到视频流：$Path"
    }
    if ($Kind -eq 'Audio' -and $null -eq $audioStream) {
        throw "未检测到音频流：$Path"
    }

    $duration = 0.0
    $durationProperty = $probeData.format.PSObject.Properties['duration']
    if ($null -ne $durationProperty -and $null -ne $durationProperty.Value) {
        [void][double]::TryParse(
            [string]$durationProperty.Value,
            [Globalization.NumberStyles]::Float,
            [Globalization.CultureInfo]::InvariantCulture,
            [ref]$duration
        )
    }

    $width = 0
    $height = 0
    $videoCodec = $null
    if ($null -ne $videoStream) {
        $width = [int]$videoStream.width
        $height = [int]$videoStream.height
        $videoCodec = [string]$videoStream.codec_name
    }

    $audioCodec = $null
    if ($null -ne $audioStream) {
        $audioCodec = [string]$audioStream.codec_name
    }

    $file = Get-Item -LiteralPath $Path
    return [pscustomobject]@{
        Path        = $file.FullName
        Name        = $file.Name
        BaseName    = $file.BaseName
        Extension   = $file.Extension.ToLowerInvariant()
        Kind        = $Kind
        Size        = $file.Length
        Duration    = $duration
        Width       = $width
        Height      = $height
        PixelCount  = [long]$width * [long]$height
        VideoCodec  = $videoCodec
        AudioCodec  = $audioCodec
        Format      = [string]$probeData.format.format_name
    }
}

<#
.SYNOPSIS
确认当前 FFmpeg 构建包含生成矩阵依赖的编码器和滤镜。

.PARAMETER EncoderPath
ffmpeg.exe 的完整路径。

.NOTES
能力缺失时立即抛出异常，避免运行到生成中途才发现整类任务不可用。
#>
function Assert-FfmpegCapabilities {
    param(
        [Parameter(Mandatory)]
        [string]$EncoderPath
    )

    $encoderOutput = (& $EncoderPath -hide_banner -encoders 2>&1) -join [Environment]::NewLine
    if ($LASTEXITCODE -ne 0) {
        throw '无法读取 FFmpeg 编码器列表。'
    }

    $requiredEncoders = @(
        'libx264', 'libx265', 'libsvtav1', 'libvpx-vp9', 'mpeg4', 'mpeg2video',
        'wmv2', 'prores_ks', 'aac', 'libmp3lame', 'libopus', 'libvorbis', 'flac',
        'pcm_s16le', 'mp2', 'wmav2', 'mjpeg', 'png', 'libwebp', 'bmp', 'tiff', 'gif'
    )
    foreach ($encoder in $requiredEncoders) {
        $encoderPattern = '(?m)^\s*[VAS]\S*\s+' + [regex]::Escape($encoder) + '\s'
        if ($encoderOutput -notmatch $encoderPattern) {
            throw "当前 FFmpeg 缺少编码器：$encoder"
        }
    }

    $filterOutput = (& $EncoderPath -hide_banner -filters 2>&1) -join [Environment]::NewLine
    if ($LASTEXITCODE -ne 0) {
        throw '无法读取 FFmpeg 滤镜列表。'
    }

    $requiredFilters = @('drawtext', 'overlay', 'scale', 'pad', 'format', 'colorchannelmixer')
    foreach ($filter in $requiredFilters) {
        $filterPattern = '(?m)^\s*\S+\s+' + [regex]::Escape($filter) + '\s'
        if ($filterOutput -notmatch $filterPattern) {
            throw "当前 FFmpeg 缺少滤镜：$filter"
        }
    }
}

<#
.SYNOPSIS
生成保持宽高比并补边到固定尺寸的 FFmpeg 滤镜。

.PARAMETER Width
输出宽度，单位为像素。

.PARAMETER Height
输出高度，单位为像素。

.OUTPUTS
可直接传入 -vf 或 filter_complex 的滤镜字符串。
#>
function Get-FixedScaleFilter {
    param(
        [Parameter(Mandatory)]
        [int]$Width,

        [Parameter(Mandatory)]
        [int]$Height
    )

    return "scale=${Width}:${Height}:force_original_aspect_ratio=decrease,pad=${Width}:${Height}:(ow-iw)/2:(oh-ih)/2,setsar=1"
}

<#
.SYNOPSIS
创建普通单输入视频转码任务的 FFmpeg 参数。

.PARAMETER SourcePath
源视频完整路径。

.PARAMETER Duration
输出片段最长时长，单位为秒。

.PARAMETER VideoFilter
视频滤镜字符串。

.PARAMETER VideoCodec
视频编码器名称。

.PARAMETER VideoOptions
视频编码器附加参数。

.PARAMETER AudioCodec
音频编码器名称。

.PARAMETER AudioOptions
音频编码器附加参数。

.PARAMETER ContainerOptions
封装格式附加参数。

.OUTPUTS
FFmpeg 参数数组，不包含全局参数和输出文件路径。
#>
function New-StandardVideoArguments {
    param(
        [Parameter(Mandatory)]
        [string]$SourcePath,

        [Parameter(Mandatory)]
        [int]$Duration,

        [Parameter(Mandatory)]
        [string]$VideoFilter,

        [Parameter(Mandatory)]
        [string]$VideoCodec,

        [object[]]$VideoOptions = @(),

        [Parameter(Mandatory)]
        [string]$AudioCodec,

        [object[]]$AudioOptions = @(),

        [object[]]$ContainerOptions = @()
    )

    $arguments = @(
        '-ss', '0',
        '-t', [string]$Duration,
        '-i', $SourcePath,
        '-map', '0:v:0',
        '-map', '0:a:0?',
        '-vf', $VideoFilter,
        '-c:v', $VideoCodec
    )
    $arguments += $VideoOptions
    $arguments += @('-c:a', $AudioCodec)
    $arguments += $AudioOptions
    $arguments += $ContainerOptions
    return ,$arguments
}

<#
.SYNOPSIS
创建一个可执行或可复制的媒体生成任务。

.PARAMETER Name
任务稳定名称。

.PARAMETER Category
任务分类，用于 DryRun 和 manifest 汇总。

.PARAMETER Operation
Ffmpeg 或 Copy。

.PARAMETER SourcePath
源媒体完整路径。

.PARAMETER OutputPath
目标媒体完整路径。

.PARAMETER Arguments
FFmpeg 参数，不包含输出路径；复制任务可省略。

.PARAMETER Metadata
写入 manifest.json 的编码、尺寸和水印信息。

.OUTPUTS
媒体生成任务对象。
#>
function New-GenerationTask {
    param(
        [Parameter(Mandatory)]
        [string]$Name,

        [Parameter(Mandatory)]
        [string]$Category,

        [Parameter(Mandatory)]
        [ValidateSet('Ffmpeg', 'Copy')]
        [string]$Operation,

        [Parameter(Mandatory)]
        [string]$SourcePath,

        [Parameter(Mandatory)]
        [string]$OutputPath,

        [object[]]$Arguments = @(),

        [Parameter(Mandatory)]
        [System.Collections.IDictionary]$Metadata
    )

    return [pscustomobject]@{
        Name       = $Name
        Category   = $Category
        Operation  = $Operation
        SourcePath = $SourcePath
        OutputPath = $OutputPath
        Arguments  = @($Arguments)
        Metadata   = $Metadata
    }
}

# 先解析和验证所有外部路径，避免在任务矩阵生成后才发现基础环境不可用。
$ffmpegFullPath = [IO.Path]::GetFullPath($FfmpegPath)
$ffprobeFullPath = [IO.Path]::GetFullPath($FfprobePath)
$sourceRoot = [IO.Path]::GetFullPath($SourceDirectory)
$outputRoot = [IO.Path]::GetFullPath($OutputDirectory)
$ffmpegDirectory = [IO.Path]::GetDirectoryName($ffmpegFullPath)

if (-not (Test-Path -LiteralPath $ffmpegFullPath -PathType Leaf)) {
    throw "未找到 FFmpeg：$ffmpegFullPath"
}
if (-not (Test-Path -LiteralPath $ffprobeFullPath -PathType Leaf)) {
    throw "未找到 FFprobe：$ffprobeFullPath"
}
if (-not (Test-Path -LiteralPath $sourceRoot -PathType Container)) {
    throw "未找到媒体输入目录：$sourceRoot"
}
if ([string]::Equals($sourceRoot.TrimEnd('\', '/'), $outputRoot.TrimEnd('\', '/'), [StringComparison]::OrdinalIgnoreCase)) {
    throw '输出目录不能与媒体输入目录相同。'
}
if ([string]::Equals($ffmpegDirectory.TrimEnd('\', '/'), $outputRoot.TrimEnd('\', '/'), [StringComparison]::OrdinalIgnoreCase)) {
    throw '输出目录不能与 FFmpeg 工具目录相同。'
}

Assert-FfmpegCapabilities -EncoderPath $ffmpegFullPath

# 水印使用系统字体文件，固定字体路径可避免不同机器的 fontconfig 默认字体产生差异。
$fontCandidates = @(
    (Join-Path $env:WINDIR 'Fonts\msyh.ttc'),
    (Join-Path $env:WINDIR 'Fonts\msyhbd.ttc'),
    (Join-Path $env:WINDIR 'Fonts\arial.ttf')
)
$fontFile = $fontCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
if ([string]::IsNullOrWhiteSpace($fontFile)) {
    throw '未找到可用于 drawtext 水印的微软雅黑或 Arial 字体。'
}
$ffmpegFontPath = $fontFile.Replace('\', '/').Replace(':', '\:')

# 扩展名只用于初步分类，最终仍由 FFprobe 验证实际流类型。
$videoExtensions = @('.avi', '.mkv', '.mp4', '.wmv', '.mov', '.webm', '.mpg', '.mpeg', '.m4v', '.ts')
$audioExtensions = @('.mp3', '.wav', '.flac', '.aac', '.m4a', '.ogg', '.opus', '.wma')
$imageExtensions = @('.jpg', '.jpeg', '.png', '.webp', '.bmp', '.tif', '.tiff', '.gif')

$sourceFiles = @(
    Get-ChildItem -LiteralPath $sourceRoot -File -Recurse | Where-Object {
        -not (Test-PathInsideDirectory -Path $_.FullName -Directory $ffmpegDirectory) -and
        -not (Test-PathInsideDirectory -Path $_.FullName -Directory $outputRoot)
    }
)

$mediaInfo = New-Object System.Collections.Generic.List[object]
foreach ($sourceFile in $sourceFiles) {
    $extension = $sourceFile.Extension.ToLowerInvariant()
    $kind = $null
    if ($videoExtensions -contains $extension) {
        $kind = 'Video'
    } elseif ($audioExtensions -contains $extension) {
        $kind = 'Audio'
    } elseif ($imageExtensions -contains $extension) {
        $kind = 'Image'
    } else {
        continue
    }

    try {
        [void]$mediaInfo.Add((Get-MediaProbeInfo -Path $sourceFile.FullName -Kind $kind -ProbePath $ffprobeFullPath))
    } catch {
        Write-Warning "跳过无法识别的媒体文件：$($sourceFile.FullName)。$($_.Exception.Message)"
    }
}

$videoSources = @($mediaInfo | Where-Object { $_.Kind -eq 'Video' } | Sort-Object Path)
$audioSources = @($mediaInfo | Where-Object { $_.Kind -eq 'Audio' } | Sort-Object Path)
$imageSources = @($mediaInfo | Where-Object { $_.Kind -eq 'Image' } | Sort-Object Path)
if ($videoSources.Count -eq 0) {
    throw '输入目录中没有可解码的视频样本。'
}
if ($audioSources.Count -eq 0) {
    throw '输入目录中没有可解码的独立音频样本。'
}
if ($imageSources.Count -eq 0) {
    throw '输入目录中没有可解码的图片样本。'
}

# 优先使用 MP4 作为主视频、像素数最大的图片作为主图片，确保矩阵生成稳定且覆盖高清缩放。
$primaryVideo = $videoSources | Where-Object { $_.Extension -eq '.mp4' } | Sort-Object Duration -Descending | Select-Object -First 1
if ($null -eq $primaryVideo) {
    $primaryVideo = $videoSources | Sort-Object Duration -Descending | Select-Object -First 1
}
$primaryAudio = $audioSources | Where-Object { $_.Extension -eq '.mp3' } | Sort-Object Duration -Descending | Select-Object -First 1
if ($null -eq $primaryAudio) {
    $primaryAudio = $audioSources | Sort-Object Duration -Descending | Select-Object -First 1
}
$primaryImage = $imageSources | Sort-Object PixelCount -Descending | Select-Object -First 1
$watermarkImage = $imageSources | Where-Object { $_.Extension -eq '.png' } | Sort-Object PixelCount -Descending | Select-Object -First 1
if ($null -eq $watermarkImage) {
    $watermarkImage = $primaryImage
}

Write-Host "FFmpeg   ：$ffmpegFullPath"
Write-Host "FFprobe  ：$ffprobeFullPath"
Write-Host "输入目录 ：$sourceRoot"
Write-Host "输出目录 ：$outputRoot"
Write-Host "主视频   ：$($primaryVideo.Path)"
Write-Host "主音频   ：$($primaryAudio.Path)"
Write-Host "主图片   ：$($primaryImage.Path)"
Write-Host "水印图片 ：$($watermarkImage.Path)"

$tasks = New-Object System.Collections.Generic.List[object]
$baseScale = Get-FixedScaleFilter -Width 1280 -Height 720
$baseVideoFilter = "$baseScale,format=yuv420p"

# 每个已有视频都生成一份统一 H.264 样本，用于覆盖不同输入解码链路。
$sourceIndex = 1
foreach ($videoSource in $videoSources) {
    $safeBaseName = [regex]::Replace($videoSource.BaseName, '[^A-Za-z0-9._-]', '_')
    $sourceTag = $videoSource.Extension.TrimStart('.').ToLowerInvariant()
    $outputPath = Join-Path $outputRoot ("video\sources\{0:D2}_{1}_{2}_to_h264.mp4" -f $sourceIndex, $sourceTag, $safeBaseName)
    $arguments = @(New-StandardVideoArguments `
        -SourcePath $videoSource.Path `
        -Duration $DurationSeconds `
        -VideoFilter $baseVideoFilter `
        -VideoCodec 'libx264' `
        -VideoOptions @('-preset', 'veryfast', '-crf', '23') `
        -AudioCodec 'aac' `
        -AudioOptions @('-b:a', '128k') `
        -ContainerOptions @('-movflags', '+faststart'))
    [void]$tasks.Add((New-GenerationTask `
        -Name ("source-video-{0:D2}" -f $sourceIndex) `
        -Category 'video-source' `
        -Operation 'Ffmpeg' `
        -SourcePath $videoSource.Path `
        -OutputPath $outputPath `
        -Arguments $arguments `
        -Metadata ([ordered]@{
            container = 'mp4'; videoCodec = 'h264'; audioCodec = 'aac';
            width = 1280; height = 720; watermark = 'none'; sourceCodec = $videoSource.VideoCodec
        })))
    $sourceIndex++
}

# 编码和封装采用精选合法组合，避免生成播放器普遍无法识别的无效组合。
$videoProfiles = @(
    [pscustomobject]@{ Name = 'h264-mp4'; Extension = 'mp4'; VideoCodec = 'libx264'; VideoOptions = @('-preset', 'veryfast', '-crf', '23'); AudioCodec = 'aac'; AudioOptions = @('-b:a', '128k'); ContainerOptions = @('-movflags', '+faststart'); Container = 'mp4' },
    [pscustomobject]@{ Name = 'h264-mkv'; Extension = 'mkv'; VideoCodec = 'libx264'; VideoOptions = @('-preset', 'veryfast', '-crf', '23'); AudioCodec = 'aac'; AudioOptions = @('-b:a', '128k'); ContainerOptions = @(); Container = 'matroska' },
    [pscustomobject]@{ Name = 'h264-mpegts'; Extension = 'ts'; VideoCodec = 'libx264'; VideoOptions = @('-preset', 'veryfast', '-crf', '23'); AudioCodec = 'aac'; AudioOptions = @('-b:a', '128k'); ContainerOptions = @('-f', 'mpegts'); Container = 'mpegts' },
    [pscustomobject]@{ Name = 'hevc-mp4'; Extension = 'mp4'; VideoCodec = 'libx265'; VideoOptions = @('-preset', 'ultrafast', '-crf', '28', '-tag:v', 'hvc1'); AudioCodec = 'aac'; AudioOptions = @('-b:a', '128k'); ContainerOptions = @('-movflags', '+faststart'); Container = 'mp4' },
    [pscustomobject]@{ Name = 'hevc-mkv'; Extension = 'mkv'; VideoCodec = 'libx265'; VideoOptions = @('-preset', 'ultrafast', '-crf', '28'); AudioCodec = 'aac'; AudioOptions = @('-b:a', '128k'); ContainerOptions = @(); Container = 'matroska' },
    [pscustomobject]@{ Name = 'vp9-webm'; Extension = 'webm'; VideoCodec = 'libvpx-vp9'; VideoOptions = @('-crf', '32', '-b:v', '0', '-deadline', 'good', '-cpu-used', '4'); AudioCodec = 'libopus'; AudioOptions = @('-b:a', '96k'); ContainerOptions = @(); Container = 'webm' },
    [pscustomobject]@{ Name = 'av1-mkv'; Extension = 'mkv'; VideoCodec = 'libsvtav1'; VideoOptions = @('-preset', '10', '-crf', '36'); AudioCodec = 'libopus'; AudioOptions = @('-b:a', '96k'); ContainerOptions = @(); Container = 'matroska' },
    [pscustomobject]@{ Name = 'mpeg4-avi'; Extension = 'avi'; VideoCodec = 'mpeg4'; VideoOptions = @('-q:v', '5'); AudioCodec = 'libmp3lame'; AudioOptions = @('-b:a', '128k'); ContainerOptions = @(); Container = 'avi' },
    [pscustomobject]@{ Name = 'mpeg2-mpg'; Extension = 'mpg'; VideoCodec = 'mpeg2video'; VideoOptions = @('-q:v', '5'); AudioCodec = 'mp2'; AudioOptions = @('-b:a', '192k'); ContainerOptions = @('-f', 'mpeg'); Container = 'mpeg' },
    [pscustomobject]@{ Name = 'wmv2-wmv'; Extension = 'wmv'; VideoCodec = 'wmv2'; VideoOptions = @('-b:v', '2M'); AudioCodec = 'wmav2'; AudioOptions = @('-b:a', '128k'); ContainerOptions = @(); Container = 'asf' },
    [pscustomobject]@{ Name = 'prores-mov'; Extension = 'mov'; VideoCodec = 'prores_ks'; VideoOptions = @('-profile:v', '0', '-pix_fmt', 'yuv422p10le'); AudioCodec = 'pcm_s16le'; AudioOptions = @(); ContainerOptions = @(); Container = 'mov' }
)

$profileIndex = 1
foreach ($profile in $videoProfiles) {
    $profileFilter = $baseVideoFilter
    if ($profile.VideoCodec -eq 'prores_ks') {
        $profileFilter = "$baseScale,format=yuv422p10le"
    }
    $outputPath = Join-Path $outputRoot ("video\codecs\{0:D2}_{1}.{2}" -f $profileIndex, $profile.Name, $profile.Extension)
    $arguments = @(New-StandardVideoArguments `
        -SourcePath $primaryVideo.Path `
        -Duration $DurationSeconds `
        -VideoFilter $profileFilter `
        -VideoCodec $profile.VideoCodec `
        -VideoOptions @($profile.VideoOptions) `
        -AudioCodec $profile.AudioCodec `
        -AudioOptions @($profile.AudioOptions) `
        -ContainerOptions @($profile.ContainerOptions))
    [void]$tasks.Add((New-GenerationTask `
        -Name ("video-codec-{0:D2}-{1}" -f $profileIndex, $profile.Name) `
        -Category 'video-codec' `
        -Operation 'Ffmpeg' `
        -SourcePath $primaryVideo.Path `
        -OutputPath $outputPath `
        -Arguments $arguments `
        -Metadata ([ordered]@{
            container = $profile.Container; videoCodec = $profile.VideoCodec; audioCodec = $profile.AudioCodec;
            width = 1280; height = 720; watermark = 'none'
        })))
    $profileIndex++
}

$resolutionProfiles = @(
    [pscustomobject]@{ Label = '360p'; Width = 640; Height = 360 },
    [pscustomobject]@{ Label = '480p'; Width = 854; Height = 480 },
    [pscustomobject]@{ Label = '720p'; Width = 1280; Height = 720 },
    [pscustomobject]@{ Label = '1080p'; Width = 1920; Height = 1080 },
    [pscustomobject]@{ Label = '1440p'; Width = 2560; Height = 1440 },
    [pscustomobject]@{ Label = '2160p'; Width = 3840; Height = 2160 }
)

$resolutionIndex = 1
foreach ($resolution in $resolutionProfiles) {
    $scaleFilter = Get-FixedScaleFilter -Width $resolution.Width -Height $resolution.Height
    $outputPath = Join-Path $outputRoot ("video\resolutions\{0:D2}_h264_{1}_{2}x{3}.mp4" -f $resolutionIndex, $resolution.Label, $resolution.Width, $resolution.Height)
    $arguments = @(New-StandardVideoArguments `
        -SourcePath $primaryVideo.Path `
        -Duration $DurationSeconds `
        -VideoFilter "$scaleFilter,format=yuv420p" `
        -VideoCodec 'libx264' `
        -VideoOptions @('-preset', 'veryfast', '-crf', '25') `
        -AudioCodec 'aac' `
        -AudioOptions @('-b:a', '128k') `
        -ContainerOptions @('-movflags', '+faststart'))
    [void]$tasks.Add((New-GenerationTask `
        -Name ("video-resolution-{0}" -f $resolution.Label) `
        -Category 'video-resolution' `
        -Operation 'Ffmpeg' `
        -SourcePath $primaryVideo.Path `
        -OutputPath $outputPath `
        -Arguments $arguments `
        -Metadata ([ordered]@{
            container = 'mp4'; videoCodec = 'h264'; audioCodec = 'aac';
            width = $resolution.Width; height = $resolution.Height; watermark = 'none'
        })))
    $resolutionIndex++
}

# 文字滤镜保持 ASCII 内容，避免测试结果受系统区域设置影响；字体文件仍显式固定。
$textTopLeft = "drawtext=fontfile='$ffmpegFontPath':text='VideoSc Test':x=24:y=24:fontsize=40:fontcolor=white:borderw=2:bordercolor=black"
$textCenter = "drawtext=fontfile='$ffmpegFontPath':text='VideoSc Center':x=(w-text_w)/2:y=(h-text_h)/2:fontsize=56:fontcolor=white@0.45:borderw=2:bordercolor=black@0.45"
$timeCode = "drawtext=fontfile='$ffmpegFontPath':text='Time %{pts\:hms}':x=w-text_w-24:y=h-text_h-24:fontsize=34:fontcolor=yellow:borderw=2:bordercolor=black"
$tileText = @(
    "drawtext=fontfile='$ffmpegFontPath':text='VideoSc':x=80:y=80:fontsize=40:fontcolor=white@0.28",
    "drawtext=fontfile='$ffmpegFontPath':text='VideoSc':x=(w-text_w)/2:y=(h-text_h)/2:fontsize=40:fontcolor=white@0.28",
    "drawtext=fontfile='$ffmpegFontPath':text='VideoSc':x=w-text_w-80:y=h-text_h-80:fontsize=40:fontcolor=white@0.28"
) -join ','

$textWatermarkProfiles = @(
    [pscustomobject]@{ Name = 'text-top-left'; Filter = $textTopLeft; Watermark = 'text-top-left' },
    [pscustomobject]@{ Name = 'text-center-transparent'; Filter = $textCenter; Watermark = 'text-center-transparent' },
    [pscustomobject]@{ Name = 'timecode-bottom-right'; Filter = $timeCode; Watermark = 'timecode-bottom-right' },
    [pscustomobject]@{ Name = 'text-tiled'; Filter = $tileText; Watermark = 'text-tiled' }
)

$watermarkIndex = 1
foreach ($watermarkProfile in $textWatermarkProfiles) {
    $outputPath = Join-Path $outputRoot ("video\watermarks\{0:D2}_{1}.mp4" -f $watermarkIndex, $watermarkProfile.Name)
    $arguments = @(New-StandardVideoArguments `
        -SourcePath $primaryVideo.Path `
        -Duration $DurationSeconds `
        -VideoFilter "$baseScale,$($watermarkProfile.Filter),format=yuv420p" `
        -VideoCodec 'libx264' `
        -VideoOptions @('-preset', 'veryfast', '-crf', '23') `
        -AudioCodec 'aac' `
        -AudioOptions @('-b:a', '128k') `
        -ContainerOptions @('-movflags', '+faststart'))
    [void]$tasks.Add((New-GenerationTask `
        -Name ("video-watermark-{0}" -f $watermarkProfile.Name) `
        -Category 'video-watermark' `
        -Operation 'Ffmpeg' `
        -SourcePath $primaryVideo.Path `
        -OutputPath $outputPath `
        -Arguments $arguments `
        -Metadata ([ordered]@{
            container = 'mp4'; videoCodec = 'h264'; audioCodec = 'aac';
            width = 1280; height = 720; watermark = $watermarkProfile.Watermark
        })))
    $watermarkIndex++
}

$videoOverlayFilter = "[0:v]$baseScale,format=yuv420p[base];[1:v]scale=160:-1,format=rgba,colorchannelmixer=aa=0.45[wm];[base][wm]overlay=W-w-24:24,format=yuv420p[v]"
$videoOverlayArguments = @(
    '-ss', '0', '-i', $primaryVideo.Path,
    '-loop', '1', '-framerate', '25', '-i', $watermarkImage.Path,
    '-t', [string]$DurationSeconds,
    '-filter_complex', $videoOverlayFilter,
    '-map', '[v]', '-map', '0:a:0?',
    '-c:v', 'libx264', '-preset', 'veryfast', '-crf', '23',
    '-c:a', 'aac', '-b:a', '128k', '-movflags', '+faststart'
)
[void]$tasks.Add((New-GenerationTask `
    -Name 'video-watermark-image-top-right' `
    -Category 'video-watermark' `
    -Operation 'Ffmpeg' `
    -SourcePath $primaryVideo.Path `
    -OutputPath (Join-Path $outputRoot 'video\watermarks\05_image-top-right.mp4') `
    -Arguments $videoOverlayArguments `
    -Metadata ([ordered]@{
        container = 'mp4'; videoCodec = 'h264'; audioCodec = 'aac';
        width = 1280; height = 720; watermark = 'image-top-right'; watermarkSource = $watermarkImage.Path
    })))

$audioProfiles = @(
    [pscustomobject]@{ Name = 'mp3'; Extension = 'mp3'; Codec = 'libmp3lame'; Options = @('-b:a', '192k'); Container = 'mp3' },
    [pscustomobject]@{ Name = 'aac'; Extension = 'm4a'; Codec = 'aac'; Options = @('-b:a', '160k', '-movflags', '+faststart'); Container = 'm4a' },
    [pscustomobject]@{ Name = 'opus'; Extension = 'ogg'; Codec = 'libopus'; Options = @('-b:a', '96k'); Container = 'ogg' },
    [pscustomobject]@{ Name = 'vorbis'; Extension = 'ogg'; Codec = 'libvorbis'; Options = @('-q:a', '5'); Container = 'ogg' },
    [pscustomobject]@{ Name = 'flac'; Extension = 'flac'; Codec = 'flac'; Options = @(); Container = 'flac' },
    [pscustomobject]@{ Name = 'pcm-s16le'; Extension = 'wav'; Codec = 'pcm_s16le'; Options = @(); Container = 'wav' }
)

$audioIndex = 1
foreach ($audioProfile in $audioProfiles) {
    $outputPath = Join-Path $outputRoot ("audio\{0:D2}_{1}.{2}" -f $audioIndex, $audioProfile.Name, $audioProfile.Extension)
    $arguments = @('-t', [string]$DurationSeconds, '-i', $primaryAudio.Path, '-vn', '-c:a', $audioProfile.Codec)
    $arguments += @($audioProfile.Options)
    [void]$tasks.Add((New-GenerationTask `
        -Name ("audio-{0}" -f $audioProfile.Name) `
        -Category 'audio-format' `
        -Operation 'Ffmpeg' `
        -SourcePath $primaryAudio.Path `
        -OutputPath $outputPath `
        -Arguments $arguments `
        -Metadata ([ordered]@{
            container = $audioProfile.Container; audioCodec = $audioProfile.Codec;
            width = $null; height = $null; watermark = 'none'
        })))
    $audioIndex++
}

$imageFormatProfiles = @(
    [pscustomobject]@{ Name = 'jpeg'; Extension = 'jpg'; Codec = 'mjpeg'; Options = @('-q:v', '2'); MuxerOptions = @('-update', '1') },
    [pscustomobject]@{ Name = 'png'; Extension = 'png'; Codec = 'png'; Options = @(); MuxerOptions = @('-update', '1') },
    [pscustomobject]@{ Name = 'webp'; Extension = 'webp'; Codec = 'libwebp'; Options = @('-quality', '85'); MuxerOptions = @() },
    [pscustomobject]@{ Name = 'bmp'; Extension = 'bmp'; Codec = 'bmp'; Options = @(); MuxerOptions = @('-update', '1') },
    [pscustomobject]@{ Name = 'tiff'; Extension = 'tiff'; Codec = 'tiff'; Options = @(); MuxerOptions = @('-update', '1') },
    [pscustomobject]@{ Name = 'gif'; Extension = 'gif'; Codec = 'gif'; Options = @(); MuxerOptions = @() }
)

$imageIndex = 1
foreach ($imageProfile in $imageFormatProfiles) {
    $outputPath = Join-Path $outputRoot ("image\formats\{0:D2}_{1}_1280x720.{2}" -f $imageIndex, $imageProfile.Name, $imageProfile.Extension)
    $arguments = @('-i', $primaryImage.Path, '-vf', $baseScale, '-frames:v', '1', '-c:v', $imageProfile.Codec)
    $arguments += @($imageProfile.Options)
    $arguments += @($imageProfile.MuxerOptions)
    [void]$tasks.Add((New-GenerationTask `
        -Name ("image-format-{0}" -f $imageProfile.Name) `
        -Category 'image-format' `
        -Operation 'Ffmpeg' `
        -SourcePath $primaryImage.Path `
        -OutputPath $outputPath `
        -Arguments $arguments `
        -Metadata ([ordered]@{
            container = $imageProfile.Extension; videoCodec = $imageProfile.Codec;
            width = 1280; height = 720; watermark = 'none'
        })))
    $imageIndex++
}

$imageResolutionProfiles = @(
    [pscustomobject]@{ Label = 'thumbnail'; Width = 320; Height = 240; Extension = 'jpg'; Codec = 'mjpeg'; Options = @('-q:v', '3'); MuxerOptions = @('-update', '1') },
    [pscustomobject]@{ Label = 'sd'; Width = 640; Height = 480; Extension = 'png'; Codec = 'png'; Options = @(); MuxerOptions = @('-update', '1') },
    [pscustomobject]@{ Label = 'full-hd'; Width = 1920; Height = 1080; Extension = 'webp'; Codec = 'libwebp'; Options = @('-quality', '85'); MuxerOptions = @() },
    [pscustomobject]@{ Label = '4k'; Width = 3840; Height = 2160; Extension = 'jpg'; Codec = 'mjpeg'; Options = @('-q:v', '3'); MuxerOptions = @('-update', '1') }
)

$imageResolutionIndex = 1
foreach ($resolution in $imageResolutionProfiles) {
    $scaleFilter = Get-FixedScaleFilter -Width $resolution.Width -Height $resolution.Height
    $outputPath = Join-Path $outputRoot ("image\resolutions\{0:D2}_{1}_{2}x{3}.{4}" -f $imageResolutionIndex, $resolution.Label, $resolution.Width, $resolution.Height, $resolution.Extension)
    $arguments = @('-i', $primaryImage.Path, '-vf', $scaleFilter, '-frames:v', '1', '-c:v', $resolution.Codec)
    $arguments += @($resolution.Options)
    $arguments += @($resolution.MuxerOptions)
    [void]$tasks.Add((New-GenerationTask `
        -Name ("image-resolution-{0}" -f $resolution.Label) `
        -Category 'image-resolution' `
        -Operation 'Ffmpeg' `
        -SourcePath $primaryImage.Path `
        -OutputPath $outputPath `
        -Arguments $arguments `
        -Metadata ([ordered]@{
            container = $resolution.Extension; videoCodec = $resolution.Codec;
            width = $resolution.Width; height = $resolution.Height; watermark = 'none'
        })))
    $imageResolutionIndex++
}

$imageTextWatermarks = @(
    [pscustomobject]@{ Name = 'text-top-left'; Filter = $textTopLeft; Extension = 'png'; Codec = 'png'; Options = @(); MuxerOptions = @('-update', '1'); Watermark = 'text-top-left' },
    [pscustomobject]@{ Name = 'text-center-transparent'; Filter = $textCenter; Extension = 'jpg'; Codec = 'mjpeg'; Options = @('-q:v', '2'); MuxerOptions = @('-update', '1'); Watermark = 'text-center-transparent' }
)

$imageWatermarkIndex = 1
foreach ($watermarkProfile in $imageTextWatermarks) {
    $outputPath = Join-Path $outputRoot ("image\watermarks\{0:D2}_{1}.{2}" -f $imageWatermarkIndex, $watermarkProfile.Name, $watermarkProfile.Extension)
    $arguments = @('-i', $primaryImage.Path, '-vf', "$baseScale,$($watermarkProfile.Filter)", '-frames:v', '1', '-c:v', $watermarkProfile.Codec)
    $arguments += @($watermarkProfile.Options)
    $arguments += @($watermarkProfile.MuxerOptions)
    [void]$tasks.Add((New-GenerationTask `
        -Name ("image-watermark-{0}" -f $watermarkProfile.Name) `
        -Category 'image-watermark' `
        -Operation 'Ffmpeg' `
        -SourcePath $primaryImage.Path `
        -OutputPath $outputPath `
        -Arguments $arguments `
        -Metadata ([ordered]@{
            container = $watermarkProfile.Extension; videoCodec = $watermarkProfile.Codec;
            width = 1280; height = 720; watermark = $watermarkProfile.Watermark
        })))
    $imageWatermarkIndex++
}

$imageOverlayFilter = "[0:v]$baseScale,format=rgba[base];[1:v]scale=160:-1,format=rgba,colorchannelmixer=aa=0.45[wm];[base][wm]overlay=W-w-24:24,format=rgba[v]"
$imageOverlayArguments = @(
    '-i', $primaryImage.Path, '-i', $watermarkImage.Path,
    '-filter_complex', $imageOverlayFilter,
    '-map', '[v]', '-frames:v', '1', '-c:v', 'png', '-update', '1'
)
[void]$tasks.Add((New-GenerationTask `
    -Name 'image-watermark-image-top-right' `
    -Category 'image-watermark' `
    -Operation 'Ffmpeg' `
    -SourcePath $primaryImage.Path `
    -OutputPath (Join-Path $outputRoot 'image\watermarks\03_image-top-right.png') `
    -Arguments $imageOverlayArguments `
    -Metadata ([ordered]@{
        container = 'png'; videoCodec = 'png'; width = 1280; height = 720;
        watermark = 'image-top-right'; watermarkSource = $watermarkImage.Path
    })))

# 精确复制样本用于验证 SHA-512 重复检测；Copy-Item 不改变任何媒体字节。
$duplicateDefinitions = @(
    [pscustomobject]@{ Name = 'video-exact-copy'; Source = $primaryVideo; OutputName = "video_exact_copy$($primaryVideo.Extension)"; Kind = 'video' },
    [pscustomobject]@{ Name = 'audio-exact-copy'; Source = $primaryAudio; OutputName = "audio_exact_copy$($primaryAudio.Extension)"; Kind = 'audio' },
    [pscustomobject]@{ Name = 'image-exact-copy'; Source = $primaryImage; OutputName = "image_exact_copy$($primaryImage.Extension)"; Kind = 'image' }
)
foreach ($duplicate in $duplicateDefinitions) {
    [void]$tasks.Add((New-GenerationTask `
        -Name $duplicate.Name `
        -Category 'exact-duplicate' `
        -Operation 'Copy' `
        -SourcePath $duplicate.Source.Path `
        -OutputPath (Join-Path $outputRoot ("duplicates\{0}" -f $duplicate.OutputName)) `
        -Metadata ([ordered]@{
            container = $duplicate.Source.Extension.TrimStart('.'); sourceKind = $duplicate.Kind;
            width = $(if ($duplicate.Kind -eq 'audio') { $null } else { $duplicate.Source.Width });
            height = $(if ($duplicate.Kind -eq 'audio') { $null } else { $duplicate.Source.Height });
            watermark = 'none'; exactCopy = $true
        })))
}

if ($DryRun) {
    Write-Host ''
    Write-Host "DryRun：共规划 $($tasks.Count) 个任务，不创建目录或文件。"
    $taskNumber = 1
    foreach ($task in $tasks) {
        Write-Host ("{0,2}. [{1}] {2} -> {3}" -f $taskNumber, $task.Category, $task.Name, $task.OutputPath)
        $taskNumber++
    }
    return
}

$manifestRecords = New-Object System.Collections.Generic.List[object]
$successCount = 0
$skippedCount = 0
$failedCount = 0

foreach ($task in $tasks) {
    $status = 'pending'
    $errorMessage = $null
    try {
        $shouldGenerate = $true
        if ((Test-Path -LiteralPath $task.OutputPath -PathType Leaf) -and -not $Overwrite) {
            $existingFile = Get-Item -LiteralPath $task.OutputPath
            if ($existingFile.Length -gt 0) {
                $status = 'skipped-existing'
                $skippedCount++
                $shouldGenerate = $false
                Write-Host "跳过已有非空文件：$($task.OutputPath)"
            } else {
                # 上次失败留下的空文件不是有效结果，必须移除后重新生成。
                Remove-Item -LiteralPath $task.OutputPath -Force
                Write-Host "重新生成 0 字节文件：$($task.OutputPath)"
            }
        }

        if ($shouldGenerate) {
            $targetDirectory = [IO.Path]::GetDirectoryName($task.OutputPath)
            [IO.Directory]::CreateDirectory($targetDirectory) | Out-Null

            Write-Host "生成 [$($task.Category)]：$($task.OutputPath)"
            if ($task.Operation -eq 'Copy') {
                Copy-Item -LiteralPath $task.SourcePath -Destination $task.OutputPath -Force:$Overwrite
            } else {
                $commandArguments = @('-hide_banner', '-loglevel', 'warning', '-nostdin')
                $commandArguments += $(if ($Overwrite) { '-y' } else { '-n' })
                $commandArguments += @($task.Arguments)
                $commandArguments += $task.OutputPath

                # FFmpeg 按约定把普通警告写入 stderr；Windows PowerShell 5.1 下必须以退出码判断成败。
                $nativeOutput = @()
                $nativeExitCode = $null
                $previousErrorActionPreference = $ErrorActionPreference
                try {
                    $ErrorActionPreference = 'Continue'
                    $nativeOutput = @(& $ffmpegFullPath @commandArguments 2>&1)
                    $nativeExitCode = $LASTEXITCODE
                } finally {
                    $ErrorActionPreference = $previousErrorActionPreference
                }
                if ($nativeExitCode -ne 0) {
                    $nativeError = ($nativeOutput | Select-Object -Last 20) -join [Environment]::NewLine
                    throw "FFmpeg 退出码 $nativeExitCode。$([Environment]::NewLine)$nativeError"
                }
                if ($nativeOutput.Count -gt 0) {
                    Write-Verbose ($nativeOutput -join [Environment]::NewLine)
                }
            }

            $status = 'generated'
            $successCount++
        }
    } catch {
        $status = 'failed'
        $errorMessage = $_.Exception.Message
        if ($task.Operation -eq 'Ffmpeg' -and (Test-Path -LiteralPath $task.OutputPath -PathType Leaf)) {
            try {
                # 只清理当前失败任务的目标文件，防止下次运行把部分输出误判为完成。
                Remove-Item -LiteralPath $task.OutputPath -Force
            } catch {
                $errorMessage += "$([Environment]::NewLine)清理失败输出时出错：$($_.Exception.Message)"
            }
        }
        $failedCount++
        Write-Warning "任务失败：$($task.Name)。$errorMessage"
    }

    $record = [ordered]@{
        name      = $task.Name
        category  = $task.Category
        operation = $task.Operation
        source    = $task.SourcePath
        output    = $task.OutputPath
        status    = $status
        error     = $errorMessage
    }
    foreach ($entry in $task.Metadata.GetEnumerator()) {
        $record[$entry.Key] = $entry.Value
    }
    [void]$manifestRecords.Add([pscustomobject]$record)
}

$manifestPath = Join-Path $outputRoot 'manifest.json'
[IO.Directory]::CreateDirectory($outputRoot) | Out-Null
$manifest = [ordered]@{
    generatedAt = [DateTimeOffset]::Now.ToString('o')
    ffmpeg       = $ffmpegFullPath
    ffprobe      = $ffprobeFullPath
    sourceRoot   = $sourceRoot
    outputRoot   = $outputRoot
    duration     = $DurationSeconds
    overwrite    = [bool]$Overwrite
    summary      = [ordered]@{
        total = $tasks.Count
        generated = $successCount
        skipped = $skippedCount
        failed = $failedCount
    }
    files        = @($manifestRecords | ForEach-Object { $_ })
}
$manifestJson = $manifest | ConvertTo-Json -Depth 8
$utf8WithoutBom = New-Object Text.UTF8Encoding($false)
[IO.File]::WriteAllText($manifestPath, $manifestJson, $utf8WithoutBom)

Write-Host ''
Write-Host "任务完成：生成 $successCount，跳过 $skippedCount，失败 $failedCount。"
Write-Host "清单文件：$manifestPath"
if ($failedCount -gt 0) {
    throw "存在 $failedCount 个生成失败任务，请查看 manifest.json。"
}
