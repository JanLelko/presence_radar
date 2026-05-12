#include <stdio.h>
#include "driver/i2c.h"
#include "esp_log.h"

#define I2C_MASTER_SDA_IO 10    // Twoje podłączenie
#define I2C_MASTER_SCL_IO 11    // Twoje podłączenie
#define I2C_MASTER_NUM I2C_NUM_0
#define I2C_MASTER_FREQ_HZ 100000

static const char *TAG = "I2C_CHECKER";

void app_main(void) {
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    
    i2c_param_config(I2C_MASTER_NUM, &conf);
    i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);

    ESP_LOGI(TAG, "Rozpoczynam skanowanie I2C (SDA: %d, SCL: %d)...", I2C_MASTER_SDA_IO, I2C_MASTER_SCL_IO);

    while(1) {
        bool found = false;
        for (int i = 1; i < 127; i++) {
            i2c_cmd_handle_t cmd = i2c_cmd_link_create();
            i2c_master_start(cmd);
            i2c_master_write_byte(cmd, (i << 1) | I2C_MASTER_WRITE, true);
            i2c_master_stop(cmd);
            esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, 10 / portTICK_PERIOD_MS);
            i2c_cmd_link_delete(cmd);

            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "ZNALAZŁEM URZĄDZENIE! Adres: 0x%02x", i);
                found = true;
            }
        }
        if (!found) ESP_LOGW(TAG, "Nic nie widzę na magistrali...");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
