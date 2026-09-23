# SmartMediVend Two-Stage Medical Flow Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make mild supported cases reach a fast local `OFFER` and a physically confirmed 500 ms relay pulse, while keeping explicit clinical danger and transaction-integrity failures closed.

**Architecture:** Parse and validate rules/catalog once into an immutable `MedicalPolicyCache`, merge small turn deltas into one bounded `MedicalIntakeSession`, run indexed `FastScreen` after each answer, and run `FullEvaluate` only when decisive facts are complete. `VendingCoordinator` consumes the structured result, preserves `next_question_vi`, and retains the existing local approval, stock, route, button, NVS reservation, and relay boundaries.

**Tech Stack:** C++23 on ESP-IDF 6.1, cJSON, NVS, `esp_console`, `esp_timer`, CMake, PowerShell/MSVC host tests, Python standard library approval tooling.

**Spec:** `docs/superpowers/specs/2026-09-23-smartmedivend-two-stage-medical-flow-design.md`

## Global Constraints

- Work directly on `main`; do not create a branch or worktree.
- Preserve unrelated worktree changes and never stage `Smart-Medication-Vending.zip`.
- Static medical artifacts are parsed once during board initialization and never reparsed per turn.
- Missing or ambiguous non-dangerous information returns `ASK`; only explicit hard clinical risk returns `REFER`; integrity/configuration failures return `BLOCK`.
- AI input cannot select or observe a SKU route, relay channel, stock count, quantity, transaction authorization, or pulse.
- GPIO17 remains active LOW with external pull-up; selector GPIO39–42 settle 10 ms; pulse is 500 ms; all-off guard is 100 ms.
- Relay, protocol, timer, and button callbacks stay bounded and schedule application mutations through `Application::Schedule()` or event bits.
- Pharmacist identity, license, timestamp, product strength, and clinical priority are never fabricated. Synthetic values are confined to host tests.
- NVS schema/key changes use explicit versioned keys; old stock is not silently migrated into a new catalog binding.
- Development build remains physical-vending-disabled; `smartmedivend-s3-production` explicitly enables only the compile-time gate.
- Format only touched C/C++ files with the repository `.clang-format`.

## Review Focus

- An unrelated recognized condition/medicine/allergy must not remove every candidate; an unknown/ambiguous token must return one clarification question rather than silently pass or globally refer.
- A correction, stale/replayed turn, ten-minute session expiry, MQTT loss, or WebSocket loss must invalidate any staged offer before a physical click can vend.
- Null/empty/malformed safety predicates and question arrays must never become “no contraindications”; the boot policy must become invalid with a stable reason.
- Missing/corrupt/catalog-mismatched/pending inventory must be recoverable only by explicit local physical-count reconciliation; no automatic stock seeding or pending clear is permitted.
- A stopped, expired-but-queued, or concurrent timer callback must never observe a later arm's generation/context or produce/extend a LOW pulse.

---

### Task 1: Verify and commit the existing disconnect, timer, and schema hardening

**Files:**
- Modify: `main/application.cc` (`Initialize`, `Run`, `InitializeProtocol`)
- Modify: `main/application.h` (transport health member and event bit)
- Modify: `main/protocols/websocket_protocol.cc`
- Modify: `main/protocols/websocket_protocol.h`
- Modify: `main/boards/smartmedivend-s3/esp_relay_platform.cc`
- Modify: `main/boards/smartmedivend-s3/esp_relay_platform.h`
- Modify: `main/boards/smartmedivend-s3/smartmedivend_board.cc`
- Modify: `main/vending/catalog_router.cc`
- Create: `main/vending/transport_health_gate.h`
- Create: `main/vending/tests/test_transport_health_gate.cc`
- Create: `main/vending/tests/test_esp_relay_platform.cc`
- Create: `main/vending/tests/esp_host_include/driver/gpio.h`
- Create: `main/vending/tests/esp_host_include/esp_timer.h`
- Create: `main/vending/tests/esp_host_include/freertos/FreeRTOS.h`
- Create: `main/vending/tests/host_include/cJSON.h`
- Create: `scripts/tests/test_smartmedivend_transport_disconnect.py`
- Modify: `main/vending/tests/test_catalog_router.cc`
- Modify: `main/vending/tests/run_host_tests.sh`
- Modify: `main/vending/tests/run_host_tests.ps1`

**Interfaces:**
- Consumes: protocol connect/disconnect callbacks and ESP timer expiration/cancel behavior.
- Produces: `Application::IsTransportOperational()`, stale-reconnect rejection, immutable timer callback leases, and strict current rule-envelope validation.

- [ ] **Step 1: Inspect the existing uncommitted patch and isolate its scope**

Run:

```powershell
git status --short
git diff -- main/application.cc main/application.h main/protocols/websocket_protocol.cc main/protocols/websocket_protocol.h main/boards/smartmedivend-s3/esp_relay_platform.cc main/boards/smartmedivend-s3/esp_relay_platform.h main/boards/smartmedivend-s3/smartmedivend_board.cc main/vending/catalog_router.cc main/vending/tests
```

Expected: only transport gating, timer lease protection, strict rule validation, and their tests are in scope. Keep the ZIP and every unrelated path unstaged.

- [ ] **Step 2: Add the new ESP adapter and transport tests to the PowerShell runner**

Append MSVC compile/run blocks equivalent to:

```powershell
$transportArgs = @("/nologo", "/std:c++20", "/EHsc", "/W4", "/WX",
    "/Imain", "/Imain/vending", "main/vending/tests/test_transport_health_gate.cc",
    "/Fe:$transportExe")
$espRelayArgs = @("/nologo", "/std:c++20", "/EHsc", "/W4", "/WX",
    "/Imain", "/Imain/vending", "/Imain/boards/smartmedivend-s3",
    "/Imain/vending/tests/esp_host_include",
    "main/boards/smartmedivend-s3/esp_relay_platform.cc",
    "main/vending/tests/test_esp_relay_platform.cc", "/Fe:$espRelayExe")
```

Run each executable and throw on a nonzero exit code, matching the existing runner style.

Create a static Python regression that asserts `Application::InitializeProtocol()` registers
`Protocol::OnDisconnected`, MQTT invokes `on_disconnected_` on broker loss, WebSocket invokes it on
unintentional socket loss, and both paths reach the board hook through the application event bit.

- [ ] **Step 3: Run the focused host suites**

Run:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/tests/run_smartmedivend_host_tests.ps1
powershell -ExecutionPolicy Bypass -File main/vending/tests/run_host_tests.ps1
& 'C:\Espressif\python_env\idf6.1_py3.14_env\Scripts\python.exe' -m unittest scripts.tests.test_smartmedivend_transport_disconnect -v
```

Expected: medical, review, catalog, inventory, coordinator, relay, ESP relay platform, and transport health tests pass. Fix only failures caused by these hardening hunks.

- [ ] **Step 4: Build the current development variant with ESP-IDF 6.1**

Run:

```powershell
$env:PYTHONUTF8='1'
$env:PATH='C:\Espressif\tools\python\v6.1\venv\Scripts;' + $env:PATH
. 'C:\esp\v6.1\esp-idf\export.ps1'
& 'C:\Espressif\python_env\idf6.1_py3.14_env\Scripts\python.exe' scripts/build.py smartmedivend-s3 --name smartmedivend-s3
```

Expected: build succeeds and physical vending remains disabled in this variant.

- [ ] **Step 5: Commit only the verified hardening files**

Run `git add` with the exact Task 1 paths, including
`scripts/tests/test_smartmedivend_transport_disconnect.py`, then:

```powershell
git diff --cached --check
git status --short
git commit -m "fix: harden vending transport and timer races"
```

Expected: the ZIP remains untracked and no unrelated file is included.

### Task 2: Add the immutable boot-time medical policy cache

**Files:**
- Create: `main/medical/medical_policy_cache.h`
- Create: `main/medical/medical_policy_cache.cc`
- Create: `main/medical/tests/test_medical_policy_cache.cc`
- Modify: `data/medical_rules.json` (recognized non-excluding input sentinels and version)
- Modify: `data/medicines.json` (channel-15 identity and catalog version only)
- Modify: `main/vending/catalog_router.cc` (temporary schema compatibility until Task 5)
- Modify: `main/vending/tests/test_catalog_router.cc`
- Modify: `main/CMakeLists.txt` (SmartMediVend source list)
- Modify: `scripts/tests/run_smartmedivend_host_tests.ps1`
- Modify: `scripts/tests/run_smartmedivend_host_tests.sh`

**Interfaces:**
- Consumes: exact embedded rules and catalog bytes.
- Produces: one immutable typed cache with `valid()`, `validation_reason()`, normalized questions/rules/items, and indexed lookups used by later tasks.

- [ ] **Step 1: Write cache tests that fail because the cache does not exist**

Create fixtures around these assertions:

```cpp
smv::MedicalPolicyCache policy(rules, fixed_catalog);
Check(policy.valid(), "valid repository-shaped policy rejected");
Check(policy.artifact_parse_count() == 2, "artifacts were not parsed exactly once");
Check(policy.RulesForSymptom("mild_headache").size() == 1,
      "symptom index missing paracetamol");
Check(policy.FindQuestion("headache_mild") != nullptr,
      "question index missing symptom check");
Check(policy.FindCatalogItem("PARACETAMOL_500")->primary_channel == 0,
      "primary route not normalized");
Check(policy.FindCatalogItem("PARACETAMOL_500")->backup_channel == 13,
      "backup route not normalized");
```

Add mutations that make `valid()` false for null/empty/non-string `symptoms`, `refer_if`, or `exclude_if`; duplicate question IDs; missing symptom profiles; unknown canonical IDs; duplicate/out-of-range channels; unsupported units; primary/backup mismatch; oversized strings/arrays; and trailing JSON bytes.

- [ ] **Step 2: Run the new cache test and verify it fails**

Run:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/tests/run_smartmedivend_host_tests.ps1
```

Expected: compilation fails on `medical/medical_policy_cache.h`.

- [ ] **Step 3: Define bounded policy types and lookups**

Use these public shapes consistently in later tasks:

```cpp
inline constexpr size_t kMaxMedicalRules = 16;
inline constexpr size_t kMaxMedicalQuestions = 64;
inline constexpr size_t kMaxMedicalFlags = 128;
inline constexpr size_t kMaxMedicalSymptoms = 32;

enum class QuestionAction { kRefer, kClarify };

struct PolicyQuestion {
    std::string id;
    std::string question_vi;
    bool expected = false;
    QuestionAction on_mismatch = QuestionAction::kRefer;
    uint16_t priority = 0;
};

struct MedicinePolicy {
    std::string canonical_id;
    std::vector<size_t> symptom_indices;
    std::bitset<kMaxMedicalFlags> refer_flags;
    std::bitset<kMaxMedicalFlags> exclude_flags;
    std::optional<uint8_t> minimum_age_years;
    std::optional<uint16_t> selection_priority;
};

struct PolicyCatalogItem {
    std::string canonical_id;
    std::string name;
    std::string active_ingredient;
    std::string strength;
    std::string primary_sku;
    uint8_t primary_channel = kInvalidVendingChannel;
    std::optional<std::string> backup_sku;
    std::optional<uint8_t> backup_channel;
};
```

`MedicalPolicyCache` stores no cJSON tree after construction. It enforces the constants above and
exposes `RulesForSymptom`, `QuestionsForSymptom`, `FindQuestion`, `FindCatalogItem`, `FlagIndex`,
`SymptomIndex`, versions, validation reason, and the read-only diagnostic
`artifact_parse_count()`.

- [ ] **Step 4: Implement one-time parse and complete cross-validation**

Copy each bounded input view into a NUL-terminated `std::string`, parse it once with
`cJSON_ParseWithLengthOpts(text.c_str(), text.size() + 1, nullptr, true)`, normalize into bounded
vectors/bitsets/maps, then destroy the cJSON trees. Validate the complete
rule/interview/catalog schema currently split between `MedicalAdvisor` and `CatalogRouter`;
malformed optional predicates are invalid, never interpreted as absent.

Accept an optional integer `selection_priority` only in the range 1–65535. If multiple rules share a symptom, absent or tied priorities remain representable so `FullEvaluate` can return a safe ambiguity; file order is never recorded as priority.

Use configured array order only to assign deterministic question-scheduling priority when a question
does not contain an explicit priority; this ordering never participates in medicine selection.

- [ ] **Step 5: Repair only the mechanical antacid backup identity**

Add `recognized_non_excluding_flags` containing exactly `other_condition`,
`other_current_medicine`, and `other_drug_allergy`, and bump `rules_version` to
`rules-2026-09-23-two-stage-v1`. These values become pharmacist-reviewed input vocabulary; they are
not hardcoded bypasses. Temporarily allow this root field in the existing router validator until
Task 5 removes that duplicate parser.

Change channel 15 `strength` from `"cùng SKU channel 7"` to the exact current channel-7 string and
bump `catalog_version` to `catalog-2026-09-23-two-stage-v1`. Do not alter the channel-7 package text
or add approval metadata. Update the router regression from “repository mismatch is locked” to
“repository exact backup is accepted,” while retaining a synthetic mismatched-strength rejection.
Assert the real review remains invalid.

- [ ] **Step 6: Run cache and existing host tests**

Run both PowerShell host runners.

Expected: cache tests pass, repository antacid mapping is structurally valid, and the existing review gate still reports invalid.

- [ ] **Step 7: Commit the cache and mechanical catalog repair**

```powershell
git add main/medical/medical_policy_cache.h main/medical/medical_policy_cache.cc main/medical/tests/test_medical_policy_cache.cc data/medical_rules.json data/medicines.json main/vending/catalog_router.cc main/vending/tests/test_catalog_router.cc main/CMakeLists.txt scripts/tests/run_smartmedivend_host_tests.ps1 scripts/tests/run_smartmedivend_host_tests.sh
git diff --cached --check
git commit -m "feat: cache validated medical policy at boot"
```

### Task 3: Add the bounded incremental medical intake session

**Files:**
- Create: `main/medical/medical_intake_session.h`
- Create: `main/medical/medical_intake_session.cc`
- Create: `main/medical/tests/test_medical_intake_session.cc`
- Modify: `main/CMakeLists.txt`
- Modify: `scripts/tests/run_smartmedivend_host_tests.ps1`
- Modify: `scripts/tests/run_smartmedivend_host_tests.sh`

**Interfaces:**
- Consumes: a turn-delta JSON string, immutable policy, and monotonic milliseconds.
- Produces: normalized one-session facts, accepted turn/facts revisions, explicit field-presence flags, bounded screening answers, and correction/expiry signals.

- [ ] **Step 1: Write failing delta/session tests**

Cover first-turn full payload, later one-answer delta, omitted-field preservation, explicit empty-array replacement, stale/decreasing turn rejection, duplicate keys, forbidden SKU/channel/stock/relay fields, 4096-byte limit, ten-minute expiry boundary, session replacement, primary-symptom correction, and new danger after an earlier safe answer.

Use a sequence such as:

```cpp
smv::MedicalIntakeSession session;
Check(session.ApplyDelta(policy,
    R"({"session_id":"s","turn_id":1,"symptoms":["mild_headache"]})", 1000).accepted,
    "first delta rejected");
Check(session.ApplyDelta(policy,
    R"({"session_id":"s","turn_id":2,"age_years":30})", 1100).accepted,
    "second delta rejected");
Check(session.facts().primary_symptom == policy.SymptomIndex("mild_headache"),
    "omitted symptom was lost");
Check(!session.ApplyDelta(policy,
    R"({"session_id":"s","turn_id":2,"relay":true})", 1200).accepted,
    "replayed forbidden turn accepted");
```

- [ ] **Step 2: Run the new test and verify the missing interface failure**

Run the medical host runner.

Expected: compilation fails on `medical_intake_session.h`.

- [ ] **Step 3: Implement normalized fixed-bound facts**

Define:

```cpp
using MedicalFactBits = std::bitset<kMaxMedicalFlags>;

struct NormalizedMedicalFacts {
    std::optional<uint8_t> age_years;
    std::optional<float> weight_kg;
    std::optional<bool> pregnancy_or_breastfeeding;
    std::optional<uint32_t> duration_hours;
    std::optional<size_t> primary_symptom;
    std::bitset<kMaxMedicalSymptoms> symptoms;
    MedicalFactBits danger_signs;
    MedicalFactBits conditions;
    MedicalFactBits current_medicines;
    MedicalFactBits drug_allergies;
    bool danger_signs_reported = false;
    bool conditions_reported = false;
    bool current_medicines_reported = false;
    bool drug_allergies_reported = false;
    std::array<int8_t, kMaxMedicalQuestions> screening_answers{};
};

struct IntakeApplyResult {
    bool accepted = false;
    bool session_replaced = false;
    bool facts_changed = false;
    std::string reason;
    std::string clarify_field;
};
```

Initialize answers to `-1`; use `0` and `1` only for explicit booleans. Enforce one active session, session ID length 1–64, turn 1–1,000,000, and 600,000 ms inactivity expiry.

- [ ] **Step 4: Implement delta merge and compatibility semantics**

Accept `primary_symptom` plus current intake fields. On the first turn, exactly one `symptoms` entry also establishes the primary symptom for compatibility; multiple symptoms without `primary_symptom` remain accepted facts but produce `clarify_field="primary_symptom"`. Present arrays replace their category; omitted arrays preserve it; `screening_answers` merges.

Accept policy-known flags plus only the sentinels declared in the policy's
`recognized_non_excluding_flags`. Return a clarification result for the literal `unknown` or an
identifier outside the approved vocabulary. Unknown object-field names and vending/control fields
remain hard input rejections.

A primary-symptom change clears only old symptom-profile answers. Any accepted fact change increments `facts_revision`; every new accepted turn updates `last_activity_ms` and invalidates a staged candidate later through the coordinator.

- [ ] **Step 5: Run all session and policy tests**

Run the medical host runner.

Expected: delta merge, expiry, correction, control-field, and bounded-input tests pass without parsing the policy again.

- [ ] **Step 6: Commit the session unit**

```powershell
git add main/medical/medical_intake_session.h main/medical/medical_intake_session.cc main/medical/tests/test_medical_intake_session.cc main/CMakeLists.txt scripts/tests/run_smartmedivend_host_tests.ps1 scripts/tests/run_smartmedivend_host_tests.sh
git diff --cached --check
git commit -m "feat: retain bounded medical turn deltas"
```

### Task 4: Refactor MedicalAdvisor into FastScreen and FullEvaluate

**Files:**
- Modify: `main/medical/medical_advisor.h`
- Modify: `main/medical/medical_advisor.cc`
- Modify: `main/medical/tests/test_medical_advisor.cc`
- Modify: `main/vending/vending_coordinator.cc`
- Modify: `main/vending/tests/test_vending_coordinator.cc`
- Modify: `main/boards/smartmedivend-s3/smartmedivend_board.cc`
- Modify: `scripts/tests/run_smartmedivend_host_tests.ps1`
- Modify: `scripts/tests/run_smartmedivend_host_tests.sh`

**Interfaces:**
- Consumes: `MedicalPolicyCache`, `MedicalIntakeSession`, turn delta, monotonic milliseconds, and an injected microsecond clock.
- Produces: a structured `MedicalEvaluation` with exactly one next question for `ASK`, indexed fast-screen behavior, and at most one full evaluation per facts revision.

- [ ] **Step 1: Replace snapshot-oriented tests with failing two-stage tests**

Add these assertions before implementation:

```cpp
auto first = advisor.EvaluateTurn(
    R"({"session_id":"s","turn_id":1,"symptoms":["mild_headache"]})", 1000);
Check(first.decision == smv::MedicalDecision::kAsk, "missing facts did not ASK");
Check(!first.next_question_vi.empty(), "ASK dropped next_question_vi");

auto offer = CompleteMildHeadacheAsDeltas(advisor);
Check(offer.decision == smv::MedicalDecision::kOffer, "mild case did not offer");
Check(advisor.full_evaluation_count() == 1,
      "full rules were evaluated more than once for one facts revision");
Check(policy.artifact_parse_count() == 2, "static JSON reparsed during conversation");
```

Also test explicit danger, `refer_if`, `exclude_if` removing only one candidate, unrelated sentinel allergy not removing paracetamol, unknown token producing `ASK`, priority selection in a synthetic test-only policy, priority tie producing no offer, and a correction rerunning full evaluation exactly once.

- [ ] **Step 2: Run tests and verify the old API/behavior fails**

Run the medical host runner.

Expected: compilation or assertions fail because `EvaluateTurn`, structured questions, and two-stage counters do not exist.

- [ ] **Step 3: Define the structured result and advisor API**

Use:

```cpp
struct MedicalEvaluation {
    MedicalDecision decision = MedicalDecision::kDeny;
    std::string reason = "NOT_EVALUATED";
    std::string session_id;
    uint32_t turn_id = 0;
    uint32_t facts_revision = 0;
    std::string next_question_id;
    std::string next_question_vi;
    std::string missing_field;
    std::optional<MedicalOffer> offer;
    bool full_evaluation_ran = false;
    uint32_t fast_screen_us = 0;
    uint32_t full_evaluation_us = 0;
    std::string response_json;
};

class MedicalAdvisor {
public:
    using ClockUs = std::function<uint64_t()>;
    MedicalAdvisor(const MedicalPolicyCache& policy, ClockUs clock_us);
    MedicalEvaluation EvaluateTurn(std::string_view delta_json, uint64_t now_ms);
    void ResetSession();
    uint32_t facts_revision() const;
    uint32_t full_evaluation_count() const;
};
```

Keep `IntakeSchema()` and `SymptomGuide()` but render them from the cache. Remove raw `rules_`, `catalog_`, and `review_` storage.

Add one `std::unique_ptr<MedicalPolicyCache>` board member. Construct it before the advisor and pass
the same reference to `MedicalAdvisor`; keep the existing raw-JSON `CatalogRouter` construction
only until Task 5 removes its parser. Pass `esp_timer_get_time()` as the production microsecond
clock. Change the coordinator's existing evaluation call to
`advisor_.EvaluateTurn(patient_facts_json, now_ms)` and update its host fixture to construct the
cache/session-based advisor; response forwarding itself remains Task 5.

- [ ] **Step 4: Implement FastScreen with permissive uncertainty semantics**

Order checks exactly as the spec: input/session validity, explicit danger/hard population referral, missing always-required facts, indexed primary-symptom candidates, known `refer_if`, candidate-local `exclude_if`, then the highest-priority unanswered relevant symptom question.

Build derived bits deterministically from normalized facts (`weight_below_50kg`, `age_16_or_17`,
and `duration_over_48_hours`) before applying candidate predicates. Pregnancy/breastfeeding and age
below the approved scope remain explicit population referrals.

An explicit `danger_signs:[]` is the consolidated global danger confirmation; do not require all six legacy global questions after it. A `CLARIFY` mismatch and an unknown medical token return `ASK` with one Vietnamese question. Missing and silence never become false.

Use these bounded field questions when no policy question applies:

```cpp
{"primary_symptom", "Triệu chứng chính làm bạn khó chịu nhất là gì?"},
{"danger_signs", "Bạn có khó thở, đau ngực, ngất, co giật, chảy máu, đau dữ dội hoặc nặng lên nhanh không?"},
{"age_years", "Bạn bao nhiêu tuổi?"},
{"pregnancy_or_breastfeeding", "Bạn có đang mang thai hoặc cho con bú không?"},
{"duration_hours", "Triệu chứng này đã kéo dài khoảng bao lâu?"},
{"weight_kg", "Cân nặng hiện tại của bạn khoảng bao nhiêu ki-lô-gam?"},
{"conditions", "Bạn có bệnh nền nào đang điều trị không?"},
{"current_medicines", "Bạn đang dùng thuốc hoặc thực phẩm bổ sung nào không?"},
{"drug_allergies", "Bạn có dị ứng với thuốc nào không?"},
```

- [ ] **Step 5: Implement FullEvaluate and per-revision caching**

Evaluate only `policy.RulesForSymptom(primary)`. A `refer_if` produces `REFER`; an `exclude_if` removes that rule; another remaining rule may still win. Select one remaining rule only if it is unique or has a unique pharmacist-authored priority. A tie returns `REFER` unless the cache exposes a configured discriminator question, in which case return `ASK`.

Cache the full result by `{session_id, turn_id, facts_revision, rules_version, catalog_version}`. The evaluation returns a canonical ID and display identity only, never SKU/channel. Serialize `ASK` with `next_question_id`, `next_question_vi`, and `vend_allowed:false`.

- [ ] **Step 6: Add bounded timing instrumentation**

Measure fast and full stages with the injected `ClockUs`; do not log medical facts. Tests use a deterministic clock and assert one full-stage timing sample per facts revision. Board logging is added in Task 7.

- [ ] **Step 7: Run medical/vending suites and compile the development board**

Run both PowerShell host runners, then:

```powershell
$env:PYTHONUTF8='1'
$env:PATH='C:\Espressif\tools\python\v6.1\venv\Scripts;' + $env:PATH
. 'C:\esp\v6.1\esp-idf\export.ps1'
& 'C:\Espressif\python_env\idf6.1_py3.14_env\Scripts\python.exe' scripts/build.py smartmedivend-s3 --name smartmedivend-s3
```

Run the medical suite a second time after repeating a completed turn sequence.

Expected: both host runs and the board build pass; parse count stays two, incomplete mild cases
remain `ASK`, and completed mild cases return a structured offer.

- [ ] **Step 8: Commit the two-stage advisor**

```powershell
git add main/medical/medical_advisor.h main/medical/medical_advisor.cc main/medical/tests/test_medical_advisor.cc main/vending/vending_coordinator.cc main/vending/tests/test_vending_coordinator.cc main/boards/smartmedivend-s3/smartmedivend_board.cc scripts/tests/run_smartmedivend_host_tests.ps1 scripts/tests/run_smartmedivend_host_tests.sh
git diff --cached --check
git commit -m "feat: evaluate medical intake in two stages"
```

### Task 5: Share the policy with routing and preserve advisor responses in the coordinator

**Files:**
- Modify: `main/vending/catalog_router.h`
- Modify: `main/vending/catalog_router.cc`
- Modify: `main/vending/vending_coordinator.h`
- Modify: `main/vending/vending_coordinator.cc`
- Modify: `main/boards/smartmedivend-s3/smartmedivend_board.cc`
- Modify: `main/vending/tests/test_catalog_router.cc`
- Modify: `main/vending/tests/test_vending_coordinator.cc`
- Modify: `main/vending/tests/run_host_tests.ps1`
- Modify: `main/vending/tests/run_host_tests.sh`

**Interfaces:**
- Consumes: the shared `MedicalPolicyCache`, structured advisor evaluations, current stock, and transport/application gates.
- Produces: route lookup without JSON parsing, exact `next_question_vi` forwarding, a candidate bound to facts revision, and session reset on cancel/disconnect.

- [ ] **Step 1: Write failing coordinator response tests**

Assert:

```cpp
const std::string ask = coordinator.EvaluateAndStage(
    R"({"session_id":"s","turn_id":1,"symptoms":["mild_headache"]})", 1000);
Check(Field(ask, "status") == "ASK", "advisor ASK status replaced");
Check(!Field(ask, "next_question_vi").empty(), "coordinator dropped next question");
Check(Field(ask, "reason") != "PRODUCTION_DISABLED",
      "compile gate blocked conversation before an offer");
```

Add tests that any newer accepted fact invalidates a candidate; `OnDisconnected()` resets session and candidate; stale turns remain blocked; an operational failure after a medical offer exposes `medical_status:"OFFER"` plus a precise reason; and JSON never contains SKU/channel/stock.

- [ ] **Step 2: Run vending tests and verify failures**

Run the vending PowerShell runner.

Expected: the current coordinator loses `next_question_vi` and the current router constructor still reparses JSON.

- [ ] **Step 3: Make CatalogRouter a zero-parse policy view**

Change the constructor to:

```cpp
explicit CatalogRouter(const MedicalPolicyCache& policy) : policy_(policy) {}
```

`valid()` delegates to `policy.valid()`. `Select(canonical_id, stock)` retrieves the normalized `PolicyCatalogItem`, prefers stocked primary, then exact backup, and emits route data locally. Delete catalog/rule parsing helpers from `CatalogRouter`; policy validation now has one owner.

- [ ] **Step 4: Preserve structured non-offer responses**

For `ASK` and `REFER`, return `evaluation.response_json` directly after stale-turn checks. For `BLOCK`, render the structured advisor reason. Run production/review/router/inventory gates only after a medical offer exists.

Operational offer failures render:

```json
{"status":"BLOCK","medical_status":"OFFER","reason":"INVENTORY_UNAVAILABLE","vend_allowed":false}
```

Do not expose route identity. A staged candidate stores `facts_revision`; `Confirm()` requires it still equals `advisor_.facts_revision()`.

- [ ] **Step 5: Reset session and candidate on lifecycle events**

`Cancel()` and `OnDisconnected()` clear the candidate and call `advisor_.ResetSession()`. An active relay still follows the existing certain/uncertain cancellation path. Add `IsQuiescent()` for the maintenance service: true only with no candidate, no active transaction, and an idle relay.

Add `using RuntimeReady = std::function<bool()>` to the coordinator and recheck that callback while
staging an offer and again in `Confirm()`. Host fixtures and the board pass a lambda returning
`true` until Task 7 connects the maintenance reboot lock. Update the board to pass the shared policy
to `CatalogRouter` so this commit remains target-buildable.

- [ ] **Step 6: Run medical and vending suites**

Run both PowerShell host runners.

Expected: `next_question_vi` survives exactly, routing performs no cJSON parse, corrections/disconnects invalidate offers, and successful confirmation behavior remains unchanged.

- [ ] **Step 7: Commit the shared policy/coordinator integration**

```powershell
git add main/vending/catalog_router.h main/vending/catalog_router.cc main/vending/vending_coordinator.h main/vending/vending_coordinator.cc main/boards/smartmedivend-s3/smartmedivend_board.cc main/vending/tests/test_catalog_router.cc main/vending/tests/test_vending_coordinator.cc main/vending/tests/run_host_tests.ps1 main/vending/tests/run_host_tests.sh
git diff --cached --check
git commit -m "feat: preserve fast medical decisions through vending"
```

### Task 6: Add catalog-bound inventory v2 and explicit technician reconciliation

**Files:**
- Modify: `main/vending/vending_types.h`
- Modify: `main/inventory/inventory_store.h`
- Modify: `main/inventory/inventory_store.cc`
- Modify: `main/boards/smartmedivend-s3/smartmedivend_board.cc`
- Create: `main/inventory/inventory_maintenance.h`
- Create: `main/inventory/inventory_maintenance.cc`
- Modify: `main/vending/tests/test_inventory_store.cc`
- Modify: `main/vending/tests/test_vending_coordinator.cc`
- Create: `main/vending/tests/test_inventory_maintenance.cc`
- Modify: `main/vending/tests/run_host_tests.ps1`
- Modify: `main/vending/tests/run_host_tests.sh`
- Modify: `main/CMakeLists.txt`

**Interfaces:**
- Consumes: technician-counted 16-channel stock and the exact embedded catalog SHA-256.
- Produces: versioned catalog-bound NVS snapshots, explicit reconciliation, read-only status, and a reboot-required runtime lock.

- [ ] **Step 1: Write failing v2 inventory tests**

Test new keys `snapshot_v2_a`, `snapshot_v2_b`, and `head_v2`; 32-byte catalog digest;
mismatched digest rejection; refusal to provision over pending state; explicit reconcile over
corrupt/missing/pending state; exact 16 counts; 0–10000 range; dual-slot readback; a simulated reset
after each reconciliation write; and old `snapshot_a`/`snapshot_b` being ignored rather than
migrated.

Use:

```cpp
const smv::ArtifactSha256 digest = Digest(0x11);
smv::InventoryStore store(backend, digest);
Check(store.Provision(Counts(5)) == smv::InventoryResult::kOk,
      "v2 provisioning failed");
smv::InventoryStore wrong(backend, Digest(0x22));
Check(wrong.Load() == smv::InventoryResult::kCatalogMismatch,
      "stock crossed catalog identity");
Check(wrong.Reconcile(Counts(3), digest) == smv::InventoryResult::kCatalogMismatch,
      "wrong command digest reconciled stock");
```

- [ ] **Step 2: Run inventory tests and verify v2 failures**

Run the vending host runner.

Expected: compile fails on `ArtifactSha256`, `kCatalogMismatch`, or the new constructor.

- [ ] **Step 3: Implement versioned snapshot keys and payload**

Add:

```cpp
using ArtifactSha256 = std::array<uint8_t, 32>;

class InventoryStore {
public:
    InventoryStore(InventoryBackend& backend, ArtifactSha256 catalog_sha256);
    InventoryResult Provision(const std::array<uint32_t, 16>& counts,
                              uint16_t valid_mask = 0xffff);
    InventoryResult Reconcile(const std::array<uint32_t, 16>& counts,
                              const ArtifactSha256& command_catalog_sha256,
                              uint16_t valid_mask = 0xffff);
};
```

Add `InventoryResult::kCatalogMismatch` and `InventoryResult::kMaintenanceLocked`. Extend
`StockSnapshot` with the pending transaction ID/channel needed by the local read-only status command;
do not expose those fields through MCP responses.

Encode magic `SMV2`, schema 2, catalog digest, revision, counts, mask, pending transaction, last
outcome, and CRC32. `head_v2` has its own CRC and contains either the committed active slot/revision
or `LOCKED_RECONCILE`. Normal copy-on-write writes and verifies the inactive snapshot, then commits
the head last. `Provision` preserves fail-closed pending behavior.

`Reconcile` first commits `LOCKED_RECONCILE`, then writes and reads back both fresh physically
counted snapshots, then commits the final head. Reset or write failure at any intermediate step
leaves the head locked, so boot cannot select a partially reconciled snapshot. Only another explicit
reconciliation recovers it. Read only v2 keys; leave v1 keys untouched and never use them as current
stock.

Update the board and coordinator fixture to parse/use the same catalog digest and pass it to the new
`InventoryStore` constructor so the intermediate firmware and host suite still compile.

- [ ] **Step 4: Write and implement the pure maintenance service**

Define a parser/service independent of ESP console:

```cpp
struct MaintenanceResult {
    bool ok = false;
    bool reboot_required = false;
    std::string code;
};

class InventoryMaintenance {
public:
    InventoryMaintenance(InventoryStore& inventory,
                         ArtifactSha256 catalog_sha256,
                         std::function<bool()> is_quiescent,
                         std::function<void()> enter_maintenance_lock);
    MaintenanceResult Execute(std::span<const std::string_view> args);
    std::string Status() const;
};
```

Support only `status`, `provision --catalog-sha256 HEX --counts CSV`, and
`reconcile --catalog-sha256 HEX --counts CSV`. Mutation requires exact digest, exactly 16 decimal
counts, quiescent vending, and sets the maintenance lock before writing. It returns
`REBOOT_REQUIRED` after verified success.

- [ ] **Step 5: Test pending/corrupt recovery and runtime lock**

Add tests for malformed hex, wrong digest, 15/17 counts, overflow, active candidate/relay, status redaction, successful provision, successful pending reconcile, write failure, and all subsequent mutation/vend attempts blocked until reboot.

- [ ] **Step 6: Run all vending host tests**

Expected: inventory v2, maintenance service, coordinator, route, and relay suites pass.

- [ ] **Step 7: Commit inventory v2 and maintenance core**

```powershell
git add main/vending/vending_types.h main/inventory/inventory_store.h main/inventory/inventory_store.cc main/inventory/inventory_maintenance.h main/inventory/inventory_maintenance.cc main/boards/smartmedivend-s3/smartmedivend_board.cc main/vending/tests/test_inventory_store.cc main/vending/tests/test_vending_coordinator.cc main/vending/tests/test_inventory_maintenance.cc main/vending/tests/run_host_tests.ps1 main/vending/tests/run_host_tests.sh main/CMakeLists.txt
git diff --cached --check
git commit -m "feat: reconcile catalog-bound vending inventory"
```

### Task 7: Add the production variant, USB maintenance console, approval tool, and board wiring

**Files:**
- Create: `main/boards/smartmedivend-s3/smartmedivend_maintenance_console.h`
- Create: `main/boards/smartmedivend-s3/smartmedivend_maintenance_console.cc`
- Modify: `main/boards/smartmedivend-s3/smartmedivend_board.cc`
- Modify: `main/boards/smartmedivend-s3/smartmedivend_display.h`
- Modify: `main/boards/smartmedivend-s3/smartmedivend_display.cc`
- Modify: `main/boards/smartmedivend-s3/config.json`
- Modify: `main/CMakeLists.txt`
- Create: `scripts/prepare_smartmedivend_approval.py`
- Create: `scripts/tests/test_prepare_smartmedivend_approval.py`
- Create: `scripts/tests/test_smartmedivend_two_stage_wiring.py`
- Modify: `data/pharmacist_review.json` (generated empty-real-identity template only)

**Interfaces:**
- Consumes: generated artifact identity, shared policy, local NVS, physical button, USB Serial/JTAG console, and real pharmacist metadata supplied outside this implementation.
- Produces: explicit production build selection, local stock commands, accurate readiness UI/logging, and complete board composition with no cloud control path.

- [ ] **Step 1: Write failing static build/wiring tests**

Assert `config.json` has exactly these names and flags:

```python
self.assertEqual([b["name"] for b in builds],
                 ["smartmedivend-s3", "smartmedivend-s3-production"])
self.assertNotIn("CONFIG_SMARTMEDIVEND_PRODUCTION_VENDING=y", builds[0]["sdkconfig_append"])
self.assertIn("CONFIG_SMARTMEDIVEND_PRODUCTION_VENDING=y", builds[1]["sdkconfig_append"])
self.assertIn("CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y", builds[1]["sdkconfig_append"])
```

Also assert one `DECLARE_BOARD`, cache/session/maintenance sources are in the SmartMediVend CMake branch, no MCP tool name contains vend/relay/inventory/channel, board constructs one shared policy, and the advisor tool description requests turn deltas.

- [ ] **Step 2: Write failing approval-tool tests**

Run the script on temporary rule/catalog files and assert exact `hashlib.sha256(path.read_bytes()).hexdigest()` values, copied versions, `approved:false`, and empty reviewer/license/time. Reject malformed JSON or missing version fields. The script must not overwrite a nonempty output unless `--force` is passed.

- [ ] **Step 3: Run Python tests and confirm missing feature failures**

Run:

```powershell
& 'C:\Espressif\python_env\idf6.1_py3.14_env\Scripts\python.exe' -m unittest scripts.tests.test_prepare_smartmedivend_approval scripts.tests.test_smartmedivend_two_stage_wiring -v
```

Expected: failures because the variant, console, and approval script are absent.

- [ ] **Step 4: Add the explicit production build variant**

Append this build without changing the development entry:

```json
{
  "name": "smartmedivend-s3-production",
  "sdkconfig_append": [
    "CONFIG_SMARTMEDIVEND_PRODUCTION_VENDING=y",
    "CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y"
  ]
}
```

Keep the same board type and exactly one board factory.

- [ ] **Step 5: Implement the USB Serial/JTAG console adapter**

Register one `esp_console_cmd_t` named `smv_inventory`. Its handler converts `argc/argv` to string views, invokes `InventoryMaintenance::Execute`, prints only bounded status/result codes, and never exposes an MCP or network entry point. Mutation is unavailable in the development variant.

The board retains the console object for firmware lifetime. A successful mutation sets `maintenance_lock_`, clears the coordinator/session, keeps relay HIGH, updates the display to `Cấp thuốc: CẦN KHỞI ĐỘNG LẠI`, and requires reboot before MCP evaluation or confirmation.

- [ ] **Step 6: Compose the one-cache two-stage board services**

Construct in this order after GPIO is safe:

```cpp
policy_ = std::make_unique<smv::MedicalPolicyCache>(
    smv::kMedicalRulesJson, smv::kMedicineCatalogJson);
advisor_ = std::make_unique<smv::MedicalAdvisor>(
    *policy_, [] { return static_cast<uint64_t>(esp_timer_get_time()); });
catalog_router_ = std::make_unique<smv::CatalogRouter>(*policy_);
inventory_ = std::make_unique<smv::InventoryStore>(
    *inventory_backend_, ParseArtifactSha256(smv::kMedicineCatalogSha256));
```

Update coordinator construction to use these objects and the maintenance runtime gate. Update `evaluate_symptoms` help to send only new/corrected facts after turn 1. Log only input byte count, fast/full microseconds, full-stage flag, and outcome reason.

- [ ] **Step 7: Make the status display report real readiness**

Add:

```cpp
void SmartMediVendDisplay::SetVendingStatus(const char* status_vi);
```

Store a bounded copy under the display lock and show it on the status page. Board readiness chooses one reason in priority order: development disabled, policy invalid, approval invalid, inventory unavailable/pending, relay unavailable, maintenance reboot required, transport disconnected, or ready. Remove the hard-coded always-locked text.

- [ ] **Step 8: Implement the approval-template generator**

The Python script takes `--rules`, `--catalog`, and `--output`, emits sorted UTF-8 JSON with exact versions/hashes, `approved:false`, and empty real-identity fields. Generate `data/pharmacist_review.json` from the final bytes, but do not mark it approved and do not invent reviewer data.

Run:

```powershell
& 'C:\Espressif\python_env\idf6.1_py3.14_env\Scripts\python.exe' scripts/prepare_smartmedivend_approval.py --rules data/medical_rules.json --catalog data/medicines.json --output data/pharmacist_review.json --force
```

- [ ] **Step 9: Run static, approval, host, and both build variants**

Run:

```powershell
& 'C:\Espressif\python_env\idf6.1_py3.14_env\Scripts\python.exe' -m unittest scripts.tests.test_prepare_smartmedivend_approval scripts.tests.test_smartmedivend_two_stage_wiring -v
powershell -ExecutionPolicy Bypass -File scripts/tests/run_smartmedivend_host_tests.ps1
powershell -ExecutionPolicy Bypass -File main/vending/tests/run_host_tests.ps1
$env:PYTHONUTF8='1'
$env:PATH='C:\Espressif\tools\python\v6.1\venv\Scripts;' + $env:PATH
. 'C:\esp\v6.1\esp-idf\export.ps1'
& 'C:\Espressif\python_env\idf6.1_py3.14_env\Scripts\python.exe' scripts/build.py smartmedivend-s3 --name smartmedivend-s3
& 'C:\Espressif\python_env\idf6.1_py3.14_env\Scripts\python.exe' scripts/build.py smartmedivend-s3 --name smartmedivend-s3-production
```

Expected: both builds pass; development reports compile gate off; production reports compile gate on but approval invalid until real pharmacist fields are supplied.

- [ ] **Step 10: Commit the production/board tooling**

Stage only Task 7 files and run:

```powershell
git diff --cached --check
git commit -m "feat: enable serviced SmartMediVend production builds"
```

### Task 8: Complete regression coverage, documentation, and release verification

**Files:**
- Modify: `docs/smartmedivend/FAIL_CLOSED_VENDING_TEST.md`
- Modify: `docs/smartmedivend/XIAOZHI_CLOUD_ROLE_STAGE1.md`
- Modify: `docs/smartmedivend/STAGE1_INTEGRATION_AND_TEST.md`
- Create: `docs/smartmedivend/TWO_STAGE_MEDICAL_FLOW.md`

**Interfaces:**
- Consumes: completed firmware, real approval fields when available, technician-counted stock, and hardware measurements.
- Produces: reproducible operator instructions and evidence that separates automated, electrical, and physical-dispense verification.

- [ ] **Step 1: Document the exact conversational and operational flow**

Document turn deltas, `ASK/REFER/BLOCK/OFFER`, one-question speech behavior, production build command, approval-template command, real pharmacist fields, USB inventory commands, v2 NVS keys, pending reconciliation, 30-second confirmation, and button behavior. State that current `approved:false` intentionally prevents production relay use until a pharmacist signs the final bytes.

- [ ] **Step 2: Add an end-to-end host scenario**

Extend the coordinator test with this complete sequence:

```cpp
SendDelta(symptom_only);          // ASK primary safety fact
SendDelta(explicit_no_danger);    // ASK demographic fact
SendRemainingSafeFacts();         // ASK relevant symptom question
SendExpectedSymptomAnswer();      // OFFER
PressPhysicalButton();            // reserve primary, start relay
CompletePulse();                  // count decremented exactly once
```

Repeat with primary count zero and backup count one. Add negative sequences for explicit danger, unrelated sentinel allergy, candidate exclusion with an alternative, correction after offer, disconnect after offer, pending inventory, and maintenance reboot lock.

- [ ] **Step 3: Run source-safety scans**

Run:

```powershell
rg -n "AddTool.*(vend|relay|inventory|channel)|initial_stock.*(Provision|Reserve)|vTaskDelay|delay\(" main/boards/smartmedivend-s3 main/medical main/vending main/inventory
rg -n "cJSON_Parse" main/medical main/vending
```

Expected: no cloud control tool, no initial-stock seeding, no blocking delay in the new path, and cJSON parsing confined to policy construction, turn-delta parsing, approval verification, and response/test helpers—not per-turn static-artifact parsing.

- [ ] **Step 4: Format touched C/C++ files and verify formatting**

Use:

```powershell
$formatFiles = @(git diff --name-only 6077680..HEAD -- '*.cc' '*.h') | Where-Object { $_ }
if ($formatFiles.Count -eq 0) { throw 'No touched C++ files found' }
& 'C:\Espressif\tools\esp-clang\esp-21.1.3_20260408\esp-clang\bin\clang-format.exe' -i @formatFiles
& 'C:\Espressif\tools\esp-clang\esp-21.1.3_20260408\esp-clang\bin\clang-format.exe' --dry-run -Werror @formatFiles
git add -- @formatFiles
git diff --cached --check
git diff --cached --quiet
if ($LASTEXITCODE -eq 1) {
    git commit -m "style: format SmartMediVend two-stage changes"
} elseif ($LASTEXITCODE -ne 0) {
    throw "git diff --cached --quiet failed: $LASTEXITCODE"
}
```

The range starts at the approved-spec commit, so only C++ files touched by this implementation are
formatted; unrelated files remain untouched.

- [ ] **Step 5: Run the full automated verification set**

Run medical tests, vending tests, all Python `scripts/tests`, and both ESP-IDF 6.1 SmartMediVend variants. Then run:

```powershell
git diff --check
git status --short
```

Expected: every runnable test and both builds pass. Report any unavailable generic test separately with its exact environment error; do not convert an infrastructure failure into a pass.

- [ ] **Step 6: Perform no-load electrical verification**

With no medicine/load connected and a controlled production image using non-clinical test approval/stock only:

1. Verify GPIO17 remains HIGH through reset, policy/approval/inventory failures, disconnect, and cancel.
2. Verify selector stability for at least 10 ms before LOW.
3. Measure a 500 ms LOW pulse followed by at least 100 ms guard.
4. Disconnect MQTT and WebSocket separately before confirmation and confirm no pulse.
5. Expire/cancel timers at settle, pulse, and guard boundaries and confirm no stale extra pulse.
6. Provision counts over USB, reboot, test primary then backup route, and verify one count decrement per confirmed command.
7. Interrupt power during a pulse, confirm pending lock on reboot, physically count stock, reconcile, and reboot before another vend.

Record logic-analyzer timing separately from package-delivery evidence. Without a drop/current sensor, retain `COMMAND_SENT_UNVERIFIED`.

- [ ] **Step 7: Commit documentation and any scoped regression fix**

```powershell
git add docs/smartmedivend/FAIL_CLOSED_VENDING_TEST.md docs/smartmedivend/XIAOZHI_CLOUD_ROLE_STAGE1.md docs/smartmedivend/STAGE1_INTEGRATION_AND_TEST.md docs/smartmedivend/TWO_STAGE_MEDICAL_FLOW.md
git diff --cached --check
git commit -m "docs: verify two-stage SmartMediVend operation"
```

- [ ] **Step 8: Run the completion gate and final review**

Invoke `superpowers:verification-before-completion`, rerun the fresh automated commands whose outputs support the final claims, inspect all commits/diffs, and invoke `superpowers:requesting-code-review`. Fix confirmed findings with focused tests and commits, then rerun affected suites and both board builds.

## Completion Gate

Implementation is code-complete when the development and production variants build, all available host/static tests pass, static artifacts parse once, mild complete cases reach a locally staged offer, `next_question_vi` is preserved, local stock can be reconciled, and no race/error path creates a stray LOW transition. Production medicine dispensing is operationally releasable only after real pharmacist metadata approves the final exact artifact hashes, a technician provisions counted stock, and the physical timing/pull-up checks are recorded.
