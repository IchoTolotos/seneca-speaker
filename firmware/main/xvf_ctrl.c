#include "xvf_ctrl.h"

#include <string.h>
#include "board_xvf3800.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "xvf_ctrl";
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static SemaphoreHandle_t s_lock;

esp_err_t gls_xvf_init(void)
{
    if (s_dev) return ESP_OK;
    i2c_master_bus_config_t bc = {
        .i2c_port = GLS_I2C_PORT,
        .sda_io_num = GLS_I2C_SDA_GPIO,
        .scl_io_num = GLS_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bc, &s_bus);
    if (err != ESP_OK) return err;
    i2c_device_config_t dc = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = GLS_XVF3800_I2C_ADDR, .scl_speed_hz = GLS_I2C_FREQ_HZ};
    err = i2c_master_bus_add_device(s_bus, &dc, &s_dev);
    if (err != ESP_OK) return err;
    s_lock = xSemaphoreCreateMutex();
    uint8_t ver[3] = {0};
    if (gls_xvf_read(48, 0, ver, 3) == ESP_OK) ESP_LOGI(TAG, "XVF3800 firmware %u.%u.%u", ver[0], ver[1], ver[2]);
    return ESP_OK;
}

esp_err_t gls_xvf_write(uint8_t resid, uint8_t cmd, const uint8_t *data, size_t n)
{
    if (!s_dev || n > 60) return ESP_ERR_INVALID_STATE;
    uint8_t buf[64] = {resid, cmd, (uint8_t)n};
    memcpy(buf + 3, data, n);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = i2c_master_transmit(s_dev, buf, n + 3, 100);
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t gls_xvf_read(uint8_t resid, uint8_t cmd, uint8_t *data, size_t n)
{
    if (!s_dev || n > 60) return ESP_ERR_INVALID_STATE;
    uint8_t req[3] = {resid, (uint8_t)(cmd | 0x80), (uint8_t)(n + 1)}, rsp[64] = {0};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = i2c_master_transmit(s_dev, req, 3, 100);
    if (err == ESP_OK) err = i2c_master_receive(s_dev, rsp, n + 1, 100);
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) return err;
    if (rsp[0] != 0) return ESP_FAIL;  // status byte
    memcpy(data, rsp + 1, n);
    return ESP_OK;
}

esp_err_t gls_xvf_amp_enable(bool on)
{
    uint8_t d[2] = {31, (uint8_t)(on ? 0 : 1)};
    esp_err_t err = gls_xvf_write(20, 1, d, 2);
    ESP_LOGI(TAG, "speaker amp %s (%s)", on ? "on" : "off", esp_err_to_name(err));
    return err;
}

esp_err_t gls_xvf_set_ref_delay(int32_t samples)
{
    uint8_t d[4];
    memcpy(d, &samples, 4);  // little endian
    return gls_xvf_write(35, 26, d, 4);
}

esp_err_t gls_xvf_ref_delay(int32_t *samples)
{
    uint8_t d[4];
    esp_err_t err = gls_xvf_read(35, 26, d, 4);
    if (err == ESP_OK) memcpy(samples, d, 4);
    return err;
}
