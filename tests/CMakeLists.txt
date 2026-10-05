add_executable(emeron_tests
    TestMain.cpp
    test_string_util.cpp
    test_ini_file.cpp
    test_adb_parsers.cpp
    test_frame_stats.cpp
    test_logcat.cpp
    test_collectors.cpp
    test_file_ops.cpp
    test_subprocess.cpp
    test_theme.cpp
    test_fonts.cpp
    test_icons.cpp
    test_crash_handler.cpp
    test_app_inspector.cpp
    test_apk_analysis.cpp
    test_table_settings.cpp

    # Theme lives in the GUI target, but its palettes are pure data and the
    # contrast of a hand-written light theme is exactly the thing worth
    # asserting. ImGui and ImPlot contexts work headlessly, so no window is
    # needed to test it.
    ${CMAKE_SOURCE_DIR}/src/app/Theme.cpp
)

target_link_libraries(emeron_tests PRIVATE emeron_core emeron_assets implot::implot)
target_compile_definitions(emeron_tests PRIVATE
    EMERON_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
    EMERON_FA_HEADER_PATH="${CMAKE_SOURCE_DIR}/vendor/IconFontCppHeaders/IconsFontAwesome6.h")
target_include_directories(emeron_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
emeron_set_warnings(emeron_tests)
emeron_set_sanitizers(emeron_tests)

add_test(NAME emeron_unit COMMAND emeron_tests)
set_tests_properties(emeron_unit PROPERTIES TIMEOUT 120)
