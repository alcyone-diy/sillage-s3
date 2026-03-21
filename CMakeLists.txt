cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tiles_prototype)

# Fix for LVGL v9.5.0 FatFS compilation error:
# The managed lvgl component is missing 'fatfs' in its requirements.
# We inject it here at the project level to ensure it survives 'fullclean'.
if(TARGET __idf_lvgl__lvgl)
    message(STATUS "Injecting fatfs dependency into lvgl__lvgl")
    target_link_libraries(__idf_lvgl__lvgl PRIVATE __idf_fatfs)
endif()
