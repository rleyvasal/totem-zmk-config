# Standalone module: keep only the active host connected (the cross-talk fix).
if(CONFIG_TOTEM_EXCLUSIVE_HOST)
  zephyr_library()
  # ZMK adds its public headers (app/include, home of zmk/ble.h) PRIVATE to its
  # own target, so external modules must add that path explicitly.
  zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)
  zephyr_library_sources(src/exclusive_host.c)
endif()

# Note: TOTEM_ADV_THROTTLE and TOTEM_IDLE_DISCONNECT are NOT modules here -- they
# live in the throttle code (patches/zmk-ble.patch), because they have to control
# advertising, which a standalone module can't do cleanly.
