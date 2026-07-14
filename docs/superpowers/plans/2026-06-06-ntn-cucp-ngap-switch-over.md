# NTN CU-CP NGAP And Switch-over Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 CU-CP 范围内完成 `CUCP-010` NGAP/core mapping 收口，并实现 `CUCP-011` NTN service switch-over/resilience 控制面能力。

**Architecture:** `CUCP-010` 先用 characterization tests 固化当前 NGAP NTN 位置上报行为，再补齐 cancel/ref-id 等缺口；不把不存在的 Mapped Cell IE 硬塞进 `UserLocationInformationNR`。`CUCP-011` 新增一个小型 CU-CP NTN service switch-over 控制器，`cu_cp_impl` 只负责命令接入、beam/NCI 反查、admission gate 和 placement refresh。

**Tech Stack:** C++17, srsRAN CU-CP, NGAP ASN.1 converter, gtest/ctest, harness path guard.

---

## 执行边界

本计划只允许使用以下路径：

- `lib/cu_cp/`
- `include/srsran/cu_cp/`
- `tests/unittests/cu_cp/`
- `tests/integrationtests/cu_cp/`

`CUCP-010` 额外允许这些 exact NGAP paths：

- `include/srsran/ngap/ngap.h`
- `include/srsran/ngap/ngap_location_reporting.h`
- `lib/ngap/ngap_asn1_converters.h`
- `lib/ngap/ngap_impl.cpp`
- `lib/ngap/ngap_impl.h`
- `tests/unittests/ngap/ngap_ue_context_management_procedure_test.cpp`
- `tests/unittests/ngap/test_helpers.h`

禁止修改：

- O-DU / `flexible_o_du`
- DU-high / MAC scheduler
- PHY / lower PHY
- RU / RF / ZMQ
- PRACH / HARQ timing / TA scheduler
- GIS site code

本阶段不实现真实 SIB19 广播调度、不实现 DU/MAC 侧 SR/SRS 资源落地、不实现物理 feeder/gateway 切换。

## 文件结构

### CUCP-010

- Modify: `tests/unittests/ngap/ngap_ue_context_management_procedure_test.cpp`
  - 增加 NGAP converter characterization test，证明 `UserLocationInformationNR` 携带 NR CGI、TAI、timestamp、`NRNTNTAIInformation` 和 derived TAC。
- Modify: `lib/ngap/ngap_asn1_converters.h`
  - 仅当 converter characterization test 失败时修正字段填充。
- Modify: `tests/unittests/cu_cp/cu_cp_ntn_mobility_test.cpp`
  - 加强 `LocationReportingControl` 的 area-of-interest、cancel、duplicate ref-id、draining、NTN TAI extension 覆盖。
- Modify: `lib/cu_cp/cu_cp_impl.cpp`
  - 仅补齐 cancel ref-id 对 active area-of-interest request 的精确删除语义。

### CUCP-011

- Create: `include/srsran/cu_cp/ntn_service_switch_over.h`
  - 公共 CU-CP switch-over event、manual override、source priority、snapshot 类型。
- Create: `lib/cu_cp/ntn_mobility/ntn_service_switch_over_controller.h`
  - 纯 CU-CP policy controller 接口。
- Create: `lib/cu_cp/ntn_mobility/ntn_service_switch_over_controller.cpp`
  - soft/hard event、clear event、manual freeze/restore/replace、beam policy 计算。
- Modify: `lib/cu_cp/ntn_mobility/CMakeLists.txt`
  - 把 controller 加入 `srsran_cu_cp_ntn_mobility`。
- Create: `tests/unittests/cu_cp/ntn_mobility/ntn_service_switch_over_controller_test.cpp`
  - 纯策略单元测试。
- Modify: `tests/unittests/cu_cp/ntn_mobility/CMakeLists.txt`
  - 把新 test 文件加入 `ntn_mobility_test`。
- Modify: `include/srsran/cu_cp/cu_cp_command_handler.h`
  - 在 `cu_cp_ntn_command_handler` 增加 switch-over command 和 snapshot 查询。
- Modify: `lib/cu_cp/cu_cp_impl.h`
  - 增加 controller 成员和 helper 声明。
- Modify: `lib/cu_cp/cu_cp_impl.cpp`
  - 接入 switch-over event、manual override、source freeze、admission gate、placement refresh。
- Modify: `tests/unittests/cu_cp/cu_cp_ntn_mobility_test.cpp`
  - CU-CP integration tests：hard drain、新 access/PDU demand gate、manual freeze/restore、soft 不释放已有 UE。

## 当前本地结论

- `cu_cp_user_location_info_nr` 当前已有 `nr_cgi`、`tai`、`time_stamp`、`ntn_derived_tac`。
- `cu_cp_user_location_info_to_asn1()` 当前会填充 `UserLocationInformationNR` 的 `timeStamp` 和 `NRNTNTAIInformation` 扩展。
- 本地 NGAP ASN.1 的 `UserLocationInformationNR` 未暴露名为 Mapped Cell 的字段；本阶段不新增错误字段。`Mapped Cell / derived TAC / TAI` 中可落地的部分是 TAI、derived TAC 和 NRNTNTAIInformation。
- `cu_cp_impl` 当前已有 `handle_location_reporting_control()`、`report_ntn_location_to_core_if_required()`、`build_ntn_core_user_location_info()` 和 stale assistance gate。
- 当前 cancel ref-id 逻辑会删除任何包含该 ref-id 的 active request；本计划把它收紧为只删除被 cancel 的 ref-id，保留同一 request 中未被 cancel 的 ref-id。

---

### Task 1: CUCP-010 NGAP Converter Characterization

**Files:**
- Modify: `tests/unittests/ngap/ngap_ue_context_management_procedure_test.cpp`
- Modify only if test fails: `lib/ngap/ngap_asn1_converters.h`

- [ ] **Step 1: 增加 converter include**

在 `tests/unittests/ngap/ngap_ue_context_management_procedure_test.cpp` 的 include 区加入：

```cpp
#include "lib/ngap/ngap_asn1_converters.h"
```

- [ ] **Step 2: 写 characterization test**

在 `ngap_ue_context_management_procedure_test.cpp` 文件末尾加入：

```cpp
TEST(ngap_ntn_user_location_converter_test, packs_nr_ntn_tai_info_timestamp_and_derived_tac)
{
  cu_cp_user_location_info_nr location;
  location.nr_cgi.plmn_id = plmn_identity::test_value();
  location.nr_cgi.nci     = nr_cell_identity::create(gnb_id_t{411, 22}, 3).value();
  location.tai.plmn_id    = plmn_identity::test_value();
  location.tai.tac        = 0x000abc;
  location.time_stamp     = 0x01020304;
  location.ntn_derived_tac = 0x000def;

  asn1::ngap::user_location_info_nr_s asn1_location = cu_cp_user_location_info_to_asn1(location);

  EXPECT_EQ(asn1_location.nr_cgi.nr_cell_id.to_number(), location.nr_cgi.nci.value());
  EXPECT_EQ(asn1_location.tai.tac.to_number(), location.tai.tac);
  ASSERT_TRUE(asn1_location.time_stamp_present);
  EXPECT_EQ(asn1_location.time_stamp.to_number(), location.time_stamp.value());

  ASSERT_TRUE(asn1_location.ie_exts_present);
  ASSERT_TRUE(asn1_location.ie_exts.nr_ntn_tai_info_present);

  const auto& ntn_tai_info = asn1_location.ie_exts.nr_ntn_tai_info;
  ASSERT_EQ(ntn_tai_info.tac_list_in_nr_ntn.size(), 1);
  EXPECT_EQ(ntn_tai_info.tac_list_in_nr_ntn.front().to_number(), location.tai.tac);
  ASSERT_TRUE(ntn_tai_info.ue_location_derived_tac_in_nr_ntn_present);
  EXPECT_EQ(ntn_tai_info.ue_location_derived_tac_in_nr_ntn.to_number(), location.ntn_derived_tac.value());
}
```

- [ ] **Step 3: 运行 NGAP focused test**

Run:

```powershell
ctest --test-dir build/ai-clean -R "ngap_.*packs_nr_ntn_tai_info_timestamp_and_derived_tac" --output-on-failure
```

Expected:

```text
100% tests passed
```

如果 test discovery 名称和 regex 不匹配，运行：

```powershell
ctest --test-dir build/ai-clean -R "ngap" --output-on-failure
```

Expected:

```text
ngap tests pass, including packs_nr_ntn_tai_info_timestamp_and_derived_tac
```

- [ ] **Step 4: 只在失败时修 converter**

如果 Step 3 失败，并且失败点是 `time_stamp_present` 或 `nr_ntn_tai_info_present`，在 `lib/ngap/ngap_asn1_converters.h` 的 `cu_cp_user_location_info_to_asn1()` 保持如下逻辑：

```cpp
  if (cu_cp_user_location_info.time_stamp.has_value()) {
    asn1_user_location_info.time_stamp_present = true;
    asn1_user_location_info.time_stamp.from_number(cu_cp_user_location_info.time_stamp.value());
  }
  if (cu_cp_user_location_info.ntn_derived_tac.has_value()) {
    asn1_user_location_info.ie_exts_present                 = true;
    asn1_user_location_info.ie_exts.nr_ntn_tai_info_present = true;

    auto& ntn_tai_info        = asn1_user_location_info.ie_exts.nr_ntn_tai_info;
    ntn_tai_info.serving_plmn = cu_cp_user_location_info.tai.plmn_id.to_bytes();

    asn1::fixed_octstring<3, true> tac;
    tac.from_number(cu_cp_user_location_info.tai.tac);
    ntn_tai_info.tac_list_in_nr_ntn.push_back(tac);

    ntn_tai_info.ue_location_derived_tac_in_nr_ntn_present = true;
    ntn_tai_info.ue_location_derived_tac_in_nr_ntn.from_number(cu_cp_user_location_info.ntn_derived_tac.value());
  }
```

- [ ] **Step 5: 记录 Mapped Cell 边界**

在本任务最终报告写明：

```text
本地 UserLocationInformationNR ASN.1 当前没有 Mapped Cell IE；CUCP-010 只固化 NR CGI、TAI、timestamp、NRNTNTAIInformation 和 derived TAC。没有修改 ASN.1 生成代码。
```

- [ ] **Step 6: Commit**

仅在执行者确认当前环境是隔离任务分支时提交：

```bash
git add tests/unittests/ngap/ngap_ue_context_management_procedure_test.cpp lib/ngap/ngap_asn1_converters.h
git commit -m "test(cucp-010): characterize NTN NGAP user location conversion"
```

当前仓库如果仍是共享 dirty worktree，则跳过 commit，并在最终报告列出这一步未提交。

---

### Task 2: CUCP-010 Area-of-interest Cancel Semantics

**Files:**
- Modify: `tests/unittests/cu_cp/cu_cp_ntn_mobility_test.cpp`
- Modify: `lib/cu_cp/cu_cp_impl.cpp`

- [ ] **Step 1: 写 failing test，证明 cancel ref-id 只取消被点名的 ref**

在 `tests/unittests/cu_cp/cu_cp_ntn_mobility_test.cpp` 的 `location_reporting_control_rejects_overlapping_area_of_interest_ref_id` 后加入：

```cpp
TEST(cu_cp_ntn_mobility_test, location_reporting_control_cancel_ref_id_preserves_other_area_refs)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));

  env.run_ng_setup();
  const std::optional<connected_ngap_ntn_ue> ue = connect_ngap_ntn_ue(env);
  ASSERT_TRUE(ue.has_value());
  ASSERT_TRUE(env.get_cu_cp()
                  .get_command_handler()
                  .get_ntn_command_handler()
                  .handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  auto* cu_cp_impl = get_cu_cp_impl(env);
  ASSERT_NE(cu_cp_impl, nullptr);

  ngap_location_reporting_control control;
  control.ue_index                              = ue->ue_index;
  control.request_type.event_type               = ngap_location_reporting_event_type::ue_presence_in_area_of_interest;
  control.request_type.area_of_interest_ref_ids = {7, 8};
  ASSERT_TRUE(cu_cp_impl->get_cu_cp_ngap_handler().handle_location_reporting_control(control).accepted);

  const auto base_time = std::chrono::steady_clock::now();
  cu_cp_impl->get_cu_cp_measurement_handler().handle_ue_location_report(
      make_ntn_location_report(ue->ue_index, make_default_env_nci(0), base_time));

  ngap_message ngap_pdu;
  ASSERT_TRUE(env.wait_for_ngap_tx_pdu(ngap_pdu));
  ASSERT_TRUE(is_pdu_type(ngap_pdu,
                          asn1::ngap::ngap_elem_procs_o::init_msg_c::types::types_opts::location_report));
  ASSERT_EQ(ngap_pdu.pdu.init_msg().value.location_report()->ue_presence_in_area_of_interest_list.size(), 2);

  ngap_location_reporting_control cancel;
  cancel.ue_index                                                    = ue->ue_index;
  cancel.request_type.event_type = ngap_location_reporting_event_type::cancel_location_report_for_the_ue;
  cancel.request_type.location_report_ref_id_to_be_cancelled         = 7;
  ASSERT_TRUE(cu_cp_impl->get_cu_cp_ngap_handler().handle_location_reporting_control(cancel).accepted);

  cu_cp_impl->get_cu_cp_measurement_handler().handle_ue_location_report(
      make_ntn_location_report(ue->ue_index, make_default_env_nci(0), base_time + std::chrono::milliseconds{1}));

  ASSERT_TRUE(env.wait_for_ngap_tx_pdu(ngap_pdu));
  const auto& report = ngap_pdu.pdu.init_msg().value.location_report();
  ASSERT_EQ(report->ue_presence_in_area_of_interest_list.size(), 1);
  EXPECT_EQ(report->ue_presence_in_area_of_interest_list.front().location_report_ref_id, 8);
}
```

- [ ] **Step 2: 运行 test，确认当前行为失败**

Run:

```powershell
ctest --test-dir build/ai-clean -R "location_reporting_control_cancel_ref_id_preserves_other_area_refs" --output-on-failure
```

Expected before implementation:

```text
FAIL: expected one remaining area-of-interest ref-id after cancel
```

- [ ] **Step 3: 修改 cancel ref-id 处理**

在 `lib/cu_cp/cu_cp_impl.cpp` 的 `cancel_location_report_for_the_ue` 分支中，把当前“包含 ref-id 就删除整个 request”的逻辑替换为：

```cpp
      auto state_it = ntn_core_location_reporting_states.find(request.ue_index);
      if (state_it != ntn_core_location_reporting_states.end()) {
        auto&        active_requests = state_it->second.active_requests;
        const uint8_t ref_id         = request_type.location_report_ref_id_to_be_cancelled.value();
        for (auto& active_request : active_requests) {
          if (active_request.event_type != ngap_location_reporting_event_type::ue_presence_in_area_of_interest) {
            continue;
          }
          active_request.area_of_interest_ref_ids.erase(
              std::remove(active_request.area_of_interest_ref_ids.begin(),
                          active_request.area_of_interest_ref_ids.end(),
                          ref_id),
              active_request.area_of_interest_ref_ids.end());
        }
        active_requests.erase(std::remove_if(active_requests.begin(),
                                             active_requests.end(),
                                             [](const ngap_location_reporting_request_type& active_request) {
                                               return active_request.event_type ==
                                                          ngap_location_reporting_event_type::ue_presence_in_area_of_interest &&
                                                      active_request.area_of_interest_ref_ids.empty();
                                             }),
                              active_requests.end());
      }
```

- [ ] **Step 4: 运行 focused CU-CP test**

Run:

```powershell
ctest --test-dir build/ai-clean -R "location_reporting_control_cancel_ref_id_preserves_other_area_refs" --output-on-failure
```

Expected:

```text
100% tests passed
```

- [ ] **Step 5: Commit**

仅在隔离任务分支提交：

```bash
git add tests/unittests/cu_cp/cu_cp_ntn_mobility_test.cpp lib/cu_cp/cu_cp_impl.cpp
git commit -m "fix(cucp-010): preserve NTN area reporting refs on cancel"
```

---

### Task 3: CUCP-010 Strengthen NTN Location Report Assertions

**Files:**
- Modify: `tests/unittests/cu_cp/cu_cp_ntn_mobility_test.cpp`

- [ ] **Step 1: 强化本地转发 LocationReport 断言**

在 `accepted_ntn_location_report_is_forwarded_to_ngap_location_report` 中，`const auto& nr_info = ...` 后增加：

```cpp
  ASSERT_EQ(nr_info.nr_cgi.nr_cell_id.to_number(), make_default_env_nci(0).value());
  ASSERT_TRUE(nr_info.time_stamp_present);
  EXPECT_NE(nr_info.time_stamp.to_number(), 0U);

  const auto& ntn_tai_info = nr_info.ie_exts.nr_ntn_tai_info;
  ASSERT_EQ(ntn_tai_info.tac_list_in_nr_ntn.size(), 1);
  ASSERT_TRUE(ntn_tai_info.ue_location_derived_tac_in_nr_ntn_present);
```

- [ ] **Step 2: 写 duplicate-in-same-request test**

在 area-of-interest tests 附近加入：

```cpp
TEST(cu_cp_ntn_mobility_test, location_reporting_control_rejects_duplicate_area_ref_ids_in_same_request)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));

  env.run_ng_setup();
  const std::optional<connected_ngap_ntn_ue> ue = connect_ngap_ntn_ue(env);
  ASSERT_TRUE(ue.has_value());

  auto* cu_cp_impl = get_cu_cp_impl(env);
  ASSERT_NE(cu_cp_impl, nullptr);

  ngap_location_reporting_control control;
  control.ue_index                              = ue->ue_index;
  control.request_type.event_type               = ngap_location_reporting_event_type::ue_presence_in_area_of_interest;
  control.request_type.area_of_interest_ref_ids = {9, 9};

  const ngap_location_reporting_control_response response =
      cu_cp_impl->get_cu_cp_ngap_handler().handle_location_reporting_control(control);
  ASSERT_FALSE(response.accepted);
  assert_ngap_radio_cause(response.cause, ngap_cause_radio_network_t::multiple_location_report_ref_id_instances);
}
```

- [ ] **Step 3: 运行 CUCP-010 focused tests**

Run:

```powershell
ctest --test-dir build/ai-clean -R "cu_cp_ntn_mobility_test.*location_reporting_control|cu_cp_ntn_mobility_test.*accepted_ntn_location_report_is_forwarded" --output-on-failure
```

Expected:

```text
100% tests passed
```

- [ ] **Step 4: Commit**

仅在隔离任务分支提交：

```bash
git add tests/unittests/cu_cp/cu_cp_ntn_mobility_test.cpp
git commit -m "test(cucp-010): harden NTN core location reporting coverage"
```

---

### Task 4: CUCP-011 Add Switch-over Public Types

**Files:**
- Create: `include/srsran/cu_cp/ntn_service_switch_over.h`

- [ ] **Step 1: 创建 public header**

Create `include/srsran/cu_cp/ntn_service_switch_over.h`:

```cpp
/*
 *
 * Copyright 2021-2026 Software Radio Systems Limited
 *
 * This file is part of srsRAN.
 *
 * srsRAN is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * srsRAN is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#pragma once

#include "srsran/ran/nr_cgi.h"
#include "srsran/ran/ntn.h"
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace srsran {
namespace srs_cu_cp {

enum class ntn_service_switch_over_type { soft, hard };

enum class ntn_service_switch_over_source { manual_override, tle, circular_fallback, operator_command };

enum class ntn_service_switch_over_policy { prepare, drain, handover_preferred, release_allowed };

enum class ntn_service_beam_policy { normal, prepare, block_new_demand, drain, release_allowed };

enum class ntn_manual_override_mode { none, freeze_current_state, replace_service_state, restore_automatic_source };

struct ntn_service_switch_over_event {
  uint64_t                         event_id = 0;
  ntn_service_switch_over_type     type     = ntn_service_switch_over_type::soft;
  ntn_service_switch_over_source   source   = ntn_service_switch_over_source::operator_command;
  ntn_service_switch_over_policy   policy   = ntn_service_switch_over_policy::prepare;
  std::vector<std::string>         affected_beam_ids;
  std::vector<nr_cell_identity>    affected_ncis;
  std::chrono::steady_clock::time_point start_time = std::chrono::steady_clock::now();
  std::optional<std::chrono::steady_clock::time_point> expected_end_time;
  std::string                      reason;
};

struct ntn_manual_override_command {
  ntn_manual_override_mode         mode = ntn_manual_override_mode::none;
  std::optional<ecef_coordinates_t> replacement_satellite_ecef;
  std::vector<std::string>         replacement_served_beam_ids;
  std::string                      reason;
};

struct ntn_service_switch_over_beam_snapshot {
  std::string             beam_id;
  nr_cell_identity        nci = nr_cell_identity::min();
  ntn_service_beam_policy policy = ntn_service_beam_policy::normal;
};

struct ntn_service_switch_over_snapshot {
  ntn_manual_override_mode                     manual_override_mode = ntn_manual_override_mode::none;
  bool                                         automatic_source_updates_allowed = true;
  std::vector<ntn_service_switch_over_event>   active_events;
  std::vector<ntn_service_switch_over_beam_snapshot> beams;
};

} // namespace srs_cu_cp
} // namespace srsran
```

- [ ] **Step 2: 运行 include-only build check**

Run:

```powershell
cmake --build build/ai-clean --target ntn_mobility_test -j2
```

Expected:

```text
ntn_mobility_test builds
```

- [ ] **Step 3: Commit**

仅在隔离任务分支提交：

```bash
git add include/srsran/cu_cp/ntn_service_switch_over.h
git commit -m "feat(cucp-011): add NTN service switch-over contracts"
```

---

### Task 5: CUCP-011 Pure Switch-over Controller

**Files:**
- Create: `lib/cu_cp/ntn_mobility/ntn_service_switch_over_controller.h`
- Create: `lib/cu_cp/ntn_mobility/ntn_service_switch_over_controller.cpp`
- Modify: `lib/cu_cp/ntn_mobility/CMakeLists.txt`
- Create: `tests/unittests/cu_cp/ntn_mobility/ntn_service_switch_over_controller_test.cpp`
- Modify: `tests/unittests/cu_cp/ntn_mobility/CMakeLists.txt`

- [ ] **Step 1: 写 controller test**

Create `tests/unittests/cu_cp/ntn_mobility/ntn_service_switch_over_controller_test.cpp`:

```cpp
/*
 *
 * Copyright 2021-2026 Software Radio Systems Limited
 *
 * This file is part of srsRAN.
 *
 * srsRAN is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * srsRAN is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#include "lib/cu_cp/ntn_mobility/ntn_service_switch_over_controller.h"
#include <gtest/gtest.h>

using namespace srsran;
using namespace srs_cu_cp;

namespace {

nr_cell_identity make_nci(unsigned sector_id)
{
  return nr_cell_identity::create(gnb_id_t{411, 22}, sector_id).value();
}

ntn_service_switch_over_event make_event(uint64_t id, ntn_service_switch_over_type type, std::string beam_id)
{
  ntn_service_switch_over_event event;
  event.event_id = id;
  event.type     = type;
  event.source   = ntn_service_switch_over_source::operator_command;
  event.policy   = type == ntn_service_switch_over_type::soft ? ntn_service_switch_over_policy::prepare
                                                              : ntn_service_switch_over_policy::drain;
  event.affected_beam_ids.push_back(std::move(beam_id));
  event.start_time = std::chrono::steady_clock::now();
  event.reason     = "unit-test";
  return event;
}

} // namespace

TEST(ntn_service_switch_over_controller_test, soft_event_marks_beam_for_preparation)
{
  ntn_service_switch_over_controller controller;
  ASSERT_TRUE(controller.apply_event(make_event(1, ntn_service_switch_over_type::soft, "CN-BEAM-0001")));

  ntn_service_switch_over_snapshot snapshot =
      controller.build_snapshot({{"CN-BEAM-0001", make_nci(0)}, {"CN-BEAM-0002", make_nci(1)}});

  ASSERT_EQ(snapshot.active_events.size(), 1);
  ASSERT_EQ(snapshot.beams.size(), 2);
  EXPECT_EQ(snapshot.beams[0].policy, ntn_service_beam_policy::prepare);
  EXPECT_EQ(snapshot.beams[1].policy, ntn_service_beam_policy::normal);
  EXPECT_FALSE(controller.blocks_new_demand_for_beam("CN-BEAM-0001"));
  EXPECT_FALSE(controller.forces_drain_for_beam("CN-BEAM-0001"));
}

TEST(ntn_service_switch_over_controller_test, hard_event_blocks_new_demand_and_forces_drain)
{
  ntn_service_switch_over_controller controller;
  ASSERT_TRUE(controller.apply_event(make_event(2, ntn_service_switch_over_type::hard, "CN-BEAM-0001")));

  EXPECT_TRUE(controller.blocks_new_demand_for_beam("CN-BEAM-0001"));
  EXPECT_TRUE(controller.forces_drain_for_beam("CN-BEAM-0001"));
  EXPECT_FALSE(controller.blocks_new_demand_for_beam("CN-BEAM-0002"));
}

TEST(ntn_service_switch_over_controller_test, clear_event_restores_normal_policy)
{
  ntn_service_switch_over_controller controller;
  ASSERT_TRUE(controller.apply_event(make_event(3, ntn_service_switch_over_type::hard, "CN-BEAM-0001")));
  ASSERT_TRUE(controller.clear_event(3));

  EXPECT_FALSE(controller.blocks_new_demand_for_beam("CN-BEAM-0001"));
  EXPECT_FALSE(controller.forces_drain_for_beam("CN-BEAM-0001"));
}

TEST(ntn_service_switch_over_controller_test, manual_freeze_disables_automatic_source_updates_until_restore)
{
  ntn_service_switch_over_controller controller;

  ntn_manual_override_command freeze;
  freeze.mode   = ntn_manual_override_mode::freeze_current_state;
  freeze.reason = "freeze-test";
  ASSERT_TRUE(controller.apply_manual_override(freeze));
  EXPECT_FALSE(controller.automatic_source_updates_allowed());

  ntn_manual_override_command restore;
  restore.mode = ntn_manual_override_mode::restore_automatic_source;
  ASSERT_TRUE(controller.apply_manual_override(restore));
  EXPECT_TRUE(controller.automatic_source_updates_allowed());
}
```

- [ ] **Step 2: 更新 CMake test list**

在 `tests/unittests/cu_cp/ntn_mobility/CMakeLists.txt` 的 `add_executable(ntn_mobility_test` 列表加入：

```cmake
  ntn_service_switch_over_controller_test.cpp
```

- [ ] **Step 3: 运行 test，确认缺少 controller**

Run:

```powershell
cmake --build build/ai-clean --target ntn_mobility_test -j2
```

Expected before implementation:

```text
fatal error: lib/cu_cp/ntn_mobility/ntn_service_switch_over_controller.h: No such file or directory
```

- [ ] **Step 4: 创建 controller header**

Create `lib/cu_cp/ntn_mobility/ntn_service_switch_over_controller.h`:

```cpp
#pragma once

#include "srsran/cu_cp/ntn_service_switch_over.h"
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace srsran {
namespace srs_cu_cp {

class ntn_service_switch_over_controller
{
public:
  bool apply_event(const ntn_service_switch_over_event& event);
  bool clear_event(uint64_t event_id);
  bool apply_manual_override(const ntn_manual_override_command& command);

  bool automatic_source_updates_allowed() const;
  bool blocks_new_demand_for_beam(const std::string& beam_id) const;
  bool forces_drain_for_beam(const std::string& beam_id) const;

  ntn_manual_override_mode get_manual_override_mode() const { return manual_override_mode; }

  ntn_service_switch_over_snapshot
  build_snapshot(const std::vector<std::pair<std::string, nr_cell_identity>>& known_beams) const;

private:
  ntn_service_beam_policy policy_for_beam(const std::string& beam_id) const;

  std::vector<ntn_service_switch_over_event> active_events;
  ntn_manual_override_mode                   manual_override_mode = ntn_manual_override_mode::none;
};

} // namespace srs_cu_cp
} // namespace srsran
```

- [ ] **Step 5: 创建 controller implementation**

Create `lib/cu_cp/ntn_mobility/ntn_service_switch_over_controller.cpp`:

```cpp
#include "ntn_service_switch_over_controller.h"
#include <algorithm>

using namespace srsran;
using namespace srs_cu_cp;

static bool event_affects_beam(const ntn_service_switch_over_event& event, const std::string& beam_id)
{
  return std::find(event.affected_beam_ids.begin(), event.affected_beam_ids.end(), beam_id) !=
         event.affected_beam_ids.end();
}

bool ntn_service_switch_over_controller::apply_event(const ntn_service_switch_over_event& event)
{
  if (event.event_id == 0 || (event.affected_beam_ids.empty() && event.affected_ncis.empty())) {
    return false;
  }

  clear_event(event.event_id);
  active_events.push_back(event);
  return true;
}

bool ntn_service_switch_over_controller::clear_event(uint64_t event_id)
{
  const auto old_size = active_events.size();
  active_events.erase(std::remove_if(active_events.begin(),
                                     active_events.end(),
                                     [event_id](const ntn_service_switch_over_event& event) {
                                       return event.event_id == event_id;
                                     }),
                      active_events.end());
  return active_events.size() != old_size;
}

bool ntn_service_switch_over_controller::apply_manual_override(const ntn_manual_override_command& command)
{
  switch (command.mode) {
    case ntn_manual_override_mode::none:
    case ntn_manual_override_mode::freeze_current_state:
    case ntn_manual_override_mode::replace_service_state:
      manual_override_mode = command.mode;
      return true;
    case ntn_manual_override_mode::restore_automatic_source:
      manual_override_mode = ntn_manual_override_mode::none;
      return true;
  }
  return false;
}

bool ntn_service_switch_over_controller::automatic_source_updates_allowed() const
{
  return manual_override_mode != ntn_manual_override_mode::freeze_current_state &&
         manual_override_mode != ntn_manual_override_mode::replace_service_state;
}

ntn_service_beam_policy ntn_service_switch_over_controller::policy_for_beam(const std::string& beam_id) const
{
  ntn_service_beam_policy policy = ntn_service_beam_policy::normal;
  for (const auto& event : active_events) {
    if (!event_affects_beam(event, beam_id)) {
      continue;
    }
    if (event.type == ntn_service_switch_over_type::hard) {
      return event.policy == ntn_service_switch_over_policy::release_allowed ? ntn_service_beam_policy::release_allowed
                                                                             : ntn_service_beam_policy::drain;
    }
    policy = ntn_service_beam_policy::prepare;
  }
  return policy;
}

bool ntn_service_switch_over_controller::blocks_new_demand_for_beam(const std::string& beam_id) const
{
  const ntn_service_beam_policy policy = policy_for_beam(beam_id);
  return policy == ntn_service_beam_policy::drain || policy == ntn_service_beam_policy::release_allowed ||
         policy == ntn_service_beam_policy::block_new_demand;
}

bool ntn_service_switch_over_controller::forces_drain_for_beam(const std::string& beam_id) const
{
  const ntn_service_beam_policy policy = policy_for_beam(beam_id);
  return policy == ntn_service_beam_policy::drain || policy == ntn_service_beam_policy::release_allowed;
}

ntn_service_switch_over_snapshot ntn_service_switch_over_controller::build_snapshot(
    const std::vector<std::pair<std::string, nr_cell_identity>>& known_beams) const
{
  ntn_service_switch_over_snapshot snapshot;
  snapshot.manual_override_mode             = manual_override_mode;
  snapshot.automatic_source_updates_allowed = automatic_source_updates_allowed();
  snapshot.active_events                    = active_events;
  snapshot.beams.reserve(known_beams.size());
  for (const auto& beam : known_beams) {
    snapshot.beams.push_back({beam.first, beam.second, policy_for_beam(beam.first)});
  }
  return snapshot;
}
```

- [ ] **Step 6: 更新 library CMake**

在 `lib/cu_cp/ntn_mobility/CMakeLists.txt` 的 library source list 加入：

```cmake
  ntn_service_switch_over_controller.cpp
```

- [ ] **Step 7: 运行 controller tests**

Run:

```powershell
cmake --build build/ai-clean --target ntn_mobility_test -j2
ctest --test-dir build/ai-clean -R "ntn_service_switch_over_controller_test" --output-on-failure
```

Expected:

```text
100% tests passed
```

- [ ] **Step 8: Commit**

仅在隔离任务分支提交：

```bash
git add include/srsran/cu_cp/ntn_service_switch_over.h lib/cu_cp/ntn_mobility/ntn_service_switch_over_controller.* lib/cu_cp/ntn_mobility/CMakeLists.txt tests/unittests/cu_cp/ntn_mobility/ntn_service_switch_over_controller_test.cpp tests/unittests/cu_cp/ntn_mobility/CMakeLists.txt
git commit -m "feat(cucp-011): add NTN service switch-over controller"
```

---

### Task 6: CUCP-011 Command Handler Contract

**Files:**
- Modify: `include/srsran/cu_cp/cu_cp_command_handler.h`
- Modify: `lib/cu_cp/cu_cp_impl.h`
- Modify: `lib/cu_cp/cu_cp_impl.cpp`

- [ ] **Step 1: 扩展 command handler interface**

在 `include/srsran/cu_cp/cu_cp_command_handler.h` include 区加入：

```cpp
#include "srsran/cu_cp/ntn_service_switch_over.h"
```

在 `class cu_cp_ntn_command_handler` 中加入：

```cpp
  /// Apply a CU-CP-only NTN service switch-over event.
  virtual bool handle_ntn_service_switch_over_event(const ntn_service_switch_over_event& event) = 0;

  /// Clear an active CU-CP-only NTN service switch-over event by id.
  virtual bool clear_ntn_service_switch_over_event(uint64_t event_id) = 0;

  /// Apply a CU-CP-only manual override for NTN service source state.
  virtual bool handle_ntn_manual_override(const ntn_manual_override_command& command) = 0;

  /// Get the current CU-CP-side NTN service switch-over snapshot.
  virtual ntn_service_switch_over_snapshot get_current_ntn_service_switch_over_snapshot() const = 0;
```

- [ ] **Step 2: 增加 cu_cp_impl members 和 helpers**

在 `lib/cu_cp/cu_cp_impl.h` include 区加入：

```cpp
#include "ntn_mobility/ntn_service_switch_over_controller.h"
```

在 private methods 区加入：

```cpp
  std::optional<std::string> find_ntn_beam_id_by_nci(nr_cell_identity nci) const;
  std::vector<std::pair<std::string, nr_cell_identity>> get_configured_ntn_beam_cells() const;
  bool refresh_ntn_beam_placement_for_service_policy();
```

在 members 区加入：

```cpp
  ntn_service_switch_over_controller ntn_service_switch_over_ctrl;
```

- [ ] **Step 3: 实现 command methods**

在 `lib/cu_cp/cu_cp_impl.cpp` 的 NTN command methods 附近加入：

```cpp
bool cu_cp_impl::handle_ntn_service_switch_over_event(const ntn_service_switch_over_event& event)
{
  if (!ntn_service_switch_over_ctrl.apply_event(event)) {
    logger.warning("Rejected NTN service switch-over event id={}", event.event_id);
    return false;
  }
  return refresh_ntn_beam_placement_for_service_policy();
}

bool cu_cp_impl::clear_ntn_service_switch_over_event(uint64_t event_id)
{
  const bool changed = ntn_service_switch_over_ctrl.clear_event(event_id);
  if (changed) {
    return refresh_ntn_beam_placement_for_service_policy();
  }
  return false;
}

bool cu_cp_impl::handle_ntn_manual_override(const ntn_manual_override_command& command)
{
  if (!ntn_service_switch_over_ctrl.apply_manual_override(command)) {
    logger.warning("Rejected NTN manual override command mode={}", static_cast<unsigned>(command.mode));
    return false;
  }

  if (command.mode == ntn_manual_override_mode::replace_service_state) {
    if (command.replacement_satellite_ecef.has_value()) {
      current_ntn_satellite_ecef          = command.replacement_satellite_ecef.value();
      current_ntn_satellite_epoch         = std::chrono::system_clock::now();
      current_ntn_satellite_received_time = std::chrono::steady_clock::now();
    }
    if (!command.replacement_served_beam_ids.empty() && !update_ntn_served_beams(command.replacement_served_beam_ids)) {
      return false;
    }
  }

  return refresh_ntn_beam_placement_for_service_policy();
}

ntn_service_switch_over_snapshot cu_cp_impl::get_current_ntn_service_switch_over_snapshot() const
{
  return ntn_service_switch_over_ctrl.build_snapshot(get_configured_ntn_beam_cells());
}
```

- [ ] **Step 4: 实现 beam lookup helpers**

在 `lib/cu_cp/cu_cp_impl.cpp` 的 static/helper 区或 methods 区加入：

```cpp
std::optional<std::string> cu_cp_impl::find_ntn_beam_id_by_nci(nr_cell_identity nci) const
{
  const auto& ntn_cfg = cfg.mobility.meas_manager_config.ntn_location_mobility;
  const auto  it      = std::find_if(ntn_cfg.beams.begin(),
                                ntn_cfg.beams.end(),
                                [nci](const ntn_beam_position& beam) { return beam.nci == nci; });
  if (it == ntn_cfg.beams.end()) {
    return std::nullopt;
  }
  return it->beam_id;
}

std::vector<std::pair<std::string, nr_cell_identity>> cu_cp_impl::get_configured_ntn_beam_cells() const
{
  std::vector<std::pair<std::string, nr_cell_identity>> result;
  const auto& ntn_cfg = cfg.mobility.meas_manager_config.ntn_location_mobility;
  result.reserve(ntn_cfg.beams.size());
  for (const auto& beam : ntn_cfg.beams) {
    result.emplace_back(beam.beam_id, beam.nci);
  }
  return result;
}

bool cu_cp_impl::refresh_ntn_beam_placement_for_service_policy()
{
  std::vector<ntn_served_beam_candidate> filtered_candidates;
  filtered_candidates.reserve(current_ntn_served_beam_candidates.size());
  for (const auto& candidate : current_ntn_served_beam_candidates) {
    if (!ntn_service_switch_over_ctrl.forces_drain_for_beam(candidate.beam_id)) {
      filtered_candidates.push_back(candidate);
    }
  }
  return update_ntn_served_beam_candidates(filtered_candidates);
}
```

- [ ] **Step 5: manual freeze gate satellite updates**

在 `cu_cp_impl::handle_ntn_satellite_state_update()` 开头、`ntn_served_beam_sched` 检查之后加入：

```cpp
  if (!ntn_service_switch_over_ctrl.automatic_source_updates_allowed()) {
    logger.debug("Ignoring NTN satellite state update because manual override freezes automatic source updates");
    return false;
  }
```

- [ ] **Step 6: 编译 CU-CP target**

Run:

```powershell
cmake --build build/ai-clean --target cu_cp_test ntn_mobility_test -j2
```

Expected:

```text
cu_cp_test and ntn_mobility_test build
```

- [ ] **Step 7: Commit**

仅在隔离任务分支提交：

```bash
git add include/srsran/cu_cp/cu_cp_command_handler.h lib/cu_cp/cu_cp_impl.h lib/cu_cp/cu_cp_impl.cpp
git commit -m "feat(cucp-011): expose NTN switch-over commands"
```

---

### Task 7: CUCP-011 Admission And Demand Gates

**Files:**
- Modify: `lib/cu_cp/cu_cp_impl.h`
- Modify: `lib/cu_cp/cu_cp_impl.cpp`
- Modify: `tests/unittests/cu_cp/cu_cp_ntn_mobility_test.cpp`

- [ ] **Step 1: 增加 gate helper 声明**

在 `lib/cu_cp/cu_cp_impl.h` 中 `block_new_ntn_demand_if_assistance_is_stale` 附近加入：

```cpp
  bool block_new_ntn_demand_if_service_policy_blocks(const char* demand_name, std::optional<std::string> beam_id);
  bool block_new_ntn_demand_if_ntn_policy_blocks(const char* demand_name, std::optional<std::string> beam_id);
```

- [ ] **Step 2: 实现 gate helper**

在 `lib/cu_cp/cu_cp_impl.cpp` 中加入：

```cpp
bool cu_cp_impl::block_new_ntn_demand_if_service_policy_blocks(const char* demand_name,
                                                               std::optional<std::string> beam_id)
{
  if (!beam_id.has_value()) {
    return false;
  }
  if (!ntn_service_switch_over_ctrl.blocks_new_demand_for_beam(beam_id.value())) {
    return false;
  }
  logger.warning("Blocking new NTN {} demand for beam={}. Cause: service switch-over policy",
                 demand_name,
                 beam_id.value());
  refresh_ntn_beam_placement_for_service_policy();
  return true;
}

bool cu_cp_impl::block_new_ntn_demand_if_ntn_policy_blocks(const char* demand_name, std::optional<std::string> beam_id)
{
  if (block_new_ntn_demand_if_assistance_is_stale(demand_name)) {
    return true;
  }
  return block_new_ntn_demand_if_service_policy_blocks(demand_name, std::move(beam_id));
}
```

- [ ] **Step 3: RRC setup gate 使用 UE serving beam**

在 `handle_ue_setup_request()` 中把 stale-only gate 替换为：

```cpp
  std::optional<std::string> serving_beam_id;
  if (cu_cp_ue* ue = ue_mng.find_du_ue(ue_index); ue != nullptr) {
    if (ue->get_meas_context().last_ntn_location_report.has_value()) {
      serving_beam_id = find_ntn_beam_id_by_nci(ue->get_meas_context().last_ntn_location_report->serving_nci);
    }
  }
  if (block_new_ntn_demand_if_ntn_policy_blocks("RRC setup", serving_beam_id)) {
    logger.debug("ue={}: Rejecting RRC setup. Cause: NTN service policy", ue_index);
    return false;
  }
```

如果 `cu_cp_ue` 没有 last NTN location，setup gate 只应用 stale policy；beam-specific switch-over gate 会在 PDU session、location handover 和 NG handover allocation 阶段生效。

- [ ] **Step 4: PDU session gate 使用已有 UE NTN location**

在 `handle_new_pdu_session_resource_setup_request()` 中把 stale-only gate 替换为：

```cpp
  std::optional<std::string> serving_beam_id;
  if (ue->get_meas_context().last_ntn_location_report.has_value()) {
    serving_beam_id = find_ntn_beam_id_by_nci(ue->get_meas_context().last_ntn_location_report->serving_nci);
  }
  if (block_new_ntn_demand_if_ntn_policy_blocks("PDU session setup", serving_beam_id)) {
    logger.warning("ue={}: Rejecting PDU session resource setup. Cause: NTN service policy", request.ue_index);
    cu_cp_pdu_session_resource_setup_response response;
    for (const auto& setup_item : request.pdu_session_res_setup_items) {
      cu_cp_pdu_session_res_setup_failed_item failed_item;
      failed_item.pdu_session_id              = setup_item.pdu_session_id;
      failed_item.unsuccessful_transfer.cause = ngap_cause_radio_network_t::radio_res_not_available;
      response.pdu_session_res_failed_to_setup_items.emplace(failed_item.pdu_session_id, failed_item);
    }
    return launch_async([response](coro_context<async_task<cu_cp_pdu_session_resource_setup_response>>& ctx) mutable {
      CORO_BEGIN(ctx);
      CORO_RETURN(response);
    });
  }
```

- [ ] **Step 5: NG handover allocation gate 使用 target CGI**

在 `handle_ue_index_allocation_request(const nr_cell_global_id_t& cgi, const plmn_identity& plmn)` 的 admission 前加入：

```cpp
  std::optional<std::string> target_beam_id = find_ntn_beam_id_by_nci(cgi.nci);
  if (block_new_ntn_demand_if_ntn_policy_blocks("incoming handover", target_beam_id)) {
    logger.warning("Could not allocate new UE index for incoming handover CGI={}. Cause: NTN service policy", cgi.nci);
    return ue_index_t::invalid;
  }
```

- [ ] **Step 6: location handover gate 使用 target beam**

在 `handle_ue_location_report()` 中保留 stale gate，然后在 `cell_meas_mng.report_ue_location()` 前增加：

```cpp
  std::optional<std::string> serving_beam_id = find_ntn_beam_id_by_nci(location_report.serving_nci);
  if (block_new_ntn_demand_if_service_policy_blocks("location handover", serving_beam_id)) {
    logger.debug("ue={}: NTN location report ignored. Cause: NTN service policy", location_report.ue_index);
    return;
  }
```

- [ ] **Step 7: 写 integration test，hard event drains loaded beam**

在 `tests/unittests/cu_cp/cu_cp_ntn_mobility_test.cpp` 加入：

```cpp
TEST(cu_cp_ntn_mobility_test, hard_switch_over_drains_loaded_beam_and_blocks_new_pdu_demand)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));

  env.run_ng_setup();
  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());
  ASSERT_TRUE(connect_cu_up_for_ue_admission(env));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  ASSERT_TRUE(env.connect_new_ue(du_idx.value(), du_ue_id, to_rnti(0x4601)));
  ASSERT_TRUE(env.authenticate_ue(du_idx.value(), du_ue_id, uint_to_amf_ue_id(0)));

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());

  auto* cu_cp_impl = get_cu_cp_impl(env);
  ASSERT_NE(cu_cp_impl, nullptr);
  const ue_index_t ue_index = uint_to_ue_index(gnb_cu_ue_f1ap_id_to_uint(ue_ctx->cu_ue_id.value()));
  cu_cp_impl->get_cu_cp_measurement_handler().handle_ue_location_report(
      make_ntn_location_report(ue_index, make_default_env_nci(0)));

  ntn_service_switch_over_event event;
  event.event_id = 101;
  event.type     = ntn_service_switch_over_type::hard;
  event.source   = ntn_service_switch_over_source::operator_command;
  event.policy   = ntn_service_switch_over_policy::drain;
  event.affected_beam_ids = {"CN-BEAM-0001"};
  ASSERT_TRUE(ntn_handler.handle_ntn_service_switch_over_event(event));

  const cu_cp_ntn_beam_status beam_status =
      find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001");
  ASSERT_EQ(beam_status.state, cu_cp_ntn_beam_assignment_state::draining);

  const pdu_session_id_t blocked_psi = uint_to_pdu_session_id(2);
  env.get_amf().push_tx_pdu(generate_valid_pdu_session_resource_setup_request_message(
      uint_to_amf_ue_id(0), uint_to_ran_ue_id(0), blocked_psi));

  ngap_message ngap_pdu;
  ASSERT_TRUE(env.wait_for_ngap_tx_pdu(ngap_pdu));
  ASSERT_TRUE(test_helpers::is_valid_pdu_session_resource_setup_response(ngap_pdu));
  ASSERT_TRUE(test_helpers::is_expected_pdu_session_resource_setup_response(ngap_pdu, {}, {blocked_psi}));
}
```

- [ ] **Step 8: 写 manual freeze/restore integration test**

在同一测试文件加入：

```cpp
TEST(cu_cp_ntn_mobility_test, manual_freeze_ignores_satellite_updates_until_restore)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));

  ASSERT_TRUE(connect_du_for_ntn_beams(env).has_value());

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));
  ASSERT_EQ(ntn_handler.get_current_ntn_served_beam_ids(), std::vector<std::string>({"CN-BEAM-0001"}));

  ntn_manual_override_command freeze;
  freeze.mode = ntn_manual_override_mode::freeze_current_state;
  ASSERT_TRUE(ntn_handler.handle_ntn_manual_override(freeze));
  ASSERT_FALSE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 1.0, 500000.0)));
  ASSERT_EQ(ntn_handler.get_current_ntn_served_beam_ids(), std::vector<std::string>({"CN-BEAM-0001"}));

  ntn_manual_override_command restore;
  restore.mode = ntn_manual_override_mode::restore_automatic_source;
  ASSERT_TRUE(ntn_handler.handle_ntn_manual_override(restore));
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 1.0, 500000.0)));
  ASSERT_EQ(ntn_handler.get_current_ntn_served_beam_ids(), std::vector<std::string>({"CN-BEAM-0002"}));
}
```

- [ ] **Step 9: 运行 CUCP-011 focused tests**

Run:

```powershell
ctest --test-dir build/ai-clean -R "hard_switch_over_drains_loaded_beam_and_blocks_new_pdu_demand|manual_freeze_ignores_satellite_updates_until_restore" --output-on-failure
```

Expected:

```text
100% tests passed
```

- [ ] **Step 10: Commit**

仅在隔离任务分支提交：

```bash
git add lib/cu_cp/cu_cp_impl.h lib/cu_cp/cu_cp_impl.cpp tests/unittests/cu_cp/cu_cp_ntn_mobility_test.cpp
git commit -m "feat(cucp-011): gate NTN demand during service switch-over"
```

---

### Task 8: CUCP-011 Soft Event And Snapshot Behavior

**Files:**
- Modify: `tests/unittests/cu_cp/cu_cp_ntn_mobility_test.cpp`
- Modify: `lib/cu_cp/cu_cp_impl.cpp`

- [ ] **Step 1: 写 soft event integration test**

在 `tests/unittests/cu_cp/cu_cp_ntn_mobility_test.cpp` 加入：

```cpp
TEST(cu_cp_ntn_mobility_test, soft_switch_over_marks_snapshot_without_releasing_existing_ue)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));

  env.run_ng_setup();
  const std::optional<connected_ngap_ntn_ue> ue = connect_ngap_ntn_ue(env);
  ASSERT_TRUE(ue.has_value());

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  ntn_service_switch_over_event event;
  event.event_id = 201;
  event.type     = ntn_service_switch_over_type::soft;
  event.source   = ntn_service_switch_over_source::operator_command;
  event.policy   = ntn_service_switch_over_policy::prepare;
  event.affected_beam_ids = {"CN-BEAM-0001"};
  ASSERT_TRUE(ntn_handler.handle_ntn_service_switch_over_event(event));

  ntn_service_switch_over_snapshot snapshot = ntn_handler.get_current_ntn_service_switch_over_snapshot();
  ASSERT_EQ(snapshot.active_events.size(), 1);
  const auto beam_it = std::find_if(snapshot.beams.begin(),
                                    snapshot.beams.end(),
                                    [](const ntn_service_switch_over_beam_snapshot& beam) {
                                      return beam.beam_id == "CN-BEAM-0001";
                                    });
  ASSERT_NE(beam_it, snapshot.beams.end());
  EXPECT_EQ(beam_it->policy, ntn_service_beam_policy::prepare);

  ngap_message ngap_pdu;
  ASSERT_FALSE(env.wait_for_ngap_tx_pdu(ngap_pdu, std::chrono::milliseconds{20}));
}
```

- [ ] **Step 2: 确认 soft event 不触发 drain**

Run:

```powershell
ctest --test-dir build/ai-clean -R "soft_switch_over_marks_snapshot_without_releasing_existing_ue" --output-on-failure
```

Expected:

```text
100% tests passed
```

- [ ] **Step 3: Commit**

仅在隔离任务分支提交：

```bash
git add tests/unittests/cu_cp/cu_cp_ntn_mobility_test.cpp lib/cu_cp/cu_cp_impl.cpp
git commit -m "test(cucp-011): cover soft NTN service switch-over snapshot"
```

---

### Task 9: Stage Validation

**Files:**
- No code changes

- [ ] **Step 1: Build focused targets**

Run:

```powershell
cmake --build build/ai-clean --target ntn_mobility_test cu_cp_test ngap_test -j2
```

Expected:

```text
ntn_mobility_test, cu_cp_test, and ngap_test build
```

- [ ] **Step 2: Run focused ctest**

Run:

```powershell
ctest --test-dir build/ai-clean -R "ntn_service_switch_over_controller_test|cu_cp_ntn_mobility_test|ngap" --output-on-failure
```

Expected:

```text
All matched executable tests pass
```

- [ ] **Step 3: Run harness validation for CUCP-010**

Run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File ai_harness/scripts/run_task_validation.ps1 -TaskId CUCP-010 -TaskFile ai_harness/tasks/CUCP-010-ngap-core-mapping.md -CTestRegex "cu_cp_ntn_mobility_test|ngap"
```

Expected:

```text
path guard passes
rejected overlap passes
task metadata passes
focused ctest completes
logs written under ai_harness/results/CUCP-010-<timestamp>/
```

- [ ] **Step 4: Run harness validation for CUCP-011**

Run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File ai_harness/scripts/run_task_validation.ps1 -TaskId CUCP-011 -TaskFile ai_harness/tasks/CUCP-011-switch-over-resilience.md -CTestRegex "cu_cp_ntn_mobility_test|ntn_mobility_test"
```

Expected:

```text
path guard passes
rejected overlap passes
task metadata passes
focused ctest completes
logs written under ai_harness/results/CUCP-011-<timestamp>/
```

- [ ] **Step 5: Run required CU-CP validation**

Run:

```bash
bash ai_harness/scripts/configure_build.sh
bash ai_harness/scripts/build.sh
bash ai_harness/scripts/run_cucp_tests.sh
python ai_harness/scripts/guard_changed_paths.py --base ai/cucp-harness-base --task-file ai_harness/tasks/CUCP-010-ngap-core-mapping.md
python ai_harness/scripts/guard_changed_paths.py --base ai/cucp-harness-base --task-file ai_harness/tasks/CUCP-011-switch-over-resilience.md
python ai_harness/scripts/check_rejected_overlap.py --base ai/cucp-harness-base
python ai_harness/scripts/validate_task_metadata.py ai_harness/tasks/CUCP-010-ngap-core-mapping.md ai_harness/tasks/CUCP-011-switch-over-resilience.md
```

Expected:

```text
guards and metadata checks pass
focused CU-CP executable tests pass
any _NOT_BUILT placeholders or unrelated build timeouts are reported with log paths
```

- [ ] **Step 6: Final summary**

最终中文报告必须包含：

```text
base:
files changed:
behavior changed:
tests:
validation results:
risks:
exceptions:
```

---

## 风险与处理

- `CUCP-010` 大部分能力已经存在；执行时以“测试固化和小缺口修复”为主，避免重写稳定路径。
- `Mapped Cell` 在当前本地 NGAP ASN.1 `UserLocationInformationNR` 中没有明显承载字段；本阶段不修改 ASN.1 生成物。
- `handle_ue_setup_request()` 不直接带 beam id；beam-specific gate 需要通过 UE context 的 last NTN location、handover target CGI 或 PDU-session UE context 间接定位。
- `soft switch-over` 只表达 prepare，不释放已有 UE，不触碰 lower layer。
- `hard switch-over` 通过过滤 current served candidates 让已有 loaded beam 进入 planner 的 draining 语义，不改 DU/MAC scheduler。
- manual replace 只替换 CU-CP runtime satellite/service state，不写入持久 OAM schema。

## 自检结果

- Spec coverage:
  - `CUCP-010`：Task 1-3 覆盖 NR CGI、TAI、timestamp、derived TAC、LocationReportingControl direct/change/area/cancel、draining 报告语义和 Mapped Cell 边界。
  - `CUCP-011`：Task 4-8 覆盖 soft/hard event、manual override、source freeze、beam drain、new demand gate、snapshot。
  - Validation：Task 9 覆盖 focused ctest、harness validation、path guard、rejected overlap 和 metadata。
- Placeholder scan:
  - 计划没有使用未落地的泛化表达。
  - 对 Mapped Cell 的处理是本地 ASN.1 搜索结论，执行时按该边界报告。
- Type consistency:
  - public type header、controller header、command handler method、`cu_cp_impl` helper、test snippet 的类型名一致。
  - `ntn_service_switch_over_controller` 的 test 方法名与实现方法名一致。
