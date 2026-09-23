#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
CXX="${CXX:-g++}"
FLAGS=(-std=c++23 -Wall -Wextra -Werror -Imain/medical/tests/host_include \
  -Imain -Imain/medical -Imain/vending -Imain/inventory \
  -Imain/boards/smartmedivend-s3)
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT
"$CXX" "${FLAGS[@]}" main/medical/medical_policy_cache.cc \
  main/medical/tests/test_medical_policy_cache.cc -Wl,-l:libcjson.so.1 \
  -o "$TMP_DIR/medical_policy_cache_test"
"$TMP_DIR/medical_policy_cache_test" data/medical_rules.json data/medicines.json
"$CXX" "${FLAGS[@]}" main/medical/medical_policy_cache.cc \
  main/medical/medical_intake_session.cc main/medical/tests/test_medical_intake_session.cc \
  -Wl,-l:libcjson.so.1 -o "$TMP_DIR/medical_intake_session_test"
"$TMP_DIR/medical_intake_session_test" data/medical_rules.json data/medicines.json
"$CXX" "${FLAGS[@]}" main/medical/medical_policy_cache.cc \
  main/medical/medical_intake_session.cc main/medical/medical_advisor.cc \
  main/medical/tests/test_medical_advisor.cc -Wl,-l:libcjson.so.1 \
  -o "$TMP_DIR/medical_advisor_test"
"$TMP_DIR/medical_advisor_test" data/medical_rules.json data/medicines.json
"$CXX" "${FLAGS[@]}" main/medical/pharmacist_review_verifier.cc \
  main/medical/tests/test_pharmacist_review_verifier.cc -Wl,-l:libcjson.so.1 \
  -o "$TMP_DIR/pharmacist_review_test"
"$TMP_DIR/pharmacist_review_test" data/pharmacist_review.json
