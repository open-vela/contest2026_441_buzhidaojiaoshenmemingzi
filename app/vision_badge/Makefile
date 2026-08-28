############################################################################
# Contest 2026 team 441 vision badge - Makefile
############################################################################

include $(APPDIR)/Make.defs

PROGNAME  =
PRIORITY  =
STACKSIZE =
MAINSRC   =
MODULE    = y

CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/packages/demos/contest2026_441_vision_badge/include

ifneq ($(CONFIG_LVX_USE_DEMO_CONTEST2026_441_VISION_BADGE),)
PROGNAME  += vision_badge
PRIORITY  += 100
STACKSIZE += $(CONFIG_CONTEST2026_441_VISION_BADGE_STACKSIZE)
MAINSRC   += src/vision_badge_main.c
CSRCS   = src/workflow.c src/camera_service.c src/vision_service.c
CSRCS  += src/audio_service.c src/feedback_service.c
endif

ifneq ($(CONFIG_LVX_USE_DEMO_CONTEST2026_441_R1_KEYLED),)
PROGNAME  += r1keyled
PRIORITY  += 100
STACKSIZE += $(CONFIG_CONTEST2026_441_R1_KEYLED_STACKSIZE)
MAINSRC   += src/r1_keyled_main.c
endif

include $(APPDIR)/Application.mk
