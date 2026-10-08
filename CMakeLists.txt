# Standalone modules (multi-host BLE helpers). ZMK public headers are PRIVATE
# on the ZMK target, so external modules must add app/include explicitly.
if(CONFIG_TOTEM_EXCLUSIVE_HOST OR CONFIG_ZMK_EXCLUSIVE_HOST
   OR CONFIG_TOTEM_LAZY_INACTIVE_HOST OR CONFIG_TOTEM_RECONNECT_WATCH OR CONFIG_ZMK_RECONNECT_WATCH
   OR CONFIG_TOTEM_HOST_EVENT_LOG OR CONFIG_TOTEM_WATCHDOG OR CONFIG_TOTEM_RESET_REASON_CAPTURE
   OR CONFIG_TOTEM_BOOT_PROFILE_DUMP OR CONFIG_TOTEM_FAULT_CAPTURE
   OR CONFIG_TOTEM_ENDPOINT_LOG
   OR CONFIG_TOTEM_BATTERY_LOG OR CONFIG_ZMK_BATTERY_LOG
   OR CONFIG_TOTEM_USB_QUIET_HOST
   OR CONFIG_TOTEM_STUDIO_CONSOLE OR CONFIG_ZMK_STUDIO_CONSOLE
   OR CONFIG_ZMK_STUDIO)
  zephyr_library()
  zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)
  zephyr_library_include_directories(${CMAKE_CURRENT_LIST_DIR}/include)
endif()

if(CONFIG_TOTEM_EXCLUSIVE_HOST OR CONFIG_ZMK_EXCLUSIVE_HOST)
  zephyr_library_sources(src/exclusive_host.c)
endif()

if(CONFIG_TOTEM_LAZY_INACTIVE_HOST)
  zephyr_library_sources(src/lazy_inactive_host.c)
endif()

if(CONFIG_TOTEM_RECONNECT_WATCH OR CONFIG_ZMK_RECONNECT_WATCH)
  zephyr_library_sources(src/reconnect_watch.c)
endif()

if(CONFIG_TOTEM_HOST_EVENT_LOG)
  zephyr_library_sources(src/host_event_log.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_SPLIT_ROLE_CENTRAL src/hid_connection_log.c)
  if(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    zephyr_ld_options(-Wl,--wrap=bt_conn_set_security)
    zephyr_library_sources(src/diagnostic_timing.c)
    zephyr_library_sources(src/security_timing.c)
    zephyr_library_sources(src/hid_report_diagnostics.c)
    zephyr_library_include_directories(${ZEPHYR_BASE}/subsys/bluetooth/host)
    zephyr_ld_options(-Wl,--wrap=bt_conn_set_state -Wl,--wrap=settings_save_one)
    zephyr_ld_options(-Wl,--wrap=net_buf_slist_put -Wl,--wrap=net_buf_slist_get
                      -Wl,--wrap=bt_conn_security_changed
                      -Wl,--wrap=bt_l2cap_security_changed
                      -Wl,--wrap=bt_l2cap_send_pdu)
    if(CONFIG_BT_LL_SW_SPLIT)
      # Internal layouts come from the pinned Zephyr software controller headers.
      zephyr_library_sources(src/handoff_diagnostics.c)
      zephyr_library_sources(src/security_controller_timing.c)
      if(CONFIG_SOC_NRF52840)
        zephyr_library_sources(src/handoff_radio_tx.c)
        zephyr_ld_options(-Wl,--wrap=radio_isr_set -Wl,--wrap=radio_pkt_tx_set
                          -Wl,--wrap=radio_status_reset)
        if(CONFIG_BT_CTLR_LE_ENC)
          zephyr_ld_options(-Wl,--wrap=radio_ccm_tx_pkt_set)
        endif()
      endif()
      zephyr_library_include_directories(${ZEPHYR_BASE}/subsys/bluetooth/controller
        ${ZEPHYR_BASE}/subsys/bluetooth/controller/ll_sw
        ${ZEPHYR_BASE}/subsys/bluetooth/controller/ll_sw/nordic)
      zephyr_ld_options(-Wl,--wrap=bt_conn_disconnect -Wl,--wrap=llcp_tx_enqueue
                        -Wl,--wrap=llcp_lp_comm_tx_ack -Wl,--wrap=ull_conn_done
                        -Wl,--wrap=ull_cp_prt_elapse
                        -Wl,--wrap=ull_cp_rx -Wl,--wrap=ull_cp_ltk_req_reply
                        -Wl,--wrap=ull_cp_ltk_req_neq_reply)
    endif()
    zephyr_ld_options(-Wl,--wrap=zmk_hid_release -Wl,--wrap=z_impl_k_msgq_put
                      -Wl,--wrap=z_impl_k_msgq_get -Wl,--wrap=bt_gatt_notify_cb
                      -Wl,--wrap=zmk_ble_active_profile_conn)
  endif()
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

if(CONFIG_TOTEM_BATTERY_LOG OR CONFIG_ZMK_BATTERY_LOG)
  zephyr_library_sources(src/totem_battery_log.c)
endif()

if(CONFIG_TOTEM_ENDPOINT_LOG)
  zephyr_library_sources(src/totem_endpoint_log.c)
endif()

if(CONFIG_ZMK_STUDIO)
  zephyr_library_sources(src/totem_studio_usb.c)
endif()

if(CONFIG_TOTEM_STUDIO_CONSOLE OR CONFIG_ZMK_STUDIO_CONSOLE)
  zephyr_library_sources(src/totem_studio_log.c)
endif()

if(CONFIG_TOTEM_USB_QUIET_HOST)
  zephyr_library_sources(src/usb_host_quiet.c)
endif()

# Note: TOTEM_ADV_* / DIR_THEN_OPEN / FAL / reselect live in patches/zmk-ble.patch.
# TOTEM_EXCLUSIVE_HOST vs TOTEM_LAZY_INACTIVE_HOST are mutually exclusive policies.
