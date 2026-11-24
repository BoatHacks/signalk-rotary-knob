# SPDX-License-Identifier: Apache-2.0
#
# SignalK rotary knob firmware for the Elecrow CrowPanel 1.28" rotary display (ESP32-S3), built on espOS.
#
# Plain ESP-IDF plus espOS from the component registry, without espOS's project prologue (that needs an espOS checkout).
# What the prologue would set is in sdkconfig.defaults, where espos_core's configure-time lint checks it. Adapted from
# espOS's from_registry example (Apache-2.0, copyright Dirk Wahrheit), see NOTICE.
cmake_minimum_required(VERSION 3.22)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# The project name is the app name a running knob reports (GET /api/v1/system/ping, mDNS). The espOS registry and the
# OTA manifest match updates on it, so it must not change once boards are in the field.
project(signalk-rotary-knob)

# The web UI bundle -> the LittleFS "storage" partition, written by `idf.py flash`. Defined by espos_httpd's
# project_include.cmake; called after project() because littlefs_create_partition_image() comes from a managed component.
espos_project_ui_partition()
