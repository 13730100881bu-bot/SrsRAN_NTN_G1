$ErrorActionPreference = "Stop"

Add-Type -AssemblyName System.IO.Compression.FileSystem

$RepoRoot = Split-Path -Parent $PSScriptRoot
$OutFile = Join-Path $RepoRoot "docs\ai_comm_software_assist_eval.pptx"
$TempRoot = Join-Path $env:TEMP ("ai_comm_eval_ppt_" + [Guid]::NewGuid().ToString("N"))
$Utf8NoBom = New-Object System.Text.UTF8Encoding -ArgumentList $false

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

function New-RectXml {
    param(
        [int]$Id,
        [string]$Name,
        [int64]$X,
        [int64]$Y,
        [int64]$W,
        [int64]$H,
        [string]$Fill,
        [string]$Stroke = "",
        [string]$Geom = "rect"
    )
    $lineXml = if ([string]::IsNullOrWhiteSpace($Stroke)) {
        '<a:ln><a:noFill/></a:ln>'
    } else {
        "<a:ln w=`"12700`"><a:solidFill><a:srgbClr val=`"$Stroke`"/></a:solidFill></a:ln>"
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
    <a:solidFill><a:srgbClr val="$Fill"/></a:solidFill>
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
        [string]$Color = "1F2937",
        [bool]$Bold = $false,
        [string]$Align = "l",
        [bool]$Bullet = $false
    )
    $boldXml = if ($Bold) { ' b="1"' } else { "" }
    $pPr = if ($Bullet) {
        "<a:pPr marL=`"285750`" indent=`"-171450`"><a:buChar char=`"•`"/><a:defRPr sz=`"$($Size * 100)`"/></a:pPr>"
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
        [string]$ParagraphXml
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
    <a:bodyPr wrap="square" rtlCol="0"><a:spAutoFit/></a:bodyPr>
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
        [int]$Size = 20
    )
    $paragraphs = foreach ($bullet in $Bullets) {
        New-ParagraphXml -Text $bullet -Size $Size -Color "253042" -Bullet $true
    }
    return New-TextBoxXml -Id $Id -Name "Bullets" -X $X -Y $Y -W $W -H $H -ParagraphXml ($paragraphs -join "`n")
}

function New-FooterXml {
    param([int]$Id, [int]$Page, [int]$Total)
    $left = New-ParagraphXml -Text "AI辅助通信软件编程测评 | 2026.05" -Size 9 -Color "64748B"
    $right = New-ParagraphXml -Text ("{0:00}/{1:00}" -f $Page, $Total) -Size 9 -Color "64748B" -Align "r"
    return @(
        (New-TextBoxXml -Id $Id -Name "FooterLeft" -X 520000 -Y 6460000 -W 5200000 -H 220000 -ParagraphXml $left),
        (New-TextBoxXml -Id ($Id + 1) -Name "FooterRight" -X 10350000 -Y 6460000 -W 1300000 -H 220000 -ParagraphXml $right)
    ) -join "`n"
}

function New-SlideXml {
    param(
        [int]$Index,
        [int]$Total,
        [hashtable]$Slide
    )
    $shapeId = 2
    $shapes = New-Object System.Collections.Generic.List[string]
    $shapes.Add((New-RectXml -Id $shapeId -Name "Background" -X 0 -Y 0 -W 12192000 -H 6858000 -Fill "F7F9FB")); $shapeId++
    $shapes.Add((New-RectXml -Id $shapeId -Name "AccentLine" -X 0 -Y 0 -W 12192000 -H 105000 -Fill "1B7F8C")); $shapeId++

    if ($Slide.Layout -eq "title") {
        $shapes.Add((New-RectXml -Id $shapeId -Name "TitleAccent" -X 520000 -Y 900000 -W 110000 -H 2600000 -Fill "1B7F8C")); $shapeId++
        $shapes.Add((New-TextBoxXml -Id $shapeId -Name "Title" -X 790000 -Y 880000 -W 10100000 -H 1250000 -ParagraphXml (New-ParagraphXml -Text $Slide.Title -Size 34 -Color "111827" -Bold $true))); $shapeId++
        $shapes.Add((New-TextBoxXml -Id $shapeId -Name "Subtitle" -X 810000 -Y 2150000 -W 9800000 -H 520000 -ParagraphXml (New-ParagraphXml -Text $Slide.Subtitle -Size 18 -Color "475569"))); $shapeId++
        $chips = @("测评目标", "任务设计", "指标体系")
        $chipX = 810000
        foreach ($chip in $chips) {
            $shapes.Add((New-RectXml -Id $shapeId -Name "Chip" -X $chipX -Y 3170000 -W 1520000 -H 410000 -Fill "E6F4F1" -Stroke "95CFC7" -Geom "roundRect")); $shapeId++
            $shapes.Add((New-TextBoxXml -Id $shapeId -Name "ChipText" -X ($chipX + 130000) -Y 3260000 -W 1260000 -H 210000 -ParagraphXml (New-ParagraphXml -Text $chip -Size 12 -Color "0F766E" -Bold $true -Align "ctr"))); $shapeId++
            $chipX += 1750000
        }
        $shapes.Add((New-BulletBoxXml -Id $shapeId -X 810000 -Y 4100000 -W 10000000 -H 1150000 -Bullets $Slide.Bullets -Size 17)); $shapeId++
    } else {
        $section = New-ParagraphXml -Text $Slide.Section -Size 10 -Color "1B7F8C" -Bold $true -Align "r"
        $shapes.Add((New-TextBoxXml -Id $shapeId -Name "Section" -X 9860000 -Y 280000 -W 1700000 -H 240000 -ParagraphXml $section)); $shapeId++
        $shapes.Add((New-TextBoxXml -Id $shapeId -Name "Title" -X 520000 -Y 330000 -W 9100000 -H 520000 -ParagraphXml (New-ParagraphXml -Text $Slide.Title -Size 27 -Color "111827" -Bold $true))); $shapeId++
        $shapes.Add((New-RectXml -Id $shapeId -Name "KeyBox" -X 520000 -Y 1040000 -W 11150000 -H 700000 -Fill "E8F6F3" -Stroke "A7D8D1" -Geom "roundRect")); $shapeId++
        $shapes.Add((New-TextBoxXml -Id $shapeId -Name "KeyMessage" -X 790000 -Y 1190000 -W 10600000 -H 260000 -ParagraphXml (New-ParagraphXml -Text $Slide.Key -Size 17 -Color "0F3D46" -Bold $true))); $shapeId++
        $shapes.Add((New-BulletBoxXml -Id $shapeId -X 780000 -Y 2050000 -W 10550000 -H 3800000 -Bullets $Slide.Bullets -Size 18)); $shapeId++
    }

    $shapes.Add((New-FooterXml -Id $shapeId -Page $Index -Total $Total))
    $shapeXml = $shapes -join "`n"
    $groupXml = New-GroupShapeXml

    return @"
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<p:sld xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
       xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"
       xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">
  <p:cSld>
    <p:spTree>
      $groupXml
      $shapeXml
    </p:spTree>
  </p:cSld>
  <p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr>
</p:sld>
"@
}

$slides = @(
    @{
        Layout = "title"
        Title = "AI在通信软件辅助编程中的作用测评"
        Subtitle = "面向5G/O-RAN/SDR协议栈的效率、质量与风险评估"
        Bullets = @(
            "测评对象：代码理解、实现、测试、调试、优化、文档与评审",
            "样例载体：srsRAN等开源通信软件，也可替换为内部代码库",
            "输出形式：量化指标、典型案例、风险边界和落地建议"
        )
    },
    @{
        Section = "01 / 测评目标"
        Title = "为什么要测：AI到底帮到了哪里"
        Key = "目标不是证明AI万能，而是找到可复用、可度量、可管控的协作方式。"
        Bullets = @(
            "量化AI对开发效率、代码质量、问题定位和知识传递的影响",
            "识别AI最适合介入的通信软件开发环节",
            "区分「可自动化辅助」和「必须专家把关」的任务边界",
            "形成工具链接入、提示规范、评审机制和安全约束"
        )
    },
    @{
        Section = "02 / 工程特点"
        Title = "通信软件为什么特别适合也特别难"
        Key = "通信软件有大量结构化规则，但实时性、并发和协议正确性让AI输出必须被严格验证。"
        Bullets = @(
            "协议栈层次深：PHY、MAC、RLC、PDCP、RRC、NGAP等模块互相影响",
            "实时性要求高：线程、定时器、内存、吞吐和时延都会改变结果",
            "标准和实现强相关：3GPP/O-RAN术语、状态机和消息格式容易误读",
            "测试环境复杂：需要单元测试、仿真、集成测试、日志回放和空口场景验证"
        )
    },
    @{
        Section = "03 / 介入环节"
        Title = "AI可介入的六类编程活动"
        Key = "越靠近代码理解、测试补齐和日志初筛，收益越稳定；越靠近协议语义和实时路径，越需要专家审查。"
        Bullets = @(
            "代码理解：模块职责、调用链、接口约定、状态机解释",
            "代码实现：配置项扩展、字段处理、错误路径和小功能补齐",
            "测试生成：单元测试、mock/fake、边界条件和回归用例",
            "调试定位：日志归因、崩溃栈分析、失败测试最小化",
            "性能优化：热点路径、锁竞争、内存分配和实时路径分析",
            "文档评审：变更说明、接口文档、代码审查清单"
        )
    },
    @{
        Section = "04 / 任务集"
        Title = "测评任务集设计"
        Key = "任务要覆盖真实开发闭环：理解需求、修改代码、验证测试、解释风险。"
        Bullets = @(
            "代码理解类：解释模块职责、关键调用链和数据流",
            "功能实现类：新增配置项、协议字段处理或错误分支",
            "缺陷修复类：根据测试失败、日志或崩溃栈定位并修复问题",
            "测试补齐类：为关键模块补单元测试、边界测试和mock",
            "性能分析类：针对热点路径提出优化并跑基准测试",
            "文档评审类：生成变更说明、接口说明和风险清单"
        )
    },
    @{
        Section = "05 / 实验分组"
        Title = "怎么做对照实验"
        Key = "用相同任务、相同时间盒和相同验收标准比较不同AI使用方式。"
        Bullets = @(
            "A组：人工开发，不使用AI，作为基线",
            "B组：使用通用问答式AI，主要依靠人工粘贴上下文",
            "C组：使用代码库上下文增强的AI/IDE/Agent",
            "参与者分层：通信背景、C/C++经验、项目熟悉度",
            "统一约束：任务卡、代码版本、测试环境、时间限制和评分表"
        )
    },
    @{
        Section = "06 / 指标体系"
        Title = "评价指标：既看快，也看稳"
        Key = "通信软件测评不能只看代码是否生成，还要看协议行为、实时性和回归风险。"
        Bullets = @(
            "效率：完成时间、上下文查找时间、调试轮次、提示交互次数",
            "正确性：测试通过率、协议行为一致性、边界条件覆盖",
            "质量：可维护性、接口一致性、代码风格、复杂度变化",
            "稳定性：内存、线程、实时性、吞吐、时延和回归风险",
            "协作成本：人工修改比例、评审发现问题数、返工次数"
        )
    },
    @{
        Section = "07 / 典型用例"
        Title = "可直接落地的典型测评用例"
        Key = "优先选择能被自动测试和人工评审共同判定的任务，避免主观打分过重。"
        Bullets = @(
            "新增gNB/UE配置项，并贯穿解析、校验、日志和文档",
            "修复一个RRC/MAC消息处理边界条件问题",
            "根据失败测试补齐协议字段编码/解码逻辑",
            "根据运行日志定位一次接入失败或调度异常",
            "为关键模块补充mock和单元测试",
            "对高频路径进行小范围性能优化并跑基准测试"
        )
    },
    @{
        Section = "08 / 风险边界"
        Title = "AI辅助通信编程的主要风险"
        Key = "AI可以降低探索成本，但不能绕过协议校验、并发审查和安全合规。"
        Bullets = @(
            "编造不存在的API、协议字段、配置语义或测试前提",
            "忽略实时约束、竞态条件、生命周期和跨线程资源释放",
            "对标准文本理解不完整，导致状态机或消息字段语义错误",
            "生成的测试只覆盖表面路径，无法证明空口场景正确",
            "内部代码、日志和数据输入AI前需要权限控制和脱敏"
        )
    },
    @{
        Section = "09 / 落地流程"
        Title = "从测评到工程落地"
        Key = "把AI放进现有工程纪律中：任务卡、CI、静态检查、评审和复盘。"
        Bullets = @(
            "准备任务卡、基线代码、测试脚本和评分表",
            "建立AI使用规范：上下文输入、提示模板、禁止事项、审查要求",
            "执行测评：记录耗时、提示、代码变更、测试结果和人工干预",
            "汇总分析：按任务类型和开发者画像比较增益与风险",
            "输出建议：推荐场景、慎用场景、工具链接入方式和后续计划"
        )
    },
    @{
        Section = "10 / 预期结论"
        Title = "预期结论：副驾驶，而不是自动驾驶"
        Key = "AI最适合承担探索、草拟和检查工作；关键协议判断仍要由工程体系兜底。"
        Bullets = @(
            "高收益场景：代码理解、测试补齐、日志初筛、文档生成、评审清单",
            "谨慎场景：协议状态机修改、实时路径优化、复杂并发和跨模块重构",
            "最佳实践：AI建议 + 自动化测试 + 静态检查 + 专家评审",
            "团队收益：降低新成员上手成本，提升重复性工程工作的处理速度"
        )
    },
    @{
        Section = "11 / 下一步"
        Title = "四周试点计划"
        Key = "先用小规模、可量化、低风险任务跑通闭环，再逐步接入真实迭代。"
        Bullets = @(
            "第1周：确定任务集、评分表、基线环境和AI使用规范",
            "第2周：开展小规模开发者测评，收集过程数据和代码结果",
            "第3周：复盘指标，沉淀提示模板、风险清单和典型案例",
            "第4周：扩展到真实迭代任务，评估长期生产力变化"
        )
    }
)

New-Item -ItemType Directory -Force -Path $TempRoot | Out-Null

$slideOverrides = for ($i = 1; $i -le $slides.Count; $i++) {
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
  <Slides>$($slides.Count)</Slides>
  <Company></Company>
</Properties>
"@
Write-TextFile -Path (Join-Path $TempRoot "docProps\app.xml") -Content $appXml

$slideIdXml = for ($i = 1; $i -le $slides.Count; $i++) {
    $id = 255 + $i
    $rid = $i + 1
    "<p:sldId id=`"$id`" r:id=`"rId$rid`"/>"
}
$presentationRels = New-Object System.Collections.Generic.List[string]
$presentationRels.Add('<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideMaster" Target="slideMasters/slideMaster1.xml"/>')
for ($i = 1; $i -le $slides.Count; $i++) {
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
    <p:titleStyle><a:lvl1pPr><a:defRPr sz="3200"/></a:lvl1pPr></p:titleStyle>
    <p:bodyStyle><a:lvl1pPr><a:defRPr sz="1800"/></a:lvl1pPr></p:bodyStyle>
    <p:otherStyle><a:lvl1pPr><a:defRPr sz="1600"/></a:lvl1pPr></p:otherStyle>
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
<a:theme xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" name="AI Comm Eval">
  <a:themeElements>
    <a:clrScheme name="AI Comm Eval">
      <a:dk1><a:srgbClr val="111827"/></a:dk1>
      <a:lt1><a:srgbClr val="F7F9FB"/></a:lt1>
      <a:dk2><a:srgbClr val="253042"/></a:dk2>
      <a:lt2><a:srgbClr val="E8F6F3"/></a:lt2>
      <a:accent1><a:srgbClr val="1B7F8C"/></a:accent1>
      <a:accent2><a:srgbClr val="0F766E"/></a:accent2>
      <a:accent3><a:srgbClr val="F59E0B"/></a:accent3>
      <a:accent4><a:srgbClr val="6366F1"/></a:accent4>
      <a:accent5><a:srgbClr val="64748B"/></a:accent5>
      <a:accent6><a:srgbClr val="EF4444"/></a:accent6>
      <a:hlink><a:srgbClr val="2563EB"/></a:hlink>
      <a:folHlink><a:srgbClr val="7C3AED"/></a:folHlink>
    </a:clrScheme>
    <a:fontScheme name="Microsoft YaHei">
      <a:majorFont><a:latin typeface="Microsoft YaHei"/><a:ea typeface="Microsoft YaHei"/><a:cs typeface="Microsoft YaHei"/></a:majorFont>
      <a:minorFont><a:latin typeface="Microsoft YaHei"/><a:ea typeface="Microsoft YaHei"/><a:cs typeface="Microsoft YaHei"/></a:minorFont>
    </a:fontScheme>
    <a:fmtScheme name="AI Comm Eval">
      <a:fillStyleLst>
        <a:solidFill><a:schemeClr val="phClr"/></a:solidFill>
        <a:gradFill rotWithShape="1"><a:gsLst><a:gs pos="0"><a:schemeClr val="phClr"/></a:gs><a:gs pos="100000"><a:schemeClr val="phClr"><a:tint val="50000"/></a:schemeClr></a:gs></a:gsLst><a:lin ang="5400000" scaled="0"/></a:gradFill>
        <a:solidFill><a:schemeClr val="phClr"/></a:solidFill>
      </a:fillStyleLst>
      <a:lnStyleLst>
        <a:ln w="12700"><a:solidFill><a:schemeClr val="phClr"/></a:solidFill><a:prstDash val="solid"/></a:ln>
        <a:ln w="19050"><a:solidFill><a:schemeClr val="phClr"/></a:solidFill><a:prstDash val="solid"/></a:ln>
        <a:ln w="25400"><a:solidFill><a:schemeClr val="phClr"/></a:solidFill><a:prstDash val="solid"/></a:ln>
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

for ($i = 0; $i -lt $slides.Count; $i++) {
    $slideNumber = $i + 1
    Write-TextFile -Path (Join-Path $TempRoot "ppt\slides\slide$slideNumber.xml") -Content (New-SlideXml -Index $slideNumber -Total $slides.Count -Slide $slides[$i])
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
