# One library for all Totem BLE modules. zephyr_library() may only be called once
# per CMakeLists (a second call collides on the same default name), so sources are
# added conditionally into the single library.
if(CONFIG_TOTEM_ACTIVE_PROFILE_ONLY OR CONFIG_TOTEM_EXCLUSIVE_HOST)
  zephyr_library()
  # ZMK adds its public headers (app/include, home of zmk/ble.h) PRIVATE to its
  # own target, so external modules must add that path explicitly.
  zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)

  # Retired eviction module -- do not enable alongside TOTEM_EXCLUSIVE_HOST (they
  # both hook connect/profile-changed and would double up).
  if(CONFIG_TOTEM_ACTIVE_PROFILE_ONLY)
    zephyr_library_sources(src/active_profile_only.c)
  endif()

  # Keep only the active host connected (cross-talk fix).
  if(CONFIG_TOTEM_EXCLUSIVE_HOST)
    zephyr_library_sources(src/exclusive_host.c)
  endif()
endif()

# Note: TOTEM_IDLE_DISCONNECT (drop the idle host, then go dark) is NOT a module
# here -- it lives in the throttle code (patches/totem-ble.patch), because it has to
# control advertising, which a standalone module can't do cleanly.
