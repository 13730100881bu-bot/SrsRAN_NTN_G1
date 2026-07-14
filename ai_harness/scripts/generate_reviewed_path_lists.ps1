param(
  [string]$ChangedFilesPath = "ai_harness/audit/goal_mode_changed_files.txt",
  [string]$AcceptedCandidatePath = "ai_harness/audit/accepted_paths.candidate.txt",
  [string]$AcceptedPath = "ai_harness/audit/accepted_paths.txt",
  [string]$RejectedPath = "ai_harness/audit/rejected_or_quarantined_paths.txt",
  [string]$ReviewNotesPath = "ai_harness/audit/path_review_notes.md"
)

$ErrorActionPreference = "Stop"

function Read-PathList {
  param([string]$Path)

  if (-not (Test-Path $Path)) {
    throw "Missing path list: $Path"
  }

  Get-Content $Path | ForEach-Object { $_.Trim() } | Where-Object {
    $_ -ne "" -and -not $_.StartsWith("#")
  }
}

function Read-ChangedPathList {
  param([string]$Path)

  if (-not (Test-Path $Path)) {
    throw "Missing changed files list: $Path"
  }

  Get-Content $Path | ForEach-Object {
    $parts = $_ -split "`t", 2
    if ($parts.Count -eq 2) {
      $parts[1].Trim()
    }
  } | Where-Object { $_ -ne "" }
}

$changed = @(Read-ChangedPathList $ChangedFilesPath)
$accepted = New-Object "System.Collections.Generic.HashSet[string]"

foreach ($path in Read-PathList $AcceptedCandidatePath) {
  [void]$accepted.Add($path)
}

# Reviewed narrow exceptions. These are outside strict lib/include/tests cu_cp prefixes, but are needed by the CU-CP
# snapshot to remain coherent: app config glue, F1-C/NGAP/RRC control-plane contracts, and minimal shared NTN orbit
# support referenced by CU-CP satellite state updater.
$reviewedExceptions = @(
  "apps/units/o_cu_cp/cu_cp/CMakeLists.txt",
  "apps/units/o_cu_cp/cu_cp/cu_cp_cmdline_commands.h",
  "apps/units/o_cu_cp/cu_cp/cu_cp_config_translators.cpp",
  "apps/units/o_cu_cp/cu_cp/cu_cp_unit_config.h",
  "apps/units/o_cu_cp/cu_cp/cu_cp_unit_config_cli11_schema.cpp",
  "apps/units/o_cu_cp/cu_cp/cu_cp_unit_config_validator.cpp",
  "apps/units/o_cu_cp/cu_cp/cu_cp_unit_config_yaml_writer.cpp",
  "apps/units/o_cu_cp/o_cu_cp_builder.cpp",
  "configs/CMakeLists.txt",
  "configs/cu_cp_neighbor_cells_example.json",
  "configs/leo_500km_beam_table.json",
  "configs/leo_500km_cucp_ntn.yml",
  "include/srsran/f1ap/cu_cp/f1ap_cu_ue_context_update.h",
  "include/srsran/f1ap/ntn_ul_slot_resource_request.h",
  "include/srsran/ngap/ngap.h",
  "include/srsran/ngap/ngap_location_reporting.h",
  "include/srsran/rrc/rrc_ue.h",
  "lib/f1ap/cu_cp/procedures/ue_context_modification_procedure.cpp",
  "lib/f1ap/cu_cp/procedures/ue_context_setup_procedure.cpp",
  "lib/ngap/ngap_asn1_converters.h",
  "lib/ngap/ngap_impl.cpp",
  "lib/ngap/ngap_impl.h",
  "lib/rrc/ue/rrc_ue_impl.h",
  "lib/rrc/ue/rrc_ue_message_handlers.cpp",
  "tests/unittests/apps/units/CMakeLists.txt",
  "tests/unittests/apps/units/o_cu_cp/CMakeLists.txt",
  "tests/unittests/apps/units/o_cu_cp/cu_cp/CMakeLists.txt",
  "tests/unittests/apps/units/o_cu_cp/cu_cp/cu_cp_unit_config_test.cpp",
  "tests/unittests/f1ap/common/f1ap_asn1_helpers_test.cpp",
  "tests/unittests/f1ap/cu_cp/f1ap_cu_ue_context_modification_procedure_test.cpp",
  "tests/unittests/f1ap/cu_cp/f1ap_cu_ue_context_setup_procedure_test.cpp",
  "tests/unittests/ngap/ngap_ue_context_management_procedure_test.cpp",
  "tests/unittests/ngap/test_helpers.h",
  "utils/ntn/generate_leo_beam_table.py",
  "include/srsran/ntn/beam_hopping_table.h",
  "lib/ntn/beam_hopping_table.cpp",
  "include/srsran/ntn/orbit_propagator.h",
  "lib/ntn/orbit_propagator.cpp",
  "lib/ntn/CMakeLists.txt"
)

foreach ($path in $reviewedExceptions) {
  if ($changed -notcontains $path) {
    throw "Reviewed exception is not in changed-file list: $path"
  }
  [void]$accepted.Add($path)
}

$acceptedList = @($accepted) | Sort-Object
$rejectedList = @($changed | Where-Object { $accepted -notcontains $_ }) | Sort-Object

$allReviewed = @($acceptedList + $rejectedList)
$duplicates = $allReviewed | Group-Object | Where-Object { $_.Count -gt 1 }
if ($duplicates.Count -ne 0) {
  throw "Duplicate reviewed path detected: $($duplicates[0].Name)"
}

$missing = $changed | Where-Object { $allReviewed -notcontains $_ }
if ($missing.Count -ne 0) {
  throw "Missing changed path from reviewed lists: $($missing[0])"
}

$extra = $allReviewed | Where-Object { $changed -notcontains $_ }
if ($extra.Count -ne 0) {
  throw "Reviewed path not present in changed list: $($extra[0])"
}

@(
  "# Reviewed accepted paths."
  "# Strategy: accept direct CU-CP files, CU-CP docs/harness files, and narrow control-plane/shared-NTN exceptions needed by accepted CU-CP code."
  "# One repository-relative path per line. Comments and blank lines are ignored by the import script."
  ""
  $acceptedList
) | Set-Content -Path $AcceptedPath -Encoding UTF8

@(
  "# Reviewed rejected or quarantined paths."
  "# Strategy: quarantine DU/DU-side F1AP, broad shared NTN components not needed by CU-CP, unrelated GIS/site collateral, and non-NTN tooling."
  "# One repository-relative path per line. Comments and blank lines are ignored by the import script."
  ""
  $rejectedList
) | Set-Content -Path $RejectedPath -Encoding UTF8

$notes = New-Object System.Collections.Generic.List[string]
$notes.Add("# Path Review Notes")
$notes.Add("")
$notes.Add("Reviewed source lists:")
$notes.Add("")
$notes.Add("- ``ai_harness/audit/accepted_paths.candidate.txt``")
$notes.Add("- ``ai_harness/audit/rejected_or_quarantined_paths.candidate.txt``")
$notes.Add("- ``ai_harness/audit/goal_mode_import_report.md``")
$notes.Add("")
$notes.Add("## Result")
$notes.Add("")
$notes.Add("- Accepted paths: $($acceptedList.Count)")
$notes.Add("- Rejected/quarantined paths: $($rejectedList.Count)")
$notes.Add("- Total reviewed paths: $($allReviewed.Count)")
$notes.Add("")
$notes.Add("## Review Findings")
$notes.Add("")
$notes.Add("- The candidate accepted list did not contain obvious DU/MAC/PHY/RU/RF/GIS leaks. ``tests/unittests/cu_cp/du_processor/du_processor_test_helpers.cpp`` is kept because it is under CU-CP tests despite the directory name containing ``du_processor``.")
$notes.Add("- The candidate rejected list was too conservative for a buildable CU-CP starting point. Accepted CU-CP files reference F1AP NTN slot-resource request types, NGAP location-reporting types, RRC UE hooks, and the orbit propagator used by the CU-CP satellite-state updater.")
$notes.Add("- I moved narrow F1-C, NGAP, RRC, o_cu_cp config/test, config example, beam-table generator, and minimal shared NTN orbit/beam-table dependency files into ``accepted_paths.txt``.")
$notes.Add("- I kept DU manager, DU-side F1AP, ``flexible_o_du``, broad NTN configuration-manager/controller changes, broad NTN tests, unrelated GIS site files, GIS planning notes, and presentation tooling in quarantine.")
$notes.Add("")
$notes.Add("## Accepted Non-CU-CP Exceptions")
$notes.Add("")
foreach ($path in $reviewedExceptions | Sort-Object) {
  $notes.Add("- ``$path``")
}
$notes.Add("")
$notes.Add("## Remaining Quarantine Themes")
$notes.Add("")
$notes.Add("- DU scheduler/resource-manager SR/SRS realization remains quarantined; it can be reviewed later as an explicit non-CU-CP exception if end-to-end slot realization is required.")
$notes.Add("- Shared ``include/srsran/ntn`` / ``lib/ntn`` controller and configuration-manager changes remain quarantined except for the orbit/beam-table files required by CU-CP satellite-state updates.")
$notes.Add("- GIS remote-service site files are unrelated and should stay out of the CU-CP branch.")

$notes | Set-Content -Path $ReviewNotesPath -Encoding UTF8

Write-Host "Reviewed accepted paths: $($acceptedList.Count)"
Write-Host "Reviewed rejected/quarantined paths: $($rejectedList.Count)"
Write-Host "Total reviewed paths: $($allReviewed.Count)"
