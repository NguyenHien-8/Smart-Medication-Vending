#include "medical_advisor.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {
void Check(bool passed, const char* message) {
    if (!passed)
        throw std::runtime_error(message);
}

std::string Read(const char* path) {
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error(path);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::string ReplaceOnce(std::string value, const std::string& from, const std::string& to) {
    const auto position = value.find(from);
    if (position == std::string::npos)
        throw std::runtime_error("fixture token missing");
    value.replace(position, from.size(), to);
    return value;
}

struct Clock {
    uint64_t operator()() {
        value += 10;
        return value;
    }
    uint64_t value = 0;
};

std::string CompleteDelta(std::string_view symptom, std::string_view answers,
                          std::string_view conditions = "[]",
                          std::string_view allergies = "[]", uint32_t turn = 2) {
    std::ostringstream json;
    json << "{\"session_id\":\"s\",\"turn_id\":" << turn
         << ",\"primary_symptom\":\"" << symptom
         << "\",\"age_years\":30,\"weight_kg\":65,"
            "\"pregnancy_or_breastfeeding\":false,\"duration_hours\":3,"
            "\"danger_signs\":[],\"conditions\":"
         << conditions
         << ",\"current_medicines\":[],\"drug_allergies\":" << allergies
         << ",\"screening_answers\":" << answers << "}";
    return json.str();
}

std::string HeadacheAnswers(bool mild = true) {
    return std::string("{\"headache_sudden\":false,\"headache_vomit\":false,") +
           "\"headache_stiff\":false,\"headache_mild\":" + (mild ? "true}" : "false}");
}

std::string DiarrhoeaAnswers() {
    return "{\"diarrhea_watery\":true,\"diarrhea_blood\":false}";
}

std::string DryCoughAnswers() {
    return "{\"dry_has_phlegm\":false,\"dry_cough_blood\":false}";
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 3)
        return 2;
    try {
        const std::string rules = Read(argv[1]);
        const std::string catalog = Read(argv[2]);
        int count = 0;

        Clock clock;
        const smv::MedicalPolicyCache policy(rules, catalog);
        Check(policy.valid(), "repository policy rejected");
        smv::MedicalAdvisor advisor(policy, [&clock]() { return clock(); });

        const auto first = advisor.EvaluateTurn(
            R"({"session_id":"s","turn_id":1,"symptoms":["mild_headache"]})", 1000);
        Check(first.decision == smv::MedicalDecision::kAsk,
              "missing facts did not produce ASK");
        Check(!first.next_question_vi.empty() && !first.next_question_id.empty(),
              "ASK dropped the next question");
        Check(first.fast_screen_us > 0 && !first.full_evaluation_ran,
              "fast-screen timing or stage marker missing");
        ++count;

        const auto offer =
            advisor.EvaluateTurn(CompleteDelta("mild_headache", HeadacheAnswers()), 1100);
        Check(offer.decision == smv::MedicalDecision::kOffer && offer.offer &&
                  offer.offer->canonical_id == "PARACETAMOL_500",
              "completed mild headache did not offer paracetamol");
        Check(offer.full_evaluation_ran && offer.full_evaluation_us > 0,
              "full-stage timing or stage marker missing");
        Check(advisor.full_evaluation_count() == 1,
              "full rules were evaluated more than once for one facts revision");
        Check(offer.response_json.find("\"sku\"") == std::string::npos &&
                  offer.response_json.find("\"channel\"") == std::string::npos,
              "medical result leaked physical routing data");
        ++count;

        const auto cached = advisor.EvaluateTurn(
            R"({"session_id":"s","turn_id":3,"age_years":30})", 1200);
        Check(cached.decision == smv::MedicalDecision::kOffer && !cached.full_evaluation_ran &&
                  advisor.full_evaluation_count() == 1,
              "unchanged facts reran the full evaluation");
        ++count;
        const auto corrected = advisor.EvaluateTurn(
            R"({"session_id":"s","turn_id":4,"weight_kg":45})", 1300);
        Check(corrected.decision == smv::MedicalDecision::kRefer &&
                  advisor.full_evaluation_count() == 1,
              "known refer_if did not refer in the fast screen");
        ++count;
        const auto restored = advisor.EvaluateTurn(
            R"({"session_id":"s","turn_id":5,"weight_kg":65})", 1400);
        Check(restored.decision == smv::MedicalDecision::kOffer && restored.full_evaluation_ran &&
                  advisor.full_evaluation_count() == 2,
              "fact correction did not rerun full evaluation exactly once");
        ++count;
        const auto expired_same_id = advisor.EvaluateTurn(
            CompleteDelta("dry_cough", DryCoughAnswers(), "[]", "[]", 1), 601400);
        Check(expired_same_id.decision == smv::MedicalDecision::kOffer &&
                  expired_same_id.offer &&
                  expired_same_id.offer->canonical_id == "DEXTROMETHORPHAN_15" &&
                  expired_same_id.full_evaluation_ran && advisor.full_evaluation_count() == 3,
              "expired same-ID session reused a stale cached offer");
        ++count;

        {
            Clock local_clock;
            smv::MedicalAdvisor danger(policy, [&local_clock]() { return local_clock(); });
            const auto result = danger.EvaluateTurn(
                R"({"session_id":"danger","turn_id":1,"danger_signs":["chest_pain"]})",
                1);
            Check(result.decision == smv::MedicalDecision::kRefer &&
                      result.reason == "POSSIBLE_DANGER_SIGN",
                  "explicit danger did not refer before missing-field checks");
            ++count;
        }
        {
            Clock local_clock;
            smv::MedicalAdvisor unknown(policy, [&local_clock]() { return local_clock(); });
            const auto result = unknown.EvaluateTurn(
                R"({"session_id":"unknown","turn_id":1,"conditions":["unknown"]})", 1);
            Check(result.decision == smv::MedicalDecision::kAsk &&
                      result.missing_field == "conditions" && !result.next_question_vi.empty(),
                  "unknown medical token did not ask for clarification");
            ++count;
        }
        {
            Clock local_clock;
            smv::MedicalAdvisor sentinel(policy, [&local_clock]() { return local_clock(); });
            sentinel.EvaluateTurn(
                R"({"session_id":"s","turn_id":1,"symptoms":["mild_headache"]})", 1);
            const auto result = sentinel.EvaluateTurn(
                CompleteDelta("mild_headache", HeadacheAnswers(), "[]",
                              "[\"other_drug_allergy\"]"),
                2);
            Check(result.decision == smv::MedicalDecision::kOffer,
                  "unrelated approved sentinel allergy excluded paracetamol");
            ++count;
        }
        {
            Clock local_clock;
            smv::MedicalAdvisor clarify(policy, [&local_clock]() { return local_clock(); });
            clarify.EvaluateTurn(
                R"({"session_id":"s","turn_id":1,"symptoms":["mild_headache"]})", 1);
            const auto result = clarify.EvaluateTurn(
                CompleteDelta("mild_headache", HeadacheAnswers(false)), 2);
            Check(result.decision == smv::MedicalDecision::kAsk &&
                      result.next_question_id == "headache_mild",
                  "CLARIFY mismatch did not ask its policy question");
            ++count;
        }
        {
            Clock local_clock;
            smv::MedicalAdvisor exclusions(policy, [&local_clock]() { return local_clock(); });
            exclusions.EvaluateTurn(
                R"({"session_id":"s","turn_id":1,"symptoms":["acute_watery_diarrhoea"]})",
                1);
            const auto result = exclusions.EvaluateTurn(
                CompleteDelta("acute_watery_diarrhoea", DiarrhoeaAnswers(), "[\"fever\"]"),
                2);
            Check(result.decision == smv::MedicalDecision::kOffer && result.offer &&
                      result.offer->canonical_id == "S_BOULARDII_250",
                  "candidate-local exclude_if removed the wrong candidate");
            ++count;
        }
        {
            Clock local_clock;
            smv::MedicalAdvisor ambiguous(policy, [&local_clock]() { return local_clock(); });
            ambiguous.EvaluateTurn(
                R"({"session_id":"s","turn_id":1,"symptoms":["acute_watery_diarrhoea"]})",
                1);
            const auto result = ambiguous.EvaluateTurn(
                CompleteDelta("acute_watery_diarrhoea", DiarrhoeaAnswers()), 2);
            Check(result.decision == smv::MedicalDecision::kRefer && !result.offer,
                  "unprioritized multiple options created an offer");
            ++count;
        }

        const std::string loperamide = "\"canonical_id\": \"LOPERAMIDE_2\",";
        const std::string boulardii = "\"canonical_id\": \"S_BOULARDII_250\",";
        {
            std::string prioritized = ReplaceOnce(
                rules, loperamide, loperamide + "\n      \"selection_priority\": 10,");
            prioritized = ReplaceOnce(prioritized, boulardii,
                                      boulardii + "\n      \"selection_priority\": 20,");
            const smv::MedicalPolicyCache priority_policy(prioritized, catalog);
            Check(priority_policy.valid(), "priority policy fixture rejected");
            Clock local_clock;
            smv::MedicalAdvisor priority(priority_policy,
                                         [&local_clock]() { return local_clock(); });
            priority.EvaluateTurn(
                R"({"session_id":"s","turn_id":1,"symptoms":["acute_watery_diarrhoea"]})",
                1);
            const auto result = priority.EvaluateTurn(
                CompleteDelta("acute_watery_diarrhoea", DiarrhoeaAnswers()), 2);
            Check(result.decision == smv::MedicalDecision::kOffer && result.offer &&
                      result.offer->canonical_id == "LOPERAMIDE_2",
                  "unique pharmacist-authored priority did not select a candidate");
            ++count;
        }
        {
            std::string tied = ReplaceOnce(
                rules, loperamide, loperamide + "\n      \"selection_priority\": 10,");
            tied = ReplaceOnce(tied, boulardii,
                               boulardii + "\n      \"selection_priority\": 10,");
            const smv::MedicalPolicyCache tie_policy(tied, catalog);
            Check(tie_policy.valid(), "priority tie policy fixture rejected");
            Clock local_clock;
            smv::MedicalAdvisor tie(tie_policy, [&local_clock]() { return local_clock(); });
            tie.EvaluateTurn(
                R"({"session_id":"s","turn_id":1,"symptoms":["acute_watery_diarrhoea"]})",
                1);
            const auto result = tie.EvaluateTurn(
                CompleteDelta("acute_watery_diarrhoea", DiarrhoeaAnswers()), 2);
            Check(result.decision == smv::MedicalDecision::kRefer && !result.offer,
                  "tied priorities created an offer");
            ++count;
        }

        Check(policy.artifact_parse_count() == 2,
              "static JSON was reparsed during the conversation");
        ++count;
        const std::string schema = advisor.IntakeSchema();
        Check(schema.size() <= 3200 &&
                  schema.find("INTAKE_ONLY_NO_DIAGNOSIS_NO_VEND") != std::string::npos,
              "cache-backed intake schema invalid");
        ++count;
        const std::string guide = advisor.SymptomGuide("mild_headache");
        Check(guide.size() <= 2400 && guide.find("headache_mild") != std::string::npos,
              "cache-backed symptom guide invalid");
        ++count;
        advisor.ResetSession();
        Check(advisor.facts_revision() == 0, "advisor session reset retained facts");
        ++count;

        std::cout << "HOST_MEDICAL_ADVISOR_TESTS_PASS=" << count << "\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}
