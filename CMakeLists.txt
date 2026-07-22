# Standalone modules (multi-host BLE helpers). ZMK public headers are PRIVATE
# on the ZMK target, so external modules must add app/include explicitly.
if(CONFIG_TOTEM_EXCLUSIVE_HOST OR CONFIG_TOTEM_LAZY_INACTIVE_HOST OR CONFIG_TOTEM_RECONNECT_WATCH
   OR CONFIG_TOTEM_HOST_EVENT_LOG)
  zephyr_library()
  zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)
  zephyr_library_include_directories(${CMAKE_CURRENT_LIST_DIR}/include)
endif()

if(CONFIG_TOTEM_EXCLUSIVE_HOST)
  zephyr_library_sources(src/exclusive_host.c)
endif()

if(CONFIG_TOTEM_LAZY_INACTIVE_HOST)
  zephyr_library_sources(src/lazy_inactive_host.c)
endif()

if(CONFIG_TOTEM_RECONNECT_WATCH)
  zephyr_library_sources(src/reconnect_watch.c)
endif()

if(CONFIG_TOTEM_HOST_EVENT_LOG)
  zephyr_library_sources(src/host_event_log.c)
endif()

# Note: TOTEM_ADV_* / DIR_THEN_OPEN / FAL / reselect live in patches/zmk-ble.patch.
# TOTEM_EXCLUSIVE_HOST vs TOTEM_LAZY_INACTIVE_HOST are mutually exclusive policies.
