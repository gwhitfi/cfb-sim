# The WebSocket host for the viewer: runs matches through the C API.
add_executable(cfbsim_server main.cpp)
set_target_properties(cfbsim_server PROPERTIES OUTPUT_NAME cfbsim-server)
target_link_libraries(cfbsim_server PRIVATE cfbsim_core ixwebsocket cfbsim_options)
