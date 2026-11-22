include(ExternalProject)

# The two engines pin different ggml versions. Give the worker its own build
# and DLL directory so both engines can use independently compiled backends.
set(xsr_whisper_version "1.9.3")
target_compile_definitions(xsr-engine PRIVATE XSR_WHISPER_VERSION="${xsr_whisper_version}")
FetchContent_Declare(whisper_cpp
    URL "https://codeload.github.com/ggml-org/whisper.cpp/tar.gz/refs/tags/v${xsr_whisper_version}"
    URL_HASH SHA256=1650f884effba487025143bd8facd2f9fb40a83b3737a732803c67a8d659d9c0
    SOURCE_SUBDIR xsr-source-only)
FetchContent_MakeAvailable(whisper_cpp)

set(whisper_build "${PROJECT_BINARY_DIR}/whisper-build")
set(whisper_bin "${whisper_build}/bin")
if(CMAKE_CONFIGURATION_TYPES)
    string(APPEND whisper_bin "/$<CONFIG>")
endif()
set(whisper_args)
foreach(variable IN ITEMS CMAKE_TOOLCHAIN_FILE CMAKE_C_COMPILER CMAKE_CXX_COMPILER CMAKE_RC_COMPILER
        CMAKE_BUILD_TYPE CMAKE_EXE_LINKER_FLAGS CMAKE_SHARED_LINKER_FLAGS CMAKE_MODULE_LINKER_FLAGS
        Vulkan_INCLUDE_DIR Vulkan_LIBRARY Vulkan_GLSLC_EXECUTABLE Vulkan_GLSLANG_VALIDATOR_EXECUTABLE SPIRV-Headers_DIR)
    if(DEFINED ${variable} AND NOT "${${variable}}" MATCHES "-NOTFOUND$")
        list(APPEND whisper_args "-D${variable}=${${variable}}")
    endif()
endforeach()
if(CMAKE_CROSSCOMPILING)
    list(APPEND whisper_args "-DCMAKE_SYSTEM_NAME=${CMAKE_SYSTEM_NAME}"
        "-DCMAKE_SYSTEM_PROCESSOR=${CMAKE_SYSTEM_PROCESSOR}")
endif()
ExternalProject_Add(xsr-whisper-build
    SOURCE_DIR "${PROJECT_SOURCE_DIR}/cmake/whisper-worker"
    BINARY_DIR "${whisper_build}"
    CMAKE_ARGS ${whisper_args}
        "-DXSR_VULKAN=${XSR_VULKAN}" "-DXSR_LINUX_AVX2=${XSR_LINUX_AVX2}"
        "-DWHISPER_SOURCE=${whisper_cpp_SOURCE_DIR}" "-DJSON_SOURCE=${json_SOURCE_DIR}"
        "-DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>"
    BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --config $<CONFIG>
        --target xsr-whisper --parallel 2
    BUILD_ALWAYS TRUE
    INSTALL_COMMAND "")
add_custom_target(xsr-whisper-runtime
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "$<TARGET_FILE_DIR:xsr-engine>/whisper"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${whisper_bin}" "$<TARGET_FILE_DIR:xsr-engine>/whisper"
    DEPENDS xsr-whisper-build)
add_dependencies(xsr-engine xsr-whisper-runtime)

if(WIN32 AND XSR_BUILD_PLUGINS)
    install(DIRECTORY "${whisper_bin}/" DESTINATION X-Speech-Rekon/bin/whisper COMPONENT xsr
        FILES_MATCHING PATTERN "xsr-whisper.exe" PATTERN "*.dll")
    install(FILES "${whisper_cpp_SOURCE_DIR}/LICENSE" DESTINATION licenses
        RENAME whisper.cpp.txt COMPONENT xsr)
    install(FILES "${whisper_cpp_SOURCE_DIR}/ggml/LICENSE" DESTINATION licenses
        RENAME whisper-ggml.txt COMPONENT xsr)
endif()
