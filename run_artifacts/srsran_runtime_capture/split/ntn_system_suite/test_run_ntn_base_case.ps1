$ErrorActionPreference = "Stop"

$SplitDir = Split-Path -Parent $PSScriptRoot
$ScriptPath = Join-Path $SplitDir "run_ntn_base_case.ps1"
$CmdPath = Join-Path $SplitDir "run_ntn_base_case.cmd"

if (-not (Test-Path -LiteralPath $ScriptPath)) {
  throw "missing base case runner: $ScriptPath"
}
if (-not (Test-Path -LiteralPath $CmdPath)) {
  throw "missing cmd base case wrapper: $CmdPath"
}

$listOutput = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $ScriptPath -List 2>&1
if ($LASTEXITCODE -ne 0) {
  throw "-List failed with exit code $LASTEXITCODE`: $listOutput"
}
if (($listOutput | Out-String) -notmatch "CUCP075-BASE-001") {
  throw "-List output does not contain CUCP075-BASE-001"
}

$dryRunOutput = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $ScriptPath 001 -DryRun 2>&1
if ($LASTEXITCODE -ne 0) {
  throw "-DryRun failed with exit code $LASTEXITCODE`: $dryRunOutput"
}
$dryRunText = $dryRunOutput | Out-String
foreach ($expected in @("case_id=CUCP075-BASE-001", "127.0.0.1:38412", "systemctl start open5gs")) {
  if ($dryRunText -notmatch [regex]::Escape($expected)) {
    throw "-DryRun output does not contain expected text: $expected"
  }
}

$cmdDryRunOutput = & cmd.exe /c "`"$CmdPath`" 001 -DryRun" 2>&1
if ($LASTEXITCODE -ne 0) {
  throw "cmd wrapper dry-run failed with exit code $LASTEXITCODE`: $cmdDryRunOutput"
}
$cmdDryRunText = $cmdDryRunOutput | Out-String
if ($cmdDryRunText -notmatch "case_id=CUCP075-BASE-001") {
  throw "cmd wrapper output does not contain CUCP075-BASE-001"
}

Write-Host "run_ntn_base_case selftest passed"
