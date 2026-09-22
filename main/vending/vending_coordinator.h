#pragma once

#include "catalog_router.h"
#include "inventory/inventory_store.h"
#include "medical/medical_advisor.h"
#include "medical/pharmacist_review_verifier.h"
#include "vending_types.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace smv {
struct AuditEvent {
    uint64_t transaction_id = 0;
    std::string rules_version;
    std::string catalog_version;
    std::string rules_sha256;
    std::string catalog_sha256;
    std::string decision_reason;
    uint8_t channel = kInvalidVendingChannel;
    uint32_t route_inventory_revision = 0;
    uint32_t reservation_revision = 0;
    uint32_t final_inventory_revision = 0;
    uint64_t monotonic_ms = 0;
    RelayOutcome relay_outcome = RelayOutcome::kUncertain;
};

enum class ConfirmResult {
    kStarted,
    kNoCandidate,
    kExpired,
    kApplicationStateBlocked,
    kBlocked,
    kInventoryFailed,
    kRelayStartFailed,
};

class VendingCoordinator {
public:
    using TransactionIdSource = std::function<uint64_t()>;
    using AuditSink = std::function<void(const AuditEvent&)>;

    VendingCoordinator(MedicalAdvisor& advisor, CatalogRouter& router, InventoryStore& inventory,
                       RelayActuator& relay, ArtifactIdentity identity, ReviewResult review,
                       bool production_enabled, TransactionIdSource next_transaction_id,
                       AuditSink audit_sink);

    std::string EvaluateAndStage(const std::string& patient_facts_json, uint64_t now_ms);
    bool AwaitingConfirmation(uint64_t now_ms);
    ConfirmResult Confirm(uint64_t now_ms, bool application_state_idle);
    void Cancel();
    void OnDisconnected(uint64_t now_ms);
    void OnRelayOutcome(uint64_t transaction_id, RelayOutcome outcome);

private:
    struct Candidate {
        uint64_t transaction_id = 0;
        std::string session_id;
        uint32_t turn_id = 0;
        std::string canonical_id;
        MedicalOffer offer;
        RouteSelection route;
        uint64_t expires_at_ms = 0;
    };

    struct ActiveTransaction {
        ReservationToken token;
        RouteSelection route;
        uint64_t started_at_ms = 0;
    };

    std::string Response(const char* status, const std::string& reason,
                         const MedicalOffer* offer = nullptr) const;
    void EmitAudit(const ActiveTransaction& active, RelayOutcome outcome,
                   const char* decision_reason);

    MedicalAdvisor& advisor_;
    CatalogRouter& router_;
    InventoryStore& inventory_;
    RelayActuator& relay_;
    ArtifactIdentity identity_;
    ReviewResult review_;
    bool production_enabled_ = false;
    TransactionIdSource next_transaction_id_;
    AuditSink audit_sink_;
    std::optional<Candidate> candidate_;
    std::optional<ActiveTransaction> active_;
    std::string last_session_id_;
    uint32_t last_turn_id_ = 0;
};
}  // namespace smv
