#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "medical_intake_session.h"
#include "medical_policy_cache.h"

namespace smv {
enum class MedicalDecision { kDeny, kAsk, kRefer, kOffer };

struct MedicalOffer {
    std::string canonical_id;
    std::string name;
    std::string active_ingredient;
    std::string strength;
};

struct MedicalEvaluation {
    MedicalDecision decision = MedicalDecision::kDeny;
    std::string reason = "NOT_EVALUATED";
    std::string session_id;
    uint32_t turn_id = 0;
    uint32_t facts_revision = 0;
    std::string next_question_id;
    std::string next_question_vi;
    std::string missing_field;
    std::optional<MedicalOffer> offer;
    bool full_evaluation_ran = false;
    uint32_t fast_screen_us = 0;
    uint32_t full_evaluation_us = 0;
    std::string response_json;
};

class MedicalAdvisor {
public:
    using ClockUs = std::function<uint64_t()>;

    MedicalAdvisor(const MedicalPolicyCache& policy, ClockUs clock_us);
    std::string IntakeSchema() const;
    std::string SymptomGuide(std::string_view symptom_enum) const;
    MedicalEvaluation EvaluateTurn(std::string_view delta_json, uint64_t now_ms);
    void ResetSession();
    uint32_t facts_revision() const { return session_.facts_revision(); }
    uint32_t full_evaluation_count() const { return full_evaluation_count_; }

private:
    MedicalEvaluation AskForField(std::string_view field, std::string reason) const;
    MedicalEvaluation AskQuestion(const PolicyQuestion& question, std::string reason) const;
    MedicalEvaluation FullEvaluate(const MedicalFactBits& combined_facts);
    std::string Serialize(const MedicalEvaluation& evaluation) const;
    uint64_t NowUs() const;

    const MedicalPolicyCache& policy_;
    ClockUs clock_us_;
    MedicalIntakeSession session_;
    uint32_t full_evaluation_count_ = 0;
    std::optional<MedicalEvaluation> cached_full_evaluation_;
    std::string cached_session_id_;
    uint32_t cached_facts_revision_ = 0;
    std::string cached_rules_version_;
    std::string cached_catalog_version_;
};
}  // namespace smv
