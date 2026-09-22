#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
CXX="${CXX:-g++}"
FLAGS=(-std=c++23 -Wall -Wextra -Werror -Imain/medical/tests/host_include \
  -Imain -Imain/medical -Imain/vending -Imain/inventory \
  -Imain/boards/smartmedivend-s3)
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT
"$CXX" "${FLAGS[@]}" main/vending/catalog_router.cc \
  main/vending/tests/test_catalog_router.cc -Wl,-l:libcjson.so.1 \
  -o "$TMP_DIR/catalog_router_test"
"$TMP_DIR/catalog_router_test" data/medicines.json
"$CXX" "${FLAGS[@]}" main/inventory/inventory_store.cc \
  main/vending/tests/test_inventory_store.cc \
  -o "$TMP_DIR/inventory_store_test"
"$TMP_DIR/inventory_store_test"
"$CXX" "${FLAGS[@]}" main/medical/medical_advisor.cc \
  main/medical/pharmacist_review_verifier.cc main/vending/catalog_router.cc \
  main/inventory/inventory_store.cc main/vending/vend_guard.cc \
  main/vending/vending_coordinator.cc main/vending/tests/test_vending_coordinator.cc \
  -Wl,-l:libcjson.so.1 -o "$TMP_DIR/vending_coordinator_test"
"$TMP_DIR/vending_coordinator_test" data/medical_rules.json data/medicines.json \
  data/pharmacist_review.json
"$CXX" "${FLAGS[@]}" main/boards/smartmedivend-s3/relay_driver.cc \
  main/vending/tests/test_relay_driver.cc -o "$TMP_DIR/relay_driver_test"
"$TMP_DIR/relay_driver_test"
