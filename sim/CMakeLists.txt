# The engine-independent sim core, built as a static library.
add_library(cfbsim_core STATIC
    src/core/version.cpp)

# PUBLIC: anything that links cfbsim_core (the CLI, tests) also gets this
# include path, so it can write #include "cfbsim/version.h".
target_include_directories(cfbsim_core
    PUBLIC  include
    PRIVATE src)

target_link_libraries(cfbsim_core
    PUBLIC  nlohmann_json::nlohmann_json
    PRIVATE cfbsim_options)

# Bake the project version from CMakeLists.txt into the code.
target_compile_definitions(cfbsim_core PRIVATE CFBSIM_VERSION="${PROJECT_VERSION}")
