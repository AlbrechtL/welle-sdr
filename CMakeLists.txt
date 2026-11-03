# Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_SOURCE_DIR}/modules/udc_wch_usbhs)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(rtlsdr_emu)

target_sources(app PRIVATE
  src/main.c
  src/rtl2832u.c
  src/r820t2.c
  src/sample_src.c
  src/welle_sdr_shell.c
)
