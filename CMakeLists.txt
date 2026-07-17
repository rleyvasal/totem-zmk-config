# Standalone module: keep only the active host connected (the cross-talk fix).
if(CONFIG_TOTEM_EXCLUSIVE_HOST)
  zephyr_library()
  # ZMK adds its public headers (app/include, home of zmk/ble.h) PRIVATE to its
  # own target, so external modules must add that path explicitly.
  zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)
  zephyr_library_sources(src/exclusive_host.c)
endif()

# Note: TOTEM_ADV_THROTTLE, TOTEM_ADV_BOOST, TOTEM_IDLE_DISCONNECT,
# TOTEM_RESELECT_RECONNECT, and TOTEM_EVICT_ADV_COOLDOWN_MS are NOT modules
# here -- they live in patches/zmk-ble.patch (ZMK app/src/ble.c), because they
# have to control advertising / profile-select, which a standalone module can't
# do cleanly. TOTEM_BOND_HEAL is part of exclusive_host.c above.
