$ErrorActionPreference = "Stop"

Add-Type -AssemblyName System.IO.Compression.FileSystem

$RepoRoot = Split-Path -Parent $PSScriptRoot
$OutFile = Join-Path $RepoRoot "docs\ai_comm_software_assist_eval_results_focused.pptx"
$TempRoot = Join-Path $env:TEMP ("ai_comm_eval_ppt_polished_" + [Guid]::NewGuid().ToString("N"))
$Utf8NoBom = New-Object System.Text.UTF8Encoding -ArgumentList $false

$SW = 12192000
$SH = 6858000

function Write-TextFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Content
    )
    $dir = Split-Path -Parent $Path
    if ($dir -and -not (Test-Path $dir)) {
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
    }
    [System.IO.File]::WriteAllText($Path, $Content, $Utf8NoBom)
}

function Escape-Xml {
    param([string]$Text)
    if ($null -eq $Text) { return "" }
    return [System.Security.SecurityElement]::Escape($Text)
}

function New-GroupShapeXml {
    return @"
<p:nvGrpSpPr>
  <p:cNvPr id="1" name=""/>
  <p:cNvGrpSpPr/>
  <p:nvPr/>
</p:nvGrpSpPr>
<p:grpSpPr>
  <a:xfrm>
    <a:off x="0" y="0"/>
    <a:ext cx="0" cy="0"/>
    <a:chOff x="0" y="0"/>
    <a:chExt cx="0" cy="0"/>
  </a:xfrm>
</p:grpSpPr>
"@
}

function New-ShapeXml {
    param(
        [int]$Id,
        [string]$Name,
        [int64]$X,
        [int64]$Y,
        [int64]$W,
        [int64]$H,
        [string]$Fill,
        [string]$Stroke = "",
        [string]$Geom = "rect",
        [int]$StrokeWidth = 12700,
        [int]$Alpha = 100000
    )
    $fillXml = if ([string]::IsNullOrWhiteSpace($Fill)) {
        '<a:noFill/>'
    } elseif ($Alpha -lt 100000) {
        "<a:solidFill><a:srgbClr val=`"$Fill`"><a:alpha val=`"$Alpha`"/></a:srgbClr></a:solidFill>"
    } else {
        "<a:solidFill><a:srgbClr val=`"$Fill`"/></a:solidFill>"
    }
    $lineXml = if ([string]::IsNullOrWhiteSpace($Stroke)) {
        '<a:ln><a:noFill/></a:ln>'
    } else {
        "<a:ln w=`"$StrokeWidth`"><a:solidFill><a:srgbClr val=`"$Stroke`"/></a:solidFill></a:ln>"
    }

    return @"
<p:sp>
  <p:nvSpPr>
    <p:cNvPr id="$Id" name="$(Escape-Xml $Name)"/>
    <p:cNvSpPr/>
    <p:nvPr/>
  </p:nvSpPr>
  <p:spPr>
    <a:xfrm>
      <a:off x="$X" y="$Y"/>
      <a:ext cx="$W" cy="$H"/>
    </a:xfrm>
    <a:prstGeom prst="$Geom"><a:avLst/></a:prstGeom>
    $fillXml
    $lineXml
  </p:spPr>
  <p:style>
    <a:lnRef idx="0"><a:schemeClr val="accent1"/></a:lnRef>
    <a:fillRef idx="0"><a:schemeClr val="accent1"/></a:fillRef>
    <a:effectRef idx="0"><a:schemeClr val="accent1"/></a:effectRef>
    <a:fontRef idx="minor"><a:schemeClr val="tx1"/></a:fontRef>
  </p:style>
</p:sp>
"@
}

function New-ParagraphXml {
    param(
        [string]$Text,
        [int]$Size,
        [string]$Color = "172033",
        [bool]$Bold = $false,
        [string]$Align = "l",
        [bool]$Bullet = $false
    )
    $boldXml = if ($Bold) { ' b="1"' } else { "" }
    $pPr = if ($Bullet) {
        "<a:pPr marL=`"252000`" indent=`"-151200`"><a:buChar char=`"&#8226;`"/><a:defRPr sz=`"$($Size * 100)`"/></a:pPr>"
    } else {
        "<a:pPr algn=`"$Align`"/>"
    }
    return @"
<a:p>
  $pPr
  <a:r>
    <a:rPr lang="zh-CN" sz="$($Size * 100)"$boldXml dirty="0">
      <a:solidFill><a:srgbClr val="$Color"/></a:solidFill>
      <a:latin typeface="Microsoft YaHei"/>
      <a:ea typeface="Microsoft YaHei"/>
      <a:cs typeface="Microsoft YaHei"/>
    </a:rPr>
    <a:t>$(Escape-Xml $Text)</a:t>
  </a:r>
  <a:endParaRPr lang="zh-CN" sz="$($Size * 100)"/>
</a:p>
"@
}

function New-TextBoxXml {
    param(
        [int]$Id,
        [string]$Name,
        [int64]$X,
        [int64]$Y,
        [int64]$W,
        [int64]$H,
        [string]$ParagraphXml,
        [int]$L = 0,
        [int]$T = 0,
        [int]$R = 0,
        [int]$B = 0
    )
    return @"
<p:sp>
  <p:nvSpPr>
    <p:cNvPr id="$Id" name="$(Escape-Xml $Name)"/>
    <p:cNvSpPr txBox="1"/>
    <p:nvPr/>
  </p:nvSpPr>
  <p:spPr>
    <a:xfrm>
      <a:off x="$X" y="$Y"/>
      <a:ext cx="$W" cy="$H"/>
    </a:xfrm>
    <a:prstGeom prst="rect"><a:avLst/></a:prstGeom>
    <a:noFill/>
    <a:ln><a:noFill/></a:ln>
  </p:spPr>
  <p:txBody>
    <a:bodyPr wrap="square" rtlCol="0" lIns="$L" tIns="$T" rIns="$R" bIns="$B"><a:spAutoFit/></a:bodyPr>
    <a:lstStyle/>
    $ParagraphXml
  </p:txBody>
</p:sp>
"@
}

function New-BulletBoxXml {
    param(
        [int]$Id,
        [int64]$X,
        [int64]$Y,
        [int64]$W,
        [int64]$H,
        [string[]]$Bullets,
        [int]$Size = 16,
        [string]$Color = "263142"
    )
    $paragraphs = foreach ($bullet in $Bullets) {
        New-ParagraphXml -Text $bullet -Size $Size -Color $Color -Bullet $true
    }
    return New-TextBoxXml -Id $Id -Name "Bullets" -X $X -Y $Y -W $W -H $H -ParagraphXml ($paragraphs -join "`n")
}

function New-LabelXml {
    param(
        [int]$Id,
        [int64]$X,
        [int64]$Y,
        [int64]$W,
        [int64]$H,
        [string]$Text,
        [string]$Fill,
        [string]$Color = "FFFFFF",
        [int]$Size = 11
    )
    $p = New-ParagraphXml -Text $Text -Size $Size -Color $Color -Bold $true -Align "ctr"
    return @(
        (New-ShapeXml -Id $Id -Name "Label" -X $X -Y $Y -W $W -H $H -Fill $Fill -Geom "roundRect"),
        (New-TextBoxXml -Id ($Id + 1) -Name "LabelText" -X $X -Y ($Y + 55000) -W $W -H ($H - 50000) -ParagraphXml $p)
    ) -join "`n"
}

function New-CardXml {
    param(
        [int]$Id,
        [int64]$X,
        [int64]$Y,
        [int64]$W,
        [int64]$H,
        [string]$Title,
        [string]$Body,
        [string]$Accent = "0E9384",
        [string]$Fill = "FFFFFF"
    )
    $titleXml = New-ParagraphXml -Text $Title -Size 15 -Color "172033" -Bold $true
    $bodyXml = New-ParagraphXml -Text $Body -Size 11 -Color "657085"
    return @(
        (New-ShapeXml -Id $Id -Name "Card" -X $X -Y $Y -W $W -H $H -Fill $Fill -Stroke "E2E8F0" -Geom "roundRect" -StrokeWidth 9000),
        (New-ShapeXml -Id ($Id + 1) -Name "CardAccent" -X $X -Y $Y -W 105000 -H $H -Fill $Accent -Geom "rect"),
        (New-TextBoxXml -Id ($Id + 2) -Name "CardTitle" -X ($X + 230000) -Y ($Y + 150000) -W ($W - 360000) -H 280000 -ParagraphXml $titleXml),
        (New-TextBoxXml -Id ($Id + 3) -Name "CardBody" -X ($X + 230000) -Y ($Y + 520000) -W ($W - 360000) -H ($H - 610000) -ParagraphXml $bodyXml)
    ) -join "`n"
}

function New-HeaderXml {
    param(
        [int]$Id,
        [int]$Page,
        [int]$Total,
        [string]$Title,
        [string]$Section
    )
    $sectionXml = New-ParagraphXml -Text $Section -Size 10 -Color "0E9384" -Bold $true -Align "r"
    $titleXml = New-ParagraphXml -Text $Title -Size 25 -Color "111827" -Bold $true
    $pageXml = New-ParagraphXml -Text ("{0:00}/{1:00}" -f $Page, $Total) -Size 9 -Color "94A3B8" -Align "r"
    return @(
        (New-ShapeXml -Id $Id -Name "TopLineA" -X 0 -Y 0 -W 7000000 -H 84000 -Fill "0E9384"),
        (New-ShapeXml -Id ($Id + 1) -Name "TopLineB" -X 7000000 -Y 0 -W 2600000 -H 84000 -Fill "F59E0B"),
        (New-ShapeXml -Id ($Id + 2) -Name "TopLineC" -X 9600000 -Y 0 -W 2592000 -H 84000 -Fill "4F46E5"),
        (New-TextBoxXml -Id ($Id + 3) -Name "SlideTitle" -X 520000 -Y 300000 -W 8700000 -H 470000 -ParagraphXml $titleXml),
        (New-TextBoxXml -Id ($Id + 4) -Name "Section" -X 9450000 -Y 350000 -W 2000000 -H 220000 -ParagraphXml $sectionXml),
        (New-TextBoxXml -Id ($Id + 5) -Name "Page" -X 10800000 -Y 6400000 -W 850000 -H 210000 -ParagraphXml $pageXml),
        (New-ShapeXml -Id ($Id + 6) -Name "FooterDot" -X 520000 -Y 6500000 -W 95000 -H 95000 -Fill "0E9384" -Geom "ellipse"),
        (New-TextBoxXml -Id ($Id + 7) -Name "Footer" -X 680000 -Y 6420000 -W 5200000 -H 220000 -ParagraphXml (New-ParagraphXml -Text "AI辅助通信软件编程测评 | srsRAN / 5G / O-RAN" -Size 9 -Color "94A3B8"))
    ) -join "`n"
}

function New-SlideShellXml {
    param([string]$ShapeXml)
    $groupXml = New-GroupShapeXml
    return @"
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<p:sld xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
       xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"
       xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">
  <p:cSld>
    <p:spTree>
      $groupXml
      $ShapeXml
    </p:spTree>
  </p:cSld>
  <p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr>
</p:sld>
"@
}

function New-CoverSlide {
    param([int]$Page, [int]$Total)
    $id = 2
    $s = New-Object System.Collections.Generic.List[string]
    $s.Add((New-ShapeXml -Id $id -Name "Background" -X 0 -Y 0 -W $SW -H $SH -Fill "101623")); $id++
    $s.Add((New-ShapeXml -Id $id -Name "DiagonalA" -X 8500000 -Y -520000 -W 3100000 -H 8200000 -Fill "15213A" -Geom "parallelogram")); $id++
    $s.Add((New-ShapeXml -Id $id -Name "DiagonalB" -X 10300000 -Y -250000 -W 1150000 -H 7600000 -Fill "0E9384" -Alpha 68000 -Geom "parallelogram")); $id++
    $s.Add((New-ShapeXml -Id $id -Name "DiagonalC" -X 11250000 -Y 0 -W 720000 -H 6858000 -Fill "F59E0B" -Alpha 82000 -Geom "parallelogram")); $id++
    for ($i = 0; $i -lt 7; $i++) {
        $y = 960000 + $i * 430000
        $w = 2500000 - $i * 120000
        $s.Add((New-ShapeXml -Id $id -Name "SignalLine" -X 7960000 -Y $y -W $w -H 26000 -Fill "32425F" -Alpha 80000)); $id++
    }
    $s.Add((New-LabelXml -Id $id -X 720000 -Y 720000 -W 1350000 -H 360000 -Text "测评方案" -Fill "0E9384")); $id += 2
    $s.Add((New-TextBoxXml -Id $id -Name "Title" -X 710000 -Y 1280000 -W 7000000 -H 1050000 -ParagraphXml (New-ParagraphXml -Text "AI在通信软件辅助编程中的作用测评" -Size 34 -Color "FFFFFF" -Bold $true))); $id++
    $s.Add((New-TextBoxXml -Id $id -Name "Subtitle" -X 735000 -Y 2530000 -W 6500000 -H 520000 -ParagraphXml (New-ParagraphXml -Text "结果版：结合本周 srsRAN NTN beam hopping 开发样本" -Size 17 -Color "B7C4D8"))); $id++
    $chips = @(
        @("效率", "0E9384"),
        @("质量", "F59E0B"),
        @("风险", "4F46E5"),
        @("落地", "E11D48")
    )
    $x = 740000
    foreach ($chip in $chips) {
        $s.Add((New-LabelXml -Id $id -X $x -Y 3380000 -W 900000 -H 330000 -Text $chip[0] -Fill $chip[1])); $id += 2
        $x += 1040000
    }
    $coverBullets = @(
        "评估AI在代码理解、实现、测试、调试与评审中的真实增益",
        "本轮样本：NTN 多波束 / beam hopping 配置、控制器与单测",
        "输出：测评方法、实测结果、收益判断与残余风险"
    )
    $s.Add((New-BulletBoxXml -Id $id -X 775000 -Y 4200000 -W 6200000 -H 1050000 -Bullets $coverBullets -Size 15 -Color "D8E1EE")); $id++

    $stack = @("RRC", "PDCP", "RLC", "MAC", "PHY")
    $y0 = 1700000
    foreach ($layer in $stack) {
        $s.Add((New-ShapeXml -Id $id -Name "StackLayer" -X 8310000 -Y $y0 -W 1500000 -H 440000 -Fill "F8FAFC" -Alpha 95000 -Stroke "5A6B86" -Geom "roundRect")); $id++
        $s.Add((New-TextBoxXml -Id $id -Name "LayerText" -X 8310000 -Y ($y0 + 105000) -W 1500000 -H 190000 -ParagraphXml (New-ParagraphXml -Text $layer -Size 14 -Color "172033" -Bold $true -Align "ctr"))); $id++
        $y0 += 560000
    }
    $s.Add((New-ShapeXml -Id $id -Name "AIHub" -X 10200000 -Y 2550000 -W 920000 -H 920000 -Fill "0E9384" -Geom "ellipse")); $id++
    $s.Add((New-TextBoxXml -Id $id -Name "AIText" -X 10200000 -Y 2820000 -W 920000 -H 250000 -ParagraphXml (New-ParagraphXml -Text "AI" -Size 24 -Color "FFFFFF" -Bold $true -Align "ctr"))); $id++
    foreach ($y in @(1940000, 2500000, 3060000, 3620000, 4180000)) {
        $s.Add((New-ShapeXml -Id $id -Name "Connector" -X 9800000 -Y $y -W 520000 -H 26000 -Fill "77D4C6")); $id++
    }
    return New-SlideShellXml -ShapeXml ($s -join "`n")
}

function New-ExecutiveSummarySlide {
    param([int]$Page, [int]$Total)
    $id = 2; $s = New-Object System.Collections.Generic.List[string]
    $s.Add((New-ShapeXml -Id $id -Name "Background" -X 0 -Y 0 -W $SW -H $SH -Fill "F8FAFC")); $id++
    $s.Add((New-HeaderXml -Id $id -Page $Page -Total $Total -Title "执行摘要：本轮测评给出的答案" -Section "00 / 结论先行")); $id += 8

    $s.Add((New-ShapeXml -Id $id -Name "SummaryBand" -X 650000 -Y 980000 -W 10880000 -H 760000 -Fill "172033" -Geom "roundRect")); $id++
    $s.Add((New-TextBoxXml -Id $id -Name "SummaryText" -X 940000 -Y 1180000 -W 10300000 -H 260000 -ParagraphXml (New-ParagraphXml -Text "AI在本轮通信软件开发中表现为有效的工程副驾驶：能串联跨模块实现、补齐核心单测并定位验证路径，但实时生命周期和协议语义仍需人工兜底。" -Size 16 -Color "FFFFFF" -Bold $true -Align "ctr"))); $id++

    $kpis = @(
        @("81/100", "综合评分", "可行性样本成立", "0E9384"),
        @("3套", "干净基线", "4bf1543 / 0e4a114 / df8cfef", "4F46E5"),
        @("1/1", "CTest通过", "ntn_test Test time = 0.02 sec", "F59E0B"),
        @("4/4", "gtest通过", "4 suites，13 ms total", "14B8A6")
    )
    $x = 780000
    foreach ($k in $kpis) {
        $s.Add((New-ShapeXml -Id $id -Name "KpiCard" -X $x -Y 2200000 -W 2500000 -H 1600000 -Fill "FFFFFF" -Stroke "E2E8F0" -Geom "roundRect")); $id++
        $s.Add((New-ShapeXml -Id $id -Name "KpiTop" -X $x -Y 2200000 -W 2500000 -H 140000 -Fill $k[3])); $id++
        $s.Add((New-TextBoxXml -Id $id -Name "KpiValue" -X $x -Y 2540000 -W 2500000 -H 420000 -ParagraphXml (New-ParagraphXml -Text $k[0] -Size 31 -Color $k[3] -Bold $true -Align "ctr"))); $id++
        $s.Add((New-TextBoxXml -Id $id -Name "KpiName" -X $x -Y 3060000 -W 2500000 -H 230000 -ParagraphXml (New-ParagraphXml -Text $k[1] -Size 14 -Color "172033" -Bold $true -Align "ctr"))); $id++
        $s.Add((New-TextBoxXml -Id $id -Name "KpiBody" -X ($x + 210000) -Y 3410000 -W 2080000 -H 260000 -ParagraphXml (New-ParagraphXml -Text $k[2] -Size 10 -Color "657085" -Align "ctr"))); $id++
        $x += 2700000
    }

    $takeaways = @(
        "当前工程相对上游主线新增了 beam hopping 能力链路，不是局部小修。",
        "AI收益主要体现在代码理解、glue code、测试点枚举和验证路径定位。",
        "主要风险集中在 stop 后周期 tick、parser负向测试、late tick 行为和系统级验证。"
    )
    $s.Add((New-ShapeXml -Id $id -Name "TakeawayPanel" -X 920000 -Y 4350000 -W 10350000 -H 950000 -Fill "E8F6F3" -Stroke "A7D8D1" -Geom "roundRect")); $id++
    $s.Add((New-BulletBoxXml -Id $id -X 1250000 -Y 4540000 -W 9600000 -H 520000 -Bullets $takeaways -Size 13 -Color "0F3D46")); $id++
    return New-SlideShellXml -ShapeXml ($s -join "`n")
}

function New-SectionNtnSlide {
    param([int]$Page, [int]$Total)
    $id = 2; $s = New-Object System.Collections.Generic.List[string]
    $s.Add((New-ShapeXml -Id $id -Name "Background" -X 0 -Y 0 -W $SW -H $SH -Fill "101623")); $id++
    $s.Add((New-ShapeXml -Id $id -Name "AccentA" -X 0 -Y 0 -W 12192000 -H 90000 -Fill "0E9384")); $id++
    $s.Add((New-ShapeXml -Id $id -Name "Diagonal" -X 8700000 -Y -500000 -W 2600000 -H 8000000 -Fill "172033" -Geom "parallelogram")); $id++
    $s.Add((New-TextBoxXml -Id $id -Name "SectionNo" -X 720000 -Y 1120000 -W 1800000 -H 340000 -ParagraphXml (New-ParagraphXml -Text "第一部分" -Size 17 -Color "77D4C6" -Bold $true))); $id++
    $s.Add((New-TextBoxXml -Id $id -Name "Title" -X 720000 -Y 1660000 -W 7600000 -H 760000 -ParagraphXml (New-ParagraphXml -Text "NTN测评对象" -Size 34 -Color "FFFFFF" -Bold $true))); $id++
    $s.Add((New-TextBoxXml -Id $id -Name "Subtitle" -X 750000 -Y 2640000 -W 7300000 -H 360000 -ParagraphXml (New-ParagraphXml -Text "回答：这次到底拿什么通信软件任务来测AI辅助编程？" -Size 16 -Color "B7C4D8"))); $id++
    $points = @("测评对象：NTN 多波束 / beam hopping 开发样本", "功能链路：远程命令、O-DU hopping session、SIB19更新、核心算法", "验证链路：ntn_test 单测 + CTest 目标复跑")
    $s.Add((New-BulletBoxXml -Id $id -X 820000 -Y 3500000 -W 7200000 -H 950000 -Bullets $points -Size 15 -Color "D8E1EE")); $id++
    $s.Add((New-ShapeXml -Id $id -Name "MetricBox" -X 8800000 -Y 2020000 -W 2350000 -H 1800000 -Fill "F8FAFC" -Alpha 96000 -Stroke "5A6B86" -Geom "roundRect")); $id++
    $s.Add((New-TextBoxXml -Id $id -Name "Metric" -X 8800000 -Y 2380000 -W 2350000 -H 430000 -ParagraphXml (New-ParagraphXml -Text "+482" -Size 34 -Color "0E9384" -Bold $true -Align "ctr"))); $id++
    $s.Add((New-TextBoxXml -Id $id -Name "MetricLabel" -X 8950000 -Y 2940000 -W 2050000 -H 320000 -ParagraphXml (New-ParagraphXml -Text "上游对比仅作为样本规模证据" -Size 12 -Color "172033" -Bold $true -Align "ctr"))); $id++
    return New-SlideShellXml -ShapeXml ($s -join "`n")
}

function New-SectionTestSlide {
    param([int]$Page, [int]$Total)
    $id = 2; $s = New-Object System.Collections.Generic.List[string]
    $s.Add((New-ShapeXml -Id $id -Name "Background" -X 0 -Y 0 -W $SW -H $SH -Fill "101623")); $id++
    $s.Add((New-ShapeXml -Id $id -Name "AccentA" -X 0 -Y 0 -W 12192000 -H 90000 -Fill "F59E0B")); $id++
    $s.Add((New-ShapeXml -Id $id -Name "Diagonal" -X 8700000 -Y -500000 -W 2600000 -H 8000000 -Fill "172033" -Geom "parallelogram")); $id++
    $s.Add((New-TextBoxXml -Id $id -Name "SectionNo" -X 720000 -Y 1120000 -W 1800000 -H 340000 -ParagraphXml (New-ParagraphXml -Text "第二部分" -Size 17 -Color "FCD34D" -Bold $true))); $id++
    $s.Add((New-TextBoxXml -Id $id -Name "Title" -X 720000 -Y 1660000 -W 7600000 -H 760000 -ParagraphXml (New-ParagraphXml -Text "测试运行与测评结果" -Size 34 -Color "FFFFFF" -Bold $true))); $id++
    $s.Add((New-TextBoxXml -Id $id -Name "Subtitle" -X 750000 -Y 2640000 -W 7300000 -H 360000 -ParagraphXml (New-ParagraphXml -Text "回答：实际跑了什么？结果是否通过？AI辅助效果如何评分？还有哪些风险？" -Size 16 -Color "B7C4D8"))); $id++
    $points = @("构建：cmake --build . --target ntn_test -j2", "测试：ctest -R ntn_test --output-on-failure", "结果：CTest 1/1 Passed；gtest 4/4 Passed；综合评分 81/100")
    $s.Add((New-BulletBoxXml -Id $id -X 820000 -Y 3500000 -W 7200000 -H 950000 -Bullets $points -Size 15 -Color "D8E1EE")); $id++
    $s.Add((New-ShapeXml -Id $id -Name "MetricBox" -X 8800000 -Y 2020000 -W 2350000 -H 1800000 -Fill "F8FAFC" -Alpha 96000 -Stroke "5A6B86" -Geom "roundRect")); $id++
    $s.Add((New-TextBoxXml -Id $id -Name "Metric" -X 8800000 -Y 2380000 -W 2350000 -H 430000 -ParagraphXml (New-ParagraphXml -Text "13 ms" -Size 32 -Color "F59E0B" -Bold $true -Align "ctr"))); $id++
    $s.Add((New-TextBoxXml -Id $id -Name "MetricLabel" -X 8950000 -Y 2940000 -W 2050000 -H 320000 -ParagraphXml (New-ParagraphXml -Text "gtest 4个用例总运行时间" -Size 12 -Color "172033" -Bold $true -Align "ctr"))); $id++
    return New-SlideShellXml -ShapeXml ($s -join "`n")
}

function New-ObservedSampleSlide {
    param([int]$Page, [int]$Total)
    $id = 2; $s = New-Object System.Collections.Generic.List[string]
    $s.Add((New-ShapeXml -Id $id -Name "Background" -X 0 -Y 0 -W $SW -H $SH -Fill "F8FAFC")); $id++
    $s.Add((New-HeaderXml -Id $id -Page $Page -Total $Total -Title "本周 srsRAN 实测样本" -Section "00 / 实测数据")); $id += 8
    $s.Add((New-ShapeXml -Id $id -Name "Hero" -X 620000 -Y 1050000 -W 10950000 -H 700000 -Fill "172033" -Geom "roundRect")); $id++
    $s.Add((New-TextBoxXml -Id $id -Name "HeroText" -X 920000 -Y 1230000 -W 10300000 -H 260000 -ParagraphXml (New-ParagraphXml -Text "这版PPT不只讲方法，而是把 2026-05-06 的 NTN beam hopping 开发与验证结果填入测评样本。" -Size 17 -Color "FFFFFF" -Bold $true))); $id++

    $cards = @(
        @("时间窗口", "2026-05-06 22:50 - 23:08，源码修改、构建产物和CTest日志集中出现。", "0E9384"),
        @("改动范围", "13个NTN核心/测试文件 + 5个O-DU NTN集成文件，覆盖算法、命令、session和单测。", "4F46E5"),
        @("代码规模", "NTN核心选择路径 1,416 行；O-DU NTN集成选择路径 1,077 行。", "F59E0B"),
        @("验证结果", "WSL重新构建 ntn_test；CTest 1/1 Passed；gtest 4/4 Passed，13 ms。", "E11D48")
    )
    $coords = @(@(650000,2100000),@(6220000,2100000),@(650000,3800000),@(6220000,3800000))
    for ($i=0; $i -lt $cards.Count; $i++) {
        $s.Add((New-CardXml -Id $id -X $coords[$i][0] -Y $coords[$i][1] -W 5100000 -H 1220000 -Title $cards[$i][0] -Body $cards[$i][1] -Accent $cards[$i][2])); $id += 4
    }
    $s.Add((New-ShapeXml -Id $id -Name "Caveat" -X 650000 -Y 5450000 -W 10670000 -H 420000 -Fill "E8F6F3" -Stroke "A7D8D1" -Geom "roundRect")); $id++
    $s.Add((New-TextBoxXml -Id $id -Name "CaveatText" -X 900000 -Y 5570000 -W 10150000 -H 170000 -ParagraphXml (New-ParagraphXml -Text "口径说明：当前目录不是git仓库，因此本页采用文件时间、源码内容、构建产物和CTest结果作为样本证据；效率增益仍需后续人工对照组补齐。" -Size 11 -Color "0F3D46" -Bold $true -Align "ctr"))); $id++
    return New-SlideShellXml -ShapeXml ($s -join "`n")
}

function New-TargetSlide {
    param([int]$Page, [int]$Total)
    $id = 2; $s = New-Object System.Collections.Generic.List[string]
    $s.Add((New-ShapeXml -Id $id -Name "Background" -X 0 -Y 0 -W $SW -H $SH -Fill "F6F7F9")); $id++
    $s.Add((New-HeaderXml -Id $id -Page $Page -Total $Total -Title "为什么要测：AI到底帮到了哪里" -Section "01 / 测评目标")); $id += 8
    $s.Add((New-ShapeXml -Id $id -Name "Hero" -X 600000 -Y 1050000 -W 11000000 -H 720000 -Fill "172033" -Geom "roundRect")); $id++
    $s.Add((New-TextBoxXml -Id $id -Name "HeroText" -X 910000 -Y 1230000 -W 10200000 -H 270000 -ParagraphXml (New-ParagraphXml -Text "目标不是证明AI万能，而是找到可复用、可度量、可管控的协作方式。" -Size 18 -Color "FFFFFF" -Bold $true))); $id++
    $cards = @(
        @("01 量化增益", "完成时间、调试轮次、上下文查找时间和返工成本。", "0E9384"),
        @("02 找到场景", "识别AI最适合介入的代码理解、测试、日志和文档环节。", "4F46E5"),
        @("03 明确边界", "区分可自动化辅助和必须专家把关的协议与实时路径。", "F59E0B"),
        @("04 沉淀流程", "形成提示规范、评审机制、CI门禁和安全约束。", "E11D48")
    )
    $coords = @(@(650000,2100000),@(6220000,2100000),@(650000,3850000),@(6220000,3850000))
    for ($i=0; $i -lt $cards.Count; $i++) {
        $s.Add((New-CardXml -Id $id -X $coords[$i][0] -Y $coords[$i][1] -W 5100000 -H 1280000 -Title $cards[$i][0] -Body $cards[$i][1] -Accent $cards[$i][2])); $id += 4
    }
    return New-SlideShellXml -ShapeXml ($s -join "`n")
}

function New-SrsranCaseSlide {
    param([int]$Page, [int]$Total)
    $id = 2; $s = New-Object System.Collections.Generic.List[string]
    $s.Add((New-ShapeXml -Id $id -Name "Background" -X 0 -Y 0 -W $SW -H $SH -Fill "F6F7F9")); $id++
    $s.Add((New-HeaderXml -Id $id -Page $Page -Total $Total -Title "实际改动链路：NTN beam hopping" -Section "01 / srsRAN案例")); $id += 8
    $s.Add((New-ShapeXml -Id $id -Name "MainBand" -X 610000 -Y 1110000 -W 10970000 -H 520000 -Fill "172033" -Geom "roundRect")); $id++
    $s.Add((New-TextBoxXml -Id $id -Name "MainBandText" -X 880000 -Y 1250000 -W 10400000 -H 210000 -ParagraphXml (New-ParagraphXml -Text "从远程JSON命令到O-DU配置管理，再到beam hopping表、控制器、SIB19更新和单元测试，形成一个可验证的开发闭环。" -Size 15 -Color "FFFFFF" -Bold $true))); $id++

    $items = @(
        @("远程配置入口", "解析 beam_hopping / stop_beam_hopping；校验 n_active、dwell_frames、网格边界、周期不超过1024 SFN。", "0E9384"),
        @("O-DU管理器集成", "校验网格与跳波参数；启动/停止 hopping session；用时间映射和星历更新生成SIB19。", "4F46E5"),
        @("核心算法", "3相位颜色交织的 hopping table；支持 cycle_frames 不整除 SFN wrap；按 dwell_frames 选择活动波束。", "F59E0B"),
        @("控制器与验证", "slot tick 到达边界后提前调度下一 dwell；按beam中心计算TA；用 gtest 覆盖关键路径。", "E11D48")
    )
    $x0 = 650000; $y0 = 2050000; $w = 5100000; $h = 1230000; $gapX = 530000; $gapY = 360000
    for ($i=0; $i -lt $items.Count; $i++) {
        $row = [math]::Floor($i / 2); $col = $i % 2
        $x = $x0 + $col * ($w + $gapX); $y = $y0 + $row * ($h + $gapY)
        $s.Add((New-CardXml -Id $id -X $x -Y $y -W $w -H $h -Title $items[$i][0] -Body $items[$i][1] -Accent $items[$i][2])); $id += 4
    }

    $s.Add((New-ShapeXml -Id $id -Name "Evidence" -X 650000 -Y 5300000 -W 10680000 -H 520000 -Fill "FFFFFF" -Stroke "E2E8F0" -Geom "roundRect")); $id++
    $evidence = "证据文件：include/srsran/ntn/beam_hopping_*.h，lib/ntn/beam_hopping_*.cpp，flexible_o_du_ntn_configuration_manager_factory.cpp，ntn_config_update_remote_command_factory.cpp，tests/unittests/ntn/ntn_multibeam_test.cpp。"
    $s.Add((New-TextBoxXml -Id $id -Name "EvidenceText" -X 900000 -Y 5460000 -W 10150000 -H 190000 -ParagraphXml (New-ParagraphXml -Text $evidence -Size 10 -Color "657085" -Align "ctr"))); $id++
    return New-SlideShellXml -ShapeXml ($s -join "`n")
}

function New-UpstreamBaselineSlide {
    param([int]$Page, [int]$Total)
    $id = 2; $s = New-Object System.Collections.Generic.List[string]
    $s.Add((New-ShapeXml -Id $id -Name "Background" -X 0 -Y 0 -W $SW -H $SH -Fill "F8FAFC")); $id++
    $s.Add((New-HeaderXml -Id $id -Page $Page -Total $Total -Title "新增测评方向：下载干净代码做基线" -Section "02 / 上游对比")); $id += 8

    $s.Add((New-ShapeXml -Id $id -Name "Hero" -X 650000 -Y 1050000 -W 10880000 -H 610000 -Fill "172033" -Geom "roundRect")); $id++
    $s.Add((New-TextBoxXml -Id $id -Name "HeroText" -X 940000 -Y 1220000 -W 10300000 -H 220000 -ParagraphXml (New-ParagraphXml -Text "为避免只看当前目录的主观判断，本轮额外下载三套干净代码，用上游基线对比当前 NTN beam hopping 改动。" -Size 15 -Color "FFFFFF" -Bold $true))); $id++

    $bases = @(
        @("srsRAN Project 归档主线", "GitHub: srsran/srsRAN_Project", "main @ 4bf1543", "项目已归档，作为srsRAN历史基线", "64748B"),
        @("OCUDU 当前主线", "GitLab: ocudu/ocudu", "main @ 0e4a114", "srsRAN后续开发迁移后的当前主线", "0E9384"),
        @("OCUDU NTN参考分支", "GitLab: ocudu/ocudu", "pr_add_ntn_4 @ df8cfef", "非主线，仅作为NTN方向参考", "4F46E5")
    )
    $x = 760000
    foreach ($b in $bases) {
        $s.Add((New-ShapeXml -Id $id -Name "BaselineCard" -X $x -Y 2050000 -W 3300000 -H 2450000 -Fill "FFFFFF" -Stroke "E2E8F0" -Geom "roundRect")); $id++
        $s.Add((New-ShapeXml -Id $id -Name "BaselineTop" -X $x -Y 2050000 -W 3300000 -H 160000 -Fill $b[4])); $id++
        $s.Add((New-TextBoxXml -Id $id -Name "BaselineTitle" -X ($x + 230000) -Y 2360000 -W 2850000 -H 320000 -ParagraphXml (New-ParagraphXml -Text $b[0] -Size 15 -Color "172033" -Bold $true))); $id++
        $s.Add((New-TextBoxXml -Id $id -Name "BaselineRepo" -X ($x + 230000) -Y 2860000 -W 2850000 -H 220000 -ParagraphXml (New-ParagraphXml -Text $b[1] -Size 11 -Color "657085"))); $id++
        $s.Add((New-LabelXml -Id $id -X ($x + 230000) -Y 3300000 -W 1550000 -H 330000 -Text $b[2] -Fill $b[4] -Size 9)); $id += 2
        $s.Add((New-TextBoxXml -Id $id -Name "BaselineNote" -X ($x + 230000) -Y 3820000 -W 2850000 -H 340000 -ParagraphXml (New-ParagraphXml -Text $b[3] -Size 10 -Color "475569"))); $id++
        $x += 3600000
    }

    $s.Add((New-ShapeXml -Id $id -Name "PathBand" -X 760000 -Y 5050000 -W 10550000 -H 610000 -Fill "E8F6F3" -Stroke "A7D8D1" -Geom "roundRect")); $id++
    $s.Add((New-TextBoxXml -Id $id -Name "PathText" -X 1020000 -Y 5220000 -W 10000000 -H 210000 -ParagraphXml (New-ParagraphXml -Text "本地基线路径：.analysis_baselines/srsran_project_main_4bf1543、ocudu_main_0e4a114、ocudu_pr_add_ntn_4_df8cfef" -Size 11 -Color "0F3D46" -Bold $true -Align "ctr"))); $id++
    return New-SlideShellXml -ShapeXml ($s -join "`n")
}

function New-UpstreamDiffSlide {
    param([int]$Page, [int]$Total)
    $id = 2; $s = New-Object System.Collections.Generic.List[string]
    $s.Add((New-ShapeXml -Id $id -Name "Background" -X 0 -Y 0 -W $SW -H $SH -Fill "F6F7F9")); $id++
    $s.Add((New-HeaderXml -Id $id -Page $Page -Total $Total -Title "样本规模证据：上游对比只做支撑" -Section "样本证据")); $id += 8

    $headers = @("代码集", "NTN核心", "O-DU集成", "NTN单测", "结论")
    $xs = @(760000, 2900000, 5050000, 7200000, 9350000)
    $widths = @(2000000, 1900000, 1900000, 1900000, 1900000)
    for ($i=0; $i -lt $headers.Count; $i++) {
        $s.Add((New-ShapeXml -Id $id -Name "HeaderCell" -X $xs[$i] -Y 1230000 -W $widths[$i] -H 520000 -Fill "172033" -Stroke "172033")); $id++
        $s.Add((New-TextBoxXml -Id $id -Name "HeaderText" -X $xs[$i] -Y 1390000 -W $widths[$i] -H 160000 -ParagraphXml (New-ParagraphXml -Text $headers[$i] -Size 11 -Color "FFFFFF" -Bold $true -Align "ctr"))); $id++
    }

    $rows = @(
        @("当前工程", "1,416 行", "1,077 行", "ntn_test: 4用例", "本轮测评样本"),
        @("srsRAN归档主线", "115 行", "210 行", "无 ntn 单测", "基础骨架"),
        @("OCUDU主线", "181 行", "122 行", "无 ntn 单测", "基础骨架"),
        @("OCUDU pr_add_ntn_4", "4,548 行", "偏 assistance info", "1,842 行测试", "方向不同")
    )
    $colors = @("E8F6F3", "FFFFFF", "FFFFFF", "EEF2FF")
    $y = 1810000
    for ($r=0; $r -lt $rows.Count; $r++) {
        for ($c=0; $c -lt $headers.Count; $c++) {
            $fill = $colors[$r]
            $s.Add((New-ShapeXml -Id $id -Name "TableCell" -X $xs[$c] -Y $y -W $widths[$c] -H 680000 -Fill $fill -Stroke "E2E8F0")); $id++
            $bold = ($c -eq 0 -or $r -eq 0)
            $color = if ($r -eq 0) { "0F3D46" } else { "263142" }
            $s.Add((New-TextBoxXml -Id $id -Name "CellText" -X ($xs[$c] + 90000) -Y ($y + 210000) -W ($widths[$c] - 180000) -H 190000 -ParagraphXml (New-ParagraphXml -Text $rows[$r][$c] -Size 10 -Color $color -Bold $bold -Align "ctr"))); $id++
        }
        $y += 680000
    }

    $s.Add((New-ShapeXml -Id $id -Name "Conclusion" -X 760000 -Y 4950000 -W 10550000 -H 780000 -Fill "172033" -Geom "roundRect")); $id++
    $s.Add((New-TextBoxXml -Id $id -Name "ConclusionText" -X 1040000 -Y 5140000 -W 9950000 -H 270000 -ParagraphXml (New-ParagraphXml -Text "这页只说明测评样本有实际增量：相对srsRAN归档主线，NTN核心头文件 +482 行，O-DU NTN glue code 净增 867 行。" -Size 13 -Color "FFFFFF" -Bold $true -Align "ctr"))); $id++
    return New-SlideShellXml -ShapeXml ($s -join "`n")
}

function New-UpstreamImpactSlide {
    param([int]$Page, [int]$Total)
    $id = 2; $s = New-Object System.Collections.Generic.List[string]
    $s.Add((New-ShapeXml -Id $id -Name "Background" -X 0 -Y 0 -W $SW -H $SH -Fill "F8FAFC")); $id++
    $s.Add((New-HeaderXml -Id $id -Page $Page -Total $Total -Title "上游对比对测评结论的影响" -Section "04 / 结论增强")); $id += 8

    $cards = @(
        @("结论增强 1", "AI的价值不是只改单点代码，而是跨模块串联：远程命令、O-DU配置、核心算法、SIB19更新和单测接入。", "0E9384"),
        @("结论增强 2", "当前工程相对上游主线新增了多波束 hopping 能力，但测试覆盖仍偏窄，需要补 parser/stop/集成/系统场景。", "4F46E5"),
        @("结论增强 3", "OCUDU NTN参考分支偏 orbital/ephemeris/assistance-info，当前方向偏 beam hopping，后续迁移需要处理模型差异。", "F59E0B"),
        @("结论增强 4", "评分 81/100 保持不变：功能链路更有价值，但上游对比也暴露了更明确的测试和生命周期缺口。", "E11D48")
    )
    $coords = @(@(760000,1260000),@(6350000,1260000),@(760000,3400000),@(6350000,3400000))
    for ($i=0; $i -lt $cards.Count; $i++) {
        $s.Add((New-CardXml -Id $id -X $coords[$i][0] -Y $coords[$i][1] -W 5050000 -H 1500000 -Title $cards[$i][0] -Body $cards[$i][1] -Accent $cards[$i][2])); $id += 4
    }

    $s.Add((New-ShapeXml -Id $id -Name "Bottom" -X 760000 -Y 5480000 -W 10550000 -H 430000 -Fill "E8F6F3" -Stroke "A7D8D1" -Geom "roundRect")); $id++
    $s.Add((New-TextBoxXml -Id $id -Name "BottomText" -X 1010000 -Y 5610000 -W 10000000 -H 160000 -ParagraphXml (New-ParagraphXml -Text "新增证据已写入 docs/ai_comm_eval_upstream_compare_2026-05-07.md，可用于答辩时说明基线来源和对比口径。" -Size 12 -Color "0F3D46" -Bold $true -Align "ctr"))); $id++
    return New-SlideShellXml -ShapeXml ($s -join "`n")
}

function New-EngineeringSlide {
    param([int]$Page, [int]$Total)
    $id = 2; $s = New-Object System.Collections.Generic.List[string]
    $s.Add((New-ShapeXml -Id $id -Name "Background" -X 0 -Y 0 -W $SW -H $SH -Fill "F8FAFC")); $id++
    $s.Add((New-HeaderXml -Id $id -Page $Page -Total $Total -Title "通信软件为什么特别适合也特别难" -Section "02 / 工程特点")); $id += 8
    $s.Add((New-ShapeXml -Id $id -Name "Center" -X 4630000 -Y 2250000 -W 2850000 -H 1250000 -Fill "172033" -Geom "roundRect")); $id++
    $s.Add((New-TextBoxXml -Id $id -Name "CenterText" -X 4630000 -Y 2630000 -W 2850000 -H 260000 -ParagraphXml (New-ParagraphXml -Text "通信软件工程约束" -Size 18 -Color "FFFFFF" -Bold $true -Align "ctr"))); $id++
    $items = @(
        @("协议栈层次深", "PHY/MAC/RLC/PDCP/RRC/NGAP等模块互相影响", 830000, 1410000, "0E9384"),
        @("实时性能敏感", "线程、定时器、吞吐、时延和内存路径都会改变结果", 7730000, 1410000, "F59E0B"),
        @("标准语义强", "3GPP/O-RAN术语、状态机和消息字段容易误读", 830000, 4150000, "4F46E5"),
        @("验证环境复杂", "单元测试、仿真、集成测试、日志回放和空口场景缺一不可", 7730000, 4150000, "E11D48")
    )
    foreach ($it in $items) {
        $s.Add((New-CardXml -Id $id -X $it[2] -Y $it[3] -W 3600000 -H 1120000 -Title $it[0] -Body $it[1] -Accent $it[4])); $id += 4
    }
    $s.Add((New-ShapeXml -Id $id -Name "Line1" -X 3840000 -Y 2400000 -W 790000 -H 26000 -Fill "CBD5E1")); $id++
    $s.Add((New-ShapeXml -Id $id -Name "Line2" -X 7480000 -Y 2400000 -W 790000 -H 26000 -Fill "CBD5E1")); $id++
    $s.Add((New-ShapeXml -Id $id -Name "Line3" -X 3840000 -Y 4630000 -W 790000 -H 26000 -Fill "CBD5E1")); $id++
    $s.Add((New-ShapeXml -Id $id -Name "Line4" -X 7480000 -Y 4630000 -W 790000 -H 26000 -Fill "CBD5E1")); $id++
    return New-SlideShellXml -ShapeXml ($s -join "`n")
}

function New-AiFindingsSlide {
    param([int]$Page, [int]$Total)
    $id = 2; $s = New-Object System.Collections.Generic.List[string]
    $s.Add((New-ShapeXml -Id $id -Name "Background" -X 0 -Y 0 -W $SW -H $SH -Fill "F8FAFC")); $id++
    $s.Add((New-HeaderXml -Id $id -Page $Page -Total $Total -Title "基于本轮改动的 AI 作用评估" -Section "02 / 结果观察")); $id += 8
    $rows = @(
        @("代码理解", "快速定位 NTN、flexible_o_du、远程命令、CMake和测试入口之间的关系。", "收益高", "0E9384"),
        @("实现辅助", "适合生成 glue code、参数校验、状态管理骨架和边界保护。", "收益中高", "4F46E5"),
        @("测试补齐", "能把隐含边界转成用例：颜色约束、SFN非整除循环、TA计算、tick边界。", "收益高", "F59E0B"),
        @("调试验证", "能根据构建目录和日志找到 WSL/CTest 真实验证路径，并复跑目标测试。", "收益中", "14B8A6"),
        @("协议把关", "SIB19语义、空口行为、实时调度裕量仍需要通信专家和系统级验证兜底。", "需人工", "E11D48")
    )
    $y = 1180000
    foreach ($r in $rows) {
        $s.Add((New-ShapeXml -Id $id -Name "FindingRow" -X 760000 -Y $y -W 10650000 -H 730000 -Fill "FFFFFF" -Stroke "E2E8F0" -Geom "roundRect")); $id++
        $s.Add((New-ShapeXml -Id $id -Name "FindingDot" -X 1040000 -Y ($y + 235000) -W 250000 -H 250000 -Fill $r[3] -Geom "ellipse")); $id++
        $s.Add((New-TextBoxXml -Id $id -Name "FindingName" -X 1460000 -Y ($y + 170000) -W 1450000 -H 230000 -ParagraphXml (New-ParagraphXml -Text $r[0] -Size 15 -Color "172033" -Bold $true))); $id++
        $s.Add((New-TextBoxXml -Id $id -Name "FindingBody" -X 3060000 -Y ($y + 180000) -W 6100000 -H 240000 -ParagraphXml (New-ParagraphXml -Text $r[1] -Size 12 -Color "475569"))); $id++
        $s.Add((New-LabelXml -Id $id -X 9500000 -Y ($y + 210000) -W 1200000 -H 300000 -Text $r[2] -Fill $r[3] -Size 10)); $id += 2
        $y += 860000
    }
    $s.Add((New-ShapeXml -Id $id -Name "Summary" -X 760000 -Y 5700000 -W 10650000 -H 440000 -Fill "172033" -Geom "roundRect")); $id++
    $s.Add((New-TextBoxXml -Id $id -Name "SummaryText" -X 1030000 -Y 5830000 -W 10100000 -H 180000 -ParagraphXml (New-ParagraphXml -Text "初步结论：AI更像「工程副驾驶」，对探索、补齐和验证很有用；最终协议正确性和系统行为仍要由CI与专家评审闭环保证。" -Size 13 -Color "FFFFFF" -Bold $true -Align "ctr"))); $id++
    return New-SlideShellXml -ShapeXml ($s -join "`n")
}

function New-TouchpointSlide {
    param([int]$Page, [int]$Total)
    $id = 2; $s = New-Object System.Collections.Generic.List[string]
    $s.Add((New-ShapeXml -Id $id -Name "Background" -X 0 -Y 0 -W $SW -H $SH -Fill "F6F7F9")); $id++
    $s.Add((New-HeaderXml -Id $id -Page $Page -Total $Total -Title "AI可介入的六类编程活动" -Section "03 / 介入环节")); $id += 8
    $s.Add((New-ShapeXml -Id $id -Name "Band" -X 610000 -Y 1220000 -W 10970000 -H 440000 -Fill "E8F6F3" -Geom "roundRect")); $id++
    $s.Add((New-TextBoxXml -Id $id -Name "BandText" -X 900000 -Y 1330000 -W 10200000 -H 190000 -ParagraphXml (New-ParagraphXml -Text "越靠近代码理解、测试补齐和日志初筛，收益越稳定；越靠近协议语义和实时路径，越需要专家审查。" -Size 14 -Color "0F3D46" -Bold $true))); $id++
    $steps = @(
        @("理解", "模块职责 / 调用链 / 接口约定", "0E9384"),
        @("实现", "配置扩展 / 字段处理 / 错误路径", "4F46E5"),
        @("测试", "单元测试 / mock / 边界条件", "F59E0B"),
        @("调试", "日志归因 / 崩溃栈 / 失败最小化", "E11D48"),
        @("优化", "热点路径 / 锁竞争 / 内存分配", "14B8A6"),
        @("评审", "变更说明 / 风险清单 / 接口文档", "7C3AED")
    )
    $x = 690000
    for ($i=0; $i -lt $steps.Count; $i++) {
        $s.Add((New-ShapeXml -Id $id -Name "Stage" -X $x -Y 2280000 -W 1650000 -H 2100000 -Fill "FFFFFF" -Stroke "E2E8F0" -Geom "roundRect")); $id++
        $s.Add((New-ShapeXml -Id $id -Name "StageTop" -X $x -Y 2280000 -W 1650000 -H 180000 -Fill $steps[$i][2] -Geom "rect")); $id++
        $s.Add((New-ShapeXml -Id $id -Name "StageCircle" -X ($x + 515000) -Y 2670000 -W 610000 -H 610000 -Fill $steps[$i][2] -Geom "ellipse")); $id++
        $s.Add((New-TextBoxXml -Id $id -Name "StageNo" -X ($x + 515000) -Y 2840000 -W 610000 -H 180000 -ParagraphXml (New-ParagraphXml -Text ("0" + ($i + 1)) -Size 18 -Color "FFFFFF" -Bold $true -Align "ctr"))); $id++
        $s.Add((New-TextBoxXml -Id $id -Name "StageTitle" -X ($x + 130000) -Y 3450000 -W 1390000 -H 270000 -ParagraphXml (New-ParagraphXml -Text $steps[$i][0] -Size 17 -Color "172033" -Bold $true -Align "ctr"))); $id++
        $s.Add((New-TextBoxXml -Id $id -Name "StageBody" -X ($x + 150000) -Y 3850000 -W 1350000 -H 410000 -ParagraphXml (New-ParagraphXml -Text $steps[$i][1] -Size 10 -Color "657085" -Align "ctr"))); $id++
        if ($i -lt $steps.Count - 1) {
            $s.Add((New-ShapeXml -Id $id -Name "Arrow" -X ($x + 1660000) -Y 3240000 -W 260000 -H 150000 -Fill "CBD5E1" -Geom "rightArrow")); $id++
        }
        $x += 1770000
    }
    return New-SlideShellXml -ShapeXml ($s -join "`n")
}

function New-ActualResultSlide {
    param([int]$Page, [int]$Total)
    $id = 2; $s = New-Object System.Collections.Generic.List[string]
    $s.Add((New-ShapeXml -Id $id -Name "Background" -X 0 -Y 0 -W $SW -H $SH -Fill "F6F7F9")); $id++
    $s.Add((New-HeaderXml -Id $id -Page $Page -Total $Total -Title "本轮样本的测评结果填充" -Section "03 / 验证结果")); $id += 8
    $s.Add((New-ShapeXml -Id $id -Name "ResultPanel" -X 720000 -Y 1160000 -W 4880000 -H 3850000 -Fill "FFFFFF" -Stroke "BFE7DE" -Geom "roundRect")); $id++
    $s.Add((New-ShapeXml -Id $id -Name "RiskPanel" -X 6510000 -Y 1160000 -W 4920000 -H 3850000 -Fill "FFFFFF" -Stroke "FDE68A" -Geom "roundRect")); $id++
    $s.Add((New-LabelXml -Id $id -X 1040000 -Y 1480000 -W 1600000 -H 390000 -Text "已验证" -Fill "0E9384")); $id += 2
    $s.Add((New-LabelXml -Id $id -X 6840000 -Y 1480000 -W 1600000 -H 390000 -Text "仍需补充" -Fill "F59E0B")); $id += 2
    $done = @(
        "cmake --build . --target ntn_test 成功",
        "CTest: ntn_test 1/1 Passed，Test time = 0.02 sec",
        "gtest: 4 tests from 4 suites，13 ms total，4 passed",
        "覆盖 grid 着色、hopping table、TA计算、controller边界调度"
    )
    $todo = @(
        "没有完整人工对照组，效率提升暂不能写成最终统计结论",
        "尚未跑 gNB/O-DU 真实运行链路和空口场景",
        "需要补充远程JSON解析的负向/异常输入测试",
        "需要协议专家复核SIB19字段语义和调度提前量"
    )
    $s.Add((New-BulletBoxXml -Id $id -X 1090000 -Y 2210000 -W 3800000 -H 1950000 -Bullets $done -Size 14 -Color "263142")); $id++
    $s.Add((New-BulletBoxXml -Id $id -X 6880000 -Y 2210000 -W 3800000 -H 1950000 -Bullets $todo -Size 14 -Color "263142")); $id++
    $s.Add((New-ShapeXml -Id $id -Name "Bottom" -X 1060000 -Y 5350000 -W 10060000 -H 520000 -Fill "172033" -Geom "roundRect")); $id++
    $s.Add((New-TextBoxXml -Id $id -Name "BottomText" -X 1290000 -Y 5500000 -W 9560000 -H 210000 -ParagraphXml (New-ParagraphXml -Text "推荐把本轮结果作为「AI辅助开发可行性样本」，下一轮补人工基线、系统集成测试和异常输入测试，升级为正式统计测评。" -Size 13 -Color "FFFFFF" -Bold $true -Align "ctr"))); $id++
    return New-SlideShellXml -ShapeXml ($s -join "`n")
}

function New-ExecutionEvidenceSlide {
    param([int]$Page, [int]$Total)
    $id = 2; $s = New-Object System.Collections.Generic.List[string]
    $s.Add((New-ShapeXml -Id $id -Name "Background" -X 0 -Y 0 -W $SW -H $SH -Fill "F8FAFC")); $id++
    $s.Add((New-HeaderXml -Id $id -Page $Page -Total $Total -Title "实际开展：执行命令与证据" -Section "04 / 执行证据")); $id += 8

    $s.Add((New-ShapeXml -Id $id -Name "CommandPanel" -X 700000 -Y 1180000 -W 5200000 -H 3750000 -Fill "101623" -Geom "roundRect")); $id++
    $s.Add((New-LabelXml -Id $id -X 1020000 -Y 1480000 -W 1450000 -H 360000 -Text "执行命令" -Fill "0E9384")); $id += 2
    $cmd1 = "cd /mnt/d/code/srsRAN_Project-main/build"
    $cmd2 = "cmake --build . --target ntn_test -j2"
    $cmd3 = "ctest -R ntn_test --output-on-failure"
    $cmd4 = "./build/tests/unittests/ntn/ntn_test --gtest_list_tests"
    $cmds = @($cmd1, $cmd2, $cmd3, $cmd4)
    $y = 2140000
    foreach ($cmd in $cmds) {
        $s.Add((New-ShapeXml -Id $id -Name "CmdLine" -X 1030000 -Y $y -W 4550000 -H 420000 -Fill "172033" -Stroke "334155" -Geom "roundRect")); $id++
        $s.Add((New-TextBoxXml -Id $id -Name "CmdText" -X 1220000 -Y ($y + 115000) -W 4180000 -H 160000 -ParagraphXml (New-ParagraphXml -Text $cmd -Size 10 -Color "D8E1EE"))); $id++
        $y += 560000
    }

    $s.Add((New-ShapeXml -Id $id -Name "ResultPanel" -X 6400000 -Y 1180000 -W 5000000 -H 3750000 -Fill "FFFFFF" -Stroke "E2E8F0" -Geom "roundRect")); $id++
    $s.Add((New-LabelXml -Id $id -X 6740000 -Y 1480000 -W 1450000 -H 360000 -Text "验证结果" -Fill "4F46E5")); $id += 2
    $results = @(
        @("构建", "srsran_ntn 与 ntn_test target 均构建通过", "0E9384"),
        @("CTest", "ntn_test: 1/1 Passed，Test time = 0.02 sec", "4F46E5"),
        @("gtest", "4 tests / 4 suites / 13 ms total / 4 passed", "F59E0B"),
        @("环境", "WSL 中有 cmake/ctest；未发现 clang-tidy/cppcheck", "E11D48")
    )
    $y = 2120000
    foreach ($r in $results) {
        $s.Add((New-ShapeXml -Id $id -Name "ResultDot" -X 6780000 -Y ($y + 75000) -W 220000 -H 220000 -Fill $r[2] -Geom "ellipse")); $id++
        $s.Add((New-TextBoxXml -Id $id -Name "ResultTitle" -X 7160000 -Y $y -W 900000 -H 210000 -ParagraphXml (New-ParagraphXml -Text $r[0] -Size 14 -Color "172033" -Bold $true))); $id++
        $s.Add((New-TextBoxXml -Id $id -Name "ResultBody" -X 8120000 -Y $y -W 2900000 -H 250000 -ParagraphXml (New-ParagraphXml -Text $r[1] -Size 11 -Color "657085"))); $id++
        $y += 620000
    }

    $s.Add((New-ShapeXml -Id $id -Name "BottomBand" -X 720000 -Y 5350000 -W 10660000 -H 520000 -Fill "E8F6F3" -Stroke "A7D8D1" -Geom "roundRect")); $id++
    $s.Add((New-TextBoxXml -Id $id -Name "BottomText" -X 980000 -Y 5500000 -W 10150000 -H 190000 -ParagraphXml (New-ParagraphXml -Text "本轮测评证据已落地到 docs/ai_comm_eval_execution_2026-05-07.md，可复跑、可追溯、可作为后续对照实验基线。" -Size 12 -Color "0F3D46" -Bold $true -Align "ctr"))); $id++
    return New-SlideShellXml -ShapeXml ($s -join "`n")
}

function New-ScoreResultSlide {
    param([int]$Page, [int]$Total)
    $id = 2; $s = New-Object System.Collections.Generic.List[string]
    $s.Add((New-ShapeXml -Id $id -Name "Background" -X 0 -Y 0 -W $SW -H $SH -Fill "F6F7F9")); $id++
    $s.Add((New-HeaderXml -Id $id -Page $Page -Total $Total -Title "测评评分：81/100" -Section "05 / 评分结果")); $id += 8

    $s.Add((New-ShapeXml -Id $id -Name "ScoreCircle" -X 770000 -Y 1530000 -W 2600000 -H 2600000 -Fill "172033" -Geom "ellipse")); $id++
    $s.Add((New-ShapeXml -Id $id -Name "ScoreRing" -X 990000 -Y 1750000 -W 2160000 -H 2160000 -Fill "" -Stroke "0E9384" -Geom "ellipse" -StrokeWidth 63500)); $id++
    $s.Add((New-TextBoxXml -Id $id -Name "ScoreNumber" -X 780000 -Y 2300000 -W 2600000 -H 520000 -ParagraphXml (New-ParagraphXml -Text "81" -Size 48 -Color "FFFFFF" -Bold $true -Align "ctr"))); $id++
    $s.Add((New-TextBoxXml -Id $id -Name "ScoreTotal" -X 780000 -Y 2950000 -W 2600000 -H 260000 -ParagraphXml (New-ParagraphXml -Text "/ 100" -Size 18 -Color "B7C4D8" -Bold $true -Align "ctr"))); $id++
    $s.Add((New-TextBoxXml -Id $id -Name "ScoreLabel" -X 690000 -Y 4400000 -W 2800000 -H 260000 -ParagraphXml (New-ParagraphXml -Text "AI辅助开发可行性样本" -Size 15 -Color "172033" -Bold $true -Align "ctr"))); $id++

    $metrics = @(
        @("代码理解", 18, 20, "能快速定位 NTN、O-DU、远程命令、CMake 和单测入口", "0E9384"),
        @("实现辅助", 15, 20, "适合生成表结构、glue code、参数校验和controller骨架", "4F46E5"),
        @("测试补齐", 14, 20, "已覆盖核心算法路径，但parser、stop和集成路径不足", "F59E0B"),
        @("调试验证", 14, 15, "能找到WSL构建环境并复跑target/CTest", "14B8A6"),
        @("风险识别", 12, 15, "识别timer生命周期、parser单测缺口和late tick风险", "E11D48"),
        @("落地可控", 8, 10, "已有CI+单测+风险清单闭环，仍需系统验证", "7C3AED")
    )
    $y = 1180000
    foreach ($m in $metrics) {
        $s.Add((New-ShapeXml -Id $id -Name "MetricRow" -X 4400000 -Y $y -W 6950000 -H 610000 -Fill "FFFFFF" -Stroke "E2E8F0" -Geom "roundRect")); $id++
        $s.Add((New-TextBoxXml -Id $id -Name "MetricName" -X 4650000 -Y ($y + 120000) -W 1100000 -H 190000 -ParagraphXml (New-ParagraphXml -Text $m[0] -Size 13 -Color "172033" -Bold $true))); $id++
        $s.Add((New-TextBoxXml -Id $id -Name "MetricBody" -X 4650000 -Y ($y + 345000) -W 4300000 -H 150000 -ParagraphXml (New-ParagraphXml -Text $m[3] -Size 9 -Color "657085"))); $id++
        $s.Add((New-ShapeXml -Id $id -Name "BarBg" -X 9200000 -Y ($y + 230000) -W 1450000 -H 110000 -Fill "E2E8F0" -Geom "roundRect")); $id++
        $barW = [int64](1450000 * ([double]$m[1] / [double]$m[2]))
        $s.Add((New-ShapeXml -Id $id -Name "Bar" -X 9200000 -Y ($y + 230000) -W $barW -H 110000 -Fill $m[4] -Geom "roundRect")); $id++
        $s.Add((New-TextBoxXml -Id $id -Name "MetricScore" -X 10600000 -Y ($y + 160000) -W 500000 -H 180000 -ParagraphXml (New-ParagraphXml -Text ("{0}/{1}" -f $m[1], $m[2]) -Size 11 -Color $m[4] -Bold $true -Align "r"))); $id++
        $y += 720000
    }
    return New-SlideShellXml -ShapeXml ($s -join "`n")
}

function New-CodeReviewFindingsSlide {
    param([int]$Page, [int]$Total)
    $id = 2; $s = New-Object System.Collections.Generic.List[string]
    $s.Add((New-ShapeXml -Id $id -Name "Background" -X 0 -Y 0 -W $SW -H $SH -Fill "F8FAFC")); $id++
    $s.Add((New-HeaderXml -Id $id -Page $Page -Total $Total -Title "代码审查发现：AI没有自动兜住的风险" -Section "06 / 风险发现")); $id += 8

    $findings = @(
        @("发现 1", "stop 后 1ms 周期 tick 仍可能持续自调度", "schedule_periodic_tick() 在timer回调中无条件再次调度；stop最后一个session后 hopping_tick_started 未恢复。", "影响：当前单测不暴露，但长期运行可能产生不必要的1ms唤醒。建议：停止最后一个session后取消timer或允许重新进入idle状态。", "E11D48"),
        @("发现 2", "ntn_config_update parser 缺少专项负向单测", "解析代码已有 n_active、dwell_frames、经纬度、ephemeris 类型等校验，但没有独立测试覆盖异常输入。", "建议：补JSON解析单测，覆盖 n_active=0、非3倍数、周期超过1024、极区、orbital+beam_hopping等失败路径。", "F59E0B"),
        @("发现 3", "controller late tick 行为需要设计断言", "当前测试覆盖边界后调度下一dwell；但若周期tick丢失并越过边界，策略需要更明确。", "建议：补 missed boundary 用例，并明确是补发当前dwell、调度下一dwell，还是上报warning。", "4F46E5")
    )
    $y = 1180000
    foreach ($f in $findings) {
        $s.Add((New-ShapeXml -Id $id -Name "FindingCard" -X 700000 -Y $y -W 10850000 -H 1260000 -Fill "FFFFFF" -Stroke "E2E8F0" -Geom "roundRect")); $id++
        $s.Add((New-LabelXml -Id $id -X 1020000 -Y ($y + 210000) -W 860000 -H 320000 -Text $f[0] -Fill $f[4] -Size 10)); $id += 2
        $s.Add((New-TextBoxXml -Id $id -Name "FindingTitle" -X 2100000 -Y ($y + 170000) -W 4550000 -H 250000 -ParagraphXml (New-ParagraphXml -Text $f[1] -Size 15 -Color "172033" -Bold $true))); $id++
        $s.Add((New-TextBoxXml -Id $id -Name "FindingEvidence" -X 2100000 -Y ($y + 520000) -W 4200000 -H 390000 -ParagraphXml (New-ParagraphXml -Text $f[2] -Size 10 -Color "657085"))); $id++
        $s.Add((New-ShapeXml -Id $id -Name "FindingDivider" -X 6800000 -Y ($y + 230000) -W 26000 -H 800000 -Fill "E2E8F0")); $id++
        $s.Add((New-TextBoxXml -Id $id -Name "FindingAction" -X 7100000 -Y ($y + 300000) -W 3750000 -H 470000 -ParagraphXml (New-ParagraphXml -Text $f[3] -Size 10 -Color "263142"))); $id++
        $y += 1480000
    }

    $s.Add((New-ShapeXml -Id $id -Name "Bottom" -X 700000 -Y 5700000 -W 10850000 -H 390000 -Fill "172033" -Geom "roundRect")); $id++
    $s.Add((New-TextBoxXml -Id $id -Name "BottomText" -X 980000 -Y 5810000 -W 10250000 -H 160000 -ParagraphXml (New-ParagraphXml -Text "审查结论：AI能补齐显性工程路径，但通信软件的长期运行、实时性和协议语义仍需要人工强审查。" -Size 12 -Color "FFFFFF" -Bold $true -Align "ctr"))); $id++
    return New-SlideShellXml -ShapeXml ($s -join "`n")
}

function New-NextTaskSlide {
    param([int]$Page, [int]$Total)
    $id = 2; $s = New-Object System.Collections.Generic.List[string]
    $s.Add((New-ShapeXml -Id $id -Name "Background" -X 0 -Y 0 -W $SW -H $SH -Fill "F6F7F9")); $id++
    $s.Add((New-HeaderXml -Id $id -Page $Page -Total $Total -Title "下一步任务卡：从可行性样本到正式测评" -Section "07 / 后续计划")); $id += 8

    $lanes = @(
        @("P0", "立即补齐", @("修复或明确 stop 后周期 tick 生命周期策略", "为 ntn_config_update parser 增加负向单测"), "E11D48"),
        @("P1", "进入迭代", @("增加 late tick / missed boundary controller 单测", "增加 beam hopping start/stop 的 O-DU mock集成测试"), "F59E0B"),
        @("P2", "系统增强", @("补充 SIB19 编码字段一致性检查", "增加多cell、多session和星历连续更新场景"), "4F46E5")
    )
    $x = 760000
    foreach ($lane in $lanes) {
        $s.Add((New-ShapeXml -Id $id -Name "Lane" -X $x -Y 1320000 -W 3300000 -H 3650000 -Fill "FFFFFF" -Stroke "E2E8F0" -Geom "roundRect")); $id++
        $s.Add((New-ShapeXml -Id $id -Name "LaneTop" -X $x -Y 1320000 -W 3300000 -H 620000 -Fill $lane[3] -Geom "rect")); $id++
        $s.Add((New-TextBoxXml -Id $id -Name "LanePriority" -X ($x + 230000) -Y 1480000 -W 650000 -H 200000 -ParagraphXml (New-ParagraphXml -Text $lane[0] -Size 18 -Color "FFFFFF" -Bold $true))); $id++
        $s.Add((New-TextBoxXml -Id $id -Name "LaneTitle" -X ($x + 1020000) -Y 1500000 -W 1800000 -H 180000 -ParagraphXml (New-ParagraphXml -Text $lane[1] -Size 15 -Color "FFFFFF" -Bold $true -Align "r"))); $id++
        $taskY = 2300000
        foreach ($task in $lane[2]) {
            $s.Add((New-ShapeXml -Id $id -Name "TaskBullet" -X ($x + 300000) -Y ($taskY + 65000) -W 170000 -H 170000 -Fill $lane[3] -Geom "ellipse")); $id++
            $s.Add((New-TextBoxXml -Id $id -Name "TaskText" -X ($x + 600000) -Y $taskY -W 2300000 -H 420000 -ParagraphXml (New-ParagraphXml -Text $task -Size 12 -Color "263142"))); $id++
            $taskY += 900000
        }
        $x += 3620000
    }

    $s.Add((New-ShapeXml -Id $id -Name "Conclusion" -X 760000 -Y 5400000 -W 10550000 -H 570000 -Fill "172033" -Geom "roundRect")); $id++
    $s.Add((New-TextBoxXml -Id $id -Name "ConclusionText" -X 1050000 -Y 5540000 -W 9950000 -H 240000 -ParagraphXml (New-ParagraphXml -Text "正式测评升级路径：补人工对照组 + 扩展异常输入测试 + 跑O-DU集成链路 + 协议专家复核SIB19语义。" -Size 14 -Color "FFFFFF" -Bold $true -Align "ctr"))); $id++
    return New-SlideShellXml -ShapeXml ($s -join "`n")
}

function New-TaskSlide {
    param([int]$Page, [int]$Total)
    $id = 2; $s = New-Object System.Collections.Generic.List[string]
    $s.Add((New-ShapeXml -Id $id -Name "Background" -X 0 -Y 0 -W $SW -H $SH -Fill "F8FAFC")); $id++
    $s.Add((New-HeaderXml -Id $id -Page $Page -Total $Total -Title "测评任务集设计" -Section "04 / 任务集")); $id += 8
    $tasks = @(
        @("代码理解", "解释模块职责、关键调用链和数据流", "0E9384"),
        @("功能实现", "新增配置项、协议字段处理或错误分支", "4F46E5"),
        @("缺陷修复", "根据失败测试、日志或崩溃栈定位问题", "E11D48"),
        @("测试补齐", "补单元测试、边界测试和mock/fake", "F59E0B"),
        @("性能分析", "针对热点路径提出优化并验证指标", "14B8A6"),
        @("文档评审", "生成变更说明、接口说明和风险清单", "7C3AED")
    )
    $x0 = 620000; $y0 = 1250000; $w = 3450000; $h = 1350000; $gapX = 360000; $gapY = 310000
    for ($i=0; $i -lt $tasks.Count; $i++) {
        $row = [math]::Floor($i / 3); $col = $i % 3
        $x = $x0 + $col * ($w + $gapX); $y = $y0 + $row * ($h + $gapY)
        $s.Add((New-ShapeXml -Id $id -Name "TaskCard" -X $x -Y $y -W $w -H $h -Fill "FFFFFF" -Stroke "E2E8F0" -Geom "roundRect")); $id++
        $s.Add((New-ShapeXml -Id $id -Name "TaskIcon" -X ($x + 220000) -Y ($y + 220000) -W 460000 -H 460000 -Fill $tasks[$i][2] -Geom "ellipse")); $id++
        $s.Add((New-TextBoxXml -Id $id -Name "TaskNo" -X ($x + 220000) -Y ($y + 350000) -W 460000 -H 140000 -ParagraphXml (New-ParagraphXml -Text ("T" + ($i + 1)) -Size 12 -Color "FFFFFF" -Bold $true -Align "ctr"))); $id++
        $s.Add((New-TextBoxXml -Id $id -Name "TaskTitle" -X ($x + 840000) -Y ($y + 240000) -W 2250000 -H 250000 -ParagraphXml (New-ParagraphXml -Text $tasks[$i][0] -Size 16 -Color "172033" -Bold $true))); $id++
        $s.Add((New-TextBoxXml -Id $id -Name "TaskBody" -X ($x + 840000) -Y ($y + 640000) -W 2250000 -H 380000 -ParagraphXml (New-ParagraphXml -Text $tasks[$i][1] -Size 11 -Color "657085"))); $id++
    }
    $s.Add((New-ShapeXml -Id $id -Name "BottomBand" -X 620000 -Y 4850000 -W 11160000 -H 650000 -Fill "172033" -Geom "roundRect")); $id++
    $s.Add((New-TextBoxXml -Id $id -Name "BottomText" -X 900000 -Y 5040000 -W 10400000 -H 230000 -ParagraphXml (New-ParagraphXml -Text "每个任务都必须产出：代码差异、测试结果、耗时记录、提示记录、风险说明。" -Size 16 -Color "FFFFFF" -Bold $true))); $id++
    return New-SlideShellXml -ShapeXml ($s -join "`n")
}

function New-ExperimentSlide {
    param([int]$Page, [int]$Total)
    $id = 2; $s = New-Object System.Collections.Generic.List[string]
    $s.Add((New-ShapeXml -Id $id -Name "Background" -X 0 -Y 0 -W $SW -H $SH -Fill "F6F7F9")); $id++
    $s.Add((New-HeaderXml -Id $id -Page $Page -Total $Total -Title "怎么做对照实验" -Section "05 / 实验分组")); $id += 8
    $cols = @(
        @("A组", "人工开发", "不使用AI，作为基线", "64748B"),
        @("B组", "通用问答式AI", "人工粘贴上下文，观察提示成本", "0E9384"),
        @("C组", "代码库增强AI/Agent", "具备工程上下文，观察端到端收益", "4F46E5")
    )
    $x = 830000
    foreach ($col in $cols) {
        $s.Add((New-ShapeXml -Id $id -Name "GroupCard" -X $x -Y 1350000 -W 3150000 -H 2500000 -Fill "FFFFFF" -Stroke "E2E8F0" -Geom "roundRect")); $id++
        $s.Add((New-ShapeXml -Id $id -Name "GroupHeader" -X $x -Y 1350000 -W 3150000 -H 620000 -Fill $col[3] -Geom "rect")); $id++
        $s.Add((New-TextBoxXml -Id $id -Name "GroupName" -X ($x + 180000) -Y 1500000 -W 650000 -H 210000 -ParagraphXml (New-ParagraphXml -Text $col[0] -Size 18 -Color "FFFFFF" -Bold $true))); $id++
        $s.Add((New-TextBoxXml -Id $id -Name "GroupTitle" -X ($x + 180000) -Y 2140000 -W 2700000 -H 300000 -ParagraphXml (New-ParagraphXml -Text $col[1] -Size 18 -Color "172033" -Bold $true))); $id++
        $s.Add((New-TextBoxXml -Id $id -Name "GroupBody" -X ($x + 180000) -Y 2700000 -W 2700000 -H 530000 -ParagraphXml (New-ParagraphXml -Text $col[2] -Size 13 -Color "657085"))); $id++
        $x += 3500000
    }
    $controls = @("相同任务", "相同时间盒", "相同测试环境", "相同验收标准", "参与者分层")
    $x = 780000
    foreach ($control in $controls) {
        $s.Add((New-LabelXml -Id $id -X $x -Y 4550000 -W 1900000 -H 400000 -Text $control -Fill "E8F6F3" -Color "0F3D46" -Size 12)); $id += 2
        $x += 2140000
    }
    $s.Add((New-TextBoxXml -Id $id -Name "Note" -X 820000 -Y 5200000 -W 10300000 -H 280000 -ParagraphXml (New-ParagraphXml -Text "参与者按通信背景、C/C++经验和项目熟悉度分层，避免把AI效果误判为个人经验差异。" -Size 14 -Color "475569" -Align "ctr"))); $id++
    return New-SlideShellXml -ShapeXml ($s -join "`n")
}

function New-ScorecardSlide {
    param([int]$Page, [int]$Total)
    $id = 2; $s = New-Object System.Collections.Generic.List[string]
    $s.Add((New-ShapeXml -Id $id -Name "Background" -X 0 -Y 0 -W $SW -H $SH -Fill "F8FAFC")); $id++
    $s.Add((New-HeaderXml -Id $id -Page $Page -Total $Total -Title "评价指标：既看快，也看稳" -Section "06 / 指标体系")); $id += 8
    $metrics = @(
        @("效率", "完成时间、上下文查找时间、调试轮次", 86, "0E9384"),
        @("正确性", "测试通过率、协议行为一致性、边界覆盖", 94, "4F46E5"),
        @("代码质量", "可维护性、接口一致性、风格和复杂度", 78, "F59E0B"),
        @("稳定性", "内存、线程、实时性、吞吐、时延", 88, "E11D48"),
        @("协作成本", "提示次数、人工修改比例、评审问题数", 72, "14B8A6")
    )
    $y = 1300000
    foreach ($m in $metrics) {
        $s.Add((New-ShapeXml -Id $id -Name "MetricRow" -X 850000 -Y $y -W 10300000 -H 720000 -Fill "FFFFFF" -Stroke "E2E8F0" -Geom "roundRect")); $id++
        $s.Add((New-ShapeXml -Id $id -Name "MetricDot" -X 1120000 -Y ($y + 230000) -W 260000 -H 260000 -Fill $m[4] -Geom "ellipse")); $id++
        $s.Add((New-TextBoxXml -Id $id -Name "MetricName" -X 1540000 -Y ($y + 170000) -W 1450000 -H 230000 -ParagraphXml (New-ParagraphXml -Text $m[0] -Size 16 -Color "172033" -Bold $true))); $id++
        $s.Add((New-TextBoxXml -Id $id -Name "MetricBody" -X 1540000 -Y ($y + 430000) -W 4300000 -H 190000 -ParagraphXml (New-ParagraphXml -Text $m[1] -Size 10 -Color "657085"))); $id++
        $s.Add((New-ShapeXml -Id $id -Name "BarBg" -X 6500000 -Y ($y + 310000) -W 3500000 -H 130000 -Fill "E2E8F0" -Geom "roundRect")); $id++
        $barW = [int64](3500000 * ([double]$m[2] / 100.0))
        $s.Add((New-ShapeXml -Id $id -Name "Bar" -X 6500000 -Y ($y + 310000) -W $barW -H 130000 -Fill $m[4] -Geom "roundRect")); $id++
        $s.Add((New-TextBoxXml -Id $id -Name "MetricScore" -X 10200000 -Y ($y + 230000) -W 600000 -H 210000 -ParagraphXml (New-ParagraphXml -Text ($m[2].ToString() + "%") -Size 14 -Color $m[4] -Bold $true -Align "r"))); $id++
        $y += 890000
    }
    return New-SlideShellXml -ShapeXml ($s -join "`n")
}

function New-UsecaseSlide {
    param([int]$Page, [int]$Total)
    $id = 2; $s = New-Object System.Collections.Generic.List[string]
    $s.Add((New-ShapeXml -Id $id -Name "Background" -X 0 -Y 0 -W $SW -H $SH -Fill "F6F7F9")); $id++
    $s.Add((New-HeaderXml -Id $id -Page $Page -Total $Total -Title "可直接落地的典型测评用例" -Section "07 / 典型用例")); $id += 8
    $left = @(
        "新增gNB/UE配置项，并贯穿解析、校验、日志和文档",
        "修复一个RRC/MAC消息处理边界条件问题",
        "根据失败测试补齐协议字段编码/解码逻辑"
    )
    $right = @(
        "根据运行日志定位一次接入失败或调度异常",
        "为关键模块补充mock和单元测试",
        "对高频路径进行小范围性能优化并跑基准测试"
    )
    $s.Add((New-ShapeXml -Id $id -Name "PanelLeft" -X 700000 -Y 1250000 -W 5200000 -H 3950000 -Fill "FFFFFF" -Stroke "E2E8F0" -Geom "roundRect")); $id++
    $s.Add((New-ShapeXml -Id $id -Name "PanelRight" -X 6490000 -Y 1250000 -W 5000000 -H 3950000 -Fill "FFFFFF" -Stroke "E2E8F0" -Geom "roundRect")); $id++
    $s.Add((New-LabelXml -Id $id -X 1010000 -Y 1510000 -W 1600000 -H 390000 -Text "功能与协议" -Fill "0E9384")); $id += 2
    $s.Add((New-LabelXml -Id $id -X 6790000 -Y 1510000 -W 1600000 -H 390000 -Text "调试与质量" -Fill "4F46E5")); $id += 2
    $y = 2250000
    for ($i=0; $i -lt $left.Count; $i++) {
        $s.Add((New-ShapeXml -Id $id -Name "No" -X 1020000 -Y ($y - 50000) -W 420000 -H 420000 -Fill "E8F6F3" -Stroke "A7D8D1" -Geom "ellipse")); $id++
        $s.Add((New-TextBoxXml -Id $id -Name "NoText" -X 1020000 -Y ($y + 65000) -W 420000 -H 140000 -ParagraphXml (New-ParagraphXml -Text ($i + 1).ToString() -Size 13 -Color "0E9384" -Bold $true -Align "ctr"))); $id++
        $s.Add((New-TextBoxXml -Id $id -Name "Usecase" -X 1580000 -Y $y -W 3650000 -H 300000 -ParagraphXml (New-ParagraphXml -Text $left[$i] -Size 14 -Color "263142"))); $id++
        $s.Add((New-ShapeXml -Id $id -Name "No" -X 6810000 -Y ($y - 50000) -W 420000 -H 420000 -Fill "EEF2FF" -Stroke "C7D2FE" -Geom "ellipse")); $id++
        $s.Add((New-TextBoxXml -Id $id -Name "NoText" -X 6810000 -Y ($y + 65000) -W 420000 -H 140000 -ParagraphXml (New-ParagraphXml -Text ($i + 4).ToString() -Size 13 -Color "4F46E5" -Bold $true -Align "ctr"))); $id++
        $s.Add((New-TextBoxXml -Id $id -Name "Usecase" -X 7370000 -Y $y -W 3650000 -H 300000 -ParagraphXml (New-ParagraphXml -Text $right[$i] -Size 14 -Color "263142"))); $id++
        $y += 890000
    }
    return New-SlideShellXml -ShapeXml ($s -join "`n")
}

function New-RiskSlide {
    param([int]$Page, [int]$Total)
    $id = 2; $s = New-Object System.Collections.Generic.List[string]
    $s.Add((New-ShapeXml -Id $id -Name "Background" -X 0 -Y 0 -W $SW -H $SH -Fill "F8FAFC")); $id++
    $s.Add((New-HeaderXml -Id $id -Page $Page -Total $Total -Title "AI辅助通信编程的主要风险" -Section "08 / 风险边界")); $id += 8
    $risks = @(
        @("幻觉API", "编造不存在的接口、字段或配置语义", "高", "E11D48"),
        @("并发遗漏", "忽略跨线程生命周期、锁竞争或资源释放", "高", "E11D48"),
        @("协议误读", "状态机和消息字段语义理解不完整", "高", "F59E0B"),
        @("测试表面化", "只覆盖明显路径，难证明空口场景正确", "中", "F59E0B"),
        @("性能回归", "实时路径增加额外分配或同步开销", "中", "4F46E5"),
        @("数据合规", "内部代码、日志、配置输入前需要权限与脱敏", "高", "0E9384")
    )
    $x0 = 640000; $y0 = 1260000; $w = 3450000; $h = 1260000; $gapX = 360000; $gapY = 300000
    for ($i=0; $i -lt $risks.Count; $i++) {
        $row = [math]::Floor($i / 3); $col = $i % 3
        $x = $x0 + $col * ($w + $gapX); $y = $y0 + $row * ($h + $gapY)
        $s.Add((New-ShapeXml -Id $id -Name "RiskCard" -X $x -Y $y -W $w -H $h -Fill "FFFFFF" -Stroke "E2E8F0" -Geom "roundRect")); $id++
        $s.Add((New-LabelXml -Id $id -X ($x + 2400000) -Y ($y + 200000) -W 640000 -H 300000 -Text $risks[$i][2] -Fill $risks[$i][3] -Size 10)); $id += 2
        $s.Add((New-TextBoxXml -Id $id -Name "RiskTitle" -X ($x + 230000) -Y ($y + 210000) -W 1900000 -H 250000 -ParagraphXml (New-ParagraphXml -Text $risks[$i][0] -Size 16 -Color "172033" -Bold $true))); $id++
        $s.Add((New-TextBoxXml -Id $id -Name "RiskBody" -X ($x + 230000) -Y ($y + 610000) -W 2850000 -H 360000 -ParagraphXml (New-ParagraphXml -Text $risks[$i][1] -Size 11 -Color "657085"))); $id++
    }
    $s.Add((New-ShapeXml -Id $id -Name "Guardrail" -X 640000 -Y 4670000 -W 10970000 -H 640000 -Fill "172033" -Geom "roundRect")); $id++
    $s.Add((New-TextBoxXml -Id $id -Name "GuardrailText" -X 930000 -Y 4850000 -W 10300000 -H 240000 -ParagraphXml (New-ParagraphXml -Text "防护原则：AI输出必须进入CI、静态检查、单元测试、协议专家评审和必要的性能回归。" -Size 15 -Color "FFFFFF" -Bold $true))); $id++
    return New-SlideShellXml -ShapeXml ($s -join "`n")
}

function New-ProcessSlide {
    param([int]$Page, [int]$Total)
    $id = 2; $s = New-Object System.Collections.Generic.List[string]
    $s.Add((New-ShapeXml -Id $id -Name "Background" -X 0 -Y 0 -W $SW -H $SH -Fill "F6F7F9")); $id++
    $s.Add((New-HeaderXml -Id $id -Page $Page -Total $Total -Title "从测评到工程落地" -Section "09 / 落地流程")); $id += 8
    $steps = @(
        @("准备", "任务卡 / 基线代码 / 测试脚本 / 评分表", "0E9384"),
        @("规范", "上下文输入 / 提示模板 / 禁止事项 / 审查要求", "4F46E5"),
        @("执行", "耗时 / 提示 / 代码变更 / 测试结果 / 人工干预", "F59E0B"),
        @("分析", "按任务类型和开发者画像比较增益与风险", "E11D48"),
        @("沉淀", "推荐场景 / 慎用场景 / 工具链接入 / 后续计划", "14B8A6")
    )
    $x = 610000
    for ($i=0; $i -lt $steps.Count; $i++) {
        $s.Add((New-ShapeXml -Id $id -Name "StepCircle" -X ($x + 420000) -Y 1510000 -W 900000 -H 900000 -Fill $steps[$i][2] -Geom "ellipse")); $id++
        $s.Add((New-TextBoxXml -Id $id -Name "StepNo" -X ($x + 420000) -Y 1790000 -W 900000 -H 200000 -ParagraphXml (New-ParagraphXml -Text ("0" + ($i + 1)) -Size 20 -Color "FFFFFF" -Bold $true -Align "ctr"))); $id++
        $s.Add((New-ShapeXml -Id $id -Name "StepCard" -X $x -Y 2760000 -W 1750000 -H 1600000 -Fill "FFFFFF" -Stroke "E2E8F0" -Geom "roundRect")); $id++
        $s.Add((New-TextBoxXml -Id $id -Name "StepTitle" -X ($x + 150000) -Y 3010000 -W 1450000 -H 250000 -ParagraphXml (New-ParagraphXml -Text $steps[$i][0] -Size 17 -Color "172033" -Bold $true -Align "ctr"))); $id++
        $s.Add((New-TextBoxXml -Id $id -Name "StepBody" -X ($x + 160000) -Y 3420000 -W 1430000 -H 560000 -ParagraphXml (New-ParagraphXml -Text $steps[$i][1] -Size 10 -Color "657085" -Align "ctr"))); $id++
        if ($i -lt $steps.Count - 1) {
            $s.Add((New-ShapeXml -Id $id -Name "FlowArrow" -X ($x + 1780000) -Y 3380000 -W 430000 -H 180000 -Fill "CBD5E1" -Geom "rightArrow")); $id++
        }
        $x += 2200000
    }
    $s.Add((New-ShapeXml -Id $id -Name "Bottom" -X 800000 -Y 4980000 -W 10590000 -H 520000 -Fill "E8F6F3" -Geom "roundRect")); $id++
    $s.Add((New-TextBoxXml -Id $id -Name "BottomText" -X 1030000 -Y 5130000 -W 10100000 -H 210000 -ParagraphXml (New-ParagraphXml -Text "把AI放进现有工程纪律中：CI、静态检查、代码评审、协议专家确认和复盘闭环。" -Size 14 -Color "0F3D46" -Bold $true -Align "ctr"))); $id++
    return New-SlideShellXml -ShapeXml ($s -join "`n")
}

function New-ConclusionSlide {
    param([int]$Page, [int]$Total)
    $id = 2; $s = New-Object System.Collections.Generic.List[string]
    $s.Add((New-ShapeXml -Id $id -Name "Background" -X 0 -Y 0 -W $SW -H $SH -Fill "F8FAFC")); $id++
    $s.Add((New-HeaderXml -Id $id -Page $Page -Total $Total -Title "预期结论：副驾驶，而不是自动驾驶" -Section "10 / 预期结论")); $id += 8
    $s.Add((New-ShapeXml -Id $id -Name "GoodPanel" -X 720000 -Y 1260000 -W 5150000 -H 3600000 -Fill "FFFFFF" -Stroke "BFE7DE" -Geom "roundRect")); $id++
    $s.Add((New-ShapeXml -Id $id -Name "WarnPanel" -X 6350000 -Y 1260000 -W 5100000 -H 3600000 -Fill "FFFFFF" -Stroke "FDE68A" -Geom "roundRect")); $id++
    $s.Add((New-LabelXml -Id $id -X 1040000 -Y 1560000 -W 1600000 -H 400000 -Text "高收益场景" -Fill "0E9384")); $id += 2
    $s.Add((New-LabelXml -Id $id -X 6670000 -Y 1560000 -W 1600000 -H 400000 -Text "谨慎场景" -Fill "F59E0B")); $id += 2
    $good = @("代码理解与调用链梳理", "测试补齐和边界条件枚举", "日志初筛和失败归因", "文档生成与评审清单")
    $warn = @("协议状态机修改", "实时路径性能优化", "复杂并发和跨模块重构", "标准语义最终判定")
    $s.Add((New-BulletBoxXml -Id $id -X 1080000 -Y 2350000 -W 4000000 -H 1750000 -Bullets $good -Size 15 -Color "263142")); $id++
    $s.Add((New-BulletBoxXml -Id $id -X 6710000 -Y 2350000 -W 3900000 -H 1750000 -Bullets $warn -Size 15 -Color "263142")); $id++
    $s.Add((New-ShapeXml -Id $id -Name "Formula" -X 1410000 -Y 5360000 -W 9400000 -H 550000 -Fill "172033" -Geom "roundRect")); $id++
    $s.Add((New-TextBoxXml -Id $id -Name "FormulaText" -X 1530000 -Y 5520000 -W 9150000 -H 210000 -ParagraphXml (New-ParagraphXml -Text "推荐落地公式：AI建议 + 自动化测试 + 静态检查 + 专家评审" -Size 16 -Color "FFFFFF" -Bold $true -Align "ctr"))); $id++
    return New-SlideShellXml -ShapeXml ($s -join "`n")
}

function New-RoadmapSlide {
    param([int]$Page, [int]$Total)
    $id = 2; $s = New-Object System.Collections.Generic.List[string]
    $s.Add((New-ShapeXml -Id $id -Name "Background" -X 0 -Y 0 -W $SW -H $SH -Fill "101623")); $id++
    $s.Add((New-ShapeXml -Id $id -Name "TopLineA" -X 0 -Y 0 -W 5200000 -H 84000 -Fill "0E9384")); $id++
    $s.Add((New-ShapeXml -Id $id -Name "TopLineB" -X 5200000 -Y 0 -W 3200000 -H 84000 -Fill "F59E0B")); $id++
    $s.Add((New-ShapeXml -Id $id -Name "TopLineC" -X 8400000 -Y 0 -W 3792000 -H 84000 -Fill "4F46E5")); $id++
    $s.Add((New-TextBoxXml -Id $id -Name "Title" -X 660000 -Y 500000 -W 7200000 -H 620000 -ParagraphXml (New-ParagraphXml -Text "四周试点计划" -Size 30 -Color "FFFFFF" -Bold $true))); $id++
    $s.Add((New-TextBoxXml -Id $id -Name "Subtitle" -X 680000 -Y 1180000 -W 7200000 -H 320000 -ParagraphXml (New-ParagraphXml -Text "先用小规模、可量化、低风险任务跑通闭环，再逐步接入真实迭代。" -Size 15 -Color "B7C4D8"))); $id++
    $weeks = @(
        @("第1周", "确定任务集、评分表、基线环境和AI使用规范", "0E9384"),
        @("第2周", "开展小规模开发者测评，收集过程数据和代码结果", "4F46E5"),
        @("第3周", "复盘指标，沉淀提示模板、风险清单和典型案例", "F59E0B"),
        @("第4周", "扩展到真实迭代任务，评估长期生产力变化", "E11D48")
    )
    $s.Add((New-ShapeXml -Id $id -Name "Timeline" -X 1550000 -Y 3460000 -W 8900000 -H 42000 -Fill "526179")); $id++
    $x = 1300000
    foreach ($w in $weeks) {
        $s.Add((New-ShapeXml -Id $id -Name "WeekCircle" -X $x -Y 2960000 -W 980000 -H 980000 -Fill $w[2] -Geom "ellipse")); $id++
        $s.Add((New-TextBoxXml -Id $id -Name "WeekText" -X $x -Y 3270000 -W 980000 -H 180000 -ParagraphXml (New-ParagraphXml -Text $w[0] -Size 14 -Color "FFFFFF" -Bold $true -Align "ctr"))); $id++
        $s.Add((New-ShapeXml -Id $id -Name "WeekCard" -X ($x - 520000) -Y 4200000 -W 2020000 -H 1020000 -Fill "F8FAFC" -Alpha 96000 -Stroke "5A6B86" -Geom "roundRect")); $id++
        $s.Add((New-TextBoxXml -Id $id -Name "WeekBody" -X ($x - 340000) -Y 4470000 -W 1660000 -H 420000 -ParagraphXml (New-ParagraphXml -Text $w[1] -Size 10 -Color "172033" -Align "ctr"))); $id++
        $x += 2900000
    }
    $s.Add((New-TextBoxXml -Id $id -Name "Page" -X 10800000 -Y 6400000 -W 850000 -H 210000 -ParagraphXml (New-ParagraphXml -Text ("{0:00}/{1:00}" -f $Page, $Total) -Size 9 -Color "B7C4D8" -Align "r"))); $id++
    return New-SlideShellXml -ShapeXml ($s -join "`n")
}

$slideWriters = @(
    ${function:New-CoverSlide},
    ${function:New-ExecutiveSummarySlide},
    ${function:New-TargetSlide},
    ${function:New-SectionNtnSlide},
    ${function:New-ObservedSampleSlide},
    ${function:New-SrsranCaseSlide},
    ${function:New-UpstreamDiffSlide},
    ${function:New-SectionTestSlide},
    ${function:New-ActualResultSlide},
    ${function:New-ExecutionEvidenceSlide},
    ${function:New-ScoreResultSlide},
    ${function:New-AiFindingsSlide},
    ${function:New-CodeReviewFindingsSlide},
    ${function:New-NextTaskSlide},
    ${function:New-ConclusionSlide}
)
$slideCount = $slideWriters.Count

New-Item -ItemType Directory -Force -Path $TempRoot | Out-Null

$slideOverrides = for ($i = 1; $i -le $slideCount; $i++) {
    "<Override PartName=`"/ppt/slides/slide$i.xml`" ContentType=`"application/vnd.openxmlformats-officedocument.presentationml.slide+xml`"/>"
}

$contentTypes = @"
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
  <Default Extension="xml" ContentType="application/xml"/>
  <Override PartName="/docProps/app.xml" ContentType="application/vnd.openxmlformats-officedocument.extended-properties+xml"/>
  <Override PartName="/docProps/core.xml" ContentType="application/vnd.openxmlformats-package.core-properties+xml"/>
  <Override PartName="/ppt/presentation.xml" ContentType="application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml"/>
  <Override PartName="/ppt/slideMasters/slideMaster1.xml" ContentType="application/vnd.openxmlformats-officedocument.presentationml.slideMaster+xml"/>
  <Override PartName="/ppt/slideLayouts/slideLayout1.xml" ContentType="application/vnd.openxmlformats-officedocument.presentationml.slideLayout+xml"/>
  <Override PartName="/ppt/theme/theme1.xml" ContentType="application/vnd.openxmlformats-officedocument.theme+xml"/>
  $($slideOverrides -join "`n  ")
</Types>
"@
Write-TextFile -Path (Join-Path $TempRoot "[Content_Types].xml") -Content $contentTypes

$rootRels = @"
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="ppt/presentation.xml"/>
  <Relationship Id="rId2" Type="http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties" Target="docProps/core.xml"/>
  <Relationship Id="rId3" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/extended-properties" Target="docProps/app.xml"/>
</Relationships>
"@
Write-TextFile -Path (Join-Path $TempRoot "_rels\.rels") -Content $rootRels

$now = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
$coreXml = @"
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<cp:coreProperties xmlns:cp="http://schemas.openxmlformats.org/package/2006/metadata/core-properties"
                   xmlns:dc="http://purl.org/dc/elements/1.1/"
                   xmlns:dcterms="http://purl.org/dc/terms/"
                   xmlns:dcmitype="http://purl.org/dc/dcmitype/"
                   xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">
  <dc:title>AI在通信软件辅助编程中的作用测评</dc:title>
  <dc:creator>Codex</dc:creator>
  <cp:lastModifiedBy>Codex</cp:lastModifiedBy>
  <dcterms:created xsi:type="dcterms:W3CDTF">$now</dcterms:created>
  <dcterms:modified xsi:type="dcterms:W3CDTF">$now</dcterms:modified>
</cp:coreProperties>
"@
Write-TextFile -Path (Join-Path $TempRoot "docProps\core.xml") -Content $coreXml

$appXml = @"
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Properties xmlns="http://schemas.openxmlformats.org/officeDocument/2006/extended-properties"
            xmlns:vt="http://schemas.openxmlformats.org/officeDocument/2006/docPropsVTypes">
  <Application>Codex OpenXML Generator</Application>
  <PresentationFormat>宽屏</PresentationFormat>
  <Slides>$slideCount</Slides>
  <Company></Company>
</Properties>
"@
Write-TextFile -Path (Join-Path $TempRoot "docProps\app.xml") -Content $appXml

$slideIdXml = for ($i = 1; $i -le $slideCount; $i++) {
    $id = 255 + $i
    $rid = $i + 1
    "<p:sldId id=`"$id`" r:id=`"rId$rid`"/>"
}
$presentationRels = New-Object System.Collections.Generic.List[string]
$presentationRels.Add('<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideMaster" Target="slideMasters/slideMaster1.xml"/>')
for ($i = 1; $i -le $slideCount; $i++) {
    $rid = $i + 1
    $presentationRels.Add("<Relationship Id=`"rId$rid`" Type=`"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slide`" Target=`"slides/slide$i.xml`"/>")
}

$presentationXml = @"
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<p:presentation xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
                xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"
                xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">
  <p:sldMasterIdLst>
    <p:sldMasterId id="2147483648" r:id="rId1"/>
  </p:sldMasterIdLst>
  <p:sldIdLst>
    $($slideIdXml -join "`n    ")
  </p:sldIdLst>
  <p:sldSz cx="12192000" cy="6858000" type="wide"/>
  <p:notesSz cx="6858000" cy="9144000"/>
  <p:defaultTextStyle>
    <a:defPPr>
      <a:defRPr lang="zh-CN">
        <a:latin typeface="Microsoft YaHei"/>
        <a:ea typeface="Microsoft YaHei"/>
      </a:defRPr>
    </a:defPPr>
  </p:defaultTextStyle>
</p:presentation>
"@
Write-TextFile -Path (Join-Path $TempRoot "ppt\presentation.xml") -Content $presentationXml

$presentationRelsXml = @"
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  $($presentationRels -join "`n  ")
</Relationships>
"@
Write-TextFile -Path (Join-Path $TempRoot "ppt\_rels\presentation.xml.rels") -Content $presentationRelsXml

$groupXml = New-GroupShapeXml
$slideMasterXml = @"
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<p:sldMaster xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
             xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"
             xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">
  <p:cSld>
    <p:spTree>
      $groupXml
    </p:spTree>
  </p:cSld>
  <p:clrMap bg1="lt1" tx1="dk1" bg2="lt2" tx2="dk2" accent1="accent1" accent2="accent2" accent3="accent3" accent4="accent4" accent5="accent5" accent6="accent6" hlink="hlink" folHlink="folHlink"/>
  <p:sldLayoutIdLst>
    <p:sldLayoutId id="2147483649" r:id="rId1"/>
  </p:sldLayoutIdLst>
  <p:txStyles>
    <p:titleStyle><a:lvl1pPr><a:defRPr sz="2800"/></a:lvl1pPr></p:titleStyle>
    <p:bodyStyle><a:lvl1pPr><a:defRPr sz="1600"/></a:lvl1pPr></p:bodyStyle>
    <p:otherStyle><a:lvl1pPr><a:defRPr sz="1400"/></a:lvl1pPr></p:otherStyle>
  </p:txStyles>
</p:sldMaster>
"@
Write-TextFile -Path (Join-Path $TempRoot "ppt\slideMasters\slideMaster1.xml") -Content $slideMasterXml

$slideMasterRelsXml = @"
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideLayout" Target="../slideLayouts/slideLayout1.xml"/>
  <Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/theme" Target="../theme/theme1.xml"/>
</Relationships>
"@
Write-TextFile -Path (Join-Path $TempRoot "ppt\slideMasters\_rels\slideMaster1.xml.rels") -Content $slideMasterRelsXml

$slideLayoutXml = @"
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<p:sldLayout xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
             xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"
             xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main"
             type="blank" preserve="1">
  <p:cSld name="Blank">
    <p:spTree>
      $groupXml
    </p:spTree>
  </p:cSld>
  <p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr>
</p:sldLayout>
"@
Write-TextFile -Path (Join-Path $TempRoot "ppt\slideLayouts\slideLayout1.xml") -Content $slideLayoutXml

$slideLayoutRelsXml = @"
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideMaster" Target="../slideMasters/slideMaster1.xml"/>
</Relationships>
"@
Write-TextFile -Path (Join-Path $TempRoot "ppt\slideLayouts\_rels\slideLayout1.xml.rels") -Content $slideLayoutRelsXml

$themeXml = @"
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<a:theme xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" name="AI Comm Eval Polished">
  <a:themeElements>
    <a:clrScheme name="AI Comm Eval Polished">
      <a:dk1><a:srgbClr val="101623"/></a:dk1>
      <a:lt1><a:srgbClr val="F8FAFC"/></a:lt1>
      <a:dk2><a:srgbClr val="172033"/></a:dk2>
      <a:lt2><a:srgbClr val="E8F6F3"/></a:lt2>
      <a:accent1><a:srgbClr val="0E9384"/></a:accent1>
      <a:accent2><a:srgbClr val="F59E0B"/></a:accent2>
      <a:accent3><a:srgbClr val="4F46E5"/></a:accent3>
      <a:accent4><a:srgbClr val="E11D48"/></a:accent4>
      <a:accent5><a:srgbClr val="64748B"/></a:accent5>
      <a:accent6><a:srgbClr val="14B8A6"/></a:accent6>
      <a:hlink><a:srgbClr val="2563EB"/></a:hlink>
      <a:folHlink><a:srgbClr val="7C3AED"/></a:folHlink>
    </a:clrScheme>
    <a:fontScheme name="Microsoft YaHei">
      <a:majorFont><a:latin typeface="Microsoft YaHei"/><a:ea typeface="Microsoft YaHei"/><a:cs typeface="Microsoft YaHei"/></a:majorFont>
      <a:minorFont><a:latin typeface="Microsoft YaHei"/><a:ea typeface="Microsoft YaHei"/><a:cs typeface="Microsoft YaHei"/></a:minorFont>
    </a:fontScheme>
    <a:fmtScheme name="AI Comm Eval Polished">
      <a:fillStyleLst>
        <a:solidFill><a:schemeClr val="phClr"/></a:solidFill>
        <a:solidFill><a:schemeClr val="phClr"><a:tint val="94000"/></a:schemeClr></a:solidFill>
        <a:solidFill><a:schemeClr val="phClr"/></a:solidFill>
      </a:fillStyleLst>
      <a:lnStyleLst>
        <a:ln w="9000"><a:solidFill><a:schemeClr val="phClr"/></a:solidFill><a:prstDash val="solid"/></a:ln>
        <a:ln w="12700"><a:solidFill><a:schemeClr val="phClr"/></a:solidFill><a:prstDash val="solid"/></a:ln>
        <a:ln w="19050"><a:solidFill><a:schemeClr val="phClr"/></a:solidFill><a:prstDash val="solid"/></a:ln>
      </a:lnStyleLst>
      <a:effectStyleLst>
        <a:effectStyle><a:effectLst/></a:effectStyle>
        <a:effectStyle><a:effectLst/></a:effectStyle>
        <a:effectStyle><a:effectLst/></a:effectStyle>
      </a:effectStyleLst>
      <a:bgFillStyleLst>
        <a:solidFill><a:schemeClr val="phClr"/></a:solidFill>
        <a:solidFill><a:schemeClr val="phClr"><a:tint val="95000"/></a:schemeClr></a:solidFill>
        <a:solidFill><a:schemeClr val="phClr"/></a:solidFill>
      </a:bgFillStyleLst>
    </a:fmtScheme>
  </a:themeElements>
  <a:objectDefaults/>
  <a:extraClrSchemeLst/>
</a:theme>
"@
Write-TextFile -Path (Join-Path $TempRoot "ppt\theme\theme1.xml") -Content $themeXml

for ($i = 0; $i -lt $slideWriters.Count; $i++) {
    $slideNumber = $i + 1
    $xml = & $slideWriters[$i] -Page $slideNumber -Total $slideCount
    Write-TextFile -Path (Join-Path $TempRoot "ppt\slides\slide$slideNumber.xml") -Content $xml
    $slideRel = @"
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideLayout" Target="../slideLayouts/slideLayout1.xml"/>
</Relationships>
"@
    Write-TextFile -Path (Join-Path $TempRoot "ppt\slides\_rels\slide$slideNumber.xml.rels") -Content $slideRel
}

if (Test-Path $OutFile) {
    Remove-Item -LiteralPath $OutFile -Force
}
[System.IO.Compression.ZipFile]::CreateFromDirectory($TempRoot, $OutFile)
Remove-Item -LiteralPath $TempRoot -Recurse -Force

Write-Host "Created $OutFile"
