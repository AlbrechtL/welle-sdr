# Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(welle_sdr_ddc_test)

target_include_directories(app PRIVATE ../../src)
target_sources(app PRIVATE
  src/main.c
  ../../src/ddc.c
)
