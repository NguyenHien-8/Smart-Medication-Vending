#include "medical_advisor.h"

#include <cJSON.h>
#include <cmath>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace smv {
namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
Json Parse(const std::string& text) {
    return Json(cJSON_ParseWithLengthOpts(text.c_str(), text.size() + 1, nullptr, true), &cJSON_Delete);
}
Json Make() { return Json(cJSON_CreateObject(), &cJSON_Delete); }
const cJSON* Get(const cJSON* o, const char* key) { return cJSON_GetObjectItemCaseSensitive(o, key); }
const char* Str(const cJSON* node) { return cJSON_IsString(node) ? node->valuestring : nullptr; }
void Put(cJSON* o, const char* k, const char* v) {
    cJSON_DeleteItemFromObjectCaseSensitive(o, k);
    cJSON_AddStringToObject(o, k, v);
}
void AddText(cJSON* a, const std::string& v) { cJSON_AddItemToArray(a, cJSON_CreateString(v.c_str())); }
std::string Serialize(const cJSON* j) {
    if (!j) return "{\"status\":\"DENY\",\"vend_allowed\":false}";
    std::unique_ptr<char, decltype(&cJSON_free)> out(cJSON_PrintUnformatted(j), &cJSON_free);
    return out ? out.get() : "{\"status\":\"DENY\",\"vend_allowed\":false}";
}
std::set<std::string> ReadEnumArray(const cJSON* value, bool& valid, size_t max = 24) {
    std::set<std::string> result;
    if (!cJSON_IsArray(value) || cJSON_GetArraySize(value) > static_cast<int>(max)) {
        valid = false;
        return result;
    }
    const cJSON* entry = nullptr;
    cJSON_ArrayForEach(entry, value) {
        const char* s = Str(entry);
        if (!s || !*s || std::strlen(s) > 64 || !result.insert(s).second) valid = false;
    }
    return result;
}
bool HasAny(const std::set<std::string>& values, const cJSON* names) {
    if (!cJSON_IsArray(names)) return false;
    const cJSON* n = nullptr;
    cJSON_ArrayForEach(n, names) {
        const char* s = Str(n);
        if (s && values.count(s)) return true;
    }
    return false;
}
void AddAll(std::set<std::string>& target, const cJSON* a) {
    if (!cJSON_IsArray(a)) return;
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, a) { if (const char* s = Str(item)) target.insert(s); }
}
std::string Error(const char* status, const char* reason) {
    auto o = Make();
    Put(o.get(), "status", status);
    Put(o.get(), "reason", reason);
    cJSON_AddBoolToObject(o.get(), "vend_allowed", false);
    return Serialize(o.get());
}
// The device chooses ONE short question from the authoritative rule set.
// No answer is inferred from silence, unrelated utterances or a cloud guess.
void NextQuestion(cJSON* out, const cJSON* check) {
    const char* id = Str(Get(check, "id"));
    const char* question = Str(Get(check, "question_vi"));
    if (id && question) {
        Put(out, "next_question_id", id);
        Put(out, "next_question_vi", question);
    }
}
void NextField(cJSON* out, const char* field) {
    struct FieldQuestion { const char* key; const char* question; };
    static const FieldQuestion kQuestions[] = {
        {"symptoms", "Bạn đang khó chịu ở đâu?"},
        {"danger_signs", "Bạn có dấu hiệu nguy hiểm nào khác không?"},
        {"age_years", "Bạn bao nhiêu tuổi?"},
        {"duration_hours", "Triệu chứng bắt đầu từ khi nào?"},
        {"weight_kg", "Bạn nặng bao nhiêu kilôgam?"},
        {"pregnancy_or_breastfeeding", "Bạn có đang mang thai hoặc cho con bú không?"},
        {"conditions", "Bạn có bệnh nền nào không?"},
        {"current_medicines", "Bạn đang sử dụng những thuốc gì?"},
        {"drug_allergies", "Bạn có dị ứng với thuốc nào không?"},
    };
    for (const auto& q : kQuestions) if (std::strcmp(field, q.key) == 0) {
        Put(out, "next_question_id", field);
        Put(out, "next_question_vi", q.question);
        return;
    }
}
const cJSON* FindCheck(const cJSON* interview, const char* id) {
    const cJSON* check = nullptr;
    cJSON_ArrayForEach(check, Get(interview, "global_checks")) {
        if (const char* key = Str(Get(check, "id")); key && std::strcmp(id,key)==0) return check;
    }
    const cJSON* profiles = Get(interview, "symptom_checks");
    const cJSON* profile = nullptr;
    cJSON_ArrayForEach(profile, profiles) {
        cJSON_ArrayForEach(check, profile) {
            if (const char* key = Str(Get(check, "id")); key && std::strcmp(id,key)==0) return check;
        }
    }
    return nullptr;
}
// A single positive danger answer must stop the workflow, even when other fields are missing.
bool IsPositiveDanger(const cJSON* interview, const cJSON* answers) {
    if (!cJSON_IsObject(answers)) return false;
    const cJSON* a = nullptr;
    cJSON_ArrayForEach(a, answers) {
        const cJSON* check = a->string ? FindCheck(interview, a->string) : nullptr;
        if (check && cJSON_IsTrue(a) && cJSON_IsBool(Get(check, "expected")) && !cJSON_IsTrue(Get(check, "expected")) &&
            std::strcmp(Str(Get(check, "on_mismatch")) ? Str(Get(check, "on_mismatch")) : "", "REFER") == 0)
            return true;
    }
    return false;
}

std::set<std::string> AllowedSymptoms(const cJSON* rules) {
    std::set<std::string> result;
    const cJSON* r = nullptr;
    cJSON_ArrayForEach(r, Get(rules, "medicine_rules")) AddAll(result, Get(r, "symptoms"));
    return result;
}
// A rule contains flags that may apply to conditions, medication, allergy, or symptom history.
std::set<std::string> AllowedFlags(const cJSON* rules) {
    std::set<std::string> result;
    const cJSON* r = nullptr;
    cJSON_ArrayForEach(r, Get(rules, "medicine_rules")) {
        AddAll(result, Get(r, "exclude_if"));
        AddAll(result, Get(r, "refer_if"));
    }
    AddAll(result, Get(rules, "global_danger_signs"));
    // These are explicitly rule-enumerated symptoms which can also serve as flags.
    return result;
}
bool Within(const std::set<std::string>& set, const std::set<std::string>& allowed) {
    for (const auto& s : set) if (!allowed.count(s)) return false;
    return true;
}
const cJSON* FindSlot(const cJSON* catalog, const char* canonical, bool backup) {
    const cJSON* slots = Get(catalog, "slots");
    const cJSON* slot = nullptr;
    cJSON_ArrayForEach(slot, slots) {
        const char* id = Str(Get(slot, "canonical_id"));
        if (id && std::strcmp(id, canonical) == 0 &&
            (cJSON_IsNumber(Get(slot, "backup_of_channel")) == backup)) return slot;
    }
    return nullptr;
}
}

MedicalAdvisor::MedicalAdvisor(const char* rules, const char* catalog, const char* review)
    : rules_(rules ? rules : ""), catalog_(catalog ? catalog : ""), review_(review ? review : "") {}

std::string MedicalAdvisor::IntakeSchema() const {
    // The legacy response copied every screening question, 13 long drug guides,
    // and dozens of flags into a single MQTT PUBLISH. Only expose the concise
    // intake contract here; Evaluate() reads all authoritative local rules and
    // returns exactly ONE applicable question on each call.
    const auto rules = Parse(rules_);
    const auto catalog = Parse(catalog_);
    if (!rules || !catalog || !cJSON_IsObject(Get(rules.get(), "interview")) ||
        !cJSON_IsObject(Get(Get(rules.get(), "interview"), "symptom_checks")) ||
        !cJSON_IsArray(Get(rules.get(), "global_required_fields")))
        return Error("DENY", "LOCAL_DATA_UNAVAILABLE");

    auto out = Make();
    Put(out.get(), "purpose", "INTAKE_ONLY_NO_DIAGNOSIS_NO_VEND");
    Put(out.get(), "instruction", "Describe the user's symptoms without assuming mildness or a diagnosis. Call evaluate_symptoms after EACH user reply using only facts explicitly stated. Ask only next_question_vi, with no preface or extra question. Unclear, incomplete or unrelated reply: repeat the pending question without adding an answer. Unknown fields are OMITTED, never false or []. Never suggest a medicine before a device result.");
    cJSON* fields = cJSON_Duplicate(Get(rules.get(), "global_required_fields"), 1);
    if (!fields) return Error("DENY", "LOCAL_DATA_UNAVAILABLE");
    cJSON_AddItemToObject(out.get(), "required_fields", fields);

    // Brief symptom-to-enum hints, NOT medicine names and NOT clinical approval.
    // The local symptom checks and exclusion rules remain in medical_rules.json.
    static constexpr struct { const char* id; const char* vi; } kLabels[] = {
        {"fever", "sốt đo nhiệt độ"}, {"mild_headache", "đau đầu nhẹ; chưa rõ mức độ thì hỏi lại"},
        {"mild_body_ache", "đau mỏi nhẹ"}, {"mild_inflammatory_pain", "đau cơ hay khớp nhẹ"},
        {"allergic_rhinitis", "hắt hơi, ngứa mũi/mắt do dị ứng"},
        {"dry_cough", "ho không đờm"}, {"productive_cough", "ho có đờm"},
        {"mild_sore_throat", "đau rát họng nhẹ"}, {"gas_bloating", "đầy hơi, ợ hơi"},
        {"acid_indigestion", "ợ nóng, khó tiêu acid"}, {"short_term_reflux", "ợ chua, trào ngược"},
        {"acute_watery_diarrhoea", "tiêu chảy phân nước"},
        {"short_term_constipation", "táo bón ngắn hạn"},
        {"motion_sickness", "buồn nôn chỉ khi đi tàu xe"},
        {"digestive_support", "hỗ trợ tiêu chảy; không dùng cho đau bụng đơn độc"}
    };
    const auto supported = AllowedSymptoms(rules.get());
    auto* groups = cJSON_CreateObject();
    if (!groups) return Error("DENY", "LOCAL_DATA_UNAVAILABLE");
    cJSON_AddItemToObject(out.get(), "symptom_labels_vi", groups);
    for (const auto& item : kLabels) {
        if (supported.count(item.id)) Put(groups, item.id, item.vi);
    }
    if (cJSON_GetArraySize(groups) != static_cast<int>(supported.size()))
        return Error("DENY", "UNMAPPED_SYMPTOM");
    Put(out.get(), "unknown_semantics", "Omit unknowns. [] only after explicitly asked and denied; 'no' answers ONLY the one pending question.");
    Put(out.get(), "stock_semantics", "Initial catalog stock unverified; dispensing disabled.");
    cJSON_AddBoolToObject(out.get(), "vend_allowed", false);
    const std::string response = Serialize(out.get());
    // Bound the *text* result as well as the transport. Fail closed if someone
    // later grows this bootstrap schema into another oversized MQTT message.
    return response.size() <= 3200 ? response : Error("DENY", "INTAKE_SCHEMA_TOO_LARGE");
}


std::string MedicalAdvisor::SymptomGuide(const std::string& symptom_enum) const {
    // Detailed guidance is fetched for ONE symptom only, not bundled into a
    // 13-medicine schema response. This tool cannot issue any drug authorization.
    if (symptom_enum.empty() || symptom_enum.size() > 64)
        return Error("DENY", "INVALID_SYMPTOM_ENUM");
    const auto rules = Parse(rules_);
    const auto catalog = Parse(catalog_);
    if (!rules || !catalog) return Error("DENY", "LOCAL_DATA_UNAVAILABLE");
    const auto supported = AllowedSymptoms(rules.get());
    if (!supported.count(symptom_enum)) return Error("DENY", "UNKNOWN_SYMPTOM");
    const auto* checks = Get(Get(Get(rules.get(), "interview"), "symptom_checks"), symptom_enum.c_str());
    if (!cJSON_IsArray(checks) || !cJSON_GetArraySize(checks))
        return Error("DENY", "INVALID_INTERVIEW_CONFIG");
    auto out = Make();
    Put(out.get(), "symptom_enum", symptom_enum.c_str());
    auto* questions = cJSON_Duplicate(checks, 1);
    if (!questions) return Error("DENY", "LOCAL_DATA_UNAVAILABLE");
    cJSON_AddItemToObject(out.get(), "checks_reference_only", questions);
    const cJSON* rule = nullptr;
    const cJSON* slot = nullptr;
    cJSON_ArrayForEach(rule, Get(rules.get(), "medicine_rules")) {
        const char* canonical = Str(Get(rule, "canonical_id"));
        if (!canonical || !HasAny(std::set<std::string>{symptom_enum}, Get(rule, "symptoms"))) continue;
        slot = FindSlot(catalog.get(), canonical, false);
        if (slot) break;
    }
    if (!slot) return Error("DENY", "NO_SYMPTOM_GUIDE");
    if (const char* cue = Str(Get(slot, "symptom_group"))) Put(out.get(), "symptom_cues_vi", cue);
    if (const char* caution = Str(Get(slot, "differentiate_and_escalate")))
        Put(out.get(), "do_not_confuse_with_vi", caution);
    Put(out.get(), "instruction", "Use to clarify symptom classification only; do not read all checks in one turn. Actual next question and any provisional option MUST come from evaluate_symptoms.");
    cJSON_AddBoolToObject(out.get(), "vend_allowed", false);
    const std::string response = Serialize(out.get());
    return response.size() <= 2400 ? response : Error("DENY", "SYMPTOM_GUIDE_TOO_LARGE");
}

std::string MedicalAdvisor::Evaluate(const std::string& untrusted_json) const {
    if (untrusted_json.size() > 4096) return Error("DENY", "INPUT_TOO_LARGE");
    auto rules = Parse(rules_);
    auto catalog = Parse(catalog_);
    auto review = Parse(review_);
    if (!rules || !catalog || !review || !cJSON_IsObject(rules.get()) ||
        !cJSON_IsObject(catalog.get()) || !cJSON_IsObject(review.get()))
        return Error("DENY", "LOCAL_DATA_UNAVAILABLE");
    const auto* scope = Get(rules.get(), "scope");
    const auto* min_age = Get(scope, "minimum_age_years");
    const auto* max_meds = Get(scope, "maximum_medicines_per_transaction");
    if (!cJSON_IsBool(Get(review.get(), "approved")))
        return Error("DENY", "INVALID_PHARMACIST_REVIEW_CONFIGURATION");
    const auto* rule_version = Str(Get(rules.get(), "rules_version"));
    const auto* catalog_version = Str(Get(catalog.get(), "catalog_version"));
    const auto* required = Get(rules.get(), "global_required_fields");
    if (!cJSON_IsNumber(min_age) || !cJSON_IsNumber(max_meds) || !rule_version ||
        !catalog_version || !cJSON_IsArray(required) || !cJSON_IsArray(Get(rules.get(), "medicine_rules")) ||
        !cJSON_IsArray(Get(catalog.get(), "slots")) || !cJSON_IsArray(Get(rules.get(), "global_danger_signs")) ||
        !cJSON_IsObject(Get(rules.get(), "interview")) ||
        !cJSON_IsArray(Get(Get(rules.get(), "interview"), "global_checks")) ||
        !cJSON_IsObject(Get(Get(rules.get(), "interview"), "symptom_checks")))
        return Error("DENY", "INVALID_LOCAL_CONFIGURATION");
    auto input = Parse(untrusted_json);
    if (!input || !cJSON_IsObject(input.get())) return Error("DENY", "INVALID_INPUT_JSON");
    // Never accept cloud-side authorization, channel, SKU, quantity, or a free-text prescription.
    static const std::set<std::string> kFields = {
        "session_id", "turn_id", "age_years", "weight_kg", "pregnancy_or_breastfeeding",
        "symptoms", "duration_hours", "danger_signs", "conditions", "current_medicines", "drug_allergies", "screening_answers"};
    const cJSON* entry = nullptr;
    std::set<std::string> seen_fields;
    cJSON_ArrayForEach(entry, input.get()) {
        if (!entry->string || !kFields.count(entry->string)) return Error("DENY", "FORBIDDEN_OR_UNKNOWN_FIELD");
        if (!seen_fields.insert(entry->string).second) return Error("DENY", "DUPLICATE_FIELD");
    }
    const cJSON* interview = Get(rules.get(), "interview");
    const cJSON* answers = Get(input.get(), "screening_answers");
    if (answers) {
        if (!cJSON_IsObject(answers) || cJSON_GetArraySize(answers) > 48)
            return Error("DENY", "INVALID_SCREENING_ANSWERS");
        std::set<std::string> answer_keys;
        const cJSON* answer = nullptr;
        cJSON_ArrayForEach(answer, answers) {
            if (!answer->string || !answer_keys.insert(answer->string).second ||
                !cJSON_IsBool(answer) || !FindCheck(interview, answer->string))
                return Error("DENY", "INVALID_SCREENING_ANSWERS");
        }
    }
    const char* session = Str(Get(input.get(), "session_id"));
    const auto* turn = Get(input.get(), "turn_id");
    if (!session || !*session || std::strlen(session) > 64 || !cJSON_IsNumber(turn) ||
        !std::isfinite(turn->valuedouble) || turn->valuedouble < 1 || turn->valuedouble > 1000000 ||
        std::floor(turn->valuedouble) != turn->valuedouble)
        return Error("DENY", "INVALID_SESSION_OR_TURN");
    auto out = Make();
    Put(out.get(), "status", "NEED_MORE_INFO");
    Put(out.get(), "session_id", session);
    cJSON_AddNumberToObject(out.get(), "turn_id", turn->valuedouble);
    cJSON_AddBoolToObject(out.get(), "vend_allowed", false);
    cJSON_AddBoolToObject(out.get(), "pharmacist_approved", false);
    Put(out.get(), "rules_version", rule_version);
    Put(out.get(), "catalog_version", catalog_version);
    cJSON* missing = cJSON_AddArrayToObject(out.get(), "missing_fields");
    const cJSON* f = nullptr;
    cJSON_ArrayForEach(f, required) {
        const char* key = Str(f);
        if (key && !Get(input.get(), key)) AddText(missing, key);
    }
    // Known red flags and unsupported population are terminal even when some fields are missing.
    const auto* age = Get(input.get(), "age_years");
    if (age && (!cJSON_IsNumber(age) || !std::isfinite(age->valuedouble) ||
                std::floor(age->valuedouble) != age->valuedouble || age->valuedouble < 0 || age->valuedouble > 120))
        return Error("DENY", "INVALID_AGE");
    if (age && age->valuedouble < min_age->valuedouble) {
        Put(out.get(), "status", "REFER"); Put(out.get(), "reason", "UNDER_MINIMUM_AGE"); return Serialize(out.get());
    }
    const auto* pregnancy = Get(input.get(), "pregnancy_or_breastfeeding");
    if (pregnancy && !cJSON_IsBool(pregnancy)) return Error("DENY", "INVALID_PREGNANCY_FIELD");
    if (cJSON_IsTrue(pregnancy)) {
        Put(out.get(), "status", "REFER"); Put(out.get(), "reason", "PREGNANCY_OR_BREASTFEEDING_UNSUPPORTED");
        return Serialize(out.get());
    }
    bool valid = true;
    std::set<std::string> danger, symptoms, conditions, medicines, allergies;
    if (Get(input.get(), "danger_signs")) danger = ReadEnumArray(Get(input.get(), "danger_signs"), valid);
    if (Get(input.get(), "symptoms")) symptoms = ReadEnumArray(Get(input.get(), "symptoms"), valid);
    if (Get(input.get(), "conditions")) conditions = ReadEnumArray(Get(input.get(), "conditions"), valid);
    if (Get(input.get(), "current_medicines")) medicines = ReadEnumArray(Get(input.get(), "current_medicines"), valid);
    if (Get(input.get(), "drug_allergies")) allergies = ReadEnumArray(Get(input.get(), "drug_allergies"), valid);
    if (!valid) return Error("DENY", "INVALID_ARRAY_OR_DUPLICATE_FLAG");
    if (IsPositiveDanger(interview, answers)) {
        Put(out.get(), "status", "REFER");
        Put(out.get(), "reason", "INTERVIEW_DANGER_OR_CONTRAINDICATION");
        return Serialize(out.get());
    }
    if (!danger.empty()) {
        Put(out.get(), "status", "REFER"); Put(out.get(), "reason", "POSSIBLE_DANGER_SIGN"); return Serialize(out.get());
    }
    // Ask the main symptom first. Then screen urgent global signs one at a time.
    if (!Get(input.get(), "symptoms") || symptoms.empty()) {
        if (Get(input.get(), "symptoms")) Put(out.get(), "reason", "SYMPTOMS_NOT_SPECIFIED");
        NextField(out.get(), "symptoms");
        return Serialize(out.get());
    }
    const auto* global_checks = Get(interview, "global_checks");
    const cJSON* check = nullptr;
    cJSON_ArrayForEach(check, global_checks) {
        const char* id = Str(Get(check, "id"));
        if (!id || !Str(Get(check, "question_vi")) || !cJSON_IsBool(Get(check, "expected")))
            return Error("DENY", "INVALID_INTERVIEW_CONFIG");
        const cJSON* answer = Get(answers, id);
        if (!answer) {
            Put(out.get(), "reason", "UNANSWERED_SAFETY_QUESTION");
            NextQuestion(out.get(), check);
            return Serialize(out.get());
        }
        // Check the entire configured global screening contract, not just
        // positive red flags. Do not silently accept contradictory answers.
        if (cJSON_IsTrue(answer) != cJSON_IsTrue(Get(check, "expected"))) {
            const char* mismatch = Str(Get(check, "on_mismatch"));
            if (!mismatch || (std::strcmp(mismatch, "REFER") != 0 &&
                              std::strcmp(mismatch, "CLARIFY") != 0))
                return Error("DENY", "INVALID_INTERVIEW_CONFIG");
            if (std::strcmp(mismatch, "REFER") == 0) {
                Put(out.get(), "status", "REFER");
                Put(out.get(), "reason", "GLOBAL_SAFETY_CHECK_MISMATCH");
            } else {
                Put(out.get(), "reason", "GLOBAL_CHECK_NEEDS_CLARIFICATION");
                NextQuestion(out.get(), check);
            }
            return Serialize(out.get());
        }
    }
    // A full snapshot from AI can omit known facts; always ask again rather than infer.
    if (cJSON_GetArraySize(missing) > 0) {
        const cJSON* first = cJSON_GetArrayItem(missing, 0);
        if (const char* key = Str(first)) NextField(out.get(), key);
        return Serialize(out.get());
    }
    const auto* weight = Get(input.get(), "weight_kg");
    const auto* duration = Get(input.get(), "duration_hours");
    if (!cJSON_IsNumber(weight) || !std::isfinite(weight->valuedouble) ||
        weight->valuedouble <= 0 || weight->valuedouble > 300 ||
        !cJSON_IsNumber(duration) || !std::isfinite(duration->valuedouble) ||
        duration->valuedouble < 0 || duration->valuedouble > 24 * 365)
        return Error("DENY", "INVALID_WEIGHT_OR_DURATION");
    if (symptoms.empty()) {
        Put(out.get(), "status", "NEED_MORE_INFO"); Put(out.get(), "reason", "SYMPTOMS_NOT_SPECIFIED");
        return Serialize(out.get());
    }
    const auto supported_symptoms = AllowedSymptoms(rules.get());
    if (!Within(symptoms, supported_symptoms)) {
        Put(out.get(), "status", "REFER"); Put(out.get(), "reason", "UNSUPPORTED_SYMPTOM"); return Serialize(out.get());
    }
    const auto flags = AllowedFlags(rules.get());
    if (!Within(conditions, flags) || !Within(medicines, flags) || !allergies.empty()) {
        Put(out.get(), "status", "REFER"); Put(out.get(), "reason", "UNREVIEWED_CONDITION_MEDICINE_OR_ALLERGY");
        return Serialize(out.get());
    }
    std::set<std::string> combined = symptoms;
    combined.insert(conditions.begin(), conditions.end());
    combined.insert(medicines.begin(), medicines.end());
    if (weight->valuedouble < 50) combined.insert("weight_below_50kg");
    if (age->valuedouble < 18) combined.insert("age_16_or_17");
    if (duration->valuedouble > 48) combined.insert("duration_over_48_hours");
    // The interview questions are mandatory for every reported symptom. A classification
    // mismatch requires symptom reclassification; a risk mismatch requires referral.
    const cJSON* profiles = Get(interview, "symptom_checks");
    for (const auto& symptom : symptoms) {
        const cJSON* questions = Get(profiles, symptom.c_str());
        if (!cJSON_IsArray(questions) || cJSON_GetArraySize(questions) == 0)
            return Error("DENY", "MISSING_SYMPTOM_INTERVIEW_CONFIG");
        const cJSON* detail = nullptr;
        cJSON_ArrayForEach(detail, questions) {
            const char* id = Str(Get(detail, "id"));
            const char* question = Str(Get(detail, "question_vi"));
            const cJSON* expected = Get(detail, "expected");
            const char* action = Str(Get(detail, "on_mismatch"));
            if (!id || !question || !cJSON_IsBool(expected) ||
                !action || (std::strcmp(action,"REFER") != 0 && std::strcmp(action,"CLARIFY") != 0))
                return Error("DENY", "INVALID_INTERVIEW_CONFIG");
            const cJSON* answer = Get(answers, id);
            if (!answer) {
                Put(out.get(), "reason", "UNANSWERED_SYMPTOM_QUESTION");
                NextQuestion(out.get(), detail);
                return Serialize(out.get());
            }
            if (cJSON_IsTrue(answer) != cJSON_IsTrue(expected)) {
                if (std::strcmp(action,"REFER") == 0) {
                    Put(out.get(), "status", "REFER");
                    Put(out.get(), "reason", "SYMPTOM_DANGER_OR_CONTRAINDICATION");
                } else {
                    Put(out.get(), "reason", "SYMPTOM_CLASSIFICATION_CONFLICT");
                    NextField(out.get(), "symptoms");
                }
                return Serialize(out.get());
            }
        }
    }
    // No pharmacist sign-off and no independent inventory reconciliation: advisory only.
    cJSON* options = cJSON_AddArrayToObject(out.get(), "provisional_options");
    int count = 0;
    int eligible_count = 0;
    bool referred = false;
    const cJSON* rule = nullptr;
    cJSON_ArrayForEach(rule, Get(rules.get(), "medicine_rules")) {
        if (!HasAny(symptoms, Get(rule, "symptoms"))) continue;
        const char* canonical = Str(Get(rule, "canonical_id"));
        if (!canonical) return Error("DENY", "INVALID_RULE_ID");
        const auto* rule_min = Get(rule, "minimum_age_years");
        if ((cJSON_IsNumber(rule_min) && age->valuedouble < rule_min->valuedouble) ||
            HasAny(combined, Get(rule, "refer_if")) || HasAny(combined, Get(rule, "exclude_if"))) {
            referred = true;
            continue;
        }
        const cJSON* main = FindSlot(catalog.get(), canonical, false);
        if (!main || !cJSON_IsNumber(Get(main, "channel")))
            return Error("DENY", "INVALID_MAIN_CATALOG_SLOT");
        const cJSON* backup = FindSlot(catalog.get(), canonical, true);
        if (backup) {
            const auto* backup_of = Get(backup, "backup_of_channel");
            if (!cJSON_IsNumber(backup_of) ||
                backup_of->valueint != Get(main, "channel")->valueint)
                backup = nullptr;
        }
        const cJSON* selected = main;
        // initial_stock is a catalogue snapshot, NOT measured real stock.
        const auto* initial = Get(main, "initial_stock");
        if (!cJSON_IsNumber(initial) || initial->valueint <= 0) selected = backup;
        if (!selected) continue;
        initial = Get(selected, "initial_stock");
        if (!cJSON_IsNumber(initial) || initial->valueint <= 0) continue;
        const char* name = Str(Get(selected, "name"));
        const char* strength = Str(Get(selected, "strength"));
        const char* ingredient = Str(Get(selected, "active_ingredient"));
        if (!name || !strength || !ingredient) return Error("DENY", "INVALID_CATALOG_ITEM");
        // More than one possible medicine must NOT silently default to the
        // first rule in file order. Collect at most one *provisional* option,
        // then fail closed if a second distinct candidate is encountered.
        ++eligible_count;
        if (count >= 1 || count >= max_meds->valueint) continue;
        auto* option = cJSON_CreateObject();
        Put(option, "canonical_id", canonical);
        Put(option, "name", name);
        Put(option, "ingredient", ingredient);
        Put(option, "strength", strength);
        Put(option, "stock_status", "INITIAL_CATALOG_ONLY_UNVERIFIED");
        Put(option, "clinical_status", "UNREVIEWED_PROVISIONAL_ONLY");
        // SKU and channel are intentionally NOT returned to the cloud or AI.
        cJSON_AddItemToArray(options, option);
        ++count;
    }
    if (eligible_count > 1) {
        cJSON_DeleteItemFromObjectCaseSensitive(out.get(), "provisional_options");
        Put(out.get(), "status", "REFER");
        Put(out.get(), "reason", "MULTIPLE_OPTIONS_REQUIRE_HUMAN_REVIEW");
    } else if (count > 0) {
        Put(out.get(), "status", "PROVISIONAL_OPTIONS");
        Put(out.get(), "reason", "PHARMACIST_REVIEW_AND_STOCK_VERIFICATION_REQUIRED");
    } else {
        Put(out.get(), "status", "REFER");
        Put(out.get(), "reason", referred ? "RULE_EXCLUSION_OR_REFERRAL" : "NO_REVIEWABLE_OPTION_IN_CATALOG");
    }
    Put(out.get(), "safety_notice", "AI-reported facts are unverified. No diagnosis, dose, dispense authorization, or relay operation.");
    return Serialize(out.get());
}
}
