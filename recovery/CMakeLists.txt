# WiFi recovery app for the LiveLab board, installed once into the factory partition.
# Build: idf.py -C recovery -B build_recovery build   (flash build_recovery/recovery.bin at 0x620000)
cmake_minimum_required(VERSION 3.16)
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(recovery)
