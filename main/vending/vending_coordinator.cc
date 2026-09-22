#include "vending_coordinator.h"

#include "vend_guard.h"

#include <cJSON.h>

#include <limits>
#include <memory>
#include <utility>

namespace smv {
namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

const char* StatusFor(MedicalDecision decision) {
    switch (decision) {
        case MedicalDecision::kAsk:
            return "ASK";
        case MedicalDecision::kRefer:
            return "REFER";
        case MedicalDecision::kDeny:
            return "BLOCK";
        case MedicalDecision::kOffer:
            return "OFFER";
    }
    return "BLOCK";
}
}  // namespace

VendingCoordinator::VendingCoordinator(MedicalAdvisor& advisor, CatalogRouter& router,
                                       InventoryStore& inventory, RelayActuator& relay,
                                       ArtifactIdentity identity, ReviewResult review,
                                       bool production_enabled,
                                       TransactionIdSource next_transaction_id,
                                       AuditSink audit_sink)
    : advisor_(advisor),
      router_(router),
      inventory_(inventory),
      relay_(relay),
      identity_(std::move(identity)),
      review_(review),
      production_enabled_(production_enabled),
      next_transaction_id_(std::move(next_transaction_id)),
      audit_sink_(std::move(audit_sink)) {}

std::string VendingCoordinator::Response(const char* status, const std::string& reason,
                                         const MedicalOffer* offer) const {
    Json response(cJSON_CreateObject(), &cJSON_Delete);
    if (!response)
        return "{\"status\":\"BLOCK\",\"reason\":\"OUT_OF_MEMORY\",\"vend_allowed\":false}";
    cJSON_AddStringToObject(response.get(), "status", status);
    cJSON_AddStringToObject(response.get(), "reason", reason.c_str());
    cJSON_AddFalseToObject(response.get(), "vend_allowed");
    if (offer != nullptr) {
        cJSON_AddStringToObject(response.get(), "name", offer->name.c_str());
        cJSON_AddStringToObject(response.get(), "active_ingredient",
                                offer->active_ingredient.c_str());
        cJSON_AddStringToObject(response.get(), "strength", offer->strength.c_str());
        cJSON_AddStringToObject(response.get(), "confirmation", "PRESS_PHYSICAL_BUTTON");
    }
    std::unique_ptr<char, decltype(&cJSON_free)> rendered(cJSON_PrintUnformatted(response.get()),
                                                          &cJSON_free);
    return rendered ? rendered.get()
                    : "{\"status\":\"BLOCK\",\"reason\":\"OUT_OF_MEMORY\",\"vend_allowed\":false}";
}

std::string VendingCoordinator::EvaluateAndStage(const std::string& patient_facts_json,
                                                 uint64_t now_ms) {
    candidate_.reset();
    const MedicalEvaluation evaluation = advisor_.EvaluateStructured(patient_facts_json);
    if (!evaluation.session_id.empty() && evaluation.turn_id != 0) {
        if (evaluation.session_id == last_session_id_ && evaluation.turn_id <= last_turn_id_)
            return Response("BLOCK", "STALE_TURN");
        last_session_id_ = evaluation.session_id;
        last_turn_id_ = evaluation.turn_id;
    }
    if (evaluation.decision != MedicalDecision::kOffer || !evaluation.offer)
        return Response(StatusFor(evaluation.decision), evaluation.reason);
    if (!production_enabled_)
        return Response("BLOCK", "PRODUCTION_DISABLED");
    if (!review_.valid)
        return Response("BLOCK", "PHARMACIST_REVIEW_INVALID");
    if (!router_.valid())
        return Response("BLOCK", router_.validation_reason());

    const RouteSelection route =
        router_.Select(evaluation.offer->canonical_id, inventory_.Snapshot());
    if (route.status != RouteStatus::kReady)
        return Response("BLOCK", route.reason);
    if (!next_transaction_id_ || now_ms > std::numeric_limits<uint64_t>::max() - 30000)
        return Response("BLOCK", "TRANSACTION_ID_UNAVAILABLE");
    const uint64_t transaction_id = next_transaction_id_();
    if (transaction_id == 0)
        return Response("BLOCK", "TRANSACTION_ID_UNAVAILABLE");

    candidate_ = Candidate{
        transaction_id,     evaluation.session_id,
        evaluation.turn_id, evaluation.offer->canonical_id,
        *evaluation.offer,  route,
        now_ms + 30000,
    };
    return Response("OFFER", "PHYSICAL_CONFIRMATION_REQUIRED", &candidate_->offer);
}

bool VendingCoordinator::AwaitingConfirmation(uint64_t now_ms) {
    if (!candidate_)
        return false;
    if (now_ms >= candidate_->expires_at_ms) {
        candidate_.reset();
        return false;
    }
    return true;
}

ConfirmResult VendingCoordinator::Confirm(uint64_t now_ms, bool application_state_idle) {
    if (!candidate_)
        return ConfirmResult::kNoCandidate;
    if (now_ms >= candidate_->expires_at_ms) {
        candidate_.reset();
        return ConfirmResult::kExpired;
    }

    const StockSnapshot& stock = inventory_.Snapshot();
    const RouteSelection current_route = router_.Select(candidate_->canonical_id, stock);
    VendGuardInput guard_input;
    guard_input.production_enabled = production_enabled_;
    guard_input.review_valid = review_.valid;
    guard_input.candidate_valid = current_route.status == RouteStatus::kReady;
    guard_input.physical_confirmation = true;
    guard_input.application_state_idle = application_state_idle;
    guard_input.relay_idle = relay_.IsIdle() && !active_.has_value();
    guard_input.inventory_available = stock.available;
    guard_input.inventory_pending = stock.transaction_pending;
    guard_input.quantity = 1;
    guard_input.transaction_id = candidate_->transaction_id;
    guard_input.confirmed_transaction_id = candidate_->transaction_id;
    guard_input.candidate_channel = candidate_->route.channel;
    guard_input.current_channel = current_route.channel;
    guard_input.candidate_inventory_revision = candidate_->route.inventory_revision;
    guard_input.current_inventory_revision = stock.revision;
    if (current_route.channel < kVendingChannelCount)
        guard_input.selected_count = stock.counts[current_route.channel];
    const VendGuardResult guard = VendGuard::Check(guard_input);
    if (!guard.authorized) {
        if (std::string_view(guard.reason) == "APPLICATION_STATE_BLOCKED")
            return ConfirmResult::kApplicationStateBlocked;
        candidate_.reset();
        return ConfirmResult::kBlocked;
    }

    Candidate confirmed = std::move(*candidate_);
    candidate_.reset();
    const auto token = inventory_.Reserve(confirmed.transaction_id, confirmed.route.channel,
                                          confirmed.route.inventory_revision);
    if (!token)
        return ConfirmResult::kInventoryFailed;

    active_ = ActiveTransaction{*token, confirmed.route, now_ms};
    const bool started =
        relay_.Start(*token, [this](uint64_t transaction_id, RelayOutcome outcome) {
            OnRelayOutcome(transaction_id, outcome);
        });
    if (!started) {
        if (active_)
            OnRelayOutcome(token->transaction_id, RelayOutcome::kNotStartedCertain);
        return ConfirmResult::kRelayStartFailed;
    }
    return ConfirmResult::kStarted;
}

void VendingCoordinator::EmitAudit(const ActiveTransaction& active, RelayOutcome outcome,
                                   const char* decision_reason) {
    if (!audit_sink_)
        return;
    audit_sink_(AuditEvent{
        active.token.transaction_id,
        identity_.rules_version,
        identity_.catalog_version,
        identity_.rules_sha256,
        identity_.catalog_sha256,
        decision_reason,
        active.token.channel,
        active.route.inventory_revision,
        active.token.reserved_revision,
        inventory_.Snapshot().revision,
        active.started_at_ms,
        outcome,
    });
}

void VendingCoordinator::OnRelayOutcome(uint64_t transaction_id, RelayOutcome outcome) {
    if (!active_ || active_->token.transaction_id != transaction_id)
        return;
    const ActiveTransaction completed = *active_;
    active_.reset();
    const char* reason = "UNCERTAIN_RELAY_OUTCOME";
    switch (outcome) {
        case RelayOutcome::kNotStartedCertain:
            inventory_.CancelBeforePulse(completed.token);
            reason = "NOT_STARTED_CERTAIN";
            break;
        case RelayOutcome::kCommandSentUnverified:
            inventory_.Complete(completed.token);
            reason = "COMMAND_SENT_UNVERIFIED";
            break;
        case RelayOutcome::kUncertain:
            break;
    }
    EmitAudit(completed, outcome, reason);
}

void VendingCoordinator::Cancel() {
    candidate_.reset();
    if (active_)
        relay_.Cancel();
}

void VendingCoordinator::OnDisconnected(uint64_t now_ms) {
    candidate_.reset();
    if (active_) {
        active_->started_at_ms = now_ms;
        relay_.Cancel();
    }
}
}  // namespace smv
