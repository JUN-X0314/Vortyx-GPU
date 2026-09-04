add_executable(test_version test_version.cpp)
target_include_directories(test_version PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME VersionTest COMMAND test_version)
