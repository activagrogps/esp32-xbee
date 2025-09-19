#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"

void app_main()
{
    ESP_LOGI("TEST", "Basic app_main found");
    while(1) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}
