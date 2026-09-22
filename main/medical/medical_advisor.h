#pragma once

#include <cstdint>
#include <optional>
#include <string>

// Read-only, fail-closed first-stage advisor. No GPIO, inventory mutation, or vend API.
// Inputs from AI are NEVER treated as user consent or clinical verification.
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
    std::optional<MedicalOffer> offer;
    std::string response_json;
};

class MedicalAdvisor {
public:
    MedicalAdvisor(const char* rules_json, const char* catalog_json, const char* review_json);
    std::string IntakeSchema() const;
    std::string SymptomGuide(const std::string& symptom_enum) const;
    MedicalEvaluation EvaluateStructured(const std::string& untrusted_json) const;
    std::string Evaluate(const std::string& untrusted_json) const;

private:
    std::string rules_;
    std::string catalog_;
    std::string review_;
};
}  // namespace smv
