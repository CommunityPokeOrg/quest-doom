cmake_minimum_required(VERSION 3.22.1)
project(questdoom C)

set(CMAKE_C_STANDARD 99)

# --- android_native_app_glue (from NDK) ---
# Compiled directly into the shared lib rather than a static archive: as a
# static lib the linker drops ANativeActivity_onCreate since nothing in app
# code references it, leaving NativeActivity unable to load the module.

# --- vendored OpenXR loader (see app/src/main/jniLibs/) ---
add_library(openxr_loader SHARED IMPORTED GLOBAL)
set_target_properties(openxr_loader PROPERTIES
    IMPORTED_LOCATION
    ${CMAKE_CURRENT_SOURCE_DIR}/../jniLibs/${ANDROID_ABI}/libopenxr_loader.so)

# --- doomgeneric core (vendored, see cpp/doomgeneric/) ---
file(GLOB DOOM_SOURCES CONFIGURE_DEPENDS doomgeneric/*.c)
list(FILTER DOOM_SOURCES EXCLUDE REGEX "doomgeneric_[a-z]+\\.c$")
add_library(doomcore STATIC ${DOOM_SOURCES})
target_include_directories(doomcore PUBLIC doomgeneric)
target_compile_definitions(doomcore PRIVATE NORMALUNIX LINUX _DEFAULT_SOURCE)
target_compile_options(doomcore PRIVATE -w -fno-strict-aliasing)

# --- Quest platform layer ---
add_library(questdoom SHARED
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c
    quest/quest_doom.c
    quest/quest_stubs.c
    quest/xr_engine.c
    quest/xr_input.c
    quest/xr_hands.c
    quest/gl_renderer.c
    quest/gl_world.c)

target_include_directories(questdoom PRIVATE
    openxr/include
    ${ANDROID_NDK}/sources/android/native_app_glue)

find_library(lib_log log)
find_library(lib_android android)
find_library(lib_egl EGL)
find_library(lib_gles GLESv3)

target_link_libraries(questdoom
    doomcore
    openxr_loader
    ${lib_log}
    ${lib_android}
    ${lib_egl}
    ${lib_gles}
    m)
