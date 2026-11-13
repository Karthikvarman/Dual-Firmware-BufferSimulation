cmake_minimum_required(VERSION 3.16)

if(DEFINED ENV{IDF_PATH})
    include($ENV{IDF_PATH}/tools/cmake/project.cmake)
    project(esp32_circular_buffer_server)
else()
    project(esp32_circular_buffer_server C)
    include_directories(
        "${CMAKE_CURRENT_SOURCE_DIR}/main"
        "${CMAKE_CURRENT_SOURCE_DIR}/../common"
        "${CMAKE_CURRENT_SOURCE_DIR}/../mock_esp_idf/include"
    )
    add_library(server_component STATIC
        main/main_server.c
        ../common/circular_buffer.c
    )
    message(STATUS "Configured standalone project with mock ESP-IDF headers.")
endif()
