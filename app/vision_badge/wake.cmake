# SPDX-License-Identifier: Apache-2.0
find_package(Python3 COMPONENTS Interpreter REQUIRED)
get_filename_component(_r1_wake_app "${CMAKE_CURRENT_LIST_DIR}" REALPATH)
get_filename_component(_r1_wake_repo "${_r1_wake_app}/../.." ABSOLUTE)
get_filename_component(_r1_wake_apps "${NUTTX_APPS_DIR}" REALPATH)
get_filename_component(R1_WAKE_WORKSPACE "${_r1_wake_apps}" DIRECTORY)
set(_r1_wake_model "${CMAKE_CURRENT_BINARY_DIR}/r1-wake-model.c")
file(READ "${_r1_wake_repo}/tools/bk7258/wake.json" _r1_wake_lock)
string(JSON _r1_wake_asset_dir GET "${_r1_wake_lock}" candidate_model path)
string(JSON _r1_wake_asset_name GET "${_r1_wake_lock}" candidate_model model)
# The verified model is generated at configure time. An emitter/lock change
# must invalidate that step rather than link the previous raw storage blob.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${_r1_wake_repo}/tools/wake_dependency.py"
  "${_r1_wake_repo}/tools/bk7258/wake.json"
  "${R1_WAKE_WORKSPACE}/${_r1_wake_asset_dir}/${_r1_wake_asset_name}")
execute_process(COMMAND ${Python3_EXECUTABLE} "${_r1_wake_repo}/tools/wake_dependency.py"
  --workspace "${R1_WAKE_WORKSPACE}" --emit-model "${_r1_wake_model}"
  RESULT_VARIABLE _r1_wake_verified)
if(NOT _r1_wake_verified EQUAL 0)
  message(FATAL_ERROR "R1 wake input verification failed; restore exact reviewed dependencies")
endif()
include("${_r1_wake_repo}/tools/bk7258/wake_sources.cmake")
nuttx_add_library(r1_wake_ml STATIC ${R1_WAKE_CORE_SOURCES})
target_include_directories(r1_wake_ml PUBLIC ${R1_WAKE_INCLUDE_DIRS})
target_compile_definitions(r1_wake_ml PUBLIC ${R1_WAKE_COMPILE_DEFINITIONS})
target_compile_options(r1_wake_ml PRIVATE -Os -Wno-shadow -Wno-undef
  $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions> $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>)
target_sources(apps_vision_badge PRIVATE "${_r1_wake_model}")
target_link_libraries(apps_vision_badge PRIVATE r1_wake_ml)
target_compile_options(apps_vision_badge PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions> $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>)
