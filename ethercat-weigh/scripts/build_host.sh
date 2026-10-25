#!/usr/bin/env bash

cmake -S . -B build/host -DCMAKE_BUILD_TYPE=Debug
cmake --build build/host
./build/host/ew_tests

# ./build/host/ethercat-weigh --write-default-config build/dev.json
echo "correct web_root in dev.json"
echo "run ./build/host/ethercat-weigh --sim -c ./build/dev.json"
echo " or ./build/host/ethercat-weigh --sim-csv <measurements.csv> -c ./build/dev.json"

