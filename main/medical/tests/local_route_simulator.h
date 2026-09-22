#pragma once

#include <array>
#include <cstdint>
#include <string>

// HOST-ONLY bench simulation. Do not compile into ESP-IDF firmware or connect
// the callbacks to GPIO. Real dispensing requires an independently reviewed
// clinical policy, verified stock/lot/expiry and closed-loop delivery sensing.
namespace smv::bench {
struct Inventory {
    // Values MUST come from an independent, verified source, never
    // medicines.json.initial_stock or any AI-provided text.
    std::array<unsigned, 16> measured{};
    bool externally_verified = false;
};
struct Route {
    std::string status = "BLOCKED";
    std::string reason = "NOT_EVALUATED";
    std::string sku;
    std::string canonical_id;
    int channel = -1;
};

// All inputs are local test fixtures. Never exposes a vend authorization.
Route PreviewRoute(const std::string& advisor_output,
                   const std::string& catalog_json,
                   const std::string& rules_json,
                   const std::string& review_json,
                   const Inventory& inventory);

// Logical, non-blocking active-LOW pulse simulation; no hardware access.
class RelayPulseSimulation {
public:
    bool Start(const Route& route, uint32_t now_ms);
    void Tick(uint32_t now_ms);
    void Cancel();
    bool active() const { return active_; }
    int channel() const { return channel_; }
    int logical_level() const { return active_ ? 0 : 1; }
private:
    bool active_ = false;
    int channel_ = -1;
    uint32_t started_ms_ = 0;
};
} // namespace smv::bench
