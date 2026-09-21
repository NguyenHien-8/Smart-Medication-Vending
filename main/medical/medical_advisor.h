#pragma once

#include <string>

// Read-only, fail-closed first-stage advisor. No GPIO, inventory mutation, or vend API.
// Inputs from AI are NEVER treated as user consent or clinical verification.
namespace smv {
class MedicalAdvisor {
public:
    MedicalAdvisor(const char* rules_json, const char* catalog_json, const char* review_json);
    std::string IntakeSchema() const;
    std::string SymptomGuide(const std::string& symptom_enum) const;
    std::string Evaluate(const std::string& untrusted_json) const;
private:
    std::string rules_;
    std::string catalog_;
    std::string review_;
};
}
