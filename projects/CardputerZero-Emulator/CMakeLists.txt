# SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.16)
project(cardputer-zero-emu LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Paths — same repo, sibling projects
set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(APPLAUNCH  ${REPO_ROOT}/projects/APPLaunch/main)
set(CP0_LVGL   ${REPO_ROOT}/ext_components/cp0_lvgl)
set(APPLAUNCH_LVGL_OVERRIDE ${APPLAUNCH}/lvgl)
set(APPLAUNCH_EMU_ENTRY ${CMAKE_CURRENT_SOURCE_DIR}/src/app_launch_entry.cpp)

set(APPLAUNCH_GENERATED_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/generated/include)
set(APPLAUNCH_GENERATED_PAGE_APP ${APPLAUNCH_GENERATED_INCLUDE}/generated/page_app.h)

file(GLOB PAGE_APP_HEADERS
    RELATIVE ${APPLAUNCH}/ui
    CONFIGURE_DEPENDS
    ${APPLAUNCH}/ui/page_app/*.hpp
)
list(SORT PAGE_APP_HEADERS)
if(PAGE_APP_HEADERS)
    file(MAKE_DIRECTORY ${APPLAUNCH_GENERATED_INCLUDE}/generated)
    file(WRITE ${APPLAUNCH_GENERATED_PAGE_APP} "#pragma once\n\n")
    foreach(PAGE_APP_HEADER ${PAGE_APP_HEADERS})
        file(APPEND ${APPLAUNCH_GENERATED_PAGE_APP} "#include \"${PAGE_APP_HEADER}\"\n")
    endforeach()
else()
    message(FATAL_ERROR "No APPLaunch page_app headers found")
endif()

# ---------------------------------------------------------------------------
# SDL2 + Freetype
# ---------------------------------------------------------------------------
find_package(SDL2 QUIET)
if(NOT SDL2_FOUND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED sdl2)
endif()

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(FREETYPE freetype2)
endif()
if(NOT FREETYPE_FOUND)
    find_package(Freetype QUIET)
    if(Freetype_FOUND)
        set(FREETYPE_INCLUDE_DIRS ${FREETYPE_INCLUDE_DIRS})
        set(FREETYPE_LIBRARIES Freetype::Freetype)
    endif()
endif()

# ---------------------------------------------------------------------------
# LVGL 9.5
# ---------------------------------------------------------------------------
set(LV_CONF_BUILD_DISABLE_DEMOS ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_EXAMPLES ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_THORVG OFF CACHE BOOL "" FORCE)
set(LV_BUILD_CONF_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE PATH "" FORCE)
add_definitions(-DLV_CONF_INCLUDE_SIMPLE)

# SDL2 parent include for <SDL2/SDL.h>
if(SDL2_FOUND AND TARGET SDL2::SDL2)
    get_target_property(_sdl2_inc SDL2::SDL2 INTERFACE_INCLUDE_DIRECTORIES)
    if(_sdl2_inc)
        foreach(_dir ${_sdl2_inc})
            get_filename_component(_parent "${_dir}" DIRECTORY)
            include_directories("${_parent}")
        endforeach()
    endif()
else()
    foreach(_dir ${SDL2_INCLUDE_DIRS})
        get_filename_component(_parent "${_dir}" DIRECTORY)
        include_directories("${_parent}")
    endforeach()
endif()
if(FREETYPE_INCLUDE_DIRS)
    include_directories(${FREETYPE_INCLUDE_DIRS})
endif()

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/lib/lvgl ${CMAKE_CURRENT_BINARY_DIR}/lvgl)

file(GLOB APPLAUNCH_LVGL_FREETYPE_SRCS CONFIGURE_DEPENDS
    ${APPLAUNCH_LVGL_OVERRIDE}/lv_freetype*.c
)
if(APPLAUNCH_LVGL_FREETYPE_SRCS)
    get_target_property(LVGL_SRCS lvgl SOURCES)
    foreach(_override_src ${APPLAUNCH_LVGL_FREETYPE_SRCS})
        get_filename_component(_override_name ${_override_src} NAME)
        list(FILTER LVGL_SRCS EXCLUDE REGEX ".*/src/libs/freetype/${_override_name}$")
    endforeach()
    set_property(TARGET lvgl PROPERTY SOURCES ${LVGL_SRCS})
    target_sources(lvgl PRIVATE ${APPLAUNCH_LVGL_FREETYPE_SRCS})
    target_include_directories(lvgl BEFORE PRIVATE
        ${APPLAUNCH_LVGL_OVERRIDE}
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/lvgl/src/libs/freetype
    )
endif()

# ===========================================================================
# Targets
# ===========================================================================

file(GLOB_RECURSE APPLAUNCH_UI_C CONFIGURE_DEPENDS ${APPLAUNCH}/ui/*.c)
file(GLOB_RECURSE APPLAUNCH_UI_CPP CONFIGURE_DEPENDS ${APPLAUNCH}/ui/*.cpp)
file(GLOB CP0_LVGL_COMMON_SRCS CONFIGURE_DEPENDS
    ${CP0_LVGL}/src/*.c
    ${CP0_LVGL}/src/*.cpp
)
file(GLOB CP0_LVGL_SDL_SRCS CONFIGURE_DEPENDS
    ${CP0_LVGL_COMMON_SRCS}
    ${CP0_LVGL}/src/cp0/cp0_desktop_exec_policy.cpp
    ${CP0_LVGL}/src/cp0/cp0_process_commands.cpp
    ${CP0_LVGL}/src/cp0/cp0_process_lifecycle.cpp
    ${CP0_LVGL}/src/sdl/*.c
    ${CP0_LVGL}/src/sdl/*.cpp
)

# ---------------------------------------------------------------------------
# APP: APPLaunch shared lib
# ---------------------------------------------------------------------------
add_library(APPLaunch SHARED
    ${APPLAUNCH_UI_C}
    ${APPLAUNCH_UI_CPP}
    ${APPLAUNCH_EMU_ENTRY}
)
target_include_directories(APPLaunch PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/lib
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/lvgl
    ${REPO_ROOT}/ext_components/cp0_lvgl/include
    ${REPO_ROOT}/SDK/github_source/eventpp/include
    ${REPO_ROOT}/SDK/components/utilities/include
    ${APPLAUNCH_GENERATED_INCLUDE}
    ${APPLAUNCH}/ui
    ${APPLAUNCH}/include
    ${APPLAUNCH}
)
target_compile_definitions(APPLaunch PRIVATE HAL_PLATFORM_SDL=1)
target_compile_options(APPLaunch PRIVATE -Wno-c++11-narrowing)
target_link_options(APPLaunch PRIVATE -Wl,--unresolved-symbols=ignore-all)

# ---------------------------------------------------------------------------
# Emulator executable
# ---------------------------------------------------------------------------
add_executable(cardputer-zero-emu src/main.cpp)
add_dependencies(cardputer-zero-emu lvgl lvgl_thorvg APPLaunch)
set_target_properties(cardputer-zero-emu PROPERTIES ENABLE_EXPORTS ON)
target_sources(cardputer-zero-emu PRIVATE ${CP0_LVGL_SDL_SRCS})
target_compile_definitions(cardputer-zero-emu PRIVATE HAL_PLATFORM_SDL=1)
target_link_options(cardputer-zero-emu PRIVATE -Wl,--allow-multiple-definition)

target_include_directories(cardputer-zero-emu PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/lib
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/lvgl
    ${CP0_LVGL}/include
    ${CP0_LVGL}/src/sdl
    ${REPO_ROOT}/SDK/github_source/eventpp/include
)
target_compile_options(cardputer-zero-emu PRIVATE -Wno-c++11-narrowing)

# Force-load LVGL for dlopen'd apps
target_link_libraries(cardputer-zero-emu PRIVATE
    -Wl,--whole-archive $<TARGET_FILE:lvgl> $<TARGET_FILE:lvgl_thorvg> -Wl,--no-whole-archive
)

# SDL2
if(TARGET SDL2::SDL2)
    target_link_libraries(cardputer-zero-emu PRIVATE SDL2::SDL2)
elseif(TARGET SDL2::SDL2-static)
    target_link_libraries(cardputer-zero-emu PRIVATE SDL2::SDL2-static)
else()
    target_include_directories(cardputer-zero-emu PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(cardputer-zero-emu PRIVATE ${SDL2_LIBRARIES})
endif()
if(TARGET SDL2::SDL2main)
    target_link_libraries(cardputer-zero-emu PRIVATE SDL2::SDL2main)
endif()

# SDL2_image
if(PkgConfig_FOUND)
    pkg_check_modules(SDL2_IMAGE SDL2_image)
endif()
if(SDL2_IMAGE_FOUND)
    target_include_directories(cardputer-zero-emu PRIVATE ${SDL2_IMAGE_INCLUDE_DIRS})
    target_link_libraries(cardputer-zero-emu PRIVATE ${SDL2_IMAGE_LINK_LIBRARIES})
    target_link_directories(cardputer-zero-emu PRIVATE ${SDL2_IMAGE_LIBRARY_DIRS})
else()
    target_link_libraries(cardputer-zero-emu PRIVATE SDL2_image)
endif()

# Freetype
if(FREETYPE_FOUND AND FREETYPE_LINK_LIBRARIES)
    target_link_libraries(cardputer-zero-emu PRIVATE ${FREETYPE_LINK_LIBRARIES})
    target_link_directories(cardputer-zero-emu PRIVATE ${FREETYPE_LIBRARY_DIRS})
else()
    target_link_libraries(cardputer-zero-emu PRIVATE freetype)
endif()

# dl + threads
target_link_libraries(cardputer-zero-emu PRIVATE ${CMAKE_DL_LIBS})
find_package(Threads REQUIRED)
target_link_libraries(cardputer-zero-emu PRIVATE Threads::Threads)

# ---------------------------------------------------------------------------
# Post-build: copy apps and resources
# ---------------------------------------------------------------------------
add_custom_command(TARGET cardputer-zero-emu POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:cardputer-zero-emu>/apps
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:cardputer-zero-emu>/applications
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:cardputer-zero-emu>/images
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${REPO_ROOT}/projects/APPLaunch/APPLaunch/share/images $<TARGET_FILE_DIR:cardputer-zero-emu>/images
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:cardputer-zero-emu>/APPLaunch
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${REPO_ROOT}/projects/APPLaunch/APPLaunch/share $<TARGET_FILE_DIR:cardputer-zero-emu>/APPLaunch/share
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:cardputer-zero-emu>/assets
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/assets/device_skin.png $<TARGET_FILE_DIR:cardputer-zero-emu>/assets/device_skin.png
    COMMENT "Copying apps and resources"
)
add_custom_command(TARGET APPLaunch POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/apps
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:APPLaunch> ${CMAKE_CURRENT_BINARY_DIR}/apps/
    COMMENT "Copying APPLaunch"
)
