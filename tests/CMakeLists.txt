# GoogleTest suites. Each *_test.cpp file holds related tests; all go into one
# executable, and gtest_discover_tests registers each TEST() with CTest.
add_executable(cfbsim_tests
    version_test.cpp)

target_link_libraries(cfbsim_tests PRIVATE
    cfbsim_core
    cfbsim_options
    GTest::gtest_main)   # provides main() for the test executable

include(GoogleTest)
gtest_discover_tests(cfbsim_tests)
