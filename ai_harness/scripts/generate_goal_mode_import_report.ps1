param(
  [string]$AuditDir = "ai_harness/audit"
)

$ErrorActionPreference = "Stop"

$changedFilesPath = Join-Path $AuditDir "goal_mode_changed_files.txt"
$baselinePath     = Join-Path $AuditDir "baseline_commit.txt"
$snapshotPath     = Join-Path $AuditDir "goal_snapshot_commit.txt"
$reportPath       = Join-Path $AuditDir "goal_mode_import_report.md"

if (-not (Test-Path $changedFilesPath)) {
  throw "Missing changed files list: $changedFilesPath"
}

$baseline = (Get-Content -Raw $baselinePath).Trim()
$snapshot = (Get-Content -Raw $snapshotPath).Trim()
$lines    = Get-Content $changedFilesPath | Where-Object { $_.Trim().Length -gt 0 }

function Convert-ChangeLine {
  param([string]$Line)

  $parts = $Line -split "`t", 2
  if ($parts.Count -ne 2) {
    return [PSCustomObject]@{
      Change = "?"
      Path   = $Line.Trim()
    }
  }

  [PSCustomObject]@{
    Change = $parts[0].Trim()
    Path   = $parts[1].Trim()
  }
}

function New-Classification {
  param(
    [string]$Classification,
    [string]$Action,
    [string]$Rationale
  )

  [PSCustomObject]@{
    Classification = $Classification
    Action         = $Action
    Rationale      = $Rationale
  }
}

function Classify-Path {
  param([string]$Path)

  if ($Path -like "tools/gis_remote_service_site/*") {
    return New-Classification "Out of CU-CP scope" "Quarantine/reject" "Unrelated GIS remote-service web application, not NTN CU-CP."
  }
  if ($Path -like "docs/superpowers/plans/2026-05-23-gis-*") {
    return New-Classification "Out of CU-CP scope" "Quarantine/reject" "GIS planning notes are unrelated to the CU-CP NTN harness."
  }
  if ($Path -eq "scripts/create_ai_comm_eval_ppt_polished.ps1") {
    return New-Classification "Out of CU-CP scope" "Quarantine/reject" "Presentation-generation helper, not needed by CU-CP import."
  }
  if ($Path -like "apps/units/flexible_o_du/*") {
    return New-Classification "Out of CU-CP scope" "Quarantine/reject" "DU application/configuration path; outside CU-CP ownership."
  }
  if ($Path -like "include/srsran/f1ap/du/*" -or
      $Path -like "lib/f1ap/du/*" -or
      $Path -like "tests/unittests/f1ap/du/*") {
    return New-Classification "Out of CU-CP scope" "Quarantine/reject" "DU-side F1AP implementation/test path; not part of CU-CP harness import."
  }
  if ($Path -like "lib/du/*" -or $Path -like "tests/unittests/du_manager/*") {
    return New-Classification "Out of CU-CP scope" "Quarantine/reject" "DU manager/resource scheduling path; keep out of CU-CP-only import."
  }
  if ($Path -like "include/srsran/ntn/*" -or
      $Path -like "lib/ntn/*" -or
      $Path -like "tests/unittests/ntn/*") {
    return New-Classification "Out of CU-CP scope" "Quarantine unless explicitly accepted" "Shared NTN library/test path; useful concepts may be re-homed or accepted only by human exception."
  }

  if ($Path -like "include/srsran/cu_cp/*" -or
      $Path -like "lib/cu_cp/*" -or
      $Path -like "tests/unittests/cu_cp/*") {
    return New-Classification "CU-CP allowed" "Accept" "Direct CU-CP source or CU-CP unit test path."
  }

  if ($Path -like "apps/units/o_cu_cp/*") {
    return New-Classification "CU-CP-adjacent exception candidate" "Accept with app-boundary review" "o_cu_cp app/config glue needed to expose CU-CP NTN controls."
  }
  if ($Path -like "configs/*") {
    return New-Classification "CU-CP-adjacent exception candidate" "Accept with config review" "Runtime examples/configuration used to drive CU-CP NTN behavior."
  }
  if ($Path -eq "include/srsran/f1ap/ntn_ul_slot_resource_request.h") {
    return New-Classification "CU-CP-adjacent exception candidate" "Accept with interface review" "Shared F1AP contract for CU-CP to request NTN uplink slot resources."
  }
  if ($Path -like "include/srsran/f1ap/cu_cp/*" -or
      $Path -like "lib/f1ap/cu_cp/*" -or
      $Path -like "tests/unittests/f1ap/cu_cp/*" -or
      $Path -like "tests/unittests/f1ap/common/*") {
    return New-Classification "CU-CP-adjacent exception candidate" "Accept with F1-C review" "CU-CP-side F1AP/control-plane boundary touched by NTN scheduling/resource requests."
  }
  if ($Path -like "include/srsran/ngap/*" -or
      $Path -like "lib/ngap/*" -or
      $Path -like "tests/unittests/ngap/*") {
    return New-Classification "CU-CP-adjacent exception candidate" "Accept with NGAP review" "NGAP UE context/location reporting can be part of CU-CP NTN control-plane support."
  }
  if ($Path -like "include/srsran/rrc/*" -or $Path -like "lib/rrc/ue/*") {
    return New-Classification "CU-CP-adjacent exception candidate" "Accept with RRC review" "RRC UE control-plane messaging may carry NTN measurement/mobility context."
  }
  if ($Path -like "tests/unittests/apps/units/o_cu_cp/*") {
    return New-Classification "CU-CP-adjacent exception candidate" "Accept with app-test review" "o_cu_cp unit tests for CU-CP application configuration."
  }
  if ($Path -eq "utils/ntn/generate_leo_beam_table.py") {
    return New-Classification "CU-CP-adjacent exception candidate" "Accept with tooling review" "Utility generates beam-table inputs consumed by CU-CP NTN configuration."
  }

  if ($Path -like "docs/*" -or $Path -like "ai_harness/*") {
    return New-Classification "Harness/test/docs only" "Keep for audit/docs" "Documentation or harness artifact, not production runtime code."
  }

  return New-Classification "Unknown / human review required" "Human decision required" "Path is not clearly inside the CU-CP import boundary."
}

function Escape-Cell {
  param([string]$Text)
  return (($Text -replace "\|", "\|") -replace "`r?`n", " ")
}

$items = @()
foreach ($line in $lines) {
  $change = Convert-ChangeLine $line
  $classification = Classify-Path $change.Path
  $items += [PSCustomObject]@{
    Change         = $change.Change
    Path           = $change.Path
    Classification = $classification.Classification
    Action         = $classification.Action
    Rationale      = $classification.Rationale
  }
}

$counts = $items | Group-Object Classification | Sort-Object Name
$generatedAt = Get-Date -Format "yyyy-MM-dd HH:mm:ss zzz"

$acceptedSeed = @(
  "ai_harness/",
  "docs/ai_comm_eval_cucp_opportunities_2026-05-09.md",
  "docs/ai_comm_eval_ntn_iteration_stats_2026-05-08.md",
  "docs/ai_srsran_ntn_cucp_change_summary_2026-05-23.md",
  "docs/assets/ntn_cucp_multibeam_overview.png",
  "docs/assets/ntn_cucp_multibeam_overview.svg",
  "docs/ntn_cucp_leo_500km_plan.md",
  "docs/ntn_cucp_multibeam_visualization.md",
  "configs/",
  "utils/ntn/generate_leo_beam_table.py",
  "apps/units/o_cu_cp/",
  "include/srsran/cu_cp/",
  "lib/cu_cp/",
  "include/srsran/f1ap/cu_cp/",
  "lib/f1ap/cu_cp/",
  "include/srsran/f1ap/ntn_ul_slot_resource_request.h",
  "include/srsran/ngap/",
  "lib/ngap/",
  "include/srsran/rrc/",
  "lib/rrc/ue/",
  "tests/unittests/apps/units/o_cu_cp/",
  "tests/unittests/cu_cp/",
  "tests/unittests/f1ap/common/",
  "tests/unittests/f1ap/cu_cp/",
  "tests/unittests/ngap/"
)

$rejectedSeed = @(
  "apps/units/flexible_o_du/",
  "docs/superpowers/plans/2026-05-23-gis-lead-site-private-admin-upgrade.md",
  "docs/superpowers/plans/2026-05-23-gis-remote-service-lead-site.md",
  "include/srsran/f1ap/du/",
  "include/srsran/ntn/",
  "lib/du/",
  "lib/f1ap/du/",
  "lib/ntn/",
  "scripts/create_ai_comm_eval_ppt_polished.ps1",
  "tests/unittests/apps/units/CMakeLists.txt",
  "tests/unittests/du_manager/",
  "tests/unittests/f1ap/du/",
  "tests/unittests/ntn/",
  "tools/gis_remote_service_site/"
)

$report = New-Object System.Collections.Generic.List[string]
$report.Add("# Goal Mode Import Report")
$report.Add("")
$report.Add("Generated: $generatedAt")
$report.Add("")
$report.Add("Scope: CU-CP NTN harness import audit through Step 3 only. This report does not make Step 4 path decisions.")
$report.Add("")
$report.Add("- Baseline commit: ``$baseline``")
$report.Add("- Goal snapshot commit: ``$snapshot``")
$report.Add("- Changed file source: ``$changedFilesPath``")
$report.Add("- Diff source: ``$(Join-Path $AuditDir "goal_mode_full.diff")``")
$report.Add("- Note: the read-only ``codex exec`` report-generation attempt timed out, so this deterministic local script generated the report from the audit inputs.")
$report.Add("")
$report.Add("## Classification Summary")
$report.Add("")
$report.Add("| Classification | Count | Meaning |")
$report.Add("|---|---:|---|")
foreach ($count in $counts) {
  $meaning = switch ($count.Name) {
    "CU-CP allowed" { "Direct CU-CP source or unit-test changes." }
    "CU-CP-adjacent exception candidate" { "Outside strict CU-CP directories but likely needed for CU-CP config, F1-C, NGAP, RRC, or tooling boundaries." }
    "Harness/test/docs only" { "Audit, documentation, or visualization artifacts." }
    "Out of CU-CP scope" { "DU-side, shared non-CU-CP, GIS, or unrelated support work; quarantine by default." }
    default { "Needs explicit human judgement before import." }
  }
  $report.Add("| $(Escape-Cell $count.Name) | $($count.Count) | $(Escape-Cell $meaning) |")
}
$report.Add("")
$report.Add("## File-by-file Classification")
$report.Add("")
$report.Add("| # | Change | Path | Classification | Recommended action | Rationale |")
$report.Add("|---:|---|---|---|---|---|")
$idx = 1
foreach ($item in $items) {
  $report.Add("| $idx | $(Escape-Cell $item.Change) | ``$(Escape-Cell $item.Path)`` | $(Escape-Cell $item.Classification) | $(Escape-Cell $item.Action) | $(Escape-Cell $item.Rationale) |")
  $idx++
}
$report.Add("")
$report.Add("## Recommended accepted_paths.txt Seed")
$report.Add("")
$report.Add("Human review is still required before Step 4. These are conservative seed paths for a CU-CP-focused import:")
$report.Add("")
$report.Add("````text")
foreach ($path in $acceptedSeed) {
  $report.Add($path)
}
$report.Add("````")
$report.Add("")
$report.Add("## Recommended rejected_or_quarantined_paths.txt Seed")
$report.Add("")
$report.Add("Human review is still required before Step 4. These paths should be quarantined or rejected unless there is an explicit CU-CP exception:")
$report.Add("")
$report.Add("````text")
foreach ($path in $rejectedSeed) {
  $report.Add($path)
}
$report.Add("````")
$report.Add("")
$report.Add("## Risks and Review Notes")
$report.Add("")
$report.Add("- The snapshot contains both CU-CP NTN work and unrelated GIS/site collateral. The GIS/site files should not enter the CU-CP harness branch.")
$report.Add("- Shared ``include/srsran/ntn`` and ``lib/ntn`` changes may contain useful orbit/beam concepts, but they are outside a strict CU-CP import boundary. Accept them only by explicit exception or re-home the minimal logic under CU-CP.")
$report.Add("- DU and DU-side F1AP changes may be needed later for end-to-end SR/SRS/PUCCH/SRS resource realization, but Step 3 keeps them quarantined because the requested focus is CU-CP.")
$report.Add("- o_cu_cp, F1AP CU-CP, NGAP, RRC, configs, and beam-table tooling are adjacent control-plane surfaces. They should be reviewed as exception candidates rather than accepted blindly.")
$report.Add("- Build and unit-test validation are intentionally deferred until the human path decision files are created and Step 4 produces a clean accepted import branch.")
$report.Add("")
$report.Add("## Step 3 Completion Checklist")
$report.Add("")
$report.Add("- [x] Baseline commit recorded.")
$report.Add("- [x] Goal snapshot commit recorded.")
$report.Add("- [x] Full diff/stat/path audit files generated.")
$report.Add("- [x] CUCP-000 task card created.")
$report.Add("- [x] Goal-mode import report generated.")
$report.Add("- [ ] Human reviewer creates ``accepted_paths.txt`` and ``rejected_or_quarantined_paths.txt`` for Step 4.")

$report | Set-Content -Path $reportPath -Encoding UTF8
Write-Host "Generated $reportPath with $($items.Count) classified files."
