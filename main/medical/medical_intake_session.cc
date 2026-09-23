#include "medical_intake_session.h"

#include <cJSON.h>

#include <cmath>
#include <cstring>
#include <memory>
#include <set>
#include <string>

namespace smv {
namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

const cJSON* Get(const cJSON* object, const char* key) {
    return cJSON_GetObjectItemCaseSensitive(object, key);
}

bool ExactInteger(const cJSON* value, uint32_t minimum, uint32_t maximum, uint32_t& result) {
    if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) ||
        std::floor(value->valuedouble) != value->valuedouble ||
        value->valuedouble < static_cast<double>(minimum) ||
        value->valuedouble > static_cast<double>(maximum)) {
        return false;
    }
    result = static_cast<uint32_t>(value->valuedouble);
    return true;
}

bool HasUniqueKnownFields(const cJSON* object) {
    static const std::set<std::string_view> allowed = {
        "session_id",
        "turn_id",
        "age_years",
        "weight_kg",
        "pregnancy_or_breastfeeding",
        "primary_symptom",
        "symptoms",
        "duration_hours",
        "danger_signs",
        "conditions",
        "current_medicines",
        "drug_allergies",
        "screening_answers",
    };
    std::set<std::string_view> seen;
    const cJSON* field = nullptr;
    cJSON_ArrayForEach (field, object) {
        if (field->string == nullptr || !allowed.count(field->string) ||
            !seen.insert(field->string).second) {
            return false;
        }
    }
    return true;
}

enum class ArrayReadResult { kApplied, kClarify, kInvalid };

ArrayReadResult ReadFlags(const MedicalPolicyCache& policy, const cJSON* array,
                          std::bitset<kMaxMedicalFlags>& output, bool danger_only) {
    if (!cJSON_IsArray(array) || cJSON_GetArraySize(array) > 32)
        return ArrayReadResult::kInvalid;
    std::bitset<kMaxMedicalFlags> parsed;
    std::set<std::string> seen;
    const cJSON* entry = nullptr;
    cJSON_ArrayForEach (entry, array) {
        if (!cJSON_IsString(entry) || entry->valuestring == nullptr ||
            std::strlen(entry->valuestring) > 64 || !seen.insert(entry->valuestring).second) {
            return ArrayReadResult::kInvalid;
        }
        const std::string_view value(entry->valuestring);
        const auto index = policy.FlagIndex(value);
        if (value == "unknown" || !index || (danger_only && !policy.IsDangerFlag(*index)))
            return ArrayReadResult::kClarify;
        parsed.set(*index);
    }
    output = parsed;
    return ArrayReadResult::kApplied;
}

ArrayReadResult ReadSymptoms(const MedicalPolicyCache& policy, const cJSON* array,
                             std::bitset<kMaxMedicalSymptoms>& output) {
    if (!cJSON_IsArray(array) ||
        cJSON_GetArraySize(array) > static_cast<int>(kMaxMedicalSymptoms)) {
        return ArrayReadResult::kInvalid;
    }
    std::bitset<kMaxMedicalSymptoms> parsed;
    std::set<std::string> seen;
    const cJSON* entry = nullptr;
    cJSON_ArrayForEach (entry, array) {
        if (!cJSON_IsString(entry) || entry->valuestring == nullptr ||
            std::strlen(entry->valuestring) > 64 || !seen.insert(entry->valuestring).second) {
            return ArrayReadResult::kInvalid;
        }
        const std::string_view value(entry->valuestring);
        const auto index = policy.SymptomIndex(value);
        if (value == "unknown" || !index)
            return ArrayReadResult::kClarify;
        parsed.set(*index);
    }
    output = parsed;
    return ArrayReadResult::kApplied;
}

void SetClarification(IntakeApplyResult& result, std::string field) {
    if (result.clarify_field.empty())
        result.clarify_field = std::move(field);
    result.reason = "UNKNOWN_MEDICAL_TERM";
}

size_t OnlySetBit(const std::bitset<kMaxMedicalSymptoms>& bits) {
    for (size_t index = 0; index < kMaxMedicalSymptoms; ++index) {
        if (bits.test(index))
            return index;
    }
    return kMaxMedicalSymptoms;
}
}  // namespace

IntakeApplyResult MedicalIntakeSession::ApplyDelta(const MedicalPolicyCache& policy,
                                                   std::string_view delta_json, uint64_t now_ms) {
    IntakeApplyResult result;
    if (!policy.valid()) {
        result.reason = "INVALID_MEDICAL_POLICY";
        return result;
    }
    if (delta_json.empty() || delta_json.size() > kMaximumDeltaBytes) {
        result.reason = "INPUT_TOO_LARGE";
        return result;
    }
    const std::string text(delta_json);
    Json root(cJSON_ParseWithLengthOpts(text.c_str(), text.size() + 1, nullptr, true),
              &cJSON_Delete);
    if (!root || !cJSON_IsObject(root.get()) || !HasUniqueKnownFields(root.get())) {
        result.reason = "INVALID_OR_FORBIDDEN_INPUT";
        return result;
    }

    const cJSON* session = Get(root.get(), "session_id");
    const cJSON* turn = Get(root.get(), "turn_id");
    uint32_t turn_id = 0;
    if (!cJSON_IsString(session) || session->valuestring == nullptr ||
        session->valuestring[0] == '\0' || std::strlen(session->valuestring) > 64 ||
        !ExactInteger(turn, 1, 1000000, turn_id)) {
        result.reason = "INVALID_SESSION_OR_TURN";
        return result;
    }

    const bool expired =
        active_ && now_ms >= last_activity_ms_ && now_ms - last_activity_ms_ >= kSessionExpiryMs;
    const bool different_session = active_ && session_id_ != session->valuestring;
    const bool replacing = !active_ || expired || different_session;
    if (!replacing && turn_id <= highest_turn_id_) {
        result.reason = "STALE_OR_REPLAYED_TURN";
        return result;
    }

    NormalizedMedicalFacts next = replacing ? NormalizedMedicalFacts{} : facts_;
    const std::optional<size_t> old_primary = replacing ? std::nullopt : facts_.primary_symptom;
    bool symptoms_present = false;
    bool primary_present = false;

    if (const cJSON* age = Get(root.get(), "age_years")) {
        uint32_t parsed = 0;
        if (!ExactInteger(age, 1, 120, parsed)) {
            result.reason = "INVALID_AGE";
            return result;
        }
        next.age_years = static_cast<uint8_t>(parsed);
    }
    if (const cJSON* weight = Get(root.get(), "weight_kg")) {
        if (!cJSON_IsNumber(weight) || !std::isfinite(weight->valuedouble) ||
            weight->valuedouble < 1.0 || weight->valuedouble > 500.0) {
            result.reason = "INVALID_WEIGHT";
            return result;
        }
        next.weight_kg = static_cast<float>(weight->valuedouble);
    }
    if (const cJSON* pregnancy = Get(root.get(), "pregnancy_or_breastfeeding")) {
        if (!cJSON_IsBool(pregnancy)) {
            result.reason = "INVALID_PREGNANCY_FACT";
            return result;
        }
        next.pregnancy_or_breastfeeding = cJSON_IsTrue(pregnancy);
    }
    if (const cJSON* duration = Get(root.get(), "duration_hours")) {
        uint32_t parsed = 0;
        if (!ExactInteger(duration, 0, 87600, parsed)) {
            result.reason = "INVALID_DURATION";
            return result;
        }
        next.duration_hours = parsed;
    }

    if (const cJSON* symptoms = Get(root.get(), "symptoms")) {
        symptoms_present = true;
        auto parsed = next.symptoms;
        const ArrayReadResult read = ReadSymptoms(policy, symptoms, parsed);
        if (read == ArrayReadResult::kInvalid) {
            result.reason = "INVALID_SYMPTOMS";
            return result;
        }
        if (read == ArrayReadResult::kClarify)
            SetClarification(result, "symptoms");
        else
            next.symptoms = parsed;
    }
    if (const cJSON* primary = Get(root.get(), "primary_symptom")) {
        primary_present = true;
        if (!cJSON_IsString(primary) || primary->valuestring == nullptr ||
            std::strlen(primary->valuestring) > 64) {
            result.reason = "INVALID_PRIMARY_SYMPTOM";
            return result;
        }
        const std::string_view value(primary->valuestring);
        const auto index = policy.SymptomIndex(value);
        if (value == "unknown" || !index) {
            SetClarification(result, "primary_symptom");
        } else {
            next.primary_symptom = *index;
            next.symptoms.set(*index);
        }
    }

    const auto apply_flag_array = [&](const char* key, MedicalFactBits& destination, bool& reported,
                                      bool danger_only) {
        const cJSON* value = Get(root.get(), key);
        if (value == nullptr)
            return true;
        MedicalFactBits parsed = destination;
        const ArrayReadResult read = ReadFlags(policy, value, parsed, danger_only);
        if (read == ArrayReadResult::kInvalid) {
            result.reason = std::string("INVALID_") + key;
            return false;
        }
        if (read == ArrayReadResult::kClarify) {
            SetClarification(result, key);
            return true;
        }
        destination = parsed;
        reported = true;
        return true;
    };
    if (!apply_flag_array("danger_signs", next.danger_signs, next.danger_signs_reported, true) ||
        !apply_flag_array("conditions", next.conditions, next.conditions_reported, false) ||
        !apply_flag_array("current_medicines", next.current_medicines,
                          next.current_medicines_reported, false) ||
        !apply_flag_array("drug_allergies", next.drug_allergies, next.drug_allergies_reported,
                          false)) {
        return result;
    }

    if (const cJSON* answers = Get(root.get(), "screening_answers")) {
        if (!cJSON_IsObject(answers) ||
            cJSON_GetArraySize(answers) > static_cast<int>(kMaxMedicalQuestions)) {
            result.reason = "INVALID_SCREENING_ANSWERS";
            return result;
        }
        std::set<std::string_view> seen;
        const cJSON* answer = nullptr;
        cJSON_ArrayForEach (answer, answers) {
            if (answer->string == nullptr || !seen.insert(answer->string).second ||
                !cJSON_IsBool(answer)) {
                result.reason = "INVALID_SCREENING_ANSWERS";
                return result;
            }
            const auto index = policy.QuestionIndex(answer->string);
            if (!index) {
                result.reason = "INVALID_SCREENING_ANSWERS";
                return result;
            }
            next.screening_answers[*index] = cJSON_IsTrue(answer) ? 1 : 0;
        }
    }

    if (!primary_present && symptoms_present && result.clarify_field != "symptoms") {
        if (next.symptoms.none()) {
            next.primary_symptom.reset();
        } else if (next.symptoms.count() == 1) {
            next.primary_symptom = OnlySetBit(next.symptoms);
        } else if (!next.primary_symptom || !next.symptoms.test(*next.primary_symptom)) {
            next.primary_symptom.reset();
            if (result.clarify_field.empty()) {
                result.clarify_field = "primary_symptom";
                result.reason = "PRIMARY_SYMPTOM_REQUIRED";
            }
        }
    }

    if (old_primary && next.primary_symptom != old_primary) {
        for (const size_t question_index : policy.QuestionsForSymptom(*old_primary))
            next.screening_answers[question_index] = -1;
    }

    result.facts_changed = replacing ? !(next == NormalizedMedicalFacts{}) : !(next == facts_);
    if (result.facts_changed)
        ++facts_revision_;
    if (replacing)
        facts_revision_ = result.facts_changed ? 1 : 0;
    facts_ = std::move(next);
    result.accepted = true;
    result.session_replaced = active_ && (expired || different_session);
    session_id_ = session->valuestring;
    highest_turn_id_ = turn_id;
    last_activity_ms_ = now_ms;
    active_ = true;
    if (result.reason.empty())
        result.reason = "OK";
    return result;
}

void MedicalIntakeSession::Reset() {
    active_ = false;
    session_id_.clear();
    highest_turn_id_ = 0;
    facts_revision_ = 0;
    last_activity_ms_ = 0;
    facts_ = NormalizedMedicalFacts{};
}
}  // namespace smv
