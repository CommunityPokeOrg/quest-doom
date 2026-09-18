cmake_minimum_required(VERSION 3.22.1)
project(openxr_smoke C CXX)

add_library(openxr_smoke SHARED
    smoke.c
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)

target_compile_definitions(openxr_smoke PRIVATE XR_USE_PLATFORM_ANDROID)

target_include_directories(openxr_smoke PRIVATE
    ${ANDROID_NDK}/sources/android/native_app_glue
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../build/${ANDROID_ABI}/include)

target_link_libraries(openxr_smoke log android dl)
