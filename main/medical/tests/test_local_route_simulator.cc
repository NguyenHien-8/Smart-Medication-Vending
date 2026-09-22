#include "medical_advisor.h"
#include "local_route_simulator.h"
#include <cJSON.h>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <climits>

namespace {
std::string Read(const char* path) { std::ifstream f(path); if (!f) throw std::runtime_error(path); std::stringstream b; b<<f.rdbuf(); return b.str(); }
void Check(bool p, const char* what) { if (!p) throw std::runtime_error(what); }
std::string Snapshot() { return R"({"session_id":"bench","turn_id":1,"age_years":30,"weight_kg":65,"pregnancy_or_breastfeeding":false,"symptoms":["mild_headache"],"duration_hours":3,"danger_signs":[],"conditions":[],"current_medicines":[],"drug_allergies":[],"screening_answers":{"redflag_breathing":false,"redflag_neurologic":false,"redflag_weakness":false,"redflag_bleeding":false,"redflag_black_stool":false,"redflag_other":false,"headache_sudden":false,"headache_vomit":false,"headache_stiff":false,"headache_mild":true}})"; }
std::string Change(const std::string& a,const std::string& b,const std::string& c) {
 auto pos=a.find(b); if(pos==std::string::npos)throw std::runtime_error("fixture not found");
 auto d=a;d.replace(pos,b.size(),c);return d;
}
}
int main(int argc, char** argv) {
 if(argc!=4) return 2;
 try {
 auto rules=Read(argv[1]),catalog=Read(argv[2]),review=Read(argv[3]);
 smv::MedicalAdvisor adviser(rules.c_str(),catalog.c_str(),review.c_str());
 auto evaluated=adviser.Evaluate(Snapshot());
 smv::bench::Inventory inventory;
 int n=0;
 auto preview=[&](const std::string& response,const std::string& cat,const std::string& rev) {
   return smv::bench::PreviewRoute(response,cat,rules,rev,inventory);
 };
 auto test=[&](bool passed,const char* msg){Check(passed,msg);++n;};
 test(preview(evaluated,catalog,review).reason=="PHARMACIST_REVIEW_OR_VERSION_MISMATCH", "pilot review must block");
 // Synthetic approval in an ISOLATED HOST BENCH TEST, never update data/pharmacist_review.json.
 auto synthetic=Change(review,"\"approved\": false","\"approved\": true");
 synthetic=Change(synthetic,"\"reviewed_catalog_version\": null", "\"reviewed_catalog_version\": \"catalog-2026-09-22-interview-pilot\"");
 synthetic=Change(synthetic,"\"reviewed_rules_version\": null", "\"reviewed_rules_version\": \"rules-2026-09-22-interview-pilot-v2\"");
 test(preview(evaluated,catalog,synthetic).reason=="INVENTORY_NOT_VERIFIED", "unknown stock cannot route");
 inventory.externally_verified=true; inventory.measured[0]=2; inventory.measured[13]=3;
 auto main=preview(evaluated,catalog,synthetic);
 test(main.status=="SIMULATION_READY" && main.channel==0 && main.sku=="SMV-PARA500", "primary channel");
 inventory.measured[0]=0;
 auto backup=preview(evaluated,catalog,synthetic);
 test(backup.status=="SIMULATION_READY" && backup.channel==13 && backup.sku=="SMV-PARA500-B", "backup after primary empty");
 inventory.measured[13]=0;
 test(preview(evaluated,catalog,synthetic).reason=="VERIFIED_STOCK_EMPTY", "empty stocks must block");
 inventory.measured[0]=1;
 // Mapping of all backup pairs using synthetic local-output fixtures: routing
 // tests only, not evidence that any clinical recommendation is appropriate.
 auto route_fixture=[](const char* id) {
   return std::string("{\"status\":\"PROVISIONAL_OPTIONS\",\"vend_allowed\":false,\"provisional_options\":[{\"canonical_id\":\"") + id + "\"}]}";
 };
 inventory.measured[0]=0;
 inventory.measured[3]=1; inventory.measured[14]=1;
 auto dry_primary=preview(route_fixture("DEXTROMETHORPHAN_15"),catalog,synthetic);
 test(dry_primary.channel==3 && dry_primary.sku=="SMV-DXM15", "dry-cough main channel fixture");
 inventory.measured[3]=0;
 auto dry_backup=preview(route_fixture("DEXTROMETHORPHAN_15"),catalog,synthetic);
 test(dry_backup.channel==14 && dry_backup.sku=="SMV-DXM15-B", "dry-cough backup channel fixture");
 inventory.measured[7]=1; inventory.measured[15]=1;
 test(preview(route_fixture("ANTACID_200_200"),catalog,synthetic).reason=="BACKUP_SKU_MISMATCH",
      "original antacid backup strength is ambiguous; must block both channels");
 // A corrected catalog below exists ONLY in memory inside this routing test.
 // It is not a pharmacist sign-off or a change to data/medicines.json.
 const auto corrected_antacid_catalog=Change(catalog,"\"strength\": \"cùng SKU channel 7\"",
                                                    "\"strength\": \"ví dụ 200 mg + 200 mg\"");
 auto antacid_main=preview(route_fixture("ANTACID_200_200"),corrected_antacid_catalog,synthetic);
 test(antacid_main.channel==7 && antacid_main.sku=="SMV-ANTACID", "corrected-fixture antacid main channel");
 inventory.measured[7]=0;
 auto antacid_backup=preview(route_fixture("ANTACID_200_200"),corrected_antacid_catalog,synthetic);
 test(antacid_backup.channel==15 && antacid_backup.sku=="SMV-ANTACID-B", "corrected-fixture antacid backup channel");
 inventory.measured[0]=1;
 test(preview(evaluated,catalog,Change(synthetic,"\"catalog-2026-09-22-interview-pilot\"","\"old\"")).reason=="PHARMACIST_REVIEW_OR_VERSION_MISMATCH", "catalog drift");
 test(preview("{\"status\":\"PROVISIONAL_OPTIONS\",\"vend_allowed\":true}",catalog,synthetic).reason=="ADVISOR_NOT_PROVISIONAL", "cloud forced authorization");
 // Keep versions unchanged while introducing a duplicate channel to verify the mapping guard.
 test(preview(evaluated,Change(catalog,"\"channel\": 13","\"channel\": 0"),synthetic).reason=="INVALID_OR_DUPLICATED_CHANNEL_MAP", "duplicate map fail-closed");
 test(preview(evaluated,Change(catalog,"\"backup_of_channel\": 0","\"backup_of_channel\": 1"),synthetic).reason=="BACKUP_SKU_MISMATCH", "wrong backup reference");
 test(preview("{}",catalog,synthetic).reason=="ADVISOR_NOT_PROVISIONAL", "invalid advisory");
 smv::bench::RelayPulseSimulation relay;
 test(!relay.Start(smv::bench::Route{},100), "blocked plan must not energize");
 test(relay.Start(main,100) && relay.channel()==0 && relay.logical_level()==0, "active low bench start");
 test(!relay.Start(backup,101), "no overlapping pulse");
 relay.Tick(599);
 test(relay.active() && relay.logical_level()==0, "still active at 499ms");
 relay.Tick(600);
 test(!relay.active() && relay.logical_level()==1 && relay.channel()==-1, "inactive at 500ms");
 test(relay.Start(backup,UINT_MAX-100) && relay.channel()==13, "wraparound start");
 relay.Tick(398);
 test(relay.active(),"wraparound 499ms");
 relay.Tick(399);
 test(!relay.active(),"wraparound 500ms");
 test(relay.Start(main,42),"restart after stop"); relay.Cancel();
 test(!relay.active() && relay.logical_level()==1, "cancel must de-energize");
 std::cout<<"HOST_LOCAL_ROUTE_SIMULATION_TESTS_PASS="<<n<<"\n";
 } catch(const std::exception& e) { std::cerr<<"FAIL: "<<e.what()<<"\n";return 1; }
}
