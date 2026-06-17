# gen_version.cmake — generates version.txt with build timestamp
string(TIMESTAMP BUILD_TIME "%Y%m%d-%H%M%S")
set(VERSION_FILE "${CMAKE_BINARY_DIR}/version.txt")
file(WRITE "${VERSION_FILE}" "${BUILD_TIME}\n${PROJECT_NAME}.bin\n")
message("  version.txt: ${BUILD_TIME}")