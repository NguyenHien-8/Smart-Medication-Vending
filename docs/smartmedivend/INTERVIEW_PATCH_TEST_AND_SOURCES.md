# Stage 1B — short-question interview, local checks, source notes

## Observed evidence from the uploaded conversation logs
- Log 1: the AI asks both "thuốc gì" and "tiền sử" in the same turn, later combines age and weight, then medication and drug allergy. A single "không có" after a two-part question cannot safely clear both fields. After the long danger-sign question, the transcript includes "cứng cổ..." but the system still presented a provisional option; this is a transcription/negation ambiguity that firmware cannot independently resolve.
- Log 2: AI initially requests abdomen location, pain type, duration, nausea, fever, and diarrhea together. "Cảm ơn" is not an answer to outstanding medical questions, and "không có" cannot negate two independently unanswered fields. Unspecified abdominal pain must not be mapped to flatulence/acid-indigestion/constipation on guesswork.

## Changes
- Catalog's `symptom_group` expanded for 13 different medicine groups, propagated to all three backup channels; `differentiate_and_escalate` and `information_source` are descriptive guidance, not new indications or clinical authorization. `initial_stock` remains an unverified snapshot.
- Rules add versioned `interview.global_checks` and `interview.symptom_checks` for all 15 supported symptom enums. No historical exclusion or indication rule removed. Any `REFER` mismatch prevents a provisional option. `CLARIFY` mismatch calls for reclassification rather than guessing.
- MCP schema returns descriptions + check ids. The local evaluator returns exactly ONE `next_question_id` / `next_question_vi` and requires an explicit boolean in `screening_answers` for every applicable question; unanswered questions are repeated deterministically. Response always sets `vend_allowed=false`.
- AI must only ask one question per turn and preserve an explicit full snapshot; stage-2 cloud role is a separate file that needs manual installation on Xiaozhi Cloud. Device cannot prove the AI accurately transcribed a user, and there is no microphone-independent confirmation. **Do not use this pilot to dispense drugs.**

## Medical reference links (background only; not locally approved prescribing guidance)
- Fever / mild headache / body ache and paracetamol: https://www.nhs.uk/medicines/paracetamol-for-adults/ ; headache urgent signs: https://www.nhs.uk/symptoms/headaches/
- Mild inflammatory pain and ibuprofen: https://www.nhs.uk/medicines/ibuprofen-for-adults/
- Allergic rhinitis and loratadine: https://www.nhs.uk/medicines/loratadine/about-loratadine/
- Dry cough and dextromethorphan: https://medlineplus.gov/druginfo/meds/a682492.html ; cough types: https://medlineplus.gov/ency/article/003072.htm
- Productive cough/ambroxol and serious skin reactions: https://www.ema.europa.eu/en/medicines/human/referrals/ambroxol-bromhexine-containing-medicines
- Mild sore throat red flags: https://www.nhs.uk/symptoms/sore-throat/
- Trapped gas and simethicone: https://medlineplus.gov/druginfo/meds/a682683.html
- Heartburn and antacid safety: https://medlineplus.gov/ency/patientinstructions/000198.htm
- Reflux and omeprazole: https://www.nhs.uk/conditions/heartburn-and-acid-reflux/ ; https://www.nhs.uk/medicines/omeprazole/
- Acute watery diarrhea and loperamide: https://www.nhs.uk/medicines/loperamide/who-can-and-cannot-take-loperamide/
- Constipation and bisacodyl: https://www.nhs.uk/medicines/bisacodyl/about-bisacodyl/ ; https://www.nhs.uk/medicines/bisacodyl/who-can-and-cannot-take-bisacodyl/
- Motion sickness and dimenhydrinate: https://medlineplus.gov/druginfo/meds/a607046.html
- Saccharomyces boulardii safety and central venous catheters: https://www.ema.europa.eu/en/documents/psusa/saccharomyces-boulardii-cmdh-scientific-conclusions-and-grounds-variation-amendments-product-information-and-timetable-implementation-psusa00009284201702_en.pdf
- Generic abdominal pain red flags: https://www.nhs.uk/symptoms/stomach-ache/

These are UK/US/EU informational sources, not verification of the exact Vietnamese product brand, marketed indication, legal OTC status, or a pharmacist's review. The project's lower-age bound of 16 is NOT supported for all products by NHS adult-only source pages. Any 16–17-year-old must receive no real-world vending pending product-specific review. Medicines with an illustrative strength, unknown tablets-per-blister, or unverified inventory must not be dispensed.

## Build and test
From repository root, host tests:
```
g++ -std=c++23 -O2 -Wall -Wextra -Werror \
 -Imain/medical/tests/host_include -Imain/medical \
 main/medical/medical_advisor.cc main/medical/tests/test_medical_advisor.cc \
 -Wl,-l:libcjson.so.1 -o /tmp/smv_interview_test
/tmp/smv_interview_test data/medical_rules.json data/medicines.json data/pharmacist_review.json
python3 -m unittest discover -s scripts/tests -v
```
On an ESP-IDF machine: `idf.py build` then flash/monitor on a device without energized relay loads. Check each of 15 symptom profiles, a missing answer (the same `next_question_id` must repeat), contradictory answers, one red flag, unknown symptom, and the 2 log scenarios. Confirm remote Xiaozhi role is actually updated. Full ESP32 firmware build, cloud integration, audible intelligibility, and real-world medication safety CANNOT be inferred from host tests.
