cmake_minimum_required(VERSION 3.24)

# The VERSION file is the single source of the product version. It feeds
# project(), the generated SmackMyRezUpVersion.h (window title, logs) and the
# VERSIONINFO resource stamped into both executables.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" SMRU_VERSION LIMIT_COUNT 1)
string(STRIP "${SMRU_VERSION}" SMRU_VERSION)
if(NOT SMRU_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "VERSION must contain MAJOR.MINOR.PATCH (found '${SMRU_VERSION}').")
endif()

project(SmackMyRezUp VERSION ${SMRU_VERSION} LANGUAGES CXX RC)

if(NOT WIN32)
    message(FATAL_ERROR "SmackMyRezUp currently targets Windows x64 / D3D12 only.")
endif()
if(NOT MSVC)
    message(FATAL_ERROR "Use Visual Studio 2022 / MSVC for this build.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# --- NVIDIA DLSS SDK: NGX headers + the driver-core import library ----------
set(DLSS_SDK "${CMAKE_SOURCE_DIR}/external/DLSS" CACHE PATH "Path to NVIDIA/DLSS SDK checkout")
if(NOT EXISTS "${DLSS_SDK}/include/nvsdk_ngx.h")
    message(FATAL_ERROR "NVIDIA DLSS SDK not found at ${DLSS_SDK}. Run build.ps1; it clones the official NVIDIA/DLSS repository automatically.")
endif()
set(DLSS_LIB_DIR "${DLSS_SDK}/lib/Windows_x86_64/x64")
# nvngx_dlss.dll (DLSS SR) is neither staged nor embedded: the direct-NR
# pipeline never creates the SR feature (upscale = resize + 1:1 neural pass).

# --- Generated version header (included by src/AppIdentity.h and the .rc) ----
set(SMRU_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
configure_file(src/Version.h.in "${SMRU_GENERATED_DIR}/SmackMyRezUpVersion.h" @ONLY)

# --- Build settings shared by every target ---------------------------------
add_library(SmackMyRezUpSettings INTERFACE)
target_compile_definitions(SmackMyRezUpSettings INTERFACE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
)
target_compile_options(SmackMyRezUpSettings INTERFACE
    $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /EHsc /Zc:__cplusplus>
)
target_include_directories(SmackMyRezUpSettings INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${SMRU_GENERATED_DIR}"
    "${DLSS_SDK}/include"
)
target_link_directories(SmackMyRezUpSettings INTERFACE "${DLSS_LIB_DIR}")

# --- Core: decode -> temporal guides -> D3D12/NGX neural pass ----------------
# Compiled once and linked into both executables.
add_library(SmackMyRezUpCore STATIC
    src/AppIdentity.h
    src/AppPaths.h
    src/ChildProcess.h
    src/TextEncoding.h
    src/Log.h
    src/CubeLUT.h
    src/LabelStamp.h
    src/VideoDecoder.cpp
    src/VideoDecoder.h
    src/NeuralEngine.cpp
    src/NeuralEngine.h
    src/D3D12Renderer.cpp
    src/D3D12Renderer.h
    src/FramePipeline.cpp
    src/FramePipeline.h
    src/TemporalGuides.cpp
    src/TemporalGuides.h
)
target_link_libraries(SmackMyRezUpCore
    PUBLIC SmackMyRezUpSettings
           d3d12 dxgi dxguid d3dcompiler
           mfplat mfreadwrite mfuuid
           ole32 shell32 shlwapi user32 gdi32
           nvsdk_ngx_d
)

# --- SmackMyRezUpExport: headless console exporter ---------------------------
# Raw stdin/stdout frame piping, --serve batch mode and file mode. Emitted into
# payload/ so the player can embed it.
add_executable(SmackMyRezUpExport
    src/ExportMain.cpp
    src/SmackMyRezUpExport.rc
    src/VersionInfo.rc
)
target_link_libraries(SmackMyRezUpExport PRIVATE SmackMyRezUpCore)

# --- SmackMyRezUp: the GUI player ---------------------------------------------
add_executable(SmackMyRezUp WIN32
    src/PlayerMain.cpp
    src/AudioPlayer.cpp
    src/AudioPlayer.h
    src/Localization.h
    src/SmackMyRezUp.rc
    src/VersionInfo.rc
)
target_link_libraries(SmackMyRezUp PRIVATE
    SmackMyRezUpCore
    comdlg32 comctl32 winmm dwmapi
)

# VersionInfo.rc is #included by the two target .rc files; listing it keeps it
# visible in IDEs, HEADER_FILE_ONLY keeps it out of the resource compiler.
set_source_files_properties(src/VersionInfo.rc PROPERTIES HEADER_FILE_ONLY ON)

# The player embeds payload/ (see src/SmackMyRezUp.rc); keep the exporter copy
# there fresh and make sure it is built before the player's resources compile.
add_dependencies(SmackMyRezUp SmackMyRezUpExport)
add_custom_command(TARGET SmackMyRezUpExport POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:SmackMyRezUpExport>"
        "${CMAKE_SOURCE_DIR}/payload/SmackMyRezUpExport.exe"
)

# build.ps1 stages FFmpeg into external/ffmpeg/bin. Kept optional so CMake can
# be configured before FFmpeg has been downloaded.
set(FFMPEG_STAGED_DIR "${CMAKE_SOURCE_DIR}/external/ffmpeg/bin")
if(EXISTS "${FFMPEG_STAGED_DIR}/ffmpeg.exe" AND EXISTS "${FFMPEG_STAGED_DIR}/ffprobe.exe")
    add_custom_command(TARGET SmackMyRezUp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FFMPEG_STAGED_DIR}/ffmpeg.exe"
            "$<TARGET_FILE_DIR:SmackMyRezUp>/ffmpeg.exe"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FFMPEG_STAGED_DIR}/ffprobe.exe"
            "$<TARGET_FILE_DIR:SmackMyRezUp>/ffprobe.exe"
    )
endif()
