#pragma once

#include <array>
#include <bitset>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "medical_policy_cache.h"

namespace smv {
using MedicalFactBits = std::bitset<kMaxMedicalFlags>;

struct NormalizedMedicalFacts {
    NormalizedMedicalFacts() { screening_answers.fill(-1); }

    std::optional<uint8_t> age_years;
    std::optional<float> weight_kg;
    std::optional<bool> pregnancy_or_breastfeeding;
    std::optional<uint32_t> duration_hours;
    std::optional<size_t> primary_symptom;
    std::bitset<kMaxMedicalSymptoms> symptoms;
    MedicalFactBits danger_signs;
    MedicalFactBits conditions;
    MedicalFactBits current_medicines;
    MedicalFactBits drug_allergies;
    bool danger_signs_reported = false;
    bool conditions_reported = false;
    bool current_medicines_reported = false;
    bool drug_allergies_reported = false;
    std::array<int8_t, kMaxMedicalQuestions> screening_answers;

    bool operator==(const NormalizedMedicalFacts&) const = default;
};

struct IntakeApplyResult {
    bool accepted = false;
    bool session_replaced = false;
    bool facts_changed = false;
    std::string reason;
    std::string clarify_field;
};

class MedicalIntakeSession {
public:
    static constexpr uint64_t kSessionExpiryMs = 600000;
    static constexpr size_t kMaximumDeltaBytes = 4096;

    IntakeApplyResult ApplyDelta(const MedicalPolicyCache& policy, std::string_view delta_json,
                                 uint64_t now_ms);
    void Reset();

    bool active() const { return active_; }
    const std::string& session_id() const { return session_id_; }
    uint32_t highest_turn_id() const { return highest_turn_id_; }
    uint32_t facts_revision() const { return facts_revision_; }
    uint64_t last_activity_ms() const { return last_activity_ms_; }
    const NormalizedMedicalFacts& facts() const { return facts_; }

private:
    bool active_ = false;
    std::string session_id_;
    uint32_t highest_turn_id_ = 0;
    uint32_t facts_revision_ = 0;
    uint64_t last_activity_ms_ = 0;
    NormalizedMedicalFacts facts_;
};
}  // namespace smv
