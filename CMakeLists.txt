# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# The preferred-codec API and the internal SBC patch are verified together.
# Keep one exact SDK revision shared by local builds and CI.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/.idf-version" MAKE2HEAR_IDF_COMMIT LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/.idf-version")
execute_process(COMMAND git -C "$ENV{IDF_PATH}" rev-parse HEAD
                OUTPUT_VARIABLE make2hear_actual_idf OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE make2hear_git_result ERROR_QUIET)
if(NOT make2hear_git_result EQUAL 0 OR NOT make2hear_actual_idf STREQUAL MAKE2HEAR_IDF_COMMIT)
    message(FATAL_ERROR "Make2Hear requires ESP-IDF commit ${MAKE2HEAR_IDF_COMMIT}. See README.md installation steps. Both latency modes use this SDK.")
endif()
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)
set(PROJECT_VER "0.1.0-dev")
project(make2hear)
if(NOT IDF_TARGET STREQUAL "esp32")
    message(FATAL_ERROR "Make2Hear currently supports the classic ESP32 target only.")
endif()

# Generate a project-only copy of the SDK media source with shorter batching.
include(cmake/a2dp_low_latency.cmake)
