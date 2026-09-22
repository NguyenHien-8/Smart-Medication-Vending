# SmartMediVend fail-closed vending design

Status: Proposed for implementation review

Date: 2026-09-22

Target: ESP-IDF 6.1, `smartmedivend-s3` board

## 1. Goal and safety boundary

Convert the current interview pilot into a non-blocking, fail-closed single-item vending flow. The conversational AI gathers facts, but the ESP32 makes every medical eligibility, SKU, stock-routing, and relay decision from pharmacist-approved local data.

The machine must never present itself as diagnosing disease. It may offer one pharmacist-approved over-the-counter option for a narrowly matched symptom profile. Any missing, uncertain, contradictory, stale, or unapproved input produces `REFER` or `BLOCK` and no relay pulse.

This design does not approve medical content. Production vending remains locked until a pharmacist supplies a complete approval record that is bound to the exact rule and catalog artifacts embedded in the firmware.

## 2. Confirmed hardware and product decisions

- Relay inputs are active LOW and each input has an external pull-up that keeps it HIGH while the ESP32 is resetting or its pins are high impedance.
- A CD74HC4067 selects one of 16 relay channels.
- `GPIO17` is the shared active-LOW relay signal.
- `GPIO39`, `GPIO40`, `GPIO41`, and `GPIO42` are selector bits S0 through S3.
- A valid pulse lasts 500 ms. The relay signal is then restored HIGH.
- Channel switching uses a 10 ms settle interval while the relay signal is HIGH, followed by a 100 ms all-off guard interval after the pulse.
- A transaction may offer and dispense exactly one SKU and one unit.
- Inventory is authoritative only when loaded from a valid NVS snapshot.
- Missing or invalid approval, inventory, candidate, confirmation, or hardware state fails closed.

## 3. Trust boundaries

### Conversational AI

The AI may collect and submit only structured patient facts: session and turn identifiers, age, weight, pregnancy state, symptoms and duration, danger signs, chronic conditions, current medicines, allergies, and answers to local screening questions.

The AI must not submit or choose a SKU, catalog ID, relay channel, primary/backup route, stock value, authorization flag, or pulse command. Unknown or forbidden fields are rejected rather than ignored.

### ESP32 local policy

The ESP32 validates the interview, applies local medical rules, selects at most one canonical catalog item, verifies pharmacist approval, selects a stocked physical route, waits for local physical confirmation, and controls the relay. Cloud responses are advisory conversation only and cannot authorize vending.

### Physical layer

Only the board-owned relay driver may write the selector and signal pins. No MCP tool, protocol callback, medical rule, or AI response receives a direct relay API.

## 4. Proposed component ownership

- `main/medical/medical_advisor.*` remains the validated medical interview and local rule boundary. It gains a structured internal evaluation result; the MCP JSON response is rendered from that result so routing never parses its own display text.
- `main/medical/pharmacist_review_verifier.*` verifies approval metadata and artifact binding.
- `main/inventory/inventory_store.*` owns versioned NVS snapshots, validation, and stock commits.
- `main/vending/catalog_router.*` maps the local canonical item to a primary or backup channel using the current inventory snapshot.
- `main/vending/vend_guard.*` performs the final authorization checks and idempotency checks.
- `main/vending/vending_coordinator.*` owns the candidate/confirmation/transaction lifecycle and schedules persistence/UI work.
- `main/boards/smartmedivend-s3/relay_driver.*` owns the CD74HC4067 GPIO sequence and timer state machine.
- `main/boards/smartmedivend-s3/smartmedivend_board.cc` wires these board capabilities together without placing SmartMediVend pin definitions in core modules.

Core code depends on narrow interfaces, never on the board's concrete `config.h`. Application mutations triggered from callbacks are scheduled onto the application task.

## 5. Pharmacist approval gate

`data/pharmacist_review.json` is valid only when all of these conditions hold:

- `schema_version` is supported.
- `approved` is exactly `true`.
- `reviewed_catalog_version` exactly matches the embedded catalog version.
- `reviewed_rules_version` exactly matches the embedded rules version.
- `catalog_sha256` and `rules_sha256` exactly match build-generated hashes of the embedded artifacts.
- `reviewer`, `reviewer_license`, and an RFC 3339 `reviewed_at` value are non-empty and syntactically valid.

The build embeds the review record, versions, and hashes. A hash binds approval to exact bytes but does not by itself prove the reviewer's identity; authenticity remains part of the controlled pharmacist release process. No reviewer identity, license, timestamp, approval, or corrected medicine data will be invented by firmware or by this implementation.

The current review file has `approved=true` but lacks the required review identity, timestamp, versions, and hashes. It therefore remains invalid and production relay authorization remains disabled.

A compile-time production-vending option and the runtime approval verifier must both be enabled. Development builds default to relay-disabled simulation. There is no runtime network command that can bypass either gate.

## 6. Medical evaluation and candidate lifecycle

The local evaluator returns one of:

- `BLOCK`: danger sign, contraindication, unsupported patient group, forbidden input, malformed data, or incomplete safety answer.
- `ASK`: a required fact is still missing.
- `REFER`: the local rules cannot safely distinguish one option, including multiple eligible candidates.
- `OFFER`: exactly one local canonical item is eligible.

An `OFFER` creates an internal candidate containing the canonical item ID, session ID, accepted turn ID, rule/catalog versions and hashes, inventory revision, creation time, expiry time, and a random transaction ID. It contains no AI-provided SKU or channel. The candidate expires after 30 seconds, on a newer accepted interview turn, cancellation, disconnect, state error, rule/catalog mismatch, inventory revision change, or reboot.

The response shown to the AI contains the safe display name/strength and asks the user to confirm on the physical device. It does not expose a relay channel and does not accept spoken confirmation as vending authorization.

The physical button confirms only while one unexpired candidate is awaiting confirmation. A long press cancels the candidate before any existing board-specific long-press action is processed. Confirmation is consumed exactly once.

## 7. Catalog validation and deterministic routing

At initialization, the local catalog is rejected unless:

- every physical channel is an integer from 0 through 15 and is unique;
- every vendable canonical item has exactly one primary route and at most one backup route;
- primary and backup routes match canonical ID, medicine name, active ingredient, strength, and unit;
- all required medical-rule references resolve to a catalog item;
- no entry is disabled, expired, malformed, or unapproved.

For one eligible canonical item, routing takes an immutable inventory snapshot and selects the primary channel if its count is greater than zero; otherwise it selects the matching backup channel if present and stocked. It never substitutes a different medicine. If neither is available, it returns out of stock without creating relay work.

The present antacid backup entry does not match the primary strength text, so that route must stay blocked until the catalog is corrected and the corrected catalog is pharmacist-approved.

## 8. NVS inventory model

Inventory uses namespace `smv_inventory` with two snapshot keys, `snapshot_a` and `snapshot_b`. Each snapshot contains:

- schema version;
- monotonically increasing revision;
- 16 unsigned item counts;
- per-channel validity flags;
- an optional pending transaction marker containing transaction ID, channel, and `ARMED` state;
- CRC32 over the serialized payload.

On boot, the store validates both snapshots and selects the valid snapshot with the newest revision. A missing snapshot, CRC failure, unsupported schema, ambiguous revision, or out-of-range count marks inventory unavailable and locks vending.

Updates use copy-on-write: write and verify the inactive slot, then treat its newer revision as current. Before relay work starts, the coordinator atomically reserves the unit by writing a newer snapshot with the selected count decremented and the transaction marked `ARMED`. If that commit or read-back verification fails, no pulse starts. After the relay pulse finishes, another snapshot clears the pending marker and records `COMMAND_SENT_UNVERIFIED` in the audit state without decrementing again.

If power is lost or a commit fails while a pending marker exists, boot detects the unfinished transaction and locks all vending until a technician reconciles it. This intentionally prefers under-reporting available stock over a duplicate dispense. Firmware never automatically retries a pulse or restores a reservation whose physical outcome is uncertain.

Inventory can be replaced only through a local technician provisioning path that calls the inventory store API; no AI or network MCP inventory-write tool is added. Defining the external refill/service application is outside this firmware change. Until a technician has provisioned a valid snapshot, vending remains locked.

## 9. Final vend guard

Immediately before starting a pulse, `VendGuard` atomically rechecks:

- production vending is compiled in;
- pharmacist approval is complete and matches embedded rule/catalog bytes;
- medical result is one unexpired `OFFER` for the same session and latest accepted turn;
- candidate and current rule/catalog versions and hashes still match;
- physical confirmation was consumed for the same transaction ID;
- inventory is valid, its revision matches the candidate route decision, selected count is nonzero, and no pending transaction exists;
- selected route matches the candidate canonical item and catalog identity fields;
- exactly one unit is requested;
- no relay transaction is active or in its guard interval;
- the transaction ID has not previously started or completed;
- application/device state permits a local vend and no disconnect, cancellation, or fault is pending.

Any failed condition clears or invalidates the candidate and produces no LOW transition.

After these checks pass, the coordinator persists and verifies the inventory reservation described above. The relay driver accepts only the resulting reservation token and transaction ID, never a raw channel from protocol or AI code.

## 10. Non-blocking relay state machine

The relay driver states are `Disabled`, `Idle`, `Settling`, `Pulsing`, `GuardGap`, and `Fault`.

Initialization raises `GPIO17` HIGH before enabling it as an output, then configures selector pins while the signal remains HIGH. A transaction performs:

1. Verify the persisted reservation token, idle state, and transaction generation, then drive the signal HIGH.
2. Set S0-S3 to the selected channel.
3. Start a one-shot 10 ms settle timer and return immediately.
4. On expiry, revalidate the driver's transaction generation, drive `GPIO17` LOW, and start a one-shot 500 ms timer.
5. On expiry, drive `GPIO17` HIGH as the first callback action, mark the command `COMMAND_SENT_UNVERIFIED`, and schedule completion work on the application task.
6. Start a one-shot 100 ms all-off guard timer before accepting another transaction.

Timer callbacks perform only bounded GPIO/state changes and scheduling. JSON parsing, logging, UI updates, and NVS writes do not run in timer callbacks. No `delay`, sleep, busy loop, or blocking queue is used.

On cancellation or fault before the LOW edge, no pulse occurs; the durable reservation is either released through a verified newer snapshot when non-delivery is certain or left pending for technician reconciliation. A reset, disconnect, watchdog, or fault during a pulse drives HIGH as soon as software can execute, leaves the transaction pending, marks the outcome uncertain, and blocks further vending. External pull-ups provide the hardware fail-safe while the ESP32 resets. The design does not claim exact physical dispensing because there is no drop/current sensor.

Software timer timing must be verified on hardware with a logic analyzer. Build success alone is not evidence of a 500 ms electrical pulse.

## 11. Protocol and application integration

The existing medical MCP tools remain assessment-only. No `vend`, `select_sku`, `select_channel`, `set_inventory`, or `pulse_relay` tool is exposed. The assessment callback submits validated structured results to the coordinator on the application task.

The coordinator records a candidate only from a local `OFFER`, presents the local confirmation state, and invokes the final guard after the physical button event. Protocol reconnects cannot replay or resume a candidate or transaction. Device-state changes continue through `Application::SetDeviceState()`; callbacks use `Application::Schedule()` or event bits.

Audit logs contain transaction ID, versions/hashes, decision reason, selected local route, inventory revisions, timestamps, and relay outcome. They must not log free-form symptom text, full medication lists, or other unnecessary patient-identifying content.

## 12. Failure behavior

These conditions all result in relay HIGH and no automatic retry: invalid approval; missing or corrupt inventory; catalog mismatch; ambiguous medical result; missing screening answer; stale turn/session; forbidden AI field; timeout; cancellation; disconnect; relay busy; timer creation/start failure; NVS commit failure; reboot; or unknown transaction outcome.

Power loss before the LOW edge is a non-vend. Power loss after the LOW edge creates an uncertain transaction and requires technician reconciliation because this hardware has no feedback sensor. The firmware never assumes success merely because it issued a pulse.

## 13. Verification strategy

Host tests use fake clocks, fake GPIO, and an isolated NVS abstraction to cover:

- approval field/version/hash validation and default lockout;
- forbidden AI control fields and malformed medical inputs;
- danger signs, contraindications, missing facts, and multiple-candidate referral;
- catalog channel uniqueness and exact primary/backup identity matching;
- primary-first and backup-on-empty routing;
- dual-slot CRC recovery, revision selection, pre-pulse reservation, interrupted writes, pending-transaction boot lock, and post-pulse commit failure;
- candidate expiry, stale inventory revision, disconnect, cancellation, idempotency, and concurrent requests;
- exact state transitions `HIGH -> select -> settle -> LOW -> 500 ms -> HIGH -> guard` without blocking;
- reset/fault at every relay state and absence of automatic retry.

Firmware validation includes host script tests, an ESP-IDF 6.1 build for the exact SmartMediVend variant, and hardware tests with no medicine/load connected first. A logic analyzer must verify selector stability, active-LOW polarity, a 500 ms pulse, all-off reset behavior, and back-to-back guard timing. Final tests then verify primary/backup stock behavior and power interruption with real pull-ups.

## 14. Explicitly out of scope

- Creating, changing, or approving clinical rules or contraindications.
- Inventing pharmacist approval metadata.
- Diagnosing disease or vending prescription-only medicine.
- Multiple-SKU transactions or quantity greater than one.
- Remote stock administration or an AI-accessible inventory command.
- Claiming confirmed product delivery without a physical feedback sensor.
- Legal/regulatory certification and the external technician refill application.

## 15. Acceptance criteria

- AI-supplied SKU/channel/relay/stock fields cannot reach a relay decision.
- Incomplete or mismatched pharmacist approval keeps the relay disabled.
- Invalid or absent NVS inventory keeps the relay disabled.
- Exactly one locally eligible canonical item is routed primary-first, then to its exact matching backup.
- Physical confirmation is required, single-use, and expires after 30 seconds.
- Relay control is non-blocking and active LOW for 500 ms after a 10 ms selector settle time.
- Every cancel, error, disconnect, and reset path returns or leaves the relay signal HIGH.
- Inventory is durably reserved before a pulse, and no transaction is automatically replayed after an uncertain outcome.
- Host tests and the target firmware build pass; physical timing and fail-safe behavior are separately documented from hardware measurements.
