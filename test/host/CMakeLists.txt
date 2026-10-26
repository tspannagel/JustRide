# Host unit tests for pure components:
#   cmake -S test/host -B test/host/build && cmake --build test/host/build && ctest --test-dir test/host/build
cmake_minimum_required(VERSION 3.16)
project(justride_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror)
enable_testing()

set(COMPONENTS ${CMAKE_CURRENT_SOURCE_DIR}/../../components)

add_executable(test_proto_ftms test_proto_ftms.c ${COMPONENTS}/proto_ftms/proto_ftms.c)
target_include_directories(test_proto_ftms PRIVATE ${COMPONENTS}/proto_ftms/include)
add_test(NAME proto_ftms COMMAND test_proto_ftms)
