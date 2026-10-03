# GoogleTest suites. Each *_test.cpp file holds related tests; all go into one
# executable, and gtest_discover_tests registers each TEST() with CTest.
add_executable(cfbsim_tests
    attributes_test.cpp
    ball_flight_test.cpp
    blocking_test.cpp
    catch_test.cpp
    tackling_test.cpp
    curve_test.cpp
    defense_test.cpp
    data_dir_test.cpp
    determinism_test.cpp
    generator_test.cpp
    geometry_test.cpp
    movement_test.cpp
    parse_test.cpp
    play_test.cpp
    rng_test.cpp
    route_test.cpp
    rules_test.cpp
    game_test.cpp
    coach_test.cpp
    match_test.cpp
    special_teams_test.cpp
    version_test.cpp)

target_link_libraries(cfbsim_tests PRIVATE
    cfbsim_core
    cfbsim_options
    GTest::gtest_main)   # provides main() for the test executable

# White-box tests may include the core's internal headers (sim/src).
target_include_directories(cfbsim_tests PRIVATE ${PROJECT_SOURCE_DIR}/sim/src)

# Tests that load the real starter data find it through this path.
target_compile_definitions(cfbsim_tests PRIVATE CFBSIM_DATA_DIR="${PROJECT_SOURCE_DIR}/data")

include(GoogleTest)
gtest_discover_tests(cfbsim_tests)
