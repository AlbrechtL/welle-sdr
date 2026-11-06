# Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

zephyr_include_directories(include)

if(CONFIG_ADC_DVP_WCH)
  zephyr_library()
  zephyr_library_sources(dvp_adc_wch.c)
endif()
