cmake_minimum_required(VERSION 3.22)
project(dcorerouter VERSION 0.1.0 LANGUAGES C CXX OBJC OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "macOS deployment target")

include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.4
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(JUCE)

juce_add_gui_app(dcorerouter
    PRODUCT_NAME "D-Router"
    COMPANY_NAME "ZDAudio"
    COMPANY_COPYRIGHT "Copyright (C) ZDAudio"
    BUNDLE_ID com.zdaudio.drouter
    # App icon (Dock / Finder / About).  JUCE generates the .icns from this
    # 1024x1024 PNG and sets CFBundleIconFile automatically.
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/Resources/icon.png"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "D-Router routes audio between devices and needs microphone access."
)

juce_generate_juce_header(dcorerouter)

target_sources(dcorerouter PRIVATE
    Source/Main.cpp
    Source/MainComponent.cpp
    Source/Engine/SampleRateConverter.cpp
    Source/Engine/DeviceWorker.cpp
    Source/Engine/MatrixProcessor.cpp
    Source/Engine/WorkerPool.cpp
    Source/Engine/AudioEngine.cpp
    Source/Routing/RoutingMatrix.cpp
    Source/Routing/OutputGroupManager.cpp
    Source/Routing/InputGroupManager.cpp
    Source/UI/Crosspoint.cpp
    Source/UI/CrosspointGrid.cpp
    Source/UI/LookAndFeel.cpp
    Source/UI/MatrixView.cpp
    Source/UI/DeviceManagerDialog.cpp
    Source/UI/SettingsDialog.cpp
    Source/UI/GroupManagerDialog.cpp
    Source/UI/OutputGroupPanel.cpp
    Source/UI/StatusPanel.cpp
    Source/UI/LevelMeter.cpp
    Source/Persistence/SnapshotStore.cpp
    Source/Persistence/SettingsStore.cpp
    Source/Persistence/CrashGuard.cpp
    Source/Diagnostics/Logger.cpp
    Source/Diagnostics/CrashHandler.mm
    Source/Diagnostics/PerfMonitor.cpp
    Source/UI/LogViewerDialog.cpp
    Source/UI/LoadingOverlay.cpp
    Source/DSP/PluginHost.mm
    Source/DSP/MultiChannelPluginHost.mm
    Source/DSP/SafePluginOps.mm
    Source/DSP/Builtin/InternalPluginFormat.cpp
    Source/UI/PluginEditorWindow.mm
    Source/Update/UpdateChecker.cpp
    Source/Update/UpdateInstaller.cpp
)

target_include_directories(dcorerouter PRIVATE Source)

target_compile_definitions(dcorerouter PRIVATE
    JUCE_PLUGINHOST_AU=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:dcorerouter,JUCE_PROJECT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:dcorerouter,JUCE_VERSION>"
)

target_link_libraries(dcorerouter PRIVATE
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_core
    juce::juce_data_structures
    juce::juce_dsp
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
    juce::juce_opengl
    "-framework AudioToolbox"
    "-framework CoreAudio"
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)

# ===========================================================================
# Tests -- pure-logic regression suite (no JUCE, no audio device).  Runs
# headless in milliseconds; guards the SPSC ring buffer + the routing matrix
# (gains, mute/solo, the virtual-device self-loop block).
#   cmake --build build --target dcorerouter_tests
#   ctest --test-dir build --output-on-failure
# DSP tests that need JUCE (STFT/WOLA, gain staging) are a separate follow-up.
# ===========================================================================
enable_testing()
add_executable(dcorerouter_tests
    tests/test_main.cpp
    Source/Routing/RoutingMatrix.cpp
)
target_include_directories(dcorerouter_tests PRIVATE Source)
target_compile_features(dcorerouter_tests PRIVATE cxx_std_20)
add_test(NAME dcorerouter_tests COMMAND dcorerouter_tests)
