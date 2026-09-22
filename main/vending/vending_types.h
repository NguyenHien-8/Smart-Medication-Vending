#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace smv {
inline constexpr size_t kVendingChannelCount = 16;
inline constexpr uint8_t kInvalidVendingChannel = 0xff;

struct StockSnapshot {
    bool available = false;
    uint32_t revision = 0;
    std::array<uint32_t, kVendingChannelCount> counts{};
    uint16_t valid_mask = 0;
    bool transaction_pending = false;
};

enum class RouteStatus {
    kReady,
    kInvalidCatalog,
    kUnknownItem,
    kInventoryUnavailable,
    kOutOfStock,
};

struct RouteSelection {
    RouteStatus status = RouteStatus::kInvalidCatalog;
    std::string reason = "NOT_ROUTED";
    std::string canonical_id;
    std::string sku;
    std::string name;
    std::string active_ingredient;
    std::string strength;
    uint32_t inventory_revision = 0;
    uint8_t channel = kInvalidVendingChannel;
    bool backup = false;
};

struct ReservationToken {
    uint64_t transaction_id = 0;
    uint32_t reserved_revision = 0;
    uint8_t channel = kInvalidVendingChannel;
};
}  // namespace smv
