cmake_minimum_required(VERSION 3.16)
project(hello-triangle C)

# --- Find SDL2 ---
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(SDL2 IMPORTED_TARGET sdl2)
endif()
if(NOT SDL2_FOUND)
    find_package(SDL2 REQUIRED)
endif()

add_executable(hello-triangle main.c)

# --- Platform GL setup ---
if(APPLE)
    target_compile_definitions(hello-triangle PRIVATE GL_SILENCE_DEPRECATION)
    target_link_libraries(hello-triangle PRIVATE "-framework OpenGL")
else()
    # RPi 5 / Linux: link GLES2 + EGL
    find_library(GLES2_LIB GLESv2 REQUIRED)
    find_library(EGL_LIB EGL REQUIRED)
    target_link_libraries(hello-triangle PRIVATE ${GLES2_LIB} ${EGL_LIB})
endif()

# --- Link SDL2 ---
if(TARGET PkgConfig::SDL2)
    target_link_libraries(hello-triangle PRIVATE PkgConfig::SDL2)
elseif(TARGET SDL2::SDL2)
    target_link_libraries(hello-triangle PRIVATE SDL2::SDL2 SDL2::SDL2main)
else()
    target_include_directories(hello-triangle PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(hello-triangle PRIVATE ${SDL2_LIBRARIES})
endif()
