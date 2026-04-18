cmake_minimum_required(VERSION 3.22.1)
project("popmobile" C CXX)

# Optimization flags
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -Wall")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -Wall -std=c++17")

# ARM NEON is enabled by default on arm64-v8a
# For armeabi-v7a, enable it explicitly
if(${ANDROID_ABI} STREQUAL "armeabi-v7a")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mfpu=neon")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mfpu=neon")
endif()

add_library(popmobile SHARED
    keccak.c
    kheavyhash.c
    mining_engine.c
    jni_bridge.cpp
)

target_include_directories(popmobile PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

find_library(log-lib log)
target_link_libraries(popmobile ${log-lib})
