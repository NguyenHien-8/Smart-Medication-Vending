#include "medical_advisor.h"

#include <cJSON.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace smv {
namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

const char* StatusFor(MedicalDecision decision) {
    switch (decision) {
        case MedicalDecision::kAsk:
            return "ASK";
        case MedicalDecision::kRefer:
            return "REFER";
        case MedicalDecision::kOffer:
            return "PROVISIONAL_OPTIONS";
        case MedicalDecision::kDeny:
            return "DENY";
    }
    return "DENY";
}

uint32_t ElapsedUs(uint64_t start, uint64_t finish) {
    if (finish <= start)
        return 0;
    return static_cast<uint32_t>(
        std::min<uint64_t>(finish - start, std::numeric_limits<uint32_t>::max()));
}

struct FieldQuestion {
    const char* id;
    const char* question_vi;
};

constexpr FieldQuestion kFieldQuestions[] = {
    {"primary_symptom", "Triệu chứng chính làm bạn khó chịu nhất là gì?"},
    {"danger_signs",
     "Bạn có khó thở, đau ngực, ngất, co giật, chảy máu, đau dữ dội hoặc nặng lên nhanh "
     "không?"},
    {"age_years", "Bạn bao nhiêu tuổi?"},
    {"pregnancy_or_breastfeeding", "Bạn có đang mang thai hoặc cho con bú không?"},
    {"duration_hours", "Triệu chứng này đã kéo dài khoảng bao lâu?"},
    {"weight_kg", "Cân nặng hiện tại của bạn khoảng bao nhiêu ki-lô-gam?"},
    {"conditions", "Bạn có bệnh nền nào đang điều trị không?"},
    {"current_medicines", "Bạn đang dùng thuốc hoặc thực phẩm bổ sung nào không?"},
    {"drug_allergies", "Bạn có dị ứng với thuốc nào không?"},
};

const FieldQuestion* FindFieldQuestion(std::string_view field) {
    const auto found = std::find_if(std::begin(kFieldQuestions), std::end(kFieldQuestions),
                                    [field](const FieldQuestion& question) {
                                        return question.id == field;
                                    });
    return found == std::end(kFieldQuestions) ? nullptr : found;
}

void AddString(cJSON* object, const char* key, const std::string& value) {
    if (!value.empty())
        cJSON_AddStringToObject(object, key, value.c_str());
}
}  // namespace

MedicalAdvisor::MedicalAdvisor(const MedicalPolicyCache& policy, ClockUs clock_us)
    : policy_(policy), clock_us_(std::move(clock_us)) {}

uint64_t MedicalAdvisor::NowUs() const { return clock_us_ ? clock_us_() : 0; }

MedicalEvaluation MedicalAdvisor::AskForField(std::string_view field, std::string reason) const {
    MedicalEvaluation result;
    result.decision = MedicalDecision::kAsk;
    result.reason = std::move(reason);
    result.missing_field = std::string(field);
    if (const FieldQuestion* question = FindFieldQuestion(field)) {
        result.next_question_id = question->id;
        result.next_question_vi = question->question_vi;
    } else {
        result.next_question_id = "medical_clarification";
        result.next_question_vi = "Bạn vui lòng mô tả lại thông tin y tế vừa nêu rõ hơn được không?";
    }
    return result;
}

MedicalEvaluation MedicalAdvisor::AskQuestion(const PolicyQuestion& question,
                                               std::string reason) const {
    MedicalEvaluation result;
    result.decision = MedicalDecision::kAsk;
    result.reason = std::move(reason);
    result.next_question_id = question.id;
    result.next_question_vi = question.question_vi;
    return result;
}

std::string MedicalAdvisor::Serialize(const MedicalEvaluation& evaluation) const {
    Json root(cJSON_CreateObject(), &cJSON_Delete);
    if (!root)
        return R"({"status":"DENY","reason":"OUT_OF_MEMORY","vend_allowed":false})";
    cJSON_AddStringToObject(root.get(), "status", StatusFor(evaluation.decision));
    cJSON_AddStringToObject(root.get(), "reason", evaluation.reason.c_str());
    cJSON_AddFalseToObject(root.get(), "vend_allowed");
    AddString(root.get(), "session_id", evaluation.session_id);
    if (evaluation.turn_id != 0)
        cJSON_AddNumberToObject(root.get(), "turn_id", evaluation.turn_id);
    if (evaluation.facts_revision != 0)
        cJSON_AddNumberToObject(root.get(), "facts_revision", evaluation.facts_revision);
    AddString(root.get(), "next_question_id", evaluation.next_question_id);
    AddString(root.get(), "next_question_vi", evaluation.next_question_vi);
    AddString(root.get(), "missing_field", evaluation.missing_field);
    cJSON_AddNumberToObject(root.get(), "fast_screen_us", evaluation.fast_screen_us);
    cJSON_AddNumberToObject(root.get(), "full_evaluation_us", evaluation.full_evaluation_us);
    if (evaluation.offer) {
        cJSON_AddStringToObject(root.get(), "canonical_id",
                                evaluation.offer->canonical_id.c_str());
        cJSON_AddStringToObject(root.get(), "name", evaluation.offer->name.c_str());
        cJSON_AddStringToObject(root.get(), "active_ingredient",
                                evaluation.offer->active_ingredient.c_str());
        cJSON_AddStringToObject(root.get(), "strength", evaluation.offer->strength.c_str());
    }
    std::unique_ptr<char, decltype(&cJSON_free)> rendered(cJSON_PrintUnformatted(root.get()),
                                                          &cJSON_free);
    return rendered ? rendered.get()
                    : R"({"status":"DENY","reason":"OUT_OF_MEMORY","vend_allowed":false})";
}

MedicalEvaluation MedicalAdvisor::FullEvaluate(const MedicalFactBits& combined_facts) {
    MedicalEvaluation result;
    ++full_evaluation_count_;
    result.full_evaluation_ran = true;

    const auto primary = session_.facts().primary_symptom;
    if (!primary) {
        result.decision = MedicalDecision::kAsk;
        result.reason = "PRIMARY_SYMPTOM_REQUIRED";
        return result;
    }
    std::vector<size_t> eligible;
    for (const size_t rule_index : policy_.RulesForSymptom(*primary)) {
        const MedicinePolicy& rule = policy_.rules()[rule_index];
        if ((rule.refer_flags & combined_facts).any()) {
            result.decision = MedicalDecision::kRefer;
            result.reason = "RULE_REQUIRES_REFERRAL";
            return result;
        }
        if (rule.minimum_age_years && session_.facts().age_years &&
            *session_.facts().age_years < *rule.minimum_age_years) {
            continue;
        }
        if ((rule.exclude_flags & combined_facts).none())
            eligible.push_back(rule_index);
    }
    if (eligible.empty()) {
        result.decision = MedicalDecision::kRefer;
        result.reason = "NO_ELIGIBLE_MEDICINE";
        return result;
    }

    size_t selected = eligible.front();
    if (eligible.size() > 1) {
        std::optional<uint16_t> best_priority;
        size_t best_count = 0;
        for (const size_t rule_index : eligible) {
            const auto priority = policy_.rules()[rule_index].selection_priority;
            if (!priority)
                continue;
            if (!best_priority || *priority < *best_priority) {
                best_priority = priority;
                selected = rule_index;
                best_count = 1;
            } else if (*priority == *best_priority) {
                ++best_count;
            }
        }
        if (!best_priority || best_count != 1) {
            result.decision = MedicalDecision::kRefer;
            result.reason = "MULTIPLE_OPTIONS_REQUIRE_HUMAN_REVIEW";
            return result;
        }
    }

    const MedicinePolicy& rule = policy_.rules()[selected];
    const PolicyCatalogItem* item = policy_.FindCatalogItem(rule.canonical_id);
    if (item == nullptr) {
        result.decision = MedicalDecision::kDeny;
        result.reason = "POLICY_CATALOG_INCONSISTENT";
        return result;
    }
    result.decision = MedicalDecision::kOffer;
    result.reason = "LOCAL_POLICY_OPTION_READY";
    result.offer = MedicalOffer{item->canonical_id, item->name, item->active_ingredient,
                                item->strength};
    return result;
}

MedicalEvaluation MedicalAdvisor::EvaluateTurn(std::string_view delta_json, uint64_t now_ms) {
    const uint64_t fast_start = NowUs();
    const IntakeApplyResult applied = session_.ApplyDelta(policy_, delta_json, now_ms);
    if (applied.session_replaced) {
        cached_full_evaluation_.reset();
        cached_session_id_.clear();
        cached_facts_revision_ = 0;
    }
    MedicalEvaluation result;
    if (!applied.accepted) {
        result.decision = MedicalDecision::kDeny;
        result.reason = applied.reason;
    } else if (!applied.clarify_field.empty()) {
        result = AskForField(applied.clarify_field, applied.reason);
    }

    const NormalizedMedicalFacts& facts = session_.facts();
    if (applied.accepted && applied.clarify_field.empty()) {
        if (facts.danger_signs.any()) {
            result.decision = MedicalDecision::kRefer;
            result.reason = "POSSIBLE_DANGER_SIGN";
        } else if (facts.age_years && *facts.age_years < policy_.minimum_age_years()) {
            result.decision = MedicalDecision::kRefer;
            result.reason = "UNDER_MINIMUM_AGE";
        } else if (facts.pregnancy_or_breastfeeding && *facts.pregnancy_or_breastfeeding) {
            result.decision = MedicalDecision::kRefer;
            result.reason = "PREGNANCY_OR_BREASTFEEDING_UNSUPPORTED";
        } else if (!facts.primary_symptom) {
            result = AskForField("primary_symptom", "MISSING_REQUIRED_FACT");
        } else if (!facts.danger_signs_reported) {
            result = AskForField("danger_signs", "MISSING_REQUIRED_FACT");
        } else if (!facts.age_years) {
            result = AskForField("age_years", "MISSING_REQUIRED_FACT");
        } else if (!facts.pregnancy_or_breastfeeding) {
            result = AskForField("pregnancy_or_breastfeeding", "MISSING_REQUIRED_FACT");
        } else if (!facts.duration_hours) {
            result = AskForField("duration_hours", "MISSING_REQUIRED_FACT");
        } else if (!facts.weight_kg) {
            result = AskForField("weight_kg", "MISSING_REQUIRED_FACT");
        } else if (!facts.conditions_reported) {
            result = AskForField("conditions", "MISSING_REQUIRED_FACT");
        } else if (!facts.current_medicines_reported) {
            result = AskForField("current_medicines", "MISSING_REQUIRED_FACT");
        } else if (!facts.drug_allergies_reported) {
            result = AskForField("drug_allergies", "MISSING_REQUIRED_FACT");
        } else if (policy_.RulesForSymptom(*facts.primary_symptom).empty()) {
            result.decision = MedicalDecision::kRefer;
            result.reason = "UNSUPPORTED_SYMPTOM";
        } else {
            MedicalFactBits combined = facts.danger_signs | facts.conditions |
                                       facts.current_medicines | facts.drug_allergies;
            const auto set_derived = [&](const char* name, bool present) {
                const auto index = policy_.FlagIndex(name);
                if (index && present)
                    combined.set(*index);
            };
            set_derived("weight_below_50kg", facts.weight_kg && *facts.weight_kg < 50.0f);
            set_derived("age_16_or_17",
                        facts.age_years && *facts.age_years >= 16 && *facts.age_years <= 17);
            set_derived("duration_over_48_hours",
                        facts.duration_hours && *facts.duration_hours > 48);

            bool terminal = false;
            std::vector<size_t> candidates;
            for (const size_t rule_index : policy_.RulesForSymptom(*facts.primary_symptom)) {
                const MedicinePolicy& rule = policy_.rules()[rule_index];
                if ((rule.refer_flags & combined).any()) {
                    result.decision = MedicalDecision::kRefer;
                    result.reason = "RULE_REQUIRES_REFERRAL";
                    terminal = true;
                    break;
                }
                if (rule.minimum_age_years && *facts.age_years < *rule.minimum_age_years)
                    continue;
                if ((rule.exclude_flags & combined).none())
                    candidates.push_back(rule_index);
            }
            if (!terminal && candidates.empty()) {
                result.decision = MedicalDecision::kRefer;
                result.reason = "NO_ELIGIBLE_MEDICINE";
                terminal = true;
            }

            if (!terminal) {
                std::vector<size_t> question_indices =
                    policy_.QuestionsForSymptom(*facts.primary_symptom);
                std::sort(question_indices.begin(), question_indices.end(),
                          [this](size_t left, size_t right) {
                              return policy_.questions()[left].priority <
                                     policy_.questions()[right].priority;
                          });
                for (const size_t question_index : question_indices) {
                    const PolicyQuestion& question = policy_.questions()[question_index];
                    const int8_t answer = facts.screening_answers[question_index];
                    if (answer < 0) {
                        result = AskQuestion(question, "UNANSWERED_SYMPTOM_QUESTION");
                        terminal = true;
                        break;
                    }
                    if ((answer != 0) != question.expected) {
                        if (question.on_mismatch == QuestionAction::kRefer) {
                            result.decision = MedicalDecision::kRefer;
                            result.reason = "INTERVIEW_DANGER_OR_CONTRAINDICATION";
                        } else {
                            result = AskQuestion(question, "SCREENING_CLARIFICATION_REQUIRED");
                        }
                        terminal = true;
                        break;
                    }
                }
            }

            if (!terminal) {
                const bool cache_hit = cached_full_evaluation_ &&
                                       cached_session_id_ == session_.session_id() &&
                                       cached_facts_revision_ == session_.facts_revision() &&
                                       cached_rules_version_ == policy_.rules_version() &&
                                       cached_catalog_version_ == policy_.catalog_version();
                if (cache_hit) {
                    result = *cached_full_evaluation_;
                    result.full_evaluation_ran = false;
                    result.full_evaluation_us = 0;
                } else {
                    const uint64_t full_start = NowUs();
                    result = FullEvaluate(combined);
                    result.full_evaluation_us = ElapsedUs(full_start, NowUs());
                    cached_full_evaluation_ = result;
                    cached_session_id_ = session_.session_id();
                    cached_facts_revision_ = session_.facts_revision();
                    cached_rules_version_ = policy_.rules_version();
                    cached_catalog_version_ = policy_.catalog_version();
                }
            }
        }
    }

    result.session_id = session_.session_id();
    result.turn_id = session_.highest_turn_id();
    result.facts_revision = session_.facts_revision();
    result.fast_screen_us = ElapsedUs(fast_start, NowUs());
    result.response_json = Serialize(result);
    return result;
}

std::string MedicalAdvisor::IntakeSchema() const {
    Json root(cJSON_CreateObject(), &cJSON_Delete);
    if (!root || !policy_.valid())
        return R"({"status":"DENY","reason":"LOCAL_DATA_UNAVAILABLE","vend_allowed":false})";
    cJSON_AddStringToObject(root.get(), "purpose", "INTAKE_ONLY_NO_DIAGNOSIS_NO_VEND");
    cJSON_AddStringToObject(
        root.get(), "instruction",
        "Send only newly confirmed facts after each reply. Ask exactly next_question_vi. Omit "
        "unknown facts; use [] only after the user explicitly says none. Never send SKU, channel, "
        "stock, relay, quantity, or authorization fields.");
    cJSON* required = cJSON_AddArrayToObject(root.get(), "required_fields");
    for (const auto& question : kFieldQuestions)
        cJSON_AddItemToArray(required, cJSON_CreateString(question.id));
    cJSON_AddFalseToObject(root.get(), "vend_allowed");
    std::unique_ptr<char, decltype(&cJSON_free)> rendered(cJSON_PrintUnformatted(root.get()),
                                                          &cJSON_free);
    const std::string output = rendered ? rendered.get() : "";
    return output.size() <= 3200
               ? output
               : R"({"status":"DENY","reason":"INTAKE_SCHEMA_TOO_LARGE","vend_allowed":false})";
}

std::string MedicalAdvisor::SymptomGuide(std::string_view symptom_enum) const {
    if (!policy_.valid() || !policy_.SymptomIndex(symptom_enum))
        return R"({"status":"DENY","reason":"UNKNOWN_SYMPTOM","vend_allowed":false})";
    Json root(cJSON_CreateObject(), &cJSON_Delete);
    if (!root)
        return R"({"status":"DENY","reason":"OUT_OF_MEMORY","vend_allowed":false})";
    cJSON_AddStringToObject(root.get(), "symptom_enum", std::string(symptom_enum).c_str());
    cJSON* questions = cJSON_AddArrayToObject(root.get(), "checks_reference_only");
    for (const size_t question_index : policy_.QuestionsForSymptom(symptom_enum)) {
        const PolicyQuestion& question = policy_.questions()[question_index];
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", question.id.c_str());
        cJSON_AddStringToObject(item, "question_vi", question.question_vi.c_str());
        cJSON_AddItemToArray(questions, item);
    }
    cJSON_AddStringToObject(root.get(), "instruction",
                            "Dùng để làm rõ triệu chứng; quyết định tiếp theo chỉ lấy từ "
                            "evaluate_symptoms.");
    cJSON_AddFalseToObject(root.get(), "vend_allowed");
    std::unique_ptr<char, decltype(&cJSON_free)> rendered(cJSON_PrintUnformatted(root.get()),
                                                          &cJSON_free);
    const std::string output = rendered ? rendered.get() : "";
    return output.size() <= 2400
               ? output
               : R"({"status":"DENY","reason":"SYMPTOM_GUIDE_TOO_LARGE","vend_allowed":false})";
}

void MedicalAdvisor::ResetSession() {
    session_.Reset();
    cached_full_evaluation_.reset();
    cached_session_id_.clear();
    cached_facts_revision_ = 0;
    cached_rules_version_.clear();
    cached_catalog_version_.clear();
}
}  // namespace smv
