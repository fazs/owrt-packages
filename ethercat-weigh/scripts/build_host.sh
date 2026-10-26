#!/usr/bin/env bash

# Check the device description against the ESI and the supplement file
python3 scripts/esi_gen.py "esi/Beckhoff EL3356-0010.xml" --type EL3356-0010 \
  --supplement scripts/esi_supplement.el3356_0010.json \
  -o src/generated/el3356_0010.json --check || exit 1

cmake -S . -B build/host -DCMAKE_BUILD_TYPE=Debug
cmake --build build/host || exit 1

# Generate the OpenAPI spec from the registered routes (before the tests that check it)
./build/host/ethercat-weigh --write-openapi src/generated/openapi.json || exit 1
git diff --stat -- src/generated/openapi.json

./build/host/ew_tests

# ./build/host/ethercat-weigh --write-default-config build/dev.json
echo "correct web_root in dev.json"
echo "run ./build/host/ethercat-weigh --sim -c ./build/dev.json"
echo " or ./build/host/ethercat-weigh --sim-csv <measurements.csv> -c ./build/dev.json"

