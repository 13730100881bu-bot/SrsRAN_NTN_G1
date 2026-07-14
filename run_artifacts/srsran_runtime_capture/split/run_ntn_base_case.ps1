param(
  [Parameter(Position = 0)]
  [string]$CaseId = "",

  [switch]$List,
  [switch]$DryRun,
  [int]$TimeoutSeconds = 75
)

$ErrorActionPreference = "Stop"

function Normalize-BaseCaseId {
  param([string]$Value)

  $trimmed = $Value.Trim()
  if ([string]::IsNullOrWhiteSpace($trimmed)) {
    return ""
  }
  if ($trimmed -match "^\d{3}$") {
    return "CUCP075-BASE-$trimmed"
  }
  if ($trimmed -match "^BASE-(\d{3})$") {
    return "CUCP075-BASE-$($Matches[1])"
  }
  return $trimmed.ToUpperInvariant()
}

function Write-SupportedCases {
  Write-Host "Supported standalone base cases:"
  Write-Host "  001 | BASE-001 | CUCP075-BASE-001  Open5GS AMF readiness check"
  Write-Host ""
  Write-Host "Examples:"
  Write-Host "  .\run_ntn_base_case.ps1 001"
  Write-Host "  .\run_ntn_base_case.ps1 CUCP075-BASE-001 -DryRun"
}

function Invoke-Base001 {
  param(
    [int]$TimeoutSeconds,
    [bool]$DryRun
  )

  $timeout = [Math]::Max(1, $TimeoutSeconds)
  $bashTemplate = @'
set -uo pipefail
echo "case_id=CUCP075-BASE-001"
echo "area=core_network"
echo "check=Open5GS AMF 127.0.0.1:38412"
systemctl start open5gs-udrd open5gs-udmd open5gs-ausfd open5gs-nrfd open5gs-amfd open5gs-smfd open5gs-upfd open5gs-pcfd open5gs-nssfd open5gs-bsfd || true
deadline=$((SECONDS + __TIMEOUT_SECONDS__))
while [ "$SECONDS" -le "$deadline" ]; do
  if ss -a -n -p | grep -q '127.0.0.1:38412'; then
    echo "status=pass"
    echo "reason=Open5GS AMF listener is ready at 127.0.0.1:38412"
    exit 0
  fi
  sleep 1
done
echo "status=fail"
echo "reason=AMF listener 127.0.0.1:38412 was not ready before timeout"
echo "next_step=check Open5GS services and port ownership"
systemctl is-active open5gs-amfd open5gs-smfd open5gs-upfd open5gs-nrfd open5gs-ausfd open5gs-udmd open5gs-udrd open5gs-pcfd open5gs-nssfd open5gs-bsfd || true
ss -a -n -p | grep 38412 || true
exit 1
'@

  $bashCommand = $bashTemplate.Replace("__TIMEOUT_SECONDS__", [string]$timeout)
  if ($DryRun) {
    Write-Host "case_id=CUCP075-BASE-001"
    Write-Host "mode=dry-run"
    Write-Host "would_run=wsl.exe -e bash -lc <Open5GS AMF readiness check>"
    Write-Host "checks=systemctl start open5gs services; wait for 127.0.0.1:38412"
    Write-Host "timeout_seconds=$timeout"
    Write-Host "bash_command=$bashCommand"
    return 0
  }

  & wsl.exe -e bash -lc $bashCommand
  return $LASTEXITCODE
}

if ($List -or [string]::IsNullOrWhiteSpace($CaseId)) {
  Write-SupportedCases
  exit 0
}

$normalized = Normalize-BaseCaseId -Value $CaseId
switch ($normalized) {
  "CUCP075-BASE-001" {
    $exitCode = Invoke-Base001 -TimeoutSeconds $TimeoutSeconds -DryRun:$DryRun.IsPresent
    exit $exitCode
  }
  default {
    Write-Error "Unsupported standalone base case: $CaseId. Run '.\run_ntn_base_case.ps1 -List' for supported cases."
    exit 2
  }
}
