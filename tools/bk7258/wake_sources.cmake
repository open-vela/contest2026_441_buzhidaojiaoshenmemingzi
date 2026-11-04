# SPDX-License-Identifier: Apache-2.0
# Same pinned OpenVela TFLM sources in host and target builds. Only six model
# operators; no model training, network stack, second media server or SDK audio.
set(_ml "${R1_WAKE_WORKSPACE}/apps/mlearning/tflite-micro/tflite-micro")
set(_front "${_ml}/tensorflow/lite/experimental/microfrontend/lib")
option(R1_WAKE_USE_CMSIS_NN "Use the TFLM-pinned CMSIS-NN kernels" ON)
set(_r1_nn_ops conv depthwise_conv fully_connected pooling softmax)
file(GLOB R1_WAKE_CORE_SOURCES
  "${_ml}/tensorflow/lite/micro/*.cc"
  "${_ml}/tensorflow/lite/micro/arena_allocator/*.cc"
  "${_ml}/tensorflow/lite/micro/memory_planner/*.cc"
  "${_ml}/tensorflow/lite/micro/tflite_bridge/*.cc"
  "${_ml}/tensorflow/lite/core/api/*.cc"
  "${_ml}/tensorflow/lite/core/c/*.cc"
  "${_ml}/tensorflow/lite/kernels/internal/*.cc"
  "${_ml}/tensorflow/lite/kernels/internal/reference/*.cc"
  "${_ml}/tensorflow/lite/kernels/kernel_util.cc"
  "${_ml}/tensorflow/lite/schema/*.cc")
list(FILTER R1_WAKE_CORE_SOURCES EXCLUDE REGEX "(test|mock|fake|benchmark)")
foreach(op conv depthwise_conv fully_connected pooling softmax reshape kernel_util micro_tensor_utils)
  file(GLOB _sources "${_ml}/tensorflow/lite/micro/kernels/${op}_common.cc")
  if(R1_WAKE_USE_CMSIS_NN AND op IN_LIST _r1_nn_ops)
    list(APPEND _sources "${_ml}/tensorflow/lite/micro/kernels/cmsis_nn/${op}.cc")
  else()
    list(APPEND _sources "${_ml}/tensorflow/lite/micro/kernels/${op}.cc")
  endif()
  list(APPEND R1_WAKE_CORE_SOURCES ${_sources})
endforeach()
file(GLOB _frontend "${_front}/*.c" "${_front}/*.cc")
list(FILTER _frontend EXCLUDE REGEX "(test|test_util|main|memmap|kiss_fft_int32|frontend_memmap)")
list(APPEND R1_WAKE_CORE_SOURCES ${_frontend})
set(R1_WAKE_INCLUDE_DIRS "${_ml}" "${R1_WAKE_WORKSPACE}/apps/math/kissfft/kissfft"
  "${R1_WAKE_WORKSPACE}/apps/math/gemmlowp/gemmlowp" "${R1_WAKE_WORKSPACE}/apps/math/ruy/ruy"
  "${R1_WAKE_WORKSPACE}/apps/system/flatbuffers/flatbuffers/include")
set(R1_WAKE_COMPILE_DEFINITIONS TF_LITE_STATIC_MEMORY TF_LITE_DISABLE_X86_NEON TF_LITE_STRIP_ERROR_STRINGS
  TFLITE_WITH_STABLE_ABI=0 TFLITE_USE_OPAQUE_DELEGATE=0 TFLITE_SINGLE_ROUNDING=0)
if(R1_WAKE_USE_CMSIS_NN)
  list(APPEND R1_WAKE_COMPILE_DEFINITIONS CMSIS_NN)
  set(_r1_nn "${R1_WAKE_WORKSPACE}/r1-ml/CMSIS-NN")
  file(GLOB_RECURSE _r1_nn_sources "${_r1_nn}/Source/*.c")
  list(APPEND R1_WAKE_CORE_SOURCES ${_r1_nn_sources})
  list(APPEND R1_WAKE_INCLUDE_DIRS "${_r1_nn}" "${_r1_nn}/Include"
    "${R1_WAKE_WORKSPACE}/r1-ml/CMSIS_5"
    "${R1_WAKE_WORKSPACE}/r1-ml/CMSIS_5/CMSIS/Core/Include")
  if(CMAKE_SYSTEM_PROCESSOR STREQUAL "arm" OR CMAKE_C_COMPILER MATCHES "arm-none-eabi")
    list(APPEND R1_WAKE_COMPILE_DEFINITIONS ARM_MATH_DSP)
  endif()
endif()
