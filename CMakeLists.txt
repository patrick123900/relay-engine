cmake_minimum_required(VERSION 3.25)

project(RelayEngine VERSION 0.1.0 LANGUAGES CXX)

option(RELAY_BUILD_TESTS "Build Relay's tests" ON)
option(RELAY_ENABLE_SDL "Build the SDL-powered human window" ON)
option(RELAY_ENABLE_EDITOR_UI "Build the Dear ImGui human editor interface" ON)
option(RELAY_FETCH_VULKAN_HEADERS "Fetch pinned Khronos headers when the system only has a loader" ON)
option(RELAY_OPEN_DEMO_PROJECT "Open examples/demo when the editor starts (dev builds)" OFF)

find_package(Python3 3.10 COMPONENTS Interpreter REQUIRED)
include(FetchContent)
set(OVERRIDE_CXX_FLAGS OFF CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
set(GENERATE_DEBUG_SYMBOLS OFF CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(JPH_USE_VK OFF CACHE BOOL "" FORCE)
set(JPH_USE_DX12 OFF CACHE BOOL "" FORCE)
set(JPH_USE_MTL OFF CACHE BOOL "" FORCE)
set(CROSS_PLATFORM_DETERMINISTIC ON CACHE BOOL "" FORCE)
set(DOUBLE_PRECISION ON CACHE BOOL "" FORCE)
set(USE_SSE4_1 OFF CACHE BOOL "" FORCE)
set(USE_SSE4_2 OFF CACHE BOOL "" FORCE)
set(USE_AVX OFF CACHE BOOL "" FORCE)
set(USE_AVX2 OFF CACHE BOOL "" FORCE)
set(USE_LZCNT OFF CACHE BOOL "" FORCE)
set(USE_TZCNT OFF CACHE BOOL "" FORCE)
set(USE_F16C OFF CACHE BOOL "" FORCE)
set(USE_FMADD OFF CACHE BOOL "" FORCE)
FetchContent_Declare(JoltPhysics
    URL https://github.com/jrouwe/JoltPhysics/archive/refs/tags/v5.6.0.tar.gz
    URL_HASH SHA256=6e069ee0172478cc78182047aac87e5310ba14a67a53348ae14cc37801fd3f8e
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR Build)
FetchContent_MakeAvailable(JoltPhysics)
add_custom_target(relay_protocol_codegen_check ALL
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_protocol.py" --check
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Checking generated Relay protocol artifacts"
    VERBATIM
)

add_library(relay_engine
    src/control/control_protocol.cpp
    src/control/agent_session.cpp
    src/control/generated_protocol.cpp
    src/control/local_server.cpp
    src/control/protocol_validation.cpp
    src/core/engine.cpp
    src/core/first_person.cpp
    src/core/hash.cpp
    src/core/input.cpp
    src/core/log.cpp
    src/core/process.cpp
    src/physics/collision.cpp
    src/physics/jolt_world.cpp
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
    src/render/upload_budget.cpp
    src/render/shader_reflection.cpp
    src/render/vulkan_device.cpp
    src/scene/scene.cpp
    src/scene/components.cpp
    src/scene/templates.cpp
    src/scene/node_types.cpp
    src/scene/project.cpp
    src/scene/project_package.cpp
    src/scene/project_files.cpp
    src/scene/scene_edit.cpp
    src/scene/scene_history.cpp
    src/scene/scene_io.cpp
    src/script/script_system.cpp
)
add_library(Relay::Engine ALIAS relay_engine)
target_link_libraries(relay_engine PRIVATE Jolt ${CMAKE_DL_LIBS})
# Project scripts compile with the same compiler as Relay unless RELAY_SCRIPT_COMPILER overrides it.
target_compile_definitions(relay_engine PRIVATE
    RELAY_SCRIPT_DEFAULT_COMPILER="${CMAKE_CXX_COMPILER}"
    RELAY_SCRIPT_DEFAULT_SDK_DIR="${CMAKE_CURRENT_SOURCE_DIR}/sdk")
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

function(relay_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()
endfunction()
relay_enable_warnings(relay_engine)

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
target_compile_definitions(relay_demo PRIVATE RELAY_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
if(RELAY_OPEN_DEMO_PROJECT)
    target_compile_definitions(relay_demo PRIVATE RELAY_DEMO_PROJECT="examples/demo/demo.relayproject")
endif()
target_link_libraries(relay_demo PRIVATE Relay::Engine)
relay_enable_warnings(relay_demo)

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
        target_sources(relay_demo PRIVATE src/platform/sdl_window.cpp src/platform/sdl_input.cpp)
        target_link_libraries(relay_demo PRIVATE SDL3::SDL3)
        target_compile_definitions(relay_demo PRIVATE RELAY_HAS_SDL3=1)

        find_program(RELAY_GLSLC glslc)
        if(TARGET Vulkan::Vulkan AND RELAY_GLSLC)
            set(RELAY_SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
            set(RELAY_VERTEX_SHADER "${RELAY_SHADER_OUTPUT_DIR}/first_light.vert.spv")
            set(RELAY_FRAGMENT_SHADER "${RELAY_SHADER_OUTPUT_DIR}/first_light.frag.spv")
            set(RELAY_TONE_VERTEX "${RELAY_SHADER_OUTPUT_DIR}/tone_map.vert.spv")
            set(RELAY_TONE_FRAGMENT "${RELAY_SHADER_OUTPUT_DIR}/tone_map.frag.spv")
            set(RELAY_SHADOW_VERTEX "${RELAY_SHADER_OUTPUT_DIR}/directional_shadow.vert.spv")
            set(RELAY_SHADOW_FRAGMENT "${RELAY_SHADER_OUTPUT_DIR}/directional_shadow.frag.spv")
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
            foreach(stage vert frag)
                add_custom_command(
                    OUTPUT "${RELAY_SHADER_OUTPUT_DIR}/tone_map.${stage}.spv"
                    COMMAND "${CMAKE_COMMAND}" -E make_directory "${RELAY_SHADER_OUTPUT_DIR}"
                    COMMAND "${RELAY_GLSLC}" -fshader-stage=${stage}
                            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/tone_map.${stage}"
                            -o "${RELAY_SHADER_OUTPUT_DIR}/tone_map.${stage}.spv"
                    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/tone_map.${stage}"
                    VERBATIM
                )
            endforeach()
            foreach(stage vert frag)
                add_custom_command(
                    OUTPUT "${RELAY_SHADER_OUTPUT_DIR}/directional_shadow.${stage}.spv"
                    COMMAND "${CMAKE_COMMAND}" -E make_directory "${RELAY_SHADER_OUTPUT_DIR}"
                    COMMAND "${RELAY_GLSLC}" -fshader-stage=${stage}
                            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/directional_shadow.${stage}"
                            -o "${RELAY_SHADER_OUTPUT_DIR}/directional_shadow.${stage}.spv"
                    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/directional_shadow.${stage}"
                    VERBATIM
                )
            endforeach()
            foreach(stage vert frag)
                set(RELAY_GRID_${stage} "${RELAY_SHADER_OUTPUT_DIR}/editor_grid.${stage}.spv")
                add_custom_command(
                    OUTPUT "${RELAY_GRID_${stage}}"
                    COMMAND "${CMAKE_COMMAND}" -E make_directory "${RELAY_SHADER_OUTPUT_DIR}"
                    COMMAND "${RELAY_GLSLC}" -fshader-stage=${stage}
                            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/editor_grid.${stage}"
                            -o "${RELAY_GRID_${stage}}"
                    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/editor_grid.${stage}"
                    VERBATIM
                )
            endforeach()
            foreach(stage vert frag)
                set(RELAY_SELECTION_${stage} "${RELAY_SHADER_OUTPUT_DIR}/editor_selection.${stage}.spv")
                add_custom_command(
                    OUTPUT "${RELAY_SELECTION_${stage}}"
                    COMMAND "${CMAKE_COMMAND}" -E make_directory "${RELAY_SHADER_OUTPUT_DIR}"
                    COMMAND "${RELAY_GLSLC}" -fshader-stage=${stage}
                            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/editor_selection.${stage}"
                            -o "${RELAY_SELECTION_${stage}}"
                    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/editor_selection.${stage}"
                    VERBATIM
                )
            endforeach()
            add_custom_target(relay_shaders DEPENDS "${RELAY_VERTEX_SHADER}" "${RELAY_FRAGMENT_SHADER}"
                              "${RELAY_TONE_VERTEX}" "${RELAY_TONE_FRAGMENT}"
                              "${RELAY_SHADOW_VERTEX}" "${RELAY_SHADOW_FRAGMENT}"
                              "${RELAY_GRID_vert}" "${RELAY_GRID_frag}"
                              "${RELAY_SELECTION_vert}" "${RELAY_SELECTION_frag}")
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
                RELAY_TONE_VERTEX_PATH="${RELAY_TONE_VERTEX}"
                RELAY_TONE_FRAGMENT_PATH="${RELAY_TONE_FRAGMENT}"
                RELAY_SHADOW_VERTEX_PATH="${RELAY_SHADOW_VERTEX}"
                RELAY_SHADOW_FRAGMENT_PATH="${RELAY_SHADOW_FRAGMENT}"
                RELAY_GRID_VERTEX_PATH="${RELAY_GRID_vert}"
                RELAY_GRID_FRAGMENT_PATH="${RELAY_GRID_frag}"
                RELAY_SELECTION_VERTEX_PATH="${RELAY_SELECTION_vert}"
                RELAY_SELECTION_FRAGMENT_PATH="${RELAY_SELECTION_frag}"
            )

            if(RELAY_ENABLE_EDITOR_UI)
                # Dear ImGui is pinned to a tagged release rather than the untagged docking branch, so
                # the editor build stays reproducible. The library is linked into the demo and headless UI tests:
                # relay_engine must remain free of UI and model-facing dependencies.
                include(FetchContent)
                FetchContent_Declare(
                    DearImGui
                    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.1-docking.tar.gz
                    URL_HASH SHA256=2f308ae014a8f3a46d89cf4db71c814af815b704b8a88b7136bae148eb8f4b71
                    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
                )
                FetchContent_MakeAvailable(DearImGui)

                # ImGuizmo has no release tagged against a current Dear ImGui, so it is pinned to an
                # exact commit with its archive checksum rather than to a moving branch.
                FetchContent_Declare(
                    ImGuizmo
                    URL https://github.com/CedricGuillemet/ImGuizmo/archive/18cef5e031d8c6973d80284c67f60549fafd78c1.tar.gz
                    URL_HASH SHA256=6ad626f0687be12c2f3ba6542c0f1bdda9e71e395d0645e4cda37695354406d8
                    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
                    # Point at a directory with no CMakeLists so upstream's build is not added.
                    # It would compile unrelated widgets against an ImGui it cannot locate.
                    SOURCE_SUBDIR relay-does-not-build-this
                )
                FetchContent_MakeAvailable(ImGuizmo)

                add_library(relay_imgui STATIC
                    "${dearimgui_SOURCE_DIR}/imgui.cpp"
                    "${dearimgui_SOURCE_DIR}/imgui_draw.cpp"
                    "${dearimgui_SOURCE_DIR}/imgui_tables.cpp"
                    "${dearimgui_SOURCE_DIR}/imgui_widgets.cpp"
                    "${dearimgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp"
                    "${dearimgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp"
                    "${imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp"
                )
                target_compile_features(relay_imgui PUBLIC cxx_std_20)
                # SYSTEM keeps third-party headers from emitting warnings into Relay's strict builds.
                target_include_directories(relay_imgui SYSTEM PUBLIC
                    "${dearimgui_SOURCE_DIR}"
                    "${dearimgui_SOURCE_DIR}/backends"
                    "${imguizmo_SOURCE_DIR}/src"
                )
                target_link_libraries(relay_imgui PUBLIC SDL3::SDL3 Vulkan::Vulkan)
                if(TARGET Vulkan::Headers)
                    target_link_libraries(relay_imgui PUBLIC Vulkan::Headers)
                endif()

                target_sources(relay_demo PRIVATE src/editor/editor_ui.cpp src/editor/editor_theme.cpp
                    src/editor/editor_layout.cpp src/editor/chat_media.cpp
                    src/editor/file_browser.cpp)
                find_package(PkgConfig QUIET)
                if(PkgConfig_FOUND)
                    pkg_check_modules(CHAT_VIDEO QUIET IMPORTED_TARGET libavformat libavcodec libswscale libavutil)
                endif()
                if(TARGET PkgConfig::CHAT_VIDEO)
                    target_link_libraries(relay_imgui PUBLIC PkgConfig::CHAT_VIDEO)
                    target_compile_definitions(relay_imgui PUBLIC RELAY_CHAT_VIDEO=1)
                endif()
                target_link_libraries(relay_demo PRIVATE relay_imgui)
                target_compile_definitions(relay_demo PRIVATE RELAY_HAS_EDITOR_UI=1)
            endif()
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
    relay_enable_warnings(relay_tests)
    target_compile_definitions(relay_tests PRIVATE
        RELAY_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    if(TARGET relay_shaders)
        add_dependencies(relay_tests relay_shaders)
        target_compile_definitions(relay_tests PRIVATE
            RELAY_TEST_VERTEX_SHADER_PATH="${RELAY_VERTEX_SHADER}"
            RELAY_TEST_FRAGMENT_SHADER_PATH="${RELAY_FRAGMENT_SHADER}"
            RELAY_TEST_TONE_VERTEX_PATH="${RELAY_TONE_VERTEX}"
            RELAY_TEST_TONE_FRAGMENT_PATH="${RELAY_TONE_FRAGMENT}"
        )
    endif()
    add_test(NAME relay_tests COMMAND relay_tests)
    add_executable(relay_workflow_tests tests/editor_workflow_tests.cpp)
    target_link_libraries(relay_workflow_tests PRIVATE Relay::Engine)
    target_compile_definitions(relay_workflow_tests PRIVATE
        RELAY_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    relay_enable_warnings(relay_workflow_tests)
    add_test(NAME relay_workflow_tests COMMAND relay_workflow_tests)
    add_executable(relay_script_tests tests/script_tests.cpp)
    target_link_libraries(relay_script_tests PRIVATE Relay::Engine)
    relay_enable_warnings(relay_script_tests)
    add_test(NAME relay_script_tests COMMAND relay_script_tests)
    set_tests_properties(relay_script_tests PROPERTIES TIMEOUT 600)
    # Regenerates examples/demo; see tools/generate_demo_project.py.
    add_executable(relay_build_demo_project tools/demo_project/build_demo_project.cpp)
    target_link_libraries(relay_build_demo_project PRIVATE Relay::Engine)
    relay_enable_warnings(relay_build_demo_project)
    add_executable(relay_session_test_host tests/session_test_host.cpp)
    target_link_libraries(relay_session_test_host PRIVATE Relay::Engine)
    relay_enable_warnings(relay_session_test_host)
    add_test(NAME relay_session_protocol_tests
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/session_protocol_tests.py"
                "$<TARGET_FILE:relay_demo>")
    set_tests_properties(relay_session_protocol_tests PROPERTIES TIMEOUT 30)
    if(TARGET relay_imgui)
        add_executable(relay_editor_headless_tests tests/editor_headless_tests.cpp
            src/editor/editor_ui.cpp src/editor/editor_theme.cpp src/editor/editor_layout.cpp src/editor/chat_media.cpp
            src/editor/file_browser.cpp src/platform/sdl_input.cpp)
        target_link_libraries(relay_editor_headless_tests PRIVATE Relay::Engine relay_imgui)
        target_compile_definitions(relay_editor_headless_tests PRIVATE RELAY_CHAT_MEDIA_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/chat-media")
        relay_enable_warnings(relay_editor_headless_tests)
        add_test(NAME relay_editor_headless_tests COMMAND relay_editor_headless_tests)
    endif()
endif()
