# SPDX-License-Identifier: Apache-2.0
# Reference fixed vendor sources; never alter/copy the RTC archives.
set(AGORA_SRCS)
set(AGORA_INCLUDES)
if(CONFIG_BK7258_AGORA_RTC)
  if(NOT CONFIG_NET_LOOPBACK)
    message(FATAL_ERROR "Agora AOSL wakeup sockets require CONFIG_NET_LOOPBACK=y")
  endif()
  if(NOT CONFIG_NET_ETHERNET OR NOT DEFINED CONFIG_NET_ETH_PKTSIZE OR
     CONFIG_NET_ETH_PKTSIZE LESS 1514)
    message(FATAL_ERROR "Agora RTC requires Ethernet packet size >=1514 (IPv4 MTU >=1500)")
  endif()
  set(AGORA_SDK "${NUTTX_DIR}/../vendor/beken/bk_avdk_smp/ap/components/bk_thirdparty/agora-iot-sdk")
  foreach(pair
      "librtsa.a|ed5d6d94aaf13a3f8e5e90d0c8cce9487f18cbd4a39b1a29a077f86f313c68d9"
      "libagora-cjson.a|7c2842e1e7262467a1c73d0265560e1442bf2a497eb83f7b594ba72cc8689249")
    string(REPLACE "|" ";" fields "${pair}")
    list(GET fields 0 name)
    list(GET fields 1 expected)
    set(lib "${AGORA_SDK}/bk7258/libs/${name}")
    file(SHA256 "${lib}" actual)
    if(NOT actual STREQUAL expected)
      message(FATAL_ERROR "Pinned Agora archive mismatch: ${name}")
    endif()
    nuttx_add_extra_library("${lib}")
  endforeach()
  execute_process(COMMAND git -C "${AGORA_SDK}" rev-parse HEAD
                  OUTPUT_VARIABLE revision OUTPUT_STRIP_TRAILING_WHITESPACE
                  RESULT_VARIABLE result)
  if(NOT result EQUAL 0 OR NOT revision STREQUAL "cb080de1655d579c7593ecf504c440997c4c137b")
    message(FATAL_ERROR "Agora AOSL sources require the fixed Beken SDK commit")
  endif()
  # Same portable sources as the pinned SDK CMakeLists, excluding its BK HAL.
  set(portable
      kernel/file.c kernel/fileobj.c kernel/iofd.c kernel/log.c
      kernel/mpq.c kernel/mpqp.c kernel/osmp.c kernel/panic.c
      kernel/refobj.c kernel/task.c kernel/thread.c kernel/time.c
      kernel/timer.c kernel/version.c kernel/select_mp.c kernel/poll_mp.c
      kernel/et_mp.c kernel/rt_monitor.c
      lib/atomic.c lib/bitmap.c lib/byteswap.c lib/cond.c lib/errno.c
      lib/marshalling-base-obj.c lib/marshalling.c lib/psbuff.c lib/rbtree.c
      lib/rwlock.c lib/rwlock-xadd.c lib/thread.c lib/tls.c lib/utils.c
      mm/mm.c net/dns.c net/if.c net/inet_ntop.c net/inet_pton.c
      net/ipv6.c net/route.c net/sk_utils.c)
  foreach(src IN LISTS portable)
    set(path "${AGORA_SDK}/hal/aosl/${src}")
    list(APPEND AGORA_SRCS "${path}")
    set_property(SOURCE "${path}" APPEND PROPERTY COMPILE_OPTIONS
                 -include "${CMAKE_CURRENT_LIST_DIR}/aosl_nuttx_compat.h")
  endforeach()
  list(APPEND AGORA_SRCS
       "${CMAKE_CURRENT_LIST_DIR}/agora_nuttx_hal.c"
       "${CMAKE_CURRENT_LIST_DIR}/agora_nuttx_socket.c")
  list(APPEND AGORA_INCLUDES
       "${AGORA_SDK}/include/bk7258" "${AGORA_SDK}/hal/aosl/include"
       "${AGORA_SDK}/hal/aosl/platform/include"
       "${AGORA_SDK}/hal/aosl/platform/src/bk7258/config")
  nuttx_add_aux_library(bk7258_agora_port ${AGORA_SRCS})
  target_include_directories(bk7258_agora_port PRIVATE ${AGORA_INCLUDES})
  # The portable vendor header knows FreeRTOS/Linux, not NuttX's ssize_t.
  # Supply the real NuttX type first rather than redefining it in vendor code.
  target_compile_options(bk7258_agora_port PRIVATE -include sys/types.h)
  target_compile_definitions(bk7258_agora_port PRIVATE __ssize_t_defined=1)
  target_link_libraries(arch PRIVATE bk7258_agora_port)
  add_library(bk7258_agora_api INTERFACE)
  target_include_directories(bk7258_agora_api INTERFACE "${AGORA_SDK}/include/bk7258")
endif()
