# ESP-IDF project of the Desk Monitor firmware (Guition ESP32-S3-4848S040).
#   . firmware/tools/idf_env.sh && cd firmware && idf.py build flash monitor
# The desktop simulator lives in sim/ and has its own CMake project.
cmake_minimum_required(VERSION 3.16)

# Portable core (also built by the simulator)
set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/core)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(desk_monitor)
