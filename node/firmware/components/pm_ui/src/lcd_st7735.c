/*
 * lcd_st7735.c - ST7735S 128x160 TFT driver (plain SPI master + DMA, LEDC backlight), landscape 160 x 128.
 *
 * Transfers go through the plain SPI master driver (not esp_lcd) so that every transaction is under our control:
 * commands and parameters are short polling transfers, pixel data is one interrupt-driven DMA transaction straight
 * from the caller's buffer. D/C is a plain GPIO that is set before each (blocking) transaction - no pre_cb, so no
 * ISR/IRAM constraints on gpio_set_level().
 *
 * Thread-safety: all calls come from the single UI task, so there is no locking. Do not call from other tasks.
 *
 * Resources used (others must avoid them):
 *   - SPI host from the board table (pm_board_display_pins_t.spi_host), DMA channel SPI_DMA_CH_AUTO
 *   - LEDC low-speed mode, LEDC_TIMER_1 + LEDC_CHANNEL_1 (backlight PWM, 5 kHz, 10 bit)
 */
#include "lcd_st7735.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "pm_lcd";

/* ---- Panel configuration ----------------------------------------------------------------------------------- */

/* Colour order bit of MADCTL. Most 1.8" modules are BGR; set to 0x00 if red and blue are swapped. */
#define LCD_MADCTL_BGR 0x08

/* RAM offsets of the visible area. 0 for the common 128x160 "black tab" modules (in landscape: x = column,
 * y = row after the MV swap). Other tab variants may need e.g. 1/2. */
#define LCD_X_OFFSET 0
#define LCD_Y_OFFSET 0

/* MADCTL bits */
#define MADCTL_MY 0x80 /* row address order (mirror Y of the native portrait panel) */
#define MADCTL_MX 0x40 /* column address order (mirror X) */
#define MADCTL_MV 0x20 /* row/column exchange (landscape) */

/*
 * Rotation (pm_board_display_pins_t.rotation) -> MADCTL. All variants are landscape 160 x 128 (MV set):
 *
 *   rotation | MADCTL (w/o BGR) | meaning
 *   ---------+------------------+-------------------------------------------------------------
 *       0    | 0x60  MV|MX      | landscape (default mounting)
 *       1    | 0xA0  MV|MY      | landscape rotated 180 deg
 *       2    | 0x20  MV         | landscape, mirrored (0 with the X axis flipped) - test only
 *       3    | 0xE0  MV|MX|MY   | landscape, mirrored (1 with the X axis flipped) - test only
 *
 * LCD_MADCTL_BGR is OR-ed into every value.
 */
static const uint8_t s_madctl[4] = {
    MADCTL_MV | MADCTL_MX,
    MADCTL_MV | MADCTL_MY,
    MADCTL_MV,
    MADCTL_MV | MADCTL_MX | MADCTL_MY,
};

/* ST7735S commands */
#define CMD_SWRESET 0x01
#define CMD_SLPIN   0x10
#define CMD_SLPOUT  0x11
#define CMD_NORON   0x13
#define CMD_INVOFF  0x20
#define CMD_DISPOFF 0x28
#define CMD_DISPON  0x29
#define CMD_CASET   0x2A
#define CMD_RASET   0x2B
#define CMD_RAMWR   0x2C
#define CMD_MADCTL  0x36
#define CMD_COLMOD  0x3A
#define CMD_FRMCTR1 0xB1
#define CMD_FRMCTR2 0xB2
#define CMD_FRMCTR3 0xB3
#define CMD_INVCTR  0xB4
#define CMD_PWCTR1  0xC0
#define CMD_PWCTR2  0xC1
#define CMD_PWCTR3  0xC2
#define CMD_PWCTR4  0xC3
#define CMD_PWCTR5  0xC4
#define CMD_VMCTR1  0xC5
#define CMD_GMCTRP1 0xE0
#define CMD_GMCTRN1 0xE1

/* Backlight PWM. LEDC_TIMER_1 / LEDC_CHANNEL_1 are reserved for the display - do not use them elsewhere. */
#define BL_SPEED_MODE LEDC_LOW_SPEED_MODE
#define BL_TIMER      LEDC_TIMER_1
#define BL_CHANNEL    LEDC_CHANNEL_1
#define BL_RES        LEDC_TIMER_10_BIT
#define BL_DUTY_MAX   ((1u << 10) - 1u)
#define BL_FREQ_HZ    5000

/* Extra bytes on top of max_transfer for the bus (headroom, commands are much smaller anyway). */
#define SPI_TRANSFER_MARGIN 64

/* ---- State ------------------------------------------------------------------------------------------------- */

static struct {
    bool initialized;
    bool bus_owned;    /* we initialized the SPI bus and must free it on error */
    bool panel_on;
    bool bl_enabled;   /* LEDC configured */
    spi_host_device_t host;
    spi_device_handle_t dev;
    int8_t dc;
    int max_transfer;
} s_lcd;

/* ---- Low-level transfers ----------------------------------------------------------------------------------- */

/* Send a command byte (DC low) followed by up to `len` parameter bytes (DC high). Blocking. */
static esp_err_t send_cmd(uint8_t cmd, const uint8_t *data, size_t len)
{
    spi_transaction_t t = {
        .flags = SPI_TRANS_USE_TXDATA,
        .length = 8,
        .tx_data = { cmd },
    };
    gpio_set_level(s_lcd.dc, 0);
    esp_err_t err = spi_device_polling_transmit(s_lcd.dev, &t);
    if (err != ESP_OK || len == 0) {
        return err;
    }

    gpio_set_level(s_lcd.dc, 1);
    if (len <= 4) {
        spi_transaction_t d = {
            .flags = SPI_TRANS_USE_TXDATA,
            .length = len * 8,
        };
        memcpy(d.tx_data, data, len);
        return spi_device_polling_transmit(s_lcd.dev, &d);
    }
    /* Longer parameter lists (gamma, 16 bytes from the const table in flash): the SPI master driver copies
     * non-DMA-capable tx buffers into a temporary DMA bounce buffer itself. Only used during init. */
    spi_transaction_t d = {
        .length = len * 8,
        .tx_buffer = data,
    };
    return spi_device_polling_transmit(s_lcd.dev, &d);
}

static esp_err_t send_cmd1(uint8_t cmd, uint8_t p)
{
    return send_cmd(cmd, &p, 1);
}

/* Pixel data (DC high) as one DMA transaction from the caller's buffer, interrupt-driven, blocks until done. */
static esp_err_t send_pixels(const void *px, size_t bytes)
{
    spi_transaction_t t = {
        .length = bytes * 8,
        .tx_buffer = px,
    };
    gpio_set_level(s_lcd.dc, 1);
    return spi_device_transmit(s_lcd.dev, &t);
}

static esp_err_t set_window(int x, int y, int w, int h)
{
    uint16_t x0 = (uint16_t)(x + LCD_X_OFFSET);
    uint16_t x1 = (uint16_t)(x + w - 1 + LCD_X_OFFSET);
    uint16_t y0 = (uint16_t)(y + LCD_Y_OFFSET);
    uint16_t y1 = (uint16_t)(y + h - 1 + LCD_Y_OFFSET);
    const uint8_t ca[4] = { x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF };
    const uint8_t ra[4] = { y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF };

    esp_err_t err = send_cmd(CMD_CASET, ca, sizeof(ca));
    if (err == ESP_OK) {
        err = send_cmd(CMD_RASET, ra, sizeof(ra));
    }
    if (err == ESP_OK) {
        err = send_cmd(CMD_RAMWR, NULL, 0);
    }
    return err;
}

static void delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : 1);
}

/* ---- Init sequence ----------------------------------------------------------------------------------------- */

/*
 * Register values from the Adafruit ST7735 library (Adafruit_ST7735.cpp, "Rcmd1" + "Rcmd3", used for the
 * green/black tab 1.8" modules). Delays follow the ST7735S datasheet (SWRESET/SLPOUT 120 ms).
 */
typedef struct {
    uint8_t cmd;
    uint8_t len;
    uint8_t data[16];
} init_cmd_t;

static const init_cmd_t s_init_cmds[] = {
    { CMD_FRMCTR1, 3, { 0x01, 0x2C, 0x2D } },                         /* frame rate normal mode */
    { CMD_FRMCTR2, 3, { 0x01, 0x2C, 0x2D } },                         /* idle mode */
    { CMD_FRMCTR3, 6, { 0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D } },       /* partial mode (dot/line inversion) */
    { CMD_INVCTR, 1, { 0x07 } },                                      /* no inversion */
    { CMD_PWCTR1, 3, { 0xA2, 0x02, 0x84 } },                          /* -4.6 V, AUTO mode */
    { CMD_PWCTR2, 1, { 0xC5 } },                                      /* VGH25 2.4 V, VGSEL -10, VGH 3*AVDD */
    { CMD_PWCTR3, 2, { 0x0A, 0x00 } },                                /* opamp current small, boost freq */
    { CMD_PWCTR4, 2, { 0x8A, 0x2A } },                                /* BCLK/2, opamp small/medium-low */
    { CMD_PWCTR5, 2, { 0x8A, 0xEE } },
    { CMD_VMCTR1, 1, { 0x0E } },
    { CMD_INVOFF, 0, { 0 } },
    { CMD_COLMOD, 1, { 0x05 } },                                      /* 16 bit RGB565 */
    { CMD_GMCTRP1, 16, { 0x02, 0x1C, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2D,
                         0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10 } },
    { CMD_GMCTRN1, 16, { 0x03, 0x1D, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D,
                         0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10 } },
};

/* Fill the whole panel RAM with black, in strips limited by max_transfer. */
static esp_err_t clear_ram(void)
{
    int rows = s_lcd.max_transfer / (LCD_W * 2);
    if (rows < 1) {
        rows = 1;  /* bus is sized for at least one line, see lcd_st7735_init */
    }
    if (rows > LCD_H) {
        rows = LCD_H;
    }
    size_t bytes = (size_t)rows * LCD_W * 2;
    void *buf = heap_caps_calloc(1, bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (buf == NULL) {
        ESP_LOGE(TAG, "no DMA memory for clear buffer (%u bytes)", (unsigned)bytes);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = ESP_OK;
    for (int y = 0; y < LCD_H && err == ESP_OK; y += rows) {
        int h = (LCD_H - y < rows) ? LCD_H - y : rows;
        err = lcd_st7735_blit(0, y, LCD_W, h, buf);
    }
    heap_caps_free(buf);
    return err;
}

static esp_err_t panel_init(uint8_t rotation, int8_t rst)
{
    esp_err_t err;

    if (rst != PM_BOARD_PIN_NONE) {
        gpio_set_level(rst, 0);
        delay_ms(10);
        gpio_set_level(rst, 1);
        delay_ms(120);
    } else {
        err = send_cmd(CMD_SWRESET, NULL, 0);
        if (err != ESP_OK) {
            return err;
        }
        delay_ms(120);
    }

    err = send_cmd(CMD_SLPOUT, NULL, 0);
    if (err != ESP_OK) {
        return err;
    }
    delay_ms(120);

    for (size_t i = 0; i < sizeof(s_init_cmds) / sizeof(s_init_cmds[0]); i++) {
        err = send_cmd(s_init_cmds[i].cmd, s_init_cmds[i].data, s_init_cmds[i].len);
        if (err != ESP_OK) {
            return err;
        }
    }

    if (rotation > 3) {
        ESP_LOGW(TAG, "rotation %u out of range, using 0", rotation);
        rotation = 0;
    }
    err = send_cmd1(CMD_MADCTL, s_madctl[rotation] | LCD_MADCTL_BGR);
    if (err != ESP_OK) {
        return err;
    }

    err = send_cmd(CMD_NORON, NULL, 0);
    if (err != ESP_OK) {
        return err;
    }
    delay_ms(10);

    err = clear_ram();
    if (err != ESP_OK) {
        return err;
    }

    err = send_cmd(CMD_DISPON, NULL, 0);
    if (err != ESP_OK) {
        return err;
    }
    delay_ms(100);
    s_lcd.panel_on = true;
    return ESP_OK;
}

static esp_err_t backlight_init(const pm_board_display_pins_t *pins)
{
    ledc_timer_config_t tc = {
        .speed_mode = BL_SPEED_MODE,
        .duty_resolution = BL_RES,
        .timer_num = BL_TIMER,
        .freq_hz = BL_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&tc);
    if (err != ESP_OK) {
        return err;
    }
    /* duty 0 = off; output_invert turns "duty" into "on time" for active-low backlights */
    ledc_channel_config_t cc = {
        .gpio_num = pins->backlight,
        .speed_mode = BL_SPEED_MODE,
        .channel = BL_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BL_TIMER,
        .duty = 0,
        .hpoint = 0,
        .flags.output_invert = pins->backlight_active_high ? 0 : 1,
    };
    return ledc_channel_config(&cc);
}

/* ---- Public API -------------------------------------------------------------------------------------------- */

esp_err_t lcd_st7735_init(const pm_board_display_pins_t *pins, uint32_t spi_hz, int max_transfer)
{
    if (s_lcd.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pins == NULL || pins->sclk < 0 || pins->mosi < 0 || pins->cs < 0 || pins->dc < 0 || spi_hz == 0) {
        ESP_LOGE(TAG, "invalid display pins/clock");
        return ESP_ERR_INVALID_ARG;
    }
    if (max_transfer < LCD_W * 2) {
        max_transfer = LCD_W * 2;  /* at least one full line, so clear_ram() can work */
    }

    memset(&s_lcd, 0, sizeof(s_lcd));
    s_lcd.host = (spi_host_device_t)pins->spi_host;
    s_lcd.dc = pins->dc;
    s_lcd.max_transfer = max_transfer;

    /* Backlight first and off, so nothing flashes while the panel RAM still holds garbage. */
    if (pins->backlight != PM_BOARD_PIN_NONE) {
        esp_err_t err = backlight_init(pins);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "backlight PWM init failed: %s", esp_err_to_name(err));
            return err;
        }
        s_lcd.bl_enabled = true;
    }

    uint64_t out_mask = 1ULL << pins->dc;
    if (pins->rst != PM_BOARD_PIN_NONE) {
        out_mask |= 1ULL << pins->rst;
    }
    gpio_config_t io = {
        .pin_bit_mask = out_mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(err));
        return err;
    }
    if (pins->rst != PM_BOARD_PIN_NONE) {
        gpio_set_level(pins->rst, 1);
    }

    spi_bus_config_t bus = {
        .sclk_io_num = pins->sclk,
        .mosi_io_num = pins->mosi,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = max_transfer + SPI_TRANSFER_MARGIN,
    };
    err = spi_bus_initialize(s_lcd.host, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize(host %d) failed: %s", pins->spi_host, esp_err_to_name(err));
        return err;
    }
    s_lcd.bus_owned = true;

    spi_device_interface_config_t devcfg = {
        .mode = 0,
        .clock_speed_hz = (int)spi_hz,
        .spics_io_num = pins->cs,
        .queue_size = 2,
        .flags = SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_NO_DUMMY,
    };
    err = spi_bus_add_device(s_lcd.host, &devcfg, &s_lcd.dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        goto fail;
    }

    s_lcd.initialized = true;  /* blit (clear_ram) needs this */
    err = panel_init(pins->rotation, pins->rst);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel init failed: %s", esp_err_to_name(err));
        goto fail;
    }

    ESP_LOGI(TAG, "ST7735S %dx%d, SPI host %d @ %lu Hz, rotation %u", LCD_W, LCD_H, pins->spi_host,
             (unsigned long)spi_hz, pins->rotation);
    return ESP_OK;

fail:
    s_lcd.initialized = false;
    if (s_lcd.dev != NULL) {
        spi_bus_remove_device(s_lcd.dev);
        s_lcd.dev = NULL;
    }
    if (s_lcd.bus_owned) {
        spi_bus_free(s_lcd.host);
        s_lcd.bus_owned = false;
    }
    return err;
}

esp_err_t lcd_st7735_blit(int x, int y, int w, int h, const uint16_t *px)
{
    if (!s_lcd.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    /* No partial clipping: the buffer is sent contiguously, so the rectangle must lie fully on screen. */
    if (px == NULL || w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > LCD_W || y + h > LCD_H) {
        ESP_LOGW(TAG, "blit rejected: %d,%d %dx%d", x, y, w, h);
        return ESP_ERR_INVALID_ARG;
    }
    size_t bytes = (size_t)w * (size_t)h * 2;
    if (bytes > (size_t)s_lcd.max_transfer) {
        ESP_LOGW(TAG, "blit %u bytes > max_transfer %d", (unsigned)bytes, s_lcd.max_transfer);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = set_window(x, y, w, h);
    if (err == ESP_OK) {
        err = send_pixels(px, bytes);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "blit failed: %s", esp_err_to_name(err));
    }
    return err;
}

void lcd_st7735_backlight(uint8_t level)
{
    if (!s_lcd.bl_enabled) {
        return;
    }
    uint32_t duty = ((uint32_t)level * BL_DUTY_MAX + 127u) / 255u;
    esp_err_t err = ledc_set_duty(BL_SPEED_MODE, BL_CHANNEL, duty);
    if (err == ESP_OK) {
        err = ledc_update_duty(BL_SPEED_MODE, BL_CHANNEL);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "backlight duty failed: %s", esp_err_to_name(err));
    }
}

void lcd_st7735_power(bool on)
{
    if (!s_lcd.initialized || on == s_lcd.panel_on) {
        return;
    }
    esp_err_t err;
    if (on) {
        err = send_cmd(CMD_SLPOUT, NULL, 0);
        delay_ms(120);
        if (err == ESP_OK) {
            err = send_cmd(CMD_DISPON, NULL, 0);
        }
    } else {
        err = send_cmd(CMD_DISPOFF, NULL, 0);
        if (err == ESP_OK) {
            err = send_cmd(CMD_SLPIN, NULL, 0);
        }
        delay_ms(5);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "power %s failed: %s", on ? "on" : "off", esp_err_to_name(err));
        return;
    }
    s_lcd.panel_on = on;
}
