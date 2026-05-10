#include "app_display.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "app_audio_controller.h"
#include "app_config.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define APP_DISPLAY_STACK_SIZE 2048
#define APP_DISPLAY_PET_SIZE 64
#define APP_DISPLAY_PET_SCALE 4
#define APP_DISPLAY_PET_GRID 16
#define APP_DISPLAY_TEXT_SCALE 1
#define APP_DISPLAY_TEXT_CHAR_W 6
#define APP_DISPLAY_TEXT_CHAR_H 8
#define APP_DISPLAY_MAX_TRANSFER_BYTES 64
#define APP_DISPLAY_CHUNK_PIXELS (APP_DISPLAY_MAX_TRANSFER_BYTES / sizeof(uint16_t))

static const char *TAG = "app_display";

#if APP_DISPLAY_ENABLE
static spi_device_handle_t s_lcd;

static const uint8_t s_font5x7[96][5] = {
    [' ' - 32] = { 0x00, 0x00, 0x00, 0x00, 0x00 },
    ['a' - 32] = { 0x20, 0x54, 0x54, 0x54, 0x78 },
    ['e' - 32] = { 0x38, 0x54, 0x54, 0x54, 0x18 },
    ['g' - 32] = { 0x08, 0x54, 0x54, 0x54, 0x3C },
    ['k' - 32] = { 0x7F, 0x10, 0x28, 0x44, 0x00 },
    ['n' - 32] = { 0x7C, 0x08, 0x04, 0x04, 0x78 },
    ['o' - 32] = { 0x38, 0x44, 0x44, 0x44, 0x38 },
    ['r' - 32] = { 0x7C, 0x08, 0x04, 0x04, 0x08 },
    ['s' - 32] = { 0x48, 0x54, 0x54, 0x54, 0x20 },
    ['t' - 32] = { 0x04, 0x3F, 0x44, 0x40, 0x20 },
    ['P' - 32] = { 0x7F, 0x09, 0x09, 0x09, 0x06 },
};

static const char *const s_pet_frames[] = {
    "................"
    "................"
    "....2....2......"
    "...222..222....."
    "..2222222222...."
    "..22W2222W22...."
    "..2222P22222...."
    ".222222222222..."
    ".2222R22R2222..."
    "..2222222222...."
    "...2.2222.2....."
    "..2...22...2...."
    ".2..........2..."
    "................"
    "................"
    "................",
    "................"
    "................"
    "...2......2....."
    "..222....222...."
    "..2222222222...."
    "..22W2222W22...."
    "..2222P22222...."
    ".222222222222..."
    ".2222R22R2222..."
    "..2222222222...."
    "..2..2222..2...."
    ".2....22....2..."
    "2............2.."
    "................"
    "................"
    "................",
    "................"
    "................"
    "......2....2...."
    ".....222..222..."
    "....2222222222.."
    "....22W2222W22.."
    "....2222P22222.."
    "...222222222222."
    "...2222R22R2222."
    "....2222222222.."
    ".....2.2222.2..."
    "....2...22...2.."
    "...2..........2."
    "................"
    "................"
    "................",
    "................"
    "................"
    ".....2....2....."
    "....222..222...."
    "...2222222222..."
    "...22W2222W22..."
    "...2222P22222..."
    "..222222222222.."
    "..2222R22R2222.."
    "...2222222222..."
    "..2...2222...2.."
    "...2...22...2..."
    "....2......2...."
    "................"
    "................"
    "................",
};

static uint16_t app_display_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((uint16_t)(r & 0xF8) << 8) |
                      ((uint16_t)(g & 0xFC) << 3) |
                      ((uint16_t)b >> 3));
}

static esp_err_t app_display_spi_write(const void *data, size_t len)
{
    const uint8_t *cursor = (const uint8_t *)data;

    while (len > 0) {
        size_t chunk = len > APP_DISPLAY_MAX_TRANSFER_BYTES ? APP_DISPLAY_MAX_TRANSFER_BYTES : len;
        spi_transaction_t tx = {
            .length = chunk * 8U,
            .tx_buffer = cursor,
        };
        esp_err_t err = spi_device_polling_transmit(s_lcd, &tx);
        if (err != ESP_OK) {
            return err;
        }
        cursor += chunk;
        len -= chunk;
    }
    return ESP_OK;
}

static esp_err_t app_display_cmd(uint8_t cmd)
{
    gpio_set_level(APP_DISPLAY_DC_GPIO, 0);
    return app_display_spi_write(&cmd, 1);
}

static esp_err_t app_display_data(const void *data, size_t len)
{
    gpio_set_level(APP_DISPLAY_DC_GPIO, 1);
    return app_display_spi_write(data, len);
}

static esp_err_t app_display_data_u8(uint8_t value)
{
    return app_display_data(&value, 1);
}

static esp_err_t app_display_set_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    uint16_t x0 = (uint16_t)(x + APP_DISPLAY_COL_OFFSET);
    uint16_t y0 = (uint16_t)(y + APP_DISPLAY_ROW_OFFSET);
    uint16_t x1 = (uint16_t)(x0 + w - 1U);
    uint16_t y1 = (uint16_t)(y0 + h - 1U);
    uint8_t data[4];

    ESP_RETURN_ON_ERROR(app_display_cmd(0x2A), TAG, "LCD CASET failed");
    data[0] = (uint8_t)(x0 >> 8);
    data[1] = (uint8_t)x0;
    data[2] = (uint8_t)(x1 >> 8);
    data[3] = (uint8_t)x1;
    ESP_RETURN_ON_ERROR(app_display_data(data, sizeof(data)), TAG, "LCD CASET data failed");

    ESP_RETURN_ON_ERROR(app_display_cmd(0x2B), TAG, "LCD RASET failed");
    data[0] = (uint8_t)(y0 >> 8);
    data[1] = (uint8_t)y0;
    data[2] = (uint8_t)(y1 >> 8);
    data[3] = (uint8_t)y1;
    ESP_RETURN_ON_ERROR(app_display_data(data, sizeof(data)), TAG, "LCD RASET data failed");

    return app_display_cmd(0x2C);
}

static esp_err_t app_display_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    uint16_t chunk[APP_DISPLAY_CHUNK_PIXELS];
    uint16_t be_color = __builtin_bswap16(color);

    if (x >= APP_DISPLAY_WIDTH || y >= APP_DISPLAY_HEIGHT) {
        return ESP_OK;
    }
    if ((uint32_t)x + w > APP_DISPLAY_WIDTH) {
        w = (uint16_t)(APP_DISPLAY_WIDTH - x);
    }
    if ((uint32_t)y + h > APP_DISPLAY_HEIGHT) {
        h = (uint16_t)(APP_DISPLAY_HEIGHT - y);
    }

    for (uint16_t i = 0; i < APP_DISPLAY_CHUNK_PIXELS; i++) {
        chunk[i] = be_color;
    }
    ESP_RETURN_ON_ERROR(app_display_set_window(x, y, w, h), TAG, "LCD window failed");
    gpio_set_level(APP_DISPLAY_DC_GPIO, 1);
    uint32_t remaining = (uint32_t)w * h;
    while (remaining > 0) {
        uint32_t pixels = remaining > APP_DISPLAY_CHUNK_PIXELS ? APP_DISPLAY_CHUNK_PIXELS : remaining;
        ESP_RETURN_ON_ERROR(app_display_spi_write(chunk, (size_t)pixels * sizeof(chunk[0])),
                            TAG, "LCD fill failed");
        remaining -= pixels;
    }
    return ESP_OK;
}

static esp_err_t app_display_write_pixels(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t *pixels)
{
    ESP_RETURN_ON_ERROR(app_display_set_window(x, y, w, h), TAG, "LCD pixel window failed");
    gpio_set_level(APP_DISPLAY_DC_GPIO, 1);
    return app_display_spi_write(pixels, (size_t)w * h * sizeof(pixels[0]));
}

static uint16_t app_display_pet_color(char px)
{
    switch (px) {
    case '2':
        return app_display_rgb565(98, 210, 255);
    case 'W':
        return app_display_rgb565(255, 255, 255);
    case 'P':
        return app_display_rgb565(255, 118, 174);
    case 'R':
        return app_display_rgb565(255, 92, 110);
    default:
        return app_display_rgb565(12, 16, 24);
    }
}

static esp_err_t app_display_draw_pet_frame(size_t frame, bool active)
{
    const char *pixels = s_pet_frames[frame % (sizeof(s_pet_frames) / sizeof(s_pet_frames[0]))];
    const uint16_t base_x = (uint16_t)((APP_DISPLAY_WIDTH - APP_DISPLAY_PET_SIZE) / 2U);
    const uint16_t base_y = 22;
    const uint16_t bg = app_display_rgb565(12, 16, 24);
    uint16_t line[APP_DISPLAY_PET_SIZE];

    for (uint16_t py = 0; py < APP_DISPLAY_PET_GRID; py++) {
        for (uint16_t sx_row = 0; sx_row < APP_DISPLAY_PET_SCALE; sx_row++) {
            size_t out = 0;
            for (uint16_t px = 0; px < APP_DISPLAY_PET_GRID; px++) {
                char ch = pixels[(py * APP_DISPLAY_PET_GRID) + px];
                uint16_t color = ch == '.'
                    ? bg
                    : (active ? app_display_pet_color(ch) : app_display_rgb565(95, 118, 136));
                uint16_t be_color = __builtin_bswap16(color);
                for (uint16_t sx = 0; sx < APP_DISPLAY_PET_SCALE; sx++) {
                    line[out++] = be_color;
                }
            }
            ESP_RETURN_ON_ERROR(app_display_write_pixels(base_x,
                                                         (uint16_t)(base_y + (py * APP_DISPLAY_PET_SCALE) + sx_row),
                                                         APP_DISPLAY_PET_SIZE,
                                                         1,
                                                         line),
                                TAG, "LCD pet row draw failed");
        }
    }
    return ESP_OK;
}

static esp_err_t app_display_draw_char(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg)
{
    uint8_t index = (c >= 32 && c <= 127) ? (uint8_t)(c - 32) : 0;

    for (uint8_t col = 0; col < 6; col++) {
        uint8_t bits = col < 5 ? s_font5x7[index][col] : 0;
        for (uint8_t row = 0; row < 8; row++) {
            bool on = row < 7 && ((bits >> row) & 0x01U);
            ESP_RETURN_ON_ERROR(app_display_fill_rect((uint16_t)(x + col),
                                                      (uint16_t)(y + row),
                                                      APP_DISPLAY_TEXT_SCALE,
                                                      APP_DISPLAY_TEXT_SCALE,
                                                      on ? fg : bg),
                                TAG, "LCD char draw failed");
        }
    }
    return ESP_OK;
}

static esp_err_t app_display_draw_text(uint16_t x, uint16_t y, const char *text, uint16_t fg, uint16_t bg)
{
    while (text != NULL && *text != '\0') {
        ESP_RETURN_ON_ERROR(app_display_draw_char(x, y, *text, fg, bg), TAG, "LCD text draw failed");
        x = (uint16_t)(x + APP_DISPLAY_TEXT_CHAR_W);
        text++;
    }
    return ESP_OK;
}

static esp_err_t app_display_draw_footer(void)
{
    const char *text = APP_DISPLAY_STATUS_TEXT;
    size_t len = strlen(text);
    uint16_t w = (uint16_t)(len * APP_DISPLAY_TEXT_CHAR_W);
    uint16_t x = w + 2U < APP_DISPLAY_WIDTH ? (uint16_t)(APP_DISPLAY_WIDTH - w - 2U) : 0;
    uint16_t y = (uint16_t)(APP_DISPLAY_HEIGHT - APP_DISPLAY_TEXT_CHAR_H - 2U);

    ESP_RETURN_ON_ERROR(app_display_fill_rect(0, y, APP_DISPLAY_WIDTH,
                                              (uint16_t)(APP_DISPLAY_TEXT_CHAR_H + 2U),
                                              app_display_rgb565(12, 16, 24)),
                        TAG, "LCD footer clear failed");
    return app_display_draw_text(x, y, text,
                                 app_display_rgb565(201, 218, 232),
                                 app_display_rgb565(12, 16, 24));
}

static esp_err_t app_display_init_panel(void)
{
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = APP_DISPLAY_MOSI_GPIO,
        .miso_io_num = GPIO_NUM_NC,
        .sclk_io_num = APP_DISPLAY_SCLK_GPIO,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = APP_DISPLAY_MAX_TRANSFER_BYTES,
    };
    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = APP_DISPLAY_SPI_CLOCK_HZ,
        .mode = 0,
        .spics_io_num = APP_DISPLAY_CS_GPIO,
        .queue_size = 1,
    };
    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << APP_DISPLAY_DC_GPIO) |
                        (1ULL << APP_DISPLAY_RST_GPIO) |
                        (1ULL << APP_DISPLAY_BL_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_RETURN_ON_ERROR(gpio_config(&out_cfg), TAG, "LCD GPIO config failed");
    gpio_set_level(APP_DISPLAY_BL_GPIO, 0);
    gpio_set_level(APP_DISPLAY_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(APP_DISPLAY_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    ESP_RETURN_ON_ERROR(spi_bus_initialize(APP_DISPLAY_SPI_HOST, &bus_cfg, SPI_DMA_DISABLED),
                        TAG, "LCD SPI bus init failed");
    ESP_RETURN_ON_ERROR(spi_bus_add_device(APP_DISPLAY_SPI_HOST, &dev_cfg, &s_lcd),
                        TAG, "LCD SPI device init failed");

    ESP_RETURN_ON_ERROR(app_display_cmd(0x01), TAG, "LCD SWRESET failed");
    vTaskDelay(pdMS_TO_TICKS(150));
    ESP_RETURN_ON_ERROR(app_display_cmd(0x11), TAG, "LCD SLPOUT failed");
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_RETURN_ON_ERROR(app_display_cmd(0x3A), TAG, "LCD COLMOD failed");
    ESP_RETURN_ON_ERROR(app_display_data_u8(0x55), TAG, "LCD COLMOD data failed");
    ESP_RETURN_ON_ERROR(app_display_cmd(0x36), TAG, "LCD MADCTL failed");
    ESP_RETURN_ON_ERROR(app_display_data_u8(APP_DISPLAY_MADCTL), TAG, "LCD MADCTL data failed");
    ESP_RETURN_ON_ERROR(app_display_cmd(0x21), TAG, "LCD INVON failed");
    ESP_RETURN_ON_ERROR(app_display_cmd(0x13), TAG, "LCD NORON failed");
    ESP_RETURN_ON_ERROR(app_display_cmd(0x29), TAG, "LCD DISPON failed");
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(APP_DISPLAY_BL_GPIO, 1);
    return ESP_OK;
}

static void app_display_task(void *arg)
{
    size_t frame = 0;
    app_audio_mode_t last_mode = APP_AUDIO_MODE_IDLE;
    bool last_active = false;
    esp_err_t err;

    (void)arg;

    if (app_display_init_panel() != ESP_OK) {
        ESP_LOGE(TAG, "Display init failed; UI disabled");
        vTaskDelete(NULL);
        return;
    }

    err = app_display_fill_rect(0, 0, APP_DISPLAY_WIDTH, APP_DISPLAY_HEIGHT,
                                app_display_rgb565(12, 16, 24));
    if (err == ESP_OK) {
        err = app_display_draw_pet_frame(0, false);
    }
    if (err == ESP_OK) {
        err = app_display_draw_footer();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Display first draw failed: %s; UI disabled", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Cardputer display UI started stack_hwm=%u",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));

    while (true) {
        app_audio_mode_t mode = app_audio_controller_get_mode();
        bool active = mode == APP_AUDIO_MODE_PLAYBACK;

        if (active) {
            frame++;
            err = app_display_draw_pet_frame(frame, true);
        } else if (mode != last_mode || active != last_active) {
            frame = 0;
            err = app_display_draw_pet_frame(0, false);
        } else {
            err = ESP_OK;
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Display draw failed: %s; UI disabled", esp_err_to_name(err));
            vTaskDelete(NULL);
            return;
        }
        last_mode = mode;
        last_active = active;
        vTaskDelay(pdMS_TO_TICKS(active ? 160 : 250));
    }
}
#endif

esp_err_t app_display_start(void)
{
#if APP_DISPLAY_ENABLE
    BaseType_t ok = xTaskCreate(app_display_task, "app_display", APP_DISPLAY_STACK_SIZE,
                                NULL, tskIDLE_PRIORITY + 1, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
#else
    return ESP_OK;
#endif
}
