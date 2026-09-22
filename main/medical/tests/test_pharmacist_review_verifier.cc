#include "pharmacist_review_verifier.h"

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

std::string ValidReview() {
    return std::string("{\"schema_version\":1,\"approved\":true,") +
           "\"reviewed_catalog_version\":\"catalog-v1\"," +
           "\"reviewed_rules_version\":\"rules-v1\"," +
           "\"reviewer\":\"Pharmacist A\",\"reviewer_license\":\"VN-123\"," +
           "\"reviewed_at\":\"2026-09-22T10:15:30+07:00\"," + "\"catalog_sha256\":\"" +
           std::string(64, 'b') + "\"," + "\"rules_sha256\":\"" + std::string(64, 'a') + "\"}";
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 2)
        return 2;
    try {
        const smv::ArtifactIdentity identity{
            .rules_version = "rules-v1",
            .catalog_version = "catalog-v1",
            .rules_sha256 = std::string(64, 'a'),
            .catalog_sha256 = std::string(64, 'b'),
        };
        int count = 0;
        const auto valid = ValidReview();
        Check(smv::PharmacistReviewVerifier::Verify(valid, identity).valid,
              "complete matching review was rejected");
        ++count;
        const std::string embedded_review = valid + "x";
        Check(smv::PharmacistReviewVerifier::Verify(
                  std::string_view(embedded_review.data(), valid.size()), identity)
                  .valid,
              "bounded non-null-terminated review view was rejected");
        ++count;
        Check(!smv::PharmacistReviewVerifier::Verify(
                   ReplaceOnce(valid, "\"approved\":true", "\"approved\":false"), identity)
                   .valid,
              "approved=false was accepted");
        ++count;
        Check(!smv::PharmacistReviewVerifier::Verify(
                   ReplaceOnce(valid, "\"reviewer_license\":\"VN-123\",", ""), identity)
                   .valid,
              "missing license was accepted");
        ++count;
        Check(
            !smv::PharmacistReviewVerifier::Verify(
                 ReplaceOnce(valid, "\"reviewer\":\"Pharmacist A\"", "\"reviewer\":null"), identity)
                 .valid,
            "null reviewer was accepted");
        ++count;
        Check(!smv::PharmacistReviewVerifier::Verify(ReplaceOnce(valid, "catalog-v1", "catalog-v0"),
                                                     identity)
                   .valid,
              "catalog version drift was accepted");
        ++count;
        Check(!smv::PharmacistReviewVerifier::Verify(
                   ReplaceOnce(valid, std::string(64, 'a'), std::string(64, 'c')), identity)
                   .valid,
              "rules hash drift was accepted");
        ++count;
        Check(!smv::PharmacistReviewVerifier::Verify(
                   ReplaceOnce(valid, std::string(64, 'b'), std::string(64, 'B')), identity)
                   .valid,
              "uppercase hash was accepted");
        ++count;
        Check(!smv::PharmacistReviewVerifier::Verify(
                   ReplaceOnce(valid, "2026-09-22T10:15:30+07:00", "2026/09/22 10:15"), identity)
                   .valid,
              "malformed review timestamp was accepted");
        ++count;
        Check(!smv::PharmacistReviewVerifier::Verify(
                   ReplaceOnce(valid, "}", ",\"notice\":\"unexpected\"}"), identity)
                   .valid,
              "unknown review field was accepted");
        ++count;
        Check(!smv::PharmacistReviewVerifier::Verify(Read(argv[1]), identity).valid,
              "incomplete repository review was accepted");
        ++count;
        std::cout << "HOST_PHARMACIST_REVIEW_TESTS_PASS=" << count << "\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}
