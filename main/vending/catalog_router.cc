#include "catalog_router.h"

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

bool SameIdentity(const auto& primary, const auto& backup) {
    return primary.canonical_id == backup.canonical_id && primary.name == backup.name &&
           primary.active_ingredient == backup.active_ingredient &&
           primary.strength == backup.strength;
}
}  // namespace

CatalogRouter::CatalogRouter(std::string_view catalog_json) { valid_ = Parse(catalog_json); }

bool CatalogRouter::Fail(const char* reason) {
    validation_reason_ = reason;
    slots_.clear();
    return false;
}

bool CatalogRouter::Parse(std::string_view catalog_json) {
    const std::string catalog_text(catalog_json);
    Json root(
        cJSON_ParseWithLengthOpts(catalog_text.c_str(), catalog_text.size() + 1, nullptr, true),
        &cJSON_Delete);
    if (!root || !cJSON_IsObject(root.get()))
        return Fail("INVALID_JSON");

    int schema_version = 0;
    std::string catalog_version;
    std::string dispense_unit;
    const cJSON* slots = Get(root.get(), "slots");
    if (!ExactInteger(Get(root.get(), "schema_version"), 1, 1, schema_version) ||
        !BoundedText(Get(root.get(), "catalog_version"), catalog_version) ||
        !BoundedText(Get(root.get(), "dispense_unit"), dispense_unit) || !cJSON_IsArray(slots)) {
        return Fail("INVALID_CATALOG_SCHEMA");
    }
    const int slot_count = cJSON_GetArraySize(slots);
    if (slot_count < 1 || slot_count > static_cast<int>(kVendingChannelCount))
        return Fail("INVALID_SLOT_COUNT");

    std::set<int> channels;
    const cJSON* slot_json = nullptr;
    cJSON_ArrayForEach (slot_json, slots) {
        if (!cJSON_IsObject(slot_json))
            return Fail("INVALID_SLOT");

        Slot slot;
        int channel = 0;
        if (!ExactInteger(Get(slot_json, "channel"), 0, static_cast<int>(kVendingChannelCount) - 1,
                          channel) ||
            !BoundedText(Get(slot_json, "sku"), slot.sku) ||
            !BoundedText(Get(slot_json, "canonical_id"), slot.canonical_id) ||
            !BoundedText(Get(slot_json, "name"), slot.name) ||
            !BoundedText(Get(slot_json, "active_ingredient"), slot.active_ingredient) ||
            !BoundedText(Get(slot_json, "strength"), slot.strength)) {
            return Fail("INVALID_SLOT");
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
