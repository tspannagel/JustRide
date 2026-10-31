# Host unit tests for pure components:
#   cmake -S test/host -B test/host/build && cmake --build test/host/build && ctest --test-dir test/host/build
cmake_minimum_required(VERSION 3.16)
project(justride_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
# float-cast-overflow is not part of GCC's "undefined" group; parsers cast doubles
add_compile_options(-Wall -Wextra -Werror -fsanitize=address,undefined,float-cast-overflow
                    -fno-sanitize-recover=all -g)
add_link_options(-fsanitize=address,undefined,float-cast-overflow)
enable_testing()

set(C ${CMAKE_CURRENT_SOURCE_DIR}/../../components)
add_compile_definitions(TOOLS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../tools"
                        FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
include_directories(${C}/evbus/include)

# jr_test(<name> <sources...>): test_<name>.c plus the component sources
function(jr_test name)
    add_executable(test_${name} test_${name}.c ${ARGN})
    target_link_libraries(test_${name} m)
    add_test(NAME ${name} COMMAND test_${name})
endfunction()

include_directories(
    ${C}/proto_ftms/include
    ${C}/workout/include
    ${C}/metrics/include
    ${C}/vshift/include
    ${C}/ride/include
    ${C}/recorder/include
    ${C}/route/include
)

jr_test(proto_ftms ${C}/proto_ftms/proto_ftms.c)
jr_test(ftms_ctl ${C}/proto_ftms/ftms_ctl.c ${C}/proto_ftms/proto_ftms.c)
jr_test(workout ${C}/workout/zwo.c ${C}/workout/workout.c)
jr_test(metrics ${C}/metrics/metrics.c)
jr_test(vshift ${C}/vshift/vshift.c)
jr_test(ride ${C}/ride/ride.c ${C}/workout/workout.c ${C}/metrics/metrics.c ${C}/vshift/vshift.c ${C}/route/route.c
    ${C}/route/ghost.c)
jr_test(rec ${C}/recorder/csv.c ${C}/recorder/fit.c ${C}/metrics/metrics.c)
include_directories(${C}/proto_zap/include ${C}/input_map/include)
jr_test(zap ${C}/proto_zap/proto_zap.c ${C}/input_map/input_map.c)
include_directories(${C}/tz/include)
jr_test(tz ${C}/tz/tz.c)
include_directories(${C}/ics/include)
jr_test(ics ${C}/ics/ics.c)
include_directories(${C}/proto_cps/include)
jr_test(cps ${C}/proto_cps/proto_cps.c)
jr_test(route ${C}/route/route.c)
jr_test(ghost ${C}/route/ghost.c)

# tools/zwo_check: the firmware's .zwo parser as a CLI (used by tools/mcp_server.py)
add_executable(zwo_check ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/zwo_check.c ${C}/workout/zwo.c ${C}/workout/workout.c)
target_link_libraries(zwo_check m)
add_test(NAME zwo_check COMMAND zwo_check ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/workouts/sweet_spot_3x10.zwo)
