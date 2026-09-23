#include "medical/medical_policy_cache.h"

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
        throw std::runtime_error("fixture token missing: " + from);
    value.replace(position, from.size(), to);
    return value;
}

std::string RepositoryRules(std::string rules) {
    if (rules.find("\"recognized_non_excluding_flags\"") == std::string::npos) {
        rules = ReplaceOnce(
            std::move(rules), "  \"medicine_rules\": [",
            "  \"recognized_non_excluding_flags\": [\"other_condition\", "
            "\"other_current_medicine\", \"other_drug_allergy\"],\n  \"medicine_rules\": [");
    }
    return rules;
}

std::string RepositoryCatalog(std::string catalog) {
    const std::string mismatch = "\"strength\": \"cùng SKU channel 7\"";
    const auto position = catalog.find(mismatch);
    if (position != std::string::npos) {
        catalog.replace(position, mismatch.size(), "\"strength\": \"ví dụ 200 mg + 200 mg\"");
    }
    return catalog;
}

void ExpectInvalid(const std::string& rules, const std::string& catalog, const char* message) {
    const smv::MedicalPolicyCache policy(rules, catalog);
    Check(!policy.valid(), message);
    Check(!policy.validation_reason().empty() && policy.validation_reason() != "OK",
          "invalid policy did not expose a stable reason");
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 3)
        return 2;
    try {
        const std::string rules = RepositoryRules(Read(argv[1]));
        const std::string catalog = RepositoryCatalog(Read(argv[2]));
        const smv::MedicalPolicyCache policy(rules, catalog);
        int count = 0;

        Check(policy.valid(), policy.validation_reason().c_str());
        ++count;
        Check(policy.artifact_parse_count() == 2, "artifacts were not parsed exactly once");
        ++count;
        Check(policy.RulesForSymptom("mild_headache").size() == 1,
              "symptom index missing paracetamol");
        ++count;
        Check(policy.FindQuestion("headache_mild") != nullptr,
              "question index missing symptom check");
        ++count;
        const auto* paracetamol = policy.FindCatalogItem("PARACETAMOL_500");
        Check(paracetamol != nullptr && paracetamol->primary_channel == 0,
              "primary route not normalized");
        ++count;
        Check(paracetamol->backup_channel.has_value() && *paracetamol->backup_channel == 13,
              "backup route not normalized");
        ++count;
        Check(policy.FlagIndex("other_condition").has_value() &&
                  policy.FlagIndex("other_current_medicine").has_value() &&
                  policy.FlagIndex("other_drug_allergy").has_value(),
              "recognized non-excluding sentinels were not indexed");
        ++count;
        Check(policy.SymptomIndex("mild_headache").has_value(), "symptom lookup missing");
        ++count;
        Check(policy.rules_version().find("rules-") == 0 &&
                  policy.catalog_version().find("catalog-") == 0,
              "artifact versions were not retained");
        ++count;

        const std::string first_symptoms =
            "\"symptoms\": [\n        \"fever\",\n        \"mild_headache\",\n        "
            "\"mild_body_ache\"\n      ]";
        const std::string first_refer = "\"refer_if\": [\n        \"weight_below_50kg\"\n      ]";
        const std::string first_exclude =
            "\"exclude_if\": [\n        \"liver_disease\",\n        \"kidney_disease\",\n        "
            "\"heavy_alcohol_use\",\n        \"contains_paracetamol\"\n      ]";
        ExpectInvalid(ReplaceOnce(rules, first_symptoms, "\"symptoms\": null"), catalog,
                      "null symptoms accepted");
        ++count;
        ExpectInvalid(ReplaceOnce(rules, first_symptoms, "\"symptoms\": []"), catalog,
                      "empty symptoms accepted");
        ++count;
        ExpectInvalid(ReplaceOnce(rules, first_refer, "\"refer_if\": null"), catalog,
                      "null refer_if accepted");
        ++count;
        ExpectInvalid(ReplaceOnce(rules, first_refer, "\"refer_if\": []"), catalog,
                      "empty refer_if accepted");
        ++count;
        ExpectInvalid(ReplaceOnce(rules, first_exclude, "\"exclude_if\": [17, \"liver_disease\"]"),
                      catalog, "non-string exclude_if accepted");
        ++count;
        ExpectInvalid(
            ReplaceOnce(rules, "\"id\": \"headache_sudden\"", "\"id\": \"redflag_breathing\""),
            catalog, "duplicate question ID accepted");
        ++count;
        ExpectInvalid(ReplaceOnce(rules, "\"mild_headache\": [", "\"unused_headache\": ["), catalog,
                      "missing symptom profile accepted");
        ++count;
        ExpectInvalid(ReplaceOnce(rules, "\"canonical_id\": \"PARACETAMOL_500\"",
                                  "\"canonical_id\": \"UNKNOWN_MEDICINE\""),
                      catalog, "unknown canonical rule reference accepted");
        ++count;
        ExpectInvalid(ReplaceOnce(catalog, "\"channel\": 1", "\"channel\": 0"), rules,
                      "duplicate channel accepted");
        ++count;
        ExpectInvalid(ReplaceOnce(catalog, "\"channel\": 15", "\"channel\": 16"), rules,
                      "out-of-range channel accepted");
        ++count;
        ExpectInvalid(ReplaceOnce(catalog, "\"dispense_unit\": \"sealed_blister\"",
                                  "\"dispense_unit\": \"tablet\""),
                      rules, "unsupported dispense unit accepted");
        ++count;
        ExpectInvalid(ReplaceOnce(catalog, "\"strength\": \"500 mg\"", "\"strength\": \"501 mg\""),
                      rules, "primary/backup identity mismatch accepted");
        ++count;
        ExpectInvalid(ReplaceOnce(catalog, "\"name\": \"Paracetamol 500\"",
                                  "\"name\": \"" + std::string(200, 'x') + "\""),
                      rules, "oversized catalog string accepted");
        ++count;
        std::string oversized_flags = "[";
        for (size_t i = 0; i <= smv::kMaxMedicalFlags; ++i) {
            if (i != 0)
                oversized_flags += ',';
            oversized_flags += "\"flag_" + std::to_string(i) + "\"";
        }
        oversized_flags += ']';
        ExpectInvalid(
            ReplaceOnce(rules,
                        "[\n    \"other_condition\",\n    \"other_current_medicine\",\n    "
                        "\"other_drug_allergy\"\n  ]",
                        oversized_flags),
            catalog, "oversized flag array accepted");
        ++count;
        ExpectInvalid(rules + " trailing", catalog, "trailing rules bytes accepted");
        ++count;
        ExpectInvalid(rules, catalog + " trailing", "trailing catalog bytes accepted");
        ++count;
        ExpectInvalid(ReplaceOnce(rules, "\"minimum_age_years\": 18",
                                  "\"minimum_age_years\": 18, \"selection_priority\": 0"),
                      catalog, "out-of-range selection priority accepted");
        ++count;

        const std::string prioritized =
            ReplaceOnce(rules, "\"minimum_age_years\": 18",
                        "\"minimum_age_years\": 18, \"selection_priority\": 10");
        const smv::MedicalPolicyCache priority_policy(prioritized, catalog);
        Check(priority_policy.valid(), "valid selection priority rejected");
        ++count;

        std::cout << "HOST_MEDICAL_POLICY_CACHE_TESTS_PASS=" << count << "\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}
