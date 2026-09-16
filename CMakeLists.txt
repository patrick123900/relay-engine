cmake_minimum_required(VERSION 3.25)

project(RelayEngine VERSION 0.1.0 LANGUAGES CXX)

option(RELAY_BUILD_TESTS "Build Relay's tests" ON)
option(RELAY_ENABLE_SDL "Build the SDL-powered human window" ON)
option(RELAY_FETCH_VULKAN_HEADERS "Fetch pinned Khronos headers when the system only has a loader" ON)

find_package(Python3 3.10 COMPONENTS Interpreter REQUIRED)
add_custom_target(relay_protocol_codegen_check ALL
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_protocol.py" --check
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Checking generated Relay protocol artifacts"
    VERBATIM
)

add_library(relay_engine
    src/control/control_protocol.cpp
    src/control/generated_protocol.cpp
    src/control/local_server.cpp
    src/control/protocol_validation.cpp
    src/core/engine.cpp
    src/core/hash.cpp
    src/core/log.cpp
    src/observe/capture.cpp
    src/observe/performance.cpp
    src/observe/trace.cpp
    src/render/software_renderer.cpp
    src/render/asset_manifest.cpp
    src/render/assets.cpp
    src/render/blender_adapter.cpp
    src/render/image_decode.cpp
    src/render/model_import.cpp
    src/render/scene_render.cpp
    src/render/render_graph.cpp
    src/render/shader_reflection.cpp
    src/render/vulkan_device.cpp
    src/scene/scene.cpp
    src/scene/scene_history.cpp
    src/scene/scene_io.cpp
)
add_library(Relay::Engine ALIAS relay_engine)
add_dependencies(relay_engine relay_protocol_codegen_check)

target_compile_features(relay_engine PUBLIC cxx_std_20)
target_include_directories(relay_engine
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

find_program(RELAY_FFMPEG ffmpeg)
if(RELAY_FFMPEG)
    target_compile_definitions(relay_engine PRIVATE RELAY_FFMPEG_PATH="${RELAY_FFMPEG}")
endif()

find_package(assimp CONFIG QUIET)
if(TARGET assimp::assimp)
    target_link_libraries(relay_engine PRIVATE assimp::assimp)
    target_compile_definitions(relay_engine PRIVATE RELAY_HAS_ASSIMP=1)
else()
    message(STATUS "Assimp was not found; project model import will report unavailable")
endif()

find_package(PNG QUIET)
if(TARGET PNG::PNG)
    target_link_libraries(relay_engine PRIVATE PNG::PNG)
    target_compile_definitions(relay_engine PRIVATE RELAY_HAS_PNG=1)
endif()

find_package(JPEG QUIET)
if(TARGET JPEG::JPEG)
    target_link_libraries(relay_engine PRIVATE JPEG::JPEG)
    target_compile_definitions(relay_engine PRIVATE RELAY_HAS_JPEG=1)
endif()

if(MSVC)
    target_compile_options(relay_engine PRIVATE /W4 /permissive-)
else()
    target_compile_options(relay_engine PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

find_package(Vulkan QUIET)
if(NOT TARGET Vulkan::Vulkan)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(VulkanPkg QUIET IMPORTED_TARGET vulkan)
        if(TARGET PkgConfig::VulkanPkg)
            add_library(Vulkan::Vulkan ALIAS PkgConfig::VulkanPkg)
        endif()
    endif()
endif()
if(TARGET Vulkan::Vulkan)
    include(CheckIncludeFileCXX)
    check_include_file_cxx("vulkan/vulkan.h" RELAY_HAS_SYSTEM_VULKAN_HEADERS)
    if(NOT Vulkan_FOUND AND NOT RELAY_HAS_SYSTEM_VULKAN_HEADERS AND RELAY_FETCH_VULKAN_HEADERS)
        include(FetchContent)
        set(VULKAN_HEADERS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
        set(VULKAN_HEADERS_ENABLE_MODULE OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
            VulkanHeaders
            URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.4.357.tar.gz
            URL_HASH SHA256=7dc0dbcf1d49dd3d7da3761c251c6097dfbaac475321a4a8a99269d3d5abecdc
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        )
        FetchContent_MakeAvailable(VulkanHeaders)
    endif()
    target_link_libraries(relay_engine PRIVATE Vulkan::Vulkan)
    if(TARGET Vulkan::Headers)
        target_link_libraries(relay_engine PRIVATE Vulkan::Headers)
    endif()
    target_compile_definitions(relay_engine PRIVATE RELAY_HAS_VULKAN=1)
endif()

if(WIN32)
    target_link_libraries(relay_engine PRIVATE ws2_32 psapi)
endif()

add_executable(relay_demo apps/relay_demo/main.cpp)
target_link_libraries(relay_demo PRIVATE Relay::Engine)

if(RELAY_ENABLE_SDL)
    find_package(SDL3 CONFIG QUIET)
    if(NOT TARGET SDL3::SDL3)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(SDL3 QUIET IMPORTED_TARGET sdl3)
            if(TARGET PkgConfig::SDL3)
                add_library(SDL3::SDL3 ALIAS PkgConfig::SDL3)
            endif()
        endif()
    endif()

    if(TARGET SDL3::SDL3)
        target_sources(relay_demo PRIVATE src/platform/sdl_window.cpp)
        target_link_libraries(relay_demo PRIVATE SDL3::SDL3)
        target_compile_definitions(relay_demo PRIVATE RELAY_HAS_SDL3=1)

        find_program(RELAY_GLSLC glslc)
        if(TARGET Vulkan::Vulkan AND RELAY_GLSLC)
            set(RELAY_SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
            set(RELAY_VERTEX_SHADER "${RELAY_SHADER_OUTPUT_DIR}/first_light.vert.spv")
            set(RELAY_FRAGMENT_SHADER "${RELAY_SHADER_OUTPUT_DIR}/first_light.frag.spv")
            add_custom_command(
                OUTPUT "${RELAY_VERTEX_SHADER}"
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${RELAY_SHADER_OUTPUT_DIR}"
                COMMAND "${RELAY_GLSLC}" -fshader-stage=vert
                        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/first_light.vert"
                        -o "${RELAY_VERTEX_SHADER}"
                DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/first_light.vert"
                VERBATIM
            )
            add_custom_command(
                OUTPUT "${RELAY_FRAGMENT_SHADER}"
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${RELAY_SHADER_OUTPUT_DIR}"
                COMMAND "${RELAY_GLSLC}" -fshader-stage=frag
                        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/first_light.frag"
                        -o "${RELAY_FRAGMENT_SHADER}"
                DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/first_light.frag"
                VERBATIM
            )
            add_custom_target(relay_shaders DEPENDS "${RELAY_VERTEX_SHADER}" "${RELAY_FRAGMENT_SHADER}")
            add_dependencies(relay_demo relay_shaders)
            target_sources(relay_demo PRIVATE src/platform/vulkan_window.cpp)
            target_link_libraries(relay_demo PRIVATE Vulkan::Vulkan)
            if(TARGET Vulkan::Headers)
                target_link_libraries(relay_demo PRIVATE Vulkan::Headers)
            endif()
            target_compile_definitions(relay_demo PRIVATE
                RELAY_HAS_VULKAN_WINDOW=1
                RELAY_VERTEX_SHADER_PATH="${RELAY_VERTEX_SHADER}"
                RELAY_FRAGMENT_SHADER_PATH="${RELAY_FRAGMENT_SHADER}"
            )
        else()
            message(STATUS "Vulkan window disabled: Vulkan loader or glslc was not found")
        endif()
    else()
        message(WARNING "SDL3 was not found; relay_demo will support headless mode only")
    endif()
endif()

if(RELAY_BUILD_TESTS)
    enable_testing()
    add_executable(relay_tests tests/engine_tests.cpp)
    target_link_libraries(relay_tests PRIVATE Relay::Engine)
    target_compile_definitions(relay_tests PRIVATE
        RELAY_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    if(TARGET relay_shaders)
        add_dependencies(relay_tests relay_shaders)
        target_compile_definitions(relay_tests PRIVATE
            RELAY_TEST_VERTEX_SHADER_PATH="${RELAY_VERTEX_SHADER}"
            RELAY_TEST_FRAGMENT_SHADER_PATH="${RELAY_FRAGMENT_SHADER}"
        )
    endif()
    add_test(NAME relay_tests COMMAND relay_tests)
endif()
