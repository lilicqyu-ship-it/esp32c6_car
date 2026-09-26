# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(c6_car)

# ---- control page bundle: assets_src/* -> build/assets.bin ------------------------
# packed automatically as part of `idf.py build` (incremental: only re-runs when
# the page sources or the packer change).  Flashing it to the assets partition
# stays a separate step: parttool / flash.bat assets / flash.py assets.
idf_build_get_property(python_exec PYTHON)
file(GLOB C6_ASSETS_SRC ${CMAKE_CURRENT_LIST_DIR}/assets_src/*)
add_custom_command(
    OUTPUT  ${CMAKE_BINARY_DIR}/assets.bin
    COMMAND ${python_exec} ${CMAKE_CURRENT_LIST_DIR}/tools/build_assets.py
            ${CMAKE_CURRENT_LIST_DIR}/assets_src ${CMAKE_BINARY_DIR}/assets.bin
    DEPENDS ${CMAKE_CURRENT_LIST_DIR}/tools/build_assets.py ${C6_ASSETS_SRC}
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    COMMENT "Packing control page assets"
    VERBATIM)
add_custom_target(c6_assets ALL DEPENDS ${CMAKE_BINARY_DIR}/assets.bin)