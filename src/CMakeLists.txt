# CO2 Display Project - CMakeLists.txt
#
# This registers all source files in src/ and subdirectories

FILE(GLOB_RECURSE app_sources 
    ${CMAKE_SOURCE_DIR}/src/*.c
)

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS 
        "${CMAKE_SOURCE_DIR}/src"
        "${CMAKE_SOURCE_DIR}/src/drivers"
        "${CMAKE_SOURCE_DIR}/src/ui"
        "${CMAKE_SOURCE_DIR}/src/data"
        "${CMAKE_SOURCE_DIR}/src/ble"
    REQUIRES 
        driver
        esp_timer
        nvs_flash
    PRIV_REQUIRES
        bt
)
