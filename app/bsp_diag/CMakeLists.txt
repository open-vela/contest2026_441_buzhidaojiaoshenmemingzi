# SPDX-License-Identifier: Apache-2.0

if(CONFIG_CONTEST2026_441_BSP_APCTL)
  nuttx_add_application(
    NAME apctl
    SRCS apctl_main.c
    STACKSIZE 3072
    PRIORITY 100)
endif()

if(CONFIG_CONTEST2026_441_BSP_WIFI)
  nuttx_add_application(
    NAME bkwifi
    SRCS bkwifi_main.c
    STACKSIZE 4096
    PRIORITY 100)
endif()
