#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "led_strip.h"
#include "driver/gpio.h"
#include "esp_random.h"
#include "driver/i2c_master.h"
#include "rom/ets_sys.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <stdio.h>
#include <string.h>

#define SPEED_LIMIT 4

// Hardware Config
#define LED_STRIP_GPIO       3
#define NUM_LEDS             50
#define LED_STRIP_RMT_RES    (10 * 1000 * 1000)

// 3 Buttons mapped directly to R, G, B channels
#define BUTTON_RED           0  
#define BUTTON_GREEN         1  
#define BUTTON_BLUE          2  
#define BUTTON_PIN_MASK      ((1ULL << BUTTON_RED) | (1ULL << BUTTON_GREEN) | (1ULL << BUTTON_BLUE))

// static const char *TAG = "rgb_button_invaders";
static led_strip_handle_t led_strip;

// Color Archetypes
typedef enum {
    COLOR_RED = 0,
    COLOR_GREEN,
    COLOR_BLUE,
    COLOR_MAX
} rgb_color_t;

// Game State Variables
static int invader_pos = NUM_LEDS - 1;
static int invader_tick_counter = 0;
static int invader_speed_ticks = 20;

// Laser variables
static int laser_pos = -1; 
static rgb_color_t laser_color = COLOR_RED;

static rgb_color_t invader_color = COLOR_RED;
static rgb_color_t invader_led_colors[3];
static bool invader_led_alive[3];
static int invader_lives = 0;
static int score = 0;
static int max_score = 0;
static bool game_over = false;

#define I2C_MASTER_SDA_IO       8      
#define I2C_MASTER_SCL_IO       9      
#define I2C_MASTER_NUM          0      
#define LCD_ADDR                0x3C   // SSD1306 OLED display address
#define OLED_ADDR                0x3C   // SSD1306 OLED display address

// PCF8574 Pin Map
#define PIN_RS                 (1 << 0)
#define PIN_RW                 (1 << 1)
#define PIN_EN                 (1 << 2)
#define BACKLIGHT              (1 << 3)

static const char *TAG = "GJD1602_SUCCESS";

static uint8_t oled_buffer[128 * 64 / 8];
static uint8_t oled_cursor_x = 0;
static uint8_t oled_cursor_y = 0;

static void oled_delay_us(void) {
    ets_delay_us(2);
}

static void oled_i2c_init_pins(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << I2C_MASTER_SDA_IO) | (1ULL << I2C_MASTER_SCL_IO),
        .mode = GPIO_MODE_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    gpio_set_level(I2C_MASTER_SDA_IO, 1);
    gpio_set_level(I2C_MASTER_SCL_IO, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
}

static void oled_i2c_start(void) {
    gpio_set_level(I2C_MASTER_SCL_IO, 1);
    gpio_set_level(I2C_MASTER_SDA_IO, 1);
    oled_delay_us();
    gpio_set_level(I2C_MASTER_SDA_IO, 0);
    oled_delay_us();
    gpio_set_level(I2C_MASTER_SCL_IO, 0);
    oled_delay_us();
}

static void oled_i2c_stop(void) {
    gpio_set_level(I2C_MASTER_SCL_IO, 0);
    gpio_set_level(I2C_MASTER_SDA_IO, 0);
    oled_delay_us();
    gpio_set_level(I2C_MASTER_SCL_IO, 1);
    oled_delay_us();
    gpio_set_level(I2C_MASTER_SDA_IO, 1);
    oled_delay_us();
}

static bool oled_i2c_write_byte(uint8_t byte) {
    for (int bit = 7; bit >= 0; --bit) {
        gpio_set_level(I2C_MASTER_SCL_IO, 0);
        oled_delay_us();
        gpio_set_level(I2C_MASTER_SDA_IO, (byte >> bit) & 0x01);
        oled_delay_us();
        gpio_set_level(I2C_MASTER_SCL_IO, 1);
        oled_delay_us();
    }

    gpio_set_level(I2C_MASTER_SCL_IO, 0);
    oled_delay_us();
    gpio_set_level(I2C_MASTER_SDA_IO, 1);
    oled_delay_us();
    gpio_set_level(I2C_MASTER_SCL_IO, 1);
    oled_delay_us();
    bool ack = (gpio_get_level(I2C_MASTER_SDA_IO) == 0);
    gpio_set_level(I2C_MASTER_SCL_IO, 0);
    oled_delay_us();
    return ack;
}

static esp_err_t oled_i2c_write_bytes(const uint8_t *data, size_t len) {
    oled_i2c_start();
    if (!oled_i2c_write_byte((uint8_t)(OLED_ADDR << 1))) {
        oled_i2c_stop();
        return ESP_ERR_TIMEOUT;
    }

    for (size_t i = 0; i < len; ++i) {
        if (!oled_i2c_write_byte(data[i])) {
            oled_i2c_stop();
            return ESP_ERR_TIMEOUT;
        }
    }

    oled_i2c_stop();
    return ESP_OK;
}

static esp_err_t oled_write_cmd(uint8_t cmd) {
    uint8_t payload[2] = {0x00, cmd};
    return oled_i2c_write_bytes(payload, sizeof(payload));
}

static esp_err_t oled_write_data(const uint8_t *data, size_t len) {
    uint8_t payload[129];
    if (len > sizeof(payload) - 1) {
        return ESP_ERR_INVALID_SIZE;
    }

    payload[0] = 0x40;
    memcpy(&payload[1], data, len);
    return oled_i2c_write_bytes(payload, len + 1);
}

static void oled_draw_pixel(uint8_t x, uint8_t y, bool on) {
    if (x >= 128 || y >= 64) {
        return;
    }

    uint16_t index = (y / 8) * 128 + x;
    uint8_t bit = 1 << (y % 8);

    if (on) {
        oled_buffer[index] |= bit;
    } else {
        oled_buffer[index] &= (uint8_t)~bit;
    }
}

static void oled_draw_char(char ch) {
    static const uint8_t font5x7[96][5] = {
        {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x4F,0x00,0x00}, {0x00,0x07,0x00,0x07,0x00},
        {0x14,0x7F,0x14,0x7F,0x14}, {0x24,0x2A,0x7F,0x2A,0x12}, {0x23,0x13,0x08,0x64,0x62},
        {0x36,0x49,0x56,0x20,0x50}, {0x00,0x05,0x03,0x00,0x00}, {0x00,0x1C,0x22,0x41,0x00},
        {0x00,0x41,0x22,0x1C,0x00}, {0x14,0x08,0x3E,0x08,0x14}, {0x08,0x08,0x3E,0x08,0x08},
        {0x00,0x50,0x30,0x00,0x00}, {0x08,0x08,0x08,0x08,0x08}, {0x00,0x60,0x60,0x00,0x00},
        {0x20,0x10,0x08,0x04,0x02}, {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00},
        {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31}, {0x18,0x14,0x12,0x7F,0x10},
        {0x27,0x45,0x45,0x45,0x39}, {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
        {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E}, {0x00,0x36,0x36,0x00,0x00},
        {0x00,0x56,0x36,0x00,0x00}, {0x08,0x14,0x22,0x41,0x00}, {0x14,0x14,0x14,0x14,0x14},
        {0x00,0x41,0x22,0x14,0x08}, {0x02,0x01,0x51,0x09,0x06}, {0x32,0x49,0x79,0x41,0x3E},
        {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36}, {0x3E,0x41,0x41,0x41,0x22},
        {0x7F,0x41,0x41,0x22,0x1C}, {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x09,0x01},
        {0x3E,0x41,0x49,0x49,0x7A}, {0x7F,0x08,0x08,0x08,0x7F}, {0x00,0x41,0x7F,0x41,0x00},
        {0x20,0x40,0x41,0x3F,0x01}, {0x7F,0x08,0x14,0x22,0x41}, {0x7F,0x40,0x40,0x40,0x40},
        {0x7F,0x02,0x0C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F}, {0x3E,0x41,0x41,0x41,0x3E},
        {0x7F,0x09,0x09,0x09,0x06}, {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46},
        {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7F,0x01,0x01}, {0x3F,0x40,0x40,0x40,0x3F},
        {0x1F,0x20,0x40,0x20,0x1F}, {0x7F,0x20,0x18,0x20,0x7F}, {0x63,0x14,0x08,0x14,0x63},
        {0x03,0x04,0x78,0x04,0x03}, {0x61,0x51,0x49,0x45,0x43}, {0x00,0x00,0x00,0x00,0x00},
        {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00},
        {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00},
        {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00},
        {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00},
        {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00}
    };

    if (ch == '\n') {
        oled_cursor_y += 16;
        oled_cursor_x = 0;
        return;
    }

    if (ch == '\r') {
        oled_cursor_x = 0;
        return;
    }

    if (ch == ' ') {
        oled_cursor_x += 6;
        return;
    }

    if (ch < ' ' || ch > '~') {
        ch = '?';
    }

    uint8_t index = (uint8_t)(ch - ' ');
    for (uint8_t col = 0; col < 5; ++col) {
        for (uint8_t row = 0; row < 7; ++row) {
            if ((font5x7[index][col] >> row) & 0x01) {
                for (uint8_t dy = 0; dy < 2; ++dy) {
                    for (uint8_t dx = 0; dx < 2; ++dx) {
                        oled_draw_pixel(oled_cursor_x + (col * 2) + dx, oled_cursor_y + (row * 2) + dy, true);
                    }
                }
            }
        }
    }

    oled_cursor_x += 12;
}

static void oled_refresh(void) {
    for (uint8_t page = 0; page < 8; ++page) {
        esp_err_t err = oled_write_cmd(0xB0 + page);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OLED page command failed: %s", esp_err_to_name(err));
            return;
        }
        err = oled_write_cmd(0x00);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OLED column low command failed: %s", esp_err_to_name(err));
            return;
        }
        err = oled_write_cmd(0x10);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OLED column high command failed: %s", esp_err_to_name(err));
            return;
        }
        err = oled_write_data(&oled_buffer[page * 128], 128);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OLED page data write failed: %s", esp_err_to_name(err));
            return;
        }
    }
}

static void oled_clear_row(uint8_t row) {
    uint8_t start_y = (uint8_t)(row * 24);
    for (uint8_t y = start_y; y < start_y + 24 && y < 64; ++y) {
        for (uint8_t x = 0; x < 128; ++x) {
            oled_draw_pixel(x, y, false);
        }
    }
}

void lcd_send_cmd(uint8_t cmd) {
    oled_write_cmd(cmd);
}

void lcd_send_data(uint8_t data) {
    oled_draw_char((char)data);
}

void lcd_clear(void) {
    memset(oled_buffer, 0, sizeof(oled_buffer));
    oled_cursor_x = 0;
    oled_cursor_y = 0;
    oled_refresh();
}

void lcd_set_cursor(uint8_t row, uint8_t col) {
    oled_cursor_x = (uint8_t)(col * 12);
    oled_cursor_y = (uint8_t)(row * 24);
}

void lcd_send_string(const char *str);

static void oled_draw_text_line(uint8_t row, const char *str) {
    oled_clear_row(row);
    lcd_set_cursor(row, 0);
    lcd_send_string(str);
}

void lcd_send_string(const char *str) {
    while (*str) {
        oled_draw_char(*str);
        str++;
    }
    oled_refresh();
}

void lcd_init(void) {
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_err_t err;
    err = oled_write_cmd(0xAE);
    if (err != ESP_OK) { ESP_LOGE(TAG, "OLED init failed at 0xAE: %s", esp_err_to_name(err)); return; }
    err = oled_write_cmd(0xD5); if (err != ESP_OK) { ESP_LOGE(TAG, "OLED init failed at 0xD5: %s", esp_err_to_name(err)); return; }
    err = oled_write_cmd(0x80); if (err != ESP_OK) { ESP_LOGE(TAG, "OLED init failed at 0x80: %s", esp_err_to_name(err)); return; }
    err = oled_write_cmd(0xA8); if (err != ESP_OK) { ESP_LOGE(TAG, "OLED init failed at 0xA8: %s", esp_err_to_name(err)); return; }
    err = oled_write_cmd(0x3F); if (err != ESP_OK) { ESP_LOGE(TAG, "OLED init failed at 0x3F: %s", esp_err_to_name(err)); return; }
    err = oled_write_cmd(0xD3); if (err != ESP_OK) { ESP_LOGE(TAG, "OLED init failed at 0xD3: %s", esp_err_to_name(err)); return; }
    err = oled_write_cmd(0x00); if (err != ESP_OK) { ESP_LOGE(TAG, "OLED init failed at 0x00: %s", esp_err_to_name(err)); return; }
    err = oled_write_cmd(0x40); if (err != ESP_OK) { ESP_LOGE(TAG, "OLED init failed at 0x40: %s", esp_err_to_name(err)); return; }
    err = oled_write_cmd(0x8D); if (err != ESP_OK) { ESP_LOGE(TAG, "OLED init failed at 0x8D: %s", esp_err_to_name(err)); return; }
    err = oled_write_cmd(0x14); if (err != ESP_OK) { ESP_LOGE(TAG, "OLED init failed at 0x14: %s", esp_err_to_name(err)); return; }
    err = oled_write_cmd(0x20); if (err != ESP_OK) { ESP_LOGE(TAG, "OLED init failed at 0x20: %s", esp_err_to_name(err)); return; }
    err = oled_write_cmd(0x00); if (err != ESP_OK) { ESP_LOGE(TAG, "OLED init failed at 0x00-2: %s", esp_err_to_name(err)); return; }
    err = oled_write_cmd(0xA1); if (err != ESP_OK) { ESP_LOGE(TAG, "OLED init failed at 0xA1: %s", esp_err_to_name(err)); return; }
    err = oled_write_cmd(0xC8); if (err != ESP_OK) { ESP_LOGE(TAG, "OLED init failed at 0xC8: %s", esp_err_to_name(err)); return; }
    err = oled_write_cmd(0xDA); if (err != ESP_OK) { ESP_LOGE(TAG, "OLED init failed at 0xDA: %s", esp_err_to_name(err)); return; }
    err = oled_write_cmd(0x12); if (err != ESP_OK) { ESP_LOGE(TAG, "OLED init failed at 0x12: %s", esp_err_to_name(err)); return; }
    err = oled_write_cmd(0x81); if (err != ESP_OK) { ESP_LOGE(TAG, "OLED init failed at 0x81: %s", esp_err_to_name(err)); return; }
    err = oled_write_cmd(0xCF); if (err != ESP_OK) { ESP_LOGE(TAG, "OLED init failed at 0xCF: %s", esp_err_to_name(err)); return; }
    err = oled_write_cmd(0xD9); if (err != ESP_OK) { ESP_LOGE(TAG, "OLED init failed at 0xD9: %s", esp_err_to_name(err)); return; }
    err = oled_write_cmd(0xF1); if (err != ESP_OK) { ESP_LOGE(TAG, "OLED init failed at 0xF1: %s", esp_err_to_name(err)); return; }
    err = oled_write_cmd(0xDB); if (err != ESP_OK) { ESP_LOGE(TAG, "OLED init failed at 0xDB: %s", esp_err_to_name(err)); return; }
    err = oled_write_cmd(0x40); if (err != ESP_OK) { ESP_LOGE(TAG, "OLED init failed at 0x40-2: %s", esp_err_to_name(err)); return; }
    err = oled_write_cmd(0xA4); if (err != ESP_OK) { ESP_LOGE(TAG, "OLED init failed at 0xA4: %s", esp_err_to_name(err)); return; }
    err = oled_write_cmd(0xA6); if (err != ESP_OK) { ESP_LOGE(TAG, "OLED init failed at 0xA6: %s", esp_err_to_name(err)); return; }
    err = oled_write_cmd(0xAF); if (err != ESP_OK) { ESP_LOGE(TAG, "OLED init failed at 0xAF: %s", esp_err_to_name(err)); return; }

    lcd_clear();
    ESP_LOGI(TAG, "OLED Reset Sequence Completed Cleanly.");
}



// Setup Input Buttons
static void init_buttons(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = BUTTON_PIN_MASK,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
}

// Setup LED Strip (v2.x / v3.x abstraction)
static void init_led_strip(void) {
    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_STRIP_GPIO,
        .max_leds = NUM_LEDS,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };
    #if defined(LED_STRIP_COLOR_COMPONENT_FMT_GRB)
        strip_config.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    #else
        strip_config.led_pixel_format = LED_PIXEL_FORMAT_GRB;
    #endif

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = LED_STRIP_RMT_RES,
        .flags.with_dma = false,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    led_strip_clear(led_strip);
}

static rgb_color_t random_color(void) {
    return (rgb_color_t)(esp_random() % COLOR_MAX);
}

static void spawn_new_invader(void) {
    invader_pos = NUM_LEDS - 1;
    invader_lives = 3;
    invader_color = random_color();

    for (int i = 0; i < 3; ++i) {
        invader_led_alive[i] = true;
        invader_led_colors[i] = random_color();
    }
    
    // Scale speed ticks dynamically based on current performance
    invader_speed_ticks = 25 - (score * 2);
    if (invader_speed_ticks < SPEED_LIMIT) invader_speed_ticks = SPEED_LIMIT; // Speed cap limit

    ESP_LOGI(TAG, "New invader cluster spawned at %d", invader_pos);
}

static void trigger_flash(uint8_t r, uint8_t g, uint8_t b, int delay_ms) {
    for (int i = 0; i < NUM_LEDS; i++) {
        led_strip_set_pixel(led_strip, i, r, g, b);
    }
    led_strip_refresh(led_strip);
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
}

static void draw_led_group(int pos, uint8_t r, uint8_t g, uint8_t b) {
    if (pos < 0) {
        return;
    }

    for (int offset = -1; offset <= 1; ++offset) {
        int index = pos + offset;
        if (index >= 0 && index < NUM_LEDS) {
            led_strip_set_pixel(led_strip, index, r, g, b);
        }
    }
}

static void draw_invader_cluster(int pos) {
    if (pos < 0) {
        return;
    }

    for (int offset = -1; offset <= 1; ++offset) {
        int index = pos + offset;
        if (index < 0 || index >= NUM_LEDS) {
            continue;
        }

        int led_idx = offset + 1;
        if (!invader_led_alive[led_idx]) {
            continue;
        }

        switch (invader_led_colors[led_idx]) {
            case COLOR_RED:
                led_strip_set_pixel(led_strip, index, 255, 0, 0);
                break;
            case COLOR_GREEN:
                led_strip_set_pixel(led_strip, index, 0, 255, 0);
                break;
            case COLOR_BLUE:
                led_strip_set_pixel(led_strip, index, 0, 0, 255);
                break;
            default:
                break;
        }
    }
}

void update_score_display(int score) {
    char buffer[16];
    snprintf(buffer, sizeof(buffer), "Sre:%d", score);
    oled_draw_text_line(1, buffer);
}

void update_max_score_display(void) {
    char buffer[16];
    snprintf(buffer, sizeof(buffer), "Max:%d", max_score);
    oled_draw_text_line(0, buffer);
}

static void load_max_score_from_flash(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("game_data", NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No NVS namespace yet, starting with max score 0");
        max_score = 0;
        return;
    }

    int32_t stored_score = 0;
    err = nvs_get_i32(handle, "max_score", &stored_score);
    if (err == ESP_OK) {
        max_score = stored_score;
        ESP_LOGI(TAG, "Loaded max score from flash: %d", max_score);
    } else {
        max_score = 0;
        ESP_LOGW(TAG, "No stored max score found, using 0");
    }

    nvs_close(handle);
}

static void save_max_score_to_flash(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("game_data", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for max score save: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_set_i32(handle, "max_score", max_score);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save max score to flash: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Saved max score to flash: %d", max_score);
    }

    nvs_close(handle);
}

void app_main(void)
{
    init_buttons();
    init_led_strip();

    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    load_max_score_from_flash();
    spawn_new_invader();

    ESP_LOGI(TAG, "Initializing OLED display...");
    oled_i2c_init_pins();

    // Force hardware startup initialization
    lcd_init();

    // Print max score on the top line
    update_max_score_display();
    update_score_display(score);
    vTaskDelay(pdMS_TO_TICKS(1000)); // Delay to allow user to read the initial score
    
    score++; // Increment score to ensure the display updates correctly on first loop iteration
    update_max_score_display();
    update_score_display(score);
    vTaskDelay(pdMS_TO_TICKS(1000)); // Delay to allow user to read the initial score

    score++; // Increment score to ensure the display updates correctly on first loop iteration
    update_max_score_display();
    update_score_display(score);
    vTaskDelay(pdMS_TO_TICKS(1000)); // Delay to allow user to read the initial score


    // uint32_t count = 0;
    // char buffer[16]; // FIX: Changed from 'char buffer' to a 16-byte character array stack


    score = 0; // Reset score to zero at the start of the game
    update_max_score_display();
    update_score_display(score);
    vTaskDelay(pdMS_TO_TICKS(1000)); // Delay to allow user to read the initial score

    int gave_over_flash_counter = 0; // Counter to manage game over flash timing

    while (1) {
        if (game_over) {
            /*
            what happen if #sym:gave_over_flash_counter  overflow

What actually happens before any overflow matters:

gave_over_flash_counter is a plain int (32-bit signed on ESP32-C3).
For counts 0–9, each loop iteration sleeps 600 ms via the two trigger_flash() calls, so reaching 10 takes ~6 s.
From count 10 onward (until a button press resets it), neither if branch executes vTaskDelay, and the continue; on line 601 skips the loop's only other delay (line 703). So the while(1) becomes an unthrottled busy loop that just polls three GPIOs as fast as the CPU can.
Because that core's idle task never gets scheduled, ESP-IDF's Task Watchdog Timer (default ~5 s) will almost certainly fire first — you'd see Task watchdog got triggered and a panic/reboot, long before the counter gets anywhere near INT_MAX (≈2.14 billion). That's the practical failure mode a player would actually hit.
If it hypothetically did overflow (e.g., if you added a delay so the watchdog never trips): incrementing a signed int past INT_MAX is undefined behavior in C. In practice, with the GCC toolchain ESP-IDF uses (plain two's-complement arithmetic, no -ftrapv), it will silently wrap to INT_MIN (-2147483648) and keep counting up. Since the checks are < 10 and == 100:

gave_over_flash_counter < 10 becomes true again for the entire range from INT_MIN up to 9, so the red flash sequence starts replaying.
As it counts up through 100 again, the "GAME OVER!" screen gets redrawn.
So the visible symptom of a wraparound (if it ever got that far) would be the game-over flash/redraw spontaneously repeating, as if the counter had been reset — not a crash from the overflow itself. But relying on that is risky since signed overflow is technically UB and a more aggressive compiler is free to optimize the comparisons away unexpectedly.

Bottom line: the real bug here isn't really the overflow — it's the missing vTaskDelay once gave_over_flash_counter >= 10, which causes CPU starvation and a watchdog reset well before any overflow could occur.            
            */

/*
idle task 

The idle task isn't anywhere in your project — it's not something you wrote or call yourself. It's a task FreeRTOS creates automatically for you when the scheduler starts (before app_main() even runs), living inside the ESP-IDF/FreeRTOS source tree, not your sketch.

Tracing it down in your installed toolchain (/home/lincoln/.espressif/v6.0/esp-idf):

The task itself — prvIdleTask: static portTASK_FUNCTION( prvIdleTask, pvParameters ). FreeRTOS creates it internally (via xTaskCreatePinnedToCore) when vTaskStartScheduler() runs. On your ESP32-C3 (single RISC-V core), there's exactly one of these, named "IDLE0" — you'd see it listed by name if you called vTaskList() or looked at a core dump/backtrace.
Every pass of that loop calls vApplicationIdleHook(), which ESP-IDF implements as esp_vApplicationIdleHook — this is also where the CPU actually goes into a low-power wait (esp_cpu_wait_for_intr()) when nothing else needs to run.
The Task Watchdog Timer piggybacks on that hook: task_wdt.c registers an idle_hook_cb on the idle task via esp_register_freertos_idle_hook_for_cpu, and that callback calls esp_task_wdt_reset() to "feed"/pet the watchdog each time the idle task gets to run.
So the chain for the bug we discussed is: your while(1) loop (running at a higher priority than idle) never blocks once gave_over_flash_counter >= 10 → the scheduler never has a reason to switch to IDLE0 → idle_hook_cb never fires → esp_task_wdt_reset() never gets called → the watchdog's configured timeout (default 5 s) elapses with no reset → panic/reboot. That's the "idle task is starved" mechanism in concrete terms.

(Side note: I noticed my earlier explanation got pasted as a /* ... */ comment block into the file around [line 572](/home/lincoln/Arduino/esp32c3/idf/3LEDs_fixed an error/main/blink_example_main.c:572) — let me know if you'd like me to remove that comment, since it's just leftover chat text now sitting in your source.)

*/

            // Cap the counter instead of letting it grow unbounded: once past the
            // values this code checks (10/100) there is nothing left to gain by
            // continuing to increment, and capping avoids ever overflowing the int.
            if (gave_over_flash_counter < 1000) {
                gave_over_flash_counter++;
            }
            if (gave_over_flash_counter < 10) {
                trigger_flash(100, 0, 0, 300); // Game Over red pulse
                trigger_flash(0, 0, 0, 300);
            }
            if (gave_over_flash_counter == 100) {
                lcd_clear();
                lcd_set_cursor(0, 0);
                lcd_send_string("GAME OVER!");
                lcd_set_cursor(1, 0);
                char buffer[16];
                snprintf(buffer, sizeof(buffer), "Sre: %d", score);
                lcd_send_string(buffer);
            }

            // Press any of the 3 buttons to reboot the game
            if (gpio_get_level(BUTTON_RED) == 0 || gpio_get_level(BUTTON_GREEN) == 0 || gpio_get_level(BUTTON_BLUE) == 0) {
                laser_pos = -1;
                score = 0;
                gave_over_flash_counter = 0; // Reset the flash counter
                if (score > max_score) {
                    max_score = score;
                    save_max_score_to_flash();
                }
                lcd_clear();
                update_max_score_display();
                update_score_display(score);
                game_over = false;
                spawn_new_invader();
                vTaskDelay(pdMS_TO_TICKS(300));
            }
            // Always yield here: once gave_over_flash_counter >= 10 neither branch
            // above sleeps, and this path "continue"s before the frame-rate delay
            // below, so without this the loop would busy-spin and starve the idle
            // task until the watchdog reset the board.
            vTaskDelay(pdMS_TO_TICKS(25));
            continue;
        }

        // --- 1. HANDLE BUTTONS (DIRECT COLOR TRIGGERS) ---
        if (laser_pos == -1) { // Single-shot system
            if (gpio_get_level(BUTTON_RED) == 0) {
                laser_color = COLOR_RED;
                laser_pos = 1;
            } else if (gpio_get_level(BUTTON_GREEN) == 0) {
                laser_color = COLOR_GREEN;
                laser_pos = 1;
            } else if (gpio_get_level(BUTTON_BLUE) == 0) {
                laser_color = COLOR_BLUE;
                laser_pos = 1;
            }
        }

        // --- 2. ENGINE MATH UPDATE ---
        if (laser_pos != -1) {
            laser_pos += 2; // Fast laser velocity (moves 2 pixels per frame)
            if (laser_pos >= NUM_LEDS) laser_pos = -1;
        }

        invader_tick_counter++;
        if (invader_tick_counter >= invader_speed_ticks) {
            invader_tick_counter = 0;
            invader_pos--;

            if (invader_pos <= 0) {
                game_over = true;
                ESP_LOGE(TAG, "BASE INFILTRATED! Game Over. Total Score: %d", score);
            }
        }

        // --- 3. COLOR MATCH COLLISION CHECK ---
        if (laser_pos != -1) {
            bool hit_any = false;

            for (int offset = -1; offset <= 1; ++offset) {
                int inv_index = invader_pos + offset;
                if (inv_index < 0 || inv_index >= NUM_LEDS) {
                    continue;
                }

                int led_idx = offset + 1;
                if (!invader_led_alive[led_idx]) {
                    continue;
                }

                for (int laser_offset = -1; laser_offset <= 1; ++laser_offset) {
                    int laser_index = laser_pos + laser_offset;
                    if (laser_index == inv_index && laser_color == invader_led_colors[led_idx]) {
                        invader_led_alive[led_idx] = false;
                        invader_lives--;
                        score++;
                        if (score > max_score) {
                            max_score = score;
                            save_max_score_to_flash();
                        }
                        update_max_score_display();
                        update_score_display(score);
                        ESP_LOGI(TAG, "💥 LED destroyed! Score: %d", score);
                        hit_any = true;
                        break;
                    }
                }

                if (hit_any) {
                    break;
                }
            }

            if (hit_any) {
                laser_pos = -1;
                trigger_flash(100, 100, 100, 80);

                if (invader_lives <= 0) {
                    spawn_new_invader();
                }
            } else if (laser_pos != -1) {
                ESP_LOGW(TAG, "💨 WRONG ELEMENT! Laser passed right through!");
            }
        }

        // --- 4. GRAPHICS RENDERING ENGINE ---
        led_strip_clear(led_strip);

        // Draw Player Base at Index 0 (White core)
        led_strip_set_pixel(led_strip, 0, 150, 150, 150);

        // Draw Traveling Laser with matching color identity
        if (laser_pos != -1) {
            if (laser_color == COLOR_RED)   draw_led_group(laser_pos, 255, 0, 0);
            if (laser_color == COLOR_GREEN) draw_led_group(laser_pos, 0, 255, 0);
            if (laser_color == COLOR_BLUE)  draw_led_group(laser_pos, 0, 0, 255);
        }

        // Draw Invader with matching color identity
        if (!game_over) {
            draw_invader_cluster(invader_pos);
        }

        led_strip_refresh(led_strip);
        vTaskDelay(pdMS_TO_TICKS(25)); // Frame refresh loop rate
    }
}
