#!/bin/bash
git clone --recursive git@github.com:ICBench/maple.git
cd maple && ./etc/Build.sh
cd ..
make PREFIX=build CONFIG=gcc VERIFIC_DIR=maple/src/verific VERIFIC_LIB=maple/build/src/verific install -j$(nproc)
