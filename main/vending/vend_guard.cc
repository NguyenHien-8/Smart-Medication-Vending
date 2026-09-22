#include "vend_guard.h"

namespace smv {
VendGuardResult VendGuard::Check(const VendGuardInput& input) {
    if (!input.production_enabled)
        return {false, "PRODUCTION_DISABLED"};
    if (!input.review_valid)
        return {false, "PHARMACIST_REVIEW_INVALID"};
    if (!input.candidate_valid)
        return {false, "CANDIDATE_INVALID"};
    if (!input.physical_confirmation || input.transaction_id == 0 ||
        input.confirmed_transaction_id != input.transaction_id)
        return {false, "PHYSICAL_CONFIRMATION_REQUIRED"};
    if (!input.application_state_idle)
        return {false, "APPLICATION_STATE_BLOCKED"};
    if (!input.relay_idle)
        return {false, "RELAY_BUSY"};
    if (!input.inventory_available || input.inventory_pending)
        return {false, "INVENTORY_UNAVAILABLE"};
    if (input.quantity != 1)
        return {false, "INVALID_QUANTITY"};
    if (input.candidate_inventory_revision == 0 ||
        input.current_inventory_revision != input.candidate_inventory_revision)
        return {false, "STALE_INVENTORY_REVISION"};
    if (input.candidate_channel >= 16 || input.current_channel != input.candidate_channel)
        return {false, "ROUTE_CHANGED"};
    if (input.selected_count == 0)
        return {false, "OUT_OF_STOCK"};
    return {true, "AUTHORIZED"};
}
}  // namespace smv
