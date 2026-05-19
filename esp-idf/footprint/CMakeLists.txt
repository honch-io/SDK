cmake_minimum_required(VERSION 3.16)

set(HONCH_FOOTPRINT_WITH_SDK OFF CACHE BOOL "Build footprint app with Honch SDK enabled")
set(HONCH_FOOTPRINT_CONNECTED_BASELINE OFF CACHE BOOL "Build connected baseline without Honch calls")
set(EXTRA_COMPONENT_DIRS "../honch")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(honch_footprint)
