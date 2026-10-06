# The engine-independent sim core, built as a static library.
add_library(cfbsim_core STATIC
    src/agents/ball_carrier.cpp
    src/agents/defense.cpp
    src/agents/blocker.cpp
    src/agents/quarterback.cpp
    src/agents/route_runner.cpp
    src/core/curve.cpp
    src/core/rng.cpp
    src/core/version.cpp
    src/data/json_reader.cpp
    src/data/load_dir.cpp
    src/data/model.cpp
    src/data/parse_common.cpp
    src/data/parse_formation.cpp
    src/data/parse_library.cpp
    src/data/parse_play.cpp
    src/data/parse_team.cpp
    src/data/parse_tuning.cpp
    src/data/parse_rules.cpp
    src/data/parse_targets.cpp
    src/data/save.cpp
    src/drills/drills.cpp
    src/field/geometry.cpp
    src/gen/roster_gen.cpp
    src/gen/team_builder.cpp
    src/gen/team_rating.cpp
    src/interaction/catch.cpp
    src/log/tracking.cpp
    src/physics/ball_flight.cpp
    src/physics/movement.cpp
    src/play/engagements.cpp
    src/play/lanes.cpp
    src/play/tackles.cpp
    src/play/fouls.cpp
    src/play/interlude.cpp
    src/play/kick_sim.cpp
    src/agents/special_teams.cpp
    src/rules/enforce.cpp
    src/rules/clock.cpp
    src/rules/lineup.cpp
    src/rules/game.cpp
    src/rules/box_score.cpp
    src/coach/auto_coach.cpp
    src/api/match.cpp
    src/api/sim_api.cpp
    src/play/openness.cpp
    src/play/play_sim.cpp
    src/play/kick_matrix.cpp
    src/play/kick_stats.cpp
    src/play/matrix.cpp
    src/play/play_stats.cpp
    src/play/routes.cpp
    src/world/agent.cpp
    src/world/world.cpp)

# PUBLIC: anything that links cfbsim_core (the CLI, tests) also gets this
# include path, so it can write #include "cfbsim/version.h".
# PRIVATE src: internal headers like "data/json_reader.h" stay internal.
target_include_directories(cfbsim_core
    PUBLIC  include
    PRIVATE src)

target_link_libraries(cfbsim_core
    PUBLIC  nlohmann_json::nlohmann_json
    PRIVATE cfbsim_options)

# Bake the project version from CMakeLists.txt into the code.
target_compile_definitions(cfbsim_core PRIVATE CFBSIM_VERSION="${PROJECT_VERSION}")

# A Match runs the game on its own thread (cfbsim/api/match.h).
find_package(Threads REQUIRED)
target_link_libraries(cfbsim_core PUBLIC Threads::Threads)
