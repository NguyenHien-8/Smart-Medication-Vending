#include "medical_policy_cache.h"

#include <cJSON.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <set>
#include <utility>

namespace smv {
namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
constexpr size_t kMaxArtifactBytes = 64 * 1024;
constexpr size_t kMaxIdentifierBytes = 64;
constexpr size_t kMaxDisplayBytes = 96;

const cJSON* Get(const cJSON* object, const char* key) {
    return cJSON_GetObjectItemCaseSensitive(object, key);
}

bool ExactInteger(const cJSON* value, int minimum, int maximum, int& result) {
    if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) ||
        value->valuedouble != static_cast<double>(value->valueint) || value->valueint < minimum ||
        value->valueint > maximum) {
        return false;
    }
    result = value->valueint;
    return true;
}

bool Text(const cJSON* value, std::string& result, size_t maximum_length) {
    if (!cJSON_IsString(value) || value->valuestring == nullptr || value->valuestring[0] == '\0' ||
        std::strlen(value->valuestring) > maximum_length) {
        return false;
    }
    result = value->valuestring;
    return true;
}

bool Identifier(const cJSON* value, std::string& result) {
    if (!Text(value, result, kMaxIdentifierBytes))
        return false;
    return std::all_of(result.begin(), result.end(), [](unsigned char character) {
        return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
               (character >= '0' && character <= '9') || character == '_';
    });
}

bool IdentifierText(std::string_view value) {
    return !value.empty() && value.size() <= kMaxIdentifierBytes &&
           std::all_of(value.begin(), value.end(), [](unsigned char character) {
               return (character >= 'A' && character <= 'Z') ||
                      (character >= 'a' && character <= 'z') ||
                      (character >= '0' && character <= '9') || character == '_';
           });
}

bool HasOnlyFields(const cJSON* object, std::initializer_list<std::string_view> allowed) {
    if (!cJSON_IsObject(object))
        return false;
    std::set<std::string_view> seen;
    const cJSON* field = nullptr;
    cJSON_ArrayForEach (field, object) {
        if (field->string == nullptr)
            return false;
        const std::string_view name(field->string);
        if (std::find(allowed.begin(), allowed.end(), name) == allowed.end() ||
            !seen.insert(name).second) {
            return false;
        }
    }
    return true;
}

bool HasUnsafeControlField(const cJSON* object) {
    return Get(object, "enabled") != nullptr || Get(object, "approved") != nullptr ||
           Get(object, "expires_at") != nullptr || Get(object, "expiry") != nullptr;
}

bool IsExactString(const cJSON* value, const char* expected) {
    return cJSON_IsString(value) && value->valuestring != nullptr &&
           std::strcmp(value->valuestring, expected) == 0;
}

Json ParseArtifact(std::string_view input, size_t& parse_count) {
    ++parse_count;
    if (input.empty() || input.size() > kMaxArtifactBytes)
        return Json(nullptr, &cJSON_Delete);
    const std::string terminated(input);
    return Json(cJSON_ParseWithLengthOpts(terminated.c_str(), terminated.size() + 1, nullptr, true),
                &cJSON_Delete);
}

struct ParsedSlot {
    std::string sku;
    std::string canonical_id;
    std::string name;
    std::string active_ingredient;
    std::string strength;
    uint8_t channel = kInvalidVendingChannel;
    std::optional<uint8_t> backup_of_channel;
};

bool SameIdentity(const ParsedSlot& primary, const ParsedSlot& backup) {
    return primary.canonical_id == backup.canonical_id && primary.name == backup.name &&
           primary.active_ingredient == backup.active_ingredient &&
           primary.strength == backup.strength;
}
}  // namespace

MedicalPolicyCache::MedicalPolicyCache(std::string_view rules_json, std::string_view catalog_json) {
    Json rules_root = ParseArtifact(rules_json, artifact_parse_count_);
    Json catalog_root = ParseArtifact(catalog_json, artifact_parse_count_);
    if (!rules_root || !catalog_root || !cJSON_IsObject(rules_root.get()) ||
        !cJSON_IsObject(catalog_root.get())) {
        Fail("INVALID_JSON");
        return;
    }

    if (HasUnsafeControlField(rules_root.get()) ||
        !HasOnlyFields(rules_root.get(), {"schema_version", "rules_version", "status", "scope",
                                          "global_required_fields", "global_danger_signs",
                                          "recognized_non_excluding_flags", "medicine_rules",
                                          "sources", "interview"})) {
        Fail("INVALID_RULES_SCHEMA");
        return;
    }
    int schema_version = 0;
    std::string rules_status;
    if (!ExactInteger(Get(rules_root.get(), "schema_version"), 1, 1, schema_version) ||
        !Text(Get(rules_root.get(), "rules_version"), rules_version_, kMaxDisplayBytes) ||
        !Text(Get(rules_root.get(), "status"), rules_status, kMaxDisplayBytes) ||
        rules_status != "PHARMACIST_REVIEW_REQUIRED") {
        Fail("INVALID_RULES_SCHEMA");
        return;
    }

    const cJSON* scope = Get(rules_root.get(), "scope");
    int minimum_age = 0;
    int maximum_medicines = 0;
    int maximum_blisters = 0;
    if (!HasOnlyFields(scope, {"minimum_age_years", "pregnancy_or_breastfeeding_supported",
                               "missing_data_policy", "maximum_medicines_per_transaction",
                               "maximum_blisters_per_medicine", "diagnosis_claims_allowed"}) ||
        !ExactInteger(Get(scope, "minimum_age_years"), 16, 120, minimum_age) ||
        !ExactInteger(Get(scope, "maximum_medicines_per_transaction"), 1, 3, maximum_medicines) ||
        !ExactInteger(Get(scope, "maximum_blisters_per_medicine"), 1, 1, maximum_blisters) ||
        !cJSON_IsFalse(Get(scope, "pregnancy_or_breastfeeding_supported")) ||
        !cJSON_IsFalse(Get(scope, "diagnosis_claims_allowed")) ||
        !IsExactString(Get(scope, "missing_data_policy"), "NEED_MORE_INFO_THEN_NO_VEND")) {
        Fail("INVALID_SCOPE");
        return;
    }
    minimum_age_years_ = static_cast<uint8_t>(minimum_age);

    const auto read_identifier_array = [this](const cJSON* value, size_t maximum,
                                              std::vector<std::string>* output) {
        if (!cJSON_IsArray(value) || cJSON_GetArraySize(value) < 1 ||
            cJSON_GetArraySize(value) > static_cast<int>(maximum)) {
            return false;
        }
        std::set<std::string> unique;
        const cJSON* entry = nullptr;
        cJSON_ArrayForEach (entry, value) {
            std::string identifier;
            if (!Identifier(entry, identifier) || !unique.insert(identifier).second)
                return false;
            if (output != nullptr)
                output->push_back(std::move(identifier));
        }
        return true;
    };

    std::vector<std::string> required_fields;
    if (!read_identifier_array(Get(rules_root.get(), "global_required_fields"), 16,
                               &required_fields) ||
        std::set<std::string>(required_fields.begin(), required_fields.end()) !=
            std::set<std::string>{"age_years", "weight_kg", "pregnancy_or_breastfeeding",
                                  "symptoms", "duration_hours", "danger_signs", "conditions",
                                  "current_medicines", "drug_allergies"}) {
        Fail("INVALID_REQUIRED_FIELDS");
        return;
    }

    const auto ensure_flag = [this](const std::string& name) -> std::optional<size_t> {
        const auto found = flag_indices_.find(name);
        if (found != flag_indices_.end())
            return found->second;
        if (flag_names_.size() >= kMaxMedicalFlags)
            return std::nullopt;
        const size_t index = flag_names_.size();
        flag_names_.push_back(name);
        flag_indices_.emplace(name, index);
        return index;
    };

    std::vector<std::string> danger_names;
    if (!read_identifier_array(Get(rules_root.get(), "global_danger_signs"), kMaxMedicalFlags,
                               &danger_names)) {
        Fail("INVALID_DANGER_FLAGS");
        return;
    }
    for (const auto& name : danger_names) {
        const auto index = ensure_flag(name);
        if (!index) {
            Fail("TOO_MANY_FLAGS");
            return;
        }
        danger_flags_.set(*index);
    }

    std::vector<std::string> sentinel_names;
    if (!read_identifier_array(Get(rules_root.get(), "recognized_non_excluding_flags"), 16,
                               &sentinel_names) ||
        std::set<std::string>(sentinel_names.begin(), sentinel_names.end()) !=
            std::set<std::string>{"other_condition", "other_current_medicine",
                                  "other_drug_allergy"}) {
        Fail("INVALID_NON_EXCLUDING_FLAGS");
        return;
    }
    for (const auto& name : sentinel_names) {
        const auto index = ensure_flag(name);
        if (!index) {
            Fail("TOO_MANY_FLAGS");
            return;
        }
        recognized_non_excluding_flags_.set(*index);
    }

    const cJSON* sources = Get(rules_root.get(), "sources");
    if (!cJSON_IsArray(sources) || cJSON_GetArraySize(sources) < 1 ||
        cJSON_GetArraySize(sources) > static_cast<int>(kMaxMedicalRules)) {
        Fail("INVALID_SOURCES");
        return;
    }
    const cJSON* source = nullptr;
    cJSON_ArrayForEach (source, sources) {
        std::string ignored;
        if (!Text(source, ignored, 512)) {
            Fail("INVALID_SOURCES");
            return;
        }
    }

    const cJSON* interview = Get(rules_root.get(), "interview");
    int interview_version = 0;
    std::string protocol;
    if (!HasOnlyFields(interview, {"version", "global_checks", "symptom_checks", "protocol"}) ||
        !ExactInteger(Get(interview, "version"), 1, 1, interview_version) ||
        !Text(Get(interview, "protocol"), protocol, 2048)) {
        Fail("INVALID_INTERVIEW");
        return;
    }

    const auto add_questions = [this](const cJSON* checks, std::vector<size_t>& target) {
        if (!cJSON_IsArray(checks) || cJSON_GetArraySize(checks) < 1 ||
            cJSON_GetArraySize(checks) > static_cast<int>(kMaxMedicalQuestions)) {
            return false;
        }
        uint16_t fallback_priority = 1;
        const cJSON* check = nullptr;
        cJSON_ArrayForEach (check, checks) {
            if (questions_.size() >= kMaxMedicalQuestions ||
                !HasOnlyFields(check,
                               {"id", "question_vi", "expected", "on_mismatch", "priority"})) {
                return false;
            }
            PolicyQuestion question;
            std::string action;
            if (!Identifier(Get(check, "id"), question.id) ||
                question_indices_.find(question.id) != question_indices_.end() ||
                !Text(Get(check, "question_vi"), question.question_vi, 256) ||
                !cJSON_IsBool(Get(check, "expected")) ||
                !Text(Get(check, "on_mismatch"), action, 16)) {
                return false;
            }
            question.expected = cJSON_IsTrue(Get(check, "expected"));
            if (action == "REFER")
                question.on_mismatch = QuestionAction::kRefer;
            else if (action == "CLARIFY")
                question.on_mismatch = QuestionAction::kClarify;
            else
                return false;
            if (const cJSON* priority = Get(check, "priority")) {
                int parsed_priority = 0;
                if (!ExactInteger(priority, 1, 65535, parsed_priority))
                    return false;
                question.priority = static_cast<uint16_t>(parsed_priority);
            } else {
                question.priority = fallback_priority;
            }
            ++fallback_priority;
            const size_t index = questions_.size();
            question_indices_.emplace(question.id, index);
            questions_.push_back(std::move(question));
            target.push_back(index);
        }
        return true;
    };

    if (!add_questions(Get(interview, "global_checks"), global_question_indices_)) {
        Fail("INVALID_GLOBAL_QUESTIONS");
        return;
    }
    const cJSON* symptom_checks = Get(interview, "symptom_checks");
    if (!cJSON_IsObject(symptom_checks)) {
        Fail("INVALID_SYMPTOM_QUESTIONS");
        return;
    }
    const cJSON* profile = nullptr;
    cJSON_ArrayForEach (profile, symptom_checks) {
        if (profile->string == nullptr) {
            Fail("INVALID_SYMPTOM_PROFILE");
            return;
        }
        std::string symptom(profile->string);
        if (!IdentifierText(symptom) || symptom_indices_.find(symptom) != symptom_indices_.end() ||
            symptom_names_.size() >= kMaxMedicalSymptoms) {
            Fail("INVALID_SYMPTOM_PROFILE");
            return;
        }
        const size_t symptom_index = symptom_names_.size();
        symptom_names_.push_back(symptom);
        symptom_indices_.emplace(symptom, symptom_index);
        rules_by_symptom_.emplace_back();
        questions_by_symptom_.emplace_back();
        if (!add_questions(profile, questions_by_symptom_.back())) {
            Fail("INVALID_SYMPTOM_QUESTIONS");
            return;
        }
    }
    if (symptom_names_.empty()) {
        Fail("INVALID_SYMPTOM_PROFILE");
        return;
    }

    const cJSON* medicine_rules = Get(rules_root.get(), "medicine_rules");
    if (!cJSON_IsArray(medicine_rules) || cJSON_GetArraySize(medicine_rules) < 1 ||
        cJSON_GetArraySize(medicine_rules) > static_cast<int>(kMaxMedicalRules)) {
        Fail("INVALID_MEDICINE_RULES");
        return;
    }
    std::set<std::string> canonical_ids;
    const cJSON* rule_json = nullptr;
    cJSON_ArrayForEach (rule_json, medicine_rules) {
        if (HasUnsafeControlField(rule_json) ||
            !HasOnlyFields(rule_json, {"canonical_id", "symptoms", "refer_if", "exclude_if",
                                       "minimum_age_years", "selection_priority"})) {
            Fail("INVALID_MEDICINE_RULE");
            return;
        }
        MedicinePolicy rule;
        if (!Identifier(Get(rule_json, "canonical_id"), rule.canonical_id) ||
            !canonical_ids.insert(rule.canonical_id).second) {
            Fail("INVALID_CANONICAL_ID");
            return;
        }

        std::vector<std::string> symptom_names;
        if (!read_identifier_array(Get(rule_json, "symptoms"), kMaxMedicalSymptoms,
                                   &symptom_names)) {
            Fail("INVALID_RULE_SYMPTOMS");
            return;
        }
        for (const auto& symptom : symptom_names) {
            const auto index = symptom_indices_.find(symptom);
            if (index == symptom_indices_.end()) {
                Fail("MISSING_SYMPTOM_INTERVIEW");
                return;
            }
            rule.symptom_indices.push_back(index->second);
        }

        const auto apply_flags = [&](const cJSON* values, std::bitset<kMaxMedicalFlags>& bits) {
            if (values == nullptr)
                return true;
            std::vector<std::string> names;
            if (!read_identifier_array(values, kMaxMedicalFlags, &names))
                return false;
            for (const auto& name : names) {
                const auto index = ensure_flag(name);
                if (!index)
                    return false;
                bits.set(*index);
            }
            return true;
        };
        if (!apply_flags(Get(rule_json, "refer_if"), rule.refer_flags) ||
            !apply_flags(Get(rule_json, "exclude_if"), rule.exclude_flags) ||
            (rule.refer_flags.none() && rule.exclude_flags.none()) ||
            (rule.refer_flags & rule.exclude_flags).any()) {
            Fail("INVALID_RULE_PREDICATES");
            return;
        }
        if (const cJSON* age = Get(rule_json, "minimum_age_years")) {
            int parsed_age = 0;
            if (!ExactInteger(age, minimum_age, 120, parsed_age)) {
                Fail("INVALID_RULE_AGE");
                return;
            }
            rule.minimum_age_years = static_cast<uint8_t>(parsed_age);
        }
        if (const cJSON* priority = Get(rule_json, "selection_priority")) {
            int parsed_priority = 0;
            if (!ExactInteger(priority, 1, 65535, parsed_priority)) {
                Fail("INVALID_SELECTION_PRIORITY");
                return;
            }
            rule.selection_priority = static_cast<uint16_t>(parsed_priority);
        }

        const size_t rule_index = rules_.size();
        rules_.push_back(std::move(rule));
        for (const size_t symptom_index : rules_.back().symptom_indices)
            rules_by_symptom_[symptom_index].push_back(rule_index);
    }
    for (const auto& rules : rules_by_symptom_) {
        if (rules.empty()) {
            Fail("UNUSED_SYMPTOM_PROFILE");
            return;
        }
    }

    if (!HasOnlyFields(catalog_root.get(),
                       {"schema_version", "catalog_version", "dispense_unit", "tablets_per_blister",
                        "initial_stock_unit", "slots"})) {
        Fail("INVALID_CATALOG_SCHEMA");
        return;
    }
    std::string dispense_unit;
    std::string stock_unit;
    if (!ExactInteger(Get(catalog_root.get(), "schema_version"), 1, 1, schema_version) ||
        !Text(Get(catalog_root.get(), "catalog_version"), catalog_version_, kMaxDisplayBytes) ||
        !Text(Get(catalog_root.get(), "dispense_unit"), dispense_unit, kMaxDisplayBytes) ||
        !cJSON_IsNull(Get(catalog_root.get(), "tablets_per_blister")) ||
        !Text(Get(catalog_root.get(), "initial_stock_unit"), stock_unit, kMaxDisplayBytes) ||
        dispense_unit != "sealed_blister" || stock_unit != "blister") {
        Fail("UNSUPPORTED_DISPENSE_UNIT");
        return;
    }

    const cJSON* slots_json = Get(catalog_root.get(), "slots");
    if (!cJSON_IsArray(slots_json) || cJSON_GetArraySize(slots_json) < 1 ||
        cJSON_GetArraySize(slots_json) > static_cast<int>(kVendingChannelCount)) {
        Fail("INVALID_SLOT_COUNT");
        return;
    }
    std::vector<ParsedSlot> slots;
    std::set<int> channels;
    std::set<std::string> skus;
    const cJSON* slot_json = nullptr;
    cJSON_ArrayForEach (slot_json, slots_json) {
        if (HasUnsafeControlField(slot_json) ||
            !HasOnlyFields(slot_json,
                           {"channel", "sku", "canonical_id", "name", "active_ingredient",
                            "strength", "symptom_group", "initial_stock", "backup_of_channel",
                            "differentiate_and_escalate", "information_source"})) {
            Fail("INVALID_SLOT_SCHEMA");
            return;
        }
        ParsedSlot slot;
        int channel = 0;
        int initial_stock = 0;
        std::string symptom_group;
        std::string escalation;
        std::string information_source;
        if (!ExactInteger(Get(slot_json, "channel"), 0, static_cast<int>(kVendingChannelCount) - 1,
                          channel) ||
            !Text(Get(slot_json, "sku"), slot.sku, kMaxDisplayBytes) ||
            !Identifier(Get(slot_json, "canonical_id"), slot.canonical_id) ||
            !Text(Get(slot_json, "name"), slot.name, kMaxDisplayBytes) ||
            !Text(Get(slot_json, "active_ingredient"), slot.active_ingredient, kMaxDisplayBytes) ||
            !Text(Get(slot_json, "strength"), slot.strength, kMaxDisplayBytes) ||
            !Text(Get(slot_json, "symptom_group"), symptom_group, 1024) ||
            !ExactInteger(Get(slot_json, "initial_stock"), 0, 10000, initial_stock) ||
            !Text(Get(slot_json, "differentiate_and_escalate"), escalation, 1024) ||
            !Text(Get(slot_json, "information_source"), information_source, 512) ||
            !channels.insert(channel).second || !skus.insert(slot.sku).second) {
            Fail("INVALID_SLOT_SCHEMA");
            return;
        }
        slot.channel = static_cast<uint8_t>(channel);
        const cJSON* backup = Get(slot_json, "backup_of_channel");
        if (cJSON_IsNumber(backup)) {
            int primary_channel = 0;
            if (!ExactInteger(backup, 0, static_cast<int>(kVendingChannelCount) - 1,
                              primary_channel)) {
                Fail("INVALID_BACKUP_REFERENCE");
                return;
            }
            slot.backup_of_channel = static_cast<uint8_t>(primary_channel);
        } else if (!cJSON_IsNull(backup)) {
            Fail("INVALID_BACKUP_REFERENCE");
            return;
        }
        slots.push_back(std::move(slot));
    }

    std::set<std::string> catalog_canonical_ids;
    for (const auto& slot : slots)
        catalog_canonical_ids.insert(slot.canonical_id);
    for (const auto& canonical_id : catalog_canonical_ids) {
        const ParsedSlot* primary = nullptr;
        const ParsedSlot* backup = nullptr;
        for (const auto& slot : slots) {
            if (slot.canonical_id != canonical_id)
                continue;
            if (slot.backup_of_channel) {
                if (backup != nullptr) {
                    Fail("DUPLICATE_BACKUP");
                    return;
                }
                backup = &slot;
            } else {
                if (primary != nullptr) {
                    Fail("DUPLICATE_PRIMARY");
                    return;
                }
                primary = &slot;
            }
        }
        if (primary == nullptr) {
            Fail("MISSING_PRIMARY");
            return;
        }
        if (backup != nullptr &&
            (*backup->backup_of_channel != primary->channel || !SameIdentity(*primary, *backup))) {
            Fail(*backup->backup_of_channel != primary->channel ? "BACKUP_REFERENCE_MISMATCH"
                                                                : "BACKUP_IDENTITY_MISMATCH");
            return;
        }
        PolicyCatalogItem item;
        item.canonical_id = primary->canonical_id;
        item.name = primary->name;
        item.active_ingredient = primary->active_ingredient;
        item.strength = primary->strength;
        item.primary_sku = primary->sku;
        item.primary_channel = primary->channel;
        if (backup != nullptr) {
            item.backup_sku = backup->sku;
            item.backup_channel = backup->channel;
        }
        catalog_indices_.emplace(item.canonical_id, catalog_items_.size());
        catalog_items_.push_back(std::move(item));
    }
    if (catalog_canonical_ids != canonical_ids) {
        Fail("RULE_CATALOG_MISMATCH");
        return;
    }

    valid_ = true;
    validation_reason_ = "OK";
}

bool MedicalPolicyCache::Fail(std::string reason) {
    valid_ = false;
    validation_reason_ = std::move(reason);
    return false;
}

const std::vector<size_t>& MedicalPolicyCache::RulesForSymptom(std::string_view symptom) const {
    const auto index = SymptomIndex(symptom);
    return index ? RulesForSymptom(*index) : RulesForSymptom(kMaxMedicalSymptoms);
}

const std::vector<size_t>& MedicalPolicyCache::RulesForSymptom(size_t symptom_index) const {
    static const std::vector<size_t> empty;
    return symptom_index < rules_by_symptom_.size() ? rules_by_symptom_[symptom_index] : empty;
}

const std::vector<size_t>& MedicalPolicyCache::QuestionsForSymptom(std::string_view symptom) const {
    const auto index = SymptomIndex(symptom);
    return index ? QuestionsForSymptom(*index) : QuestionsForSymptom(kMaxMedicalSymptoms);
}

const std::vector<size_t>& MedicalPolicyCache::QuestionsForSymptom(size_t symptom_index) const {
    static const std::vector<size_t> empty;
    return symptom_index < questions_by_symptom_.size() ? questions_by_symptom_[symptom_index]
                                                        : empty;
}

const PolicyQuestion* MedicalPolicyCache::FindQuestion(std::string_view id) const {
    const auto index = QuestionIndex(id);
    return index ? &questions_[*index] : nullptr;
}

std::optional<size_t> MedicalPolicyCache::QuestionIndex(std::string_view id) const {
    const auto found = question_indices_.find(id);
    return found == question_indices_.end() ? std::nullopt : std::optional<size_t>(found->second);
}

const PolicyCatalogItem* MedicalPolicyCache::FindCatalogItem(std::string_view canonical_id) const {
    const auto found = catalog_indices_.find(canonical_id);
    return found == catalog_indices_.end() ? nullptr : &catalog_items_[found->second];
}

std::optional<size_t> MedicalPolicyCache::FlagIndex(std::string_view flag) const {
    const auto found = flag_indices_.find(flag);
    return found == flag_indices_.end() ? std::nullopt : std::optional<size_t>(found->second);
}

std::optional<size_t> MedicalPolicyCache::SymptomIndex(std::string_view symptom) const {
    const auto found = symptom_indices_.find(symptom);
    return found == symptom_indices_.end() ? std::nullopt : std::optional<size_t>(found->second);
}

bool MedicalPolicyCache::IsRecognizedNonExcludingFlag(size_t flag_index) const {
    return flag_index < kMaxMedicalFlags && recognized_non_excluding_flags_.test(flag_index);
}

bool MedicalPolicyCache::IsDangerFlag(size_t flag_index) const {
    return flag_index < kMaxMedicalFlags && danger_flags_.test(flag_index);
}
}  // namespace smv
