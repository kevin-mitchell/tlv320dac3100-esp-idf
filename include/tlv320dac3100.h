#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Thread safety
 * -------------
 * All public functions in this library are safe to call from multiple FreeRTOS
 * tasks simultaneously on the same handle.  An internal mutex serialises access
 * to the I2C bus and the page-register state.
 *
 * The typical pattern — an audio task calling i2s_channel_write() while a
 * control task calls tlv320_set_volume() — is safe: the audio task never calls
 * any library functions, so there is no contention in that case.
 *
 * Note: tlv320_configure() writes several registers in sequence.  The mutex
 * prevents corruption, but the chip's clock dividers will be in a transitional
 * state for a few I2C transactions during the call.  For glitch-free format
 * changes, mute first, configure, then unmute.
 */

#define TLV320_I2C_ADDR_DEFAULT  0x18

typedef struct tlv320_dev *tlv320_handle_t;

/**
 * @brief Codec configuration passed to tlv320_init().
 *
 * @param i2c_bus    Caller-owned I2C master bus handle.
 * @param i2c_addr   7-bit I2C address (TLV320_I2C_ADDR_DEFAULT = 0x18).
 * @param mclk_hz    MCLK frequency in Hz.  Set to 0 to auto-derive as
 *                   sample_rate * 256 each time tlv320_configure() is called.
 *                   Set to a fixed value (e.g. 12288000) when MCLK comes from
 *                   an external crystal and does not track the sample rate.
 * @param reset_gpio    GPIO number wired to the chip's active-low RESET pin,
 *                      or -1 if the RESET pin is not connected to the ESP32
 *                      (e.g. tied high through a pull-up resistor).  When a
 *                      GPIO is provided, tlv320_init() pulses it low before
 *                      the software reset, which guarantees a clean start even
 *                      if the chip is in an unknown state (e.g. after a
 *                      firmware crash without a full power cycle).
 * @param hp_as_headphone  Set to true if the HPL/HPR pins drive headphones
 *                      directly (e.g. a 3.5 mm headphone jack).  Set to false
 *                      (the default) when HPL/HPR feed an external amplifier
 *                      or line-level input — this is called "line-out mode" in
 *                      the datasheet.  The difference: headphone mode enables
 *                      the chip's pop-suppression ramp (designed to avoid a
 *                      click in headphones on power-up); line-out mode bypasses
 *                      it, which is correct when the downstream amp handles
 *                      its own muting.
 */
typedef struct {
    i2c_master_bus_handle_t i2c_bus;
    uint8_t  i2c_addr;
    uint32_t mclk_hz;
    int      reset_gpio;
    bool     hp_as_headphone;
} tlv320_config_t;

typedef enum {
    TLV320_OUTPUT_SPEAKER = (1 << 0),
    TLV320_OUTPUT_LINEOUT = (1 << 1),
    TLV320_OUTPUT_BOTH    = (1 << 0) | (1 << 1),
} tlv320_output_t;

/**
 * @brief  Initialise the TLV320DAC3100 and power up both outputs.
 *
 * Performs a software reset, configures clocks for 48 kHz / 16-bit I2S
 * (slave mode), powers up both the Class-D speaker and the line-out
 * (HP pins in lineout mode), and sets volume to 0 dB.
 *
 * Call tlv320_configure() after receiving stream info to match the
 * actual sample rate and bit depth.
 */
esp_err_t tlv320_init(tlv320_handle_t *handle, const tlv320_config_t *cfg);

/** @brief Free resources acquired by tlv320_init(). */
esp_err_t tlv320_deinit(tlv320_handle_t handle);

/**
 * @brief  Reconfigure DAC clocks and interface for a new sample format.
 *
 * Call this whenever the audio stream's sample rate or bit depth changes
 * (e.g. on an ADF AEL_MSG_CMD_REPORT_MUSIC_INFO event).  No full reset is
 * performed; the change takes effect on the next audio frame.
 *
 * @param sample_rate  Audio sample rate in Hz (e.g. 44100, 48000).
 * @param bits         Sample bit depth: 16 or 24.
 */
esp_err_t tlv320_configure(tlv320_handle_t handle, uint32_t sample_rate,
                           uint8_t bits);

/**
 * @brief  Set output volume.
 *
 * @param vol_pct  0 = muted (DAC mute bit), 1–100 = linear dB attenuation
 *                 from -63 dB (1%) to 0 dB (100%).
 */
esp_err_t tlv320_set_volume(tlv320_handle_t handle, int vol_pct);

/**
 * @brief  Software mute / unmute (DAC mute bits, zero-crossing safe).
 *
 * Does not change the stored volume level; tlv320_set_volume() will
 * automatically unmute when vol_pct > 0.
 */
esp_err_t tlv320_set_mute(tlv320_handle_t handle, bool mute);

/**
 * @brief  Enable or disable individual output stages.
 *
 * TLV320_OUTPUT_BOTH is the default after tlv320_init().
 */
esp_err_t tlv320_set_output(tlv320_handle_t handle, tlv320_output_t output);

#ifdef __cplusplus
}
#endif
