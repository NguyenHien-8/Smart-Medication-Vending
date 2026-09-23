#include "catalog_router.h"

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

std::string Slot(int channel, const char* sku, const char* canonical, const char* name,
                 const char* ingredient, const char* strength, const char* backup) {
    std::ostringstream json;
    json << "{\"channel\":" << channel << ",\"sku\":\"" << sku << "\",\"canonical_id\":\""
         << canonical << "\",\"name\":\"" << name << "\",\"active_ingredient\":\"" << ingredient
         << "\",\"strength\":\"" << strength
         << "\",\"symptom_group\":\"test symptom\",\"initial_stock\":1,"
            "\"backup_of_channel\":"
         << backup
         << ",\"differentiate_and_escalate\":\"test escalation\","
            "\"information_source\":\"https://example.test/medicine\"}";
    return json.str();
}

std::string Catalog(const std::string& slots) {
    return "{\"schema_version\":1,\"catalog_version\":\"catalog-test\","
           "\"dispense_unit\":\"sealed_blister\",\"tablets_per_blister\":null,"
           "\"initial_stock_unit\":\"blister\",\"slots\":[" +
           slots + "]}";
}

std::string Rules(const std::string& canonical_ids) {
    std::istringstream input(canonical_ids);
    std::ostringstream rules;
    rules << R"({"schema_version":1,"rules_version":"rules-test",
"status":"PHARMACIST_REVIEW_REQUIRED",
"scope":{"minimum_age_years":16,"pregnancy_or_breastfeeding_supported":false,
"missing_data_policy":"NEED_MORE_INFO_THEN_NO_VEND",
"maximum_medicines_per_transaction":3,"maximum_blisters_per_medicine":1,
"diagnosis_claims_allowed":false},
"global_required_fields":["age_years","weight_kg","pregnancy_or_breastfeeding",
"symptoms","duration_hours","danger_signs","conditions","current_medicines",
"drug_allergies"],
"global_danger_signs":["difficulty_breathing"],
"interview":{"version":1,"protocol":"Ask questions",
"global_checks":[{"id":"danger_check","question_vi":"Question?",
"expected":false,"on_mismatch":"REFER"}],
"symptom_checks":{"test_symptom":[{"id":"test_check","question_vi":"Question?",
"expected":true,"on_mismatch":"CLARIFY"}]}},
"medicine_rules":[)";
    std::string canonical_id;
    bool first = true;
    while (std::getline(input, canonical_id, ',')) {
        if (!first)
            rules << ',';
        rules << "{\"canonical_id\":\"" << canonical_id
              << "\",\"symptoms\":[\"test_symptom\"],"
                 "\"exclude_if\":[\"test_exclusion\"]}";
        first = false;
    }
    rules << "]}";
    return rules.str();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3)
        return 2;
    try {
        const auto primary =
            Slot(0, "SKU-A", "MED_A", "Medicine A", "Ingredient A", "10 mg", "null");
        const auto backup = Slot(1, "SKU-A-B", "MED_A", "Medicine A", "Ingredient A", "10 mg", "0");
        const auto rules = Rules("MED_A");
        const smv::CatalogRouter router(Catalog(primary + "," + backup), rules);
        Check(router.valid(), "valid primary/backup catalog was rejected");
        int count = 1;

        smv::StockSnapshot stock;
        stock.available = true;
        stock.revision = 7;
        stock.valid_mask = 0xffff;
        stock.counts[0] = 2;
        stock.counts[1] = 3;
        auto route = router.Select("MED_A", stock);
        Check(route.status == smv::RouteStatus::kReady && route.channel == 0 && !route.backup &&
                  route.sku == "SKU-A" && route.inventory_revision == 7,
              "primary route not selected");
        ++count;

        stock.counts[0] = 0;
        route = router.Select("MED_A", stock);
        Check(route.status == smv::RouteStatus::kReady && route.channel == 1 && route.backup &&
                  route.sku == "SKU-A-B",
              "backup route not selected after primary empty");
        ++count;
        stock.counts[1] = 0;
        Check(router.Select("MED_A", stock).status == smv::RouteStatus::kOutOfStock,
              "empty routes were accepted");
        ++count;
        stock.available = false;
        Check(router.Select("MED_A", stock).status == smv::RouteStatus::kInventoryUnavailable,
              "unknown inventory was accepted");
        ++count;
        stock.available = true;
        stock.transaction_pending = true;
        Check(router.Select("MED_A", stock).status == smv::RouteStatus::kInventoryUnavailable,
              "pending transaction inventory was accepted");
        ++count;
        stock.transaction_pending = false;
        Check(router.Select("UNKNOWN", stock).status == smv::RouteStatus::kUnknownItem,
              "unknown canonical item was routed");
        ++count;

        const smv::CatalogRouter duplicate_channel(
            Catalog(primary + "," + ReplaceOnce(backup, "\"channel\":1", "\"channel\":0")), rules);
        Check(!duplicate_channel.valid() &&
                  duplicate_channel.validation_reason() == "DUPLICATE_CHANNEL",
              "duplicate channel accepted");
        ++count;
        const smv::CatalogRouter duplicate_primary(
            Catalog(primary + "," +
                    ReplaceOnce(backup, "\"backup_of_channel\":0", "\"backup_of_channel\":null")),
            rules);
        Check(!duplicate_primary.valid() &&
                  duplicate_primary.validation_reason() == "DUPLICATE_PRIMARY",
              "duplicate primary accepted");
        ++count;
        const auto second_backup =
            Slot(2, "SKU-A-C", "MED_A", "Medicine A", "Ingredient A", "10 mg", "0");
        const smv::CatalogRouter duplicate_backup(
            Catalog(primary + "," + backup + "," + second_backup), rules);
        Check(
            !duplicate_backup.valid() && duplicate_backup.validation_reason() == "DUPLICATE_BACKUP",
            "multiple backups accepted");
        ++count;
        const smv::CatalogRouter wrong_reference(
            Catalog(primary + "," +
                    ReplaceOnce(backup, "\"backup_of_channel\":0", "\"backup_of_channel\":2")),
            rules);
        Check(!wrong_reference.valid() &&
                  wrong_reference.validation_reason() == "BACKUP_REFERENCE_MISMATCH",
              "wrong backup reference accepted");
        ++count;

        for (const auto& mismatch : {
                 ReplaceOnce(backup, "Medicine A", "Medicine B"),
                 ReplaceOnce(backup, "Ingredient A", "Ingredient B"),
                 ReplaceOnce(backup, "10 mg", "20 mg"),
             }) {
            const smv::CatalogRouter invalid(Catalog(primary + "," + mismatch), rules);
            Check(!invalid.valid() && invalid.validation_reason() == "BACKUP_IDENTITY_MISMATCH",
                  "backup identity mismatch accepted");
            ++count;
        }

        const smv::CatalogRouter repository_catalog(Read(argv[1]), Read(argv[2]));
        Check(!repository_catalog.valid() &&
                  repository_catalog.validation_reason() == "BACKUP_IDENTITY_MISMATCH",
              "repository antacid mismatch was not locked");
        ++count;

        // The source rules are structurally sound independently of the
        // deliberately mismatched backup strength in the repository catalog.
        const std::string reviewed_fixture_catalog =
            ReplaceOnce(Read(argv[1]), "cùng SKU channel 7", "ví dụ 200 mg + 200 mg");
        const std::string repository_rules = Read(argv[2]);
        const smv::CatalogRouter validated_repo(reviewed_fixture_catalog, repository_rules);
        Check(validated_repo.valid(), "repository rules rejected after in-memory identity fix");
        ++count;

        const auto reject_rules = [&](const std::string& changed, const char* message) {
            const smv::CatalogRouter invalid(Catalog(primary), changed);
            Check(!invalid.valid() && invalid.validation_reason() == "INVALID_RULES_SCHEMA",
                  message);
            ++count;
        };
        reject_rules(
            ReplaceOnce(rules, "\"exclude_if\":[\"test_exclusion\"]", "\"exclude_if\":null"),
            "null contraindications accepted");
        reject_rules(ReplaceOnce(rules, "\"exclude_if\":[\"test_exclusion\"]", "\"exclude_if\":[]"),
                     "empty contraindications accepted");
        reject_rules(
            ReplaceOnce(rules, "\"exclude_if\":[\"test_exclusion\"]", "\"exclude_if\":[123]"),
            "non-string contraindication accepted");
        reject_rules(ReplaceOnce(rules, "\"exclude_if\":[\"test_exclusion\"]",
                                 "\"exclude_if\":[\"test_exclusion\",\"test_exclusion\"]"),
                     "duplicate contraindication accepted");
        reject_rules(ReplaceOnce(rules, "\"exclude_if\":[\"test_exclusion\"]",
                                 "\"exclude_if\":[\"unsafe flag\"]"),
                     "malformed contraindication identifier accepted");
        reject_rules(ReplaceOnce(rules, "\"symptoms\":[\"test_symptom\"]", "\"symptoms\":[]"),
                     "missing symptoms accepted");
        reject_rules(
            ReplaceOnce(rules, "\"exclude_if\":[\"test_exclusion\"]", "\"not_a_check\":[]"),
            "unknown rules field accepted");
        reject_rules(ReplaceOnce(rules, "\"expected\":true", "\"expected\":\"yes\""),
                     "malformed interview check accepted");
        reject_rules(ReplaceOnce(rules, "\"global_danger_signs\":[\"difficulty_breathing\"]",
                                 "\"global_danger_signs\":[]"),
                     "empty global danger signs accepted");
        reject_rules(ReplaceOnce(rules, "\"missing_data_policy\":\"NEED_MORE_INFO_THEN_NO_VEND\"",
                                 "\"missing_data_policy\":\"ASSUME_SAFE\""),
                     "unsafe missing-data policy accepted");
        reject_rules(ReplaceOnce(rules, "\"pregnancy_or_breastfeeding_supported\":false",
                                 "\"pregnancy_or_breastfeeding_supported\":true"),
                     "unsupported pregnancy setting accepted");
        reject_rules(
            ReplaceOnce(rules, "\"exclude_if\":[\"test_exclusion\"]",
                        "\"refer_if\":[\"test_exclusion\"],\"exclude_if\":[\"test_exclusion\"]"),
            "conflicting refer/exclude predicates accepted");
        const smv::CatalogRouter no_symptom_interview(
            Catalog(primary), ReplaceOnce(rules, "\"symptoms\":[\"test_symptom\"]",
                                          "\"symptoms\":[\"uninterviewed_symptom\"]"));
        Check(!no_symptom_interview.valid() &&
                  no_symptom_interview.validation_reason() == "MISSING_SYMPTOM_INTERVIEW",
              "uninterviewed symptom accepted");
        ++count;

        const std::string bounded_catalog = Catalog(primary) + "trailing-untrusted-bytes";
        const std::string bounded_rules = rules + "trailing-untrusted-bytes";
        const smv::CatalogRouter bounded_view(
            std::string_view(bounded_catalog.data(), Catalog(primary).size()),
            std::string_view(bounded_rules.data(), rules.size()));
        Check(bounded_view.valid(), "bounded catalog string_view read past its declared length");
        ++count;

        const smv::CatalogRouter unsupported_unit(
            ReplaceOnce(Catalog(primary), "sealed_blister", "tablet"), rules);
        Check(!unsupported_unit.valid() &&
                  unsupported_unit.validation_reason() == "UNSUPPORTED_DISPENSE_UNIT",
              "unsupported dispense unit was accepted");
        ++count;

        const smv::CatalogRouter missing_rule(Catalog(primary), Rules("MED_UNKNOWN"));
        Check(!missing_rule.valid() && missing_rule.validation_reason() == "RULE_REFERENCE_MISSING",
              "catalog/rules reference mismatch was accepted");
        ++count;

        const smv::CatalogRouter disabled_slot(
            Catalog(ReplaceOnce(primary, "\"backup_of_channel\":null",
                                "\"backup_of_channel\":null,\"enabled\":false")),
            rules);
        Check(!disabled_slot.valid() && disabled_slot.validation_reason() == "INVALID_SLOT_SCHEMA",
              "disabled catalog slot was accepted");
        ++count;
        std::cout << "HOST_CATALOG_ROUTER_TESTS_PASS=" << count << "\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}
