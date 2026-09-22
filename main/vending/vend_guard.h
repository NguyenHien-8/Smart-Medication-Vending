#pragma once

#include <cstdint>

namespace smv {
struct VendGuardInput {
    bool production_enabled = false;
    bool review_valid = false;
    bool candidate_valid = false;
    bool physical_confirmation = false;
    bool application_state_idle = false;
    bool relay_idle = false;
    bool inventory_available = false;
    bool inventory_pending = true;
    uint32_t quantity = 0;
    uint64_t transaction_id = 0;
    uint64_t confirmed_transaction_id = 0;
    uint8_t candidate_channel = 0xff;
    uint8_t current_channel = 0xff;
    uint32_t candidate_inventory_revision = 0;
    uint32_t current_inventory_revision = 0;
    uint32_t selected_count = 0;
};

struct VendGuardResult {
    bool authorized = false;
    const char* reason = "NOT_CHECKED";
};

class VendGuard {
public:
    static VendGuardResult Check(const VendGuardInput& input);
};
}  // namespace smv
