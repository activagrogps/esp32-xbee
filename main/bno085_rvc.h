#ifndef BNO085_RVC_H
#define BNO085_RVC_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/uart.h"

/* ---- Configurare hardware ---------------------------------------------
 *
 * ATENTIE: portul UART al UM980 NU este fix in cod. Este configurabil din
 * interfata web (Config -> UART) si salvat in NVS, implicit UART_NUM_0.
 * Codul verifica la pornire ca IMU-ul nu foloseste acelasi port si refuza
 * sa porneasca daca se suprapun.
 *
 * In modul UART-RVC senzorul doar emite, nu accepta comenzi, deci TX-ul
 * S3-ului nu se conecteaza. Un singur fir de semnal.
 */
#define BNO085_UART_PORT   UART_NUM_1
#define BNO085_RX_PIN      16
#define BNO085_BAUD        115200   /* fix, impus de datasheet */

typedef struct {
    float    yaw;        /* grade; integrat din giroscop -> DERIVEAZA in timp */
    float    pitch;      /* grade; referentiat gravitational, nu deriveaza */
    float    roll;       /* grade; referentiat gravitational, nu deriveaza */
    float    ax, ay, az; /* m/s^2 */
    uint8_t  index;      /* contorul din cadru */
    int64_t  timestamp;  /* esp_timer_get_time(), microsecunde */
} bno085_rvc_data_t;

/* Porneste UART-ul si taskul de citire. Se apeleaza o singura data,
 * dupa config_init() (are nevoie de configuratia UART din NVS). */
esp_err_t bno085_rvc_start(void);

/* Copiaza ultimul cadru valid. False daca nu a sosit inca niciunul. */
bool bno085_rvc_get(bno085_rvc_data_t *out);

/* True daca a sosit un cadru in ultimele max_age_ms milisecunde. */
bool bno085_rvc_is_alive(uint32_t max_age_ms);

/* Offsetul de montaj in grade, scazut din valorile brute.
 * Se pierde la repornire. */
void bno085_rvc_set_offset(float roll_offset, float pitch_offset);

uint32_t bno085_rvc_frames_ok(void);
uint32_t bno085_rvc_frames_bad(void);

/* Formateaza "$PIMU,roll,pitch,timestamp*CS\r\n".
 * Returneaza octetii scrisi, 0 daca nu exista date valide. */
int bno085_rvc_format_pimu(char *buf, size_t buf_len);

#endif /* BNO085_RVC_H */
