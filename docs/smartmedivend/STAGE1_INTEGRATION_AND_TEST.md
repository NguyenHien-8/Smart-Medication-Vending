# SmartMediVend — fail-closed local vending integration

## Scope

The SmartMediVend-S3 build now combines the local medical interview, pharmacist-artifact verification, local catalog routing, dual-slot NVS inventory, physical confirmation, and a non-blocking relay state machine. The cloud can submit patient facts and receive `ASK`, `REFER`, `BLOCK`, or `OFFER`; it cannot select a SKU/channel, confirm a transaction, write stock, or operate a relay.

This is a fail-closed command path, not a closed-loop dispensing system. There is no package-drop or current sensor, so a completed pulse is recorded as `COMMAND_SENT_UNVERIFIED`, never as proof that medicine was dispensed.

## Decision and actuation boundary

1. `MedicalAdvisor` validates the explicitly reported patient snapshot and returns one structured local canonical item only when the approved rules reach an offer.
2. `VendingCoordinator` checks the compile gate, exact pharmacist review, catalog validity and live NVS snapshot.
3. `CatalogRouter` chooses the primary channel, or an exact-identity backup only when primary stock is zero. The cloud never sees the SKU or channel.
4. A candidate expires after 30,000 ms. Speech cannot confirm it. A short physical-button click on the application task revalidates state, route, stock revision and relay state.
5. `InventoryStore` durably reserves one unit before the relay starts. A reboot with a pending reservation locks vending and never replays the pulse.
6. The board holds GPIO17 HIGH, selects GPIO39–42, waits 10 ms, arms the 500 ms shutoff timer, then drives GPIO17 LOW. It returns HIGH before the 100 ms guard interval.

Button behavior is board-local: a short click confirms a live candidate or keeps the existing chat-toggle behavior when no candidate exists; a long press cancels and forces the relay inactive before Wi-Fi configuration; double click keeps the status-page behavior. Unsafe device-state changes cancel the candidate and relay operation.

## Source of truth and build identity

`data/medical_rules.json`, `data/medicines.json`, and `data/pharmacist_review.json` are read at CMake configure time. CMake calculates SHA-256 for the rules and catalog and writes the embedded values to:

`build/esp-idf/main/smv_medical_generated/medical_data_generated.h`

The build-generated header is evidence for the exact firmware build; do not edit it. A review is valid only when its versions and lowercase SHA-256 values exactly match those embedded values.

The compile gate `CONFIG_SMARTMEDIVEND_PRODUCTION_VENDING` is available only for `BOARD_TYPE_SMARTMEDIVEND_S3` and defaults to `n`. Enabling it satisfies only one gate; all runtime checks remain mandatory.

## Current repository remains locked

The repository data intentionally cannot vend:

- the production Kconfig gate defaults off;
- `data/pharmacist_review.json` is structurally incomplete and contains an unrecognized `notice` field, so exact pharmacist verification fails;
- channel 15 declares an antacid backup whose `strength` differs from channel 7, so the entire catalog is rejected with `BACKUP_IDENTITY_MISMATCH`;
- a new device has no valid NVS inventory snapshot until a technician provisions it.

Changing only one of these conditions must not enable vending. Pharmacist review and hardware inventory provisioning are separate controlled actions.

## MCP contract

- `self.medical.get_intake_schema`: returns supported fields and enum semantics.
- `self.medical.get_symptom_guide`: returns a short guide for exactly one reported symptom enum.
- `self.medical.evaluate_symptoms`: accepts a `payload_json` string up to 4096 bytes and returns one of `ASK`, `REFER`, `BLOCK`, or `OFFER`.

For `ASK`, the cloud reads only `next_question_vi`. `REFER` stops the offer flow. `BLOCK` reports a local fail-closed reason. `OFFER` returns bounded display information plus `confirmation=PRESS_PHYSICAL_BUTTON`, while `vend_allowed` remains false because cloud speech is not authorization. Responses contain no SKU or channel.

Unknown or duplicate keys, malformed/oversized JSON, stale/decreasing turns, unsupported clinical situations and forbidden control fields fail closed. Missing arrays are unknown; an empty array is an explicit negative answer and may be sent only after the user clearly confirms it.

## Build and automated checks

Use ESP-IDF 6.1 (minimum 6.0.1):

```powershell
. C:\esp\v6.1\esp-idf\export.ps1
python scripts/build.py smartmedivend-s3 --name smartmedivend-s3
powershell -ExecutionPolicy Bypass -File scripts/tests/run_smartmedivend_host_tests.ps1
powershell -ExecutionPolicy Bypass -File main/vending/tests/run_host_tests.ps1
```

The build must show `# CONFIG_SMARTMEDIVEND_PRODUCTION_VENDING is not set` for the default release. A successful build validates compilation, not GPIO timing, reset behavior, pharmacist approval, actual stock, or package delivery. Perform the controlled procedure in `FAIL_CLOSED_VENDING_TEST.md` before attaching medicine channels.
