#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "bno085_rvc.h"
#include "config.h"

static const char *TAG = "bno085";

/* --- Cadrul UART-RVC, 19 octeti ---------------------------------------
 *  [0]  0xAA          header
 *  [1]  0xAA          header
 *  [2]  index         contor
 *  [3:4]   yaw    int16 LE, 0.01 grade/LSB
 *  [5:6]   pitch  int16 LE, 0.01 grade/LSB
 *  [7:8]   roll   int16 LE, 0.01 grade/LSB
 *  [9:10]  ax     int16 LE, mg
 *  [11:12] ay     int16 LE, mg
 *  [13:14] az     int16 LE, mg
 *  [15:17] rezervati
 *  [18] checksum = suma octetilor [2..17] & 0xFF
 */
#define RVC_FRAME_LEN   19
#define RVC_HEADER      0xAA
#define DEG_PER_LSB     0.01f
#define MG_TO_MS2       0.0098f

static bno085_rvc_data_t s_last;
static bool     s_have_data  = false;
static float    s_roll_off   = 0.0f;
static float    s_pitch_off  = 0.0f;
static uint32_t s_frames_ok  = 0;
static uint32_t s_frames_bad = 0;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static inline int16_t rd_i16(const uint8_t *p)
{
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static bool checksum_ok(const uint8_t *f)
{
    uint8_t sum = 0;
    for (int i = 2; i < RVC_FRAME_LEN - 1; i++) sum += f[i];
    return sum == f[RVC_FRAME_LEN - 1];
}

static void parse_frame(const uint8_t *f)
{
    bno085_rvc_data_t d;

    d.index = f[2];
    d.yaw   = rd_i16(&f[3]) * DEG_PER_LSB;
    d.pitch = rd_i16(&f[5]) * DEG_PER_LSB;
    d.roll  = rd_i16(&f[7]) * DEG_PER_LSB;
    d.ax    = rd_i16(&f[9])  * MG_TO_MS2;
    d.ay    = rd_i16(&f[11]) * MG_TO_MS2;
    d.az    = rd_i16(&f[13]) * MG_TO_MS2;
    d.timestamp = esp_timer_get_time();

    portENTER_CRITICAL(&s_mux);
    d.roll  -= s_roll_off;
    d.pitch -= s_pitch_off;
    s_last = d;
    s_have_data = true;
    s_frames_ok++;
    portEXIT_CRITICAL(&s_mux);
}

/* Resincronizeaza pe 0xAA 0xAA; nu presupune ca bufferul e aliniat. */
static void bno085_task(void *arg)
{
    uint8_t frame[RVC_FRAME_LEN];
    uint8_t b;
    int state = 0;
    int fill  = 0;

    while (1) {
        if (uart_read_bytes(BNO085_UART_PORT, &b, 1, pdMS_TO_TICKS(200)) != 1) {
            continue;
        }

        switch (state) {
        case 0:
            if (b == RVC_HEADER) state = 1;
            break;

        case 1:
            if (b == RVC_HEADER) {
                frame[0] = frame[1] = RVC_HEADER;
                fill  = 2;
                state = 2;
            } else {
                state = 0;
            }
            break;

        case 2:
            frame[fill++] = b;
            if (fill == RVC_FRAME_LEN) {
                if (checksum_ok(frame)) {
                    parse_frame(frame);
                } else {
                    portENTER_CRITICAL(&s_mux);
                    s_frames_bad++;
                    portEXIT_CRITICAL(&s_mux);
                }
                state = 0;
                fill  = 0;
            }
            break;
        }
    }
}

esp_err_t bno085_rvc_start(void)
{
    /* UM980 isi ia portul din NVS, configurabil din interfata web.
     * Daca e acelasi cu al IMU-ului, ambele ar functiona gresit. */
    uint8_t gnss_port = config_get_u8(CONF_ITEM(KEY_CONFIG_UART_NUM));
    if (gnss_port == (uint8_t) BNO085_UART_PORT) {
        ESP_LOGE(TAG, "CONFLICT: UM980 este configurat pe UART%d, acelasi cu IMU. "
                      "Schimba portul UART din interfata web (Config -> UART) "
                      "sau BNO085_UART_PORT din bno085_rvc.h", gnss_port);
        return ESP_ERR_INVALID_STATE;
    }

    uart_config_t cfg = {
        .baud_rate  = BNO085_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };

    esp_err_t err = uart_param_config(BNO085_UART_PORT, &cfg);
    if (err != ESP_OK) return err;

    /* Doar RX: in modul RVC senzorul nu accepta comenzi. */
    err = uart_set_pin(BNO085_UART_PORT,
                       UART_PIN_NO_CHANGE, BNO085_RX_PIN,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) return err;

    err = uart_driver_install(BNO085_UART_PORT, 512, 0, 0, NULL, 0);
    if (err != ESP_OK) return err;

    if (xTaskCreate(bno085_task, "bno085", 3072, NULL, 6, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "UART-RVC pornit: UART%d, RX=GPIO%d, %d baud",
             BNO085_UART_PORT, BNO085_RX_PIN, BNO085_BAUD);
    return ESP_OK;
}

bool bno085_rvc_get(bno085_rvc_data_t *out)
{
    bool have;
    portENTER_CRITICAL(&s_mux);
    have = s_have_data;
    if (have) *out = s_last;
    portEXIT_CRITICAL(&s_mux);
    return have;
}

bool bno085_rvc_is_alive(uint32_t max_age_ms)
{
    bno085_rvc_data_t d;
    if (!bno085_rvc_get(&d)) return false;
    return (esp_timer_get_time() - d.timestamp) < ((int64_t) max_age_ms * 1000);
}

void bno085_rvc_set_offset(float roll_offset, float pitch_offset)
{
    portENTER_CRITICAL(&s_mux);
    s_roll_off  = roll_offset;
    s_pitch_off = pitch_offset;
    portEXIT_CRITICAL(&s_mux);
}

uint32_t bno085_rvc_frames_ok(void)  { return s_frames_ok;  }
uint32_t bno085_rvc_frames_bad(void) { return s_frames_bad; }

int bno085_rvc_format_pimu(char *buf, size_t buf_len)
{
    bno085_rvc_data_t d;
    if (!bno085_rvc_get(&d)) return 0;

    char body[64];
    int  bl = snprintf(body, sizeof(body), "PIMU,%.2f,%.2f,%lld",
                       d.roll, d.pitch, (long long)(d.timestamp / 1000));
    if (bl <= 0 || bl >= (int) sizeof(body)) return 0;

    uint8_t cs = 0;
    for (int i = 0; i < bl; i++) cs ^= (uint8_t) body[i];

    int n = snprintf(buf, buf_len, "$%s*%02X\r\n", body, cs);
    if (n <= 0 || n >= (int) buf_len) return 0;
    return n;
}
