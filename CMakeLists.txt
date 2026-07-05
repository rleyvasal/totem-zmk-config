if(CONFIG_TOTEM_ACTIVE_PROFILE_ONLY)
  zephyr_library()
  zephyr_library_sources(src/active_profile_only.c)
  # ZMK adds its public headers (app/include, home of zmk/ble.h) PRIVATE to its
  # own target, so external modules must add that path explicitly.
  zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)
endif()
