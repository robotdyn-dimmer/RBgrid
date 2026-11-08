# RBgrid Node: remote metering node for an RBgrid hub (ESP-NOW). Target: ESP32, 4 MB flash, no PSRAM.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rbgrid_node)
