# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
string(TIMESTAMP BUILD_TS "%Y%m%d-%H%M%S")
set(PROJECT_VER "${BUILD_TS}")
project(ESPOTA)

# Post-build: generate version.txt with timestamp
add_custom_command(TARGET ${CMAKE_PROJECT_NAME}.elf POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo "Generating version.txt..."
    COMMAND ${CMAKE_COMMAND} -DPROJECT_NAME=${CMAKE_PROJECT_NAME} -DPROJECT_VER=${PROJECT_VER} -DBUILD_DIR=${CMAKE_BINARY_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/gen_esp_version.cmake
    COMMENT "ESP32 version file generated"
)
