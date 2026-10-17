cmake_minimum_required(VERSION 3.23)

# add_executable(tests test.cpp)
# target_link_libraries(tests PRIVATE Catch2::Catch2WithMain)

add_compile_options(-fsanitize=address -fsanitize=undefined  -fno-omit-frame-pointer -fno-sanitize-recover)
add_link_options(-fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer -fno-sanitize-recover -static-libasan -static-libubsan)


add_subdirectory(../src/qth ${CMAKE_BINARY_DIR}/src/qth)
add_executable(test_qth test_qth.cpp)
target_link_libraries(test_qth PRIVATE QTH Catch2::Catch2WithMain)
add_test(NAME test_qth COMMAND $<TARGET_FILE:test_qth> --colour-mode=ansi )

add_subdirectory(../src/ft8 ${CMAKE_BINARY_DIR}/src/ft8)
add_executable(test_ft8_qso test_ft8_qso.cpp)
target_link_libraries(test_ft8_qso PRIVATE ft8_core QTH aether_x6100_control Catch2::Catch2WithMain)
add_test(NAME test_ft8_qso COMMAND $<TARGET_FILE:test_ft8_qso> --colour-mode=ansi )


# test_js8_db: params.db for every kind of card (src/cfg/js8_db.c with
# migrations.c; plain C, sqlite3 only). Its own binary: it stands in for
# db.cpp's cfg_db_get(), which test_cfg links.
add_executable(test_js8_db test_js8_db.cpp
    ${PROJECT_SOURCE_DIR}/src/cfg/js8_db.c ${PROJECT_SOURCE_DIR}/src/cfg/migrations.c)
target_include_directories(test_js8_db PRIVATE ${PROJECT_SOURCE_DIR}/src/cfg)
target_link_libraries(test_js8_db PRIVATE Catch2::Catch2WithMain sqlite3)
add_test(NAME test_js8_db COMMAND $<TARGET_FILE:test_js8_db> --colour-mode=ansi)

add_subdirectory(cfg)

add_subdirectory(../src/cw_decoder ${CMAKE_BINARY_DIR}/src/cw_decoder)
add_subdirectory(cw_decoder)

add_subdirectory(../src/cat ${CMAKE_BINARY_DIR}/src/cat)
add_executable(test_civ test_civ.cpp)
target_include_directories(test_civ PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(test_civ PRIVATE cat_core Catch2::Catch2WithMain)
add_test(NAME test_civ COMMAND $<TARGET_FILE:test_civ> --colour-mode=ansi)

# test_tx_level: the digital modes' drive level (src/tx_level.c, plain C,
# no radio) against a model radio.
add_executable(test_tx_level test_tx_level.cpp ${PROJECT_SOURCE_DIR}/src/tx_level.c)
target_include_directories(test_tx_level PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(test_tx_level PRIVATE Catch2::Catch2WithMain)
add_test(NAME test_tx_level COMMAND $<TARGET_FILE:test_tx_level> --colour-mode=ansi)
