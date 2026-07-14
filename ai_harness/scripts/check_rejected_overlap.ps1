param(
  [string]$Base = "ai/cucp-harness-base",
  [string]$RejectedFile = "ai_harness/audit/rejected_or_quarantined_paths.txt",
  [string]$TaskFile = ""
)

$ErrorActionPreference = "Stop"

function Normalize-RepoPath {
  param([string]$Path)

  $Path.Trim().Replace("\", "/")
}

function Get-RejectedPaths {
  param([string]$Path)

  if (-not (Test-Path $Path)) {
    Write-Error "Rejected/quarantined path file not found: $Path"
    exit 2
  }

  Get-Content $Path | ForEach-Object {
    $line = (Normalize-RepoPath $_)
    if ($line -eq "" -or $line.StartsWith("#")) {
      return
    }
    $line
  }
}

function Get-MarkdownPaths {
  param(
    [string]$Path,
    [string]$SectionHeading
  )

  if ($Path -eq "" -or -not (Test-Path $Path)) {
    return @()
  }

  $paths = @()
  $inSection = $false
  foreach ($rawLine in Get-Content $Path) {
    $line = $rawLine.Trim()
    if ($line.StartsWith("## ")) {
      $heading = $line.Substring(3).Trim().ToLowerInvariant()
      $inSection = ($heading -eq $SectionHeading.ToLowerInvariant())
      continue
    }
    if (-not $inSection) {
      continue
    }
    if ($line.StartsWith("# ")) {
      break
    }
    if ($line.StartsWith("- ``") -and $line.EndsWith("``")) {
      $value = Normalize-RepoPath $line.Substring(3, $line.Length - 4)
      if ($value -ne "" -and $value.ToLowerInvariant() -ne "none") {
        $paths += $value
      }
    }
  }

  return $paths
}

function Get-ChangedFiles {
  param([string]$BaseRef)

  $changed = New-Object 'System.Collections.Generic.HashSet[string]'

  git diff --name-only $BaseRef -- | ForEach-Object {
    $path = Normalize-RepoPath $_
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
    $path = Normalize-RepoPath $path
    if ($path -ne "") {
      [void]$changed.Add($path)
    }
  }

  @($changed) | Sort-Object
}

function Test-TaskExceptionPath {
  param(
    [string]$Path,
    [string[]]$ExceptionPaths
  )

  foreach ($exception in $ExceptionPaths) {
    $normalized = $exception.TrimEnd("/")
    if ($Path -eq $normalized) {
      return $true
    }
    if ($exception.EndsWith("/") -and $Path.StartsWith($exception)) {
      return $true
    }
  }
  return $false
}

function Test-RejectedPath {
  param(
    [string]$Path,
    [string[]]$RejectedPaths
  )

  foreach ($rejected in $RejectedPaths) {
    $normalized = $rejected.TrimEnd("/")
    if ($Path -eq $normalized) {
      return $true
    }
    if ($rejected.EndsWith("/") -and $Path.StartsWith($rejected)) {
      return $true
    }
  }
  return $false
}

$rejected = @(Get-RejectedPaths $RejectedFile)
if ($rejected.Count -eq 0) {
  Write-Error "No rejected/quarantined paths found in $RejectedFile"
  exit 2
}

$taskExceptions = @(Get-MarkdownPaths $TaskFile "Allowed task exception paths")

$violations = @()
foreach ($path in Get-ChangedFiles $Base) {
  if ((Test-RejectedPath $path $rejected) -and -not (Test-TaskExceptionPath $path $taskExceptions)) {
    $violations += $path
  }
}

if ($violations.Count -ne 0) {
  Write-Host "Rejected/quarantined overlap check failed.`n"
  Write-Host "Changed files that overlap rejected/quarantined paths:"
  $violations | ForEach-Object { Write-Host "  - $_" }
  Write-Host "`nAction required:"
  Write-Host "  1. Revert these files, or"
  Write-Host "  2. Move the path from rejected/quarantined to accepted after human review."
  exit 1
}

Write-Host "Rejected/quarantined overlap check passed."
