# Stage 1 – AI symptom intake and non-dispensing local advisor

## Scope

This patch does not modify Xiaozhi audio, VAD, network, UI, Wi-Fi, button handling,
or any relay GPIO. It adds two **read-only** MCP tools for the SmartMediVend-S3
board. No MCP tool for vend/relay/inventory writes is added. `vend_allowed` is
hardcoded false for every tool result. Existing board constructor continues
to hold the active-low relay signal HIGH.

### Source-of-truth and versioning

`data/medical_rules.json`, `data/medicines.json`, `data/pharmacist_review.json`
are read at CMake *configure* time and placed into a generated header under
`build/`; they are not duplicated manually in C++ source. Editing any of the
three causes CMake reconfiguration on the next `idf.py build`. No `data/` file
is replaced by this patch. `pharmacist_review.json` is currently `approved=false`;
this stage remains non-dispensing even if that file is changed.

### Cloud setup required

Paste/adapt `docs/smartmedivend/XIAOZHI_CLOUD_ROLE_STAGE1.md` into the official
Xiaozhi cloud role/system-prompt UI that serves this device. The repository's
role `.md` file is **not automatically transmitted to cloud** by the embedded
firmware. Tool calls depend on the service's MCP support and role configuration;
a successful firmware build is not proof the cloud followed the workflow.

### MCP contract

- `self.medical.get_intake_schema`: no arguments; returns required keys, supported
  symptom/flag enums and the semantics of empty arrays and missing keys.
- `self.medical.evaluate_symptoms`: `payload_json` string, at most 4096 bytes;
  complete snapshot of explicitly reported facts per turn. Schema:
  `session_id` (non-personal 1–64 char string), `turn_id` (integer 1–1,000,000),
  `age_years` (integer 0–120), `weight_kg` (positive number at most 300),
  `pregnancy_or_breastfeeding` (boolean), `symptoms` (enum array), `duration_hours`
  (number), `danger_signs` (enum array), `conditions` (enum array),
  `current_medicines` (enum array), `drug_allergies` (enum array).

Unknown keys, duplicate keys, oversized/malformed inputs fail closed. Reported
red flags and unsupported age/pregnancy are handled before missing-field checks.
Unknown illness/medication flags and *any* reported drug allergy are referred for
human review rather than silently assumed safe. An empty array signifies an
explicit negative answer, not a missing/unknown answer. The device cannot prove
that AI truthfully extracted a user's spoken response; this is why no result
can authorize dispensing.

`PROVISIONAL_OPTIONS` returns at most **one** local-catalog display option and
no `sku` or `channel` to the cloud. The device resolves the matching main/backup
catalogue slot using **initial_stock only**, which is *not live inventory*.
This is useful for a non-dispensing UI prototype but must not be presented as
currently available stock. An independently verified and persisted inventory
manager, broader contraindication/interaction review, local user confirmation,
pharmacist review and VendGuard are required before any future physical vending.

### Example MCP `payload_json` (TEST DATA ONLY)

```json
{"session_id":"demo-1","turn_id":1,"age_years":30,"weight_kg":65,"pregnancy_or_breastfeeding":false,"symptoms":["mild_headache"],"duration_hours":3,"danger_signs":[],"conditions":[],"current_medicines":[],"drug_allergies":[]}
```

### Tests and deployment

On a Linux host with `g++` and system `libcjson.so.1`:

```bash
g++ -std=c++23 -O2 -Wall -Wextra -Werror \
 -Imain/medical/tests/host_include -Imain/medical \
 main/medical/medical_advisor.cc main/medical/tests/test_medical_advisor.cc \
 -Wl,-l:libcjson.so.1 -o /tmp/medical_test
/tmp/medical_test data/medical_rules.json data/medicines.json data/pharmacist_review.json
python3 -m unittest discover -s scripts/tests -v
```

`main/medical/tests/host_include/cJSON.h` is a **host-test-only ABI shim** for
systems with the runtime library but without development headers. It must never
be added to the firmware include path; ESP-IDF uses its own `cJSON.h`.

On the user's ESP-IDF 6.1 setup (correct SmartMediVend-S3 board selected):

```bash
idf.py build
idf.py -p COMx flash monitor
```

Check serial for `MCP: Add tool: self.medical.get_intake_schema` and
`MCP: Add tool: self.medical.evaluate_symptoms`; then verify the service
actually calls them and that the JSON response is seen by the cloud. Test
missing data, dangerous symptoms, ages under 16, pregnancy/breastfeeding,
allergy, contraindications, and 30+ consecutive normal multi-turn sessions.
No physical dispensing or medication trials are authorized by this patch.
