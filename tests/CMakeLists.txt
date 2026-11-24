cmake_minimum_required(VERSION 3.24)
project(metalio_host_tests C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# 主机测试把 SD 挂载点指向临时目录，由调用方传入。
set(TEST_SD_ROOT "/sdcard" CACHE PATH "主机测试使用的 SD 根目录")
# 首次配置会下载并构建 Mbed TLS；也可以指向本地检出：
#   cmake -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/path/to/mbedtls
set(MBEDTLS_TAG "v3.6.2" CACHE STRING "Mbed TLS 版本")

get_filename_component(REPO "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(MAIN "${REPO}/main")
set(WEB_NOVEL "${MAIN}/display/screen/web_novel_screen")
set(STORAGE "${MAIN}/boards/common/storage")
set(PROXY "${REPO}/components/proxy_transport")
set(ESP_COMPAT "${REPO}/tools/ebook/emulator/src/esp_compat")
set(WARNINGS -Wall -Wextra -Werror -Wno-missing-field-initializers)
set(SD_DEFINES SD_MOUNT_POINT="${TEST_SD_ROOT}" SD_APP_ROOT="${TEST_SD_ROOT}/metalio/e-ink")

find_package(ZLIB REQUIRED)
find_package(cJSON CONFIG QUIET)
if(NOT cJSON_FOUND)
    find_library(CJSON_LIBRARY NAMES cjson REQUIRED)
    add_library(cjson UNKNOWN IMPORTED)
    set_target_properties(cjson PROPERTIES IMPORTED_LOCATION "${CJSON_LIBRARY}")
endif()
# 有的发行版把 cJSON.h 装在 cjson/ 子目录下，头文件按 <cJSON.h> 引用。
find_path(CJSON_HEADER_DIR NAMES cJSON.h PATH_SUFFIXES cjson REQUIRED)

find_package(MbedTLS 3 CONFIG QUIET)
if(MbedTLS_FOUND)
    set(MBEDTLS_LIBS MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
else()
    include(FetchContent)
    set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(mbedtls
        GIT_REPOSITORY https://github.com/Mbed-TLS/mbedtls.git
        GIT_TAG ${MBEDTLS_TAG}
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(mbedtls)
    set(MBEDTLS_LIBS mbedtls mbedx509 mbedcrypto)
    # GCC 15 会说 mbedtls 3.6 的字符串数组初始化截断，而它自己开了 -Werror。
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND CMAKE_C_COMPILER_VERSION VERSION_GREATER_EQUAL 15)
        foreach(library ${MBEDTLS_LIBS})
            target_compile_options(${library} PRIVATE -Wno-error=unterminated-string-initialization)
        endforeach()
    endif()
endif()

add_library(host_env INTERFACE)
target_include_directories(host_env INTERFACE "${CJSON_HEADER_DIR}")
target_link_libraries(host_env INTERFACE cjson ZLIB::ZLIB ${MBEDTLS_LIBS})

# 模型与 EPUB 打包
add_executable(model_test novelia/model_test.cc
    "${WEB_NOVEL}/web_novel_model.cc" "${WEB_NOVEL}/web_novel_epub.cc"
    "${WEB_NOVEL}/web_novel_service.cc" "${WEB_NOVEL}/novelia_site.cc"
    "${WEB_NOVEL}/novalpie_chapter.cc" "${STORAGE}/sd_file.cc"
    "${PROXY}/transport.cc" "${PROXY}/tls_stream.cc" "${PROXY}/trojan.cc")
target_include_directories(model_test PRIVATE "${WEB_NOVEL}" "${MAIN}" "${STORAGE}" "${PROXY}/include" "${ESP_COMPAT}")
target_compile_definitions(model_test PRIVATE ${SD_DEFINES})
target_compile_options(model_test PRIVATE ${WARNINGS} -ffunction-sections -fdata-sections)
target_link_libraries(model_test PRIVATE host_env)
target_link_options(model_test PRIVATE -Wl,--gc-sections)

# 服务层、章节下载、缓存与手机表单
add_executable(files_test novelia/files_test.cc
    "${WEB_NOVEL}/web_novel_service.cc" "${WEB_NOVEL}/novelia_site.cc" "${WEB_NOVEL}/novalpie_site.cc"
    "${WEB_NOVEL}/web_novel_model.cc" "${WEB_NOVEL}/web_novel_epub.cc"
    "${WEB_NOVEL}/web_novel_download.cc" "${WEB_NOVEL}/web_novel_cache.cc" "${STORAGE}/sd_file.cc"
    "${WEB_NOVEL}/novalpie_chapter.cc" "${MAIN}/reader/html_content.cc"
    "${PROXY}/transport.cc" "${PROXY}/tls_stream.cc" "${PROXY}/trojan.cc")
target_include_directories(files_test PRIVATE
    "${WEB_NOVEL}" "${MAIN}" "${STORAGE}" "${PROXY}/include" "${ESP_COMPAT}"
    "${REPO}/tools/ebook/emulator/lvgl")
target_compile_definitions(files_test PRIVATE ${SD_DEFINES} LV_CONF_SKIP=1)
target_compile_options(files_test PRIVATE ${WARNINGS} -ffunction-sections -fdata-sections)
target_link_libraries(files_test PRIVATE host_env)
target_link_options(files_test PRIVATE -Wl,--gc-sections -Wl,--wrap=rename -Wl,--wrap=remove)

# SD 文件替换
add_executable(sd_file_test sd_file_test.cc "${STORAGE}/sd_file.cc")
target_include_directories(sd_file_test PRIVATE "${STORAGE}" "${MAIN}" "${ESP_COMPAT}")
target_compile_definitions(sd_file_test PRIVATE ${SD_DEFINES})
target_compile_options(sd_file_test PRIVATE ${WARNINGS})
target_link_libraries(sd_file_test PRIVATE host_env)
target_link_options(sd_file_test PRIVATE
    -Wl,--wrap=rename -Wl,--wrap=unlink -Wl,--wrap=fwrite -Wl,--wrap=fflush
    -Wl,--wrap=fsync -Wl,--wrap=fclose)

# 阅读数据路径与外部书籍命名空间
add_executable(sd_data_layout_test sd_data_layout_test.cc
    "${STORAGE}/sd_data_layout.cc" "${STORAGE}/sd_file.cc")
target_include_directories(sd_data_layout_test PRIVATE
    "${STORAGE}" "${MAIN}" "${ESP_COMPAT}" "${CMAKE_CURRENT_SOURCE_DIR}/storage_stubs")
target_compile_definitions(sd_data_layout_test PRIVATE ${SD_DEFINES})
target_compile_options(sd_data_layout_test PRIVATE
    ${WARNINGS} -Wno-unused-variable -Wno-unused-parameter -ffunction-sections -fdata-sections)
target_link_libraries(sd_data_layout_test PRIVATE host_env)
target_link_options(sd_data_layout_test PRIVATE -Wl,--gc-sections)

# ReaderLaunch 引用了 LVGL 类型；这里只需头文件，不构建或链接图形库。
add_executable(reader_toc_test reader_toc_test.cc
    "${MAIN}/reader/txt_chapter.cc" "${MAIN}/reader/text_encoding.cc")
target_compile_options(reader_toc_test PRIVATE ${WARNINGS} -UNDEBUG)
target_compile_definitions(reader_toc_test PRIVATE LV_CONF_SKIP=1)
target_include_directories(reader_toc_test PRIVATE "${MAIN}" "${ESP_COMPAT}" "${REPO}/tools/ebook/emulator/lvgl")

# Linux 内核与控制台；镜像由统一测试入口传入。
set(LINUX_APP "${MAIN}/display/screen/linux_screen")
add_executable(linux_boot_test linux/boot_test.cc "${LINUX_APP}/linux_vm.cc")
target_include_directories(linux_boot_test PRIVATE "${LINUX_APP}")
target_compile_options(linux_boot_test PRIVATE ${WARNINGS} -Wno-unused-parameter -UNDEBUG)
add_executable(linux_console_test linux/console_test.cc "${LINUX_APP}/linux_console.cc")
target_include_directories(linux_console_test PRIVATE "${LINUX_APP}")
target_compile_options(linux_console_test PRIVATE ${WARNINGS} -UNDEBUG)
