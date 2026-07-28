#include <hal/nrf_gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/services/nus.h>
#include <zephyr/random/random.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "rgb_led.h"
#include "ir_sensor.h"
#include "buzzer.h"
#include <zephyr/drivers/gpio.h>
#include <zephyr/console/console.h>

#define DEVICE_NAME     CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)

/* ── NUS UUIDs ──────────────────────────────────────────── */
#define NUS_RX_UUID_VAL \
    BT_UUID_128_ENCODE(0x6E400002, 0xB5A3, 0xF393, 0xE0A9, 0xE50E24DCCA9E)
#define NUS_TX_UUID_VAL \
    BT_UUID_128_ENCODE(0x6E400003, 0xB5A3, 0xF393, 0xE0A9, 0xE50E24DCCA9E)

static struct bt_uuid_128 nus_rx_uuid = BT_UUID_INIT_128(NUS_RX_UUID_VAL);
static struct bt_uuid_128 nus_tx_uuid = BT_UUID_INIT_128(NUS_TX_UUID_VAL);

/* ── Pod names ──────────────────────────────────────────── */
#define NUM_ESP32_PODS  3
static const char *pod_names[NUM_ESP32_PODS] = {
    "Blazepod2", "Blazepod3", "Blazepod4"
};

/* ── Per-pod state ──────────────────────────────────────── */
typedef struct {
    struct bt_conn                 *conn;
    uint16_t                        rx_handle;
    uint16_t                        tx_ccc_handle;
    bool                            connected;
    bool                            handles_found;
    struct bt_gatt_discover_params  disc_params;
    struct bt_gatt_subscribe_params sub_params;
} pod_t;

static pod_t pods[NUM_ESP32_PODS];

/* ── Thread config ──────────────────────────────────────── */
#define IR_THREAD_STACK_SIZE    2048
#define IR_THREAD_PRIORITY      5
#define IR_POLL_INTERVAL_MS     50
#define GAME_THREAD_STACK_SIZE  2048
#define GAME_THREAD_PRIORITY    6
#define UART_THREAD_STACK_SIZE  2048
#define UART_THREAD_PRIORITY    7
#define BUZZ_THREAD_STACK_SIZE  512
#define BUZZ_THREAD_PRIORITY    8

K_THREAD_STACK_DEFINE(ir_stack,   IR_THREAD_STACK_SIZE);
K_THREAD_STACK_DEFINE(game_stack, GAME_THREAD_STACK_SIZE);
K_THREAD_STACK_DEFINE(uart_stack, UART_THREAD_STACK_SIZE);
K_THREAD_STACK_DEFINE(buzz_stack, BUZZ_THREAD_STACK_SIZE);

static struct k_thread ir_thread_data;
static struct k_thread game_thread_data;
static struct k_thread uart_thread_data;
static struct k_thread buzz_thread_data;

/* ── Buzzer message queue ───────────────────────────────── */
typedef enum {
    BEEP_NONE  = 0,
    BEEP_SHORT = 1,
    BEEP_LONG  = 2,
} beep_type_t;

K_MSGQ_DEFINE(buzz_msgq, sizeof(beep_type_t), 4, 1);

static inline void request_beep(beep_type_t type)
{
    k_msgq_put(&buzz_msgq, &type, K_NO_WAIT);
}

/* ── Timeout ────────────────────────────────────────────── */
static uint32_t timeout_ms = 10000U;

/* ── Game mode ──────────────────────────────────────────── */
typedef enum {
    GAME_MODE_1 = 1,
    GAME_MODE_2 = 2
} game_mode_t;

static game_mode_t current_mode = GAME_MODE_1;

/* ── Mode switch GPIO ───────────────────────────────────── */
#define MODE_BUTTON_NODE DT_ALIAS(mode_switch)
static const struct gpio_dt_spec mode_btn =
    GPIO_DT_SPEC_GET(MODE_BUTTON_NODE, gpios);
static struct gpio_callback button_cb_data;

/* ── Semaphores ─────────────────────────────────────────── */
static K_SEM_DEFINE(adv_sem,        0, 1);
static K_SEM_DEFINE(game_start_sem, 0, 1);
static K_SEM_DEFINE(round_done_sem, 0, 1);

/* ── Shared state ───────────────────────────────────────── */
static K_MUTEX_DEFINE(state_mutex);
static bool            game_running   = false;
static bool            led_active     = false;
static int64_t         led_on_time_ms = 0;
static struct bt_conn *phone_conn     = NULL;
static bool            notif_enabled  = false;
static bool            round_ended    = false;
static int             odd_pod_index  = -1;

/* ── Game colour (Mode 1) ───────────────────────────────── */
static uint8_t game_r = 0xFF;
static uint8_t game_g = 0x00;
static uint8_t game_b = 0x00;

/* ── Colour tables ──────────────────────────────────────── */
typedef struct {
    uint8_t     r, g, b;
    const char *name;
} colour_t;

static const colour_t colours_m2[] = {
    { 0xFF, 0x00, 0x00, "RED"   },
    { 0x00, 0xFF, 0x00, "GREEN" },
    { 0x00, 0x00, 0xFF, "BLUE"  },
};
#define NUM_COLOURS_M2 ARRAY_SIZE(colours_m2)

/* ── Track which pod we last tried to connect ───────────── */
static int last_connecting_pod = -1;

/* ── Advertisement data ─────────────────────────────────── */
static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};
static const struct bt_data sd[] = {
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_NUS_SRV_VAL),
};

/* ── Forward declarations ───────────────────────────────── */
static void send_to_phone(const char *msg);
static void ir_thread_fn(void *p1, void *p2, void *p3);
static void game_thread_fn(void *p1, void *p2, void *p3);
static void uart_console_thread_fn(void *p1, void *p2, void *p3);
static void buzz_thread_fn(void *p1, void *p2, void *p3);
static void start_scan(void);
static void write_to_pod(int pod_idx, const char *cmd);
static void turn_off_all_pods(void);

/* ══════════════════════════════════════════════════════════
 * Buzzer thread
 * ══════════════════════════════════════════════════════════ */
static void buzz_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    beep_type_t type;
    while (true) {
        k_msgq_get(&buzz_msgq, &type, K_FOREVER);
        switch (type) {
        case BEEP_SHORT:
            buzzer_beep_short();
            break;
        case BEEP_LONG:
            buzzer_beep_long();
            break;
        default:
            break;
        }
    }
}

/* ══════════════════════════════════════════════════════════
 * Helper — build list of available (connected) pods
 * ══════════════════════════════════════════════════════════ */
static int build_available(int available[4])
{
    int count = 0;
    available[count++] = 0;
    for (int i = 0; i < NUM_ESP32_PODS; i++) {
        if (pods[i].connected && pods[i].handles_found) {
            available[count++] = i + 1;
        }
    }
    return count;
}

/* ══════════════════════════════════════════════════════════
 * Helper — random colour index excluding one value
 * ══════════════════════════════════════════════════════════ */
static int random_m2_colour_excluding(int exclude_idx)
{
    int idx;
    do {
        idx = sys_rand32_get() % NUM_COLOURS_M2;
    } while (idx == exclude_idx);
    return idx;
}

/* ══════════════════════════════════════════════════════════
 * Turn off all pods
 * ══════════════════════════════════════════════════════════ */
static void turn_off_all_pods(void)
{
    rgb_led_off();
    for (int i = 0; i < NUM_ESP32_PODS; i++) {
        write_to_pod(i, "OFF");
    }
}

/* ══════════════════════════════════════════════════════════
 * Send to phone via NUS notify
 * ══════════════════════════════════════════════════════════ */
static void send_to_phone(const char *msg)
{
    if (!notif_enabled) return;
    if (!msg || strlen(msg) == 0) return;

    k_mutex_lock(&state_mutex, K_FOREVER);
    struct bt_conn *c = phone_conn;
    k_mutex_unlock(&state_mutex);

    if (!c) return;
    bt_nus_send(c, msg, strlen(msg));
}

/* ══════════════════════════════════════════════════════════
 * Write command to ESP32 pod RX characteristic
 * ══════════════════════════════════════════════════════════ */
static void write_to_pod(int pod_idx, const char *cmd)
{
    if (pod_idx < 0 || pod_idx >= NUM_ESP32_PODS) return;
    if (!pods[pod_idx].connected || !pods[pod_idx].handles_found) return;

    bt_gatt_write_without_response(
        pods[pod_idx].conn,
        pods[pod_idx].rx_handle,
        cmd, strlen(cmd), false
    );
}

/* ══════════════════════════════════════════════════════════
 * NUS callbacks — phone → nRF
 * ══════════════════════════════════════════════════════════ */
static void notif_enabled_cb(bool enabled, void *ctx)
{
    ARG_UNUSED(ctx);
    notif_enabled = enabled;
}

static void received_from_phone(struct bt_conn *conn, const void *data,
                                 uint16_t len, void *ctx)
{
    ARG_UNUSED(ctx);

    char msg[32] = {0};
    memcpy(msg, data, MIN(len, sizeof(msg) - 1));

    for (int i = 0; i < (int)sizeof(msg); i++) {
        if (msg[i] == '\r' || msg[i] == '\n') { msg[i] = '\0'; break; }
    }

    printk("Rx:[%s]\n", msg);

    if (strncmp(msg, "TIMEOUT:", 8) == 0) {
        int secs = atoi(msg + 8);
        if (secs >= 1 && secs <= 60) {
            timeout_ms = (uint32_t)(secs * 1000);
            char reply[20];
            snprintf(reply, sizeof(reply), "TO:%ds\n", secs);
            send_to_phone(reply);
        } else {
            send_to_phone("TO:ERR\n");
        }
        return;
    }

    if (strcmp(msg, "STOP") == 0) {
        k_mutex_lock(&state_mutex, K_FOREVER);
        game_running = false;
        led_active   = false;
        k_mutex_unlock(&state_mutex);
        turn_off_all_pods();
        send_to_phone("STOPPED\n");
        k_sem_give(&round_done_sem);
        return;
    }

    if (current_mode == GAME_MODE_1) {
        if      (strcmp(msg, "RED")    == 0) { game_r=0xFF; game_g=0x00; game_b=0x00; }
        else if (strcmp(msg, "GREEN")  == 0) { game_r=0x00; game_g=0xFF; game_b=0x00; }
        else if (strcmp(msg, "BLUE")   == 0) { game_r=0x00; game_g=0x00; game_b=0xFF; }
        else if (strcmp(msg, "YELLOW") == 0) { game_r=0xFF; game_g=0xFF; game_b=0x00; }
        else if (strcmp(msg, "PINK")   == 0) { game_r=0xFF; game_g=0x14; game_b=0x93; }
        else if (strcmp(msg, "VIOLET") == 0) { game_r=0x94; game_g=0x00; game_b=0xD3; }
        else if (strcmp(msg, "START")  == 0) { /* use current colour */ }
        else { send_to_phone("UNKNOWN\n"); return; }

        k_mutex_lock(&state_mutex, K_FOREVER);
        game_running = true;
        k_mutex_unlock(&state_mutex);
        k_sem_give(&game_start_sem);
        send_to_phone("M1:GO\n");
        return;
    }

    if (current_mode == GAME_MODE_2) {
        if (strcmp(msg, "START") == 0) {
            k_mutex_lock(&state_mutex, K_FOREVER);
            game_running = true;
            k_mutex_unlock(&state_mutex);
            k_sem_give(&game_start_sem);
            send_to_phone("M2:GO\n");
        } else {
            send_to_phone("UNKNOWN\n");
        }
        return;
    }
}

static struct bt_nus_cb nus_listener = {
    .notif_enabled = notif_enabled_cb,
    .received      = received_from_phone,
};

/* ══════════════════════════════════════════════════════════
 * Receive notification from ESP32 pod
 * ══════════════════════════════════════════════════════════ */
static uint8_t pod_notify_cb(struct bt_conn *conn,
                              struct bt_gatt_subscribe_params *params,
                              const void *data, uint16_t length)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(params);

    if (!data || length == 0) return BT_GATT_ITER_CONTINUE;

    char msg[64] = {0};
    memcpy(msg, data, MIN(length, sizeof(msg) - 1));

    int mlen = strlen(msg);
    if (mlen > 0 && (msg[mlen-1] == '\n' || msg[mlen-1] == '\r')) {
        msg[mlen-1] = '\0';
    }

    printk("Pod notify:[%s]\n", msg);

    k_mutex_lock(&state_mutex, K_FOREVER);
    game_mode_t mode    = current_mode;
    bool        already = round_ended;
    int         odd_pod = odd_pod_index;
    k_mutex_unlock(&state_mutex);

    if (already) return BT_GATT_ITER_CONTINUE;

    if (mode == GAME_MODE_1) {
        bool m1_timeout = (strstr(msg, ":TIMEOUT") != NULL);

        char fwd[68];
        snprintf(fwd, sizeof(fwd), "%s\n", msg);
        send_to_phone(fwd);
        k_mutex_lock(&state_mutex, K_FOREVER);
        round_ended = true;
        k_mutex_unlock(&state_mutex);

        /* Tap on ESP32 pod → short beep. Pod timeout → long beep. */
        request_beep(m1_timeout ? BEEP_LONG : BEEP_SHORT);   /* ── NEW ── */

        k_sem_give(&round_done_sem);
        return BT_GATT_ITER_CONTINUE;
    }

    int tapped_avail_idx = -1;
    if      (strncmp(msg, "POD2:", 5) == 0) tapped_avail_idx = 1;
    else if (strncmp(msg, "POD3:", 5) == 0) tapped_avail_idx = 2;
    else if (strncmp(msg, "POD4:", 5) == 0) tapped_avail_idx = 3;

    if (tapped_avail_idx < 0) {
        send_to_phone(msg);
        return BT_GATT_ITER_CONTINUE;
    }

    bool is_timeout = (strstr(msg, ":TIMEOUT") != NULL);

    if (is_timeout) {
        char fwd[68];
        snprintf(fwd, sizeof(fwd), "%s\n", msg);
        send_to_phone(fwd);
        return BT_GATT_ITER_CONTINUE;
    }

    turn_off_all_pods();

    k_mutex_lock(&state_mutex, K_FOREVER);
    round_ended = true;
    led_active  = false;
    k_mutex_unlock(&state_mutex);

    printk("M2 judge: tapped=%d odd=%d\n", tapped_avail_idx, odd_pod);

    if (odd_pod < 0) {
        return BT_GATT_ITER_CONTINUE;
    }

    if (tapped_avail_idx == odd_pod) {
        char fwd[68];
        snprintf(fwd, sizeof(fwd), "%s\n", msg);
        send_to_phone(fwd);
        printk("CORRECT pod%d\n", tapped_avail_idx + 1);
        request_beep(BEEP_SHORT);
    } else {
        char wrong[32];
        snprintf(wrong, sizeof(wrong), "POD%d:WRONG\n", tapped_avail_idx + 1);
        send_to_phone(wrong);
        printk("WRONG pod%d tapped odd was pod%d\n",
               tapped_avail_idx + 1, odd_pod + 1);
        request_beep(BEEP_LONG);
    }

    k_sem_give(&round_done_sem);
    return BT_GATT_ITER_CONTINUE;
}

/* ══════════════════════════════════════════════════════════
 * GATT discovery
 * ══════════════════════════════════════════════════════════ */
static uint8_t discover_cb(struct bt_conn *conn,
                            const struct bt_gatt_attr *attr,
                            struct bt_gatt_discover_params *params)
{
    int pod_idx = -1;
    for (int i = 0; i < NUM_ESP32_PODS; i++) {
        if (params == &pods[i].disc_params) {
            pod_idx = i;
            break;
        }
    }
    if (pod_idx < 0) return BT_GATT_ITER_STOP;

    if (!attr) {
        printk("Discovery done Pod%d rx=%d ccc=%d found=%d\n",
               pod_idx + 2,
               pods[pod_idx].rx_handle,
               pods[pod_idx].tx_ccc_handle,
               pods[pod_idx].handles_found);

        if (pods[pod_idx].handles_found) {
            pods[pod_idx].sub_params.notify       = pod_notify_cb;
            pods[pod_idx].sub_params.value        = BT_GATT_CCC_NOTIFY;
            pods[pod_idx].sub_params.ccc_handle   =
                pods[pod_idx].tx_ccc_handle;
            pods[pod_idx].sub_params.value_handle =
                pods[pod_idx].tx_ccc_handle - 1;
            int err = bt_gatt_subscribe(conn,
                          &pods[pod_idx].sub_params);
            printk("Subscribe Pod%d err=%d\n", pod_idx + 2, err);
        }
        return BT_GATT_ITER_STOP;
    }

    if (!bt_uuid_cmp(((struct bt_gatt_chrc *)attr->user_data)->uuid,
                     &nus_rx_uuid.uuid)) {
        pods[pod_idx].rx_handle = bt_gatt_attr_value_handle(attr);
        printk("Pod%d RX=%d\n", pod_idx + 2, pods[pod_idx].rx_handle);
    }

    if (!bt_uuid_cmp(((struct bt_gatt_chrc *)attr->user_data)->uuid,
                     &nus_tx_uuid.uuid)) {
        pods[pod_idx].tx_ccc_handle = attr->handle + 2;
        pods[pod_idx].handles_found = true;
        printk("Pod%d TX decl=%d ccc=%d\n",
               pod_idx + 2, attr->handle,
               pods[pod_idx].tx_ccc_handle);
    }

    return BT_GATT_ITER_CONTINUE;
}

/* ══════════════════════════════════════════════════════════
 * BLE scan
 * ══════════════════════════════════════════════════════════ */
static bool parse_device_name(struct bt_data *data, void *user_data)
{
    char *dev_name = user_data;
    if (data->type == BT_DATA_NAME_COMPLETE ||
        data->type == BT_DATA_NAME_SHORTENED) {
        memcpy(dev_name, data->data, MIN(data->data_len, 31));
        dev_name[MIN(data->data_len, 31)] = '\0';
        return false;
    }
    return true;
}

static void scan_cb(const bt_addr_le_t *addr, int8_t rssi,
                    uint8_t type, struct net_buf_simple *ad_data)
{
    char dev_name[32] = {0};
    bt_data_parse(ad_data, parse_device_name, dev_name);

    for (int i = 0; i < NUM_ESP32_PODS; i++) {
        if (strcmp(dev_name, pod_names[i]) == 0 &&
            !pods[i].connected) {
            printk("Found %s slot=%d\n", dev_name, i);
            bt_le_scan_stop();
            last_connecting_pod = i;
            struct bt_conn *conn = NULL;
            bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN,
                              BT_LE_CONN_PARAM_DEFAULT, &conn);
            if (conn) bt_conn_unref(conn);
            return;
        }
    }
}

static void start_scan(void)
{
    bool need = false;
    for (int i = 0; i < NUM_ESP32_PODS; i++) {
        if (!pods[i].connected) { need = true; break; }
    }
    if (!need) return;

    struct bt_le_scan_param scan_param = {
        .type     = BT_LE_SCAN_TYPE_ACTIVE,
        .options  = BT_LE_SCAN_OPT_NONE,
        .interval = 0x0500,
        .window   = 0x0050,
    };
    bt_le_scan_start(&scan_param, scan_cb);
    printk("Scanning...\n");
}

/* ══════════════════════════════════════════════════════════
 * BLE connection callbacks
 * ══════════════════════════════════════════════════════════ */
static void on_connected(struct bt_conn *conn, uint8_t err)
{
    if (err) {
        printk("Conn failed err=%d\n", err);
        last_connecting_pod = -1;
        start_scan();
        return;
    }

    struct bt_conn_info info;
    bt_conn_get_info(conn, &info);

    if (info.role == BT_CONN_ROLE_CENTRAL) {
        int idx = last_connecting_pod;
        last_connecting_pod = -1;

        if (idx < 0 || idx >= NUM_ESP32_PODS) {
            printk("Invalid pod slot\n");
            start_scan();
            return;
        }

        pods[idx].conn      = bt_conn_ref(conn);
        pods[idx].connected = true;
        printk("Pod%d connected slot=%d\n", idx + 2, idx);

        pods[idx].disc_params.uuid         = NULL;
        pods[idx].disc_params.func         = discover_cb;
        pods[idx].disc_params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
        pods[idx].disc_params.end_handle   = BT_ATT_LAST_ATTRIBUTE_HANDLE;
        pods[idx].disc_params.type         = BT_GATT_DISCOVER_CHARACTERISTIC;

        bt_gatt_discover(conn, &pods[idx].disc_params);
        start_scan();

    } else {
        printk("Phone connected\n");
        k_mutex_lock(&state_mutex, K_FOREVER);
        phone_conn = bt_conn_ref(conn);
        k_mutex_unlock(&state_mutex);
    }
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
    struct bt_conn_info info;
    bt_conn_get_info(conn, &info);

    if (info.role == BT_CONN_ROLE_CENTRAL) {
        for (int i = 0; i < NUM_ESP32_PODS; i++) {
            if (pods[i].conn == conn) {
                printk("Pod%d disconnected\n", i + 2);
                bt_conn_unref(pods[i].conn);
                pods[i].conn          = NULL;
                pods[i].connected     = false;
                pods[i].handles_found = false;
                pods[i].rx_handle     = 0;
                pods[i].tx_ccc_handle = 0;
                start_scan();
                break;
            }
        }
    } else {
        printk("Phone disconnected\n");
        k_mutex_lock(&state_mutex, K_FOREVER);
        if (phone_conn) {
            bt_conn_unref(phone_conn);
            phone_conn    = NULL;
        }
        game_running  = false;
        led_active    = false;
        notif_enabled = false;
        k_mutex_unlock(&state_mutex);
        turn_off_all_pods();
        k_sem_give(&adv_sem);
    }
}

static struct bt_conn_cb conn_callbacks = {
    .connected    = on_connected,
    .disconnected = on_disconnected,
};

/* ══════════════════════════════════════════════════════════
 * Mode switch button callback
 * ══════════════════════════════════════════════════════════ */
static void button_pressed(const struct device *dev,
                            struct gpio_callback *cb,
                            uint32_t pins)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(cb);
    ARG_UNUSED(pins);

    if (gpio_pin_get_dt(&mode_btn) != 1) {
        return;
    }

    if (current_mode == GAME_MODE_1) {
        current_mode = GAME_MODE_2;
        printk("MODE 2\n");
        send_to_phone("MODE:2\n");
    } else {
        current_mode = GAME_MODE_1;
        printk("MODE 1\n");
        send_to_phone("MODE:1\n");
    }

    k_mutex_lock(&state_mutex, K_FOREVER);
    bool running = game_running;
    k_mutex_unlock(&state_mutex);

    if (running) {
        turn_off_all_pods();
        k_mutex_lock(&state_mutex, K_FOREVER);
        led_active  = false;
        round_ended = true;
        k_mutex_unlock(&state_mutex);
        k_sem_give(&round_done_sem);
        k_sem_give(&game_start_sem);
    }
}

/* ══════════════════════════════════════════════════════════
 * IR thread
 * ══════════════════════════════════════════════════════════ */
static void ir_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    bool prev_detected = false;

    while (true) {
        k_sleep(K_MSEC(IR_POLL_INTERVAL_MS));

        k_mutex_lock(&state_mutex, K_FOREVER);
        bool    is_active = led_active;
        int64_t start     = led_on_time_ms;
        k_mutex_unlock(&state_mutex);

        if (!is_active) {
            prev_detected = false;
            continue;
        }

        int64_t elapsed = k_uptime_get() - start;

        if (elapsed >= timeout_ms) {
            k_mutex_lock(&state_mutex, K_FOREVER);
            bool        already = round_ended;
            game_mode_t mode    = current_mode;
            k_mutex_unlock(&state_mutex);

            if (!already) {
                rgb_led_off();
                k_mutex_lock(&state_mutex, K_FOREVER);
                led_active = false;
                if (mode == GAME_MODE_1) {
                    round_ended = true;
                }
                k_mutex_unlock(&state_mutex);

                send_to_phone("POD1:TO\n");
                request_beep(BEEP_LONG);

                if (mode == GAME_MODE_1) {
                    k_sem_give(&round_done_sem);
                }
            }
            prev_detected = false;
            continue;
        }

        bool detected = ir_sensor_detected();

        if (!detected || prev_detected) {
            prev_detected = detected;
            continue;
        }
        prev_detected = detected;

        k_mutex_lock(&state_mutex, K_FOREVER);
        bool        already = round_ended;
        game_mode_t mode    = current_mode;
        int         odd_pod = odd_pod_index;
        k_mutex_unlock(&state_mutex);

        if (already) continue;

        if (mode == GAME_MODE_1) {
            rgb_led_off();
            k_mutex_lock(&state_mutex, K_FOREVER);
            led_active  = false;
            round_ended = true;
            k_mutex_unlock(&state_mutex);

            char msg[24];
            snprintf(msg, sizeof(msg), "POD1:RT:%d\n", (int32_t)elapsed);
            send_to_phone(msg);
            printk("%s", msg);
            request_beep(BEEP_SHORT);
            k_sem_give(&round_done_sem);

        } else {
            turn_off_all_pods();
            k_mutex_lock(&state_mutex, K_FOREVER);
            led_active  = false;
            round_ended = true;
            k_mutex_unlock(&state_mutex);

            if (odd_pod == 0) {
                char msg[24];
                snprintf(msg, sizeof(msg),
                         "POD1:RT:%d\n", (int32_t)elapsed);
                send_to_phone(msg);
                printk("%s", msg);
                request_beep(BEEP_SHORT);
            } else {
                send_to_phone("POD1:WRONG\n");
                printk("POD1:WRONG\n");
                request_beep(BEEP_LONG);
            }
            k_sem_give(&round_done_sem);
        }
    }
}

/* ══════════════════════════════════════════════════════════
 * Game thread
 * ══════════════════════════════════════════════════════════ */
static void game_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    while (true) {
        k_sem_take(&game_start_sem, K_FOREVER);

        k_mutex_lock(&state_mutex, K_FOREVER);
        bool        running = game_running;
        game_mode_t mode    = current_mode;
        uint32_t    tmo     = timeout_ms;
        k_mutex_unlock(&state_mutex);

        if (!running) continue;

        int available[4];
        int count = build_available(available);

        printk("Mode%d %d pods\n", mode, count);

        if (mode == GAME_MODE_1) {
            k_mutex_lock(&state_mutex, K_FOREVER);
            uint8_t r = game_r, g = game_g, b = game_b;
            k_mutex_unlock(&state_mutex);

            k_mutex_lock(&state_mutex, K_FOREVER);
            round_ended = false;
            k_mutex_unlock(&state_mutex);

            /* Drain any stale semaphore gives from previous rounds */
            while (k_sem_take(&round_done_sem, K_NO_WAIT) == 0) {}

            int pod = available[sys_rand32_get() % count];
            printk("M1:pod=%d\n", pod + 1);

            if (pod == 0) {
                rgb_led_set_all(r, g, b, 0xFF);
                k_mutex_lock(&state_mutex, K_FOREVER);
                led_active     = true;
                led_on_time_ms = k_uptime_get();
                k_mutex_unlock(&state_mutex);
                send_to_phone("POD1:ON\n");
            } else {
                char cmd[16];
                snprintf(cmd, sizeof(cmd), "ON:%02X%02X%02X", r, g, b);
                write_to_pod(pod - 1, cmd);
                k_sleep(K_MSEC(20));
                char notify[20];
                snprintf(notify, sizeof(notify), "POD%d:ON\n", pod + 1);
                send_to_phone(notify);
            }

            int sem_ret = k_sem_take(&round_done_sem, K_MSEC(tmo + 500));

            turn_off_all_pods();
            k_mutex_lock(&state_mutex, K_FOREVER);
            led_active = false;
            round_ended = true;
            k_mutex_unlock(&state_mutex);

            /* If nobody tapped in time, fire the long beep ourselves —
             * don't rely solely on the ESP32 pod's own remote timeout
             * notification, since with short timeouts that message can
             * race the dongle's own deadline over the BLE link and
             * never arrive before we already turn the pod off. */
            if (sem_ret != 0) {
                request_beep(BEEP_LONG);   /* ── FIX: dongle-driven Mode 1 timeout beep ── */
            }

        } else {
            int same_idx = sys_rand32_get() % NUM_COLOURS_M2;
            int odd_idx  = random_m2_colour_excluding(same_idx);
            int odd_pod  = available[sys_rand32_get() % count];

            printk("M2:same=%s odd=%s odd_pod=%d\n",
                   colours_m2[same_idx].name,
                   colours_m2[odd_idx].name,
                   odd_pod + 1);

            k_mutex_lock(&state_mutex, K_FOREVER);
            odd_pod_index = odd_pod;
            round_ended   = false;
            k_mutex_unlock(&state_mutex);

            /* Drain any stale semaphore gives from previous rounds */
            while (k_sem_take(&round_done_sem, K_NO_WAIT) == 0) {}

            printk("M2 set: odd_pod=%d count=%d\n", odd_pod, count);

            int64_t round_start = k_uptime_get();

            for (int i = 0; i < count; i++) {
                int  p      = available[i];
                bool is_odd = (p == odd_pod);

                uint8_t r = is_odd ? colours_m2[odd_idx].r
                                   : colours_m2[same_idx].r;
                uint8_t g = is_odd ? colours_m2[odd_idx].g
                                   : colours_m2[same_idx].g;
                uint8_t b = is_odd ? colours_m2[odd_idx].b
                                   : colours_m2[same_idx].b;

                if (p == 0) {
                    rgb_led_set_all(r, g, b, 0xFF);
                    k_mutex_lock(&state_mutex, K_FOREVER);
                    led_active     = true;
                    led_on_time_ms = round_start;
                    k_mutex_unlock(&state_mutex);
                } else {
                    char cmd[16];
                    snprintf(cmd, sizeof(cmd),
                             "ON:%02X%02X%02X", r, g, b);
                    write_to_pod(p - 1, cmd);
                    k_sleep(K_MSEC(20));
                }
            }

            char notify[20];
            snprintf(notify, sizeof(notify),
                     "ODD:POD%d\n", odd_pod + 1);
            send_to_phone(notify);

            int sem_ret = k_sem_take(&round_done_sem, K_MSEC(tmo));

            turn_off_all_pods();
            k_mutex_lock(&state_mutex, K_FOREVER);
            led_active  = false;
            round_ended = true;
            k_mutex_unlock(&state_mutex);

            if (sem_ret != 0) {
                send_to_phone("ROUND:TO\n");
                request_beep(BEEP_LONG);
            }
        }

        k_sleep(K_SECONDS(1));

        k_mutex_lock(&state_mutex, K_FOREVER);
        bool still_running = game_running;
        k_mutex_unlock(&state_mutex);

        if (still_running) {
            k_sem_give(&game_start_sem);
        }
    }
}

/* ══════════════════════════════════════════════════════════
 * UART console thread
 * ══════════════════════════════════════════════════════════ */
static void uart_console_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    printk("UART console ready — type commands:\n");
    printk("  START, STOP, RED, GREEN, BLUE, YELLOW, PINK, VIOLET, TIMEOUT:N\n");

    while (true) {
        char *line = console_getline();
        if (!line) continue;

        char cmd[32] = {0};
        int j = 0;
        for (int i = 0; line[i] && j < (int)(sizeof(cmd) - 1); i++) {
            char c = line[i];
            if (c == '\r' || c == '\n') break;
            if (c >= 'a' && c <= 'z') c -= 32;
            cmd[j++] = c;
        }
        cmd[j] = '\0';

        if (cmd[0] == '\0') continue;

        printk("Console cmd:[%s]\n", cmd);

        if (strncmp(cmd, "TIMEOUT:", 8) == 0) {
            int secs = atoi(cmd + 8);
            if (secs >= 1 && secs <= 60) {
                timeout_ms = (uint32_t)(secs * 1000);
                printk("Timeout set to %ds\n", secs);
            } else {
                printk("TIMEOUT error (1-60)\n");
            }
            continue;
        }

        if (strcmp(cmd, "STOP") == 0) {
            k_mutex_lock(&state_mutex, K_FOREVER);
            game_running = false;
            led_active   = false;
            k_mutex_unlock(&state_mutex);
            turn_off_all_pods();
            printk("Game STOPPED\n");
            send_to_phone("STOPPED\n");
            k_sem_give(&round_done_sem);
            continue;
        }

        if (current_mode == GAME_MODE_1) {
            if      (strcmp(cmd, "RED")    == 0) { game_r=0xFF; game_g=0x00; game_b=0x00; }
            else if (strcmp(cmd, "GREEN")  == 0) { game_r=0x00; game_g=0xFF; game_b=0x00; }
            else if (strcmp(cmd, "BLUE")   == 0) { game_r=0x00; game_g=0x00; game_b=0xFF; }
            else if (strcmp(cmd, "YELLOW") == 0) { game_r=0xFF; game_g=0xFF; game_b=0x00; }
            else if (strcmp(cmd, "PINK")   == 0) { game_r=0xFF; game_g=0x14; game_b=0x93; }
            else if (strcmp(cmd, "VIOLET") == 0) { game_r=0x94; game_g=0x00; game_b=0xD3; }
            else if (strcmp(cmd, "START")  == 0) { /* use current colour */ }
            else { printk("Unknown command\n"); continue; }

            k_mutex_lock(&state_mutex, K_FOREVER);
            game_running = true;
            k_mutex_unlock(&state_mutex);
            k_sem_give(&game_start_sem);
            printk("Mode 1 GO (R=%02X G=%02X B=%02X)\n", game_r, game_g, game_b);
            send_to_phone("M1:GO\n");
            continue;
        }

        if (current_mode == GAME_MODE_2) {
            if (strcmp(cmd, "START") == 0) {
                k_mutex_lock(&state_mutex, K_FOREVER);
                game_running = true;
                k_mutex_unlock(&state_mutex);
                k_sem_give(&game_start_sem);
                printk("Mode 2 GO\n");
                send_to_phone("M2:GO\n");
            } else {
                printk("Unknown command\n");
            }
            continue;
        }
    }
}

/* ══════════════════════════════════════════════════════════
 * main()
 * ══════════════════════════════════════════════════════════ */
int main(void)
{
    int err;

    printk("Blazepod1 starting\n");

    if (!gpio_is_ready_dt(&mode_btn)) {
        printk("Mode btn not ready\n");
        return 0;
    }
    gpio_pin_configure_dt(&mode_btn, GPIO_INPUT);
    gpio_pin_interrupt_configure_dt(&mode_btn, GPIO_INT_EDGE_TO_ACTIVE);
    gpio_init_callback(&button_cb_data, button_pressed,
                       BIT(mode_btn.pin));
    gpio_add_callback(mode_btn.port, &button_cb_data);
    printk("Mode btn ready pin=%d\n", mode_btn.pin);

    if (rgb_led_init() != 0) { printk("LED failed\n"); return 0; }
    rgb_led_off();

    if (ir_sensor_init() != 0) { printk("IR failed\n"); return 0; }

    if (buzzer_init() != 0) { printk("Buzzer failed\n"); return 0; }

    memset(pods, 0, sizeof(pods));
    last_connecting_pod = -1;

    k_thread_create(&ir_thread_data, ir_stack,
                    K_THREAD_STACK_SIZEOF(ir_stack),
                    ir_thread_fn, NULL, NULL, NULL,
                    IR_THREAD_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&ir_thread_data, "ir_pod1");

    k_thread_create(&game_thread_data, game_stack,
                    K_THREAD_STACK_SIZEOF(game_stack),
                    game_thread_fn, NULL, NULL, NULL,
                    GAME_THREAD_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&game_thread_data, "game");

    k_thread_create(&buzz_thread_data, buzz_stack,
                    K_THREAD_STACK_SIZEOF(buzz_stack),
                    buzz_thread_fn, NULL, NULL, NULL,
                    BUZZ_THREAD_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&buzz_thread_data, "buzzer");

    console_getline_init();

    bt_conn_cb_register(&conn_callbacks);

    err = bt_nus_cb_register(&nus_listener, NULL);
    if (err) return err;

    err = bt_enable(NULL);
    if (err) return err;

    bt_le_adv_stop();
    bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad),
                    sd, ARRAY_SIZE(sd));
    printk("Advertising\n");

    start_scan();

    k_thread_create(&uart_thread_data, uart_stack,
                    K_THREAD_STACK_SIZEOF(uart_stack),
                    uart_console_thread_fn, NULL, NULL, NULL,
                    UART_THREAD_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&uart_thread_data, "uart_cmd");

    while (true) {
        k_sem_take(&adv_sem, K_FOREVER);
        bt_le_adv_stop();
        bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad),
                        sd, ARRAY_SIZE(sd));
        printk("Adv restarted\n");
    }

    return 0;
}