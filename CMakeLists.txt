cmake_minimum_required(VERSION 3.23)

# Require C11 standard globally
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

project(x6100_gui)

include_directories(.)
include_directories(third-party/rapidxml)
include_directories(third-party/utf8)

if(ENABLE_TESTING)
        Include(FetchContent)

        FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.4.0 # or a later release
        )

        FetchContent_MakeAvailable(Catch2)

        add_compile_definitions(LV_DRAW_NEON=0)


        # include_directories(${CMAKE_CURRENT_SOURCE_DIR}/tests/mock_gpiod)
        FetchContent_Declare(
        aether_x6100_control
        GIT_REPOSITORY https://github.com/gdyuldin/X6100Control.git
        GIT_TAG        v0.16.1
        )
        FetchContent_MakeAvailable(aether_x6100_control)

else()
        add_compile_options(-mtune=cortex-a7 -mcpu=cortex-a7 -mfloat-abi=hard -mfpu=neon-vfpv4)
endif()

add_subdirectory(lvgl)

if(ENABLE_TESTING)
        enable_testing()
        add_subdirectory(tests)
else()
        # JS8's engine (GPL-3.0, see README "License")
        add_subdirectory(third-party/js8core)
        add_subdirectory(src)
        add_subdirectory(lv_drivers)
        add_subdirectory(sql)
        install(TARGETS ${PROJECT_NAME} DESTINATION sbin)
        install(DIRECTORY rootfs/
                DESTINATION /
                USE_SOURCE_PERMISSIONS
        )
        install(DIRECTORY images DESTINATION share/x6100)
        # Callsign -> country for the FT8/FT4 table (AD1C, country-files.com)
        install(FILES third-party/cty/cty.dat DESTINATION share/x6100)
endif()


