cmake_minimum_required(VERSION 3.16)

# Add common protocol definitions as extra component
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../common")

# Include ESP-IDF build system
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(esp32_cloud_gateway)
