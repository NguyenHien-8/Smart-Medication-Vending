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
    const auto rules = Parse(rules_);
    const auto catalog = Parse(catalog_);
    if (!rules || !catalog) return Error("DENY", "LOCAL_DATA_UNAVAILABLE");
    auto out = Make();
    Put(out.get(), "purpose", "INTAKE_ONLY_NO_DIAGNOSIS_NO_VEND");
    Put(out.get(), "instruction", "Ask one or two questions per turn; retain only explicitly answered facts. Submit a complete snapshot each turn to self.medical.evaluate_symptoms. Empty arrays mean explicitly screened none, NOT unanswered. Never invent negatives or name a medicine before device response. For unknown information omit its key; do not insert default false or empty arrays.");
    cJSON* fields = cJSON_Duplicate(Get(rules.get(), "global_required_fields"), 1);
    if (fields) cJSON_AddItemToObject(out.get(), "required_fields", fields);
    cJSON* danger = cJSON_Duplicate(Get(rules.get(), "global_danger_signs"), 1);
    if (danger) cJSON_AddItemToObject(out.get(), "danger_sign_enums", danger);
    auto symptoms = AllowedSymptoms(rules.get());
    auto flags = AllowedFlags(rules.get());
    auto a = cJSON_AddArrayToObject(out.get(), "symptom_enums");
    for (const auto& s : symptoms) AddText(a, s);
    a = cJSON_AddArrayToObject(out.get(), "conditions_and_medicine_flags");
    for (const auto& s : flags) AddText(a, s);
    Put(out.get(), "array_semantics", "[] requires explicit negative answer; unknown must be omitted");
    Put(out.get(), "allergy_semantics", "Any reported drug allergy requires professional review; [] only after explicit no-allergy answer");
    Put(out.get(), "stock_semantics", "Catalog initial_stock is NOT verified current stock; no dispensing enabled");
    cJSON_AddBoolToObject(out.get(), "vend_allowed", false);
    return Serialize(out.get());
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
        !cJSON_IsArray(Get(catalog.get(), "slots")) || !cJSON_IsArray(Get(rules.get(), "global_danger_signs")))
        return Error("DENY", "INVALID_LOCAL_CONFIGURATION");
    auto input = Parse(untrusted_json);
    if (!input || !cJSON_IsObject(input.get())) return Error("DENY", "INVALID_INPUT_JSON");
    // Never accept cloud-side authorization, channel, SKU, quantity, or a free-text prescription.
    static const std::set<std::string> kFields = {
        "session_id", "turn_id", "age_years", "weight_kg", "pregnancy_or_breastfeeding",
        "symptoms", "duration_hours", "danger_signs", "conditions", "current_medicines", "drug_allergies"};
    const cJSON* entry = nullptr;
    std::set<std::string> seen_fields;
    cJSON_ArrayForEach(entry, input.get()) {
        if (!entry->string || !kFields.count(entry->string)) return Error("DENY", "FORBIDDEN_OR_UNKNOWN_FIELD");
        if (!seen_fields.insert(entry->string).second) return Error("DENY", "DUPLICATE_FIELD");
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
    if (!danger.empty()) {
        Put(out.get(), "status", "REFER"); Put(out.get(), "reason", "POSSIBLE_DANGER_SIGN"); return Serialize(out.get());
    }
    if (cJSON_GetArraySize(missing) > 0) return Serialize(out.get());
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
    // No pharmacist sign-off and no independent inventory reconciliation: advisory only.
    cJSON* options = cJSON_AddArrayToObject(out.get(), "provisional_options");
    int count = 0;
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
        initial = Get(selected, "initial_stock");
        if (!selected || !cJSON_IsNumber(initial) || initial->valueint <= 0) continue;
        const char* name = Str(Get(selected, "name"));
        const char* strength = Str(Get(selected, "strength"));
        const char* ingredient = Str(Get(selected, "active_ingredient"));
        if (!name || !strength || !ingredient) return Error("DENY", "INVALID_CATALOG_ITEM");
        if (count >= 1 || count >= max_meds->valueint) continue; // one-at-a-time: no interaction matrix certified
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
    if (count > 0) {
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
