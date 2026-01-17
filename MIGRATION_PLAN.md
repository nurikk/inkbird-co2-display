Bluedroid → NimBLE Migration Plan (Multi-Connection BLE)

  1. Configuration Changes
  ┌─────────────────────────────────────┬────────────────────────────────────┐
  │              Bluedroid              │               NimBLE               │
  ├─────────────────────────────────────┼────────────────────────────────────┤
  │ CONFIG_BT_BLUEDROID_ENABLED=y       │ CONFIG_BT_NIMBLE_ENABLED=y         │
  ├─────────────────────────────────────┼────────────────────────────────────┤
  │ CONFIG_BT_ACL_CONNECTIONS=N         │ CONFIG_BT_NIMBLE_MAX_CONNECTIONS=N │
  ├─────────────────────────────────────┼────────────────────────────────────┤
  │ CONFIG_BT_MULTI_CONNECTION_ENBALE=y │ Built-in, no separate flag         │
  └─────────────────────────────────────┴────────────────────────────────────┘
  idf.py menuconfig
  # Component config → Bluetooth → Host → "NimBLE - BLE only"
  # Set BT_NIMBLE_MAX_CONNECTIONS

  2. Initialization

  Bluedroid:
  esp_bt_controller_init(&bt_cfg);
  esp_bt_controller_enable(ESP_BT_MODE_BLE);
  esp_bluedroid_init_with_cfg(&cfg);
  esp_bluedroid_enable();
  esp_ble_gap_register_callback(gap_cb);
  esp_ble_gattc_register_callback(gattc_cb);
  esp_ble_gatts_register_callback(gatts_cb);

  NimBLE:
  nimble_port_init();
  ble_hs_cfg.reset_cb = on_reset;
  ble_hs_cfg.sync_cb = on_sync;  // Start scan/adv here
  ble_store_config_init();
  nimble_port_freertos_init(host_task);

  Key difference: NimBLE uses a sync callback (ble_hs_cfg.sync_cb) that fires when the host syncs with controller — start scanning/advertising there, not in app_main.

  3. GAP Event Handling

  Bluedroid: Separate callbacks for GAP, GATTC, GATTS
  NimBLE: Single GAP callback per role, passed to ble_gap_ext_disc() or ble_gap_ext_adv_configure()
  ┌────────────────────────────────┬─────────────────────────────┐
  │        Bluedroid Event         │        NimBLE Event         │
  ├────────────────────────────────┼─────────────────────────────┤
  │ ESP_GAP_BLE_EXT_ADV_REPORT_EVT │ BLE_GAP_EVENT_EXT_DISC      │
  ├────────────────────────────────┼─────────────────────────────┤
  │ ESP_GATTC_CONNECT_EVT          │ BLE_GAP_EVENT_CONNECT       │
  ├────────────────────────────────┼─────────────────────────────┤
  │ ESP_GATTC_DISCONNECT_EVT       │ BLE_GAP_EVENT_DISCONNECT    │
  ├────────────────────────────────┼─────────────────────────────┤
  │ ESP_GAP_BLE_SCAN_RESULT_EVT    │ BLE_GAP_EVENT_DISC (legacy) │
  └────────────────────────────────┴─────────────────────────────┘
  4. Scanning

  Bluedroid:
  esp_ble_gap_set_ext_scan_params(&params);
  esp_ble_gap_start_ext_scan(duration, period);

  NimBLE:
  ble_gap_ext_disc(own_addr_type, duration, period, filter_dups,
                   filter_policy, limited, &uncoded_params,
                   &coded_params, gap_event_cb, NULL);

  5. Connecting (Multi-Connection Key)

  Bluedroid:
  esp_ble_gattc_enh_open(gattc_if, &creat_conn_params);

  NimBLE:
  struct ble_gap_multi_conn_params params = {
      .scheduling_len_us = EVENT_LEN_MS * 1000,  // Critical for multi-conn
      .own_addr_type = BLE_OWN_ADDR_RANDOM,
      .peer_addr = &peer_addr,
      .phy_mask = BLE_GAP_LE_PHY_1M_MASK | BLE_GAP_LE_PHY_2M_MASK,
      .phy_1m_conn_params = &uncoded_params,
      // ...
  };
  ble_gap_multi_connect(&params, gap_event_cb, NULL);

  Nuance: NimBLE's ble_gap_multi_connect() has explicit scheduling_len_us for connection scheduling — critical for stable multi-connection.

  6. Connection Interval Optimization

  Bluedroid:
  esp_ble_gap_set_common_factor(common_factor);
  esp_ble_gap_set_sch_len(0, event_len_us);

  NimBLE:
  ble_gap_common_factor_set(true, interval_in_units);

  Nuance: Both require setting a common factor for connection intervals. Formula: MINIMUM_INTERVAL > ((MAX_PDU_TIME * 2) + 150us) * CONN_NUM

  7. Peer/Connection Management

  Bluedroid: Manual peer_manager.c with custom structs
  NimBLE: Built-in peer.c module with peer_init(), peer_add(), peer_delete()

  // NimBLE peer management
  peer_init(max_peers, max_svcs, max_chrs, max_dscs);
  peer_add(conn_handle);
  peer_delete(conn_handle);
  peer_disc_svc_by_uuid(conn_handle, uuid, callback, arg);

  8. Service Discovery

  Bluedroid:
  esp_ble_gattc_search_service(gattc_if, conn_id, &uuid);
  // Handle in ESP_GATTC_SEARCH_RES_EVT

  NimBLE:
  peer_disc_svc_by_uuid(conn_handle, uuid, on_disc_complete, NULL);
  // Callback receives fully parsed service/char/desc lists

  Nuance: NimBLE's peer module handles discovery automatically and populates structured data.

  9. Key Nuances & Gotchas
  ┌─────────────────────┬──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
  │        Topic        │                                                              Notes                                                               │
  ├─────────────────────┼──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
  │ Random address      │ Both change random addr per connection to allow multi-conn to same peripheral. NimBLE: ble_hs_id_gen_rnd() + ble_hs_id_set_rnd() │
  ├─────────────────────┼──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
  │ Scan during connect │ NimBLE: check MYNEWT_VAL(BLE_HOST_ALLOW_CONNECT_WITH_SCAN), if 0 must stop scan before connect                                   │
  ├─────────────────────┼──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
  │ Host task           │ NimBLE runs in dedicated FreeRTOS task via nimble_port_freertos_init()                                                           │
  ├─────────────────────┼──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
  │ Memory              │ NimBLE uses ~50% less RAM than Bluedroid for BLE-only                                                                            │
  ├─────────────────────┼──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
  │ Max connections     │ NimBLE supports more on newer chips (70 vs 50 on ESP32-C6/H2)                                                                    │
  └─────────────────────┴──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘
  10. Migration Checklist

  1. Switch Kconfig to NimBLE, set max connections
  2. Replace init — remove Bluedroid init, add NimBLE port init + host config
  3. Move startup logic to sync_cb callback
  4. Rewrite GAP callbacks — consolidate into single callback per role
  5. Replace connection API — use ble_gap_multi_connect() with scheduling params
  6. Adopt peer module — replace custom peer manager with NimBLE's peer.c
  7. Update service discovery — use peer_disc_* functions
  8. Test connection stability — verify interval/scheduling params work for your connection count

  The NimBLE example at examples/bluetooth/nimble/ble_multi_conn/ble_multi_conn_cent/ is a working reference that mirrors the Bluedroid example's functionality