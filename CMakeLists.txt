# Standalone modules (multi-host BLE helpers). ZMK public headers are PRIVATE
# on the ZMK target, so external modules must add app/include explicitly.
if(CONFIG_TOTEM_EXCLUSIVE_HOST OR CONFIG_TOTEM_RECONNECT_WATCH OR CONFIG_TOTEM_HOST_EVENT_LOG)
  zephyr_library()
  zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)
  # Module-local headers (include/totem_*.h)
  zephyr_library_include_directories(${CMAKE_CURRENT_LIST_DIR}/include)
endif()

if(CONFIG_TOTEM_EXCLUSIVE_HOST)
  zephyr_library_sources(src/exclusive_host.c)
endif()

if(CONFIG_TOTEM_RECONNECT_WATCH)
  zephyr_library_sources(src/reconnect_watch.c)
endif()

if(CONFIG_TOTEM_HOST_EVENT_LOG)
  zephyr_library_sources(src/host_event_log.c)
endif()

# Note: TOTEM_ADV_THROTTLE, TOTEM_ADV_BOOST, TOTEM_IDLE_DISCONNECT,
# TOTEM_RESELECT_RECONNECT, TOTEM_EVICT_ADV_COOLDOWN_MS, open-adv retry,
# IRK-aware profile matching, and totem ads_suppressed/boost_rearm/kick helpers
# live in patches/zmk-ble.patch (ZMK app/src/ble.c + zmk/ble.h).
# TOTEM_BOND_HEAL / thrash detect are part of exclusive_host.c above.
# TOTEM_HOST_EVENT_LOG is multi-profile (any BT_SEL index), not dual-host-only.
