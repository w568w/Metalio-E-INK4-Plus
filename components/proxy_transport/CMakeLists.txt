idf_build_get_property(target IDF_TARGET)
if(target STREQUAL "linux")
    set(network linux)
else()
    set(network lwip)
endif()
idf_component_register(SRCS "transport.cc" "tls_stream.cc" "trojan.cc" "http.cc"
    INCLUDE_DIRS "include" REQUIRES ${network} mbedtls esp_hw_support esp_timer
    PRIV_REQUIRES esp_http_client tcp_transport)
