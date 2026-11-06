# gpt-live-speaker firmware (ESP-IDF >= 5.5, target esp32s3)
#
# Components:
#   - main/                       app (Wi-Fi, chatgpt.com probe, auth/session exchange, /realtime/wm signaling)
#   - components/*                discovered automatically (e.g. components/speaker_core, owned separately)
#   - managed_components/         pulled by the IDF Component Manager from main/idf_component.yml
#                                 (espressif/esp_peer from components.espressif.com)
#
# Optional: build against a local checkout of esp-webrtc-solution instead of the registry
# (needed later for esp_webrtc, which is not published on the registry):
#   idf.py -DGLS_WEBRTC_SOLUTION_DIR=$HOME/esp/esp-webrtc-solution build
cmake_minimum_required(VERSION 3.16)

if(DEFINED GLS_WEBRTC_SOLUTION_DIR)
    list(APPEND EXTRA_COMPONENT_DIRS "${GLS_WEBRTC_SOLUTION_DIR}/components/esp_webrtc")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(gpt_live_speaker)
