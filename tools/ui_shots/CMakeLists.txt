# Renders the firmware's LVGL screens (main/ui.c) on the computer and saves
# them as PNGs for the manual. Needs managed_components/lvgl__lvgl, i.e. one
# `idf.py build` first. Run: tools/ui_shots/shots.sh
cmake_minimum_required(VERSION 3.16)
project(ui_shots C)
set(CMAKE_C_STANDARD 11)

set(REPO ${CMAKE_CURRENT_SOURCE_DIR}/../..)
# -DLVGL=<dir> to use an LVGL 9.3 checkout instead (e.g. without ESP-IDF)
if(NOT LVGL)
    set(LVGL ${REPO}/managed_components/lvgl__lvgl)
endif()
if(NOT EXISTS ${LVGL}/lvgl.h)
    message(FATAL_ERROR "LVGL not found: run `idf.py build` once so managed_components/ is filled")
endif()

file(GLOB_RECURSE LVGL_SRC ${LVGL}/src/*.c)
add_library(lvgl STATIC ${LVGL_SRC})
target_include_directories(lvgl PUBLIC ${LVGL} ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE)
target_compile_options(lvgl PRIVATE -w)

add_executable(ui_shots shots.c ${REPO}/main/font_digits_100.c ${REPO}/main/font_icons_18.c
    ${REPO}/components/workout/zwo.c ${REPO}/components/workout/workout.c ${REPO}/components/tz/tz.c)
# stubs/ first: it replaces the ESP-IDF headers ui.c includes
target_include_directories(ui_shots PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs ${REPO}/main
    ${REPO}/components/evbus/include ${REPO}/components/ride/include ${REPO}/components/metrics/include
    ${REPO}/components/vshift/include ${REPO}/components/workout/include ${REPO}/components/settings/include
    ${REPO}/components/web/include ${REPO}/components/input_map/include ${REPO}/components/tz/include)
# The workouts screen lists BSP_SD_MOUNT/workouts: use the sample workouts
target_compile_definitions(ui_shots PRIVATE BSP_SD_MOUNT="${REPO}/tools")
target_link_libraries(ui_shots lvgl m)
