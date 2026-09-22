#pragma once

#include "vending_types.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace smv {
class CatalogRouter {
public:
    explicit CatalogRouter(std::string_view catalog_json);

    bool valid() const { return valid_; }
    const std::string& validation_reason() const { return validation_reason_; }
    RouteSelection Select(std::string_view canonical_id, const StockSnapshot& stock) const;

private:
    struct Slot {
        uint8_t channel = kInvalidVendingChannel;
        std::string sku;
        std::string canonical_id;
        std::string name;
        std::string active_ingredient;
        std::string strength;
        std::optional<uint8_t> backup_of_channel;
    };

    bool Parse(std::string_view catalog_json);
    bool Fail(const char* reason);

    bool valid_ = false;
    std::string validation_reason_ = "NOT_PARSED";
    std::vector<Slot> slots_;
};
}  // namespace smv
