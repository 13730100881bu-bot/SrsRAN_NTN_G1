param(
  [string]$TaskId = "",
  [string]$TaskFile = "",
  [string]$Base = "ai/cucp-harness-base",
  [string]$BuildDir = "build/ai-clean",
  [string]$CTestRegex = "",
  [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"

if ($TaskId -eq "") {
  Write-Error "TaskId is required"
  exit 2
}
if ($TaskFile -eq "") {
  Write-Error "TaskFile is required"
  exit 2
}

$timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
$resultDir = Join-Path "ai_harness/results" "$TaskId-$timestamp"
New-Item -ItemType Directory -Force -Path $resultDir | Out-Null

function Invoke-Logged {
  param(
    [string]$Name,
    [scriptblock]$Command
  )

  $log = Join-Path $resultDir "$Name.log"
  Write-Host "Running $Name..."
  $global:LASTEXITCODE = 0
  $oldErrorActionPreference = $ErrorActionPreference
  $ErrorActionPreference = "Continue"
  try {
    & $Command 2>&1 | Tee-Object -FilePath $log
    $exitCode = $global:LASTEXITCODE
  } catch {
    $_ | Out-File -FilePath $log -Append
    $ErrorActionPreference = $oldErrorActionPreference
    throw
  }
  $ErrorActionPreference = $oldErrorActionPreference
  if ($exitCode -ne 0) {
    throw "$Name failed with exit code $exitCode. See $log"
  }
}

function Invoke-PythonMetadata {
  $log = Join-Path $resultDir "task_metadata.log"
  Write-Host "Running task metadata..."
  $global:LASTEXITCODE = 0
  $pythonOk = $false
  $python = Get-Command python -ErrorAction SilentlyContinue
  $oldErrorActionPreference = $ErrorActionPreference
  $ErrorActionPreference = "Continue"
  if ($python -ne $null -and -not $python.Source.Contains("WindowsApps")) {
    & python ai_harness/scripts/validate_task_metadata.py $TaskFile 2>&1 | Tee-Object -FilePath $log
    $pythonOk = ($global:LASTEXITCODE -eq 0)
  }
  if (-not $pythonOk) {
    $bash = Get-Command bash -ErrorAction SilentlyContinue
    if ($bash -eq $null) {
      $ErrorActionPreference = $oldErrorActionPreference
      throw "No real python or bash/python3 available for task metadata validation. See $log"
    }
    $cmd = "bash -lc ""python3 ai_harness/scripts/validate_task_metadata.py $TaskFile"" 2>&1"
    & cmd.exe /d /c $cmd | Tee-Object -FilePath $log
    $exitCode = $global:LASTEXITCODE
    $ErrorActionPreference = $oldErrorActionPreference
    if ($exitCode -ne 0) {
      throw "task_metadata failed with exit code $exitCode. See $log"
    }
  } else {
    $ErrorActionPreference = $oldErrorActionPreference
  }
}

Invoke-Logged "path_guard" {
  powershell -NoProfile -ExecutionPolicy Bypass -File ai_harness/scripts/guard_changed_paths.ps1 -Base $Base -TaskFile $TaskFile
}

Invoke-Logged "rejected_overlap" {
  powershell -NoProfile -ExecutionPolicy Bypass -File ai_harness/scripts/check_rejected_overlap.ps1 -Base $Base
}

Invoke-PythonMetadata

if (-not $SkipBuild) {
  $env:BUILD_DIR = $BuildDir
  Invoke-Logged "configure" {
    bash ai_harness/scripts/configure_build.sh
  }
  Invoke-Logged "build" {
    bash ai_harness/scripts/build.sh
  }
  if ($CTestRegex -ne "") {
    Invoke-Logged "ctest_focused" {
      ctest --test-dir $BuildDir --output-on-failure -R $CTestRegex
    }
  }
  Invoke-Logged "cucp_tests" {
    bash ai_harness/scripts/run_cucp_tests.sh
  }
}

$summary = Join-Path $resultDir "run_summary.md"
@"
# $TaskId-$timestamp

## Base

`$Base`

## Task

`$TaskFile`

## Validation results

- Path guard: see `path_guard.log`
- Rejected overlap: see `rejected_overlap.log`
- Task metadata: see `task_metadata.log`
- Build skipped: $SkipBuild

"@ | Out-File -FilePath $summary -Encoding utf8

Write-Host "Task validation complete. Results: $resultDir"
