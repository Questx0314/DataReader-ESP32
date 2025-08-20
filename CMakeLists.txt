# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# 设置编译器优化选项以避免内部编译错误
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O1")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O1")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mqtt)

# 添加SPIFFS文件系统支持
spiffs_create_partition_image(storage ./spiffs FLASH_IN_PROJECT)
