param(
  [string]$Base = "ai/cucp-harness-base",
  [string]$AllowedFile = "ai_harness/context/allowed_paths.md",
  [Alias("task-file")]
  [string]$TaskFile = ""
)

$ErrorActionPreference = "Stop"

$forbiddenMarkers = @(
  "/flexible_o_du/",
  "/o_du/",
  "/du/",
  "/du_high/",
  "/du_low/",
  "/mac/",
  "/scheduler/",
  "/phy/",
  "/lower_phy/",
  "/radio/",
  "/ru/",
  "/rf/",
  "/prach/",
  "/harq/",
  "/gis/"
)

function Get-MarkdownPaths {
  param(
    [string]$Path,
    [string]$SectionHeading = ""
  )

  if (-not (Test-Path $Path)) {
    return @()
  }

  $inSection = ($SectionHeading -eq "")
  $paths = @()

  Get-Content $Path | ForEach-Object {
    $line = $_.Trim()
    if ($SectionHeading -ne "") {
      if ($line.StartsWith("## ")) {
        $heading = $line.Substring(3).Trim().ToLowerInvariant()
        $inSection = ($heading -eq $SectionHeading.ToLowerInvariant())
        return
      }
      if ($line.StartsWith("# ") -and $inSection) {
        $inSection = $false
        return
      }
      if (-not $inSection) {
        return
      }
    }
    if ($line.StartsWith('- `') -and $line.EndsWith('`')) {
      $value = $line.Substring(3, $line.Length - 4).Trim()
      if ($value -ne "" -and $value.ToLowerInvariant() -ne "none") {
        $paths += $value
      }
    } elseif ($line.StartsWith("- ")) {
      $value = $line.Substring(2).Trim()
      if ($value.ToLowerInvariant() -ne "none") {
        return
      }
    }
  }

  $paths
}

function Get-ChangedFiles {
  param([string]$BaseRef)

  $changed = New-Object 'System.Collections.Generic.HashSet[string]'
  git diff --name-only $BaseRef -- | ForEach-Object {
    $path = $_.Trim().Replace("\", "/")
    if ($path -ne "") {
      [void]$changed.Add($path)
    }
  }

  git status --porcelain --untracked-files=all | ForEach-Object {
    if ($_.Length -lt 4) {
      return
    }
    $path = $_.Substring(3).Trim()
    if ($path.Contains(" -> ")) {
      $path = ($path -split " -> ", 2)[1].Trim()
    }
    $path = $path.Replace("\", "/")
    if ($path -ne "") {
      [void]$changed.Add($path)
    }
  }

  @($changed) | Sort-Object
}

function Test-AllowedPath {
  param(
    [string]$Path,
    [string[]]$AllowedPrefixes
  )

  foreach ($prefix in $AllowedPrefixes) {
    $normalized = $prefix.TrimEnd("/")
    if ($Path -eq $normalized) {
      return $true
    }
    if ($prefix.EndsWith("/") -and $Path.StartsWith($prefix)) {
      return $true
    }
    if ($Path.StartsWith($prefix)) {
      return $true
    }
  }
  return $false
}

function Test-ForbiddenPath {
  param([string]$Path)

  $normalized = "/" + $Path
  foreach ($marker in $forbiddenMarkers) {
    if ($normalized.Contains($marker)) {
      return $true
    }
  }
  return $false
}

$allowed = @(Get-MarkdownPaths $AllowedFile)
if ($TaskFile -ne "") {
  $allowed += @(Get-MarkdownPaths $TaskFile "Allowed task exception paths")
}
if ($allowed.Count -eq 0) {
  Write-Error "No allowed paths found in $AllowedFile"
  exit 2
}

$violations = @()
$warnings = @()

foreach ($path in Get-ChangedFiles $Base) {
  if (Test-AllowedPath $path $allowed) {
    continue
  }

  if (Test-ForbiddenPath $path) {
    $violations += $path
  } else {
    $warnings += $path
  }
}

if ($violations.Count -ne 0 -or $warnings.Count -ne 0) {
  Write-Host "CU-CP path guard failed.`n"

  if ($violations.Count -ne 0) {
    Write-Host "Likely forbidden non-CU-CP changes:"
    $violations | ForEach-Object { Write-Host "  - $_" }
  }

  if ($warnings.Count -ne 0) {
    Write-Host "`nChanged files outside allowed prefixes:"
    $warnings | ForEach-Object { Write-Host "  - $_" }
  }

  Write-Host "`nAction required:"
  Write-Host "  1. Revert these files, or"
  Write-Host "  2. Add an explicit task-level exception after human review."
  exit 1
}

Write-Host "CU-CP path guard passed."
