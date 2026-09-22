#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
CXX="${CXX:-g++}"
FLAGS=(-std=c++23 -Wall -Wextra -Werror -Imain/medical/tests/host_include \
  -Imain -Imain/medical -Imain/vending -Imain/inventory \
  -Imain/boards/smartmedivend-s3)
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT
"$CXX" "${FLAGS[@]}" main/medical/medical_advisor.cc \
  main/medical/tests/test_medical_advisor.cc -Wl,-l:libcjson.so.1 \
  -o "$TMP_DIR/medical_advisor_test"
"$TMP_DIR/medical_advisor_test" data/medical_rules.json data/medicines.json \
  data/pharmacist_review.json
