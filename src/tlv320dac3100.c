#include "tlv320dac3100.h"
#include "tlv320dac3100_regs.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
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
    SemaphoreHandle_t       lock;      /* serialises concurrent API calls */
    uint8_t                 spk_att;   /* last SPK analog attenuation written (line-out mode) */
};

/* -----------------------------------------------------------------------
 * Clock lookup table
 *
 * The chip cannot run at an audio sample rate directly — it needs a chain
 * of integer dividers (NDAC, MDAC, DOSR) to step the incoming master clock
 * (MCLK) down to the right speed.  Computing valid dividers at runtime
 * requires a brute-force search, so instead we pre-calculate the values for
 * the clock rates the ESP32 normally produces and store them here.
 *
 * Each row is: (MCLK frequency, sample rate) → divider values.
 * The ESP32 I2S peripheral generates MCLK as a fixed multiple of the sample
 * rate — typically 256× for 16-bit audio (e.g. 48 000 × 256 = 12 288 000 Hz).
 *
 * Math: NDAC × MDAC × DOSR must equal MCLK / sample_rate.
 * Datasheet constraint: 2.8 MHz < DOSR × sample_rate < 6.2 MHz.
 * DOSR must be a multiple of 8 to enable the highest-quality filter (Filter A).
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

/* Send one byte to a register: [reg_address, value] over I2C. */
static esp_err_t write_reg(struct tlv320_dev *dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    esp_err_t ret = i2c_master_transmit(dev->i2c_dev, buf, sizeof(buf), 50);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C write failed (reg=0x%02x val=0x%02x): %s",
                 reg, val, esp_err_to_name(ret));
    }
    return ret;
}

/* Read one byte back from a register (send address, then receive). */
static esp_err_t read_reg(struct tlv320_dev *dev, uint8_t reg, uint8_t *val)
{
    esp_err_t ret = i2c_master_transmit(dev->i2c_dev, &reg, 1, 50);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C read failed sending address for reg=0x%02x: %s",
                 reg, esp_err_to_name(ret));
        return ret;
    }
    ret = i2c_master_receive(dev->i2c_dev, val, 1, 50);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C read failed receiving data for reg=0x%02x: %s",
                 reg, esp_err_to_name(ret));
    }
    return ret;
}

/* The chip's registers are split into pages (page 0 = digital, page 1 = analog).
 * You select a page by writing its number to register 0x00.  We cache the current
 * page and skip the I2C write when we're already on the right one. */
static esp_err_t set_page(struct tlv320_dev *dev, uint8_t page)
{
    if (dev->cur_page == page) return ESP_OK;
    esp_err_t ret = write_reg(dev, R_PAGE_SELECT, page);
    if (ret == ESP_OK) dev->cur_page = page;
    return ret;
}

/* Convenience macros: switch to page 0 (digital) or page 1 (analog), write a
 * register, and return immediately if anything fails. */
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
 * Init helpers — DAC data path and analog routing
 *
 * Keeping these in their own functions means any WR0/WR1 failure returns
 * from the helper (not from tlv320_init directly), so tlv320_init can
 * catch the error and reach the fail: cleanup label properly.
 * --------------------------------------------------------------------- */

/* Configure the digital data path: power both DACs, normal L/R routing,
 * 1-sample soft-step, 0 dB digital volume, both channels unmuted. */
static esp_err_t init_datapath(struct tlv320_dev *dev)
{
    /* Select the internal DSP processing block.  PRB_P1 is the standard
     * stereo playback configuration — no EQ or effects, lowest latency. */
    WR0(dev, R_DAC_PRB, 0x01);

    /* Power both DAC channels, route left audio → left channel and right → right
     * (no swap/mix), enable soft-step so volume changes ramp smoothly.
     * 0xD4 = 1101 0100: D7=1(L_pwr) D6=1(R_pwr) D5:D4=01(L_normal)
     *                    D3:D2=01(R_normal) D1:D0=00(softstep 1 samp) */
    WR0(dev, R_DAC_DATAPATH, 0xD4);

    /* Unmute both channels, independent L/R control. */
    WR0(dev, R_DAC_VOL_CTRL, 0x00);

    /* Digital volume: 0 dB on both channels (register value 0 = full scale). */
    WR0(dev, R_DAC_LVOL, 0x00);
    WR0(dev, R_DAC_RVOL, 0x00);
    return ESP_OK;
}

/* Configure the analog output routing: wire both DACs to the mixer, set
 * 0 dB attenuation on all paths, enable and gain-stage all output drivers. */
static esp_err_t init_routing(struct tlv320_dev *dev)
{
    /* Wire the digital DAC outputs to the analog output drivers.
     * The chip has an internal mixer that sits between the DAC and the
     * physical output pins; we route L-DAC → left mixer, R-DAC → right mixer.
     * 0x44 = 0100 0100: D7:D6=01 (L→mixer), D3:D2=01 (R→mixer) */
    WR1(dev, R1_OUT_ROUTING, 0x44);

    /* Set analog volume for each output path to 0 dB (no attenuation).
     * The D7 bit enables the route; D6:D0=0 means full level. */
    WR1(dev, R1_HPL_VOL, 0x80);
    WR1(dev, R1_HPR_VOL, 0x80);
    WR1(dev, R1_SPK_VOL, 0x80);

    /* Set the line-out driver gain to 0 dB and enable the output.
     * 0x04 = 0000 0100: D6:D3=0000 (0 dB), D2=1 (enabled) */
    WR1(dev, R1_HPL_DRIVER, 0x04);
    WR1(dev, R1_HPR_DRIVER, 0x04);

    /* Set the Class-D speaker driver to minimum gain (6 dB) and enable it.
     * 0x04 = 0000 0100: D4:D3=00 (6 dB), D2=1 (enabled) */
    WR1(dev, R1_SPK_DRIVER, 0x04);

    /* HP driver mode: line-out bypasses the pop-suppression ramp (correct for
     * an external amp); headphone mode enables it (correct for direct-drive). */
    WR1(dev, R1_HP_DRIVER_CTRL, dev->cfg.hp_as_headphone ? 0x00 : 0x06);

    /* Tie unused AIN1/AIN2 inputs to the chip's internal common-mode to prevent
     * noise from floating inputs leaking into the output mixer. */
    WR1(dev, R1_AIN_CM_CTRL, 0xC0);
    return ESP_OK;
}

/* -----------------------------------------------------------------------
 * Clock configuration
 * --------------------------------------------------------------------- */

/* Scan the table for a row matching the given MCLK and sample rate. */
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

/* Write the chip's clock dividers (NDAC, MDAC, DOSR) so the DAC runs at the
 * requested sample rate, and configure the I2S interface word length.
 * Called once during init and again any time the stream format changes. */
static esp_err_t apply_clk(struct tlv320_dev *dev, uint32_t sample_rate,
                           uint8_t bits)
{
    if (bits != 16 && bits != 24) {
        ESP_LOGE(TAG, "unsupported bit depth %u — only 16 and 24 are supported", bits);
        return ESP_ERR_INVALID_ARG;
    }

    /* Determine MCLK: use configured value if set, else 256 × sample_rate. */
    uint32_t mclk = dev->cfg.mclk_hz ? dev->cfg.mclk_hz : sample_rate * 256;

    const clk_entry_t *e = find_clk(mclk, sample_rate);
    if (!e && dev->cfg.mclk_hz) {
        /* Configured mclk_hz had no exact match; try the standard ESP32 multiples
         * in case the user's value was close but not exact. */
        ESP_LOGW(TAG, "mclk_hz=%lu has no preset for fs=%lu Hz — trying 256x/512x fallbacks",
                 (unsigned long)dev->cfg.mclk_hz, (unsigned long)sample_rate);
        e = find_clk(sample_rate * 256, sample_rate);
        if (!e) e = find_clk(sample_rate * 512, sample_rate);
    }
    if (!e) {
        ESP_LOGE(TAG, "no clock preset for fs=%lu Hz (tried mclk=%lu Hz). "
                 "Supported sample rates: 32000, 44100, 48000 Hz. "
                 "To add others, insert a row in s_clk_table in tlv320dac3100.c.",
                 (unsigned long)sample_rate, (unsigned long)mclk);
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

/* Power the line-out / headphone driver for left and/or right channels.
 * CM_1V65 sets the common-mode bias voltage to 1.65 V (mid-rail for 3.3 V). */
static esp_err_t power_hp(struct tlv320_dev *dev, bool left, bool right)
{
    uint8_t val = HP_BIT2_RESERVED | CM_1V65;
    if (left)  val |= 0x80;
    if (right) val |= 0x40;
    esp_err_t ret = set_page(dev, 1);
    if (ret != ESP_OK) return ret;
    return write_reg(dev, R1_HP_DRIVERS, val);
}

/* Enable or disable the Class-D speaker amplifier. */
static esp_err_t power_spk(struct tlv320_dev *dev, bool en)
{
    esp_err_t ret = set_page(dev, 1);
    if (ret != ESP_OK) return ret;
    return write_reg(dev, R1_SPK_AMP, en ? 0x80 : 0x00);
}

/* -----------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------- */

esp_err_t tlv320_init(tlv320_handle_t *out_handle, const tlv320_config_t *cfg)
{
    if (!out_handle || !cfg) return ESP_ERR_INVALID_ARG;

    if (!cfg->i2c_bus) {
        ESP_LOGE(TAG, "cfg->i2c_bus is NULL — create a bus with i2c_new_master_bus() first");
        return ESP_ERR_INVALID_ARG;
    }

    /* The TLV320DAC3100 I2C address is set by the ADDR pin: 0x18 (low) or 0x19 (high). */
    if (cfg->i2c_addr != 0x18 && cfg->i2c_addr != 0x19) {
        ESP_LOGW(TAG, "unusual I2C address 0x%02x — expected 0x18 (ADDR pin low) "
                 "or 0x19 (ADDR pin high)", cfg->i2c_addr);
    }

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
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to add TLV320 to I2C bus: %s", esp_err_to_name(ret));
        free(dev);
        return ret;
    }

    dev->lock = xSemaphoreCreateMutex();
    if (!dev->lock) {
        ESP_LOGE(TAG, "failed to create mutex");
        i2c_master_bus_rm_device(dev->i2c_dev);
        free(dev);
        return ESP_ERR_NO_MEM;
    }

    /* ── Hardware reset (if GPIO wired) ────────────────────────────── */
    if (cfg->reset_gpio >= 0) {
        gpio_config_t gpio_cfg = {
            .pin_bit_mask = (1ULL << cfg->reset_gpio),
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&gpio_cfg);
        gpio_set_level(cfg->reset_gpio, 0);   /* assert reset */
        vTaskDelay(pdMS_TO_TICKS(1));          /* ≥ 10 ns required; 1 ms is safe */
        gpio_set_level(cfg->reset_gpio, 1);   /* release reset */
        vTaskDelay(pdMS_TO_TICKS(1));          /* chip needs ≤ 1 ms to initialise */
        dev->cur_page = 0xFF;
    }

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
    ret = init_datapath(dev);
    if (ret != ESP_OK) goto fail;

    /* ── Analog output routing (page 1) ─────────────────────────────── */
    ret = init_routing(dev);
    if (ret != ESP_OK) goto fail;

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
    ESP_LOGE(TAG, "tlv320_init failed (%s) at I2C addr 0x%02x",
             esp_err_to_name(ret), dev->cfg.i2c_addr);
    if (ret == ESP_ERR_TIMEOUT) {
        ESP_LOGE(TAG, "  → chip not responding: check wiring, 4.7kΩ pull-ups on SDA/SCL, and power supply");
    }
    vSemaphoreDelete(dev->lock);
    i2c_master_bus_rm_device(dev->i2c_dev);
    free(dev);
    return ret;
}

esp_err_t tlv320_deinit(tlv320_handle_t handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    struct tlv320_dev *dev = handle;

    /* Take the lock so we don't tear down while another task is mid-operation. */
    if (xSemaphoreTake(dev->lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "deinit: could not acquire lock — forcing cleanup anyway");
    }

    /* Mute and power down outputs before releasing resources. */
    set_page(dev, 0);
    write_reg(dev, R_DAC_VOL_CTRL, 0x0C); /* mute L+R */
    set_page(dev, 1);
    write_reg(dev, R1_HP_DRIVERS, HP_BIT2_RESERVED); /* HPL+HPR off */
    write_reg(dev, R1_SPK_AMP, 0x00);                /* speaker off */

    /* Give then delete — must give before deleting or FreeRTOS asserts. */
    xSemaphoreGive(dev->lock);
    vSemaphoreDelete(dev->lock);

    i2c_master_bus_rm_device(dev->i2c_dev);
    free(dev);
    return ESP_OK;
}

esp_err_t tlv320_configure(tlv320_handle_t handle, uint32_t sample_rate,
                           uint8_t bits)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    if (sample_rate == 0) {
        ESP_LOGE(TAG, "sample_rate must not be 0");
        return ESP_ERR_INVALID_ARG;
    }
    struct tlv320_dev *dev = handle;
    if (xSemaphoreTake(dev->lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "configure: timeout waiting for lock");
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t ret = apply_clk(dev, sample_rate, bits);
    xSemaphoreGive(dev->lock);
    return ret;
}

esp_err_t tlv320_set_volume(tlv320_handle_t handle, int vol_pct)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    struct tlv320_dev *dev = handle;

    if (vol_pct < 0)   vol_pct = 0;
    if (vol_pct > 100) vol_pct = 100;

    if (xSemaphoreTake(dev->lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "set_volume: timeout waiting for lock");
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret;

    if (!dev->cfg.hp_as_headphone) {
        /* ── Line-out mode ────────────────────────────────────────────────
         * The line-out path (HPL/HPR → external amp) stays at 0 dB always
         * so the amp receives a consistent reference level.  Volume is
         * controlled by attenuating only the speaker (Class-D) analog path.
         *
         * vol_pct=0 uses the DAC mute bit, which silences both outputs —
         * that is the correct behaviour for a mute gesture.
         * vol_pct=1–100 maps linearly onto R1_SPK_VOL attenuation (0–127).
         * ---------------------------------------------------------------- */
        if (vol_pct == 0) {
            ret = set_page(dev, 0);
            if (ret != ESP_OK) goto done;
            ret = write_reg(dev, R_DAC_VOL_CTRL, 0x0C); /* DAC mute */
            goto done;
        }

        /* Unmute DAC (in case it was muted by a previous vol=0 call) and
         * ensure the DAC digital volume is at full scale (0 dB) so the
         * line-out gets the cleanest signal possible. */
        ret = set_page(dev, 0);
        if (ret != ESP_OK) goto done;
        ret = write_reg(dev, R_DAC_VOL_CTRL, 0x00);
        if (ret != ESP_OK) goto done;
        ret = write_reg(dev, R_DAC_LVOL, 0x00);
        if (ret != ESP_OK) goto done;
        ret = write_reg(dev, R_DAC_RVOL, 0x00);
        if (ret != ESP_OK) goto done;

        /* Attenuate only the speaker analog path.  D7=1 keeps the routing
         * to the Class-D amp enabled; D6:D0 is the attenuation (0=0 dB,
         * 127=−78.3 dB). */
        dev->spk_att = (uint8_t)((100 - vol_pct) * 127 / 100);
        ret = set_page(dev, 1);
        if (ret != ESP_OK) goto done;
        ret = write_reg(dev, R1_SPK_VOL, 0x80 | dev->spk_att);

    } else {
        /* ── Headphone mode ───────────────────────────────────────────────
         * Both the HP (headphone) and speaker outputs track the same
         * volume, controlled by the DAC digital volume register which sits
         * upstream of both paths.
         *
         * The register is signed: 0x00 = 0 dB (full), each step = −0.5 dB
         * down to −63.5 dB (0x81).  1–100% maps linearly onto 0 → −63.5 dB.
         * ---------------------------------------------------------------- */
        ret = set_page(dev, 0);
        if (ret != ESP_OK) goto done;

        if (vol_pct == 0) {
            ret = write_reg(dev, R_DAC_VOL_CTRL, 0x0C);
            goto done;
        }

        int8_t dac_val = (int8_t)(-(int32_t)(100 - vol_pct) * 127 / 100);
        ret = write_reg(dev, R_DAC_LVOL, (uint8_t)dac_val);
        if (ret != ESP_OK) goto done;
        ret = write_reg(dev, R_DAC_RVOL, (uint8_t)dac_val);
        if (ret != ESP_OK) goto done;
        ret = write_reg(dev, R_DAC_VOL_CTRL, 0x00);
    }

done:
    xSemaphoreGive(dev->lock);
    return ret;
}

esp_err_t tlv320_set_mute(tlv320_handle_t handle, bool mute)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    struct tlv320_dev *dev = handle;

    if (xSemaphoreTake(dev->lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "set_mute: timeout waiting for lock");
        return ESP_ERR_TIMEOUT;
    }

    /* The chip waits for the audio waveform to cross zero before going silent,
     * which prevents the click you'd otherwise hear from an abrupt cut.
     * D3=L_mute, D2=R_mute; D1:D0=00 (independent mode). */
    esp_err_t ret = set_page(dev, 0);
    if (ret == ESP_OK) {
        ret = write_reg(dev, R_DAC_VOL_CTRL, mute ? 0x0C : 0x00);
    }

    xSemaphoreGive(dev->lock);
    return ret;
}

esp_err_t tlv320_set_output(tlv320_handle_t handle, tlv320_output_t output)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    struct tlv320_dev *dev = handle;

    bool spk = (output & TLV320_OUTPUT_SPEAKER) != 0;
    bool lo  = (output & TLV320_OUTPUT_LINEOUT) != 0;

    if (xSemaphoreTake(dev->lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "set_output: timeout waiting for lock");
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = power_hp(dev, lo, lo);
    if (ret == ESP_OK) ret = power_spk(dev, spk);

    xSemaphoreGive(dev->lock);
    return ret;
}
