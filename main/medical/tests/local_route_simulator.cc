#include "local_route_simulator.h"
#include <cJSON.h>
#include <cstring>
#include <memory>
#include <set>

namespace smv::bench {
namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
Json Parse(const std::string& s) {
    // Reject trailing bytes and malformed JSON; the inputs are not trusted.
    return Json(cJSON_ParseWithLengthOpts(s.c_str(), s.size() + 1, nullptr, true), &cJSON_Delete);
}
const cJSON* Get(const cJSON* v, const char* key) { return cJSON_GetObjectItemCaseSensitive(v, key); }
const char* Str(const cJSON* v) { return cJSON_IsString(v) ? v->valuestring : nullptr; }
Route Block(const char* why) { Route r; r.reason = why; return r; }
bool Equal(const char* a, const char* b) { return a && b && std::strcmp(a,b)==0; }
bool Channel(const cJSON* slot, int& out) {
    const cJSON* v = Get(slot, "channel");
    if (!cJSON_IsNumber(v) || v->valuedouble < 0 || v->valuedouble > 15 ||
        v->valuedouble != static_cast<double>(v->valueint)) return false;
    out = v->valueint;
    return true;
}
}
Route PreviewRoute(const std::string& advisor_output, const std::string& catalog_json,
                   const std::string& rules_json, const std::string& review_json,
                   const Inventory& inventory) {
    const auto response=Parse(advisor_output), catalog=Parse(catalog_json);
    const auto rules=Parse(rules_json), review=Parse(review_json);
    if (!response || !catalog || !rules || !review) return Block("INVALID_LOCAL_DATA");
    if (!Equal(Str(Get(response.get(), "status")), "PROVISIONAL_OPTIONS") ||
        !cJSON_IsBool(Get(response.get(), "vend_allowed")) ||
        cJSON_IsTrue(Get(response.get(), "vend_allowed")))
        return Block("ADVISOR_NOT_PROVISIONAL");
    // The default pilot data has approved=false; it ALWAYS blocks routing.
    if (!cJSON_IsTrue(Get(review.get(), "approved")) ||
        !Equal(Str(Get(review.get(), "reviewed_catalog_version")), Str(Get(catalog.get(), "catalog_version"))) ||
        !Equal(Str(Get(review.get(), "reviewed_rules_version")), Str(Get(rules.get(), "rules_version"))))
        return Block("PHARMACIST_REVIEW_OR_VERSION_MISMATCH");
    if (!inventory.externally_verified) return Block("INVENTORY_NOT_VERIFIED");
    const cJSON* options=Get(response.get(), "provisional_options");
    if (!cJSON_IsArray(options) || cJSON_GetArraySize(options)!=1)
        return Block("AMBIGUOUS_OR_NO_OPTION");
    const char* id=Str(Get(cJSON_GetArrayItem(options,0), "canonical_id"));
    if (!id || !*id || std::strlen(id)>64) return Block("INVALID_OPTION_ID");
    // The candidate must originate from local evaluation, NOT cloud-provided SKU/channel.
    // This routine is a BENCH TOOL only; it must never trust an MCP argument as advisor_output.
    const cJSON* slots=Get(catalog.get(), "slots");
    if (!cJSON_IsArray(slots) || cJSON_GetArraySize(slots)>16 || cJSON_GetArraySize(slots)==0)
        return Block("INVALID_CATALOG");
    const cJSON *primary=nullptr, *backup=nullptr, *s=nullptr;
    std::set<int> seen_channels;
    int main_channel=-1, backup_channel=-1;
    cJSON_ArrayForEach(s, slots) {
        int ch=-1;
        const char *sid=Str(Get(s,"canonical_id")), *sku=Str(Get(s,"sku"));
        const cJSON* ref=Get(s,"backup_of_channel");
        if (!Channel(s,ch) || !seen_channels.insert(ch).second || !sid || !sku || !*sku ||
            (!cJSON_IsNumber(ref) && !cJSON_IsNull(ref)))
            return Block("INVALID_OR_DUPLICATED_CHANNEL_MAP");
        if (!Equal(sid,id)) continue;
        if (cJSON_IsNumber(ref)) {
            if (backup) return Block("AMBIGUOUS_BACKUP_MAP");
            backup=s; backup_channel=ch;
        } else {
            if (primary) return Block("AMBIGUOUS_PRIMARY_MAP");
            primary=s; main_channel=ch;
        }
    }
    if (!primary) return Block("NO_PRIMARY_CATALOG_SLOT");
    if (backup) {
        const auto* ref=Get(backup,"backup_of_channel");
        if (!cJSON_IsNumber(ref) || ref->valuedouble != main_channel || backup_channel == main_channel ||
            !Equal(Str(Get(backup,"name")), Str(Get(primary,"name"))) ||
            !Equal(Str(Get(backup,"strength")), Str(Get(primary,"strength"))) ||
            !Equal(Str(Get(backup,"active_ingredient")),Str(Get(primary,"active_ingredient"))))
            return Block("BACKUP_SKU_MISMATCH");
    }
    const cJSON* selected = inventory.measured[main_channel] ? primary :
                             (backup && inventory.measured[backup_channel] ? backup : nullptr);
    if (!selected) return Block("VERIFIED_STOCK_EMPTY");
    Route route;
    route.status="SIMULATION_READY"; // Deliberately NOT VEND_ALLOWED.
    route.reason="BENCH_ONLY_NO_GPIO";
    route.sku=Str(Get(selected,"sku"));
    route.canonical_id=id;
    route.channel=inventory.measured[main_channel] ? main_channel : backup_channel;
    return route;
}
bool RelayPulseSimulation::Start(const Route& route, uint32_t now_ms) {
    if (active_ || route.status!="SIMULATION_READY" || route.channel<0 || route.channel>15)
        return false;
    channel_=route.channel; started_ms_=now_ms; active_=true;
    return true;
}
void RelayPulseSimulation::Tick(uint32_t now_ms) {
    if (active_ && static_cast<uint32_t>(now_ms-started_ms_) >= 500U) Cancel();
}
void RelayPulseSimulation::Cancel() { active_=false; channel_=-1; }
} // namespace smv::bench
