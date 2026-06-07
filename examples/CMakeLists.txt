add_executable(quickstart quickstart.cpp)
target_link_libraries(quickstart PRIVATE zen-core zen-warnings zen-sanitize)

add_executable(heartbeat heartbeat.cpp)
target_link_libraries(heartbeat PRIVATE zen-switchboard zen-warnings zen-sanitize)
