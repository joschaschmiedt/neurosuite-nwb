find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

# Optional: a directory of NWB files from DANDI (see data/nwb/README.md in the neurosuite) that the
# tests open and read from, without knowing their values.
set(NEUROSUITE_NWB_EXTRA_TEST_DATA "" CACHE PATH "Directory with further NWB files to open in the tests")

# Loads the NeuroScope plugin like NeuroScope does and reads files through its C interface.
add_executable(neuroscope_nwb_tests test_neuroscope_plugin.cpp)
target_link_libraries(neuroscope_nwb_tests PRIVATE NeuroScope::plugin GTest::gtest_main ${CMAKE_DL_LIBS})
nwb_set_options(neuroscope_nwb_tests)
target_compile_definitions(neuroscope_nwb_tests PRIVATE
    NWB_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
    NWB_EXTRA_TEST_DATA_DIR="${NEUROSUITE_NWB_EXTRA_TEST_DATA}"
    NWB_NEUROSCOPE_PLUGIN="$<TARGET_FILE:neuroscope_nwb>")
add_dependencies(neuroscope_nwb_tests neuroscope_nwb)
gtest_discover_tests(neuroscope_nwb_tests DISCOVERY_MODE PRE_TEST)
