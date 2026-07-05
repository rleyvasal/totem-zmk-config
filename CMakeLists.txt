if(CONFIG_TOTEM_ACTIVE_PROFILE_ONLY)
  zephyr_library()
  zephyr_library_sources(src/active_profile_only.c)
endif()
