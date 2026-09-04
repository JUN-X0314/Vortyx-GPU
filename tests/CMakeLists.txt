add_executable(test_version test_version.cpp)
target_link_libraries(test_version PRIVATE vortyx_core)
add_test(NAME VersionTest COMMAND test_version)

add_executable(test_logger test_logger.cpp)
target_link_libraries(test_logger PRIVATE vortyx_core)
add_test(NAME LoggerTest COMMAND test_logger)
