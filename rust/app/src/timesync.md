# Bluetooth Time Sync, Rust Watch Faces and LVGL on PineTime Mynewt

![PineTime Smart Watch with Bluetooth Time Sync and Rust Watch Face](https://lupyuen.github.io/images/timesync-title.png)

Let's learn how PineTime syncs the time over Bluetooth LE... And how we build PineTime Watch Faces with Rust and LVGL.

# Time Sync over Bluetooth LE

TODO: Bluetooth LE Current Time Service, Discovering Bluetooth LE Services and Characteristics, Reading Bluetooth LE Characteristics, Decoding Bluetooth LE Current Time

[`apps/my_sensor_app/src/ble_main.c`](https://github.com/lupyuen/pinetime-rust-mynewt/blob/master/apps/my_sensor_app/src/ble_main.c)

```c
/**
 * The nimble host executes this callback when a GAP event occurs.  The
 * application associates a GAP event callback with each connection that forms.
 * bleprph uses the same callback for all connections. */
static int bleprph_gap_event(struct ble_gap_event *event, void *arg) {
    struct ble_gap_conn_desc desc;
    int rc;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        /* A new connection was established or a connection attempt failed. */
        MODLOG_DFLT_INFO("connection %s; status=%d ",
                    event->connect.status == 0 ? "established" : "failed",
                    event->connect.status);
        if (event->connect.status == 0) {
            rc = ble_gap_conn_find(event->connect.conn_handle, &desc);
            assert(rc == 0);
            bleprph_print_conn_desc(&desc);

#if MYNEWT_VAL(BLEPRPH_LE_PHY_SUPPORT)
            phy_conn_changed(event->connect.conn_handle);
#endif

            //  When a BLE connection is established, we read the GATT Characteristic of the Current Time Service of the BLE Peer
            //  Based on https://github.com/apache/mynewt-nimble/blob/master/apps/blecent/src/main.c

            //  Remember the BLE Peer. Ignore the error if we have already added the peer.
            rc = blepeer_add(event->connect.conn_handle);
            if (rc != 0 && rc != 2) { MODLOG_DFLT_ERROR("Failed to add peer: %d\n", rc); MODLOG_DFLT_FLUSH(); }
            else {
                //  Discover all GATT Sevices in BLE Peer (including Current Time Service)
                rc = blepeer_disc_all(event->connect.conn_handle, blecent_on_disc_complete, NULL);
                if (rc != 0) { MODLOG_DFLT_ERROR("Failed to discover services: %d\n", rc); MODLOG_DFLT_FLUSH(); }
            }
        }
```

```c
///////////////////////////////////////////////////////////////////////////////
//  Time Sync. When a BLE connection is established, we read the GATT Characteristic for the Current Time Service of the BLE Peer
//  Based on https://github.com/apache/mynewt-nimble/blob/master/apps/blecent/src/main.c

#define BLE_GATT_SVC_CTS        (0x1805)  //  GATT Service for Current Time Service
#define BLE_GATT_CHR_CUR_TIME   (0x2A2B)  //  GATT Characteristic for Current Time

static void blecent_read(const struct blepeer *peer);
static int blecent_on_read(uint16_t conn_handle, const struct ble_gatt_error *error, struct ble_gatt_attr *attr, void *arg);
static int set_system_time(const struct os_mbuf *om);
static void print_mbuf(const struct os_mbuf *om);

/// Data Format for Current Time Service. Based on https://github.com/sdalu/mynewt-nimble/blob/495ff291a15306787859a2fe8f2cc8765b546e02/nimble/host/services/cts/src/ble_svc_cts.c
struct ble_current_time {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hours;
    uint8_t minutes;
    uint8_t secondes;
    uint8_t day_of_week;
    uint8_t fraction256;
    uint8_t adjust_reason;
} __attribute__((__packed__));

/// Called when GATT Service Discovery of the BLE Peer has completed
static void blecent_on_disc_complete(const struct blepeer *peer, int status, void *arg) {
    if (status != 0) {
        //  Service discovery failed
        MODLOG_DFLT_ERROR("Error: Service discovery failed; status=%d conn_handle=%d\n", status, peer->conn_handle);
        goto err;
    }

    //  GATT Service Discovery has completed successfully.  Now we have a complete list of services, characteristics, and descriptors that the peer supports.
    MODLOG_DFLT_INFO("Service discovery complete; status=%d conn_handle=%d\n", status, peer->conn_handle);

    //  Read the GATT Characteristics from the peer
    blecent_read(peer);
    return;

err:
    //  Don't terminate the BLE connection yet, may be used by MCU Manager
    //  ble_gap_terminate(peer->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    return;
}

/// Read the GATT Characteristic for Current Time from the BLE Peer
static void blecent_read(const struct blepeer *peer) {
    //  Find the GATT Characteristic for Current Time Service from the discovered GATT Characteristics
    const struct blepeer_chr *chr = blepeer_chr_find_uuid(
        peer,
        BLE_UUID16_DECLARE(BLE_GATT_SVC_CTS),      //  GATT Service for Current Time Service
        BLE_UUID16_DECLARE(BLE_GATT_CHR_CUR_TIME)  //  GATT Characteristic for Current Time Service
    );
    if (chr == NULL) {
        MODLOG_DFLT_ERROR("Error: Peer doesn't support CTS\n");
        goto err;
    }

    //  Read the Current Time Service Characteristic
    int rc = ble_gattc_read(
        peer->conn_handle,      //  BLE Connection
        chr->chr.val_handle,    //  GATT Characteristic
        blecent_on_read,        //  Callback after reading
        NULL                    //  Callback argument
    );
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "Error: Can't read CTS: %d\n", rc);
        goto err;
    }
    return;

err:
    //  Don't terminate the BLE connection yet, may be used by MCU Manager
    //  ble_gap_terminate(peer->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    return;
}

/// Called when Current Time GATT Characteristic has been read
static int blecent_on_read(uint16_t conn_handle, const struct ble_gatt_error *error, struct ble_gatt_attr *attr, void *arg) {
    //  Read the current time from the GATT Characteristic
    MODLOG_DFLT_INFO("Read complete; status=%d conn_handle=%d", error->status, conn_handle);
    if (error->status == 0) {
        MODLOG_DFLT_INFO(" attr_handle=%d value=", attr->handle);
        print_mbuf(attr->om);
    }
    MODLOG_DFLT_INFO("\n");

    //  Set the system time from the current time
    int rc = set_system_time(attr->om);
    if (rc != 0) {
        MODLOG_DFLT_ERROR("Error: Can't set time: %d\n", rc);
        goto err;
    }

    //  Get the system time
    struct os_timeval tv;
    struct os_timezone tz;
    rc = os_gettimeofday(&tv, &tz);
    if (rc != 0) { MODLOG_DFLT_ERROR("Error: Can't get time: %d\n", rc); goto err; }
    struct clocktime ct;
    rc = timeval_to_clocktime(&tv, &tz, &ct);
    if (rc != 0) { MODLOG_DFLT_ERROR("Error: Can't convert time: %d\n", rc); goto err; }

    //  Dump the system time as 2020-10-04T13:20:26.839843+00:00
    char buf[50];
    rc = datetime_format(&tv, &tz, buf, sizeof(buf));
    if (rc != 0) { MODLOG_DFLT_ERROR("Error: Can't format time: %d\n", rc); goto err; }
    console_printf("Current Time: %s\n", buf);

    //  TODO: Update the current time periodically
    return 0;

err:
    return 0;  //  Don't propagate error to system
}

/// Set system time given the GATT Current Time in Mbuf format. Based on https://github.com/sdalu/mynewt-nimble/blob/495ff291a15306787859a2fe8f2cc8765b546e02/nimble/host/services/cts/src/ble_svc_cts.c
static int set_system_time(const struct os_mbuf *om) {
    //  Verify the Mbuf size
    uint16_t om_len = OS_MBUF_PKTLEN(om);
    if (om_len != sizeof(struct ble_current_time)) {  //  Should be 10 bytes
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    //  Copy the data from the Mbuf
    struct ble_current_time current_time;
    int rc = ble_hs_mbuf_to_flat(  //  Flatten and copy the Mbuf...
        om,                        //  From om...
		&current_time,             //  To current_time...
        om_len,                    //  For om_len bytes
        NULL
    );
    if (rc != 0) { return BLE_ATT_ERR_UNLIKELY; }

    //  Get timezone
    struct os_timeval tv0;
    struct os_timezone tz;
    rc = os_gettimeofday(&tv0, &tz);
    if (rc != 0) { return BLE_ATT_ERR_UNLIKELY; }

    //  Convert to clocktime format
    struct clocktime ct;
    ct.year = le16toh(current_time.year);
    ct.mon  = current_time.month;
    ct.day  = current_time.day;
    ct.hour = current_time.hours;
    ct.min  = current_time.minutes;
    ct.sec  = current_time.secondes;
    ct.usec = (current_time.fraction256 * 1000000) / 256;

    //  Convert to timeval format
    struct os_timeval tv;    
    rc = clocktime_to_timeval(&ct, &tz, &tv);
    if (rc != 0) { return BLE_ATT_ERR_UNLIKELY; }

#ifdef NOTUSED
    //  Set the system date and time, ignoring the day of week
    if (current_time.day_of_week) {
        if (timeval_to_clocktime(&tv, &tz, &ct)) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        if (ct.dow != (cts_current_time.day_of_week % 7)) {
            rc = 0x80;  //  Data field ignored
        }
    }
#endif  //  NOTUSED

    //  Set the system time
    rc = os_settimeofday(&tv, NULL);
    if (rc != 0) { return BLE_ATT_ERR_UNLIKELY; }
    return 0;
}
```

[`apps/my_sensor_app/src/ble_peer.c`](https://github.com/lupyuen/pinetime-rust-mynewt/blob/master/apps/my_sensor_app/src/ble_peer.c)

Bluetooth Log:

```
Starting BLE...
BLE started
Render LVGL display...
Flush display: left=63, top=27, right=196, bottom=42...
connection established; status=0 handle=1 our_ota_addr_type=1 our_ota_addr= our_id_addr_type=1 our_id_addr= peer_ota_addr_type=1 peer_ota_addr= peer_id_addr_type=1 peer_id_addr= conn_itvl=36 conn_latency=0 supervision_timeout=500 encrypted=0 authenticated=0 bonded=0
connection updated; status=0 handle=1 our_ota_addr_type=1 our_ota_addr= our_id_addr_type=1 our_id_addr= peer_ota_addr_type=1 peer_ota_addr= peer_id_addr_type=1 peer_id_addr= conn_itvl=6 conn_latency=0 supervision_timeout=500 encrypted=0 authenticated=0 bonded=0
Service discovery complete; status=0 conn_handle=1
Read complete; status=0 conn_handle=1 attr_handle=67 value=e4 07 0a 04 0e 05 29 07 87 00 
Current Time: 2020-10-04T14:05:41.527343+00:00
connection updated; status=0 handle=1 our_ota_addr_type=1 our_ota_addr= our_id_addr_type=1 our_id_addr= peer_ota_addr_type=1 peer_ota_addr= peer_id_addr_type=1 peer_id_addr= conn_itvl=36 conn_latency=0 supervision_timeout=500 encrypted=0 authenticated=0 bonded=0
disconnect; reason=531 handle=1 our_ota_addr_type=1 our_ota_addr= our_id_addr_type=1 our_id_addr= peer_ota_addr_type=1 peer_ota_addr= peer_id_addr_type=1 peer_id_addr= conn_itvl=36 conn_latency=0 supervision_timeout=500 encrypted=0 authenticated=0 bonded=0
Render LVGL display...
Flush display: left=60, top=27, right=183, bottom=42...
...
Render LVGL display...
Flush display: left=59, top=27, right=181, bottom=42...
...
Render LVGL display...
Flush display: left=59, top=27, right=180, bottom=42...
```

# Get the Time in C

TODO: os_timeval, clocktime and ISO format, [`my_sensor_app/src/watch_face.c`](https://github.com/lupyuen/pinetime-rust-mynewt/blob/master/apps/my_sensor_app/src/watch_face.c)

# Get the Time in Rust

TODO: WatchFaceTime, [`rust/app/src/watch_face.rs`](https://github.com/lupyuen/pinetime-rust-mynewt/blob/master/rust/app/src/watch_face.rs)

# Watch Face in C

TODO: Mynewt timer, [`my_sensor_app/src/watch_face.c`](https://github.com/lupyuen/pinetime-rust-mynewt/blob/master/apps/my_sensor_app/src/watch_face.c)

```c
/// Render a watch face. Called by main() in rust/app/src/lib.rs
int create_watch_face(void) {
    console_printf("Create watch face...\n"); console_flush();
    btn = lv_btn_create(lv_scr_act(), NULL);     //  Add a button the current screen
    lv_obj_set_pos(btn, 10, 10);                 //  Set its position
    lv_obj_set_size(btn, 220, 50);               //  Set its size

    label = lv_label_create(btn, NULL);          //  Add a label to the button
    lv_label_set_text(label, "Time Sync");       //  Set the label text

    //  Set a timer to update the watch face every minute
    //  TODO: Move this code to the caller
    os_callout_init(
        &watch_face_callout,   //  Timer for the watch face
        os_eventq_dflt_get(),  //  Use default event queue
        watch_face_callback,   //  Callback function for the timer
        NULL
    );
    //  Trigger the timer in 60 seconds
    os_callout_reset(
        &watch_face_callout,   //  Timer for the watch face
        OS_TICKS_PER_SEC * 60  //  Trigger timer in 60 seconds
    );
    return 0;
}
```

```c
/// Update the watch face
int update_watch_face(void) {
    //  If button or label not created, quit
    if (btn == NULL || label == NULL) { return 1; }

    //  Get the system time
    struct os_timeval tv;
    struct os_timezone tz;
    int rc = os_gettimeofday(&tv, &tz);
    if (rc != 0) { console_printf("Can't get time: %d\n", rc); return 2; }

    //  Convert the time
    struct clocktime ct;
    rc = timeval_to_clocktime(&tv, &tz, &ct);
    if (rc != 0) { console_printf("Can't convert time: %d\n", rc); return 3; }

    //  Format the time as 2020-10-04T13:20:26.839843+00:00
    char buf[50];
    rc = datetime_format(&tv, &tz, buf, sizeof(buf));
    if (rc != 0) { console_printf("Can't format time: %d\n", rc); return 4; }

    //  Truncate after minute: 2020-10-04T13:20
    buf[16] = 0;

    //  Set the label text
    lv_label_set_text(label, buf);
    return 0;
}
```

```c
/// Timer callback that is called every minute
static void watch_face_callback(struct os_event *ev) {
    assert(ev != NULL);

    //  Update the watch face
    update_watch_face();

    //  Render the watch face
    pinetime_lvgl_mynewt_render();

    //  Set the watch face timer
    os_callout_reset(
        &watch_face_callout,   //  Timer for the watch face
        OS_TICKS_PER_SEC * 60  //  Trigger timer in 60 seconds
    );
}
```

# Watch Face in Rust

TODO: Barebones watch face, LVGL styles, [`rust/app/src/watch_face.rs`](https://github.com/lupyuen/pinetime-rust-mynewt/blob/master/rust/app/src/watch_face.rs)

```rust
/// Create the widgets for the Watch Face. Called by start_watch_face() below.
pub fn create_widgets(widgets: &mut WatchFaceWidgets) -> MynewtResult<()> {
    //  Fetch the screen, which will be the parent of the widgets
    let scr = widgets.screen;
    assert!(!scr.is_null(), "null screen");

    //  Create a label for Time: "00:00"
    widgets.time_label = {
        let lbl = label::create(scr, ptr::null()) ? ;  //  `?` will terminate the function in case of error
        label::set_long_mode(lbl, label::LV_LABEL_LONG_BREAK) ? ;
        label::set_text(     lbl, strn!("00:00")) ? ;  //  strn creates a null-terminated string
        obj::set_width(      lbl, 240) ? ;
        obj::set_height(     lbl, 200) ? ;
        label::set_align(    lbl, label::LV_LABEL_ALIGN_CENTER) ? ;
        obj::align(          lbl, scr, obj::LV_ALIGN_CENTER, 0, -30) ? ;    
        lbl
    };
```

# Porting LVGL to Mynewt

TODO: SPI Driver for ST7789 Display Controller, [`pinetime_lvgl_mynewt`](https://gitlab.com/lupyuen/pinetime_lvgl_mynewt)

Located at `libs/pinetime_lvgl_mynewt`

[`src/pinetime/lvgl.c`](https://gitlab.com/lupyuen/pinetime_lvgl_mynewt/blob/master/src/pinetime/lvgl.c)

```c
/// Init the LVGL library. Called by sysinit() during startup, defined in pkg.yml.
void pinetime_lvgl_mynewt_init(void) {    
    console_printf("Init LVGL...\n"); console_flush();
    assert(pinetime_lvgl_mynewt_started == false);

    //  Init the display controller
    int rc = pinetime_lvgl_mynewt_init_display(); assert(rc == 0);

    //  Init the LVGL display
    lv_init();
    lv_port_disp_init();
    pinetime_lvgl_mynewt_started = true;
}

/// Render the LVGL display
int pinetime_lvgl_mynewt_render(void) {
    console_printf("Render LVGL display...\n"); console_flush();
    //  Must tick at least 100 milliseconds to force LVGL to update display
    lv_tick_inc(100);
    //  LVGL will flush our display driver
    lv_task_handler();
    return 0;
}
```

Display Driver for ST7789: [`src/pinetime/lv_port_disp.c`](https://gitlab.com/lupyuen/pinetime_lvgl_mynewt/blob/master/src/pinetime/lv_port_disp.c)

```c
/// Flush the content of the internal buffer the specific area on the display
static void disp_flush(lv_disp_drv_t * disp_drv, const lv_area_t * area, lv_color_t * color_p) {
    //  Validate parameters
    assert(area->x2 >= area->x1);
    assert(area->y2 >= area->y1);

    //  Set the ST7789 display window
    pinetime_lvgl_mynewt_set_window(area->x1, area->y1, area->x2, area->y2);

    //  Write Pixels (RAMWR): st7735_lcd::draw() → set_pixel()
    int len = 
        ((area->x2 - area->x1) + 1) *  //  Width
        ((area->y2 - area->y1) + 1) *  //  Height
        2;                             //  2 bytes per pixel
    pinetime_lvgl_mynewt_write_command(RAMWR, NULL, 0);
    pinetime_lvgl_mynewt_write_data((const uint8_t *) color_p, len);

    //  IMPORTANT!!! Inform the graphics library that you are ready with the flushing
    lv_disp_flush_ready(disp_drv);
}
```

[`src/pinetime/display.c`](https://gitlab.com/lupyuen/pinetime_lvgl_mynewt/blob/master/src/pinetime/display.c)

```c
/// Set the ST7789 display window to the coordinates (left, top), (right, bottom)
int pinetime_lvgl_mynewt_set_window(uint8_t left, uint8_t top, uint8_t right, uint8_t bottom) {
    assert(left < COL_COUNT && right < COL_COUNT && top < ROW_COUNT && bottom < ROW_COUNT);
    assert(left <= right);
    assert(top <= bottom);
    //  Set Address Window Columns (CASET): st7735_lcd::draw() → set_pixel() → set_address_window()
    int rc = pinetime_lvgl_mynewt_write_command(CASET, NULL, 0); assert(rc == 0);
    uint8_t col_para[4] = { 0x00, left, 0x00, right };
    rc = pinetime_lvgl_mynewt_write_data(col_para, 4); assert(rc == 0);

    //  Set Address Window Rows (RASET): st7735_lcd::draw() → set_pixel() → set_address_window()
    rc = pinetime_lvgl_mynewt_write_command(RASET, NULL, 0); assert(rc == 0);
    uint8_t row_para[4] = { 0x00, top, 0x00, bottom };
    rc = pinetime_lvgl_mynewt_write_data(row_para, 4); assert(rc == 0);
    return 0;
}
```

```c
/// Transmit ST7789 command
int pinetime_lvgl_mynewt_write_command(uint8_t command, const uint8_t *params, uint16_t len) {
    hal_gpio_write(DISPLAY_DC, 0);
    int rc = transmit_spi(&command, 1);
    assert(rc == 0);
    if (params != NULL && len > 0) {
        rc = pinetime_lvgl_mynewt_write_data(params, len);
        assert(rc == 0);
    }
    return 0;
}

/// Transmit ST7789 data
int pinetime_lvgl_mynewt_write_data(const uint8_t *data, uint16_t len) {
    hal_gpio_write(DISPLAY_DC, 1);
    transmit_spi(data, len);
    return 0;
}
```

# Rust Wrapper for LVGL

TODO: Bindgen, Safe Wrapper Proc Macro, [`rust/lvgl`](https://github.com/lupyuen/pinetime-rust-mynewt/blob/master/rust/lvgl)

# What's Next

TODO: Bluetooth Time Sync, Rust Watch Faces and LVGL were developed and tested with Remote PineTime

[Check out my PineTime articles](https://lupyuen.github.io)

[RSS Feed](https://lupyuen.github.io/rss.xml)