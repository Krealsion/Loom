add_executable(quickstart quickstart.cpp)
target_link_libraries(quickstart PRIVATE loom zen-warnings zen-sanitize)

add_executable(heartbeat heartbeat.cpp)
target_link_libraries(heartbeat PRIVATE zen-switchboard zen-warnings zen-sanitize)

add_executable(heartbeat_woven heartbeat_woven.cpp)
target_link_libraries(heartbeat_woven PRIVATE zen-switchboard zen-warnings zen-sanitize)

# The compiled twin of docs/guides/messaging.md — answers, immediate and deferred.
add_executable(answering answering.cpp)
target_link_libraries(answering PRIVATE zen-switchboard zen-warnings zen-sanitize)
