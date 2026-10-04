# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set(GADGET_SRCS
    "main.c"
    "app.c"
    "ble_server.c"
    "wifi_mgr.c"
    "wifi_known.c"
    "identity.c"
    "config_store.c"
    "vm_api.c"
    "http_proxy.c"
    "diagnostic_log.c"
    "stack_monitor.c"
    "bug_report.c"
    "ota.c"
    "image_fetch.c"
    "../avatar/happy_anim.c"
    "pixel_font.c"
    "button.c"
    "tunnel_netif.c"
    "net_discovery.c"
    "noise_control.cpp"
    "noise_tunnel.cpp"
    "link_pairing.c"
    "pairing_signer_policy.c"
    "pairing_transcript.c"
    "factory_test.c"
)

set(GADGET_REQUIRES
    nvs_flash
    esp_wifi
    esp_event
    esp_netif
    esp_http_client
    esp_https_ota
    app_update
    esp_app_format
    esp-tls
    noise_core
    product_info
    bt
    driver
    esp_driver_gpio
    esp_driver_i2c
    esp_driver_ledc
    esp_driver_spi
    esp_driver_uart
    esp_lcd
    lwip
    mbedtls
    efuse
    muse
    # The voice board's. Requirements are resolved before the configuration
    # is loaded, so this is unconditional; unused code is not linked.
    esp_driver_i2s
    esp_driver_pcnt
)

# E-paper has its own status screen in place of led_status.c.
if(CONFIG_HOMEHUB_LED_BACKEND_RETERMINAL_UC8179)
    list(APPEND GADGET_SRCS "epaper_status.c")
else()
    list(APPEND GADGET_SRCS "led_status.c")
endif()

if(CONFIG_MUSE_ENABLED)
    list(APPEND GADGET_SRCS "muse_glue.c")
endif()

if(CONFIG_MUSE_BOARD_PASSPORT)
    list(APPEND GADGET_SRCS "passport_bench.c")
endif()

if(CONFIG_HOMEHUB_LED_BACKEND_DEVKIT_GPIO27 OR CONFIG_HOMEHUB_LED_BACKEND_VOICE_RING)
    list(APPEND GADGET_REQUIRES led_strip)
endif()

if(CONFIG_HOMEHUB_SENSECAP_SENSORS)
    list(APPEND GADGET_SRCS "sensecap_sensors.c")
endif()

if(CONFIG_HOMEHUB_VOICE)
    list(APPEND GADGET_SRCS
        "voice.c"
        "voice_board.c"
        "voice_muse_chat.c"
        "voice_player.c"
    )
endif()

idf_component_register(
    SRCS ${GADGET_SRCS}
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS "../avatar"
    REQUIRES ${GADGET_REQUIRES}
)

# MUSE_HTTP_PROXY_HOST / MUSE_HTTP_PROXY_PORT are read at build time.
# The stamp is always touched so a later build sees a changed environment.
# The header itself is rewritten only when the address or port changes.
set(MUSE_PROXY_ENV_H "${CMAKE_BINARY_DIR}/generated/muse_http_proxy_env.h")
set(MUSE_PROXY_ENV_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/../cmake/write_proxy_env.cmake")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")
execute_process(
    COMMAND ${CMAKE_COMMAND} -DOUT_FILE=${MUSE_PROXY_ENV_H} -P ${MUSE_PROXY_ENV_SCRIPT}
    RESULT_VARIABLE muse_proxy_env_rc
)
if(NOT muse_proxy_env_rc EQUAL 0)
    message(FATAL_ERROR "failed to read MUSE_HTTP_PROXY_HOST")
endif()
add_custom_command(
    OUTPUT "${MUSE_PROXY_ENV_H}.stamp"
    COMMAND ${CMAKE_COMMAND} -DOUT_FILE=${MUSE_PROXY_ENV_H} -P ${MUSE_PROXY_ENV_SCRIPT}
    COMMAND ${CMAKE_COMMAND} -E touch "${MUSE_PROXY_ENV_H}.stamp"
    DEPENDS "${MUSE_PROXY_ENV_SCRIPT}"
    COMMENT "Applying MUSE_HTTP_PROXY_HOST"
    VERBATIM
)
add_custom_target(muse_http_proxy_env DEPENDS "${MUSE_PROXY_ENV_H}.stamp")
add_dependencies(${COMPONENT_LIB} muse_http_proxy_env)
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_BINARY_DIR}/generated")
set_source_files_properties(http_proxy.c vm_api.c noise_control.cpp
    PROPERTIES OBJECT_DEPENDS "${MUSE_PROXY_ENV_H}")
