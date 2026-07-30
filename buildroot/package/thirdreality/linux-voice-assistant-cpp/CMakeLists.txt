cmake_minimum_required(VERSION 3.16)
project(linux-voice-assistant-cpp
        VERSION 0.0.1
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Wpedantic)

# ---------------------------------------------------------------------------
# Main binary
# ---------------------------------------------------------------------------

find_package(PkgConfig REQUIRED)

# nlohmann_json: header-only JSON parser. Installed by the
# json-for-modern-cpp Buildroot package (staging only — no runtime
# cost). Provides the imported target `nlohmann_json::nlohmann_json`.
find_package(nlohmann_json CONFIG REQUIRED)

# libpulse-simple: retained for the bounded raw playback implementation.
pkg_check_modules(PULSE_SIMPLE REQUIRED IMPORTED_TARGET libpulse-simple)
message(STATUS "Found libpulse-simple ${PULSE_SIMPLE_VERSION} via pkg-config")

# webrtc-audio-processing 1.x: AEC, AGC, and noise suppression for the
# continuous microphone pipeline. The pkg-config name is
# "webrtc-audio-processing-1".
pkg_check_modules(WEBRTC_APM REQUIRED IMPORTED_TARGET webrtc-audio-processing-1)
message(STATUS "Found webrtc-audio-processing ${WEBRTC_APM_VERSION} via pkg-config")

# libcurl: HTTPS device tickets and WSS session transport.
pkg_check_modules(LIBCURL REQUIRED IMPORTED_TARGET libcurl)
message(STATUS "Found libcurl ${LIBCURL_VERSION} via pkg-config")

# alsa-lib: production capture and the AEC loopback diagnostic.
pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)
message(STATUS "Found alsa-lib ${ALSA_VERSION} via pkg-config")

add_executable(linux-voice-assistant-cpp
    src/main.cpp
    src/config/EndpointConfig.cpp
    src/cortana/CurlSessionTransport.cpp
    src/cortana/DeviceTicket.cpp
    src/cortana/DeviceTicketClient.cpp
    src/cortana/EndpointRuntime.cpp
    src/cortana/EndpointState.cpp
    src/cortana/Protocol.cpp
    src/cortana/SessionClient.cpp
    src/state/Preferences.cpp
    src/audio/PulseAudioSink.cpp
    src/audio/RawPcmPlayer.cpp
    src/audio/PcmRingBuffer.cpp
    src/audio/AudioCapture.cpp
    src/audio/CaptureFrame.cpp
    src/audio/CapturePipeline.cpp
    src/audio/MicrophoneIngress.cpp
    src/audio/WebRtcProcessor.cpp
    src/tr/HomeButton.cpp
    src/tr/EndpointLedPolicy.cpp
    src/tr/LedController.cpp
    src/tr/MicMuteGpio.cpp
    src/tr/SysInfo.cpp
    src/tr/SystemVolume.cpp
    src/util/Log.cpp
)

target_include_directories(linux-voice-assistant-cpp
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
)

# Threading is needed for the SIGTERM wait helper, Cortana's single-owner
# network worker, and the audio capture thread.
find_package(Threads REQUIRED)

target_link_libraries(linux-voice-assistant-cpp
    PRIVATE
        nlohmann_json::nlohmann_json
        PkgConfig::PULSE_SIMPLE
        PkgConfig::ALSA
        PkgConfig::WEBRTC_APM
        PkgConfig::LIBCURL
        Threads::Threads
)

install(TARGETS linux-voice-assistant-cpp RUNTIME DESTINATION bin)

# Dev tool: aec_loopback_test
# Verifies hardware-loopback AEC using the same WebRTC APM 1.3 pipeline
# the production capture path will use once the ALSA backend lands.
# Opens an ALSA capture device (default plughw:0,4) at S16_LE 16 kHz 4ch,
# splits ch1->mic, (ch3+ch4)/2->ref, runs ProcessReverseStream then
# ProcessStream, and writes raw_mic.wav / ref.wav / aec_out.wav for
# A/B comparison. Prints per-second ERLE.
#
# Standalone TU: does not link the production AudioCapture or
# WebRtcProcessor, so the production binary is unaffected. Run with
# linux-voice-assistant-cpp stopped (or at least with no concurrent
# reader on hw:0,4 if your codec driver is exclusive).
add_executable(aec_loopback_test
    src/tools/aec_loopback_test.cpp
)
target_include_directories(aec_loopback_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(aec_loopback_test PRIVATE
    PkgConfig::ALSA
    PkgConfig::WEBRTC_APM
    Threads::Threads
)
install(TARGETS aec_loopback_test RUNTIME DESTINATION bin)
