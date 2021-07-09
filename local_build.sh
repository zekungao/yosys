#!/bin/bash
export VERIFIC_DIR = /home/andy/icbench/maple/verific
make PREFIX=build CONFIG=gcc install -j$(nproc)
