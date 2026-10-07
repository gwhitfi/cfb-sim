# GoogleTest suites, in three executables by how long they take, so the
# quick ones can run on their own and a stuck test can't hold up the rest:
#
#   cfbsim_unit_tests  data, rules, geometry, the walks: seconds in all
#   cfbsim_play_tests  the play sim, many plays per test
#   cfbsim_game_tests  full games, the Match, the coach, special teams
#
# gtest_discover_tests registers each TEST() with CTest, with a per-test
# TIMEOUT (seconds) and a LABEL (unit, play, game): `ctest --preset debug -L
# unit` runs one group, and a test that hangs fails at its timeout instead of
# running forever. The timeouts are about 3x the slowest test of each group
# in the debug build (about 10x slower than release): unit tests under 12 s,
# Blocking.QbNeverThrowsToABackWhoIsBlocking 38 s, Game.BetweenPlaysIsOnlyToWatch
# 60 s. Raise a group's timeout if a new test needs more.

# One test executable: its sources, linked to the core, with the data path,
# each test registered with CTest under `label` with `timeout` s.
function(cfbsim_test_suite name label timeout)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE
        cfbsim_core
        cfbsim_options
        Threads::Threads
        GTest::gtest_main)   # provides main() for the test executable
    # White-box tests may include the core's internal headers (sim/src).
    target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/sim/src)
    # Tests that load the real starter data find it through this path.
    target_compile_definitions(${name} PRIVATE CFBSIM_DATA_DIR="${PROJECT_SOURCE_DIR}/data")
    gtest_discover_tests(${name} PROPERTIES TIMEOUT ${timeout} LABELS ${label})
endfunction()

include(GoogleTest)

# The matrix test runs cells on std::thread (pthreads on Linux).
find_package(Threads REQUIRED)

cfbsim_test_suite(cfbsim_unit_tests unit 30
    attributes_test.cpp
    ball_flight_test.cpp
    between_test.cpp
    curve_test.cpp
    data_dir_test.cpp
    determinism_test.cpp
    generator_test.cpp
    play_stats_test.cpp
    play_art_test.cpp
    geometry_test.cpp
    huddle_test.cpp
    movement_test.cpp
    parse_test.cpp
    pursuit_test.cpp
    tackle_contact_test.cpp
    rng_test.cpp
    route_test.cpp
    rules_test.cpp
    version_test.cpp)

cfbsim_test_suite(cfbsim_play_tests play 120
    blocking_test.cpp
    catch_test.cpp
    tackling_test.cpp
    defense_test.cpp
    kick_matrix_test.cpp
    matrix_test.cpp
    play_test.cpp
    shell_test.cpp
    team_builder_test.cpp)

cfbsim_test_suite(cfbsim_game_tests game 180
    game_test.cpp
    coach_test.cpp
    match_test.cpp
    special_teams_test.cpp)

# Exact pre-optimization play/tracking/game fixtures, independent of the C++
# implementation. Available in CTest when Python is installed; also runnable
# directly with scripts/check_play_baseline.py.
find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME Baseline.PlayResults
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/check_play_baseline.py
                --exe $<TARGET_FILE:cfbsim>)
    set_tests_properties(Baseline.PlayResults PROPERTIES TIMEOUT 600 LABELS "play;baseline")
endif()
