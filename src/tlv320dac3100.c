#include "tlv320dac3100.h"
#include "tlv320dac3100_regs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>

#define TAG "tlv320"

/* -----------------------------------------------------------------------
 * Internal device struct
 * --------------------------------------------------------------------- */

struct tlv320_dev {
    i2c_master_dev_handle_t i2c_dev;
    tlv320_config_t         cfg;
    uint8_t                 cur_page;
};

/* -----------------------------------------------------------------------
 * Clock lookup table
 *
 * Entries satisfy: NDAC × MDAC × DOSR = mclk_hz / sample_rate
 * Constraint: 2.8 MHz < DOSR × sample_rate < 6.2 MHz
 * Filter A (highest quality) requires DOSR to be a multiple of 8.
 * --------------------------------------------------------------------- */

typedef struct {
    uint32_t mclk_hz;
    uint32_t sample_rate;
    uint8_t  ndac, mdac;
    uint16_t dosr;
} clk_entry_t;

static const clk_entry_t s_clk_table[] = {
    /* 256 × fs MCLK — standard for ESP32 i2s_stream */
    { 11289600,  44100,  2, 1, 128 },   /* DOSR×fS = 5.645 MHz */
    { 12288000,  48000,  2, 1, 128 },   /* DOSR×fS = 6.144 MHz */
    {  8192000,  32000,  2, 1, 128 },   /* DOSR×fS = 4.096 MHz */
    /* 512 × fs MCLK */
    { 22579200,  44100,  2, 2, 128 },
    { 24576000,  48000,  2, 2, 128 },
    { 16384000,  32000,  2, 2, 128 },
};

/* -----------------------------------------------------------------------
 * I2C helpers
 * --------------------------------------------------------------------- */

static esp_err_t write_reg(struct tlv320_dev *dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(dev->i2c_dev, buf, sizeof(buf), 50);
}

static esp_err_t read_reg(struct tlv320_dev *dev, uint8_t reg, uint8_t *val)
{
    esp_err_t ret = i2c_master_transmit(dev->i2c_dev, &reg, 1, 50);
    if (ret != ESP_OK) return ret;
    return i2c_master_receive(dev->i2c_dev, val, 1, 50);
}

static esp_err_t set_page(struct tlv320_dev *dev, uint8_t page)
{
    if (dev->cur_page == page) return ESP_OK;
    esp_err_t ret = write_reg(dev, R_PAGE_SELECT, page);
    if (ret == ESP_OK) dev->cur_page = page;
    return ret;
}

/* Shorthand helpers that switch page before writing. */
#define WR0(dev, reg, val)  do { \
    esp_err_t _r = set_page(dev, 0); \
    if (_r == ESP_OK) _r = write_reg(dev, reg, val); \
    if (_r != ESP_OK) return _r; \
} while(0)

#define WR1(dev, reg, val)  do { \
    esp_err_t _r = set_page(dev, 1); \
    if (_r == ESP_OK) _r = write_reg(dev, reg, val); \
    if (_r != ESP_OK) return _r; \
} while(0)

/* -----------------------------------------------------------------------
 * Clock configuration
 * --------------------------------------------------------------------- */

static const clk_entry_t *find_clk(uint32_t mclk_hz, uint32_t sample_rate)
{
    for (size_t i = 0; i < sizeof(s_clk_table) / sizeof(s_clk_table[0]); i++) {
        if (s_clk_table[i].mclk_hz == mclk_hz &&
            s_clk_table[i].sample_rate == sample_rate) {
            return &s_clk_table[i];
        }
    }
    return NULL;
}

static esp_err_t apply_clk(struct tlv320_dev *dev, uint32_t sample_rate,
                           uint8_t bits)
{
    /* Determine MCLK: use configured value if set, else 256 × sample_rate. */
    uint32_t mclk = dev->cfg.mclk_hz ? dev->cfg.mclk_hz : sample_rate * 256;

    const clk_entry_t *e = find_clk(mclk, sample_rate);
    if (!e && dev->cfg.mclk_hz) {
        /* Fixed MCLK didn't match; try auto-derived fallbacks. */
        e = find_clk(sample_rate * 256, sample_rate);
        if (!e) e = find_clk(sample_rate * 512, sample_rate);
    }
    if (!e) {
        ESP_LOGE(TAG, "No clock preset for mclk=%lu fs=%lu",
                 (unsigned long)mclk, (unsigned long)sample_rate);
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* CODEC_CLKIN = MCLK (bits D1:D0 = 00). */
    WR0(dev, R_CLOCK_MUX1, 0x00);

    /* NDAC and MDAC: enable bit (D7) + divider value (D6:D0, 0 means 128). */
    WR0(dev, R_NDAC, 0x80 | (e->ndac & 0x7F));
    WR0(dev, R_MDAC, 0x80 | (e->mdac & 0x7F));

    /* DOSR (10-bit, split across two registers). */
    WR0(dev, R_DOSR_MSB, (e->dosr >> 8) & 0x03);
    WR0(dev, R_DOSR_LSB, (uint8_t)(e->dosr & 0xFF));

    /* Codec interface: I2S format, slave (BCLK/WCLK inputs from ESP32). */
    uint8_t iface = IF_FMT_I2S | IF_BCLK_IN | IF_WCLK_IN;
    iface |= (bits == 24) ? IF_LEN_24 : IF_LEN_16;
    WR0(dev, R_CODEC_IF_CTRL1, iface);

    ESP_LOGI(TAG, "clk: mclk=%lu fs=%lu ndac=%u mdac=%u dosr=%u bits=%u",
             (unsigned long)mclk, (unsigned long)sample_rate,
             e->ndac, e->mdac, e->dosr, bits);
    return ESP_OK;
}

/* -----------------------------------------------------------------------
 * Output stage helpers (page 1)
 * --------------------------------------------------------------------- */

static esp_err_t power_hp(struct tlv320_dev *dev, bool left, bool right)
{
    uint8_t val = HP_BIT2_RESERVED | CM_1V65;
    if (left)  val |= 0x80;
    if (right) val |= 0x40;
    return (set_page(dev, 1) == ESP_OK) ? write_reg(dev, R1_HP_DRIVERS, val)
                                        : ESP_FAIL;
}

static esp_err_t power_spk(struct tlv320_dev *dev, bool en)
{
    return (set_page(dev, 1) == ESP_OK)
        ? write_reg(dev, R1_SPK_AMP, en ? 0x80 : 0x00)
        : ESP_FAIL;
}

/* -----------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------- */

esp_err_t tlv320_init(tlv320_handle_t *out_handle, const tlv320_config_t *cfg)
{
    if (!out_handle || !cfg) return ESP_ERR_INVALID_ARG;

    struct tlv320_dev *dev = calloc(1, sizeof(*dev));
    if (!dev) return ESP_ERR_NO_MEM;

    dev->cfg      = *cfg;
    dev->cur_page = 0xFF; /* force first set_page() to always write */

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = cfg->i2c_addr,
        .scl_speed_hz    = 400000,
    };
    esp_err_t ret = i2c_master_bus_add_device(cfg->i2c_bus, &dev_cfg,
                                              &dev->i2c_dev);
    if (ret != ESP_OK) { free(dev); return ret; }

    /* ── Software reset ─────────────────────────────────────────────── */
    ret = set_page(dev, 0);
    if (ret != ESP_OK) goto fail;
    ret = write_reg(dev, R_RESET, 0x01);
    if (ret != ESP_OK) goto fail;
    vTaskDelay(pdMS_TO_TICKS(10));
    dev->cur_page = 0xFF; /* reset cleared the page register */

    /* ── Clock configuration (default: 48 kHz / 16-bit) ────────────── */
    ret = apply_clk(dev, 48000, 16);
    if (ret != ESP_OK) goto fail;

    /* ── DAC data path ──────────────────────────────────────────────── */
    /* PRB_P1: processing block 1 (standard for 48 kHz playback). */
    WR0(dev, R_DAC_PRB, 0x01);

    /* L+R DAC powered, normal data path, 1-sample soft-step.
     * 0xD4 = 1101 0100: D7=1(L_pwr) D6=1(R_pwr) D5:D4=01(L_normal)
     *                    D3:D2=01(R_normal) D1:D0=00(softstep 1 samp) */
    WR0(dev, R_DAC_DATAPATH, 0xD4);

    /* Unmute both channels, independent L/R control. */
    WR0(dev, R_DAC_VOL_CTRL, 0x00);

    /* Digital volume: 0 dB on both channels. */
    WR0(dev, R_DAC_LVOL, 0x00);
    WR0(dev, R_DAC_RVOL, 0x00);

    /* ── Analog output routing (page 1) ─────────────────────────────── */
    /* Route L-DAC to left mixer (D7:D6=01), R-DAC to right mixer (D3:D2=01).
     * 0x44 = 0100 0100 */
    WR1(dev, R1_OUT_ROUTING, 0x44);

    /* Analog volume controls: enable route (D7=1), 0 dB attenuation (D6:D0=0). */
    WR1(dev, R1_HPL_VOL, 0x80);
    WR1(dev, R1_HPR_VOL, 0x80);
    WR1(dev, R1_SPK_VOL, 0x80);

    /* Output driver PGA: HPL/HPR at 0 dB gain, unmuted (D2=1 means output enabled).
     * 0x04 = 0000 0100: D6:D3=0000 (0 dB), D2=1 (enabled) */
    WR1(dev, R1_HPL_DRIVER, 0x04);
    WR1(dev, R1_HPR_DRIVER, 0x04);

    /* Speaker driver: 6 dB fixed gain (minimum, D4:D3=00), output enabled.
     * 0x04 = 0000 0100: D4:D3=00 (6 dB), D2=1 (enabled) */
    WR1(dev, R1_SPK_DRIVER, 0x04);

    /* HP outputs in line-out mode (D2=1 left, D1=1 right). */
    WR1(dev, R1_HP_DRIVER_CTRL, 0x06);

    /* Power up HP drivers: both channels, CM=1.65 V.
     * 0xD4 = 1101 0100: D7=1(HPL) D6=1(HPR) D4:D3=10(CM_1V65) D2=1(reserved) */
    ret = power_hp(dev, true, true);
    if (ret != ESP_OK) goto fail;

    /* Power up Class-D speaker amplifier. */
    ret = power_spk(dev, true);
    if (ret != ESP_OK) goto fail;

    *out_handle = dev;
    ESP_LOGI(TAG, "init ok, addr=0x%02x", cfg->i2c_addr);
    return ESP_OK;

fail:
    i2c_master_bus_rm_device(dev->i2c_dev);
    free(dev);
    return ret;
}

esp_err_t tlv320_deinit(tlv320_handle_t handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    struct tlv320_dev *dev = handle;

    /* Mute and power down outputs before releasing resources. */
    set_page(dev, 0);
    write_reg(dev, R_DAC_VOL_CTRL, 0x0C); /* mute L+R */
    set_page(dev, 1);
    write_reg(dev, R1_HP_DRIVERS, HP_BIT2_RESERVED); /* HPL+HPR off */
    write_reg(dev, R1_SPK_AMP, 0x00);                /* speaker off */

    i2c_master_bus_rm_device(dev->i2c_dev);
    free(dev);
    return ESP_OK;
}

esp_err_t tlv320_configure(tlv320_handle_t handle, uint32_t sample_rate,
                           uint8_t bits)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    return apply_clk(handle, sample_rate, bits);
}

esp_err_t tlv320_set_volume(tlv320_handle_t handle, int vol_pct)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    struct tlv320_dev *dev = handle;

    /* Clamp to valid range. */
    if (vol_pct < 0)   vol_pct = 0;
    if (vol_pct > 100) vol_pct = 100;

    esp_err_t ret = set_page(dev, 0);
    if (ret != ESP_OK) return ret;

    if (vol_pct == 0) {
        /* Hard mute via DAC mute bits. */
        return write_reg(dev, R_DAC_VOL_CTRL, 0x0C);
    }

    /* Map 100% → 0 dB (reg 0x00), 1% → ~−63 dB (reg 0x81).
     * The DAC volume register is signed 8-bit two's complement:
     * positive = boost, negative = attenuation (steps of 0.5 dB). */
    int8_t reg_val = (int8_t)(-(int32_t)(100 - vol_pct) * 127 / 100);

    ret = write_reg(dev, R_DAC_LVOL, (uint8_t)reg_val);
    if (ret != ESP_OK) return ret;
    ret = write_reg(dev, R_DAC_RVOL, (uint8_t)reg_val);
    if (ret != ESP_OK) return ret;

    /* Clear mute bits (D3=0, D2=0), independent L/R mode. */
    return write_reg(dev, R_DAC_VOL_CTRL, 0x00);
}

esp_err_t tlv320_set_mute(tlv320_handle_t handle, bool mute)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    struct tlv320_dev *dev = handle;

    esp_err_t ret = set_page(dev, 0);
    if (ret != ESP_OK) return ret;

    /* D3=L_mute, D2=R_mute; D1:D0=00 (independent mode). */
    return write_reg(dev, R_DAC_VOL_CTRL, mute ? 0x0C : 0x00);
}

esp_err_t tlv320_set_output(tlv320_handle_t handle, tlv320_output_t output)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    struct tlv320_dev *dev = handle;

    bool spk = (output & TLV320_OUTPUT_SPEAKER) != 0;
    bool lo  = (output & TLV320_OUTPUT_LINEOUT) != 0;

    esp_err_t ret = power_hp(dev, lo, lo);
    if (ret != ESP_OK) return ret;
    return power_spk(dev, spk);
}
