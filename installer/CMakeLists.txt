cmake_minimum_required(VERSION 3.16)
# Slipway Portable - installer: SD card firmware browser + install onto the FM-1 over USB (stock-first rules).
set(EXTRA_COMPONENT_DIRS ../components)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(slipway_installer)
