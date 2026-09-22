#include "pharmacist_review_verifier.h"

#include <cJSON.h>

#include <cctype>
#include <cmath>
#include <cstring>
#include <memory>
#include <set>
#include <string>

namespace smv {
namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

const cJSON* Get(const cJSON* object, const char* key) {
    return cJSON_GetObjectItemCaseSensitive(object, key);
}

const char* String(const cJSON* value) {
    return cJSON_IsString(value) ? value->valuestring : nullptr;
}

bool IsBoundedText(const char* value) {
    return value != nullptr && value[0] != '\0' && std::strlen(value) <= 96;
}

bool IsLowerSha256(const char* value) {
    if (value == nullptr || std::strlen(value) != 64)
        return false;
    for (size_t index = 0; index < 64; ++index) {
        const unsigned char byte = static_cast<unsigned char>(value[index]);
        if (!std::isdigit(byte) && (byte < 'a' || byte > 'f'))
            return false;
    }
    return true;
}

bool Digits(const std::string_view value, size_t offset, size_t count) {
    if (offset + count > value.size())
        return false;
    for (size_t index = offset; index < offset + count; ++index) {
        if (!std::isdigit(static_cast<unsigned char>(value[index])))
            return false;
    }
    return true;
}

unsigned ParseUnsigned(const std::string_view value, size_t offset, size_t count) {
    unsigned result = 0;
    for (size_t index = offset; index < offset + count; ++index) {
        result = result * 10 + static_cast<unsigned>(value[index] - '0');
    }
    return result;
}

bool IsLeapYear(unsigned year) { return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0); }

bool IsRfc3339(const char* text) {
    if (text == nullptr)
        return false;
    const std::string_view value(text);
    if (value.size() != 20 && value.size() != 25)
        return false;
    if (!Digits(value, 0, 4) || !Digits(value, 5, 2) || !Digits(value, 8, 2) ||
        !Digits(value, 11, 2) || !Digits(value, 14, 2) || !Digits(value, 17, 2) ||
        value[4] != '-' || value[7] != '-' || value[10] != 'T' || value[13] != ':' ||
        value[16] != ':')
        return false;

    const unsigned year = ParseUnsigned(value, 0, 4);
    const unsigned month = ParseUnsigned(value, 5, 2);
    const unsigned day = ParseUnsigned(value, 8, 2);
    const unsigned hour = ParseUnsigned(value, 11, 2);
    const unsigned minute = ParseUnsigned(value, 14, 2);
    const unsigned second = ParseUnsigned(value, 17, 2);
    static constexpr unsigned kMonthDays[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (year == 0 || month < 1 || month > 12 || hour > 23 || minute > 59 || second > 59)
        return false;
    unsigned maximum_day = kMonthDays[month];
    if (month == 2 && IsLeapYear(year))
        ++maximum_day;
    if (day < 1 || day > maximum_day)
        return false;

    if (value.size() == 20)
        return value[19] == 'Z';
    if ((value[19] != '+' && value[19] != '-') || value[22] != ':' || !Digits(value, 20, 2) ||
        !Digits(value, 23, 2))
        return false;
    return ParseUnsigned(value, 20, 2) <= 23 && ParseUnsigned(value, 23, 2) <= 59;
}

ReviewResult Failure(ReviewFailure failure) { return ReviewResult{false, failure}; }
}  // namespace

ReviewResult PharmacistReviewVerifier::Verify(std::string_view review_json,
                                              const ArtifactIdentity& identity) {
    const std::string review_text(review_json);
    Json review(
        cJSON_ParseWithLengthOpts(review_text.c_str(), review_text.size() + 1, nullptr, true),
        &cJSON_Delete);
    if (!review || !cJSON_IsObject(review.get()))
        return Failure(ReviewFailure::kInvalidJson);

    static const std::set<std::string> kAllowedFields = {
        "schema_version",
        "approved",
        "reviewed_catalog_version",
        "reviewed_rules_version",
        "reviewer",
        "reviewer_license",
        "reviewed_at",
        "catalog_sha256",
        "rules_sha256",
    };
    std::set<std::string> seen;
    const cJSON* field = nullptr;
    cJSON_ArrayForEach (field, review.get()) {
        if (field->string == nullptr || !kAllowedFields.contains(field->string) ||
            !seen.insert(field->string).second)
            return Failure(ReviewFailure::kInvalidJson);
    }

    const cJSON* schema = Get(review.get(), "schema_version");
    if (!cJSON_IsNumber(schema) || !std::isfinite(schema->valuedouble) || schema->valueint != 1 ||
        schema->valuedouble != 1.0)
        return Failure(ReviewFailure::kUnsupportedSchema);
    if (!cJSON_IsBool(Get(review.get(), "approved")) ||
        !cJSON_IsTrue(Get(review.get(), "approved")))
        return Failure(ReviewFailure::kNotApproved);

    const char* reviewer = String(Get(review.get(), "reviewer"));
    if (!IsBoundedText(reviewer))
        return Failure(ReviewFailure::kMissingReviewer);
    const char* license = String(Get(review.get(), "reviewer_license"));
    if (!IsBoundedText(license))
        return Failure(ReviewFailure::kMissingLicense);
    if (!IsRfc3339(String(Get(review.get(), "reviewed_at"))))
        return Failure(ReviewFailure::kInvalidReviewedAt);

    const char* catalog_version = String(Get(review.get(), "reviewed_catalog_version"));
    const char* rules_version = String(Get(review.get(), "reviewed_rules_version"));
    if (catalog_version == nullptr || rules_version == nullptr ||
        identity.catalog_version != catalog_version || identity.rules_version != rules_version)
        return Failure(ReviewFailure::kVersionMismatch);

    const char* catalog_hash = String(Get(review.get(), "catalog_sha256"));
    const char* rules_hash = String(Get(review.get(), "rules_sha256"));
    if (!IsLowerSha256(catalog_hash) || !IsLowerSha256(rules_hash) ||
        identity.catalog_sha256 != catalog_hash || identity.rules_sha256 != rules_hash)
        return Failure(ReviewFailure::kHashMismatch);

    return ReviewResult{true, ReviewFailure::kNone};
}
}  // namespace smv
