# FT8 decoding core: message/QSO logic, protocol and audio workers. No UI and
# no application services; depends on liquid-dsp/ft8lib from the sysroot, QTH
# (linked by consumers) and LVGL for logging only.
add_library(ft8_core STATIC
    qso.cpp worker.c utils.c gfsk.c
    cq_scheduler.c msg_encode.c
    audio_worker.c
)

include_directories("${CMAKE_CURRENT_SOURCE_DIR}/..")

# audio_worker.c includes common/time.h, which needs POSIX declarations hidden
# by the strict -std=c11 mode (__STRICT_ANSI__).
target_compile_definitions(ft8_core PRIVATE _GNU_SOURCE)

target_link_libraries(ft8_core PUBLIC lvgl)

# FT8 TX glue. Application-level: drives the audio/radio ports and reads config.
# Depends on the core plus cfg and the legacy params module (which is compiled
# into the executable, so its symbols resolve at final link).
add_library(ft8_app STATIC
    tx_worker.c
)

target_link_libraries(ft8_app PUBLIC ft8_core cfg)
