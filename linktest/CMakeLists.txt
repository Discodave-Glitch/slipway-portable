cmake_minimum_required(VERSION 3.16)
# Slipway Portable - FM-1 link test (read-only). Uses the protocol component next door.
set(EXTRA_COMPONENT_DIRS ../components)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(slipway_linktest)
