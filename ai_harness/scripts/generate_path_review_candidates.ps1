param(
  [string]$ReportPath = "ai_harness/audit/goal_mode_import_report.md",
  [string]$AcceptedCandidatePath = "ai_harness/audit/accepted_paths.candidate.txt",
  [string]$RejectedCandidatePath = "ai_harness/audit/rejected_or_quarantined_paths.candidate.txt"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $ReportPath)) {
  throw "Missing import report: $ReportPath"
}

$acceptedClasses = @(
  "CU-CP allowed",
  "Harness/test/docs only"
)

$rejectedClasses = @(
  "CU-CP-adjacent exception candidate",
  "Out of CU-CP scope",
  "Unknown / human review required"
)

$accepted = New-Object System.Collections.Generic.List[string]
$rejected = New-Object System.Collections.Generic.List[string]

foreach ($line in Get-Content $ReportPath) {
  if ($line -notmatch '^\|\s+(\d+)\s+\|\s+[^|]+\|\s+`([^`]+)`\s+\|\s+([^|]+?)\s+\|') {
    continue
  }

  $path = $Matches[2].Trim()
  $classification = $Matches[3].Trim()

  if ($acceptedClasses -contains $classification) {
    $accepted.Add($path)
  } elseif ($rejectedClasses -contains $classification) {
    $rejected.Add($path)
  } else {
    $rejected.Add($path)
  }
}

@(
  "# Candidate accepted paths generated from goal_mode_import_report.md."
  "# Review manually before copying to accepted_paths.txt."
  "# Strategy: accept only CU-CP allowed plus harness/test/docs-only paths."
  ""
  $accepted
) | Set-Content -Path $AcceptedCandidatePath -Encoding UTF8

@(
  "# Candidate rejected/quarantined paths generated from goal_mode_import_report.md."
  "# Review manually before copying to rejected_or_quarantined_paths.txt."
  "# Strategy: quarantine all CU-CP-adjacent, out-of-scope, and unknown paths."
  ""
  $rejected
) | Set-Content -Path $RejectedCandidatePath -Encoding UTF8

Write-Host "Accepted candidate paths: $($accepted.Count)"
Write-Host "Rejected/quarantined candidate paths: $($rejected.Count)"
