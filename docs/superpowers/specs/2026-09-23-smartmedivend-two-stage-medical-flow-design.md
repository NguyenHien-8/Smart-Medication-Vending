# SmartMediVend two-stage medical flow design

Status: Approved for implementation planning

Date: 2026-09-23

Target: ESP-IDF 6.1, ESP32-S3, `smartmedivend-s3` and a new
`smartmedivend-s3-production` release variant

Supersedes only the conversational evaluation and operational-enablement portions of
`2026-09-22-smartmedivend-fail-closed-vending-design.md`. The approved active-LOW relay,
physical-confirmation, durable-inventory, and no-cloud-relay boundaries remain in force.

## 1. Intent and success criteria

The machine should normally reach a physical-confirmation offer for a mild, supported case when
the user has explicitly supplied the small set of relevant safety facts. It must not reject a
mild case merely because an answer is unclear, an unrelated medical field is absent, or one of
several possible medicines is unsuitable while another pharmacist-approved option remains.

The conversation must respond quickly. Rules, catalog data, questions, and approval metadata are
parsed and validated once during board initialization. Each spoken turn submits only new facts,
updates one bounded in-memory session, and runs a fast indexed screen. A full medical evaluation
runs once when the fast screen says that the case is ready, immediately before an offer is staged.

Success means:

- `next_question_vi` reaches the conversational AI unchanged for every `ASK` result;
- missing or ambiguous non-dangerous information produces another concise question, not a denial;
- explicit danger signs and hard referral conditions still produce `REFER` and never a relay pulse;
- one locally eligible, pharmacist-approved medicine can reach `OFFER`, physical confirmation,
  inventory reservation, channel selection, and one 500 ms active-LOW relay pulse;
- no rule/catalog JSON is reparsed on each conversational turn;
- the default development variant remains electrically safe, while an explicit production build
  variant enables the relay gate without requiring a manual `menuconfig` edit;
- a technician can provision or reconcile actual stock locally over USB serial, including recovery
  from missing/corrupt inventory or an explicitly inspected unfinished transaction.

This firmware provides symptom screening and a pharmacist-approved OTC option. It does not claim
to diagnose disease and does not allow the AI to select a SKU, channel, quantity, or relay.

## 2. Decision semantics: permissive for uncertainty, strict for real hazards

The engine uses four outcomes with non-overlapping meanings:

- `ASK`: a relevant fact is absent, unclear, inconsistent, or uses an unrecognized medical value.
  The response contains exactly one `next_question_vi`. This is the normal outcome while gathering
  information and is not a vending failure.
- `REFER`: the user explicitly reports a global danger sign, a rule marked `refer_if`, an
  unsupported population such as age below the approved minimum, or pregnancy/breastfeeding when
  the approved policy does not support it. It also covers a completed case for which no local OTC
  choice can be made safely.
- `BLOCK`: local data or transaction integrity is unavailable: malformed/unauthorized input shape,
  invalid policy artifacts, invalid pharmacist approval, production disabled at offer time,
  unavailable inventory, stale/replayed turns, or a relay/transaction fault. `BLOCK` is not used
  merely because a patient answer is incomplete.
- `OFFER`: the full evaluation found one deterministic locally approved canonical item and all
  operational gates needed to stage physical confirmation are valid.

An `exclude_if` removes only the affected medicine from the candidate set. It does not refer the
whole session if another pharmacist-approved candidate remains. A `refer_if` remains a hard
clinical referral. An unrelated allergy, condition, or medicine does not reject all catalog items;
it is matched only against the remaining candidates' normalized flags.

If more than one medicine remains, firmware may select one only when the pharmacist-authored data
contains a unique priority for the primary symptom. A tie produces a configured discriminator
question when available; otherwise it produces `REFER`. File order is never a selection rule.

The always-required patient facts are age, weight, pregnancy/breastfeeding status, one primary
symptom, duration, an explicit danger-screen result, conditions, current medicines, and drug
allergies. Empty lists are valid only when the user explicitly said “none.” After those facts are
known, only questions relevant to the primary symptom and the remaining candidate medicines are
asked. The engine does not ask every question in the complete rule set.

## 3. Boot-time policy cache

A new `MedicalPolicyCache` owns immutable, typed medical policy. Its constructor receives the
embedded rule and catalog bytes and performs all expensive parsing and cross-validation once.
The cache exposes readiness plus a stable failure reason; it never silently treats malformed
arrays such as `symptoms`, `exclude_if`, or `refer_if` as empty.

The normalized cache contains bounded collections for:

- scope, versions, and supported schema;
- canonical medicine identity and catalog display fields;
- primary/backup physical routes;
- symptom-to-rule indices;
- normalized condition, medicine, allergy, danger, and derived-fact bitsets;
- global and symptom-specific questions indexed by ID;
- candidate-specific exclusions, hard referrals, age limits, and pharmacist-authored priority;
- question applicability and optional discriminator questions.

Validation rejects duplicate IDs, duplicate channels, out-of-range channels, unsupported units,
missing canonical references, primary/backup identity mismatch, missing question text, invalid
question actions, unbounded arrays/strings, missing symptom profiles, and rule/catalog canonical-ID
set mismatch. The supported physical unit remains one sealed blister. A policy validation failure
keeps GPIO17 HIGH and reports one stable diagnostic reason.

`MedicalAdvisor` and `CatalogRouter` consume the same immutable cache. They do not each parse their
own copy of the artifacts and do not retain raw JSON for per-turn processing. Pharmacist approval
continues to bind the exact embedded rule and catalog bytes by version and SHA-256.

## 4. Incremental session model

The device stores at most one active `MedicalIntakeSession`. It contains the session ID, highest
accepted turn ID, normalized facts, screening answers, primary symptom, a facts revision, and the
last requested question ID. Collections are bounded; no transcript or free-form speech is stored.
The session expires after ten minutes of inactivity and is cleared on disconnect, cancellation,
reboot, or a new session ID.

`self.medical.evaluate_symptoms` keeps one `payload_json` property, but the payload becomes a turn
delta rather than a complete snapshot. Example:

```json
{
  "session_id": "local-session-17",
  "turn_id": 4,
  "screening_answers": {"redflag_breathing": false}
}
```

Every present scalar replaces that fact. Every present medical array replaces that category; an
explicit empty array means the user answered “none.” `screening_answers` merges by question ID.
Fields omitted from a turn keep their prior value. Unknown control fields, SKU/channel/stock/relay
fields, duplicate JSON keys, excessive lengths, invalid types, and out-of-order turns are rejected.

A user correction is accepted on a newer turn. It always invalidates a staged offer. Changing the
primary symptom clears answers that belong only to the previous symptom, but preserves unrelated
demographic and global-safety facts. A new danger sign takes effect immediately even if the full
evaluation previously succeeded.

For compatibility during rollout, a first turn may contain all known fields. Subsequent prompts
instruct the AI to send only newly stated or corrected facts plus `session_id` and `turn_id`.

## 5. Stage one: fast conversational screening

`FastScreen` runs after a turn delta is merged. It operates on normalized values and indices, not
on the rule/catalog JSON tree. Its ordered work is:

1. Validate session identity, monotonic turn ID, bounded types, and forbidden control fields.
2. Apply explicit danger signs and global hard-referral conditions immediately.
3. Ask for the next missing always-required fact.
4. Build the small candidate-rule set from the primary-symptom index.
5. Remove candidates excluded by already-known facts and stop for any applicable `refer_if`.
6. Ask the highest-priority unanswered question that is relevant to the primary symptom or a
   remaining candidate.
7. Return `READY_FOR_FULL_EVALUATION` when all decisive facts are present.

The question order favors rapid safe completion: primary symptom, global danger confirmation,
age/pregnancy, duration/weight, conditions/medicines/allergies, one symptom-classification question,
then candidate-specific contraindication questions. A response that cannot be mapped confidently
causes the same pending question to be repeated or clarified; silence is never converted to
`false`.

The fast path must not scan all catalog slots or all medicine rules. Its cost is bounded by the
turn payload plus the questions and candidates indexed to the primary symptom.

## 6. Stage two: full evaluation before an offer

`FullEvaluate` runs only after `FastScreen` reports ready, and runs again only after a later accepted
fact correction. It rechecks the complete normalized session against the immutable policy, but only
examines rules referenced by the primary symptom. It then:

1. rechecks global danger/referral facts and the supported population;
2. applies candidate minimum age, `refer_if`, and `exclude_if` semantics;
3. confirms every candidate-relevant safety answer is explicit;
4. resolves one canonical item by a unique pharmacist-authored priority or discriminator;
5. returns a structured `MedicalOffer` without a channel or SKU;
6. hands the offer to `VendingCoordinator` for approval, catalog-route, inventory, transaction-ID,
   and candidate-expiry checks.

The coordinator never parses display JSON. It consumes the structured result. A full-evaluation
cache is keyed by session ID, turn ID, facts revision, and policy identity, so a duplicate internal
read cannot redo the work or stage a second transaction.

## 7. Response contract and `next_question_vi`

`MedicalEvaluation` gains structured response fields, including `next_question_vi`, while retaining
one serializer for MCP output. `VendingCoordinator::EvaluateAndStage()` follows these rules:

- for `ASK`, return the advisor response including `next_question_vi`, session ID, turn ID, missing
  field/question ID, and `vend_allowed:false`;
- for `REFER`, return the advisor's user-safe reason and no medicine or relay details;
- for `BLOCK`, return a stable operational reason and no medicine or relay details;
- for a medical offer that passes operational gates, return the local name, active ingredient,
  strength, `confirmation:"PRESS_PHYSICAL_BUTTON"`, and no channel/SKU;
- for an otherwise valid medical offer whose operational gate fails, report the exact operational
  category (`PRODUCTION_DISABLED`, `PHARMACIST_REVIEW_INVALID`, `INVENTORY_UNAVAILABLE`,
  `OUT_OF_STOCK`, or transaction/relay fault) instead of discarding the medical state.

The AI system prompt must speak only `next_question_vi` for `ASK`, stop for `REFER/BLOCK`, and for
`OFFER` instruct the user to press the physical button after speech ends. Voice cannot confirm a
vend.

## 8. Pharmacist-approved data and deterministic selection

The implementation may change schemas and mechanics, but it must not invent clinical approval or a
reviewer's identity. The release artifacts require a real pharmacist to supply `reviewer`,
`reviewer_license`, and RFC 3339 `reviewed_at`, and to approve the final exact rules and catalog.

A host-side approval preparation command computes the final rule/catalog versions and SHA-256
values and emits a review template with those values. The real reviewer fields remain visibly
empty until supplied. A development build may run with an invalid record for testing, but production
vending remains locked until the embedded record verifies.

The current channel-15 antacid backup must use exactly the same canonical ID, product name, active
ingredient, strength, and dispense unit as channel 7. The implementation can make the backup copy
the already-declared primary identity mechanically; it must not guess a new strength or claim that
the existing “ví dụ” value is pharmacist-approved. Final package-label text is part of pharmacist
release approval.

The policy schema adds explicit unique selection priority where a symptom can map to multiple
medicines. Priority is clinical data and becomes usable only after the pharmacist approves the
updated policy bytes.

## 9. Production build variant

`main/boards/smartmedivend-s3/config.json` gains a uniquely named
`smartmedivend-s3-production` build whose appended configuration enables
`CONFIG_SMARTMEDIVEND_PRODUCTION_VENDING=y` and USB Serial/JTAG console support. The existing
`smartmedivend-s3` build remains the safe development/simulation variant with physical vending
disabled. Both variants use the same GPIO17/GPIO39–42 board identity and exactly one board factory.

The production flag enables only the compile-time gate. Runtime approval, policy, inventory,
transport, confirmation, and relay checks still apply. Startup logs and the status display report
each gate separately so an operator can distinguish “disabled build” from “approval invalid,”
“inventory unavailable,” or “relay unavailable.”

## 10. Local inventory provisioning and reconciliation

The production board registers a board-owned maintenance console on USB Serial/JTAG. It is not an
MCP tool and is never reachable through WebSocket, MQTT, or conversational AI. The console exposes:

```text
smv_inventory status
smv_inventory provision --catalog-sha256 <64-hex> --counts <c0,...,c15>
smv_inventory reconcile --catalog-sha256 <64-hex> --counts <c0,...,c15>
```

`status` is read-only and prints inventory result, revision, valid channel mask, counts, and pending
transaction identity without patient data. `provision` is accepted only when there is no pending
transaction and no relay/candidate activity. `reconcile` is accepted only after a technician has
physically counted all channels; it is the explicit recovery path for corrupt/missing snapshots or
an unfinished transaction. Both write and read back two verified snapshots bound to the current
catalog SHA-256, accept exactly 16 counts in range 0 through 10000, keep GPIO17 HIGH, invalidate any
candidate, and require reboot before vending.

This makes “inventory unavailable” recoverable without weakening transaction safety. Firmware
never seeds actual stock from catalog `initial_stock`, automatically clears a pending transaction,
or assumes whether an uncertain pulse dispensed an item.

## 11. Relay, timer, and disconnect hardening

The existing 10 ms selector settle, 500 ms active-LOW pulse, and 100 ms all-off guard remain
non-blocking. Settle, pulse, and guard use distinct timer slots. Each arm owns an immutable callback
lease until its callback returns or a successful stop proves it cannot run; a queued stale callback
cannot observe a later arm's generation or context. Every cancel/fault path raises GPIO17 HIGH first.

Application initialization registers the shared protocol `OnDisconnected` callback and schedules
board mutation through `Application::Schedule()`. Both MQTT and WebSocket transports invoke that
callback on transport loss. A disconnect clears the intake session and staged candidate and cancels
an active relay transaction according to the existing certain/uncertain outcome rules.

Physical confirmation still rechecks production enablement, approval identity, candidate/facts
revision, current catalog route, current inventory revision/count, no pending transaction,
transport health, application idle state, relay idle state, and single-use transaction ID. These
checks are operational integrity, not extra medical questioning.

## 12. Component boundaries

- `main/medical/medical_policy_cache.*`: one-time parse, full schema validation, normalized indices.
- `main/medical/medical_intake_session.*`: bounded delta merge, revisions, correction invalidation.
- `main/medical/medical_advisor.*`: `FastScreen`, `FullEvaluate`, and response serialization.
- `main/vending/catalog_router.*`: route lookup from normalized approved catalog and current stock.
- `main/vending/vending_coordinator.*`: candidate lifecycle, operational gates, and response merge.
- `main/inventory/inventory_store.*`: catalog-bound dual snapshots and explicit reconciliation API.
- `main/boards/smartmedivend-s3/smartmedivend_maintenance_console.*`: local USB commands only.
- `main/boards/smartmedivend-s3/smartmedivend_board.cc`: board wiring and physical button behavior.
- `main/application.*` and `main/protocols/*`: transport-loss propagation through shared interfaces.

Core modules never include the SmartMediVend board `config.h`. Board-specific pins and console
wiring stay in the board directory. Timer/protocol callbacks schedule application mutations and do
not parse JSON, write NVS, update UI, or block.

## 13. Performance and resource requirements

- Rule/catalog/review parsing count after board initialization: zero per conversational turn.
- Fast screen: one bounded turn-delta parse, indexed lookups, no full catalog scan, no NVS write.
- Full evaluation: at most once per accepted facts revision before an offer.
- Maximum MCP payload remains 4096 bytes; normal answer deltas should be much smaller.
- Session and policy collections have compile-time bounds suitable for the 16-channel machine.
- No unbounded queue, sleep, delay, busy wait, or large repeated allocation is added to the audio,
  main-event, protocol, or timer paths.
- Instrumentation records boot policy-parse time, fast-screen time, full-evaluation time, and outcome
  reason without logging free-form speech or medical lists.

The hardware acceptance target is that local fast screening is not perceptible relative to network
and speech latency. Measurements, rather than an assumed millisecond threshold, are recorded on the
ESP32-S3; any fast-screen outlier above 20 ms or full evaluation above 50 ms is investigated before
release.

## 14. Verification strategy

Host tests use real approved-shape fixtures plus invalid fixtures to cover:

- policy parsed once and reused over many turns;
- malformed `symptoms`, `exclude_if`, `refer_if`, questions, duplicate IDs/channels, and unmatched
  canonical IDs lock policy instead of becoming empty rules;
- delta merge, explicit empty arrays, correction behavior, session expiry, stale turns, disconnect,
  and forbidden AI control fields;
- `ASK` plus exact `next_question_vi` forwarding through `VendingCoordinator`;
- incomplete/ambiguous mild cases remain `ASK`, not `BLOCK` or `REFER`;
- explicit danger and `refer_if` remain `REFER`;
- candidate-specific exclusion leaves another safe candidate eligible;
- unique pharmacist priority resolves multiple candidates, while ties do not use file order;
- full evaluation runs once per facts revision and reruns after correction;
- invalid approval, unavailable/corrupt/pending inventory, out-of-stock routes, and disabled
  production build prevent LOW output with distinct reasons;
- USB provision/reconcile parsing, exact 16-count validation, catalog-hash binding, dual-snapshot
  readback, pending recovery, and reboot requirement;
- MQTT and WebSocket disconnects invalidate sessions/candidates and cancel relay work;
- timer cancellation and stale queued callbacks cannot trigger or extend a LOW pulse.

Integration validation builds both SmartMediVend variants with ESP-IDF 6.1 and runs the medical and
vending host suites. Hardware validation first uses no medicine/load and verifies selector bits,
active-LOW polarity, 10/500/100 ms timing, reset pull-ups, disconnect/cancel behavior, and USB stock
provisioning. A final supervised test provisions known counts, exercises primary and backup routes,
confirms one count decrement, and records the electrical pulse separately from physical product
delivery.

## 15. Release dependencies and explicit exclusions

Code implementation can complete without fabricating medical sign-off, but a production relay test
with medicine loaded depends on all of the following external facts being supplied and verified:

- exact final product identity/strength text for every primary and backup slot;
- pharmacist-approved final rule and catalog bytes;
- real reviewer name, license, and timestamp;
- technician-counted stock for all 16 channels;
- physical pull-ups and relay/multiplexer wiring matching the approved pin plan.

Remote inventory administration, cloud relay control, automatic pending-transaction recovery,
prescription-only medicines, diagnosis claims, multiple-item transactions, and inferred delivery
success remain out of scope.

## 16. Acceptance criteria

- A mild supported case with explicit relevant safety answers reaches `OFFER`; uncertainty is handled
  by `ASK` and a forwarded `next_question_vi`.
- Only explicit hard clinical risk produces `REFER`; only integrity/configuration faults produce
  `BLOCK`.
- ESP32 parses static medical JSON once at boot and does not scan all medicines after every answer.
- Full evaluation runs before every offer and never after physical confirmation as a substitute for
  the final operational guard.
- AI cannot choose or observe relay channel, stock count, SKU routing, or pulse control.
- The production build variant enables the compile gate; valid pharmacist approval and locally
  reconciled NVS stock make the normal offer-confirm-reserve-route-pulse path reachable.
- Primary routing is preferred; exact matching backup is used only when primary stock is zero.
- One button confirmation starts at most one 500 ms active-LOW pulse and decrements one unit once.
- Disconnect, correction, timeout, cancel, reset, corrupt data, or timer ambiguity cannot cause a
  stray LOW transition or an automatic retry.
- Both build variants and all host tests pass; physical timing and product delivery are reported as
  separate hardware evidence.
