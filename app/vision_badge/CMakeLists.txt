# SPDX-License-Identifier: Apache-2.0

if(CONFIG_LVX_USE_DEMO_CONTEST2026_441_VISION_BADGE)
  set(VOICE_SERVICE_SRCS)
  if(CONFIG_CONTEST2026_441_POWER)
    list(APPEND VOICE_SERVICE_SRCS src/power_service.c)
  endif()
  if(CONFIG_CONTEST2026_441_XIAOZHI)
    list(APPEND VOICE_SERVICE_SRCS src/xiaozhi_protocol.c src/xiaozhi_codec.c
         src/xiaozhi_playout.c src/xiaozhi_transport.c src/xiaozhi_conversation.c)
  endif()
  if(CONFIG_CONTEST2026_441_AGORA)
    list(APPEND VOICE_SERVICE_SRCS src/agora_service.c src/agora_control.c)
  endif()
  if(CONFIG_CONTEST2026_441_WAKE)
    list(APPEND VOICE_SERVICE_SRCS src/wake_service.c)
    if(CONFIG_CONTEST2026_441_WAKE_ASR)
      if(CONFIG_CONTEST2026_441_WAKE_UDP)
        list(APPEND VOICE_SERVICE_SRCS src/wake_asr_udp.c)
      elseif(CONFIG_CONTEST2026_441_WAKE_STREAM)
        list(APPEND VOICE_SERVICE_SRCS src/wake_asr_stream.c)
      else()
        list(APPEND VOICE_SERVICE_SRCS src/wake_asr.c)
      endif()
    else()
      list(APPEND VOICE_SERVICE_SRCS src/wake_policy.c src/wake_model.cc src/wake_traits.cc)
    endif()
  endif()
  nuttx_add_application(
    NAME vision_badge
    SRCS
      src/vision_badge_main.c
      src/workflow.c
      src/camera_service.c
      src/mimo_request.c
      src/http_date.c
      src/vision_service.c
      src/mimo_http.c
      src/vision_badge_rpc.c
      src/audio_service.c
      src/feedback_service.c
      ${VOICE_SERVICE_SRCS}
    STACKSIZE ${CONFIG_CONTEST2026_441_VISION_BADGE_STACKSIZE}
    PRIORITY 100
    INCLUDE_DIRECTORIES ${CMAKE_CURRENT_LIST_DIR}/include
      ${NUTTX_APPS_DIR}/external/opus/opus/include)
  if(CONFIG_CONTEST2026_441_AGORA)
    target_link_libraries(apps_vision_badge PRIVATE bk7258_agora_api)
  endif()
  if(CONFIG_CONTEST2026_441_WAKE AND NOT CONFIG_CONTEST2026_441_WAKE_ASR)
    include(wake.cmake)
  endif()
endif()

if(CONFIG_LVX_USE_DEMO_CONTEST2026_441_VISION_BADGE_CLIENT)
  nuttx_add_application(
    NAME vision_badge
    SRCS
      src/vision_badge_client_main.c
      src/vision_badge_rpc.c
    STACKSIZE 4096
    PRIORITY 100
    INCLUDE_DIRECTORIES ${CMAKE_CURRENT_LIST_DIR}/include)
endif()

if(CONFIG_LVX_USE_DEMO_CONTEST2026_441_R1_KEYLED)
  nuttx_add_application(
    NAME r1keyled
    SRCS src/r1_keyled_main.c
    STACKSIZE ${CONFIG_CONTEST2026_441_R1_KEYLED_STACKSIZE}
    PRIORITY 100)
endif()
