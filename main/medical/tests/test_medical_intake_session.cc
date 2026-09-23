#include "medical/medical_intake_session.h"

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
}  // namespace

int main(int argc, char** argv) {
    if (argc != 3)
        return 2;
    try {
        const smv::MedicalPolicyCache policy(Read(argv[1]), Read(argv[2]));
        Check(policy.valid(), "repository policy fixture rejected");
        int count = 0;

        smv::MedicalIntakeSession session;
        Check(
            session
                .ApplyDelta(policy,
                            R"({"session_id":"s","turn_id":1,"symptoms":["mild_headache"]})", 1000)
                .accepted,
            "first delta rejected");
        ++count;
        Check(session.facts().primary_symptom == policy.SymptomIndex("mild_headache"),
              "single first symptom did not establish primary symptom");
        ++count;
        Check(session.ApplyDelta(policy, R"({"session_id":"s","turn_id":2,"age_years":30})", 1100)
                  .accepted,
              "second delta rejected");
        ++count;
        Check(session.facts().primary_symptom == policy.SymptomIndex("mild_headache") &&
                  session.facts().age_years == 30,
              "omitted facts were lost during merge");
        ++count;

        Check(session
                  .ApplyDelta(policy,
                              R"({"session_id":"s","turn_id":3,"conditions":["liver_disease"]})",
                              1200)
                  .accepted,
              "known condition rejected");
        Check(session.facts().conditions.any() && session.facts().conditions_reported,
              "known condition was not retained");
        ++count;
        Check(session.ApplyDelta(policy, R"({"session_id":"s","turn_id":4,"conditions":[]})", 1300)
                  .accepted,
              "explicit empty condition list rejected");
        Check(session.facts().conditions.none() && session.facts().conditions_reported,
              "explicit empty condition list did not replace prior value");
        ++count;

        Check(!session.ApplyDelta(policy, R"({"session_id":"s","turn_id":4})", 1400).accepted,
              "replayed turn accepted");
        ++count;
        Check(!session.ApplyDelta(policy, R"({"session_id":"s","turn_id":3})", 1400).accepted,
              "decreasing turn accepted");
        ++count;
        Check(!session
                   .ApplyDelta(policy, R"({"session_id":"s","session_id":"dup","turn_id":5})", 1400)
                   .accepted,
              "duplicate top-level key accepted");
        ++count;
        Check(
            !session
                 .ApplyDelta(
                     policy,
                     R"({"session_id":"s","turn_id":5,"screening_answers":{"redflag_breathing":false,"redflag_breathing":true}})",
                     1400)
                 .accepted,
            "duplicate screening answer accepted");
        ++count;
        for (const char* control :
             {"sku", "channel", "stock", "relay", "quantity", "vend_allowed"}) {
            const std::string delta =
                std::string("{\"session_id\":\"s\",\"turn_id\":5,\"") + control + "\":true}";
            Check(!session.ApplyDelta(policy, delta, 1400).accepted,
                  "forbidden vending control field accepted");
            ++count;
        }
        Check(!session.ApplyDelta(policy, std::string(4097, 'x'), 1400).accepted,
              "oversized turn accepted");
        ++count;

        smv::MedicalIntakeSession expiry;
        Check(expiry.ApplyDelta(policy, R"({"session_id":"e","turn_id":1,"age_years":25})", 1000)
                  .accepted,
              "expiry fixture rejected");
        auto before_expiry =
            expiry.ApplyDelta(policy, R"({"session_id":"e","turn_id":2,"weight_kg":60})", 600999);
        Check(before_expiry.accepted && !before_expiry.session_replaced,
              "session expired before ten-minute boundary");
        ++count;
        auto at_expiry = expiry.ApplyDelta(
            policy, R"({"session_id":"e","turn_id":1,"symptoms":["dry_cough"]})", 1200999);
        Check(at_expiry.accepted && at_expiry.session_replaced && !expiry.facts().age_years &&
                  expiry.facts().primary_symptom == policy.SymptomIndex("dry_cough"),
              "session did not reset at ten-minute boundary");
        ++count;
        auto new_session = expiry.ApplyDelta(
            policy, R"({"session_id":"new","turn_id":1,"age_years":40})", 1201000);
        Check(new_session.accepted && new_session.session_replaced &&
                  expiry.session_id() == "new" && !expiry.facts().primary_symptom,
              "new session ID did not replace prior facts");
        ++count;

        smv::MedicalIntakeSession correction;
        Check(
            correction
                .ApplyDelta(
                    policy,
                    R"({"session_id":"c","turn_id":1,"symptoms":["mild_headache"],"screening_answers":{"redflag_breathing":false,"headache_mild":true}})",
                    1)
                .accepted,
            "correction fixture rejected");
        const auto global_index = policy.QuestionIndex("redflag_breathing");
        const auto old_symptom_index = policy.QuestionIndex("headache_mild");
        Check(global_index && old_symptom_index &&
                  correction.facts().screening_answers[*global_index] == 0 &&
                  correction.facts().screening_answers[*old_symptom_index] == 1,
              "screening answers were not recorded");
        ++count;
        const auto correction_result = correction.ApplyDelta(
            policy,
            R"({"session_id":"c","turn_id":2,"primary_symptom":"dry_cough","symptoms":["dry_cough"]})",
            2);
        Check(correction_result.accepted &&
                  correction.facts().primary_symptom == policy.SymptomIndex("dry_cough") &&
                  correction.facts().screening_answers[*global_index] == 0 &&
                  correction.facts().screening_answers[*old_symptom_index] == -1,
              "primary correction did not clear only old symptom answers");
        ++count;

        smv::MedicalIntakeSession danger;
        Check(danger.ApplyDelta(policy, R"({"session_id":"d","turn_id":1,"danger_signs":[]})", 1)
                      .accepted &&
                  danger.facts().danger_signs_reported && danger.facts().danger_signs.none(),
              "explicit no-danger response rejected");
        ++count;
        Check(
            danger.ApplyDelta(policy,
                              R"({"session_id":"d","turn_id":2,"danger_signs":["chest_pain"]})", 2)
                    .accepted &&
                danger.facts().danger_signs.test(*policy.FlagIndex("chest_pain")),
            "new danger after an earlier safe answer was lost");
        ++count;

        smv::MedicalIntakeSession ambiguous;
        const auto ambiguous_result = ambiguous.ApplyDelta(
            policy, R"({"session_id":"a","turn_id":1,"symptoms":["mild_headache","dry_cough"]})",
            1);
        Check(ambiguous_result.accepted && ambiguous_result.clarify_field == "primary_symptom" &&
                  !ambiguous.facts().primary_symptom,
              "multiple symptoms did not request a primary symptom");
        ++count;
        auto unknown = ambiguous.ApplyDelta(
            policy, R"({"session_id":"a","turn_id":2,"conditions":["unknown"]})", 2);
        Check(unknown.accepted && unknown.clarify_field == "conditions" &&
                  unknown.reason == "UNKNOWN_MEDICAL_TERM",
              "literal unknown did not return a clarification result");
        ++count;
        auto outside = ambiguous.ApplyDelta(
            policy, R"({"session_id":"a","turn_id":3,"drug_allergies":["penicillin"]})", 3);
        Check(outside.accepted && outside.clarify_field == "drug_allergies" &&
                  outside.reason == "UNKNOWN_MEDICAL_TERM",
              "out-of-vocabulary medical token was not clarified");
        ++count;
        auto sentinel = ambiguous.ApplyDelta(
            policy, R"({"session_id":"a","turn_id":4,"drug_allergies":["other_drug_allergy"]})", 4);
        Check(sentinel.accepted && sentinel.clarify_field.empty() &&
                  ambiguous.facts().drug_allergies.test(*policy.FlagIndex("other_drug_allergy")),
              "approved non-excluding sentinel was rejected");
        ++count;

        Check(policy.artifact_parse_count() == 2, "turn deltas reparsed static policy artifacts");
        ++count;
        Check(session.facts_revision() >= 4 && session.highest_turn_id() == 4,
              "session revisions or turn tracking are incorrect");
        ++count;
        std::cout << "HOST_MEDICAL_INTAKE_SESSION_TESTS_PASS=" << count << "\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}
