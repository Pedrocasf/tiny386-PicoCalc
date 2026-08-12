# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Board selection: -DBOARD=<name> via cmake/idf.py, or BOARD env var, or default jc3248w535.
# This must be set BEFORE include(project.cmake) so ESP-IDF reads the right sdkconfig.
if(NOT DEFINED BOARD)
    if(DEFINED ENV{BOARD})
        set(BOARD $ENV{BOARD})
    else()
        set(BOARD "jc3248w535")
    endif()
endif()
# Always sync sdkconfig from the board-specific file.
# This ensures switching boards (via -DBOARD=) just works without manual cleanup.
# If you use menuconfig, save changes back to sdkconfig.${BOARD}.
set(BOARD_SDKCONFIG "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.${BOARD}")
if(EXISTS "${BOARD_SDKCONFIG}")
    file(COPY_FILE "${BOARD_SDKCONFIG}" "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig" ONLY_IF_DIFFERENT)
endif()
set(SDKCONFIG_DEFAULTS "${BOARD_SDKCONFIG}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tiny386)

# Boards with no SD card read their config from the "storage" FAT partition.
# Stage it in the build directory from esp/flash_data/ plus bios.bin/vgabios.bin
# out of the repository root, then write it as part of `idf.py flash`.
# The staging copy happens at configure time, so re-run cmake (or touch this
# file) after rebuilding the ROMs.  Drop FLASH_IN_PROJECT once the partition
# holds data you do not want every flash to overwrite.
if(BOARD STREQUAL "picocalc" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/flash_data")
    set(STORAGE_STAGE_DIR "${CMAKE_BINARY_DIR}/storage_data")
    file(MAKE_DIRECTORY "${STORAGE_STAGE_DIR}")
    file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/flash_data/"
         DESTINATION "${STORAGE_STAGE_DIR}")
    foreach(rom bios.bin vgabios.bin)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../${rom}")
            file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/../${rom}"
                 DESTINATION "${STORAGE_STAGE_DIR}")
        else()
            message(WARNING "${rom} not found in the repository root: the "
                            "storage partition will have no ${rom} and "
                            "load_rom() will abort at run time")
        endif()
    endforeach()
    fatfs_create_spiflash_image(storage "${STORAGE_STAGE_DIR}" FLASH_IN_PROJECT)
endif()
