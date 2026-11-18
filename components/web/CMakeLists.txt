idf_component_register(
    SRCS "web.c" "www_install.c"
    INCLUDE_DIRS "include"
    REQUIRES settings
    PRIV_REQUIRES bsp_s3lcd4 workout route ics wwwpack esp_wifi esp_netif lwip esp_event esp_http_server app_update esp_timer esp_app_format bootloader_support json mbedtls
)

# The web app inside the firmware (www_install.c): tools/pack_www.py, rebuilt when a file changes
idf_build_get_property(project_dir PROJECT_DIR)
idf_build_get_property(python PYTHON)
set(www_pack ${CMAKE_CURRENT_BINARY_DIR}/www.pack)
file(GLOB_RECURSE www_src CONFIGURE_DEPENDS ${project_dir}/www/stats/*)
list(FILTER www_src EXCLUDE REGEX "/www/stats/vendor/duckdb")
file(GLOB www_docs CONFIGURE_DEPENDS ${project_dir}/docs/manual.html ${project_dir}/docs/img/*.png)
add_custom_command(OUTPUT ${www_pack}
    COMMAND ${python} ${project_dir}/tools/pack_www.py ${project_dir} ${www_pack}
    DEPENDS ${www_src} ${www_docs} ${project_dir}/tools/pack_www.py
    VERBATIM)
add_custom_target(www_pack DEPENDS ${www_pack})
add_dependencies(${COMPONENT_LIB} www_pack)
target_add_binary_data(${COMPONENT_LIB} ${www_pack} BINARY)
