find_package(Threads REQUIRED)

add_library(JS8 STATIC
    resampler.cpp
    speeds.cpp
    render.cpp
    assembler.cpp
    classify.cpp
    directed.cpp
    receiver.cpp
    wav.cpp
    tx.cpp
    js8_tx.cpp
    commands.cpp
    stations.cpp
    autoreply.cpp
    qsolog.cpp
    inbox.cpp
    datafile.cpp
    alerts.cpp
    macros.cpp
    timesync.cpp
    wisdom.cpp
    cpu_fence.cpp
    history.cpp
    js8_ops.cpp
    js8_rx.cpp
    geo.cpp
    callsign_place.cpp
    js8_map.cpp
    map_render.cpp
)

# js8core needs C++20. Link it PRIVATE so the requirement stops here: the
# app only sees the C header js8_rx.h and keeps its own C++ standard.
target_link_libraries(JS8 PRIVATE js8core sqlite3 PUBLIC Threads::Threads) # sqlite3: the QSO log, read-only (the map's "worked before"), and the station history
target_include_directories(JS8 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Test-signal generator for tests and tools/js8_wavgen; not linked into the app.
add_library(JS8TestSignal STATIC testsignal.cpp)
target_link_libraries(JS8TestSignal PRIVATE js8core JS8)
target_include_directories(JS8TestSignal PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
