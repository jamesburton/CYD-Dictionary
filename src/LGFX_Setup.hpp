#pragma once

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include "DisplayConfig.hpp"

// LovyanGFX device for the Freenove ESP32-S3 Display FNK0104 (variant AB):
//   - ILI9341 panel on SPI2 (HSPI), BGR order, inverted
//   - FT6336U capacitive touch on I2C
//   - PWM backlight on GPIO45 (active HIGH)
class LGFX : public lgfx::LGFX_Device
{
    lgfx::Panel_ILI9341 _panel_instance;
    lgfx::Bus_SPI       _bus_instance;
    lgfx::Light_PWM     _light_instance;
    lgfx::Touch_FT5x06  _touch_instance;   // FT5x06 family driver covers FT6336U

public:
    LGFX(void)
    {
        {
            auto cfg = _bus_instance.config();
            cfg.spi_host    = SPI2_HOST;          // HSPI
            cfg.spi_mode    = 0;
            cfg.freq_write  = 40000000;
            cfg.freq_read   = 16000000;
            cfg.spi_3wire   = false;
            cfg.use_lock    = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk    = PIN_LCD_SCLK;
            cfg.pin_mosi    = PIN_LCD_MOSI;
            cfg.pin_miso    = PIN_LCD_MISO;
            cfg.pin_dc      = PIN_LCD_DC;
            _bus_instance.config(cfg);
            _panel_instance.setBus(&_bus_instance);
        }

        {
            auto cfg = _panel_instance.config();
            cfg.pin_cs          = PIN_LCD_CS;
            cfg.pin_rst         = PIN_LCD_RST;
            cfg.pin_busy        = -1;
            cfg.panel_width     = LCD_NATIVE_W;
            cfg.panel_height    = LCD_NATIVE_H;
            cfg.offset_x        = 0;
            cfg.offset_y        = 0;
            cfg.offset_rotation = 0;
            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits  = 1;
            cfg.readable        = true;
            cfg.invert          = true;    // FNK0104AB setup sets TFT_INVERSION_ON
            cfg.rgb_order       = false;   // TFT_BGR
            cfg.dlen_16bit      = false;
            cfg.bus_shared      = true;
            _panel_instance.config(cfg);
        }

        {
            auto cfg = _light_instance.config();
            cfg.pin_bl      = PIN_LCD_BL;
            cfg.invert      = false;       // active HIGH
            cfg.freq        = 12000;
            cfg.pwm_channel = 7;
            _light_instance.config(cfg);
            _panel_instance.setLight(&_light_instance);
        }

        {
            auto cfg = _touch_instance.config();
            // FT6336U reports pixel coordinates in the panel's native orientation.
            cfg.x_min      = 0;
            cfg.x_max      = LCD_NATIVE_W - 1;   // 239
            cfg.y_min      = 0;
            cfg.y_max      = LCD_NATIVE_H - 1;   // 319
            cfg.pin_int    = PIN_TOUCH_INT;
            cfg.pin_rst    = -1;                 // RST driven manually in setup()
            cfg.bus_shared = false;
            cfg.offset_rotation = 0;             // adjust (0..7) if touch axes are swapped/mirrored
            cfg.i2c_port   = TOUCH_I2C_PORT;
            cfg.i2c_addr   = TOUCH_I2C_ADDR;
            cfg.pin_sda    = PIN_TOUCH_SDA;
            cfg.pin_scl    = PIN_TOUCH_SCL;
            cfg.freq       = 400000;
            _touch_instance.config(cfg);
            _panel_instance.setTouch(&_touch_instance);
        }

        setPanel(&_panel_instance);
    }
};
