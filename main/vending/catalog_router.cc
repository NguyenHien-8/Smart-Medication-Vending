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
        HasUnsafeControlField(rules_root.get())) {
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
        if (!cJSON_IsObject(rule) || HasUnsafeControlField(rule) ||
            !BoundedText(Get(rule, "canonical_id"), canonical_id) ||
            !rule_identities.insert(canonical_id).second) {
            return Fail("INVALID_RULES_SCHEMA");
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
