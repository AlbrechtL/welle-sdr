# Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

if(CONFIG_UDC_WCH_USBHS)
  zephyr_library()
  zephyr_library_sources(udc_wch_usbhs.c)
  # udc_common.h is private to drivers/usb/udc
  zephyr_library_include_directories(${ZEPHYR_BASE}/drivers/usb/udc)
endif()
