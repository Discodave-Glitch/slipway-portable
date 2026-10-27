cmake_minimum_required(VERSION 3.16)
# Slipway Portable - stage 1: SD card firmware browser (read-only: reads files from the SD card, never touches an FM-1).
set(EXTRA_COMPONENT_DIRS ../components)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(slipway_browser)
