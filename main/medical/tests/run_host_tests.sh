#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
CXX="${CXX:-g++}"
FLAGS=(-std=c++17 -Wall -Wextra -Werror -Imain/medical/tests/host_include -Imain/medical)
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT
"$CXX" "${FLAGS[@]}" main/medical/medical_advisor.cc main/medical/tests/test_medical_advisor.cc -Wl,-l:libcjson.so.1 -o "$TMP_DIR/medical"
"$TMP_DIR/medical" data/medical_rules.json data/medicines.json data/pharmacist_review.json
"$CXX" "${FLAGS[@]}" main/medical/medical_advisor.cc main/medical/tests/local_route_simulator.cc main/medical/tests/test_local_route_simulator.cc -Wl,-l:libcjson.so.1 -o "$TMP_DIR/route"
"$TMP_DIR/route" data/medical_rules.json data/medicines.json data/pharmacist_review.json
