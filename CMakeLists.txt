cmake_minimum_required(VERSION 3.21)
project(cartload VERSION 0.1.0 LANGUAGES C) # the one place the version number lives
set(CMAKE_C_STANDARD 17)

# a static triplet gives one self-contained exe (see README: "Single exe build")
if(VCPKG_TARGET_TRIPLET MATCHES "static")
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

find_package(SDL2 CONFIG REQUIRED)
find_package(SDL2_ttf CONFIG REQUIRED)

# Every emulation core is a libretro dll loaded at runtime, so nothing to compile here:
# libretro.h is just the calling convention.
configure_file(cartload.rc.in "${CMAKE_CURRENT_BINARY_DIR}/cartload.rc" @ONLY)
add_executable(cartload WIN32 main.c core.c systems.c archive.c "${CMAKE_CURRENT_BINARY_DIR}/cartload.rc")
target_compile_definitions(cartload PRIVATE _CRT_SECURE_NO_WARNINGS CARTLOAD_VERSION="${PROJECT_VERSION}")
target_link_libraries(cartload PRIVATE opengl32 urlmon) # urlmon: the in-app core downloader
# the static ports export different target names than the dll ones
if(TARGET SDL2::SDL2)
  target_link_libraries(cartload PRIVATE SDL2::SDL2)
else()
  target_link_libraries(cartload PRIVATE SDL2::SDL2-static)
endif()
if(TARGET SDL2_ttf::SDL2_ttf)
  target_link_libraries(cartload PRIVATE SDL2_ttf::SDL2_ttf)
else()
  target_link_libraries(cartload PRIVATE SDL2_ttf::SDL2_ttf-static)
endif()
target_link_libraries(cartload PRIVATE SDL2::SDL2main)

# dll build only: ship the runtime DLLs next to the exe. Static builds have none.
if(NOT VCPKG_TARGET_TRIPLET MATCHES "static")
  add_custom_command(TARGET cartload POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:cartload> $<TARGET_FILE_DIR:cartload>
    COMMAND_EXPAND_LISTS)
endif()

# the cores are not ours to build; copy whatever getcores.ps1 dropped in cores\
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cores")
  add_custom_command(TARGET cartload POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/cores" "$<TARGET_FILE_DIR:cartload>/cores")
endif()
