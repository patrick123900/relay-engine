# Builds the parts of the AMD FidelityFX SDK 1.1.4 that Relay uses (Brixelizer, Brixelizer GI,
# Denoiser, Classifier and SPD) with the Vulkan backend. This file is Relay's; upstream only
# provides Visual Studio builds. See README.md for provenance and local changes.

set(RELAY_FFX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(RELAY_FFX_SDK "${RELAY_FFX_ROOT}/sdk")
set(RELAY_FFX_GPU "${RELAY_FFX_SDK}/include/FidelityFX/gpu")
set(RELAY_FFX_TOOL "${RELAY_FFX_ROOT}/tools/ffx_shader_compiler")
set(RELAY_FFX_PERMUTATIONS "${CMAKE_CURRENT_BINARY_DIR}/permutations")

find_program(RELAY_GLSLANG_VALIDATOR glslangValidator REQUIRED)

# The shader precompiler runs on the build machine and turns each GLSL pass into SPIR-V
# permutations plus reflection tables, written as C headers.
add_executable(relay_ffx_sc
    "${RELAY_FFX_TOOL}/src/ffx_sc.cpp"
    "${RELAY_FFX_TOOL}/src/glsl_compiler.cpp"
    "${RELAY_FFX_TOOL}/src/utils.cpp"
    "${RELAY_FFX_TOOL}/libs/MD5/md5.cpp"
    "${RELAY_FFX_TOOL}/libs/tiny-process-library/process.cpp"
    "$<IF:$<PLATFORM_ID:Windows>,${RELAY_FFX_TOOL}/libs/tiny-process-library/process_win.cpp,${RELAY_FFX_TOOL}/libs/tiny-process-library/process_unix.cpp>"
    "${RELAY_FFX_TOOL}/libs/SPIRV-Reflect/spirv_reflect.cpp"
)
target_compile_features(relay_ffx_sc PRIVATE cxx_std_17)
target_include_directories(relay_ffx_sc PRIVATE
    "${RELAY_FFX_TOOL}/src"
    "${RELAY_FFX_TOOL}/libs/MD5"
    "${RELAY_FFX_TOOL}/libs/SPIRV-Reflect"
    "${RELAY_FFX_TOOL}/libs/tiny-process-library")
find_package(Threads REQUIRED)
target_link_libraries(relay_ffx_sc PRIVATE Threads::Threads)
if(NOT MSVC)
    # Upstream code; keep Relay's build output readable.
    target_compile_options(relay_ffx_sc PRIVATE -w)
endif()

set(RELAY_FFX_SC_BASE -reflection -deps=gcc -DFFX_GPU=1
    -compiler=glslang -e CS --target-env vulkan1.2 -S comp -Os -DFFX_GLSL=1
    "-glslangexe=${RELAY_GLSLANG_VALIDATOR}")
set(RELAY_FFX_PERMUTATION_HEADERS)

# Mirrors compile_shaders_with_depfile from upstream's CMakeCompileShaders.txt: every pass is
# compiled for wave32 and wave64, each with and without 16-bit types.
function(relay_ffx_compile_shaders effect)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "" "PERMUTATIONS;INCLUDES")
    file(GLOB shaders CONFIGURE_DEPENDS "${RELAY_FFX_SDK}/src/backends/vk/shaders/${effect}/*.glsl")
    set(include_args)
    foreach(directory ${ARG_INCLUDES})
        list(APPEND include_args "-I${directory}")
    endforeach()
    set(outputs)
    foreach(shader ${shaders})
        get_filename_component(name "${shader}" NAME_WE)
        foreach(variant "" _wave64 _16bit _wave64_16bit)
            if(variant MATCHES "16bit")
                set(half 1)
            else()
                set(half 0)
            endif()
            set(header "${RELAY_FFX_PERMUTATIONS}/${name}${variant}_permutations.h")
            # Arguments go through a response file: a POSIX shell would brace-expand the
            # permutation lists (-DOPTION={0,1}) into separate arguments.
            set(arguments ${RELAY_FFX_SC_BASE} ${ARG_PERMUTATIONS} "-name=${name}${variant}"
                -DFFX_HALF=${half} ${include_args} "-output=${RELAY_FFX_PERMUTATIONS}" "${shader}")
            list(JOIN arguments "\n" arguments)
            set(response "${CMAKE_CURRENT_BINARY_DIR}/arguments/${name}${variant}.rsp")
            file(GENERATE OUTPUT "${response}" CONTENT "${arguments}\n")
            add_custom_command(
                OUTPUT "${header}"
                COMMAND relay_ffx_sc "@${response}"
                DEPENDS relay_ffx_sc "${shader}" "${response}"
                DEPFILE "${header}.d"
                COMMENT "Compiling FidelityFX ${name}${variant} permutations"
                VERBATIM)
            list(APPEND outputs "${header}")
        endforeach()
    endforeach()
    set(RELAY_FFX_PERMUTATION_HEADERS ${RELAY_FFX_PERMUTATION_HEADERS} ${outputs} PARENT_SCOPE)
endfunction()

relay_ffx_compile_shaders(brixelizer
    INCLUDES "${RELAY_FFX_GPU}" "${RELAY_FFX_GPU}/brixelizer")
relay_ffx_compile_shaders(brixelizergi
    PERMUTATIONS "-DFFX_BRIXELIZER_GI_OPTION_DEPTH_INVERTED={0,1}"
                 "-DFFX_BRIXELIZER_GI_OPTION_DISABLE_SPECULAR={0,1}"
                 "-DFFX_BRIXELIZER_GI_OPTION_DISABLE_DENOISER={0,1}"
    INCLUDES "${RELAY_FFX_GPU}" "${RELAY_FFX_GPU}/brixelizer" "${RELAY_FFX_GPU}/brixelizergi")
relay_ffx_compile_shaders(denoiser
    PERMUTATIONS "-DFFX_DENOISER_OPTION_INVERTED_DEPTH={0,1}"
    INCLUDES "${RELAY_FFX_GPU}" "${RELAY_FFX_GPU}/denoiser")
relay_ffx_compile_shaders(classifier
    PERMUTATIONS "-DFFX_CLASSIFIER_OPTION_INVERTED_DEPTH={0,1}"
                 "-DFFX_CLASSIFIER_OPTION_CLASSIFIER_MODE={0,1}"
    INCLUDES "${RELAY_FFX_GPU}" "${RELAY_FFX_GPU}/classifier")
relay_ffx_compile_shaders(spd
    PERMUTATIONS "-DFFX_SPD_OPTION_LINEAR_SAMPLE={0,1}"
                 "-DFFX_SPD_OPTION_WAVE_INTEROP_LDS={0,1}"
                 "-DFFX_SPD_OPTION_DOWNSAMPLE_FILTER={0,1,2}"
    INCLUDES "${RELAY_FFX_GPU}" "${RELAY_FFX_GPU}/spd")
add_custom_target(relay_ffx_shaders DEPENDS ${RELAY_FFX_PERMUTATION_HEADERS})

set(RELAY_FFX_EFFECTS brixelizer brixelizergi denoiser classifier spd)
set(RELAY_FFX_SOURCES
    "${RELAY_FFX_SDK}/src/shared/ffx_assert.cpp"
    "${RELAY_FFX_SDK}/src/shared/ffx_message.cpp"
    "${RELAY_FFX_SDK}/src/shared/ffx_object_management.cpp"
    "${RELAY_FFX_SDK}/src/shared/ffx_breadcrumbs_list.cpp"
    "${RELAY_FFX_SDK}/src/backends/shared/ffx_shader_blobs.cpp"
    "${RELAY_FFX_SDK}/src/backends/vk/ffx_vk.cpp"
    "${RELAY_FFX_SDK}/src/components/brixelizer/ffx_brixelizer.cpp"
    "${RELAY_FFX_SDK}/src/components/brixelizer/ffx_brixelizer_raw.cpp"
    "${RELAY_FFX_SDK}/src/components/brixelizergi/ffx_brixelizergi.cpp"
    "${RELAY_FFX_SDK}/src/components/denoiser/ffx_denoiser.cpp"
    "${RELAY_FFX_SDK}/src/components/classifier/ffx_classifier.cpp"
    "${RELAY_FFX_SDK}/src/components/spd/ffx_spd.cpp")
foreach(effect ${RELAY_FFX_EFFECTS})
    list(APPEND RELAY_FFX_SOURCES
        "${RELAY_FFX_SDK}/src/backends/shared/blob_accessors/ffx_${effect}_shaderblobs.cpp")
endforeach()

add_library(relay_fidelityfx STATIC ${RELAY_FFX_SOURCES})
add_dependencies(relay_fidelityfx relay_ffx_shaders)
target_compile_features(relay_fidelityfx PUBLIC cxx_std_20)
target_compile_definitions(relay_fidelityfx PRIVATE
    FFX_BRIXELIZER FFX_BRIXELIZER_GI FFX_DENOISER FFX_CLASSIFIER FFX_SPD)
target_include_directories(relay_fidelityfx SYSTEM PUBLIC "${RELAY_FFX_SDK}/include")
target_include_directories(relay_fidelityfx PRIVATE
    "${RELAY_FFX_SDK}/src/shared"
    "${RELAY_FFX_SDK}/src/backends/shared"
    "${RELAY_FFX_SDK}/src/components"
    "${RELAY_FFX_PERMUTATIONS}")
target_link_libraries(relay_fidelityfx PUBLIC Vulkan::Vulkan)
if(TARGET Vulkan::Headers)
    target_link_libraries(relay_fidelityfx PUBLIC Vulkan::Headers)
endif()
if(NOT MSVC)
    target_compile_options(relay_fidelityfx PRIVATE -w
        -include "${RELAY_FFX_ROOT}/relay/ffx_relay_compat.h")
endif()
set_target_properties(relay_fidelityfx PROPERTIES POSITION_INDEPENDENT_CODE ON)
