cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME OTAESP32STM32)
set(PROJECT_VER "2.0.0")

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here
    ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_uart.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_spi.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_tim.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_tim_ex.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_i2c.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_i2c_ex.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/ota_receiver.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/i2c.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/oled.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/oled_data.c
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    VECT_TAB_OFFSET=0x8000U
)
# Suppress warning-as-error for external font data
target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE -Wno-missing-braces)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)
target_link_options(${CMAKE_PROJECT_NAME} PRIVATE
    -T "${CMAKE_CURRENT_SOURCE_DIR}/STM32F407XX_FLASH.ld"
)
# Auto-generate .bin after build
add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${CMAKE_PROJECT_NAME}> ${CMAKE_PROJECT_NAME}.bin
    COMMAND ${CMAKE_COMMAND} -E echo "Generating version.txt with timestamp..."
    COMMAND ${CMAKE_COMMAND} -DPROJECT_NAME=${CMAKE_PROJECT_NAME} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_version.cmake
    COMMENT "Generating ${CMAKE_PROJECT_NAME}.bin + version.txt"
)

# ---- Bootloader target (separate executable, uses bootloader.ld) ----
add_executable(bootloader
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/bootloader_main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/bootloader.c
    ${CMAKE_CURRENT_SOURCE_DIR}/startup_stm32f407xx.s
)
target_include_directories(bootloader PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/STM32F4xx_HAL_Driver/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/STM32F4xx_HAL_Driver/Inc/Legacy
    ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/CMSIS/Device/ST/STM32F4xx/Include
    ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/CMSIS/Include
)
target_compile_definitions(bootloader PRIVATE
    USE_HAL_DRIVER
    STM32F407xx
    BOOTLOADER_BUILD
    $<$<CONFIG:Debug>:DEBUG>
)
target_link_options(bootloader PRIVATE
    -T "${CMAKE_CURRENT_SOURCE_DIR}/bootloader.ld"
    -Wl,-Map=bootloader.map
    -Wl,--gc-sections
    -Wl,--print-memory-usage
)
# Bootloader reuses the same HAL driver sources as the main app
# (linked as object files from the stm32cubemx subdirectory)
target_link_libraries(bootloader
    STM32_Drivers
    m
)
# Auto-generate bootloader.bin after build
add_custom_command(TARGET bootloader POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:bootloader> bootloader.bin
    COMMENT "Generating bootloader.bin"
)

# ---- Test target (bare minimum, no bootloader) ----
add_executable(test_blinky
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/test_blinky.c
    ${CMAKE_CURRENT_SOURCE_DIR}/startup_stm32f407xx.s
)
target_include_directories(test_blinky PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/STM32F4xx_HAL_Driver/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/CMSIS/Device/ST/STM32F4xx/Include
    ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/CMSIS/Include
)
target_compile_definitions(test_blinky PRIVATE STM32F407xx)
target_link_options(test_blinky PRIVATE
    -T "${CMAKE_CURRENT_SOURCE_DIR}/bootloader.ld"
    -Wl,-Map=test_blinky.map
    -Wl,--gc-sections
    -Wl,--print-memory-usage
)
target_link_libraries(test_blinky m)
add_custom_command(TARGET test_blinky POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:test_blinky> test_blinky.bin
    COMMENT "Generating test_blinky.bin"
)
