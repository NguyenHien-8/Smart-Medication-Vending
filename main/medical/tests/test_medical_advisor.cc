#include "medical_advisor.h"
#include <cJSON.h>
#include <fstream>
#include <iostream>
#include <memory>
#include <cstring>
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
    return std::string("{\"session_id\":\"host-test\",\"turn_id\":1,\"age_years\":30,\"weight_kg\":65,\"pregnancy_or_breastfeeding\":false,\"symptoms\":[\"mild_headache\"],\"duration_hours\":3,\"danger_signs\":[],\"conditions\":[],\"current_medicines\":[],\"drug_allergies\":[],\"screening_answers\":{\"redflag_breathing\":false,\"redflag_neurologic\":false,\"redflag_weakness\":false,\"redflag_bleeding\":false,\"redflag_black_stool\":false,\"redflag_other\":false,\"headache_sudden\":false,\"headache_vomit\":false,\"headache_stiff\":false,\"headache_mild\":true}") + extra + "}";
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
        // A full set of old fields alone can no longer yield an option: the device
        // requires explicitly answered short global and symptom-specific questions.
        test("{\"session_id\":\"q\",\"turn_id\":1,\"age_years\":30,\"weight_kg\":65,\"pregnancy_or_breastfeeding\":false,\"symptoms\":[\"mild_headache\"],\"duration_hours\":2,\"danger_signs\":[],\"conditions\":[],\"current_medicines\":[],\"drug_allergies\":[]}", "NEED_MORE_INFO", "UNANSWERED_SAFETY_QUESTION");
        test("{\"session_id\":\"q\",\"turn_id\":1,\"symptoms\":[\"mild_headache\"],\"screening_answers\":{\"redflag_breathing\":true}}", "REFER", "INTERVIEW_DANGER_OR_CONTRAINDICATION");
        test("{\"session_id\":\"q\",\"turn_id\":1,\"screening_answers\":{\"fake\":false}}", "DENY", "INVALID_SCREENING_ANSWERS");
        test("{\"session_id\":\"q\",\"turn_id\":1,\"screening_answers\":{\"headache_sudden\":\"no\"}}", "DENY", "INVALID_SCREENING_ANSWERS");
        test(Snapshot(""), "PROVISIONAL_OPTIONS", "PHARMACIST_REVIEW_AND_STOCK_VERIFICATION_REQUIRED");
        test(Snapshot(",\"screening_answers\":{}"), "DENY", "DUPLICATE_FIELD");
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
        test("{\"session_id\":\"x\",\"turn_id\":1,\"age_years\":20,\"pregnancy_or_breastfeeding\":false,\"symptoms\":[\"mild_headache\"],\"duration_hours\":3,\"weight_kg\":65,\"danger_signs\":[],\"conditions\":[],\"current_medicines\":[],\"drug_allergies\":[\"penicillin\"],\"screening_answers\":{\"redflag_breathing\":false,\"redflag_neurologic\":false,\"redflag_weakness\":false,\"redflag_bleeding\":false,\"redflag_black_stool\":false,\"redflag_other\":false,\"headache_sudden\":false,\"headache_vomit\":false,\"headache_stiff\":false,\"headache_mild\":true}}","REFER", "UNREVIEWED_CONDITION_MEDICINE_OR_ALLERGY");
        test("{\"session_id\":\"x\",\"turn_id\":1,\"age_years\":20,\"pregnancy_or_breastfeeding\":false,\"symptoms\":[\"mild_headache\"],\"duration_hours\":3,\"weight_kg\":65,\"danger_signs\":[],\"conditions\":[\"unknown_condition\"],\"current_medicines\":[],\"drug_allergies\":[],\"screening_answers\":{\"redflag_breathing\":false,\"redflag_neurologic\":false,\"redflag_weakness\":false,\"redflag_bleeding\":false,\"redflag_black_stool\":false,\"redflag_other\":false,\"headache_sudden\":false,\"headache_vomit\":false,\"headache_stiff\":false,\"headache_mild\":true}}","REFER", "UNREVIEWED_CONDITION_MEDICINE_OR_ALLERGY");
        // A generic "headache" transcript never proves MILD severity. Missing
        // severity must force another brief question before any option appears.
        {
            std::string incomplete = Snapshot("");
            const std::string marker = ",\"headache_mild\":true";
            const auto pos = incomplete.find(marker);
            if (pos == std::string::npos) throw std::runtime_error("severity test fixture absent");
            incomplete.erase(pos, marker.size());
            const auto reply = a.Evaluate(incomplete);
            if (Field(reply, "status") != "NEED_MORE_INFO" ||
                Field(reply, "next_question_id") != "headache_mild")
                throw std::runtime_error("missing headache severity was not re-asked");
            ++n;
        }
        auto ReplaceOnce = [](std::string s, const std::string& from, const std::string& to) {
            auto pos = s.find(from);
            if (pos == std::string::npos) throw std::runtime_error("test setup failed");
            s.replace(pos, from.size(), to);
            return s;
        };
        // Synthetic expected=true verifies the global mismatch policy (test-only).
        {
            std::string changed_rules = rules;
            const auto key = changed_rules.find("\"id\": \"redflag_breathing\"");
            if (key == std::string::npos) throw std::runtime_error("missing first global check");
            const auto expected = changed_rules.find("\"expected\": false", key);
            if (expected == std::string::npos) throw std::runtime_error("missing expected=false");
            changed_rules.replace(expected, std::strlen("\"expected\": false"), "\"expected\": true");
            smv::MedicalAdvisor changed(changed_rules.c_str(), catalog.c_str(), review.c_str());
            Expect(changed, Snapshot(""), "REFER", "GLOBAL_SAFETY_CHECK_MISMATCH"); ++n;
        }
        test(ReplaceOnce(Snapshot(""), "\"headache_sudden\":false", "\"headache_sudden\":true"), "REFER", "INTERVIEW_DANGER_OR_CONTRAINDICATION");
        test(ReplaceOnce(Snapshot(""), "\"headache_vomit\":false", "\"headache_vomit\":true"), "REFER", "INTERVIEW_DANGER_OR_CONTRAINDICATION");
        test(ReplaceOnce(Snapshot(""), "\"headache_vomit\":false", "\"headache_vomit\":null"), "DENY", "INVALID_SCREENING_ANSWERS");
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
        const std::string schema = a.IntakeSchema();
        if (Field(schema, "purpose") != "INTAKE_ONLY_NO_DIAGNOSIS_NO_VEND" ||
            schema.size() > 3200 || schema.find("\"interview\"") != std::string::npos ||
            schema.find("\"symptom_guides\"") != std::string::npos)
            throw std::runtime_error("intake schema is too large or leaks full rules");
        std::cout << "HOST_INTAKE_SCHEMA_BYTES=" << schema.size() << "\n";
        ++n;
        auto guide = a.SymptomGuide("mild_headache");
        if (Field(guide, "symptom_enum") != "mild_headache" || guide.size() > 2400 ||
            guide.find("headache_sudden") == std::string::npos)
            throw std::runtime_error("on-demand symptom guide failed");
        std::cout << "HOST_HEADACHE_GUIDE_BYTES=" << guide.size() << "\n";
        ++n;
        if (Field(a.SymptomGuide("bad_input"), "status") != "DENY")
            throw std::runtime_error("unrecognized symptom guide accepted");
        ++n;
        // Exercise every symptom profile, not only the headache happy path. Each
        // is guarded by exactly one next_question; skipped answers never pass.
        std::unique_ptr<cJSON, decltype(&cJSON_Delete)> cfg(cJSON_ParseWithLength(rules.data(), rules.size()), &cJSON_Delete);
        const cJSON* interview = cJSON_GetObjectItemCaseSensitive(cfg.get(), "interview");
        const cJSON* profiles = cJSON_GetObjectItemCaseSensitive(interview, "symptom_checks");
        const cJSON* globals = cJSON_GetObjectItemCaseSensitive(interview, "global_checks");
        int profile_count=0;
        const cJSON* profile = nullptr;
        cJSON_ArrayForEach(profile, profiles) {
            if (!profile->string) throw std::runtime_error("profile has no enum");
            const std::string per_symptom_guide = a.SymptomGuide(profile->string);
            if (Field(per_symptom_guide, "symptom_enum") != profile->string ||
                per_symptom_guide.size() > 2400 ||
                Field(per_symptom_guide, "status") == "DENY")
                throw std::runtime_error(std::string("guide failed for: ") + profile->string);
            ++n;
            const std::string baseline = Snapshot("");
            auto doc = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>(cJSON_ParseWithLength(baseline.c_str(), baseline.size()), &cJSON_Delete);
            cJSON_DeleteItemFromObjectCaseSensitive(doc.get(), "symptoms");
            cJSON* symptoms = cJSON_AddArrayToObject(doc.get(), "symptoms");
            cJSON_AddItemToArray(symptoms, cJSON_CreateString(profile->string));
            cJSON_DeleteItemFromObjectCaseSensitive(doc.get(), "screening_answers");
            cJSON* answers = cJSON_CreateObject();
            cJSON_AddItemToObject(doc.get(), "screening_answers", answers);
            const cJSON* check = nullptr;
            cJSON_ArrayForEach(check, globals) {
                cJSON_AddBoolToObject(answers, cJSON_GetObjectItemCaseSensitive(check, "id")->valuestring, 0);
            }
            // Before symptom checks are answered, local adviser must ask again.
            std::unique_ptr<char, decltype(&cJSON_free)> partial(cJSON_PrintUnformatted(doc.get()), &cJSON_free);
            std::string first=a.Evaluate(partial.get());
            if (Field(first,"status") != "NEED_MORE_INFO" || Field(first,"reason") != "UNANSWERED_SYMPTOM_QUESTION")
                throw std::runtime_error(std::string("missing symptom screening accepted: ")+profile->string+" "+first);
            if (Field(first,"next_question_id") != Field(a.Evaluate(partial.get()),"next_question_id"))
                throw std::runtime_error("next question not stable on unanswered input");
            ++n;
            cJSON_ArrayForEach(check, profile) {
                const cJSON* id = cJSON_GetObjectItemCaseSensitive(check,"id");
                const cJSON* expected = cJSON_GetObjectItemCaseSensitive(check,"expected");
                const cJSON* question = cJSON_GetObjectItemCaseSensitive(check,"question_vi");
                if (!cJSON_IsString(id) || !cJSON_IsBool(expected) || !cJSON_IsString(question) ||
                    std::strlen(question->valuestring)>120 || !std::strchr(question->valuestring, '?'))
                    throw std::runtime_error("invalid/long question in symptom profile");
                cJSON_AddBoolToObject(answers,id->valuestring,cJSON_IsTrue(expected));
            }
            std::unique_ptr<char, decltype(&cJSON_free)> complete(cJSON_PrintUnformatted(doc.get()), &cJSON_free);
            if (std::strcmp(profile->string, "acute_watery_diarrhoea") == 0) {
                // The catalog has TWO eligible medicines for this same symptom;
                // do not choose the first rule just because it appears first.
                Expect(a, complete.get(), "REFER", "MULTIPLE_OPTIONS_REQUIRE_HUMAN_REVIEW");
            } else {
                Expect(a, complete.get(), "PROVISIONAL_OPTIONS", "PHARMACIST_REVIEW_AND_STOCK_VERIFICATION_REQUIRED");
            }
            ++n; ++profile_count;
        }
        if (profile_count!=15) throw std::runtime_error("missing symptom interview profiles");
        ++n;
        std::cout << "HOST_MEDICAL_ADVISOR_TESTS_PASS=" << n << "\n";
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << "\n"; return 1; }
}
