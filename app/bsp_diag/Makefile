############################################################################
# Contest 2026 team 441 BK7258 BSP diagnostics
############################################################################

include $(APPDIR)/Make.defs

PROGNAME  =
PRIORITY  =
STACKSIZE =
MAINSRC   =
MODULE    = y

ifneq ($(CONFIG_CONTEST2026_441_BSP_APCTL),)
PROGNAME  += apctl
PRIORITY  += 100
STACKSIZE += 3072
MAINSRC   += apctl_main.c
endif

ifneq ($(CONFIG_CONTEST2026_441_BSP_WIFI),)
PROGNAME  += bkwifi
PRIORITY  += 100
STACKSIZE += 4096
MAINSRC   += bkwifi_main.c
endif

include $(APPDIR)/Application.mk
