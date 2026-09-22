# SmartMediVend — provisioning and fail-closed verification

## Safety boundary

Run this procedure with relay loads and medicine mechanisms disconnected. Use LEDs/dummy loads or a logic analyzer first. The ESP32 output is active LOW; every relay input must have an external pull-up that holds it HIGH while the ESP32 is reset, unpowered or its pin is high-impedance.

Do not enable production vending from repository defaults. The current approval file is invalid, the current antacid backup identity is inconsistent, and a new device has no provisioned NVS inventory. Those independent locks are intentional.

## Pharmacist approval artifact

`data/pharmacist_review.json` must contain exactly these fields, with no duplicates or extra keys:

```json
{
  "schema_version": 1,
  "approved": true,
  "reviewed_catalog_version": "<exact catalog_version>",
  "reviewed_rules_version": "<exact rules_version>",
  "reviewer": "<pharmacist name or controlled identifier>",
  "reviewer_license": "<license identifier>",
  "reviewed_at": "2026-09-22T10:00:00+07:00",
  "catalog_sha256": "<64 lowercase hex characters>",
  "rules_sha256": "<64 lowercase hex characters>"
}
```

Both reviewer strings must be 1–96 bytes. `reviewed_at` must be a valid RFC 3339 whole-second timestamp using `Z` or an explicit `±HH:MM` offset. Versions must match the respective JSON `rules_version`/`catalog_version`; hashes must match the exact files used by CMake.

After configuration, inspect the values embedded for that build in `build/esp-idf/main/smv_medical_generated/medical_data_generated.h`. On PowerShell, independently calculate the source hashes with:

```powershell
(Get-FileHash data/medical_rules.json -Algorithm SHA256).Hash.ToLowerInvariant()
(Get-FileHash data/medicines.json -Algorithm SHA256).Hash.ToLowerInvariant()
```

The pharmacist must approve the actual rule/catalog contents, not merely copy hashes. Any content change invalidates the prior approval.

## Technician-only NVS inventory

Live stock is stored in NVS namespace `smv_inventory` as alternating blobs `snapshot_a` and `snapshot_b`. Each canonical snapshot carries a schema marker, monotonically increasing revision, counts for channels 0–15, a validity mask, pending/last transaction metadata and CRC32. `data/medicines.json.initial_stock` is never used for routing or provisioning.

Provisioning must call `InventoryStore::Provision(counts, valid_mask)` only from a controlled manufacturing/service path after a technician physically reconciles every marked channel. There is deliberately no cloud, MCP, voice or public UI provisioning endpoint. Counts are 0–10,000; a zero validity mask, an out-of-range count, a pending transaction or a failed read-back keeps inventory unavailable.

Do not write the binary blobs manually or erase only one slot. `Provision()` writes the inactive slot, commits, reads it back byte-for-byte and publishes it only after validation. After any reboot showing a pending transaction, isolate the mechanism, reconcile physical stock, investigate the audit record, then reprovision through the technician path; firmware must never replay the previous pulse.

## Enabling a controlled build

Only after pharmacist approval, catalog correction, NVS provisioning design review and no-load setup:

1. Select board `smartmedivend-s3` and enable `CONFIG_SMARTMEDIVEND_PRODUCTION_VENDING` in the controlled build configuration.
2. Rebuild from the exact reviewed data with ESP-IDF 6.1.
3. Archive the firmware identity, rule/catalog versions and SHA-256 values with the review record.
4. On boot, check the `SmartMediVend` line `vending gates production=1 relay=1 review=1 catalog=1 inventory=1`. Any zero means remain locked.

## No-load electrical verification

Connect the logic analyzer to GPIO17 (`MUX_SIG`) and GPIO39–42 (`S0`–`S3`). Keep relay loads disconnected.

1. Reset, power-cycle and hold the ESP32 in reset. Verify GPIO17 remains HIGH throughout. Repeat with the Kconfig gate off, invalid review, invalid catalog and missing/corrupt NVS; no LOW edge is allowed.
2. With synthetic non-clinical test fixtures, stage one `OFFER`. Confirm that spoken approval and cloud tool calls never create a LOW edge.
3. Let the candidate age to 30,000 ms. A short click at or after the boundary must not pulse.
4. Create a fresh candidate, wait until speech has ended and short-click while the application is idle. Verify S0–S3 represent the locally selected channel and remain stable for at least 10 ms before GPIO17 falls.
5. Measure GPIO17 LOW for nominal 500 ms, then verify it returns HIGH and no new transaction starts during the following 100 ms guard.
6. Verify primary selection while primary count is nonzero, exact backup selection after primary reaches zero, and no substitution when backup identity differs.
7. Repeat short-click while the application is not idle, double-click, long-press, disconnect/reconnect, and unsafe device-state changes. None may start a new pulse; long-press must force HIGH.
8. Interrupt power separately during settle, LOW pulse, guard and completion persistence. On every reboot, verify there is no automatic replay. An unresolved pending transaction must lock further vending.
9. Inject timer/GPIO/NVS failures where feasible. A failure before LOW may cancel and restore stock; an interruption after LOW begins is uncertain and must not silently restore stock.

Record minimum/maximum settle time, LOW width, HIGH guard time, selected channel bits, reset trace, firmware hash and test fixture identity. Do not connect medicines until every result is reviewed.

## Runtime results and logs

Cloud-visible gate reasons include `PRODUCTION_DISABLED`, `PHARMACIST_REVIEW_INVALID`, catalog validation reasons such as `BACKUP_IDENTITY_MISMATCH`, `INVENTORY_UNAVAILABLE`, `OUT_OF_STOCK`, `STALE_TURN`, and `TRANSACTION_ID_UNAVAILABLE`. The final guard can additionally reject `APPLICATION_STATE_BLOCKED`, `RELAY_BUSY`, `STALE_INVENTORY_REVISION`, `ROUTE_CHANGED` and invalid confirmation/quantity conditions.

Audit events contain transaction ID, selected channel, rule/catalog versions and hashes, inventory revisions and one outcome reason:

- `NOT_STARTED_CERTAIN`: relay definitely never reached LOW; reservation is cancelled.
- `COMMAND_SENT_UNVERIFIED`: the 500 ms command pulse ended and the reservation is completed.
- `UNCERTAIN_RELAY_OUTCOME`: LOW may have started or completion is unknown; inventory remains pending and vending locks.

Audit logs deliberately omit the patient payload. `COMMAND_SENT_UNVERIFIED` means only that the electrical command was sent. Without a package-drop/current sensor it is not evidence that a medicine package was dispensed.
