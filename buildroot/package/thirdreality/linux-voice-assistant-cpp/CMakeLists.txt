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

# libmpv: media playback for the MediaPlayerEntity (phase 3a) and
# the TTS announce player (phase 4). Provides libmpv.so.2.
pkg_check_modules(MPV REQUIRED IMPORTED_TARGET mpv)
message(STATUS "Found mpv ${MPV_VERSION} via pkg-config")

# libpulse-simple: synchronous PulseAudio capture API used by
# AudioCapture (phase 3b). The "simple" wrapper is sufficient — we
# only need a blocking read in a dedicated thread.
pkg_check_modules(PULSE_SIMPLE REQUIRED IMPORTED_TARGET libpulse-simple)
message(STATUS "Found libpulse-simple ${PULSE_SIMPLE_VERSION} via pkg-config")

# webrtc-audio-processing 1.x: AGC + NoiseSuppression for the mic
# pipeline (phase 6). Without this, wake-word detection rate is
# noticeably worse than the Python build (which uses
# webrtc-noise-gain). The pkg-config name is "webrtc-audio-processing-1".
pkg_check_modules(WEBRTC_APM REQUIRED IMPORTED_TARGET webrtc-audio-processing-1)
message(STATUS "Found webrtc-audio-processing ${WEBRTC_APM_VERSION} via pkg-config")

# libcurl: HTTPS device tickets and WSS session transport.
pkg_check_modules(LIBCURL REQUIRED IMPORTED_TARGET libcurl)
message(STATUS "Found libcurl ${LIBCURL_VERSION} via pkg-config")

# alsa-lib: required by the aec_loopback_test dev tool, which opens
# the hardware loopback capture device (e.g. plughw:0,4) directly.
# A future change will also use this in the production AudioCapture
# for the ALSA capture backend that drives hardware-loopback AEC.
pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)
message(STATUS "Found alsa-lib ${ALSA_VERSION} via pkg-config")

# ---------------------------------------------------------------------------
# Vendored microfrontend + kissfft (phase 4 audio feature extractor)
# ---------------------------------------------------------------------------
# These C/C++ sources are the same TFLite Micro `microfrontend` and
# kissfft fixed-point sources pymicro-features v2.0.2 ships. We
# build them as a separate object library so we can apply
# microfrontend-specific compile flags (notably -DFIXED_POINT=16
# which kissfft hard-requires) without polluting the rest of the
# project's flags.
set(MICROFRONTEND_DIR
    "${CMAKE_SOURCE_DIR}/third_party/microfrontend/include")

set(MICROFRONTEND_SOURCES
    ${MICROFRONTEND_DIR}/tensorflow/lite/experimental/microfrontend/lib/fft.cc
    ${MICROFRONTEND_DIR}/tensorflow/lite/experimental/microfrontend/lib/fft_util.cc
    ${MICROFRONTEND_DIR}/tensorflow/lite/experimental/microfrontend/lib/filterbank.cc
    ${MICROFRONTEND_DIR}/tensorflow/lite/experimental/microfrontend/lib/filterbank_util.cc
    ${MICROFRONTEND_DIR}/tensorflow/lite/experimental/microfrontend/lib/frontend.cc
    ${MICROFRONTEND_DIR}/tensorflow/lite/experimental/microfrontend/lib/frontend_util.cc
    ${MICROFRONTEND_DIR}/tensorflow/lite/experimental/microfrontend/lib/kiss_fft_int16.cc
    ${MICROFRONTEND_DIR}/tensorflow/lite/experimental/microfrontend/lib/log_lut.cc
    ${MICROFRONTEND_DIR}/tensorflow/lite/experimental/microfrontend/lib/log_scale.cc
    ${MICROFRONTEND_DIR}/tensorflow/lite/experimental/microfrontend/lib/log_scale_util.cc
    ${MICROFRONTEND_DIR}/tensorflow/lite/experimental/microfrontend/lib/noise_reduction.cc
    ${MICROFRONTEND_DIR}/tensorflow/lite/experimental/microfrontend/lib/noise_reduction_util.cc
    ${MICROFRONTEND_DIR}/tensorflow/lite/experimental/microfrontend/lib/pcan_gain_control.cc
    ${MICROFRONTEND_DIR}/tensorflow/lite/experimental/microfrontend/lib/pcan_gain_control_util.cc
    ${MICROFRONTEND_DIR}/tensorflow/lite/experimental/microfrontend/lib/window.cc
    ${MICROFRONTEND_DIR}/tensorflow/lite/experimental/microfrontend/lib/window_util.cc
)

add_library(lva_microfrontend OBJECT ${MICROFRONTEND_SOURCES})
target_include_directories(lva_microfrontend PUBLIC
    ${MICROFRONTEND_DIR}
    ${MICROFRONTEND_DIR}/kissfft
)
target_compile_definitions(lva_microfrontend PUBLIC FIXED_POINT=16)
target_compile_options(lva_microfrontend PRIVATE
    -Wno-unused-parameter
    -Wno-unused-but-set-variable
    -Wno-unused-variable
    -Wno-pedantic
    -Wno-sign-compare
)

add_executable(linux-voice-assistant-cpp
    src/main.cpp
    src/config/EndpointConfig.cpp
    src/cortana/CurlSessionTransport.cpp
    src/cortana/DeviceTicket.cpp
    src/cortana/DeviceTicketClient.cpp
    src/cortana/Protocol.cpp
    src/cortana/SessionClient.cpp
    src/state/Preferences.cpp
    src/audio/LibMpvPlayer.cpp
    src/audio/PcmRingBuffer.cpp
    src/audio/AudioCapture.cpp
    src/audio/ExternalWakeWord.cpp
    src/audio/MicroFeatures.cpp
    src/audio/MicroWakeWord.cpp
    src/audio/OpenWakeWord.cpp
    src/audio/OpenWakeWordFeatures.cpp
    src/audio/TfliteRuntime.cpp
    src/audio/WakeWordEngine.cpp
    src/audio/WakeWordScanner.cpp
    src/audio/WebRtcProcessor.cpp
    src/tr/HomeButton.cpp
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
        lva_microfrontend
        nlohmann_json::nlohmann_json
        PkgConfig::MPV
        PkgConfig::PULSE_SIMPLE
        PkgConfig::ALSA
        PkgConfig::WEBRTC_APM
        PkgConfig::LIBCURL
        Threads::Threads
        ${CMAKE_DL_LIBS}   # for dlopen/dlsym in TfliteRuntime
)

install(TARGETS linux-voice-assistant-cpp RUNTIME DESTINATION bin)

# ---------------------------------------------------------------------------
# Dev tool: micro_features_dump (phase 4a verification)
# ---------------------------------------------------------------------------
# Small CLI that reads a 16 kHz mono S16LE WAV and prints C++
# MicroFeatures output as text — one frame per line, 40 floats. The
# host-side `test/verify_phase4a_features.py` compares against
# pymicro_features.
add_executable(micro_features_dump
    src/tools/micro_features_dump.cpp
    src/audio/MicroFeatures.cpp
    src/util/Log.cpp
)
target_include_directories(micro_features_dump PRIVATE
    ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(micro_features_dump PRIVATE
    lva_microfrontend
    Threads::Threads
)
install(TARGETS micro_features_dump RUNTIME DESTINATION bin)

# Dev tool: tflite_inspect (phase 4b verification)
# Prints shape/type/quant of all input/output tensors of a .tflite.
add_executable(tflite_inspect
    src/tools/tflite_inspect.cpp
    src/audio/TfliteRuntime.cpp
    src/util/Log.cpp
)
target_include_directories(tflite_inspect PRIVATE
    ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tflite_inspect PRIVATE
    Threads::Threads
    ${CMAKE_DL_LIBS}
)
install(TARGETS tflite_inspect RUNTIME DESTINATION bin)

# Dev tool: wake_word_dump (phase 4c verification)
# Loads a microWakeWord JSON config and runs a WAV through the full
# C++ pipeline (features → quantize → invoke → window). Emits one
# line per invoke for cross-comparison with pymicro_wakeword.
add_executable(wake_word_dump
    src/tools/wake_word_dump.cpp
    src/audio/MicroFeatures.cpp
    src/audio/MicroWakeWord.cpp
    src/audio/TfliteRuntime.cpp
    src/util/Log.cpp
)
target_include_directories(wake_word_dump PRIVATE
    ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(wake_word_dump PRIVATE
    lva_microfrontend
    nlohmann_json::nlohmann_json
    Threads::Threads
    ${CMAKE_DL_LIBS}
)
install(TARGETS wake_word_dump RUNTIME DESTINATION bin)

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
