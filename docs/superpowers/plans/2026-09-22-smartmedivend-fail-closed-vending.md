# SmartMediVend Fail-Closed Vending Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement a non-blocking, single-SKU medication vending path in which local ESP32 policy selects stock and relay channel, while incomplete pharmacist approval, NVS state, confirmation, or hardware state always blocks the relay.

**Architecture:** Keep medical evaluation, approval verification, catalog routing, inventory persistence, final authorization, and board GPIO timing as separate bounded components. The application task owns the candidate and transaction lifecycle; timer callbacks only perform bounded GPIO/state work and schedule completion back to the application task. Two compile/runtime gates and durable pre-pulse inventory reservation ensure that an AI message, reboot, or interrupted NVS write cannot cause an unapproved or duplicate pulse.

**Tech Stack:** ESP-IDF 6.1, C++23 firmware, cJSON, ESP NVS, `esp_timer`, GPIO, CMake/Kconfig, standalone C++ host tests with fake NVS/timer/GPIO.

**Spec:** `docs/superpowers/specs/2026-09-22-smartmedivend-fail-closed-vending-design.md`

## Global Constraints

- Use ESP-IDF 6.1 when possible; minimum supported SDK is 6.0.1; IDF 5.x is unsupported.
- Compile all new vending sources only for `CONFIG_BOARD_TYPE_SMARTMEDIVEND_S3`; do not alter another board's pins or factory.
- Preserve the existing dirty worktree. In particular, do not stage or overwrite the deleted ZIP, the user's review-file change, or untracked bench simulator/documentation unless a task explicitly names that file.
- The AI may submit patient facts only. It cannot select or submit SKU, channel, stock, quantity, authorization, or relay commands.
- Each transaction may offer one canonical item and reserve/dispense one unit only.
- Production vending requires both `CONFIG_SMARTMEDIVEND_PRODUCTION_VENDING=y` and a valid review bound to exact rule/catalog versions and SHA-256 hashes; the Kconfig default is `n`.
- Inventory is valid only from NVS namespace `smv_inventory`, keys `snapshot_a` and `snapshot_b`; never seed it from catalog `initial_stock`.
- Hardware is CD74HC4067 with S0-S3 on GPIO39-42 and active-LOW signal on GPIO17; GPIO17 must remain HIGH except during the bounded pulse.
- Arm the 500 ms shutoff timer before driving GPIO17 LOW; use 10 ms selector settling and a 100 ms all-off guard interval.
- No blocking waits, unbounded queues, or repeated large allocations in application, audio, GPIO, or timer paths.
- Timer/button/protocol callbacks schedule application mutations through `Application::Schedule()` or event bits.
- Do not fabricate pharmacist identity, license, timestamp, hashes, approval, medicine strength, or live inventory.
- Format only touched C/C++ files with the repository `.clang-format`.

## Review Focus

- Power loss at each point between inventory reservation, LOW edge, HIGH edge, and completion commit must never replay a pulse; Task 4 and Task 6 pin each state with restart tests.
- Timer arming failure or a stale timer callback must leave GPIO17 HIGH and must not energize the relay; Task 6 tests both failure classes.
- Malformed/duplicate JSON and AI-supplied `sku`, `channel`, `stock`, `quantity`, or `vend_allowed` fields must be rejected; Task 1 tests every forbidden class.
- Equal-revision divergent NVS snapshots, CRC failure, revision exhaustion, and pending transactions must lock inventory; Task 4 tests all four.
- A stale session turn, expired confirmation, double click, reconnect/state change, or click while the application is not idle must not vend; Task 5 and Task 7 test or exercise these transitions.

---

## File Structure

- `main/medical/medical_advisor.*`: validated interview plus a structured internal decision; still no stock or relay access.
- `main/medical/pharmacist_review_verifier.*`: exact approval metadata/version/hash verification.
- `main/vending/vending_types.h`: shared value types and the narrow relay interface.
- `main/vending/catalog_router.*`: immutable catalog validation and primary/backup selection.
- `main/inventory/inventory_store.*`: pure dual-slot snapshot logic and durable reservation lifecycle.
- `main/inventory/nvs_inventory_backend.*`: bounded ESP NVS adapter for `snapshot_a`/`snapshot_b`.
- `main/vending/vend_guard.*`: final side-effect-free authorization checks.
- `main/vending/vending_coordinator.*`: candidate, physical confirmation, reservation, relay outcome, and safe JSON response lifecycle on the application task.
- `main/boards/smartmedivend-s3/relay_driver.*`: platform-independent relay state machine with an injected timer/GPIO platform.
- `main/boards/smartmedivend-s3/esp_relay_platform.*`: GPIO17/GPIO39-42 and `esp_timer` adapter.
- `main/boards/smartmedivend-s3/smartmedivend_board.cc`: board-only composition, MCP assessment callback, physical button confirmation, and UI notifications.
- `main/vending/tests/*`: host fakes and focused policy/persistence/timing tests.
- `docs/smartmedivend/FAIL_CLOSED_VENDING_TEST.md`: provisioning, locked-default behavior, and hardware verification procedure.

### Task 1: Return a structured local medical decision

**Files:**
- Modify: `main/medical/medical_advisor.h`
- Modify: `main/medical/medical_advisor.cc`
- Modify: `main/medical/tests/test_medical_advisor.cc`
- Create: `scripts/tests/run_smartmedivend_host_tests.sh`

**Interfaces:**
- Consumes: embedded rule and catalog JSON already passed to `MedicalAdvisor`.
- Produces: `MedicalEvaluation MedicalAdvisor::EvaluateStructured(const std::string&) const`; existing `Evaluate()` renders the same evaluation for compatibility.

- [ ] **Step 1: Add failing tests for the internal offer and forbidden control fields**

Add assertions around these exact public types and behaviors:

```cpp
const smv::MedicalEvaluation evaluation = advisor.EvaluateStructured(CompleteHeadacheSnapshot());
Check(evaluation.decision == smv::MedicalDecision::kOffer, "one local option must be structured");
Check(evaluation.offer.has_value(), "offer value missing");
Check(evaluation.offer->canonical_id == "PARACETAMOL_500", "wrong canonical item");
Check(evaluation.response_json.find("\"sku\"") == std::string::npos, "SKU leaked to AI");
Check(evaluation.response_json.find("\"channel\"") == std::string::npos, "channel leaked to AI");

for (const char* forbidden : {"sku", "channel", "stock", "quantity", "vend_allowed"}) {
    const auto result = advisor.EvaluateStructured(SnapshotWithExtraField(forbidden));
    Check(result.decision == smv::MedicalDecision::kDeny, "forbidden control field accepted");
    Check(result.reason == "FORBIDDEN_OR_UNKNOWN_FIELD", "wrong forbidden-field reason");
}
```

Also assert that the two-candidate diarrhoea fixture returns `kRefer`, and a catalog `initial_stock` value of zero does not remove an otherwise eligible medical candidate.

- [ ] **Step 2: Run the medical host test and confirm the new API is absent**

Run:

```bash
bash scripts/tests/run_smartmedivend_host_tests.sh
```

Expected: compilation fails because `MedicalEvaluation`, `MedicalDecision`, and `EvaluateStructured` are not defined.

- [ ] **Step 3: Add the structured types and refactor one evaluation path**

Add these exact public types to `medical_advisor.h`:

```cpp
enum class MedicalDecision { kDeny, kAsk, kRefer, kOffer };

struct MedicalOffer {
    std::string canonical_id;
    std::string name;
    std::string active_ingredient;
    std::string strength;
};

struct MedicalEvaluation {
    MedicalDecision decision = MedicalDecision::kDeny;
    std::string reason = "NOT_EVALUATED";
    std::string session_id;
    uint32_t turn_id = 0;
    std::optional<MedicalOffer> offer;
    std::string response_json;
};
```

Include `<cstdint>` and `<optional>`. Refactor the evaluator so every exit constructs a `MedicalEvaluation` through bounded helpers instead of parsing its own rendered JSON. Map `DENY` to `kDeny`, `NEED_MORE_INFO` to `kAsk`, `REFER` to `kRefer`, and one eligible candidate to `kOffer`. Keep `vend_allowed=false` in every rendered MCP response and remove all use of `initial_stock` from medical eligibility.

Implement compatibility as:

```cpp
std::string MedicalAdvisor::Evaluate(const std::string& input) const {
    return EvaluateStructured(input).response_json;
}
```

Create the host runner with bounded temporary output and the existing host-only cJSON ABI shim:

```bash
#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
CXX="${CXX:-g++}"
FLAGS=(-std=c++23 -Wall -Wextra -Werror -Imain/medical/tests/host_include \
  -Imain -Imain/medical -Imain/vending -Imain/inventory \
  -Imain/boards/smartmedivend-s3)
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT
"$CXX" "${FLAGS[@]}" main/medical/medical_advisor.cc \
  main/medical/tests/test_medical_advisor.cc -Wl,-l:libcjson.so.1 \
  -o "$TMP_DIR/medical_advisor_test"
"$TMP_DIR/medical_advisor_test" data/medical_rules.json data/medicines.json \
  data/pharmacist_review.json
```

- [ ] **Step 4: Run the medical host test**

Run `bash scripts/tests/run_smartmedivend_host_tests.sh`.

Expected: all medical advisor assertions pass, including multiple-option referral, forbidden fields, and no SKU/channel leakage.

- [ ] **Step 5: Commit only Task 1 hunks**

Because both medical source and test files were dirty before this plan, inspect `git diff` and use interactive staging so earlier user changes remain unstaged:

```bash
git add -p -- main/medical/medical_advisor.h main/medical/medical_advisor.cc main/medical/tests/test_medical_advisor.cc
git add scripts/tests/run_smartmedivend_host_tests.sh
git diff --cached --check
git commit -m "refactor: expose structured medical decisions"
```

### Task 2: Bind pharmacist approval to exact embedded artifacts

**Files:**
- Create: `main/medical/pharmacist_review_verifier.h`
- Create: `main/medical/pharmacist_review_verifier.cc`
- Create: `main/medical/tests/test_pharmacist_review_verifier.cc`
- Modify: `main/medical/medical_data_generated.h.in`
- Modify: `main/CMakeLists.txt`
- Modify: `scripts/tests/run_smartmedivend_host_tests.sh`

**Interfaces:**
- Consumes: review JSON and build-generated rule/catalog version plus SHA-256 identity.
- Produces: `ReviewResult PharmacistReviewVerifier::Verify(std::string_view, const ArtifactIdentity&)`.

- [ ] **Step 1: Write failing approval tests**

Use the exact API below and cover valid synthetic approval, `approved=false`, missing/null reviewer fields, wrong version, wrong hash, non-lowercase/non-64-byte hash, and malformed RFC 3339 time:

```cpp
const smv::ArtifactIdentity identity{
    .rules_version = "rules-v1",
    .catalog_version = "catalog-v1",
    .rules_sha256 = std::string(64, 'a'),
    .catalog_sha256 = std::string(64, 'b'),
};
const auto valid = smv::PharmacistReviewVerifier::Verify(ValidReviewJson(), identity);
Check(valid.valid, "complete matching review must pass");
Check(!smv::PharmacistReviewVerifier::Verify(ReviewWithoutLicense(), identity).valid,
      "missing license accepted");
Check(!smv::PharmacistReviewVerifier::Verify(ReviewWithWrongHash(), identity).valid,
      "wrong artifact hash accepted");
```

- [ ] **Step 2: Run the verifier test and see the missing-header failure**

Run:

```bash
bash scripts/tests/run_smartmedivend_host_tests.sh
```

Expected: compilation fails on `medical/pharmacist_review_verifier.h`.

- [ ] **Step 3: Implement strict review verification**

Define:

```cpp
struct ArtifactIdentity {
    std::string rules_version;
    std::string catalog_version;
    std::string rules_sha256;
    std::string catalog_sha256;
};

enum class ReviewFailure {
    kNone,
    kInvalidJson,
    kUnsupportedSchema,
    kNotApproved,
    kMissingReviewer,
    kMissingLicense,
    kInvalidReviewedAt,
    kVersionMismatch,
    kHashMismatch,
};

struct ReviewResult {
    bool valid = false;
    ReviewFailure failure = ReviewFailure::kInvalidJson;
};
```

Accept schema version `1`, exact boolean `approved=true`, non-empty reviewer and license capped at 96 bytes, strict `YYYY-MM-DDTHH:MM:SSZ` or numeric-offset RFC 3339 syntax, exact versions, and lowercase 64-hex hashes. Reject unknown JSON fields so a misspelled approval field cannot be silently ignored.

Append this independent test compile/run pair to `scripts/tests/run_smartmedivend_host_tests.sh`:

```bash
"$CXX" "${FLAGS[@]}" main/medical/pharmacist_review_verifier.cc \
  main/medical/tests/test_pharmacist_review_verifier.cc -Wl,-l:libcjson.so.1 \
  -o "$TMP_DIR/pharmacist_review_test"
"$TMP_DIR/pharmacist_review_test"
```

- [ ] **Step 4: Generate and embed SHA-256 identities**

In the SmartMediVend CMake branch add:

```cmake
file(SHA256 "${SMV_RULES_FILE}" SMV_RULES_SHA256)
file(SHA256 "${SMV_CATALOG_FILE}" SMV_CATALOG_SHA256)
string(JSON SMV_RULES_VERSION GET "${SMV_RULES_JSON}" rules_version)
string(JSON SMV_CATALOG_VERSION GET "${SMV_CATALOG_JSON}" catalog_version)
```

Extend `medical_data_generated.h.in` with:

```cpp
inline constexpr char kMedicalRulesSha256[] = "@SMV_RULES_SHA256@";
inline constexpr char kMedicineCatalogSha256[] = "@SMV_CATALOG_SHA256@";
inline constexpr char kMedicalRulesVersion[] = "@SMV_RULES_VERSION@";
inline constexpr char kMedicineCatalogVersion[] = "@SMV_CATALOG_VERSION@";
```

Do not modify or populate `data/pharmacist_review.json`; the current incomplete record must remain invalid.

- [ ] **Step 5: Run approval and existing medical tests**

Run `bash scripts/tests/run_smartmedivend_host_tests.sh`.

Expected: verifier tests pass; current repository review is rejected as incomplete; existing advisor tests still pass.

- [ ] **Step 6: Commit verifier sources and generated-input changes**

```bash
git add main/medical/pharmacist_review_verifier.h main/medical/pharmacist_review_verifier.cc main/medical/tests/test_pharmacist_review_verifier.cc main/medical/medical_data_generated.h.in main/CMakeLists.txt
git add -p -- scripts/tests/run_smartmedivend_host_tests.sh
git diff --cached --check
git commit -m "feat: verify pharmacist artifact approval"
```

### Task 3: Validate the catalog and select primary/backup stock locally

**Files:**
- Create: `main/vending/vending_types.h`
- Create: `main/vending/catalog_router.h`
- Create: `main/vending/catalog_router.cc`
- Create: `main/vending/tests/test_catalog_router.cc`
- Create: `main/vending/tests/run_host_tests.sh`

**Interfaces:**
- Consumes: catalog JSON, canonical ID from `MedicalEvaluation`, and a read-only `StockSnapshot`.
- Produces: immutable `RouteSelection`; never accepts SKU or channel as untrusted input.

- [ ] **Step 1: Write failing routing tests**

Cover primary stocked, primary empty/backup stocked, both empty, unknown inventory, duplicate channel, duplicate primary, multiple backups, wrong backup reference, and mismatched name/ingredient/strength. Include the repository antacid entry and assert catalog validation fails with `BACKUP_IDENTITY_MISMATCH`.

```cpp
smv::StockSnapshot stock;
stock.available = true;
stock.revision = 7;
stock.valid_mask = 0xffff;
stock.counts[0] = 2;
stock.counts[13] = 3;

const smv::CatalogRouter router(CorrectedFixtureCatalog());
const auto primary = router.Select("PARACETAMOL_500", stock);
Check(primary.status == smv::RouteStatus::kReady && primary.channel == 0,
      "primary route not selected");
stock.counts[0] = 0;
const auto backup = router.Select("PARACETAMOL_500", stock);
Check(backup.status == smv::RouteStatus::kReady && backup.channel == 13,
      "backup route not selected");
Check(backup.inventory_revision == 7, "route lost inventory revision");
```

- [ ] **Step 2: Run the new host runner and confirm missing types**

Run `bash main/vending/tests/run_host_tests.sh`.

Expected: compilation fails because `CatalogRouter` and `StockSnapshot` are absent.

- [ ] **Step 3: Implement bounded catalog parsing and route selection**

Define the shared types:

```cpp
inline constexpr size_t kVendingChannelCount = 16;

struct StockSnapshot {
    bool available = false;
    uint32_t revision = 0;
    std::array<uint32_t, kVendingChannelCount> counts{};
    uint16_t valid_mask = 0;
    bool transaction_pending = false;
};

enum class RouteStatus { kReady, kInvalidCatalog, kUnknownItem, kInventoryUnavailable, kOutOfStock };

struct RouteSelection {
    RouteStatus status = RouteStatus::kInvalidCatalog;
    std::string reason = "NOT_EVALUATED";
    std::string canonical_id;
    std::string sku;
    std::string name;
    std::string active_ingredient;
    std::string strength;
    uint32_t inventory_revision = 0;
    uint8_t channel = 0xff;
    bool backup = false;
};

struct ReservationToken {
    uint64_t transaction_id = 0;
    uint32_t reserved_revision = 0;
    uint8_t channel = 0xff;
};
```

Expose `bool valid() const`, `const std::string& validation_reason() const`, and `RouteSelection Select(std::string_view canonical_id, const StockSnapshot&) const`. `CatalogRouter` validates at most 16 unique channels in range 0-15, one primary and at most one backup per canonical ID, exact backup reference, and exact canonical/name/ingredient/strength identity. `Select()` takes only canonical ID and stock snapshot; it never takes an AI SKU/channel. Ignore `initial_stock` entirely.

The new vending runner compiles this task independently:

```bash
#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
CXX="${CXX:-g++}"
FLAGS=(-std=c++23 -Wall -Wextra -Werror -Imain/medical/tests/host_include \
  -Imain -Imain/medical -Imain/vending -Imain/inventory \
  -Imain/boards/smartmedivend-s3)
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT
"$CXX" "${FLAGS[@]}" main/vending/catalog_router.cc \
  main/vending/tests/test_catalog_router.cc -Wl,-l:libcjson.so.1 \
  -o "$TMP_DIR/catalog_router_test"
"$TMP_DIR/catalog_router_test" data/medicines.json
```

- [ ] **Step 4: Run catalog tests**

Run `bash main/vending/tests/run_host_tests.sh`.

Expected: all routing and invalid-catalog assertions pass, including the current antacid mismatch lock.

- [ ] **Step 5: Commit the routing unit**

```bash
git add main/vending/vending_types.h main/vending/catalog_router.h main/vending/catalog_router.cc main/vending/tests/test_catalog_router.cc main/vending/tests/run_host_tests.sh
git diff --cached --check
git commit -m "feat: route approved items from local stock"
```

### Task 4: Persist inventory and pre-pulse reservations in dual NVS slots

**Files:**
- Create: `main/inventory/inventory_store.h`
- Create: `main/inventory/inventory_store.cc`
- Create: `main/inventory/nvs_inventory_backend.h`
- Create: `main/inventory/nvs_inventory_backend.cc`
- Create: `main/vending/tests/test_inventory_store.cc`
- Modify: `main/vending/tests/run_host_tests.sh`

**Interfaces:**
- Consumes: bounded blob backend, technician counts, transaction ID, channel, expected inventory revision.
- Produces: `StockSnapshot`, `ReservationToken`, and explicit load/reserve/complete/cancel results.

- [ ] **Step 1: Write failing fake-backend inventory tests**

Test missing snapshots, valid provisioning, newest valid slot, one corrupt slot fallback, two equal revisions with different payloads, count range overflow, revision `UINT32_MAX`, write/read-back corruption, reservation decrement, stale expected revision, duplicate transaction ID, pending reboot lock, completion without second decrement, safe pre-LOW cancellation, and uncertain outcome left pending.

```cpp
smv::MemoryInventoryBackend backend;
smv::InventoryStore store(backend);
Check(store.Load() == smv::InventoryResult::kUnavailable, "empty NVS accepted");

std::array<uint32_t, 16> counts{};
counts[0] = 2;
Check(store.Provision(counts) == smv::InventoryResult::kOk, "provision failed");
const auto token = store.Reserve(0x1122334455667788ULL, 0, store.Snapshot().revision);
Check(token.has_value(), "reservation failed");
Check(store.Snapshot().counts[0] == 1 && store.Snapshot().transaction_pending,
      "unit was not durably reserved");

smv::InventoryStore rebooted(backend);
Check(rebooted.Load() == smv::InventoryResult::kPendingTransaction,
      "unfinished transaction did not lock reboot");
```

- [ ] **Step 2: Run the inventory test and confirm missing implementation**

Run `bash main/vending/tests/run_host_tests.sh`.

Expected: compilation fails on `inventory/inventory_store.h`.

- [ ] **Step 3: Implement a canonical snapshot codec and state machine**

Define:

```cpp
class InventoryBackend {
public:
    virtual ~InventoryBackend() = default;
    virtual bool ReadBlob(std::string_view key, std::vector<uint8_t>& value) = 0;
    virtual bool WriteBlob(std::string_view key, std::span<const uint8_t> value) = 0;
};

enum class InventoryResult {
    kOk,
    kUnavailable,
    kCorrupt,
    kAmbiguousRevision,
    kPendingTransaction,
    kStaleRevision,
    kOutOfStock,
    kDuplicateTransaction,
    kRevisionExhausted,
    kWriteFailed,
};
```

Serialize integers explicitly in little-endian order; do not write a padded C++ struct. The version-1 payload contains magic `SMV1`, schema, revision, 16 counts capped at 10,000, valid mask, pending flag/channel/transaction ID, last transaction ID/outcome, and trailing CRC32. Reject revision zero, revision exhaustion, invalid channel/mask, pending-without-ID, and equal-revision divergent snapshots.

`Reserve()` writes/verifies the inactive key with `count-1` and pending `ARMED` before returning a token. `Complete()` writes a newer snapshot that clears pending and records `COMMAND_SENT_UNVERIFIED`; it does not decrement again. `CancelBeforePulse()` restores one unit only for the matching pending token and persists a newer snapshot. Any failed write/read-back makes the store unavailable.

- [ ] **Step 4: Implement the ESP NVS backend without fatal checks**

`NvsInventoryBackend("smv_inventory")` opens read/write once, treats missing keys as absent, uses `nvs_get_blob`, `nvs_set_blob`, and `nvs_commit`, and returns `false` on every ESP error after logging a bounded error code. It must not call `ESP_ERROR_CHECK` for inventory I/O because a storage failure must lock vending rather than reboot-loop the device.

Append the pure store test to the vending runner; do not compile the ESP NVS adapter in the host binary:

```bash
"$CXX" "${FLAGS[@]}" main/inventory/inventory_store.cc \
  main/vending/tests/test_inventory_store.cc \
  -o "$TMP_DIR/inventory_store_test"
"$TMP_DIR/inventory_store_test"
```

- [ ] **Step 5: Run all vending host tests**

Run `bash main/vending/tests/run_host_tests.sh`.

Expected: catalog and inventory tests pass, including pending reboot lock and no double decrement.

- [ ] **Step 6: Commit inventory sources**

```bash
git add main/inventory main/vending/tests/test_inventory_store.cc main/vending/tests/run_host_tests.sh
git diff --cached --check
git commit -m "feat: reserve vending inventory in NVS"
```

### Task 5: Enforce the final guard and physical-confirmation lifecycle

**Files:**
- Create: `main/vending/vend_guard.h`
- Create: `main/vending/vend_guard.cc`
- Create: `main/vending/vending_coordinator.h`
- Create: `main/vending/vending_coordinator.cc`
- Create: `main/vending/tests/test_vending_coordinator.cc`
- Modify: `main/vending/vending_types.h`
- Modify: `main/vending/tests/run_host_tests.sh`

**Interfaces:**
- Consumes: structured medical evaluation, fixed artifact identity/review result, catalog router, inventory store, caller-supplied monotonic timestamp, transaction-ID generator, and `RelayActuator`.
- Produces: safe MCP JSON, one expiring internal candidate, `Confirm()`, `Cancel()`, and `OnRelayOutcome()`.

- [ ] **Step 1: Write failing guard/coordinator tests**

Exercise compile gate off, invalid review, medical non-offer, invalid catalog, inventory unavailable/pending, stale revision, stock empty, 30,000 ms expiry boundary, stale/decreasing turn, changed session, non-idle confirmation, duplicate confirmation, relay busy, disconnect before LOW, uncertain relay outcome, and a successful fake-relay flow. Assert returned JSON never contains SKU/channel and never accepts spoken/cloud confirmation.

```cpp
const auto response = coordinator.EvaluateAndStage(CompleteHeadacheSnapshot(), 1000);
Check(JsonField(response, "status") == "OFFER", "eligible local offer not staged");
Check(response.find("SMV-PARA500") == std::string::npos, "SKU leaked to cloud");
Check(response.find("\"channel\"") == std::string::npos, "channel leaked to cloud");
Check(coordinator.AwaitingConfirmation(30999), "candidate expired too early");
Check(!coordinator.AwaitingConfirmation(31000), "candidate did not expire at 30 seconds");

coordinator.EvaluateAndStage(CompleteHeadacheSnapshot(2), 40000);
Check(coordinator.Confirm(40001, false) == smv::ConfirmResult::kApplicationStateBlocked,
      "non-idle app state authorized relay");
Check(coordinator.Confirm(40002, true) == smv::ConfirmResult::kStarted,
      "valid physical confirmation did not start");
Check(coordinator.Confirm(40003, true) == smv::ConfirmResult::kNoCandidate,
      "confirmation was reusable");
```

- [ ] **Step 2: Run tests and confirm guard/coordinator are missing**

Run `bash main/vending/tests/run_host_tests.sh`.

Expected: compilation fails on `vending/vending_coordinator.h`.

- [ ] **Step 3: Add the narrow relay and outcome types**

Append to `vending_types.h`:

```cpp
enum class RelayOutcome { kNotStartedCertain, kCommandSentUnverified, kUncertain };

class RelayActuator {
public:
    using Completion = std::function<void(uint64_t, RelayOutcome)>;
    virtual ~RelayActuator() = default;
    virtual bool IsIdle() const = 0;
    virtual bool Start(const ReservationToken& token, Completion completion) = 0;
    virtual void Cancel() = 0;
};
```

The actuator receives only a reservation token created by `InventoryStore`; it has no public raw-channel start method.

- [ ] **Step 4: Implement the pure final guard**

Define `VendGuardInput` with booleans for compile gate, review valid, candidate valid, physical confirmation, application state, relay idle, and inventory validity plus matching transaction/channel/revisions. `VendGuard::Check()` returns the first explicit failure reason and authorizes only quantity one. It performs no I/O.

- [ ] **Step 5: Implement coordinator lifecycle and bounded response rendering**

Construct the coordinator with references to advisor/router/inventory/relay and value/callback dependencies:

```cpp
using TransactionIdSource = std::function<uint64_t()>;
using AuditSink = std::function<void(const AuditEvent&)>;

VendingCoordinator(MedicalAdvisor& advisor,
                   CatalogRouter& router,
                   InventoryStore& inventory,
                   RelayActuator& relay,
                   ArtifactIdentity identity,
                   ReviewResult review,
                   bool production_enabled,
                   TransactionIdSource next_transaction_id,
                   AuditSink audit_sink);
```

`EvaluateAndStage()` invalidates any older candidate, rejects non-increasing turns within a session, routes only the structured canonical ID, captures inventory revision, and sets expiry to `created_ms + 30000`. It returns `OFFER` only after all non-confirmation prerequisites pass; otherwise it returns `ASK`, `REFER`, or `BLOCK` with `vend_allowed=false`.

`Confirm()` first checks the physical event with `VendGuard`. An application-state rejection leaves the unconfirmed candidate available until its original expiry so the user must press again after the device becomes idle. Once all checks pass, it consumes the candidate before side effects, calls `InventoryStore::Reserve()`, and then calls relay `Start()`. A certain pre-LOW failure invokes `CancelBeforePulse()`; an uncertain outcome leaves the reservation pending; `kCommandSentUnverified` invokes `Complete()`. No branch retries relay automatically.

`AuditEvent` contains only transaction ID, rule/catalog versions and hashes, decision reason, local channel, inventory revisions, monotonic timestamp, and relay outcome. Add a fake sink assertion that symptom text, medicine lists, and the original JSON payload never appear in an audit event.

Append the coordinator integration test to the vending runner:

```bash
"$CXX" "${FLAGS[@]}" main/medical/medical_advisor.cc \
  main/medical/pharmacist_review_verifier.cc main/vending/catalog_router.cc \
  main/inventory/inventory_store.cc main/vending/vend_guard.cc \
  main/vending/vending_coordinator.cc main/vending/tests/test_vending_coordinator.cc \
  -Wl,-l:libcjson.so.1 -o "$TMP_DIR/vending_coordinator_test"
"$TMP_DIR/vending_coordinator_test"
```

- [ ] **Step 6: Run coordinator tests**

Run `bash main/vending/tests/run_host_tests.sh`.

Expected: all guard, expiry, stale-turn, click, reservation, outcome, and no-leak assertions pass.

- [ ] **Step 7: Commit the policy coordinator**

```bash
git add main/vending/vend_guard.h main/vending/vend_guard.cc main/vending/vending_coordinator.h main/vending/vending_coordinator.cc main/vending/vending_types.h main/vending/tests/test_vending_coordinator.cc main/vending/tests/run_host_tests.sh
git diff --cached --check
git commit -m "feat: guard physical vending transactions"
```

### Task 6: Implement the fail-safe non-blocking relay driver

**Files:**
- Create: `main/boards/smartmedivend-s3/relay_driver.h`
- Create: `main/boards/smartmedivend-s3/relay_driver.cc`
- Create: `main/boards/smartmedivend-s3/esp_relay_platform.h`
- Create: `main/boards/smartmedivend-s3/esp_relay_platform.cc`
- Create: `main/vending/tests/test_relay_driver.cc`
- Modify: `main/vending/tests/run_host_tests.sh`

**Interfaces:**
- Consumes: verified `ReservationToken` and injected bounded GPIO/timer platform.
- Produces: `RelayActuator` implementation and exactly one completion outcome.

- [ ] **Step 1: Write failing fake-platform timing tests**

Record every GPIO/timer action and assert this order:

```cpp
Check(platform.events == std::vector<std::string>{
    "SIG_HIGH", "SELECT_5", "ARM_10MS"
}, "unsafe start order");
platform.FireTimer();
Check(platform.events == std::vector<std::string>{
    "SIG_HIGH", "SELECT_5", "ARM_10MS", "ARM_500MS", "SIG_LOW"
}, "LOW occurred before shutoff timer was armed");
platform.FireTimer();
Check(platform.events.back() == "SIG_HIGH", "pulse callback did not de-energize first");
```

Also test invalid channel, busy start, settle timer failure, 500 ms arm failure, guard timer failure, stale generation callback, cancel in settling (`kNotStartedCertain`), cancel in pulsing (`kUncertain`), and no second completion callback.

- [ ] **Step 2: Run tests and confirm relay driver is missing**

Run `bash main/vending/tests/run_host_tests.sh`.

Expected: compilation fails on `relay_driver.h`.

- [ ] **Step 3: Implement the injected state machine**

Define:

```cpp
enum class RelayState { kDisabled, kIdle, kSettling, kPulsing, kGuardGap, kFault };

enum class RelayTimerPhase { kSettle, kPulse, kGuard };

class RelayPlatform {
public:
    using TimerCallback = void (*)(void*, uint32_t, RelayTimerPhase);
    virtual ~RelayPlatform() = default;
    virtual bool InitializeInactive() = 0;
    virtual void SetSignalHigh() = 0;
    virtual void SetSignalLow() = 0;
    virtual bool SelectChannel(uint8_t channel) = 0;
    virtual bool ArmOneShot(uint32_t delay_ms,
                            uint32_t generation,
                            RelayTimerPhase phase,
                            TimerCallback callback,
                            void* context) = 0;
    virtual void CancelTimer() = 0;
};
```

`RelayDriver::Start()` requires `Idle`, a token channel below 16, and a nonzero transaction ID. It sets HIGH, selects, then arms 10 ms. On settle expiry it first arms 500 ms and only then sets LOW. On pulse expiry its first operation is HIGH, it reports `kCommandSentUnverified`, and arms the 100 ms guard. A guard-arm failure stays HIGH and enters `Fault`. `OnTimer(generation, phase)` ignores callbacks whose generation or expected phase does not match the active transaction.

- [ ] **Step 4: Implement the ESP adapter**

`EspRelayPlatform::InitializeInactive()` preloads GPIO17 HIGH before configuring it output, configures GPIO39-42 outputs while HIGH, and creates one `esp_timer` handle. `ArmOneShot()` records generation/phase and returns `false` for any `esp_timer_stop/start_once` error. The timer callback copies the recorded generation/phase before invoking only the driver's bounded callback; no JSON, NVS, UI, or logging loop runs there.

Append only the platform-independent driver to the host runner:

```bash
"$CXX" "${FLAGS[@]}" main/boards/smartmedivend-s3/relay_driver.cc \
  main/vending/tests/test_relay_driver.cc -o "$TMP_DIR/relay_driver_test"
"$TMP_DIR/relay_driver_test"
```

- [ ] **Step 5: Run relay and all vending host tests**

Run `bash main/vending/tests/run_host_tests.sh`.

Expected: all tests pass and the fake trace proves `ARM_500MS` precedes `SIG_LOW`.

- [ ] **Step 6: Commit the relay unit**

```bash
git add main/boards/smartmedivend-s3/relay_driver.h main/boards/smartmedivend-s3/relay_driver.cc main/boards/smartmedivend-s3/esp_relay_platform.h main/boards/smartmedivend-s3/esp_relay_platform.cc main/vending/tests/test_relay_driver.cc main/vending/tests/run_host_tests.sh
git diff --cached --check
git commit -m "feat: pulse SmartMediVend relay fail closed"
```

### Task 7: Wire the board, application lifecycle, Kconfig, and build

**Files:**
- Modify: `main/boards/common/board.h`
- Modify: `main/application.cc`
- Modify: `main/boards/smartmedivend-s3/smartmedivend_board.cc`
- Modify: `main/Kconfig.projbuild`
- Modify: `main/CMakeLists.txt`
- Create: `scripts/tests/test_smartmedivend_vending_wiring.py`

**Interfaces:**
- Consumes: all completed components, application device-state events, MCP medical payloads, and the physical button.
- Produces: a relay-disabled-by-default SmartMediVend firmware path with no cloud vend tool.

- [ ] **Step 1: Write a failing static wiring test**

The Python test reads source/Kconfig/CMake and asserts:

```python
self.assertIn("config SMARTMEDIVEND_PRODUCTION_VENDING", kconfig)
self.assertRegex(kconfig, r"config SMARTMEDIVEND_PRODUCTION_VENDING[\s\S]*?default n")
self.assertNotRegex(board_source, r'AddTool\([^\n]*(vend|relay|inventory|channel)')
self.assertIn("Application::GetInstance().Schedule", board_source)
self.assertIn("coordinator_->Confirm", board_source)
self.assertIn("board.OnDeviceStateChanged(new_state)", application_source)
```

Also assert the SmartMediVend CMake branch contains every new source and the global source list does not compile them for other boards.

- [ ] **Step 2: Run the static test and confirm wiring is absent**

Run:

```bash
python3 -m unittest scripts.tests.test_smartmedivend_vending_wiring -v
```

Expected: failure because the production-vending Kconfig and board composition are absent.

- [ ] **Step 3: Add the locked-default Kconfig and board-only source list**

Add:

```kconfig
config SMARTMEDIVEND_PRODUCTION_VENDING
    bool "Enable pharmacist-approved SmartMediVend relay vending"
    default n
    depends on BOARD_TYPE_SMARTMEDIVEND_S3
    help
        Enables only the compile-time half of the vending gate. Runtime still
        requires exact pharmacist approval, valid NVS inventory, local policy,
        physical confirmation, and an idle fail-safe relay driver.
```

Append all medical approval, routing, inventory, coordinator, relay, and ESP adapter `.cc` files only inside `if(CONFIG_BOARD_TYPE_SMARTMEDIVEND_S3)`. Add `nvs_flash` and `esp_timer` to `PRIV_REQUIRES`; `esp_driver_gpio` is already present.

- [ ] **Step 4: Add an optional board state-change hook**

In `Board`, add a default no-op `virtual void OnDeviceStateChanged(DeviceState state)` and include the existing `device_state.h`. Call it from `Application::HandleStateChangedEvent()` on the application task immediately after retrieving `new_state` and the board. Other boards require no code changes.

SmartMediVend invalidates a candidate and forces relay cancellation on `Connecting`, `WifiConfiguring`, `Activating`, `Upgrading`, `FatalError`, and `Unknown`; it does not cancel merely because the AI transitions through `Speaking` before showing an offer.

- [ ] **Step 5: Compose the SmartMediVend services**

After GPIO-safe platform initialization, create the verifier identity from generated constants, validate the review, load NVS, validate the catalog, and construct the coordinator. Use `esp_timer_get_time()/1000` for monotonic milliseconds and two `esp_random()` values for a nonzero 64-bit transaction ID.

Change only the assessment tool callback:

```cpp
return coordinator_->EvaluateAndStage(
    properties["payload_json"].value<std::string>(),
    static_cast<uint64_t>(esp_timer_get_time() / 1000));
```

Do not add a vend, inventory-write, SKU, channel, or relay MCP tool.

- [ ] **Step 6: Route physical button events on the application task**

For a short click, schedule one lambda. If a live candidate exists, call `Confirm(now, app.GetDeviceState() == kDeviceStateIdle)` and show a bounded notification; otherwise call the existing `ToggleChatState()`. For long press, schedule `Cancel()`/relay all-off before `EnterWifiConfigMode()`. Double-click status-page behavior remains unchanged.

Relay completion must schedule `coordinator_->OnRelayOutcome()` onto the application task before touching NVS or UI.

- [ ] **Step 7: Run host and wiring tests**

Run:

```bash
bash scripts/tests/run_smartmedivend_host_tests.sh
bash main/vending/tests/run_host_tests.sh
python3 -m unittest discover -s scripts/tests -v
```

Expected: all available tests pass; static wiring finds no vend-like MCP tool and confirms default-off Kconfig.

- [ ] **Step 8: Format and build the exact board variant**

Run:

```bash
clang-format -i main/medical/medical_advisor.h main/medical/medical_advisor.cc main/medical/pharmacist_review_verifier.h main/medical/pharmacist_review_verifier.cc main/vending/*.h main/vending/*.cc main/inventory/*.h main/inventory/*.cc main/boards/smartmedivend-s3/relay_driver.h main/boards/smartmedivend-s3/relay_driver.cc main/boards/smartmedivend-s3/esp_relay_platform.h main/boards/smartmedivend-s3/esp_relay_platform.cc main/boards/smartmedivend-s3/smartmedivend_board.cc
python3 scripts/build.py smartmedivend-s3 --name smartmedivend-s3
```

Expected: ESP-IDF 6.1 build succeeds. Build logs show production vending disabled by default and the incomplete current review/inventory states locked, not fatal/rebooting.

- [ ] **Step 9: Commit only new wiring changes**

Inspect the dirty board/medical paths before staging. Stage only Task 7 hunks:

```bash
git add main/boards/common/board.h main/application.cc main/Kconfig.projbuild main/CMakeLists.txt scripts/tests/test_smartmedivend_vending_wiring.py
git add -p -- main/boards/smartmedivend-s3/smartmedivend_board.cc
git diff --cached --check
git commit -m "feat: integrate fail-closed SmartMediVend vending"
```

### Task 8: Document provisioning and physically verify fail-closed behavior

**Files:**
- Create: `docs/smartmedivend/FAIL_CLOSED_VENDING_TEST.md`
- Modify: `docs/smartmedivend/XIAOZHI_CLOUD_ROLE_STAGE1.md`
- Modify: `docs/smartmedivend/STAGE1_INTEGRATION_AND_TEST.md`

**Interfaces:**
- Consumes: the completed firmware and a pharmacist/technician-supplied approval plus NVS inventory.
- Produces: an operator procedure that cannot be mistaken for pharmacist approval or closed-loop dispense proof.

- [ ] **Step 1: Write the locked-default and provisioning documentation**

Document exact review fields, generated hash locations, Kconfig gate, NVS namespace/keys, technician-only `Provision()` boundary, candidate timeout, button behavior, log reason codes, and the fact that catalog `initial_stock` is ignored. State explicitly that the current review and antacid backup mismatch keep vending locked.

Update the cloud role from `PROVISIONAL_OPTIONS` to the coordinator's `ASK`/`REFER`/`BLOCK`/`OFFER` responses. `OFFER` instructs the user to wait until speech ends and press the physical button; voice confirmation never authorizes vending.

- [ ] **Step 2: Run documentation and source safety scans**

Run:

```bash
rg -n "AddTool.*(vend|relay|inventory|channel)|initial_stock.*(route|inventory)|delay\(|vTaskDelay" main/boards/smartmedivend-s3 main/medical main/vending main/inventory
rg -n "approved=false|voice-only pilot|PROVISIONAL_OPTIONS" docs/smartmedivend
```

Expected: no cloud-facing vend/relay/inventory/channel tool, no blocking delay in the new path, and no stale documentation claiming the old pilot state. References that explain historical behavior must be clearly labeled historical.

- [ ] **Step 3: Run the complete automated verification set**

Run:

```bash
bash scripts/tests/run_smartmedivend_host_tests.sh
bash main/vending/tests/run_host_tests.sh
python3 -m unittest discover -s scripts/tests -v
python3 scripts/build.py smartmedivend-s3 --name smartmedivend-s3
git diff --check
```

Expected: all runnable tests and the target build pass. If ESP-IDF, Python, the host C++ compiler, or cJSON development files are unavailable, record the exact missing command/dependency rather than claiming a pass.

- [ ] **Step 4: Perform no-load electrical verification before connecting medicine channels**

With production Kconfig enabled only in a controlled test build and synthetic non-clinical fixtures isolated from production data:

1. Confirm GPIO17 is HIGH throughout reset/boot and while review/inventory are invalid.
2. Confirm S0-S3 settle for at least 10 ms before the LOW edge.
3. Measure the LOW pulse at nominal 500 ms and verify it returns HIGH before the 100 ms guard interval ends.
4. Interrupt power during settling, pulse, guard, and completion persistence; confirm reboot never replays and unfinished transactions lock.
5. Confirm primary stock selection, backup selection after primary reaches zero, and no substitution for the mismatched antacid backup.

Record logic-analyzer measurements and clearly label `COMMAND_SENT_UNVERIFIED`; without a drop/current sensor this is not proof that a package was dispensed.

- [ ] **Step 5: Commit documentation only**

```bash
git add docs/smartmedivend/FAIL_CLOSED_VENDING_TEST.md docs/smartmedivend/XIAOZHI_CLOUD_ROLE_STAGE1.md docs/smartmedivend/STAGE1_INTEGRATION_AND_TEST.md
git diff --cached --check
git commit -m "docs: add fail-closed vending verification"
```

## Completion Gate

Before claiming completion, invoke `superpowers:verification-before-completion`, rerun the complete automated commands that are available, inspect the final diff for forbidden MCP/control paths and unrelated worktree changes, and report physical hardware verification separately from build/test results. The implementation is complete only when the code is fail-closed with the current incomplete approval/NVS state; it is not clinically approved or physically validated until the named human steps are performed.
