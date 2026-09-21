#include "medical_advisor.h"
#include <cJSON.h>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

std::string Read(const char* p) { std::ifstream in(p); if (!in) throw std::runtime_error(p); std::ostringstream b; b << in.rdbuf(); return b.str(); }
std::string Field(const std::string& s, const char* key) {
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> j(cJSON_ParseWithLength(s.data(), s.size()), &cJSON_Delete);
    if (!j) throw std::runtime_error("output not json: " + s);
    const cJSON* field = cJSON_GetObjectItemCaseSensitive(j.get(), key);
    return cJSON_IsString(field) ? field->valuestring : "";
}
std::string Snapshot(const std::string& extra) {
    return std::string("{\"session_id\":\"host-test\",\"turn_id\":1,\"age_years\":30,\"weight_kg\":65,\"pregnancy_or_breastfeeding\":false,\"symptoms\":[\"mild_headache\"],\"duration_hours\":3,\"danger_signs\":[],\"conditions\":[],\"current_medicines\":[],\"drug_allergies\":[]") + extra + "}";
}
void Expect(const smv::MedicalAdvisor& advisor, const std::string& json, const char* status, const char* reason = nullptr) {
    std::string result=advisor.Evaluate(json);
    if (Field(result, "status") != status || (reason && Field(result, "reason") != reason))
        throw std::runtime_error(std::string("expected ")+status+" / "+(reason?reason:"-")+" got "+result);
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> r(cJSON_ParseWithLength(result.data(),result.size()), &cJSON_Delete);
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r.get(), "vend_allowed")))
        throw std::runtime_error("unsafe vend_allowed=true");
}
int main(int argc, char** argv) {
    if (argc != 4) return 2;
    try {
        auto rules=Read(argv[1]), catalog=Read(argv[2]), review=Read(argv[3]);
        smv::MedicalAdvisor a(rules.c_str(), catalog.c_str(), review.c_str());
        int n=0;
        auto test=[&](std::string json, const char* status, const char* reason=nullptr) { Expect(a,json,status,reason); ++n; };
        test("{}", "DENY", "INVALID_SESSION_OR_TURN");
        test("not json", "DENY", "INVALID_INPUT_JSON");
        test("{\"session_id\":\"x\",\"turn_id\":1}trailing", "DENY", "INVALID_INPUT_JSON");
        test("{\"session_id\":\"x\",\"turn_id\":1}", "NEED_MORE_INFO");
        test(Snapshot(""), "PROVISIONAL_OPTIONS", "PHARMACIST_REVIEW_AND_STOCK_VERIFICATION_REQUIRED");
        test(Snapshot(",\"relay\":0"), "DENY", "FORBIDDEN_OR_UNKNOWN_FIELD");
        test(Snapshot(",\"sku\":\"SMV-PARA500\""), "DENY", "FORBIDDEN_OR_UNKNOWN_FIELD");
        test(Snapshot(",\"vend\":true"), "DENY", "FORBIDDEN_OR_UNKNOWN_FIELD");
        test(Snapshot(",\"quantity\":1"), "DENY", "FORBIDDEN_OR_UNKNOWN_FIELD");
        test(Snapshot(",\"conditions\":[\"liver_disease\"]"), "DENY", "DUPLICATE_FIELD"); // duplicated key must be rejected
        test(Snapshot(",\"danger_signs\":[\"chest_pain\"]"), "DENY", "DUPLICATE_FIELD");
        test("{\"session_id\":\"x\",\"turn_id\":1,\"age_years\":15}", "REFER", "UNDER_MINIMUM_AGE");
        test("{\"session_id\":\"x\",\"turn_id\":1,\"pregnancy_or_breastfeeding\":true}", "REFER", "PREGNANCY_OR_BREASTFEEDING_UNSUPPORTED");
        test(Snapshot(",\"foo\":1"), "DENY", "FORBIDDEN_OR_UNKNOWN_FIELD");
        test(std::string(4200,'x'), "DENY", "INPUT_TOO_LARGE");
        test("{\"session_id\":\"x\",\"turn_id\":0}", "DENY", "INVALID_SESSION_OR_TURN");
        test("{\"session_id\":\"x\",\"turn_id\":1,\"danger_signs\":[\"chest_pain\"]}", "REFER", "POSSIBLE_DANGER_SIGN");
        test("{\"session_id\":\"x\",\"turn_id\":1,\"age_years\":20,\"pregnancy_or_breastfeeding\":false,\"symptoms\":[\"mild_headache\"],\"duration_hours\":3,\"weight_kg\":65,\"danger_signs\":[],\"conditions\":[],\"current_medicines\":[],\"drug_allergies\":[\"penicillin\"]}","REFER", "UNREVIEWED_CONDITION_MEDICINE_OR_ALLERGY");
        test("{\"session_id\":\"x\",\"turn_id\":1,\"age_years\":20,\"pregnancy_or_breastfeeding\":false,\"symptoms\":[\"mild_headache\"],\"duration_hours\":3,\"weight_kg\":65,\"danger_signs\":[],\"conditions\":[\"unknown_condition\"],\"current_medicines\":[],\"drug_allergies\":[]}","REFER", "UNREVIEWED_CONDITION_MEDICINE_OR_ALLERGY");
        auto ReplaceOnce = [](std::string s, const std::string& from, const std::string& to) {
            auto pos = s.find(from);
            if (pos == std::string::npos) throw std::runtime_error("test setup failed");
            s.replace(pos, from.size(), to);
            return s;
        };
        test(ReplaceOnce(Snapshot(""), "\"weight_kg\":65", "\"weight_kg\":45"), "REFER", "RULE_EXCLUSION_OR_REFERRAL");
        test(ReplaceOnce(Snapshot(""), "\"conditions\":[]", "\"conditions\":[\"liver_disease\"]"), "REFER", "RULE_EXCLUSION_OR_REFERRAL");
        test(ReplaceOnce(Snapshot(""), "\"current_medicines\":[]", "\"current_medicines\":[\"contains_paracetamol\"]"), "REFER", "RULE_EXCLUSION_OR_REFERRAL");
        test(ReplaceOnce(Snapshot(""), "\"symptoms\":[\"mild_headache\"]", "\"symptoms\":[\"unsupported\"]"), "REFER", "UNSUPPORTED_SYMPTOM");
        test(ReplaceOnce(Snapshot(""), "\"danger_signs\":[]", "\"danger_signs\":[\"difficulty_breathing\"]"), "REFER", "POSSIBLE_DANGER_SIGN");
        test(ReplaceOnce(Snapshot(""), "\"drug_allergies\":[]", "\"drug_allergies\":[\"unknown_drug\"]"), "REFER", "UNREVIEWED_CONDITION_MEDICINE_OR_ALLERGY");
        test(ReplaceOnce(Snapshot(""), "\"duration_hours\":3", "\"duration_hours\":-1"), "DENY", "INVALID_WEIGHT_OR_DURATION");
        test(ReplaceOnce(Snapshot(""), "\"symptoms\":[\"mild_headache\"]", "\"symptoms\":[]"), "NEED_MORE_INFO", "SYMPTOMS_NOT_SPECIFIED");
        std::string unreviewed_catalog = catalog;
        unreviewed_catalog = ReplaceOnce(unreviewed_catalog, "\"initial_stock\": 5", "\"initial_stock\": 0");
        smv::MedicalAdvisor backup_only(rules.c_str(), unreviewed_catalog.c_str(), review.c_str());
        Expect(backup_only, Snapshot(""), "PROVISIONAL_OPTIONS"); ++n;
        const auto backup_idx = unreviewed_catalog.find("\"channel\": 13");
        if (backup_idx == std::string::npos) throw std::runtime_error("backup test setup failed");
        auto stock_idx = unreviewed_catalog.find("\"initial_stock\": 5", backup_idx);
        if (stock_idx == std::string::npos) throw std::runtime_error("backup stock test setup failed");
        unreviewed_catalog.replace(stock_idx, std::string("\"initial_stock\": 5").size(), "\"initial_stock\": 0");
        smv::MedicalAdvisor no_catalog_stock(rules.c_str(), unreviewed_catalog.c_str(), review.c_str());
        Expect(no_catalog_stock, Snapshot(""), "REFER", "NO_REVIEWABLE_OPTION_IN_CATALOG"); ++n;
        if (Field(a.IntakeSchema(), "purpose") != "INTAKE_ONLY_NO_DIAGNOSIS_NO_VEND") throw std::runtime_error("schema invalid");
        ++n;
        std::cout << "HOST_MEDICAL_ADVISOR_TESTS_PASS=" << n << "\n";
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << "\n"; return 1; }
}
