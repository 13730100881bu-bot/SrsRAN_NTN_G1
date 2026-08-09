@{
  baseline_attach_ping = @{
    Type     = "live_script"
    Purpose  = "Open5GS + srsUE + split CU-CP/CU-UP/DU attach, PDU session, and GTP-U ping"
    Command  = "run_split_demo.ps1"
    Evidence = @("ngap", "e1ap", "f1ap", "nr-rrc", "gtp", "icmp")
    Required = $true
  }

  ntn_runtime_sib19_visibility = @{
    Type     = "live_script"
    Purpose  = "NTN runtime, SIB19 CU-CP to DU apply, F1AP coordination pcap, and DU MAC SI-RNTI visibility"
    Command  = "run_split_ntn_sib19_ue_visibility_proof.ps1"
    Evidence = @("ntn_state", "ntn_beams", "sib19_applied", "f1ap_coordination", "mac_si_rnti")
    Required = $true
  }

  ntn_connected_mobility_sim = @{
    Type        = "ctest_filter"
    Purpose     = "Simulated AMF/DU/UE coverage for NTN handover, beam hopping, predictive mobility, load balancing, and service-pair lifecycle"
    BuildTarget = "cu_cp_test"
    Regex       = "cu_cp_ntn_mobility_test.*(handover|beam_hopping|predictive|load_balancing|service_pair|paired_access)"
    Required    = $true
  }

  ntn_paging_sim = @{
    Type        = "ctest_filter"
    Purpose     = "Simulated paging coverage for NTN beam-derived TAC, idle/inactive context, direction-aware access, and paired access"
    BuildTarget = "cu_cp_test"
    Regex       = "cu_cp.*paging|cu_cp_ntn_mobility_test.*(paging|inactive|paired_access)"
    Required    = $true
  }

  ntn_resource_repair_sim = @{
    Type        = "ctest_filter"
    Purpose     = "Simulated RNTI, SR/SRS, service-pair uplink resource audit, and repair coverage"
    BuildTarget = "ntn_mobility_test"
    Regex       = "ntn_beam_service_resource_manager.*service_pair|cu_cp_ntn_mobility_test.*(resource|repair|audit|slot)"
    Required    = $true
  }

  ntn_rnti_retirement_sim = @{
    Type        = "ctest_filter"
    Purpose     = "Simulated NTN C-RNTI allocation, expiry, atomic retirement, higher-generation reuse, and DU reconnect recovery"
    BuildTarget = @("ntn_mobility_test", "cu_cp_test", "mac_test")
    RegexGroups = @(
      "ntn_beam_service_resource_manager.*(rnti.*retire|retirement|du_disconnect|du_ledger_reset|never_sent_orphan)",
      "cu_cp_ntn_mobility_test.*(rnti_retirement|rnti_reconnect)_",
      "rnti_manager_test.*(retire|generation|terrestrial)"
    )
    Required    = $true
  }

  ntn_ue_slot_recovery_sim = @{
    Type        = "ctest_filter"
    Purpose     = "Simulated SR/SRS assignment, DU-applied snapshot, disconnect, current-connection reconciliation, bounded repair, and final clear"
    BuildTarget = @("ntn_mobility_test", "cu_cp_test", "du_manager_procedure_test")
    RegexGroups = @(
      "ntn_beam_service_resource_manager.*(ue_slot_recovery_sim|complete_empty_snapshot|exact_generation_snapshot|complete_snapshot_recovers_du_adjusted|du_only_current_ue_slot|du_disconnect_preserves_desired_slot)",
      "cu_cp_ntn_mobility_test\.ue_slot_(audit|repair)_.*",
      "du_manager_ntn_rnti_lease_test\.(authoritative_audit|incomplete_rnti_domain)_.*"
    )
    Required    = $true
  }

  ntn_nrppa_sim = @{
    Type        = "ctest_filter"
    Purpose     = "Simulated NRPPa transport, TRP information, positioning information, measurement, activation, and assistance-control coverage"
    BuildTarget = "cu_cp_test"
    Regex       = "nrppa.*|cu_cp.*nrppa|f1ap_cu.*positioning"
    Required    = $false
  }

  ntn_cli_observability_sim = @{
    Type        = "ctest_filter"
    Purpose     = "CLI coverage for ntn_state, ntn_beams, ntn_ues, ntn_diagnose, and ntn_repair"
    BuildTarget = "cu_cp_unit_config_test"
    Regex       = "cu_cp_unit_config.*ntn_(state|beams|ues|diagnose|repair)"
    Required    = $true
  }
}
