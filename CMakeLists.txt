cmake_minimum_required(VERSION 3.21)
project(neurosuite-nwb
    VERSION 0.1.0
    DESCRIPTION "Plugin that lets NeuroScope open Neurodata Without Borders (NWB) files"
    LANGUAGES C CXX)

################################################################################
# Options
################################################################################
option(NEUROSUITE_NWB_BUILD_TESTS "Build the unit tests" ${PROJECT_IS_TOP_LEVEL})
option(NEUROSUITE_NWB_BUILD_TOOLS "Build the nwbinfo command-line tool" ${PROJECT_IS_TOP_LEVEL})
option(NEUROSUITE_NWB_INSTALL "Generate the install target" ${PROJECT_IS_TOP_LEVEL})
option(NEUROSUITE_NWB_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(NEUROSUITE_NWB_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
endif()

################################################################################
# Dependencies
################################################################################
# aqnwb brings in the HDF5 C and C++ libraries.
find_package(aqnwb 0.4 CONFIG REQUIRED)

# The NeuroScope plugin interface; set CMAKE_PREFIX_PATH to the installation prefix of NeuroScope if
# CMake does not find it.
find_package(NeuroScopePlugin 3 REQUIRED)

################################################################################
# Compiler settings shared by the targets of this project
################################################################################
function(nwb_set_options target)
    target_compile_features(${target} PRIVATE cxx_std_17)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8 /permissive-)
        if(NEUROSUITE_NWB_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
        if(NEUROSUITE_NWB_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
    if(NEUROSUITE_NWB_SANITIZE)
        if(MSVC)
            message(FATAL_ERROR "NEUROSUITE_NWB_SANITIZE is not supported with MSVC")
        endif()
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
endfunction()

################################################################################
# NWB reader and plugin
################################################################################
# The reader knows NWB but not NeuroScope, so that other programs of the suite can use it too.
add_library(nwbreader STATIC src/nwbreader.cpp src/nwbreader.hpp)
target_include_directories(nwbreader PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(nwbreader PRIVATE aqnwb::aqnwb)
set_target_properties(nwbreader PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
nwb_set_options(nwbreader)

# A single file that contains aqnwb, HDF5 and the C++ runtime.
neuroscope_add_plugin(neuroscope_nwb src/neuroscope_plugin.cpp)
target_link_libraries(neuroscope_nwb PRIVATE nwbreader)
nwb_set_options(neuroscope_nwb)

if(NEUROSUITE_NWB_BUILD_TOOLS)
    add_executable(nwbinfo tools/nwbinfo.cpp)
    target_link_libraries(nwbinfo PRIVATE nwbreader)
    nwb_set_options(nwbinfo)
endif()

if(NEUROSUITE_NWB_INSTALL)
    # The plugin directory is relative to the prefix NeuroScope is installed in.
    install(TARGETS neuroscope_nwb
        LIBRARY DESTINATION ${NEUROSCOPE_PLUGIN_INSTALL_DIR}
        RUNTIME DESTINATION ${NEUROSCOPE_PLUGIN_INSTALL_DIR})
endif()

################################################################################
# Tests
################################################################################
if(NEUROSUITE_NWB_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
