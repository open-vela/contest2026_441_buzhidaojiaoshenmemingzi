############################################################################
# Contest 2026 team 441 vision badge - Makefile
############################################################################

include $(APPDIR)/Make.defs

PROGNAME  =
PRIORITY  =
STACKSIZE =
MAINSRC   =
MODULE    = y

CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/system/contest2026_441_vision_badge/include

ifneq ($(CONFIG_LVX_USE_DEMO_CONTEST2026_441_VISION_BADGE),)
ifeq ($(CONFIG_CONTEST2026_441_AGORA),y)
$(error Agora uses the verified official tools/bk7258/bk7258.py CMake entry; Make integration is not verified)
endif
PROGNAME  += vision_badge
PRIORITY  += 100
STACKSIZE += $(CONFIG_CONTEST2026_441_VISION_BADGE_STACKSIZE)
MAINSRC   += src/vision_badge_main.c
CSRCS     = src/workflow.c src/camera_service.c src/mimo_request.c
CSRCS    += src/http_date.c src/vision_service.c src/mimo_http.c
CSRCS    += src/vision_badge_rpc.c src/audio_service.c src/feedback_service.c
ifeq ($(CONFIG_CONTEST2026_441_XIAOZHI),y)
CSRCS    += src/xiaozhi_protocol.c
CSRCS    += src/xiaozhi_codec.c src/xiaozhi_playout.c
CSRCS    += src/xiaozhi_transport.c
CSRCS    += src/xiaozhi_conversation.c
endif
endif

ifneq ($(CONFIG_LVX_USE_DEMO_CONTEST2026_441_VISION_BADGE_CLIENT),)
PROGNAME  += vision_badge
PRIORITY  += 100
STACKSIZE += 4096
MAINSRC   += src/vision_badge_client_main.c
CSRCS     += src/vision_badge_rpc.c
endif

ifneq ($(CONFIG_LVX_USE_DEMO_CONTEST2026_441_R1_KEYLED),)
PROGNAME  += r1keyled
PRIORITY  += 100
STACKSIZE += $(CONFIG_CONTEST2026_441_R1_KEYLED_STACKSIZE)
MAINSRC   += src/r1_keyled_main.c
endif

include $(APPDIR)/Application.mk
