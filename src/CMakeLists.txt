add_executable(${PROJECT_NAME})

# Regression gate: an implicit function declaration is latent UB (the compiler
# assumes int() and default argument promotions), so treat it as an error for
# the application sources only. External libraries/submodules are not affected.
target_compile_options(${PROJECT_NAME} PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-Werror=implicit-function-declaration>)

# Expose POSIX declarations (clock_gettime, CLOCK_MONOTONIC, ...) hidden by the
# strict -std=c11 mode (__STRICT_ANSI__). _GNU_SOURCE also keeps the glibc
# default extensions (M_PI, usleep, ...) that _POSIX_C_SOURCE alone disables.
# Must be defined for every translation unit before any system header, hence a
# target-level definition.
target_compile_definitions(${PROJECT_NAME} PRIVATE _GNU_SOURCE)

target_sources(${PROJECT_NAME} PUBLIC
    diag_log.c
    main.c main_screen.c
    styles.c spectrum.c radio.c dsp.cpp util.cpp
    app_ports.cpp
    waterfall.c rotary.cpp keyboard.c kbd_rollover.c encoder.c
    events.c msg.c msg_tiny.c keypad.c
    hkey.c clock.c
    meter.c band_info.cpp tx_info.c tx_level.c tx_player.c
    audio.c mfk.cpp cw.cpp panel.cpp
    rtty.c screenshot.c display.cpp gps.c
    ui_list.c app_info.c
    dialog.c dialog_swrscan.c
    dxcc.c
    dialog_ft8.c dialog_freq.c dialog_gps.c dialog_msg_cw.c
    ft8_ui/table_view.c
    ft8_udp.c
    dialog_msg_voice.c dialog_recorder.c dialog_qth.c dialog_callsign.c
    textarea_window.c cw_encoder.c buttons.cpp vol.cpp recorder.c
    voice.cpp cw_tune_ui.c adif.c qso_log.c scheduler.cpp
    format.c msg_cw_store.c
    dialog_wifi.c wifi.cpp controls.cpp usb_devices.cpp
    bt_audio.c
    knobs.cpp lock_manager.c indicators.cpp freq_info.cpp
    dialog_bt.c bt_ctl.c bt_dev.c bt_spp.c
    # The apps on their way in: each one's sources below its name
    # WSPR
    # JS8
    dialog_js8.c js8_wf.c js8_clock.c
    # PSK31
    # Olivia
    # WeFax
    # SSTV
)

add_subdirectory(fonts)
add_subdirectory(ft8)
add_subdirectory(widgets)
add_subdirectory(qth)
add_subdirectory(cfg)
add_subdirectory(cat)
add_subdirectory(settings)
add_subdirectory(cw_decoder)
add_subdirectory(js8)

include_directories(utf8)
include_directories(${CMAKE_SYSROOT}/usr/include/RHVoice/)
include_directories(${CMAKE_SYSROOT}/usr/include/ft8lib/)

set(THREADS_PREFER_PTHREAD_FLAG ON)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(deps REQUIRED IMPORTED_TARGET
    libnm dbus-glib-1 sqlite3 libpng libgps sndfile libudev libpulse libpulse-simple alsa bluez
)


target_link_libraries(${PROJECT_NAME} PRIVATE
PkgConfig::deps
ft8_core ft8_app QTH
cfg
cat_core cat_transport
cat_lan
Threads::Threads
lvgl lvgl::drivers
aether_x6100_control
liquid
RHVoice RHVoice_core RHVoice_audio
ft8
cw_decoder
JS8
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(${PROJECT_NAME} PRIVATE -g -fno-omit-frame-pointer -fasynchronous-unwind-tables)
    target_link_options(${PROJECT_NAME} PRIVATE -g -rdynamic)
endif()

# target_compile_options(${PROJECT_NAME} PRIVATE -g -fsanitize=address -fsanitize=undefined  -fno-omit-frame-pointer -fno-sanitize-recover)
# target_link_options(${PROJECT_NAME} PRIVATE -g -fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer -fno-sanitize-recover -static-libasan -static-libubsan)
