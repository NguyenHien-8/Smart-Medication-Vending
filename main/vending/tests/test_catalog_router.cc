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
         << "\",\"strength\":\"" << strength << "\",\"backup_of_channel\":" << backup << "}";
    return json.str();
}

std::string Catalog(const std::string& slots) {
    return "{\"schema_version\":1,\"catalog_version\":\"catalog-test\","
           "\"dispense_unit\":\"sealed_blister\",\"slots\":[" +
           slots + "]}";
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 2)
        return 2;
    try {
        const auto primary =
            Slot(0, "SKU-A", "MED_A", "Medicine A", "Ingredient A", "10 mg", "null");
        const auto backup = Slot(1, "SKU-A-B", "MED_A", "Medicine A", "Ingredient A", "10 mg", "0");
        const smv::CatalogRouter router(Catalog(primary + "," + backup));
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
            Catalog(primary + "," + ReplaceOnce(backup, "\"channel\":1", "\"channel\":0")));
        Check(!duplicate_channel.valid() &&
                  duplicate_channel.validation_reason() == "DUPLICATE_CHANNEL",
              "duplicate channel accepted");
        ++count;
        const smv::CatalogRouter duplicate_primary(
            Catalog(primary + "," +
                    ReplaceOnce(backup, "\"backup_of_channel\":0", "\"backup_of_channel\":null")));
        Check(!duplicate_primary.valid() &&
                  duplicate_primary.validation_reason() == "DUPLICATE_PRIMARY",
              "duplicate primary accepted");
        ++count;
        const auto second_backup =
            Slot(2, "SKU-A-C", "MED_A", "Medicine A", "Ingredient A", "10 mg", "0");
        const smv::CatalogRouter duplicate_backup(
            Catalog(primary + "," + backup + "," + second_backup));
        Check(
            !duplicate_backup.valid() && duplicate_backup.validation_reason() == "DUPLICATE_BACKUP",
            "multiple backups accepted");
        ++count;
        const smv::CatalogRouter wrong_reference(
            Catalog(primary + "," +
                    ReplaceOnce(backup, "\"backup_of_channel\":0", "\"backup_of_channel\":2")));
        Check(!wrong_reference.valid() &&
                  wrong_reference.validation_reason() == "BACKUP_REFERENCE_MISMATCH",
              "wrong backup reference accepted");
        ++count;

        for (const auto& mismatch : {
                 ReplaceOnce(backup, "Medicine A", "Medicine B"),
                 ReplaceOnce(backup, "Ingredient A", "Ingredient B"),
                 ReplaceOnce(backup, "10 mg", "20 mg"),
             }) {
            const smv::CatalogRouter invalid(Catalog(primary + "," + mismatch));
            Check(!invalid.valid() && invalid.validation_reason() == "BACKUP_IDENTITY_MISMATCH",
                  "backup identity mismatch accepted");
            ++count;
        }

        const smv::CatalogRouter repository_catalog(Read(argv[1]));
        Check(!repository_catalog.valid() &&
                  repository_catalog.validation_reason() == "BACKUP_IDENTITY_MISMATCH",
              "repository antacid mismatch was not locked");
        ++count;

        const std::string bounded_catalog = Catalog(primary) + "trailing-untrusted-bytes";
        const smv::CatalogRouter bounded_view(
            std::string_view(bounded_catalog.data(), Catalog(primary).size()));
        Check(bounded_view.valid(), "bounded catalog string_view read past its declared length");
        ++count;
        std::cout << "HOST_CATALOG_ROUTER_TESTS_PASS=" << count << "\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}
