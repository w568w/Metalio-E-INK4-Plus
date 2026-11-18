# ebook-emulator 构建（原生 SDL / Emscripten WASM）

# 本仓：tools/ebook/emulator → ../../../main
set(ESP_MAIN "${CMAKE_CURRENT_SOURCE_DIR}/../../../main" CACHE PATH "ESP32 main 源码根")

if(NOT EXISTS "${ESP_MAIN}/reader/book_session.cc")
    message(FATAL_ERROR "ESP_MAIN 无效: ${ESP_MAIN}")
endif()

set(EBOOK_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/esp_compat
    ${CMAKE_CURRENT_SOURCE_DIR}/src/font
    ${CMAKE_CURRENT_SOURCE_DIR}/src/emulator
    ${ESP_MAIN}/reader
    ${ESP_MAIN}/boards/common
    ${CMAKE_CURRENT_SOURCE_DIR}
)

set(EBOOK_READER_SRC
    ${ESP_MAIN}/reader/book_session.cc
    ${ESP_MAIN}/reader/book_session_nav.cc
    ${ESP_MAIN}/reader/book_disk_cache.cc
    ${ESP_MAIN}/reader/book_bookmarks.cc
    ${ESP_MAIN}/reader/ebook_document.cc
    ${ESP_MAIN}/reader/epub_document.cc
    ${ESP_MAIN}/reader/html_content.cc
    ${ESP_MAIN}/reader/image_geometry.cc
    ${ESP_MAIN}/reader/image_tone.cc
    ${ESP_MAIN}/reader/text_encoding.cc
    ${ESP_MAIN}/reader/txt_chapter.cc
    ${ESP_MAIN}/reader/zip_reader.cc
)

set(EBOOK_FONT_SRC
    ${ESP_MAIN}/display/font/epdfont.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/font/font_loader_emulator.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/font/fontpack_lvgl_emulator.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/font/epdfont_emulator.cc
)

set(EBOOK_EMULATOR_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/emulator/book_reader_emulator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/emulator/image_util_emulator.cc
)

function(ebook_emulator_target target_name main_src)
    add_executable(${target_name}
        ${main_src}
        ${EBOOK_EMULATOR_SRC}
        ${EBOOK_FONT_SRC}
        ${EBOOK_READER_SRC}
    )
    target_include_directories(${target_name} PRIVATE ${EBOOK_INCLUDES})
    target_compile_definitions(${target_name} PRIVATE EBOOK_EMULATOR=1)
    target_link_libraries(${target_name} lvgl_linux lvgl m z pthread)
endfunction()

# 原生 SDL 模拟器（480×800）
ebook_emulator_target(ebook_sim src/main_ebook.c)

option(APP_UI_PREVIEW "Build headless app UI checks" OFF)
if(APP_UI_PREVIEW)
    set(NOVELIA "${ESP_MAIN}/display/screen/novelia_screen")
    add_executable(novelia_preview
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/novelia/ui_preview/preview.cc" "${NOVELIA}/novelia_ui.cc"
        "${ESP_MAIN}/display/screen/settings_screen/settings_common.cc"
        "${ESP_MAIN}/display/ui_qrcode.cc" "${ESP_MAIN}/display/font/font_awesome_30_1.c"
        "${ESP_MAIN}/reader/text_encoding.cc"
        src/font/fontpack_lvgl_emulator.cc src/font/font_loader_emulator.c)
    target_compile_definitions(novelia_preview PRIVATE EBOOK_EMULATOR=1)
    target_compile_options(novelia_preview PRIVATE -UNDEBUG)
    target_include_directories(novelia_preview PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/novelia/ui_preview/stubs"
        ${EBOOK_INCLUDES} "${ESP_MAIN}" "${ESP_MAIN}/display" "${ESP_MAIN}/display/font"
        "${ESP_MAIN}/display/screen" "${NOVELIA}" /usr/include/cjson)
    target_link_libraries(novelia_preview PRIVATE lvgl m)
    set(LINUX_APP "${ESP_MAIN}/display/screen/linux_screen")
    add_executable(linux_preview
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/linux/ui_preview.cc" "${LINUX_APP}/linux_ui.cc" "${LINUX_APP}/linux_console.cc"
        "${ESP_MAIN}/display/screen/screen_keyboard.cc"
        "${ESP_MAIN}/reader/text_encoding.cc"
        "${ESP_MAIN}/display/screen/settings_screen/settings_common.cc"
        "${ESP_MAIN}/display/font/font_awesome_30_1.c"
        src/font/fontpack_lvgl_emulator.cc src/font/font_loader_emulator.c)
    target_compile_definitions(linux_preview PRIVATE EBOOK_EMULATOR=1)
    target_compile_options(linux_preview PRIVATE -UNDEBUG)
    # stubs/config.h 只提供预览用的显示尺寸，与 Novelia 无关。
    target_include_directories(linux_preview PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/novelia/ui_preview/stubs"
        ${EBOOK_INCLUDES} "${ESP_MAIN}" "${ESP_MAIN}/display" "${ESP_MAIN}/display/font"
        "${ESP_MAIN}/display/screen" "${LINUX_APP}")
    target_link_libraries(linux_preview PRIVATE lvgl m)

endif()
