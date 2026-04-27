# tlv320dac3100-esp-idf

ESP-IDF component for the Texas Instruments TLV320DAC3100 stereo audio DAC.
Pure C, no Arduino, no ESP-ADF dependency.

---

## Origin and acknowledgements

This library was written with the assistance of AI (Claude by Anthropic), using the
[Adafruit_TLV320_I2S](https://github.com/adafruit/Adafruit_TLV320_I2S) Arduino library
by Limor Fried / Adafruit Industries as the primary reference for understanding the
TLV320DAC3100's register map and initialisation sequence.

This is an independent reimplementation in pure C targeting the ESP-IDF framework — no
source code was copied.  Register addresses and bit-field definitions come from the
[Texas Instruments TLV320DAC3100 datasheet](https://www.ti.com/product/TLV320DAC3100).

See [COMPATIBILITY.md](COMPATIBILITY.md) for a feature-by-feature comparison between
the two libraries.

---

## What the chip does

The TLV320DAC3100 is a stereo DAC with two output paths:

- **Class-D amplifier** — a switching amplifier that drives a small speaker directly.  No external amplifier IC needed.
- **HP pins** — a headphone/line-out driver.  This library configures them in *line-out mode*, which bypasses the headphone-specific pop-reduction circuitry and is suitable for driving an external amplifier or a line-level input.

Both outputs can be active simultaneously.  Audio data arrives over I2S; codec control (volume, mute, routing, clocks) goes over I2C.

---

## How the two buses are used

```
ESP32                TLV320DAC3100
─────                ─────────────
I2C SDA/SCL  ──────► registers (volume, mute, clock dividers, routing)
I2S BCLK     ──────► bit clock
I2S LRCK     ──────► left/right clock (word select)
I2S DOUT     ──────► audio data
I2S MCLK     ──────► master clock (MCLK)
```

The chip is an I2S *slave*: the ESP32 generates all clocks.  The TLV320 just locks onto whatever BCLK/LRCK/MCLK it sees and converts the incoming digital samples to analog.

---

## Paged register space

The chip has more registers than fit in a single 7-bit address space, so registers are split across *pages*.  You select a page by writing the page number to register `0x00` on whichever page you are currently on.  The library tracks the current page in the device struct and skips the select write when the page is already correct.

Page 0 holds the digital side: clocks, DAC data path, digital volume.
Page 1 holds the analog side: output routing, driver gains, power controls.

---

## Clock configuration

The TLV320 derives its internal operating frequency from MCLK using two dividers and an oversampling ratio:

```
MCLK → ÷ NDAC → ÷ MDAC → DAC_CLK
DAC_CLK → ÷ DOSR → sample clock (must equal the stream sample rate)
```

The datasheet constraint is `2.8 MHz < DOSR × sample_rate < 6.2 MHz`, and DOSR must be a multiple of 8 for the highest-quality filter (Filter A).

Rather than computing this at runtime with a brute-force search, the library uses a small lookup table keyed by `(mclk_hz, sample_rate)`:

| MCLK | Sample rate | NDAC | MDAC | DOSR | DOSR × fS |
|------|-------------|------|------|------|-----------|
| 11,289,600 (256×) | 44,100 | 2 | 1 | 128 | 5.645 MHz |
| 12,288,000 (256×) | 48,000 | 2 | 1 | 128 | 6.144 MHz |
|  8,192,000 (256×) | 32,000 | 2 | 1 | 128 | 4.096 MHz |
| 22,579,200 (512×) | 44,100 | 2 | 2 | 128 | 5.645 MHz |
| 24,576,000 (512×) | 48,000 | 2 | 2 | 128 | 6.144 MHz |
| 16,384,000 (512×) | 32,000 | 2 | 2 | 128 | 4.096 MHz |

The ESP32's I2S peripheral outputs MCLK at a multiple of the sample rate.  ESP-ADF's `i2s_stream` defaults to 256× for 16-bit audio.  Setting `mclk_hz = 0` in `tlv320_config_t` tells the library to derive MCLK automatically as `sample_rate × 256` each time `tlv320_configure()` is called, which is the right choice when the ESP32 is generating MCLK and the stream sample rate can vary.

If no table entry matches, the function returns `ESP_ERR_NOT_SUPPORTED` and logs an error.

---

## Audio signal path (inside the chip)

```
I2S data
   │
   ▼
Left/Right DAC  (digital volume register: −63.5 dB to 0 dB in 0.5 dB steps)
   │
   ▼
Mixer / analog volume control  (route DAC output to output drivers)
   │
   ├──► HPL / HPR driver  (0–9 dB gain, line-out mode)  ──► line-out jack
   │
   └──► Class-D driver    (6/12/18/24 dB gain)           ──► speaker
```

After `tlv320_init()`:
- Both DAC channels are powered and unmuted
- Output routing sends the left DAC to HPL and the speaker left channel, right DAC to HPR
- All analog volume controls are at 0 dB (no attenuation)
- HPL/HPR driver gain is 0 dB; speaker driver gain is 6 dB (minimum)
- HP outputs are in line-out mode
- Both output stages are powered on

---

## Volume control

Volume is controlled digitally in the DAC before the analog output stage.  The register is signed 8-bit two's complement where `0x00` = 0 dB and each step is −0.5 dB, so `0xFF` = −0.5 dB, `0x81` = −63.5 dB.  `0x80` is reserved (the library avoids it).

`tlv320_set_volume(handle, vol_pct)` maps a 0–100 percentage linearly onto this range:
- `100` → `0x00` (0 dB, full volume)
- `1`   → approximately `0x81` (−63.5 dB, nearly silent)
- `0`   → DAC mute bits set (output is muted, register value ignored)

The DAC has built-in *soft-step*: when the volume register changes, the hardware ramps to the new value one step per sample rather than jumping, which eliminates clicks.  The library enables 1-sample soft-step during init.

---

## Mute

`tlv320_set_mute()` sets or clears the DAC mute bits (page 0, register 0x40, bits D3:D2).  This is a zero-crossing-safe mute: the chip waits for the next zero crossing before silencing the output.  The stored volume level is not changed; calling `tlv320_set_volume()` with any value > 0 will clear the mute bits automatically.

---

## Usage

```c
#include "driver/i2c_master.h"
#include "tlv320dac3100.h"

// 1. Create an I2C master bus (caller owns it, may be shared with other devices)
i2c_master_bus_config_t i2c_cfg = {
    .i2c_port          = I2C_NUM_0,
    .sda_io_num        = GPIO_NUM_8,
    .scl_io_num        = GPIO_NUM_9,
    .clk_source        = I2C_CLK_SRC_DEFAULT,
    .glitch_ignore_cnt = 7,
};
i2c_master_bus_handle_t i2c_bus;
ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_cfg, &i2c_bus));

// 2. Init the codec
tlv320_config_t cfg = {
    .i2c_bus  = i2c_bus,
    .i2c_addr = TLV320_I2C_ADDR_DEFAULT,  // 0x18 (ADDR pin low)
    .mclk_hz  = 0,  // auto: 256 × sample_rate
};
tlv320_handle_t codec;
ESP_ERROR_CHECK(tlv320_init(&codec, &cfg));

// 3. Set initial volume
tlv320_set_volume(codec, 80);  // 80%

// 4. When the audio stream starts and you know its format:
tlv320_configure(codec, 44100, 16);  // 44.1 kHz, 16-bit

// 5. Volume control
tlv320_set_volume(codec, 50);   // lower to 50%
tlv320_set_mute(codec, true);   // mute
tlv320_set_mute(codec, false);  // unmute

// 6. Teardown
tlv320_deinit(codec);
```

---

## Adding to a project

In the project root `CMakeLists.txt`, before the ADF/IDF includes:

```cmake
list(APPEND EXTRA_COMPONENT_DIRS "/path/to/tlv320dac3100-esp-idf")
```

In the component or app `CMakeLists.txt`:

```cmake
idf_component_register(
    ...
    REQUIRES ... tlv320dac3100-esp-idf ...
)
```

The I2C bus must be created by the application; the library adds its device to a caller-owned bus so it can coexist with other I2C peripherals (sensors, displays, etc.) on the same bus.

---

## What is not supported

- **PLL** — the chip has an internal fractional PLL that can generate any sample rate from a fixed MCLK.  This library bypasses the PLL entirely and relies on the ESP32 generating an MCLK that is an integer multiple of the sample rate.  Extending the clock table or adding PLL support is straightforward if non-standard rates are needed.
- **ADC / microphone input** — the TLV320DAC3100 is a DAC-only device; there is no ADC.  (The TLV320AIC3100 adds an ADC and is a different part.)
- **EQ / effects processing blocks** — the chip has 25 processing block configurations.  This library uses PRB_P1, the standard block for stereo playback.
