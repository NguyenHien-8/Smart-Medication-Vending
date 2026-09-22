#pragma once

#include <string>
#include <string_view>

namespace smv {
struct ArtifactIdentity {
    std::string rules_version;
    std::string catalog_version;
    std::string rules_sha256;
    std::string catalog_sha256;
};

enum class ReviewFailure {
    kNone,
    kInvalidJson,
    kUnsupportedSchema,
    kNotApproved,
    kMissingReviewer,
    kMissingLicense,
    kInvalidReviewedAt,
    kVersionMismatch,
    kHashMismatch,
};

struct ReviewResult {
    bool valid = false;
    ReviewFailure failure = ReviewFailure::kInvalidJson;
};

class PharmacistReviewVerifier {
public:
    static ReviewResult Verify(std::string_view review_json, const ArtifactIdentity& identity);
};
}  // namespace smv
