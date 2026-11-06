# Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

list(APPEND ZEPHYR_EXTRA_MODULES
  ${CMAKE_CURRENT_SOURCE_DIR}/modules/udc_wch_usbhs
  ${CMAKE_CURRENT_SOURCE_DIR}/modules/adc_dvp_wch
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(welle-sdr)

target_sources(app PRIVATE
  src/ddc.c
  src/if_agc.c
  src/if_agc_ctl.c
  src/main.c
  src/rtl2832u.c
  src/r820t2.c
  src/sample_src.c
  src/si_fmt.c
  src/status_led.c
  src/testsig.c
  src/welle_sdr_shell.c
)

# The down-converter sample loop: -O2 is 2.2 times faster than the default -Os (measured)
set_source_files_properties(src/ddc.c PROPERTIES COMPILE_OPTIONS "-O2")
