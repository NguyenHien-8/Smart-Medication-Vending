#include "catalog_router.h"

#include <cJSON.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <set>
#include <string>
#include <string_view>

namespace smv {
namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

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

bool BoundedText(const cJSON* value, std::string& result) {
    if (!cJSON_IsString(value) || value->valuestring == nullptr || value->valuestring[0] == '\0' ||
        std::strlen(value->valuestring) > 96) {
        return false;
    }
    result = value->valuestring;
    return true;
}

bool NonEmptyText(const cJSON* value, size_t maximum_length) {
    return cJSON_IsString(value) && value->valuestring != nullptr &&
           value->valuestring[0] != '\0' && std::strlen(value->valuestring) <= maximum_length;
}

bool HasOnlyFields(const cJSON* object, std::initializer_list<std::string_view> allowed) {
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

// Reject malformed/empty safety predicates rather than accepting an ID-only rule.
// This checks the wire schema; it does not replace the pharmacist's content review.
bool SafetyEnums(const cJSON* array, std::set<std::string>& values, bool required) {
    if (array == nullptr)
        return !required;
    if (!cJSON_IsArray(array) || cJSON_GetArraySize(array) < 1 || cJSON_GetArraySize(array) > 64)
        return false;
    const cJSON* entry = nullptr;
    cJSON_ArrayForEach (entry, array) {
        if (!cJSON_IsString(entry) || entry->valuestring == nullptr)
            return false;
        const std::string_view name(entry->valuestring);
        if (name.empty() || name.size() > 64 ||
            !std::all_of(name.begin(), name.end(),
                         [](char c) {
                             return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
                         }) ||
            !values.insert(std::string(name)).second)
            return false;
    }
    return true;
}

bool ValidateRule(const cJSON* rule) {
    if (!cJSON_IsObject(rule) ||
        !HasOnlyFields(rule, {"canonical_id", "symptoms", "refer_if", "exclude_if",
                              "minimum_age_years", "selection_priority"}))
        return false;
    std::set<std::string> symptoms, referral, exclusions;
    if (!SafetyEnums(Get(rule, "symptoms"), symptoms, true) ||
        !SafetyEnums(Get(rule, "refer_if"), referral, false) ||
        !SafetyEnums(Get(rule, "exclude_if"), exclusions, false) ||
        (referral.empty() && exclusions.empty()))
        return false;
    for (const auto& flag : referral)
        if (exclusions.count(flag))
            return false;
    if (const cJSON* age = Get(rule, "minimum_age_years")) {
        int minimum_age = 0;
        if (!ExactInteger(age, 16, 120, minimum_age))
            return false;
    }
    if (const cJSON* priority = Get(rule, "selection_priority")) {
        int selection_priority = 0;
        if (!ExactInteger(priority, 1, 65535, selection_priority))
            return false;
    }
    return true;
}

bool ValidateRulesEnvelope(const cJSON* root) {
    if (!HasOnlyFields(
            root, {"schema_version", "rules_version", "status", "scope", "global_required_fields",
                   "global_danger_signs", "recognized_non_excluding_flags", "medicine_rules",
                   "sources", "interview"}))
        return false;
    const cJSON* scope = Get(root, "scope");
    if (!cJSON_IsObject(scope) ||
        !HasOnlyFields(scope, {"minimum_age_years", "pregnancy_or_breastfeeding_supported",
                               "missing_data_policy", "maximum_medicines_per_transaction",
                               "maximum_blisters_per_medicine", "diagnosis_claims_allowed"}))
        return false;
    int age = 0, max_meds = 0, max_blisters = 0;
    if (!ExactInteger(Get(scope, "minimum_age_years"), 16, 120, age) ||
        !ExactInteger(Get(scope, "maximum_medicines_per_transaction"), 1, 3, max_meds) ||
        !ExactInteger(Get(scope, "maximum_blisters_per_medicine"), 1, 1, max_blisters) ||
        (!cJSON_IsBool(Get(scope, "pregnancy_or_breastfeeding_supported")) ||
         cJSON_IsTrue(Get(scope, "pregnancy_or_breastfeeding_supported"))) ||
        (!cJSON_IsBool(Get(scope, "diagnosis_claims_allowed")) ||
         cJSON_IsTrue(Get(scope, "diagnosis_claims_allowed"))) ||
        !cJSON_IsString(Get(scope, "missing_data_policy")) ||
        std::strcmp(Get(scope, "missing_data_policy")->valuestring,
                    "NEED_MORE_INFO_THEN_NO_VEND") != 0)
        return false;
    std::set<std::string> required, danger, recognized_non_excluding;
    if (!SafetyEnums(Get(root, "global_required_fields"), required, true) ||
        required != std::set<std::string>{"age_years", "weight_kg", "pregnancy_or_breastfeeding",
                                          "symptoms", "duration_hours", "danger_signs",
                                          "conditions", "current_medicines", "drug_allergies"} ||
        !SafetyEnums(Get(root, "global_danger_signs"), danger, true) ||
        !SafetyEnums(Get(root, "recognized_non_excluding_flags"), recognized_non_excluding, true) ||
        recognized_non_excluding != std::set<std::string>{"other_condition",
                                                          "other_current_medicine",
                                                          "other_drug_allergy"})
        return false;
    // The interview engine dereferences these fields; malformed question structures
    // must not be interpreted as an empty/fulfilled safety interview.
    const cJSON* interview = Get(root, "interview");
    const cJSON* global_checks = Get(interview, "global_checks");
    const cJSON* symptom_checks = Get(interview, "symptom_checks");
    int interview_version = 0;
    if (!cJSON_IsObject(interview) ||
        !HasOnlyFields(interview, {"version", "global_checks", "symptom_checks", "protocol"}) ||
        !ExactInteger(Get(interview, "version"), 1, 1, interview_version) ||
        !NonEmptyText(Get(interview, "protocol"), 2048) || !cJSON_IsArray(global_checks) ||
        cJSON_GetArraySize(global_checks) < 1 || !cJSON_IsObject(symptom_checks))
        return false;
    std::set<std::string> check_ids;
    const auto validate_checks = [&check_ids](const cJSON* checks) {
        if (!cJSON_IsArray(checks) || cJSON_GetArraySize(checks) < 1 ||
            cJSON_GetArraySize(checks) > 64)
            return false;
        const cJSON* check = nullptr;
        cJSON_ArrayForEach (check, checks) {
            std::string id;
            const cJSON* expected = Get(check, "expected");
            const cJSON* mismatch = Get(check, "on_mismatch");
            if (!cJSON_IsObject(check) ||
                !HasOnlyFields(check, {"id", "question_vi", "expected", "on_mismatch"}) ||
                !BoundedText(Get(check, "id"), id) || !check_ids.insert(id).second ||
                !NonEmptyText(Get(check, "question_vi"), 1024) || !cJSON_IsBool(expected) ||
                !cJSON_IsString(mismatch) ||
                (std::strcmp(mismatch->valuestring, "REFER") != 0 &&
                 std::strcmp(mismatch->valuestring, "CLARIFY") != 0))
                return false;
        }
        return true;
    };
    if (!validate_checks(global_checks))
        return false;
    const cJSON* profile = nullptr;
    std::set<std::string> profile_symptoms;
    cJSON_ArrayForEach (profile, symptom_checks) {
        if (!profile->string || !profile_symptoms.insert(profile->string).second ||
            !validate_checks(profile))
            return false;
    }
    return !profile_symptoms.empty();
}

bool HasUnsafeControlField(const cJSON* object) {
    return Get(object, "enabled") != nullptr || Get(object, "approved") != nullptr ||
           Get(object, "expires_at") != nullptr || Get(object, "expiry") != nullptr;
}

bool SameIdentity(const auto& primary, const auto& backup) {
    return primary.canonical_id == backup.canonical_id && primary.name == backup.name &&
           primary.active_ingredient == backup.active_ingredient &&
           primary.strength == backup.strength;
}
}  // namespace

CatalogRouter::CatalogRouter(std::string_view catalog_json, std::string_view rules_json) {
    valid_ = Parse(catalog_json, rules_json);
}

bool CatalogRouter::Fail(const char* reason) {
    validation_reason_ = reason;
    slots_.clear();
    return false;
}

bool CatalogRouter::Parse(std::string_view catalog_json, std::string_view rules_json) {
    const std::string catalog_text(catalog_json);
    Json root(
        cJSON_ParseWithLengthOpts(catalog_text.c_str(), catalog_text.size() + 1, nullptr, true),
        &cJSON_Delete);
    if (!root || !cJSON_IsObject(root.get()))
        return Fail("INVALID_JSON");
    if (!HasOnlyFields(root.get(), {"schema_version", "catalog_version", "dispense_unit",
                                    "tablets_per_blister", "initial_stock_unit", "slots"})) {
        return Fail("INVALID_CATALOG_SCHEMA");
    }

    int schema_version = 0;
    std::string catalog_version;
    std::string dispense_unit;
    std::string initial_stock_unit;
    const cJSON* slots = Get(root.get(), "slots");
    if (!ExactInteger(Get(root.get(), "schema_version"), 1, 1, schema_version) ||
        !BoundedText(Get(root.get(), "catalog_version"), catalog_version) ||
        !BoundedText(Get(root.get(), "dispense_unit"), dispense_unit) ||
        !cJSON_IsNull(Get(root.get(), "tablets_per_blister")) ||
        !BoundedText(Get(root.get(), "initial_stock_unit"), initial_stock_unit) ||
        !cJSON_IsArray(slots)) {
        return Fail("INVALID_CATALOG_SCHEMA");
    }
    if (dispense_unit != "sealed_blister" || initial_stock_unit != "blister")
        return Fail("UNSUPPORTED_DISPENSE_UNIT");
    const int slot_count = cJSON_GetArraySize(slots);
    if (slot_count < 1 || slot_count > static_cast<int>(kVendingChannelCount))
        return Fail("INVALID_SLOT_COUNT");

    std::set<int> channels;
    const cJSON* slot_json = nullptr;
    cJSON_ArrayForEach (slot_json, slots) {
        if (!cJSON_IsObject(slot_json) ||
            !HasOnlyFields(slot_json,
                           {"channel", "sku", "canonical_id", "name", "active_ingredient",
                            "strength", "symptom_group", "initial_stock", "backup_of_channel",
                            "differentiate_and_escalate", "information_source"}) ||
            HasUnsafeControlField(slot_json)) {
            return Fail("INVALID_SLOT_SCHEMA");
        }

        Slot slot;
        int channel = 0;
        int initial_stock = 0;
        if (!ExactInteger(Get(slot_json, "channel"), 0, static_cast<int>(kVendingChannelCount) - 1,
                          channel) ||
            !BoundedText(Get(slot_json, "sku"), slot.sku) ||
            !BoundedText(Get(slot_json, "canonical_id"), slot.canonical_id) ||
            !BoundedText(Get(slot_json, "name"), slot.name) ||
            !BoundedText(Get(slot_json, "active_ingredient"), slot.active_ingredient) ||
            !BoundedText(Get(slot_json, "strength"), slot.strength) ||
            !NonEmptyText(Get(slot_json, "symptom_group"), 1024) ||
            !ExactInteger(Get(slot_json, "initial_stock"), 0, 10000, initial_stock) ||
            !NonEmptyText(Get(slot_json, "differentiate_and_escalate"), 1024) ||
            !NonEmptyText(Get(slot_json, "information_source"), 512)) {
            return Fail("INVALID_SLOT_SCHEMA");
        }
        if (!channels.insert(channel).second)
            return Fail("DUPLICATE_CHANNEL");
        slot.channel = static_cast<uint8_t>(channel);

        const cJSON* backup = Get(slot_json, "backup_of_channel");
        if (cJSON_IsNumber(backup)) {
            int primary_channel = 0;
            if (!ExactInteger(backup, 0, static_cast<int>(kVendingChannelCount) - 1,
                              primary_channel)) {
                return Fail("INVALID_BACKUP_REFERENCE");
            }
            slot.backup_of_channel = static_cast<uint8_t>(primary_channel);
        } else if (!cJSON_IsNull(backup)) {
            return Fail("INVALID_BACKUP_REFERENCE");
        }
        slots_.push_back(std::move(slot));
    }

    std::set<std::string> identities;
    for (const auto& slot : slots_)
        identities.insert(slot.canonical_id);
    for (const auto& canonical_id : identities) {
        const Slot* primary = nullptr;
        const Slot* backup = nullptr;
        for (const auto& slot : slots_) {
            if (slot.canonical_id != canonical_id)
                continue;
            if (!slot.backup_of_channel.has_value()) {
                if (primary != nullptr)
                    return Fail("DUPLICATE_PRIMARY");
                primary = &slot;
            } else {
                if (backup != nullptr)
                    return Fail("DUPLICATE_BACKUP");
                backup = &slot;
            }
        }
        if (primary == nullptr)
            return Fail("MISSING_PRIMARY");
        if (backup != nullptr) {
            if (*backup->backup_of_channel != primary->channel)
                return Fail("BACKUP_REFERENCE_MISMATCH");
            if (!SameIdentity(*primary, *backup))
                return Fail("BACKUP_IDENTITY_MISMATCH");
        }
    }

    const std::string rules_text(rules_json);
    Json rules_root(
        cJSON_ParseWithLengthOpts(rules_text.c_str(), rules_text.size() + 1, nullptr, true),
        &cJSON_Delete);
    if (!rules_root || !cJSON_IsObject(rules_root.get()) ||
        HasUnsafeControlField(rules_root.get()) || !ValidateRulesEnvelope(rules_root.get())) {
        return Fail("INVALID_RULES_SCHEMA");
    }
    int rules_schema_version = 0;
    std::string rules_version;
    std::string rules_status;
    const cJSON* medicine_rules = Get(rules_root.get(), "medicine_rules");
    if (!ExactInteger(Get(rules_root.get(), "schema_version"), 1, 1, rules_schema_version) ||
        !BoundedText(Get(rules_root.get(), "rules_version"), rules_version) ||
        !BoundedText(Get(rules_root.get(), "status"), rules_status) ||
        rules_status != "PHARMACIST_REVIEW_REQUIRED" || !cJSON_IsArray(medicine_rules)) {
        return Fail("INVALID_RULES_SCHEMA");
    }

    std::set<std::string> rule_identities;
    const cJSON* rule = nullptr;
    cJSON_ArrayForEach (rule, medicine_rules) {
        std::string canonical_id;
        if (!ValidateRule(rule) || HasUnsafeControlField(rule) ||
            !BoundedText(Get(rule, "canonical_id"), canonical_id) ||
            !rule_identities.insert(canonical_id).second) {
            return Fail("INVALID_RULES_SCHEMA");
        }
    }
    // A rule must not introduce a symptom without its mandatory interview.
    const cJSON* profiles = Get(Get(rules_root.get(), "interview"), "symptom_checks");
    cJSON_ArrayForEach (rule, medicine_rules) {
        const cJSON* symptom = nullptr;
        cJSON_ArrayForEach (symptom, Get(rule, "symptoms")) {
            if (!Get(profiles, symptom->valuestring))
                return Fail("MISSING_SYMPTOM_INTERVIEW");
        }
    }
    if (rule_identities != identities)
        return Fail("RULE_REFERENCE_MISSING");

    validation_reason_ = "OK";
    return true;
}

RouteSelection CatalogRouter::Select(std::string_view canonical_id,
                                     const StockSnapshot& stock) const {
    RouteSelection selection;
    selection.canonical_id = std::string(canonical_id);
    if (!valid_) {
        selection.status = RouteStatus::kInvalidCatalog;
        selection.reason = validation_reason_;
        return selection;
    }

    const Slot* primary = nullptr;
    const Slot* backup = nullptr;
    for (const auto& slot : slots_) {
        if (slot.canonical_id != canonical_id)
            continue;
        if (slot.backup_of_channel.has_value())
            backup = &slot;
        else
            primary = &slot;
    }
    if (primary == nullptr) {
        selection.status = RouteStatus::kUnknownItem;
        selection.reason = "UNKNOWN_ITEM";
        return selection;
    }

    const uint16_t primary_bit = static_cast<uint16_t>(1u << primary->channel);
    const uint16_t backup_bit =
        backup == nullptr ? 0 : static_cast<uint16_t>(1u << backup->channel);
    if (!stock.available || stock.revision == 0 || stock.transaction_pending ||
        (stock.valid_mask & primary_bit) == 0 ||
        (backup != nullptr && (stock.valid_mask & backup_bit) == 0)) {
        selection.status = RouteStatus::kInventoryUnavailable;
        selection.reason = "INVENTORY_UNAVAILABLE";
        return selection;
    }

    const Slot* selected = nullptr;
    bool is_backup = false;
    if (stock.counts[primary->channel] > 0) {
        selected = primary;
    } else if (backup != nullptr && stock.counts[backup->channel] > 0) {
        selected = backup;
        is_backup = true;
    }
    if (selected == nullptr) {
        selection.status = RouteStatus::kOutOfStock;
        selection.reason = "OUT_OF_STOCK";
        return selection;
    }

    selection.status = RouteStatus::kReady;
    selection.reason = "READY";
    selection.canonical_id = selected->canonical_id;
    selection.sku = selected->sku;
    selection.name = selected->name;
    selection.active_ingredient = selected->active_ingredient;
    selection.strength = selected->strength;
    selection.inventory_revision = stock.revision;
    selection.channel = selected->channel;
    selection.backup = is_backup;
    return selection;
}
}  // namespace smv
