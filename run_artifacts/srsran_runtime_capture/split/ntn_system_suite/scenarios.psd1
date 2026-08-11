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

  ntn_initial_ul_rx_port_sim = @{
    Type        = "ctest_filter"
    Purpose     = "Code-level simulated multi-port PRACH attribution, C-RNTI generation, DU observation storage, private F1 query, and CU-CP admission"
    BuildTarget = @(
      "prach_detector_port_attribution_test",
      "phy_to_fapi_results_event_fastpath_translator_test",
      "fapi_to_mac_data_msg_fastpath_translator_test",
      "mac_test",
      "ue_manager_test",
      "f1ap_cu_test",
      "f1ap_du_test",
      "cu_cp_test"
    )
    RegexGroups = @(
      "(prach_detector_port_attribution_test\.(when_single_port_is_enabled_then_only_port_is_unique|when_two_ports_have_clear_margin_then_strongest_port_is_unique|when_two_ports_have_equal_power_then_attribution_is_ambiguous|when_margin_is_exactly_six_db_then_attribution_is_unique|when_attribution_is_disabled_then_legacy_result_is_unchanged)|phy_to_fapi_results_event_fastpath_translator_test\.prach_handle_and_physical_port_attribution_are_preserved|mac_rach_indication_fixture\.(CorrectMessageConvertsCorrectly|DefaultPortAttributionRemainsUnavailable|AmbiguousPortAttributionIsPreserved))",
      "(mac_ntn_initial_ul_position_manager_test\.(no_ntn_calendar_keeps_terrestrial_prach_untracked|unique_sdr_port_is_verified_and_msg3_consumes_record_once|software_only_is_recorded_but_ambiguous_port_fails_closed|physical_receive_port_disambiguates_parallel_calendar_positions)|rnti_manager_test\.(allocate_for_cell_with_generation_returns_the_authoritative_ntn_lease_generation|allocate_for_cell_with_generation_reports_a_newer_generation_after_safe_rnti_reuse|allocate_for_cell_with_generation_fails_closed_when_generation_is_exhausted))",
      "(du_ntn_initial_ul_position_store_test\.(exact_query_consumes_once_and_same_nonce_is_idempotent|mismatched_generation_does_not_consume_and_entry_expires_at_one_second)|du_ue_manager_tester\.successful_ue_creation_preserves_initial_ul_position_under_actual_f1_identity)",
      "(f1ap_ntn_initial_ul_position_container_test\.(valid_query_round_trips_the_complete_target_identity|valid_result_round_trips_target_and_receive_identity)|f1ap_cu_test\.(when_initial_ul_position_query_is_enabled_then_rrc_creation_waits_for_exact_response|when_initial_ul_position_query_times_out_then_rrc_creation_resumes_after_timeout)|f1ap_cu_gnbdu_resource_coordination_test\.(initial_ul_position_query_has_container_priority|exact_initial_ul_position_result_completes_procedure|any_initial_ul_query_identity_mismatch_is_rejected))",
      "^f1ap_du_test$",
      "cu_cp_ntn_mobility_test\.onboard_initial_ul_position_(strict_private_f1_device_result_allows_setup|audit_private_f1_timeout_continues_setup|strict_private_f1_timeout_rejects_before_ownership|strict_software_result_rejects_before_ownership|audit_accepts_software_result_without_physical_port|strict_rnti_generation_rejection_blocks_ownership)"
    )
    Required    = $true
  }

  ntn_initial_ul_ofh_beam_sim = @{
    Type        = "ctest_filter"
    Purpose     = "Code-level simulated OFH type-3 BeamId and PRACH eAxC provenance through FAPI, MAC, private F1, and CU-CP admission"
    BuildTarget = @(
      "ofh_cplane_packet_builder_impl_test",
      "ofh_uplink_request_handler_impl_test",
      "ofh_data_flow_cplane_scheduling_commands_test",
      "ofh_data_flow_uplane_uplink_prach_impl_test",
      "ofh_uplane_prach_data_flow_notifier_test",
      "ofh_uplane_prach_symbol_data_flow_writer_test",
      "upper_phy_rx_symbol_handler_test",
      "prach_detector_port_attribution_test",
      "phy_to_fapi_results_event_fastpath_translator_test",
      "fapi_to_mac_data_msg_fastpath_translator_test",
      "mac_test",
      "f1ap_cu_test",
      "cu_cp_test"
    )
    RegexGroups = @(
      "(ofh_control_plane_packet_builder_impl_test\.prach_type_3_serializes_the_full_15_bit_beam_id|ofh_uplink_request_handler_impl_fixture\.(valid_beam_context_is_attached_to_the_matching_prach_eaxc|missing_beam_context_keeps_the_unverified_prach_path|duplicate_eaxc_beam_mapping_fails_closed)|ofh_uplane_prach_symbol_data_flow_writer_fixture\.(no_eaxc_found_does_not_write|decoded_prbs_in_one_packet_passes))",
      "data_flow_cplane_scheduling_commands_impl_fixture\.(type_3_beam_and_calendar_context_is_serialized_and_retained|conflicting_type_3_mapping_is_not_available_to_receiver)",
      "(data_flow_uplane_uplink_prach_impl_fixture\.(matched_eaxc_emits_verified_beam_and_calendar_context|invalid_beam_context_is_not_accepted_as_verified)|ofh_uplane_prach_data_flow_notifier\.(completed_buffer_forwards_verified_eaxc_context|conflicting_eaxc_context_is_not_reported_as_verified)|UpperPhyRxSymbolHandlerFixture\.(ru_adapter_attaches_complete_verified_prach_receive_context|ru_adapter_does_not_attach_duplicate_verified_buffer_port))",
      "prach_detector_port_attribution_test\.(when_two_ports_have_clear_margin_then_strongest_port_is_unique|when_two_ports_have_equal_power_then_attribution_is_ambiguous)",
      "(phy_to_fapi_results_event_fastpath_translator_test\.(prach_handle_and_physical_port_attribution_are_preserved|duplicate_verified_buffer_port_is_not_forwarded)|mac_rach_indication_fixture\.(VerifiedOfhContextIsPreservedForUniquePort|VerifiedOfhContextIsNotPreservedForAmbiguousPort|VerifiedOfhContextLifetimeExtendsPastFapiMessage))",
      "mac_ntn_initial_ul_position_manager_test\.(verified_ofh_context_keeps_buffer_index_separate_from_physical_port|stale_ofh_mapping_identity_or_calendar_hash_is_rejected_with_explicit_mapping_reason|missing_ofh_beam_context_reports_capability_unavailable|ofh_provider_exposes_each_active_eaxc_with_its_own_beam_context)",
      "(f1ap_ntn_initial_ul_position_container_test\.valid_result_round_trips_target_and_receive_identity|f1ap_cu_test\.when_initial_ul_position_query_is_enabled_then_rrc_creation_waits_for_exact_response)",
      "cu_cp_ntn_mobility_test\.(onboard_initial_ul_position_ue_removal_clears_temporary_context|onboard_initial_ul_position_strict_software_result_rejects_before_ownership)"
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
