# gen_esp_version.cmake — use PROJECT_VER from CMakeLists
set(VERSION_FILE "${BUILD_DIR}/version.txt")
file(WRITE "${VERSION_FILE}" "${PROJECT_VER}\n${PROJECT_NAME}.bin\n")
message("  ESP32 version.txt: ${PROJECT_VER}")