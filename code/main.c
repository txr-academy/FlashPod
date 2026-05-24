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
#include <zephyr/drivers/gpio.h>

#define DEVICE_NAME     CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)

/* ── NUS UUIDs ──────────────────────────────────────────── */
#define NUS_SERVICE_UUID_VAL \
    BT_UUID_128_ENCODE(0x6E400001, 0xB5A3, 0xF393, 0xE0A9, 0xE50E24DCCA9E)
#define NUS_RX_UUID_VAL \
    BT_UUID_128_ENCODE(0x6E400002, 0xB5A3, 0xF393, 0xE0A9, 0xE50E24DCCA9E)
#define NUS_TX_UUID_VAL \
    BT_UUID_128_ENCODE(0x6E400003, 0xB5A3, 0xF393, 0xE0A9, 0xE50E24DCCA9E)

static struct bt_uuid_128 nus_rx_uuid = BT_UUID_INIT_128(NUS_RX_UUID_VAL);
static struct bt_uuid_128 nus_tx_uuid = BT_UUID_INIT_128(NUS_TX_UUID_VAL);

/* ── Pod names to scan for ──────────────────────────────── */
#define NUM_ESP32_PODS  3
static const char *pod_names[NUM_ESP32_PODS] = {
    "Blazepod2", "Blazepod3", "Blazepod4"
};

/* ── Per-pod state ──────────────────────────────────────── */
typedef struct {
    struct bt_conn *conn;
    uint16_t        rx_handle;
    uint16_t        tx_ccc_handle;
    bool            connected;
    bool            handles_found;
} pod_t;

static pod_t pods[NUM_ESP32_PODS];

/* ── IR thread ──────────────────────────────────────────── */
#define IR_THREAD_STACK_SIZE  2048
#define IR_THREAD_PRIORITY    5
#define IR_POLL_INTERVAL_MS   50
static uint32_t timeout_ms = 3000U;  /* default 10s, settable by phone */
/* Game mode */
typedef enum {
    GAME_MODE_1 = 1,
    GAME_MODE_2 = 2
} game_mode_t;

static game_mode_t current_mode = GAME_MODE_1;

/* GPIO button */
#define MODE_BUTTON_NODE DT_ALIAS(mode_switch)

static const struct gpio_dt_spec mode_btn =
    GPIO_DT_SPEC_GET(MODE_BUTTON_NODE, gpios);
static struct gpio_callback button_cb_data;

K_THREAD_STACK_DEFINE(ir_stack, IR_THREAD_STACK_SIZE);
static struct k_thread ir_thread_data;

/* ── Game thread ────────────────────────────────────────── */
#define GAME_THREAD_STACK_SIZE  1024
#define GAME_THREAD_PRIORITY    6

K_THREAD_STACK_DEFINE(game_stack, GAME_THREAD_STACK_SIZE);
static struct k_thread game_thread_data;

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
static bool            round_ended    = false;   /* ← add this */
static int             odd_pod_index  = -1;    

/* Current game colour */
static uint8_t game_r = 0xFF;
static uint8_t game_g = 0x00;
static uint8_t game_b = 0x00;
typedef struct {
    uint8_t     r, g, b;
    const char *name;
} colour_t;

static const colour_t colours[] = {
    { 0xFF, 0x00, 0x00, "RED"    },
    { 0x00, 0xFF, 0x00, "GREEN"  },
    { 0x00, 0x00, 0xFF, "BLUE"   },
    { 0xFF, 0xFF, 0x00, "YELLOW" },
    { 0xFF, 0x14, 0x93, "PINK"   },
    { 0x94, 0x00, 0xD3, "VIOLET" },
};
#define NUM_COLOURS ARRAY_SIZE(colours)

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
static void start_scan(void);
static void write_to_pod(int pod_idx, const char *cmd);
static void turn_off_all_pods(void); 

/* ══════════════════════════════════════════════════════════
 * Send to phone via NUS notify
 * ══════════════════════════════════════════════════════════ */
static void send_to_phone(const char *msg)
{
    if (!notif_enabled) return;
    if (msg == NULL || strlen(msg) == 0) return;

    k_mutex_lock(&state_mutex, K_FOREVER);
    struct bt_conn *c = phone_conn;
    k_mutex_unlock(&state_mutex);

    if (c == NULL) return;

    int rc = bt_nus_send(c, msg, strlen(msg));
    if (rc) {
        printk("bt_nus_send failed rc=%d\n", rc);
    }
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

    printk("Received: [%s]\n", msg);

    /* ── Set timeout (works for both modes) ── */
    if (strncmp(msg, "TIMEOUT:", 8) == 0) {
        int secs = atoi(msg + 8);
        if (secs > 0) {
            timeout_ms = (uint32_t)(secs * 1000);
            char reply[32];
            snprintf(reply, sizeof(reply),
                     "TIMEOUT SET:%ds\n", secs);
            send_to_phone(reply);
            printk("Timeout set to %d ms\n", timeout_ms);
        } else {
            send_to_phone("INVALID TIMEOUT\n");
        }
        return;
    }

    /* ── Stop game (works for both modes) ── */
    if (strcmp(msg, "STOP") == 0) {
        k_mutex_lock(&state_mutex, K_FOREVER);
        game_running = false;
        led_active   = false;
        k_mutex_unlock(&state_mutex);

        rgb_led_off();
        send_to_phone("GAME STOPPED\n");
        for (int i = 0; i < NUM_ESP32_PODS; i++) {
            write_to_pod(i, "OFF");
        }
        k_sem_give(&round_done_sem);
        return;
    }

    /* ── Mode 1 only: colour commands start the game ── */
    if (current_mode == GAME_MODE_1) {
        if (strcmp(msg, "RED") == 0) {
            game_r = 0xFF; game_g = 0x00; game_b = 0x00;
        } else if (strcmp(msg, "GREEN") == 0) {
            game_r = 0x00; game_g = 0xFF; game_b = 0x00;
        } else if (strcmp(msg, "BLUE") == 0) {
            game_r = 0x00; game_g = 0x00; game_b = 0xFF;
        } else if (strcmp(msg, "YELLOW") == 0) {
            game_r = 0xFF; game_g = 0xFF; game_b = 0x00;
        } else if (strcmp(msg, "PINK") == 0) {
            game_r = 0xFF; game_g = 0x14; game_b = 0x93;
        } else if (strcmp(msg, "VIOLET") == 0) {
            game_r = 0x94; game_g = 0x00; game_b = 0xD3;
        } else {
            send_to_phone("UNKNOWN\n");
            return;
        }

        k_mutex_lock(&state_mutex, K_FOREVER);
        game_running = true;
        k_mutex_unlock(&state_mutex);

        k_sem_give(&game_start_sem);
        send_to_phone("GAME STARTED\n");
        return;
    }

    /* ── Mode 2 only: START command ── */
    if (current_mode == GAME_MODE_2) {
        if (strcmp(msg, "START") == 0) {
            k_mutex_lock(&state_mutex, K_FOREVER);
            game_running = true;
            k_mutex_unlock(&state_mutex);

            k_sem_give(&game_start_sem);
            send_to_phone("GAME STARTED\n");
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
 * Receive RT/TIMEOUT notification from ESP32 pod
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
    printk("Pod notify: %s\n", msg);

    k_mutex_lock(&state_mutex, K_FOREVER);
    game_mode_t mode    = current_mode;
    bool        already = round_ended;
    int         odd_pod = odd_pod_index;
    k_mutex_unlock(&state_mutex);

    if (already) return BT_GATT_ITER_CONTINUE;

    if (mode == GAME_MODE_1) {
        send_to_phone(msg);
        k_sem_give(&round_done_sem);
        return BT_GATT_ITER_CONTINUE;
    }

    /* Mode 2 — identify which pod sent this */
    int tapped_pod = -1;
    if      (strncmp(msg, "POD2:", 5) == 0) tapped_pod = 1;
    else if (strncmp(msg, "POD3:", 5) == 0) tapped_pod = 2;
    else if (strncmp(msg, "POD4:", 5) == 0) tapped_pod = 3;

    if (tapped_pod < 0) {
        send_to_phone(msg);
        return BT_GATT_ITER_CONTINUE;
    }

    bool is_timeout = (strstr(msg, "TIMEOUT") != NULL);

    turn_off_all_pods();
    k_mutex_lock(&state_mutex, K_FOREVER);
    round_ended = true;
    led_active  = false;
    k_mutex_unlock(&state_mutex);

    if (is_timeout) {
        send_to_phone(msg);
    } else if (tapped_pod == odd_pod) {
        /* Correct pod */
        send_to_phone(msg);
    } else {
        /* Wrong pod */
        char wrong[32];
        snprintf(wrong, sizeof(wrong),
                 "POD%d:WRONG\n", tapped_pod + 1);
        send_to_phone(wrong);
    }

    k_sem_give(&round_done_sem);
    return BT_GATT_ITER_CONTINUE;
}

/* ══════════════════════════════════════════════════════════
 * GATT discovery — find RX and TX handles on ESP32
 * ══════════════════════════════════════════════════════════ */
static struct bt_gatt_discover_params disc_params;
static struct bt_gatt_subscribe_params sub_params[NUM_ESP32_PODS];
static int current_disc_pod = -1;

static uint8_t discover_cb(struct bt_conn *conn,
                            const struct bt_gatt_attr *attr,
                            struct bt_gatt_discover_params *params)
{
    if (!attr) {
        printk("Discovery complete for Pod%d\n", current_disc_pod + 2);
        printk("  rx_handle=%d tx_ccc_handle=%d handles_found=%d\n",
               pods[current_disc_pod].rx_handle,
               pods[current_disc_pod].tx_ccc_handle,
               pods[current_disc_pod].handles_found);

        if (current_disc_pod >= 0 &&
            pods[current_disc_pod].handles_found) {
            sub_params[current_disc_pod].notify       = pod_notify_cb;
sub_params[current_disc_pod].value        = BT_GATT_CCC_NOTIFY;
sub_params[current_disc_pod].ccc_handle   = pods[current_disc_pod].tx_ccc_handle;
sub_params[current_disc_pod].value_handle = pods[current_disc_pod].tx_ccc_handle - 1;
int err = bt_gatt_subscribe(conn, &sub_params[current_disc_pod]);
printk("Subscribe err=%d\n", err);
        }
        return BT_GATT_ITER_STOP;
    }

    /* Print every attribute found for debugging */
    printk("Attr handle=%d\n", attr->handle);

    /* Check RX characteristic by UUID */
    if (!bt_uuid_cmp(((struct bt_gatt_chrc *)attr->user_data)->uuid,
                     &nus_rx_uuid.uuid)) {
        pods[current_disc_pod].rx_handle =
            bt_gatt_attr_value_handle(attr);
        printk("Found RX handle=%d\n",
               pods[current_disc_pod].rx_handle);
    }

    /* Check TX characteristic by UUID */
    if (!bt_uuid_cmp(((struct bt_gatt_chrc *)attr->user_data)->uuid,
                 &nus_tx_uuid.uuid)) {
    pods[current_disc_pod].tx_ccc_handle = attr->handle + 2;
    pods[current_disc_pod].handles_found = true;
    printk("Found TX decl=%d value=%d ccc=%d\n",
           attr->handle,
           attr->handle + 1,
           pods[current_disc_pod].tx_ccc_handle);
}

    return BT_GATT_ITER_CONTINUE;
}

/* ══════════════════════════════════════════════════════════
 * BLE scan — look for Blazepod2/3/4
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

            printk("Found %s — connecting\n", dev_name);
            bt_le_scan_stop();

            struct bt_conn *conn = NULL;
            bt_conn_le_create(addr,
                              BT_CONN_LE_CREATE_CONN,
                              BT_LE_CONN_PARAM_DEFAULT,
                              &conn);
            if (conn) {
                bt_conn_unref(conn);
            }
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

    /* Use slower scan interval so advertising gaps are preserved
     * interval = 500ms, window = 50ms
     * This means nRF scans for 50ms then rests 450ms
     * During the 450ms rest it can advertise and accept connections */
    struct bt_le_scan_param scan_param = {
        .type     = BT_LE_SCAN_TYPE_ACTIVE,
        .options  = BT_LE_SCAN_OPT_NONE,
        .interval = 0x0500,   /* 800ms */
        .window   = 0x0050,   /* 50ms  */
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
        printk("Connection failed err=%d\n", err);
        start_scan();
        return;
    }

    struct bt_conn_info info;
    bt_conn_get_info(conn, &info);

    if (info.role == BT_CONN_ROLE_CENTRAL) {
        /* Connected to an ESP32 pod */
        for (int i = 0; i < NUM_ESP32_PODS; i++) {
            if (!pods[i].connected && pods[i].conn == NULL) {
                pods[i].conn      = bt_conn_ref(conn);
                pods[i].connected = true;
                current_disc_pod  = i;

                printk("Connected to Pod%d — discovering\n", i + 2);

                /* Discover ALL characteristics — no UUID filter */
disc_params.uuid         = NULL;   /* NULL = find everything */
disc_params.func         = discover_cb;
disc_params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
disc_params.end_handle   = BT_ATT_LAST_ATTRIBUTE_HANDLE;
disc_params.type         = BT_GATT_DISCOVER_CHARACTERISTIC;
int err = bt_gatt_discover(conn, &disc_params);
printk("Discovery started err=%d\n", err);
                break;
            }
        }
        start_scan();

    } else {
        /* Connected to phone */
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

        rgb_led_off();
        for (int i = 0; i < NUM_ESP32_PODS; i++) {
            write_to_pod(i, "OFF");
        }
        k_sem_give(&adv_sem);
    }
}

static struct bt_conn_cb conn_callbacks = {
    .connected    = on_connected,
    .disconnected = on_disconnected,
};

/* ══════════════════════════════════════════════════════════
 * IR thread — Pod 1 (nRF own LED + IR)
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

        /* Timeout */
        if (elapsed >= timeout_ms) {
    k_mutex_lock(&state_mutex, K_FOREVER);
    bool already = round_ended;
    k_mutex_unlock(&state_mutex);

    if (!already) {
        turn_off_all_pods();
        k_mutex_lock(&state_mutex, K_FOREVER);
        led_active  = false;
        round_ended = true;
        k_mutex_unlock(&state_mutex);

        send_to_phone("POD1:TIMEOUT\n");
        printk("POD1:TIMEOUT\n");
        k_sem_give(&round_done_sem);
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
bool        already  = round_ended;
game_mode_t mode     = current_mode;
int         odd_pod  = odd_pod_index;
k_mutex_unlock(&state_mutex);

if (already) continue;

if (mode == GAME_MODE_1) {
    rgb_led_off();
    k_mutex_lock(&state_mutex, K_FOREVER);
    led_active  = false;
    round_ended = true;
    k_mutex_unlock(&state_mutex);

    char msg[32];
    snprintf(msg, sizeof(msg),
             "POD1:RT:%d\n", (int32_t)elapsed);
    send_to_phone(msg);
    printk("%s", msg);
    k_sem_give(&round_done_sem);

} else {
    /* Mode 2 — check if pod 1 is the odd pod */
    turn_off_all_pods();
    k_mutex_lock(&state_mutex, K_FOREVER);
    led_active  = false;
    round_ended = true;
    k_mutex_unlock(&state_mutex);

    if (odd_pod == 0) {
        /* Correct pod tapped */
        char msg[32];
        snprintf(msg, sizeof(msg),
                 "POD1:RT:%d\n", (int32_t)elapsed);
        send_to_phone(msg);
        printk("%s", msg);
    } else {
        /* Wrong pod tapped */
        send_to_phone("POD1:WRONG\n");
        printk("POD1:WRONG\n");
    }
    k_sem_give(&round_done_sem);
}
    }
}
/* Replace the mutex-based button_pressed with this */
static void button_pressed(const struct device *dev,
                            struct gpio_callback *cb,
                            uint32_t pins)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(cb);
    ARG_UNUSED(pins);

    if (current_mode == GAME_MODE_1) {
        current_mode = GAME_MODE_2;
        printk("Switched to MODE 2\n");
    } else {
        current_mode = GAME_MODE_1;
        printk("Switched to MODE 1\n");
    }

    /* If game is running, stop current round cleanly
     * and signal game thread to restart in new mode */
    if (game_running) {
        turn_off_all_pods();
        led_active  = false;
        round_ended = true;
        k_sem_give(&round_done_sem);   /* unblocks current round */
        k_sem_give(&game_start_sem);   /* restarts game in new mode */
    }
}
static int random_colour_excluding(int exclude_idx)
{
    int idx;
    do {
        idx = sys_rand32_get() % NUM_COLOURS;
    } while (idx == exclude_idx);
    return idx;
}
static void turn_off_all_pods(void)
{
    rgb_led_off();
    for (int i = 0; i < NUM_ESP32_PODS; i++) {
        write_to_pod(i, "OFF");
    }
}
/* ══════════════════════════════════════════════════════════
 * Game thread — random pod selection loop
 * ══════════════════════════════════════════════════════════ */
static void run_game_mode2(void)
{
    while (true) {
        k_mutex_lock(&state_mutex, K_FOREVER);
        bool     running = game_running;
        uint32_t t_out   = timeout_ms;
        k_mutex_unlock(&state_mutex);

        if (!running) break;

        /* Pick majority and odd colours */
        int maj_idx = sys_rand32_get() % NUM_COLOURS;
        int odd_idx = random_colour_excluding(maj_idx);

        /* Pick which pod gets the odd colour 0=nRF 1-3=ESP32 */
        int odd_pod = sys_rand32_get() % 4;

        printk("Mode2: maj=%s odd=%s odd_pod=%d\n",
               colours[maj_idx].name,
               colours[odd_idx].name,
               odd_pod + 1);

        k_mutex_lock(&state_mutex, K_FOREVER);
        odd_pod_index = odd_pod;
        round_ended   = false;
        k_mutex_unlock(&state_mutex);

        int64_t round_start = k_uptime_get();

        /* Light all 4 pods simultaneously */
        for (int i = 0; i < 4; i++) {
            bool    is_odd = (i == odd_pod);
            uint8_t r = is_odd ? colours[odd_idx].r
                               : colours[maj_idx].r;
            uint8_t g = is_odd ? colours[odd_idx].g
                               : colours[maj_idx].g;
            uint8_t b = is_odd ? colours[odd_idx].b
                               : colours[maj_idx].b;

            if (i == 0) {
                /* nRF Pod 1 */
                rgb_led_set_all(r, g, b, 0xFF);
                k_mutex_lock(&state_mutex, K_FOREVER);
                led_active     = true;
                led_on_time_ms = round_start;
                k_mutex_unlock(&state_mutex);
            } else {
                /* ESP32 pods */
                char cmd[16];
                snprintf(cmd, sizeof(cmd),
                         "ON:%02X%02X%02X", r, g, b);
                write_to_pod(i - 1, cmd);
            }
        }

        send_to_phone("MODE2:ALL_ON\n");

        /* Wait for result */
        k_sem_take(&round_done_sem, K_MSEC(t_out + 2000));

        /* Clean up */
        turn_off_all_pods();
        k_mutex_lock(&state_mutex, K_FOREVER);
        led_active = false;
        k_mutex_unlock(&state_mutex);

        k_sleep(K_SECONDS(2));
    }
}

static void game_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    while (true) {
        k_sem_take(&game_start_sem, K_FOREVER);

        k_mutex_lock(&state_mutex, K_FOREVER);
        game_mode_t mode = current_mode;
        bool        running = game_running;
        k_mutex_unlock(&state_mutex);

        if (!running) continue;

        printk("Starting mode %d\n", mode);

        if (mode == GAME_MODE_1) {
            /* Run one round of mode 1 then loop back
             * so mode changes are picked up quickly */
            k_mutex_lock(&state_mutex, K_FOREVER);
            bool    r_running = game_running;
            uint8_t r = game_r, g = game_g, b = game_b;
            k_mutex_unlock(&state_mutex);

            if (!r_running) continue;

            k_mutex_lock(&state_mutex, K_FOREVER);
            round_ended = false;
            k_mutex_unlock(&state_mutex);

            int pod = sys_rand32_get() % 4;
            printk("Mode1: pod=%d\n", pod + 1);

            if (pod == 0) {
                rgb_led_set_all(r, g, b, 0xFF);
                k_mutex_lock(&state_mutex, K_FOREVER);
                led_active     = true;
                led_on_time_ms = k_uptime_get();
                k_mutex_unlock(&state_mutex);
                send_to_phone("POD1:ON\n");
            } else {
                char cmd[16];
                snprintf(cmd, sizeof(cmd),
                         "ON:%02X%02X%02X", r, g, b);
                write_to_pod(pod - 1, cmd);
                char notify[32];
                snprintf(notify, sizeof(notify),
                         "POD%d:ON\n", pod + 1);
                send_to_phone(notify);
            }

            k_sem_take(&round_done_sem,
                       K_MSEC(timeout_ms + 2000));

            /* Clean up */
            turn_off_all_pods();
            k_mutex_lock(&state_mutex, K_FOREVER);
            led_active = false;
            k_mutex_unlock(&state_mutex);

            k_mutex_lock(&state_mutex, K_FOREVER);
            bool still_running = game_running;
            game_mode_t new_mode = current_mode;
            k_mutex_unlock(&state_mutex);

            if (!still_running) continue;

            /* If mode changed, loop picks it up next iteration */
            if (new_mode == GAME_MODE_1) {
                k_sleep(K_SECONDS(2));
                k_sem_give(&game_start_sem); /* keep going */
            }
            /* if mode changed to 2, button ISR already gave
             * game_start_sem so next iteration runs mode 2 */

        } else {
            /* Run one round of mode 2 */
            k_mutex_lock(&state_mutex, K_FOREVER);
            bool     r_running = game_running;
            uint32_t t_out     = timeout_ms;
            k_mutex_unlock(&state_mutex);

            if (!r_running) continue;

            int maj_idx = sys_rand32_get() % NUM_COLOURS;
            int odd_idx = random_colour_excluding(maj_idx);
            int odd_pod = sys_rand32_get() % 4;

            printk("Mode2: maj=%s odd=%s odd_pod=%d\n",
                   colours[maj_idx].name,
                   colours[odd_idx].name,
                   odd_pod + 1);

            k_mutex_lock(&state_mutex, K_FOREVER);
            odd_pod_index = odd_pod;
            round_ended   = false;
            k_mutex_unlock(&state_mutex);

            int64_t round_start = k_uptime_get();

            for (int i = 0; i < 4; i++) {
                bool    is_odd = (i == odd_pod);
                uint8_t r = is_odd ? colours[odd_idx].r
                                   : colours[maj_idx].r;
                uint8_t g = is_odd ? colours[odd_idx].g
                                   : colours[maj_idx].g;
                uint8_t b = is_odd ? colours[odd_idx].b
                                   : colours[maj_idx].b;

                if (i == 0) {
                    rgb_led_set_all(r, g, b, 0xFF);
                    k_mutex_lock(&state_mutex, K_FOREVER);
                    led_active     = true;
                    led_on_time_ms = round_start;
                    k_mutex_unlock(&state_mutex);
                } else {
                    char cmd[16];
                    snprintf(cmd, sizeof(cmd),
                             "ON:%02X%02X%02X", r, g, b);
                    write_to_pod(i - 1, cmd);
                }
            }

            send_to_phone("MODE2:ALL_ON\n");

            k_sem_take(&round_done_sem,
                       K_MSEC(t_out + 2000));

            turn_off_all_pods();
            k_mutex_lock(&state_mutex, K_FOREVER);
            led_active = false;
            k_mutex_unlock(&state_mutex);

            k_mutex_lock(&state_mutex, K_FOREVER);
            bool still_running   = game_running;
            game_mode_t new_mode = current_mode;
            k_mutex_unlock(&state_mutex);

            if (!still_running) continue;

            if (new_mode == GAME_MODE_2) {
                k_sleep(K_SECONDS(2));
                k_sem_give(&game_start_sem); /* keep going */
            }
            /* if mode changed to 1, button ISR already gave
             * game_start_sem so next iteration runs mode 1 */
        }
    }
}
/* ══════════════════════════════════════════════════════════
 * main()
 * ══════════════════════════════════════════════════════════ */
int main(void)
{
    int err;

    printk("Blazepod1 starting...\n");
    

/* ── GPIO mode button init ── */
if (!gpio_is_ready_dt(&mode_btn)) {
    printk("Mode button GPIO not ready\n");
    return 0;
}

int btn_err = gpio_pin_configure_dt(&mode_btn, GPIO_INPUT);
if (btn_err) {
    printk("Button configure failed err=%d\n", btn_err);
    return 0;
}

btn_err = gpio_pin_interrupt_configure_dt(&mode_btn,
                                          GPIO_INT_EDGE_TO_ACTIVE);
if (btn_err) {
    printk("Button interrupt configure failed err=%d\n", btn_err);
    return 0;
}

gpio_init_callback(&button_cb_data, button_pressed,
                   BIT(mode_btn.pin));
gpio_add_callback(mode_btn.port, &button_cb_data);
printk("Mode button ready pin=%d\n", mode_btn.pin);

    if (rgb_led_init() != 0) {
        printk("LED init failed\n");
        return 0;
    }
    rgb_led_off();

    if (ir_sensor_init() != 0) {
        printk("IR init failed\n");
        return 0;
    }

    memset(pods, 0, sizeof(pods));

    /* IR thread */
    k_thread_create(&ir_thread_data, ir_stack,
                    K_THREAD_STACK_SIZEOF(ir_stack),
                    ir_thread_fn, NULL, NULL, NULL,
                    IR_THREAD_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&ir_thread_data, "ir_pod1");

    /* Game thread */
    k_thread_create(&game_thread_data, game_stack,
                    K_THREAD_STACK_SIZEOF(game_stack),
                    game_thread_fn, NULL, NULL, NULL,
                    GAME_THREAD_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&game_thread_data, "game");

    bt_conn_cb_register(&conn_callbacks);

    err = bt_nus_cb_register(&nus_listener, NULL);
    if (err) return err;

    err = bt_enable(NULL);
    if (err) return err;

    /* Advertise to phone */
    bt_le_adv_stop();
    bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad),
                    sd, ARRAY_SIZE(sd));
    printk("Advertising as Blazepod1\n");

    /* Scan for ESP32 pods */
    start_scan();

    /* Main loop — restarts advertising after phone disconnects */
    while (true) {
        k_sem_take(&adv_sem, K_FOREVER);
        bt_le_adv_stop();
        bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad),
                        sd, ARRAY_SIZE(sd));
        printk("Advertising restarted\n");
    }

    return 0;
}