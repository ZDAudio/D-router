cmake_minimum_required(VERSION 3.22)
project(dcorerouter VERSION 0.9.1 LANGUAGES C CXX OBJC OBJCXX)

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
    # Core Audio process taps (app-audio capture, macOS 14.4+) are gated by the
    # audio-capture TCC permission; THIS key drives the system prompt.  Without it the
    # tap is created but delivers pure silence (every PCM sample 0.0f) and no prompt
    # ever appears -- the exact symptom we chased for hours (2026-06-24).
    PLIST_TO_MERGE "<plist version=\"1.0\"><dict><key>NSAudioCaptureUsageDescription</key><string>D-Router captures the audio of apps you select and routes it through the routing matrix.</string></dict></plist>"
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
    Source/Engine/DeviceVolume.cpp
    Source/Engine/SystemAudioDevices.cpp
    Source/Engine/ReconfigurationController.cpp
    Source/Engine/AppAudioProcesses.mm
    Source/Engine/AppAudioWorker.mm
    Source/Routing/RoutingMatrix.cpp
    Source/Routing/PanicController.cpp
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
    Source/UI/DeviceVolumePanel.cpp
    Source/UI/PanelHost.cpp
    Source/UI/LevelMeter.cpp
    Source/Persistence/SnapshotStore.cpp
    Source/Persistence/SettingsStore.cpp
    Source/Persistence/AtomicXmlWrite.cpp
    Source/Persistence/CrashGuard.cpp
    Source/Diagnostics/Logger.cpp
    Source/Diagnostics/CrashHandler.mm
    Source/Diagnostics/AppNap.mm
    Source/Diagnostics/PerfMonitor.cpp
    Source/UI/LogViewerDialog.cpp
    Source/UI/LoadingOverlay.cpp
    Source/DSP/PluginHost.mm
    Source/DSP/MultiChannelPluginHost.mm
    Source/DSP/SafePluginOps.mm
    Source/DSP/Builtin/InternalPluginFormat.cpp
    Source/DSP/Builtin/CoreAudioAacWriter.cpp
    Source/UI/PluginEditorWindow.mm
    Source/DSP/Builtin/StereoMeterEditor.mm
    Source/Update/UpdateChecker.cpp
    Source/Update/UpdateInstaller.cpp
)

target_include_directories(dcorerouter PRIVATE Source)

# The Metal meter view is new Objective-C++; compile it with ARC so the
# Metal / MTKView object graph is managed automatically. (The project's other
# .mm files predate it and use manual retain/release, so ARC is scoped per-file.)
set_source_files_properties(Source/DSP/Builtin/StereoMeterEditor.mm
    PROPERTIES COMPILE_FLAGS "-fobjc-arc")

# App-audio capture is new Objective-C++; compile with ARC so the CoreAudio tap
# objects (CATapDescription) and NSWorkspace observer tokens are managed
# automatically, like StereoMeterEditor above.
set_source_files_properties(Source/Engine/AppAudioProcesses.mm Source/Engine/AppAudioWorker.mm
    PROPERTIES COMPILE_FLAGS "-fobjc-arc")

# App Nap disabler holds an NSProcessInfo activity token; ARC manages its
# lifetime via CFBridgingRetain/Release (same pattern as the files above).
set_source_files_properties(Source/Diagnostics/AppNap.mm
    PROPERTIES COMPILE_FLAGS "-fobjc-arc")

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
    "-framework Metal"
    "-framework MetalKit"
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
    tests/CoreLogicTests.cpp
    Source/Routing/RoutingMatrix.cpp
    Source/Routing/PanicController.cpp
)
target_include_directories(dcorerouter_tests PRIVATE Source)
target_compile_features(dcorerouter_tests PRIVATE cxx_std_20)
add_test(NAME dcorerouter_tests COMMAND dcorerouter_tests)

# ---- JUCE-linked tests (persistence etc.; need ValueTree/XML/File) ---------
juce_add_console_app(dcorerouter_tests_juce PRODUCT_NAME "dcorerouter_tests_juce")
juce_generate_juce_header(dcorerouter_tests_juce)

target_sources(dcorerouter_tests_juce PRIVATE
    tests/juce/TestMainJuce.cpp
    tests/juce/SnapshotStoreTests.cpp
    tests/juce/SettingsStoreTests.cpp
    tests/juce/AtomicXmlWriteTests.cpp
    tests/juce/CrashHandlerTests.cpp
    tests/juce/ReconfigurationControllerTests.cpp
    Source/Engine/ReconfigurationController.cpp
    Source/Persistence/SnapshotStore.cpp
    Source/Persistence/SettingsStore.cpp
    Source/Persistence/AtomicXmlWrite.cpp
    Source/Diagnostics/CrashHandler.mm
    Source/Diagnostics/Logger.cpp
)
target_include_directories(dcorerouter_tests_juce PRIVATE Source)
target_compile_definitions(dcorerouter_tests_juce PRIVATE
    JUCE_PLUGINHOST_AU=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
)
target_link_libraries(dcorerouter_tests_juce PRIVATE
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
    "-framework Foundation"
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)
add_test(NAME dcorerouter_tests_juce COMMAND dcorerouter_tests_juce)
