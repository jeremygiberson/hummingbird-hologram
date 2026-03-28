cmake_minimum_required(VERSION 3.16)
project(hologram C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# --- Options ---
option(HOLOGRAM_SHADER_HOTRELOAD "Enable shader hot-reload in debug builds" ON)
option(HOLOGRAM_EMBED_SHADERS "Embed shaders as string literals (release)" OFF)

# --- Default window size (matches target display) ---
set(HOLOGRAM_DEFAULT_WIDTH 800 CACHE STRING "Default window width")
set(HOLOGRAM_DEFAULT_HEIGHT 800 CACHE STRING "Default window height")

# --- Find SDL2 ---
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(SDL2 IMPORTED_TARGET sdl2)
endif()

if(NOT SDL2_FOUND)
    find_package(SDL2 REQUIRED)
endif()

# --- Platform detection ---
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm|aarch64")
    set(PLATFORM_PI TRUE)
    message(STATUS "Target platform: Raspberry Pi (GLES2)")
    add_definitions(-DPLATFORM_PI)

    # Find GLES2 and EGL
    find_library(GLESV2_LIB GLESv2)
    find_library(EGL_LIB EGL)
    if(NOT GLESV2_LIB OR NOT EGL_LIB)
        message(FATAL_ERROR "GLESv2 and EGL required for Pi target")
    endif()
    set(GL_LIBS ${GLESV2_LIB} ${EGL_LIB})
else()
    set(PLATFORM_PI FALSE)
    message(STATUS "Target platform: Desktop (OpenGL 2.1)")
    add_definitions(-DPLATFORM_DESKTOP)

    if(APPLE)
        find_library(OPENGL_LIB OpenGL)
        set(GL_LIBS ${OPENGL_LIB})
        add_definitions(-DGL_SILENCE_DEPRECATION)
    else()
        find_package(OpenGL REQUIRED)
        set(GL_LIBS OpenGL::GL)
    endif()
endif()

# --- Sources ---
set(SOURCES
    src/main.c
    src/renderer.c
    src/layer_hummingbird.c
    src/layer_particles.c
    src/layer_debug_audio.c
    src/shader.c
    src/audio.c
    src/model.c
    src/kiss_fft.c
    src/stb_impl.c
)

add_executable(hologram ${SOURCES})

# --- Compile definitions ---
target_compile_definitions(hologram PRIVATE
    HOLOGRAM_DEFAULT_WIDTH=${HOLOGRAM_DEFAULT_WIDTH}
    HOLOGRAM_DEFAULT_HEIGHT=${HOLOGRAM_DEFAULT_HEIGHT}
    $<$<CONFIG:Debug>:HOLOGRAM_DEBUG>
    $<$<BOOL:${HOLOGRAM_SHADER_HOTRELOAD}>:HOLOGRAM_SHADER_HOTRELOAD>
)

# --- Include directories ---
target_include_directories(hologram PRIVATE src/)

# --- Link ---
if(PkgConfig_FOUND AND TARGET PkgConfig::SDL2)
    target_link_libraries(hologram PkgConfig::SDL2 ${GL_LIBS} m)
else()
    target_link_libraries(hologram ${SDL2_LIBRARIES} ${GL_LIBS} m)
    target_include_directories(hologram PRIVATE ${SDL2_INCLUDE_DIRS})
endif()

# --- Compiler warnings ---
target_compile_options(hologram PRIVATE
    -Wall -Wextra -Wpedantic
    $<$<CONFIG:Debug>:-O0 -g>
    $<$<CONFIG:Release>:-O2 -DNDEBUG>
)

# --- Install rules (used by Buildroot) ---
install(TARGETS hologram DESTINATION /usr/bin)
install(DIRECTORY assets/ DESTINATION /usr/share/hologram/assets
        FILES_MATCHING PATTERN "*.glb" PATTERN "*.gltf" PATTERN "*.png" PATTERN "*.jpg")
install(DIRECTORY shaders/ DESTINATION /usr/share/hologram/shaders
        FILES_MATCHING PATTERN "*.vert" PATTERN "*.frag")
