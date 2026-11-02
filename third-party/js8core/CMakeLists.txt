# js8core: Qt-free JS8 modem and protocol library, vendored from
# JS8Call-improved/Android-port. See UPSTREAM.md for the pinned commit and
# the local patches.

add_library(js8core STATIC
    src/engine/engine.cpp
    src/decoder/legacy_decoder.cpp
    src/dsp/flatten.cpp
    src/dsp/resampler.cpp
    src/tx/modulator.cpp
    src/protocol/submode.cpp
    src/protocol/costas.cpp
    src/protocol/constants.cpp
    src/protocol/time_utils.cpp
    src/protocol/varicode.cpp
    src/protocol/jsc.cpp
    src/protocol/jsc_map.cpp
    src/protocol/jsc_list.cpp
)

target_compile_features(js8core PUBLIC cxx_std_20)

target_include_directories(js8core
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    # commons.h and <vendor/...> are resolved relative to this directory.
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(js8core PUBLIC fftw3f)
