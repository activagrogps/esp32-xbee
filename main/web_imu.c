#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"

#include "web_imu.h"
#include "bno085_rvc.h"

static const char *TAG = "web_imu";

/* --- Log in RAM, nu in flash -----------------------------------------
 * La 10 Hz, scrisul in flash ar uza partitia degeaba, iar SPIFFS-ul de
 * la 0x210000 e al interfetei web.
 * 3000 esantioane x 10 octeti = 30 KB = 5 minute la 10 Hz.
 */
#define LOG_CAPACITY   3000
#define LOG_RATE_HZ    10

typedef struct {
    uint32_t t_ms;
    int16_t  roll_cd;    /* centigrade */
    int16_t  pitch_cd;
    int16_t  yaw_cd;
} log_sample_t;

static log_sample_t     *s_log       = NULL;
static uint16_t          s_log_head  = 0;
static uint16_t          s_log_count = 0;
static bool              s_logging   = false;
static SemaphoreHandle_t s_log_mtx   = NULL;

static void log_push(const bno085_rvc_data_t *d)
{
    if (s_log == NULL) return;

    log_sample_t s = {
        .t_ms     = (uint32_t)(d->timestamp / 1000),
        .roll_cd  = (int16_t)(d->roll  * 100.0f),
        .pitch_cd = (int16_t)(d->pitch * 100.0f),
        .yaw_cd   = (int16_t)(d->yaw   * 100.0f),
    };

    xSemaphoreTake(s_log_mtx, portMAX_DELAY);
    s_log[s_log_head] = s;
    s_log_head = (s_log_head + 1) % LOG_CAPACITY;
    if (s_log_count < LOG_CAPACITY) s_log_count++;
    xSemaphoreGive(s_log_mtx);
}

static void log_clear(void)
{
    xSemaphoreTake(s_log_mtx, portMAX_DELAY);
    s_log_head  = 0;
    s_log_count = 0;
    xSemaphoreGive(s_log_mtx);
}

static void logger_task(void *arg)
{
    const TickType_t period = pdMS_TO_TICKS(1000 / LOG_RATE_HZ);
    TickType_t last = xTaskGetTickCount();
    bno085_rvc_data_t d;

    while (1) {
        vTaskDelayUntil(&last, period);
        if (!s_logging) continue;
        if (bno085_rvc_get(&d)) log_push(&d);
    }
}

/* --- /imu.json -------------------------------------------------------
 * Controlul logului trece tot pe aici, prin query string, ca sa nu
 * consume sloturi suplimentare de handler:
 *   /imu.json?log=start | stop | clear
 *   /imu.json?zero=1
 */
static esp_err_t imu_json_handler(httpd_req_t *req)
{
    char query[64];
    char val[16];

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "log", val, sizeof(val)) == ESP_OK) {
            if      (strcmp(val, "start") == 0) s_logging = true;
            else if (strcmp(val, "stop")  == 0) s_logging = false;
            else if (strcmp(val, "clear") == 0) { s_logging = false; log_clear(); }
        }
        if (httpd_query_key_value(query, "zero", val, sizeof(val)) == ESP_OK) {
            bno085_rvc_data_t d;
            if (bno085_rvc_get(&d)) bno085_rvc_set_offset(d.roll, d.pitch);
        }
    }

    cJSON *root = cJSON_CreateObject();
    bno085_rvc_data_t d;
    bool have = bno085_rvc_get(&d);

    cJSON_AddBoolToObject(root, "alive", bno085_rvc_is_alive(500));
    if (have) {
        cJSON_AddNumberToObject(root, "roll",  d.roll);
        cJSON_AddNumberToObject(root, "pitch", d.pitch);
        cJSON_AddNumberToObject(root, "yaw",   d.yaw);
        cJSON_AddNumberToObject(root, "ax",    d.ax);
        cJSON_AddNumberToObject(root, "ay",    d.ay);
        cJSON_AddNumberToObject(root, "az",    d.az);
    }
    cJSON_AddNumberToObject(root, "ok",  bno085_rvc_frames_ok());
    cJSON_AddNumberToObject(root, "bad", bno085_rvc_frames_bad());

    cJSON *log = cJSON_AddObjectToObject(root, "log");
    cJSON_AddBoolToObject(log,   "running",  s_logging);
    cJSON_AddNumberToObject(log, "count",    s_log_count);
    cJSON_AddNumberToObject(log, "capacity", LOG_CAPACITY);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (out == NULL) return ESP_FAIL;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, out);
    free(out);
    return err;
}

/* --- /imu.csv, trimis pe bucati -------------------------------------- */
static esp_err_t imu_csv_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=imu_log.csv");
    httpd_resp_sendstr_chunk(req, "t_ms,roll,pitch,yaw\r\n");

    char line[64];
    uint16_t count, start;

    xSemaphoreTake(s_log_mtx, portMAX_DELAY);
    count = s_log_count;
    start = (s_log_head + LOG_CAPACITY - s_log_count) % LOG_CAPACITY;
    xSemaphoreGive(s_log_mtx);

    for (uint16_t i = 0; i < count; i++) {
        log_sample_t s;
        xSemaphoreTake(s_log_mtx, portMAX_DELAY);
        s = s_log[(start + i) % LOG_CAPACITY];
        xSemaphoreGive(s_log_mtx);

        int n = snprintf(line, sizeof(line), "%u,%.2f,%.2f,%.2f\r\n",
                         (unsigned) s.t_ms,
                         s.roll_cd / 100.0f,
                         s.pitch_cd / 100.0f,
                         s.yaw_cd / 100.0f);
        if (n > 0) httpd_resp_send_chunk(req, line, n);
    }

    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* --- /imu ------------------------------------------------------------
 * Pagina e generata din C, nu din SPIFFS, ca sa nu fie nevoie de
 * reconstruit si reflashat www.bin la 0x210000 la fiecare modificare.
 */
static const char PAGE_HTML[] =
"<!DOCTYPE html><html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>IMU</title><style>"
"body{background:#111;color:#eee;font:14px system-ui,sans-serif;margin:0;padding:16px}"
"h1{font-size:16px;font-weight:600;margin:0 0 14px}"
".g{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;margin-bottom:14px}"
".c{background:#1c1c1c;border-radius:6px;padding:12px}"
".c .l{font-size:11px;color:#888;text-transform:uppercase;letter-spacing:.5px}"
".c .v{font-size:26px;font-variant-numeric:tabular-nums;margin-top:4px}"
"#st{padding:6px 10px;border-radius:4px;display:inline-block;margin-bottom:12px}"
".up{background:#0a3a1a;color:#4ade80}.dn{background:#3a0a0a;color:#f87171}"
"button,a.b{background:#2a2a2a;color:#eee;border:1px solid #444;border-radius:4px;"
"padding:8px 14px;margin:0 6px 6px 0;cursor:pointer;font-size:13px;"
"text-decoration:none;display:inline-block}"
"button:hover,a.b:hover{background:#333}"
"#lg{margin-top:10px;color:#888;font-size:12px}"
"#bar{height:6px;background:#2a2a2a;border-radius:3px;margin-top:6px;overflow:hidden}"
"#fill{height:100%;background:#4ade80;width:0}"
"</style></head><body>"
"<h1>BNO085 &mdash; UART-RVC</h1>"
"<div id=st class=dn>fara date</div>"
"<div class=g>"
"<div class=c><div class=l>Roll</div><div class=v id=roll>&mdash;</div></div>"
"<div class=c><div class=l>Pitch</div><div class=v id=pitch>&mdash;</div></div>"
"<div class=c><div class=l>Yaw</div><div class=v id=yaw>&mdash;</div></div>"
"</div><div class=g>"
"<div class=c><div class=l>Cadre OK</div><div class=v id=ok>0</div></div>"
"<div class=c><div class=l>Cadre gresite</div><div class=v id=bad>0</div></div>"
"<div class=c><div class=l>Rata</div><div class=v id=hz>&mdash;</div></div>"
"</div>"
"<button onclick=\"q('zero=1')\">Zero (nivel)</button>"
"<button onclick=\"q('log=start')\">Start log</button>"
"<button onclick=\"q('log=stop')\">Stop</button>"
"<button onclick=\"q('log=clear')\">Sterge</button>"
"<a class=b href='/imu.csv'>Descarca CSV</a>"
"<div id=lg>log: oprit</div><div id=bar><div id=fill></div></div>"
"<script>"
"var po=0,pt=0;"
"function q(s){fetch('/imu.json?'+s).then(r=>r.json()).then(render)}"
"function render(d){"
" var e=document.getElementById('st');"
" if(d.alive){e.className='up';e.textContent='date valide'}"
" else{e.className='dn';e.textContent='fara date'}"
" if(d.roll!==undefined){"
"  document.getElementById('roll').textContent=d.roll.toFixed(2)+'\\u00B0';"
"  document.getElementById('pitch').textContent=d.pitch.toFixed(2)+'\\u00B0';"
"  document.getElementById('yaw').textContent=d.yaw.toFixed(2)+'\\u00B0';}"
" document.getElementById('ok').textContent=d.ok;"
" document.getElementById('bad').textContent=d.bad;"
" var t=Date.now();"
" if(pt&&t>pt){var hz=(d.ok-po)*1000/(t-pt);"
"  document.getElementById('hz').textContent=hz.toFixed(0)+' Hz';}"
" po=d.ok;pt=t;"
" document.getElementById('lg').textContent='log: '+(d.log.running?'pornit':'oprit')"
"  +' \\u2014 '+d.log.count+' / '+d.log.capacity;"
" document.getElementById('fill').style.width="
"  (100*d.log.count/d.log.capacity)+'%';}"
"setInterval(function(){fetch('/imu.json').then(r=>r.json()).then(render)},500);"
"</script></body></html>";

static esp_err_t imu_page_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PAGE_HTML, HTTPD_RESP_USE_STRLEN);
}

void web_imu_register(httpd_handle_t server)
{
    if (s_log_mtx == NULL) {
        s_log_mtx = xSemaphoreCreateMutex();
        s_log = malloc(sizeof(log_sample_t) * LOG_CAPACITY);
        if (s_log == NULL) {
            ESP_LOGW(TAG, "Heap insuficient pentru log (%d octeti); "
                          "pagina merge, logarea nu",
                     (int)(sizeof(log_sample_t) * LOG_CAPACITY));
        }
        xTaskCreate(logger_task, "imu_log", 2560, NULL, 4, NULL);
    }

    httpd_uri_t u_page = { .uri = "/imu",      .method = HTTP_GET,
                           .handler = imu_page_handler };
    httpd_uri_t u_json = { .uri = "/imu.json", .method = HTTP_GET,
                           .handler = imu_json_handler };
    httpd_uri_t u_csv  = { .uri = "/imu.csv",  .method = HTTP_GET,
                           .handler = imu_csv_handler };

    esp_err_t e1 = httpd_register_uri_handler(server, &u_page);
    esp_err_t e2 = httpd_register_uri_handler(server, &u_json);
    esp_err_t e3 = httpd_register_uri_handler(server, &u_csv);

    if (e1 != ESP_OK || e2 != ESP_OK || e3 != ESP_OK) {
        ESP_LOGE(TAG, "Inregistrare esuata (%d/%d/%d) - "
                      "mareste config.max_uri_handlers in web_server_start()",
                 e1, e2, e3);
    } else {
        ESP_LOGI(TAG, "Pagina IMU disponibila la /imu");
    }
}
