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

if(BOARD STREQUAL "picocalc")
    # The ROMs go into partitions of their own rather than onto a filesystem:
    # load_rom() reads a name without a leading '/' straight out of flash with
    # esp_partition_read(), so `bios = bios.bin` in the ini needs nothing
    # mounted and keeps working while an SD card holds the disk images.
    # Offsets come from partitions.csv so the two cannot drift apart.
    foreach(rom bios.bin vgabios.bin)
        set(rom_path "${CMAKE_CURRENT_SOURCE_DIR}/../${rom}")
        if(EXISTS "${rom_path}")
            partition_table_get_partition_info(rom_offset
                "--partition-name ${rom}" "offset")
            esptool_py_flash_target_image(flash "${rom}" "${rom_offset}"
                "${rom_path}")
        else()
            message(WARNING "${rom} not found in the repository root: its "
                            "partition will stay empty and load_rom() will "
                            "abort at run time")
        endif()
    endforeach()

    # tiny386.ini (and anything else dropped in esp/flash_data/) goes into the
    # "storage" FAT partition, which storage.c mounts as /spiflash.  Drop
    # FLASH_IN_PROJECT once that partition holds data worth keeping across a
    # flash.
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/flash_data")
        fatfs_create_spiflash_image(storage flash_data FLASH_IN_PROJECT)
    endif()
endif()
