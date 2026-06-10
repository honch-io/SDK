cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "../../..")
set(HONCH_LOCAL_SDKCONFIG_DEFAULTS "../local/sdkconfig.defaults")

if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/${HONCH_LOCAL_SDKCONFIG_DEFAULTS}")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults" "${HONCH_LOCAL_SDKCONFIG_DEFAULTS}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(honch_example)
