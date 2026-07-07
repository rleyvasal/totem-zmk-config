if(CONFIG_TOTEM_ACTIVE_PROFILE_ONLY)
  zephyr_library()
  zephyr_library_sources(src/active_profile_only.c)
  # ZMK adds its public headers (app/include, home of zmk/ble.h) PRIVATE to its
  # own target, so external modules must add that path explicitly.
  zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)
endif()

# Mutually exclusive with TOTEM_ACTIVE_PROFILE_ONLY above (both call
# zephyr_library() and would collide on the same default name). Only one of these
# eviction modules should be enabled at a time; TOTEM_ACTIVE_PROFILE_ONLY is the
# older, retired one.
if(CONFIG_TOTEM_EXCLUSIVE_HOST)
  zephyr_library()
  zephyr_library_sources(src/exclusive_host.c)
  zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)
endif()
