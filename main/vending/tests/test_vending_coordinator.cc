#include "inventory/inventory_store.h"
#include "medical/medical_advisor.h"
#include "vending/catalog_router.h"
#include "vending/vending_coordinator.h"

#include <cJSON.h>

#include <array>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

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

std::string Field(const std::string& json, const char* key) {
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(
        cJSON_ParseWithLength(json.data(), json.size()), &cJSON_Delete);
    if (!root)
        throw std::runtime_error("response is not JSON: " + json);
    const cJSON* field = cJSON_GetObjectItemCaseSensitive(root.get(), key);
    return cJSON_IsString(field) ? field->valuestring : "";
}

std::string Snapshot(std::string_view session = "host-test", uint32_t turn = 1,
                     std::string_view extra = {}) {
    std::ostringstream json;
    json << "{\"session_id\":\"" << session << "\",\"turn_id\":" << turn
         << ",\"age_years\":30,\"weight_kg\":65,"
            "\"pregnancy_or_breastfeeding\":false,\"symptoms\":[\"mild_headache\"],"
            "\"duration_hours\":3,\"danger_signs\":[],\"conditions\":[],"
            "\"current_medicines\":[],\"drug_allergies\":[],\"screening_answers\":{"
            "\"redflag_breathing\":false,\"redflag_neurologic\":false,"
            "\"redflag_weakness\":false,\"redflag_bleeding\":false,"
            "\"redflag_black_stool\":false,\"redflag_other\":false,"
            "\"headache_sudden\":false,\"headache_vomit\":false,"
            "\"headache_stiff\":false,\"headache_mild\":true}"
         << extra << "}";
    return json.str();
}

class MemoryBackend : public smv::InventoryBackend {
public:
    bool ReadBlob(std::string_view key, std::vector<uint8_t>& value) override {
        const auto found = blobs.find(std::string(key));
        if (found == blobs.end())
            return false;
        value = found->second;
        return true;
    }
    bool WriteBlob(std::string_view key, std::span<const uint8_t> value) override {
        blobs[std::string(key)] = std::vector<uint8_t>(value.begin(), value.end());
        return true;
    }
    std::map<std::string, std::vector<uint8_t>> blobs;
};

class FakeRelay : public smv::RelayActuator {
public:
    bool IsIdle() const override { return idle; }
    bool Start(const smv::ReservationToken& token, Completion completion) override {
        ++start_calls;
        last_token = token;
        if (!accept_start)
            return false;
        idle = false;
        completion_ = std::move(completion);
        return true;
    }
    void Cancel() override {
        ++cancel_calls;
        idle = true;
        if (completion_) {
            auto completion = std::move(completion_);
            completion(last_token.transaction_id, smv::RelayOutcome::kNotStartedCertain);
        }
    }
    void Finish(smv::RelayOutcome outcome) {
        idle = true;
        if (!completion_)
            throw std::runtime_error("relay completion missing");
        auto completion = std::move(completion_);
        completion(last_token.transaction_id, outcome);
    }

    bool idle = true;
    bool accept_start = true;
    int start_calls = 0;
    int cancel_calls = 0;
    smv::ReservationToken last_token;
    Completion completion_;
};

struct Fixture {
    Fixture(const char* rules_path, const char* catalog_path, const char* review_path,
            bool production = true, bool review_valid = true, bool provision = true,
            bool correct_catalog = true)
        : rules(Read(rules_path)),
          catalog(correct_catalog
                      ? Read(catalog_path)
                      : ReplaceOnce(Read(catalog_path), "\"strength\": \"ví dụ 200 mg + 200 mg\"",
                                    "\"strength\": \"không khớp kênh 7\"")),
          review_json(Read(review_path)),
          advisor(rules.c_str(), catalog.c_str(), review_json.c_str()),
          router(catalog, rules),
          inventory(backend) {
        if (provision) {
            std::array<uint32_t, smv::kVendingChannelCount> counts{};
            counts.fill(2);
            Check(inventory.Provision(counts) == smv::InventoryResult::kOk,
                  "fixture inventory provisioning failed");
        }
        smv::ArtifactIdentity identity{"rules-v1", "catalog-v1", std::string(64, 'a'),
                                       std::string(64, 'b')};
        smv::ReviewResult review{review_valid, review_valid ? smv::ReviewFailure::kNone
                                                            : smv::ReviewFailure::kNotApproved};
        coordinator = std::make_unique<smv::VendingCoordinator>(
            advisor, router, inventory, relay, std::move(identity), review, production,
            [this]() { return next_transaction_id++; },
            [this](const smv::AuditEvent& event) { audit.push_back(event); });
    }

    std::string rules;
    std::string catalog;
    std::string review_json;
    smv::MedicalAdvisor advisor;
    smv::CatalogRouter router;
    MemoryBackend backend;
    smv::InventoryStore inventory;
    FakeRelay relay;
    uint64_t next_transaction_id = 100;
    std::vector<smv::AuditEvent> audit;
    std::unique_ptr<smv::VendingCoordinator> coordinator;
};
}  // namespace

int main(int argc, char** argv) {
    if (argc != 4)
        return 2;
    try {
        int count = 0;
        {
            Fixture fixture(argv[1], argv[2], argv[3], false);
            Check(Field(fixture.coordinator->EvaluateAndStage(Snapshot(), 1000), "reason") ==
                          "PRODUCTION_DISABLED" &&
                      fixture.relay.start_calls == 0,
                  "compile gate off did not block");
            ++count;
        }
        {
            Fixture fixture(argv[1], argv[2], argv[3], true, false);
            Check(Field(fixture.coordinator->EvaluateAndStage(Snapshot(), 1000), "reason") ==
                      "PHARMACIST_REVIEW_INVALID",
                  "invalid pharmacist review did not block");
            ++count;
        }
        {
            Fixture fixture(argv[1], argv[2], argv[3]);
            Check(Field(fixture.coordinator->EvaluateAndStage(
                            "{\"session_id\":\"s\",\"turn_id\":1}", 1000),
                        "status") == "ASK",
                  "medical non-offer was not preserved");
            ++count;
        }
        {
            Fixture fixture(argv[1], argv[2], argv[3], true, true, true, false);
            Check(Field(fixture.coordinator->EvaluateAndStage(Snapshot(), 1000), "reason") ==
                      "BACKUP_IDENTITY_MISMATCH",
                  "invalid catalog did not block");
            ++count;
        }
        {
            Fixture fixture(argv[1], argv[2], argv[3], true, true, false);
            Check(Field(fixture.coordinator->EvaluateAndStage(Snapshot(), 1000), "reason") ==
                      "INVENTORY_UNAVAILABLE",
                  "missing inventory did not block");
            ++count;
        }
        {
            Fixture fixture(argv[1], argv[2], argv[3]);
            const auto external =
                fixture.inventory.Reserve(999, 0, fixture.inventory.Snapshot().revision);
            Check(external.has_value() &&
                      Field(fixture.coordinator->EvaluateAndStage(Snapshot(), 1000), "reason") ==
                          "INVENTORY_UNAVAILABLE",
                  "pending inventory did not block");
            ++count;
        }
        {
            Fixture fixture(argv[1], argv[2], argv[3]);
            const auto response = fixture.coordinator->EvaluateAndStage(Snapshot(), 1000);
            Check(Field(response, "status") == "OFFER", "eligible local offer not staged");
            Check(response.find("SMV-PARA500") == std::string::npos,
                  "SKU leaked to cloud response");
            Check(response.find("\"channel\"") == std::string::npos,
                  "channel leaked to cloud response");
            Check(fixture.coordinator->AwaitingConfirmation(30999), "candidate expired too early");
            Check(!fixture.coordinator->AwaitingConfirmation(31000),
                  "candidate did not expire at 30 seconds");
            ++count;
        }
        {
            Fixture fixture(argv[1], argv[2], argv[3]);
            Check(Field(fixture.coordinator->EvaluateAndStage(Snapshot("turn", 2), 1000),
                        "status") == "OFFER",
                  "turn fixture was not staged");
            Check(Field(fixture.coordinator->EvaluateAndStage(Snapshot("turn", 2), 1001),
                        "reason") == "STALE_TURN",
                  "duplicate turn was accepted");
            Check(Field(fixture.coordinator->EvaluateAndStage(Snapshot("turn", 1), 1002),
                        "reason") == "STALE_TURN",
                  "decreasing turn was accepted");
            ++count;
        }
        {
            Fixture fixture(argv[1], argv[2], argv[3]);
            fixture.coordinator->EvaluateAndStage(Snapshot("old", 1), 1000);
            const uint64_t old_id = fixture.next_transaction_id;
            Check(Field(fixture.coordinator->EvaluateAndStage(Snapshot("new", 1), 1001),
                        "status") == "OFFER" &&
                      fixture.next_transaction_id == old_id + 1,
                  "changed session did not replace the old candidate");
            ++count;
        }
        {
            Fixture fixture(argv[1], argv[2], argv[3]);
            fixture.coordinator->EvaluateAndStage(Snapshot(), 40000);
            Check(fixture.coordinator->Confirm(40001, false) ==
                      smv::ConfirmResult::kApplicationStateBlocked,
                  "non-idle app state authorized relay");
            Check(fixture.coordinator->Confirm(40002, true) == smv::ConfirmResult::kStarted,
                  "valid physical confirmation did not start");
            Check(fixture.coordinator->Confirm(40003, true) == smv::ConfirmResult::kNoCandidate,
                  "confirmation was reusable");
            fixture.relay.Finish(smv::RelayOutcome::kCommandSentUnverified);
            Check(!fixture.inventory.Snapshot().transaction_pending &&
                      fixture.inventory.Snapshot().counts[0] == 1,
                  "successful outcome did not complete reservation");
            ++count;
        }
        {
            Fixture fixture(argv[1], argv[2], argv[3]);
            fixture.coordinator->EvaluateAndStage(Snapshot(), 1000);
            fixture.relay.idle = false;
            Check(fixture.coordinator->Confirm(1001, true) == smv::ConfirmResult::kBlocked &&
                      fixture.relay.start_calls == 0,
                  "busy relay accepted confirmation");
            ++count;
        }
        {
            Fixture fixture(argv[1], argv[2], argv[3]);
            fixture.coordinator->EvaluateAndStage(Snapshot(), 1000);
            fixture.coordinator->OnDisconnected(1001);
            Check(fixture.coordinator->Confirm(1002, true) == smv::ConfirmResult::kNoCandidate &&
                      fixture.relay.start_calls == 0,
                  "disconnect did not invalidate staged candidate");
            ++count;
        }
        {
            Fixture fixture(argv[1], argv[2], argv[3]);
            fixture.coordinator->EvaluateAndStage(Snapshot(), 1000);
            std::array<uint32_t, smv::kVendingChannelCount> counts{};
            counts.fill(2);
            Check(fixture.inventory.Provision(counts) == smv::InventoryResult::kOk,
                  "stale revision fixture update failed");
            Check(fixture.coordinator->Confirm(1001, true) == smv::ConfirmResult::kBlocked,
                  "stale inventory revision authorized relay");
            ++count;
        }
        {
            Fixture fixture(argv[1], argv[2], argv[3]);
            fixture.coordinator->EvaluateAndStage(Snapshot(), 1000);
            std::array<uint32_t, smv::kVendingChannelCount> counts{};
            counts.fill(2);
            counts[0] = 0;
            counts[13] = 0;
            Check(fixture.inventory.Provision(counts) == smv::InventoryResult::kOk,
                  "empty stock fixture update failed");
            Check(fixture.coordinator->Confirm(1001, true) == smv::ConfirmResult::kBlocked,
                  "empty stock authorized relay");
            ++count;
        }
        {
            Fixture fixture(argv[1], argv[2], argv[3]);
            fixture.coordinator->EvaluateAndStage(Snapshot(), 1000);
            Check(fixture.coordinator->Confirm(1001, true) == smv::ConfirmResult::kStarted,
                  "disconnect fixture did not start");
            fixture.coordinator->OnDisconnected(1002);
            Check(!fixture.inventory.Snapshot().transaction_pending &&
                      fixture.inventory.Snapshot().counts[0] == 2 &&
                      fixture.relay.cancel_calls == 1,
                  "disconnect before LOW did not safely restore reservation");
            ++count;
        }
        {
            Fixture fixture(argv[1], argv[2], argv[3]);
            fixture.coordinator->EvaluateAndStage(Snapshot(), 1000);
            Check(fixture.coordinator->Confirm(1001, true) == smv::ConfirmResult::kStarted,
                  "uncertain fixture did not start");
            fixture.relay.Finish(smv::RelayOutcome::kUncertain);
            Check(fixture.inventory.Snapshot().transaction_pending,
                  "uncertain outcome was auto-cleared");
            ++count;
        }
        {
            Fixture fixture(argv[1], argv[2], argv[3]);
            fixture.relay.accept_start = false;
            fixture.coordinator->EvaluateAndStage(Snapshot(), 1000);
            Check(
                fixture.coordinator->Confirm(1001, true) == smv::ConfirmResult::kRelayStartFailed &&
                    !fixture.inventory.Snapshot().transaction_pending &&
                    fixture.inventory.Snapshot().counts[0] == 2,
                "certain relay start failure did not restore stock");
            ++count;
        }
        {
            Fixture fixture(argv[1], argv[2], argv[3]);
            const auto response = fixture.coordinator->EvaluateAndStage(
                Snapshot("voice", 1, ",\"confirmed\":true"), 1000);
            Check(Field(response, "status") == "BLOCK" && fixture.relay.start_calls == 0,
                  "spoken/cloud confirmation was accepted");
            ++count;
        }
        {
            Fixture fixture(argv[1], argv[2], argv[3]);
            const std::string patient_payload = Snapshot("private-symptom-session", 1);
            fixture.coordinator->EvaluateAndStage(patient_payload, 1000);
            fixture.coordinator->Confirm(1001, true);
            fixture.relay.Finish(smv::RelayOutcome::kCommandSentUnverified);
            Check(!fixture.audit.empty(), "audit event was not emitted");
            const auto& event = fixture.audit.back();
            Check(event.decision_reason.find("headache") == std::string::npos &&
                      event.decision_reason.find("private") == std::string::npos,
                  "audit event retained patient input");
            ++count;
        }
        std::cout << "HOST_VENDING_COORDINATOR_TESTS_PASS=" << count << "\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}
