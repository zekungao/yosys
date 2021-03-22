#!/bin/bash

make PREFIX=build CONFIG=gcc install -j$(nproc)
