# Standalone modules (central dual-host / reconnect). ZMK public headers are
# PRIVATE on the ZMK target, so external modules must add app/include explicitly.
if(CONFIG_TOTEM_EXCLUSIVE_HOST OR CONFIG_TOTEM_RECONNECT_WATCH)
  zephyr_library()
  zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)
endif()

if(CONFIG_TOTEM_EXCLUSIVE_HOST)
  zephyr_library_sources(src/exclusive_host.c)
endif()

if(CONFIG_TOTEM_RECONNECT_WATCH)
  zephyr_library_sources(src/reconnect_watch.c)
endif()

# Note: TOTEM_ADV_THROTTLE, TOTEM_ADV_BOOST, TOTEM_IDLE_DISCONNECT,
# TOTEM_RESELECT_RECONNECT, TOTEM_EVICT_ADV_COOLDOWN_MS, open-adv retry, and
# IRK-aware profile matching live in patches/zmk-ble.patch (ZMK app/src/ble.c).
# TOTEM_BOND_HEAL is part of exclusive_host.c above.
