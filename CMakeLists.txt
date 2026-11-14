cmake_minimum_required(VERSION 3.25.0)

if(APPLE AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "macOS deployment target")
endif()

# Disable VST3 SDK sample plugins while keeping validator & moduleinfotool
set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VSTGUI_SUPPORT ON CACHE BOOL "" FORCE)

if(APPLE)
    if(NOT DEFINED SMTG_BUILD_UNIVERSAL_BINARY)
        set(SMTG_BUILD_UNIVERSAL_BINARY OFF CACHE BOOL "" FORCE)
    endif()
    # Allow building with Apple Command Line Tools when full Xcode.app is not installed
    if(NOT DEFINED ENV{XCODE_VERSION})
        execute_process(COMMAND xcodebuild -version OUTPUT_VARIABLE _xc_out ERROR_QUIET RESULT_VARIABLE _xc_rc)
        if(NOT _xc_rc EQUAL 0)
            set(ENV{XCODE_VERSION} "16.0")
            set(XCODE_VERSION "16.0")
        endif()
    endif()
endif()

if(APPLE)
    project(RetroTVVideo
        VERSION 1.0.0
        DESCRIPTION "Video displaying plugin for audio DAWs"
        LANGUAGES C CXX OBJC OBJCXX
    )
else()
    project(RetroTVVideo
        VERSION 1.0.0
        DESCRIPTION "Video displaying plugin for audio DAWs"
        LANGUAGES C CXX
    )
endif()

# ---------------------------------------------------------------------------
# Platform-native Media Backend (Zero external FFmpeg dylibs)
#   macOS:   AVFoundation + CoreMedia + CoreVideo + Accelerate
#   Windows: Microsoft Media Foundation
# ---------------------------------------------------------------------------
if(APPLE)
    set(MEDIA_BACKEND_SOURCES
        source/videoengine_backend.h
        source/videoengine_mac.mm
    )
    set_source_files_properties(source/videoengine_mac.mm PROPERTIES
        COMPILE_FLAGS "-fobjc-arc"
    )
    set(MEDIA_BACKEND_LIBS
        "-framework Foundation"
        "-framework CoreGraphics"
        "-framework AVFoundation"
        "-framework CoreMedia"
        "-framework CoreVideo"
        "-framework Accelerate"
        "-lobjc"
    )
elseif(WIN32)
    set(MEDIA_BACKEND_SOURCES
        source/videoengine_backend.h
        source/videoengine_win.cpp
    )
    set(MEDIA_BACKEND_LIBS
        mfplat
        mfreadwrite
        mfuuid
        ole32
        propsys
    )
else()
    message(FATAL_ERROR "Unsupported platform: native video backend supports macOS and Windows.")
endif()

# ---------------------------------------------------------------------------
# Steinberg VST3 SDK
# ---------------------------------------------------------------------------
set(vst3sdk_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vst3sdk")
add_subdirectory("${vst3sdk_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/vst3sdk")
smtg_enable_vst3_sdk()

# ---------------------------------------------------------------------------
# RetroTVVideo VST3 Plugin Target
# ---------------------------------------------------------------------------
set(target RetroTVVideo)

smtg_add_vst3plugin(${target}
    PACKAGE_NAME "RetroTVVideo"
    SOURCES_LIST
        ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/vstsinglecomponenteffect.cpp
        ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/vstsinglecomponenteffect.h
        source/version.h
        source/pluginids.h
        source/videoengine.h
        source/videoengine.cpp
        ${MEDIA_BACKEND_SOURCES}
        source/retrotvplugin.h
        source/retrotvplugin.cpp
        source/retrotveditor.h
        source/retrotveditor.cpp
        source/factory.cpp
)

smtg_target_configure_version_file(${target})

target_compile_features(${target}
    PUBLIC
        cxx_std_17
)

target_link_libraries(${target}
    PRIVATE
        sdk
        vstgui_support
        ${MEDIA_BACKEND_LIBS}
)

if(APPLE)
    smtg_target_set_bundle(${target}
        BUNDLE_IDENTIFIER "com.reasonvideo.vst3.retrotv"
        COMPANY_NAME "flx.agency"
    )
    # Bundle assets, re-seal adhoc signature, copy a real bundle, and refresh mtime so DAWs like Reason rescan cleanly
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_BUNDLE_DIR:${target}>/Contents/Resources"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/practice-crazy.mp4"
            "$<TARGET_BUNDLE_DIR:${target}>/Contents/Resources/practice-crazy.mp4"
        COMMAND codesign --force --sign - "$<TARGET_BUNDLE_DIR:${target}>"
        COMMAND rm -rf "$ENV{HOME}/Library/Audio/Plug-Ins/VST3/RetroTVVideo.vst3"
        COMMAND ditto "$<TARGET_BUNDLE_DIR:${target}>" "$ENV{HOME}/Library/Audio/Plug-Ins/VST3/RetroTVVideo.vst3"
        COMMAND touch -c "$ENV{HOME}/Library/Audio/Plug-Ins/VST3/RetroTVVideo.vst3" "$ENV{HOME}/Library/Audio/Plug-Ins/VST3/RetroTVVideo.vst3/Contents/MacOS/RetroTVVideo"
        COMMENT "Bundling resources, re-signing, & installing RetroTVVideo.vst3 to ~/Library/Audio/Plug-Ins/VST3/"
    )
endif()

# ---------------------------------------------------------------------------
# Engine Test Executable
# ---------------------------------------------------------------------------
add_executable(RetroTVVideoTests
    source/videoengine.h
    source/videoengine.cpp
    ${MEDIA_BACKEND_SOURCES}
    tests/test_video_engine.cpp
)
target_include_directories(RetroTVVideoTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/source")
target_compile_features(RetroTVVideoTests PUBLIC cxx_std_17)
target_link_libraries(RetroTVVideoTests PRIVATE ${MEDIA_BACKEND_LIBS})
