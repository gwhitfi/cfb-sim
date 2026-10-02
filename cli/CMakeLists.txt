# The command-line tool. Builds an executable named "cfbsim".
add_executable(cfbsim main.cpp)

target_link_libraries(cfbsim PRIVATE cfbsim_core cfbsim_options)

# std::thread (the batch command) needs the platform's thread library
# (pthreads on Linux); CMake's Threads package finds it.
find_package(Threads REQUIRED)
target_link_libraries(cfbsim PRIVATE Threads::Threads)
