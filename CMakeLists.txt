# Standalone modules (multi-host BLE helpers). ZMK public headers are PRIVATE
# on the ZMK target, so external modules must add app/include explicitly.
if(CONFIG_TOTEM_EXCLUSIVE_HOST OR CONFIG_TOTEM_LAZY_INACTIVE_HOST OR CONFIG_TOTEM_RECONNECT_WATCH
   OR CONFIG_TOTEM_HOST_EVENT_LOG OR CONFIG_TOTEM_WATCHDOG OR CONFIG_TOTEM_RESET_REASON_CAPTURE
   OR CONFIG_TOTEM_BOOT_PROFILE_DUMP OR CONFIG_TOTEM_FAULT_CAPTURE
   OR CONFIG_TOTEM_ENDPOINT_LOG)
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

# Watchdog and reset-reason live in one file: the reset reason is read at boot by
# the same SYS_INIT that arms the dog, and is still captured when the dog is off.
if(CONFIG_TOTEM_WATCHDOG OR CONFIG_TOTEM_RESET_REASON_CAPTURE
   OR CONFIG_TOTEM_BOOT_PROFILE_DUMP)
  zephyr_library_sources(src/totem_watchdog.c)
endif()

# Crash capture. Also built for the reset-reason/profile paths, which call
# totem_fault_report_and_clear() at boot even when the watchdog itself is off.
if(CONFIG_TOTEM_FAULT_CAPTURE OR CONFIG_TOTEM_WATCHDOG OR CONFIG_TOTEM_RESET_REASON_CAPTURE)
  zephyr_library_sources(src/totem_fault.c)
endif()

if(CONFIG_TOTEM_ENDPOINT_LOG)
  zephyr_library_sources(src/totem_endpoint_log.c)
endif()

# Note: TOTEM_ADV_* / DIR_THEN_OPEN / FAL / reselect live in patches/zmk-ble.patch.
# TOTEM_EXCLUSIVE_HOST vs TOTEM_LAZY_INACTIVE_HOST are mutually exclusive policies.
