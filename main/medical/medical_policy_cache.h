#pragma once

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "vending/vending_types.h"

namespace smv {
inline constexpr size_t kMaxMedicalRules = 16;
inline constexpr size_t kMaxMedicalQuestions = 64;
inline constexpr size_t kMaxMedicalFlags = 128;
inline constexpr size_t kMaxMedicalSymptoms = 32;

enum class QuestionAction { kRefer, kClarify };

struct PolicyQuestion {
    std::string id;
    std::string question_vi;
    bool expected = false;
    QuestionAction on_mismatch = QuestionAction::kRefer;
    uint16_t priority = 0;
};

struct MedicinePolicy {
    std::string canonical_id;
    std::vector<size_t> symptom_indices;
    std::bitset<kMaxMedicalFlags> refer_flags;
    std::bitset<kMaxMedicalFlags> exclude_flags;
    std::optional<uint8_t> minimum_age_years;
    std::optional<uint16_t> selection_priority;
};

struct PolicyCatalogItem {
    std::string canonical_id;
    std::string name;
    std::string active_ingredient;
    std::string strength;
    std::string primary_sku;
    uint8_t primary_channel = kInvalidVendingChannel;
    std::optional<std::string> backup_sku;
    std::optional<uint8_t> backup_channel;
};

class MedicalPolicyCache {
public:
    MedicalPolicyCache(std::string_view rules_json, std::string_view catalog_json);

    bool valid() const { return valid_; }
    const std::string& validation_reason() const { return validation_reason_; }
    size_t artifact_parse_count() const { return artifact_parse_count_; }
    const std::string& rules_version() const { return rules_version_; }
    const std::string& catalog_version() const { return catalog_version_; }
    uint8_t minimum_age_years() const { return minimum_age_years_; }

    const std::vector<MedicinePolicy>& rules() const { return rules_; }
    const std::vector<PolicyQuestion>& questions() const { return questions_; }
    const std::vector<PolicyCatalogItem>& catalog_items() const { return catalog_items_; }
    const std::vector<size_t>& global_questions() const { return global_question_indices_; }
    const std::vector<size_t>& RulesForSymptom(std::string_view symptom) const;
    const std::vector<size_t>& RulesForSymptom(size_t symptom_index) const;
    const std::vector<size_t>& QuestionsForSymptom(std::string_view symptom) const;
    const std::vector<size_t>& QuestionsForSymptom(size_t symptom_index) const;
    const PolicyQuestion* FindQuestion(std::string_view id) const;
    std::optional<size_t> QuestionIndex(std::string_view id) const;
    const PolicyCatalogItem* FindCatalogItem(std::string_view canonical_id) const;
    std::optional<size_t> FlagIndex(std::string_view flag) const;
    std::optional<size_t> SymptomIndex(std::string_view symptom) const;
    bool IsRecognizedNonExcludingFlag(size_t flag_index) const;
    bool IsDangerFlag(size_t flag_index) const;

private:
    bool Fail(std::string reason);
    bool valid_ = false;
    std::string validation_reason_ = "NOT_PARSED";
    size_t artifact_parse_count_ = 0;
    std::string rules_version_;
    std::string catalog_version_;
    uint8_t minimum_age_years_ = 0;
    std::vector<MedicinePolicy> rules_;
    std::vector<PolicyQuestion> questions_;
    std::vector<PolicyCatalogItem> catalog_items_;
    std::vector<std::string> flag_names_;
    std::vector<std::string> symptom_names_;
    std::map<std::string, size_t, std::less<>> flag_indices_;
    std::map<std::string, size_t, std::less<>> symptom_indices_;
    std::map<std::string, size_t, std::less<>> question_indices_;
    std::map<std::string, size_t, std::less<>> catalog_indices_;
    std::vector<std::vector<size_t>> rules_by_symptom_;
    std::vector<std::vector<size_t>> questions_by_symptom_;
    std::vector<size_t> global_question_indices_;
    std::bitset<kMaxMedicalFlags> recognized_non_excluding_flags_;
    std::bitset<kMaxMedicalFlags> danger_flags_;
};
}  // namespace smv
