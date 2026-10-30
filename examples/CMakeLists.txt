add_executable(quickstart quickstart.cpp)
target_link_libraries(quickstart PRIVATE loom zen-warnings zen-sanitize)

add_executable(heartbeat heartbeat.cpp)
target_link_libraries(heartbeat PRIVATE zen-switchboard zen-warnings zen-sanitize)

add_executable(heartbeat_woven heartbeat_woven.cpp)
target_link_libraries(heartbeat_woven PRIVATE zen-switchboard zen-warnings zen-sanitize)

# The compiled twin of docs/guides/messaging.md — answers, immediate and deferred.
add_executable(answering answering.cpp)
target_link_libraries(answering PRIVATE zen-switchboard zen-warnings zen-sanitize)

# The compiled twin of docs/guides/message-journey.md: each journey a message takes, and the
# bus's own record of it. The `message_journey` CTest entry compares its output with the page's.
# The link journey mounts the supplied host's own link (src/host/link.hpp, from zen-host-boot).
add_executable(message_journey message_journey.cpp)
target_link_libraries(message_journey PRIVATE zen-bridge zen-host-boot zen-switchboard
                                              zen-warnings zen-sanitize)
