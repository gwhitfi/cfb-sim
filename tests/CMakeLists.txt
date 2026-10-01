# GoogleTest suites. Each *_test.cpp file holds related tests; all go into one
# executable, and gtest_discover_tests registers each TEST() with CTest.
add_executable(cfbsim_tests
    attributes_test.cpp
    data_dir_test.cpp
    generator_test.cpp
    parse_test.cpp
    rng_test.cpp
    version_test.cpp)

target_link_libraries(cfbsim_tests PRIVATE
    cfbsim_core
    cfbsim_options
    GTest::gtest_main)   # provides main() for the test executable

# Tests that load the real starter data find it through this path.
target_compile_definitions(cfbsim_tests PRIVATE CFBSIM_DATA_DIR="${PROJECT_SOURCE_DIR}/data")

include(GoogleTest)
gtest_discover_tests(cfbsim_tests)
