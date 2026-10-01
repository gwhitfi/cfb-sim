# The engine-independent sim core, built as a static library.
add_library(cfbsim_core STATIC
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
    src/data/save.cpp
    src/gen/roster_gen.cpp)

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
