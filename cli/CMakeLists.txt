# The command-line tool. Builds an executable named "cfbsim".
add_executable(cfbsim main.cpp)

target_link_libraries(cfbsim PRIVATE cfbsim_core cfbsim_options)
