# Technical internals

This document covers the deeper details of how the TLV320DAC3100 works and how this library
configures it.  It's here for anyone who wants to understand *why* things are set up the way
they are, or who needs to extend the library.

---

## How the two buses are used

The TLV320DAC3100 speaks two protocols simultaneously:

```
ESP32                TLV320DAC3100
─────                ─────────────
I2C SDA/SCL  ──────► registers (volume, mute, clock dividers, routing)
I2S BCLK     ──────► bit clock
I2S LRCK     ──────► left/right clock (word select)
I2S DOUT     ──────► audio data
I2S MCLK     ──────► master clock (MCLK)
```

**I2C** is used to write configuration registers — volume, mute state, output routing, clock
dividers.  This library handles all of that.

**I2S** carries the actual audio samples.  The chip is an I2S *slave*: the ESP32 generates all
the clocks, and the TLV320 locks onto them and converts the incoming digital samples to analog.
You set up the I2S stream using the standard ESP-IDF `driver/i2s_std.h` API separately from
this library.

---

## Paged register space

The chip has more registers than fit in a single 7-bit I2C address space, so they are split
across numbered *pages* — think of it like chapters in a book.  You select a page by writing
its number to register `0x00`, then access registers within that page normally.

- **Page 0** — digital side: clock dividers, DAC data path, digital volume
- **Page 1** — analog side: output routing, driver gains, power controls

The library tracks which page is currently selected and skips the page-select write when
it's already on the right one, which saves I2C bus time.

---

## Clock configuration

The TLV320 cannot run at an audio sample rate directly.  It has an internal clock chain that
divides the incoming master clock (MCLK) down to the right speed:

```
MCLK → ÷ NDAC → ÷ MDAC → DAC_CLK → ÷ DOSR → sample clock
```

The values NDAC, MDAC, and DOSR must be chosen so that `NDAC × MDAC × DOSR = MCLK / sample_rate`.

The datasheet adds the constraint `2.8 MHz < DOSR × sample_rate < 6.2 MHz`, and requires DOSR
to be a multiple of 8 to enable Filter A (the highest quality interpolation filter).

Rather than computing valid dividers at runtime (which requires a brute-force search), the
library uses a lookup table of pre-calculated values for the clock rates the ESP32 normally
produces.  The ESP32 I2S peripheral generates MCLK as a fixed multiple of the sample rate —
typically 256× for 16-bit audio.

| MCLK | Sample rate | NDAC | MDAC | DOSR | DOSR × fS |
|------|-------------|------|------|------|-----------|
| 11,289,600 Hz (256×) | 44,100 Hz | 2 | 1 | 128 | 5.645 MHz |
| 12,288,000 Hz (256×) | 48,000 Hz | 2 | 1 | 128 | 6.144 MHz |
|  8,192,000 Hz (256×) | 32,000 Hz | 2 | 1 | 128 | 4.096 MHz |
| 22,579,200 Hz (512×) | 44,100 Hz | 2 | 2 | 128 | 5.645 MHz |
| 24,576,000 Hz (512×) | 48,000 Hz | 2 | 2 | 128 | 6.144 MHz |
| 16,384,000 Hz (512×) | 32,000 Hz | 2 | 2 | 128 | 4.096 MHz |

Setting `mclk_hz = 0` in `tlv320_config_t` tells the library to derive MCLK automatically as
`sample_rate × 256` each time `tlv320_configure()` is called, which is the right choice when
the ESP32 is generating MCLK and the stream sample rate can vary.

If no table entry matches the requested combination, `tlv320_configure()` returns
`ESP_ERR_NOT_SUPPORTED` and logs an error.  To support additional rates, add a row to
`s_clk_table` in `src/tlv320dac3100.c`.

---

## Audio signal path (inside the chip)

After `tlv320_init()` the signal flows like this:

```
I2S data in
   │
   ▼
Left/Right DAC    ← digital volume register (−63.5 dB to 0 dB in 0.5 dB steps)
   │
   ▼
Internal mixer    ← analog volume / routing registers
   │
   ├──► HPL / HPR driver  (0 dB gain, line-out mode)  ──► line-out pins
   │
   └──► Class-D driver    (6 dB fixed gain)            ──► speaker pins
```

Default state after `tlv320_init()`:
- Both DAC channels powered and unmuted, left → left, right → right (no swap)
- All analog volume controls at 0 dB (no attenuation)
- HP pins in line-out mode (bypasses headphone pop-suppression)
- Both output stages powered on

---

## Volume control detail

Volume is controlled in the digital domain, inside the DAC, before the analog output stage.
The register is a signed 8-bit value where `0x00` = 0 dB (full volume), and each step
downward is −0.5 dB.  So `0xFF` = −0.5 dB, `0x81` = −63.5 dB.  The value `0x80` is
reserved and avoided by the library.

`tlv320_set_volume(handle, vol_pct)` maps a 0–100 integer percentage linearly:

| `vol_pct` | Register value | Level |
|-----------|---------------|-------|
| 100 | 0x00 | 0 dB (full volume) |
| 50 | ~0xC0 | ~−32 dB |
| 1 | 0x81 | −63.5 dB (near-silent) |
| 0 | — | DAC mute bits set (hard mute) |

The chip has built-in *soft-step*: when the volume register changes, the hardware ramps to
the new value one step per sample rather than jumping abruptly.  This eliminates clicks on
volume changes.  The library enables 1-sample soft-step during init.

---

## Mute behaviour

`tlv320_set_mute()` sets or clears two mute bits in the DAC volume control register (page 0,
register `0x40`, bits D3 and D2).

This is a *zero-crossing-safe* mute: the chip holds its output and waits until the audio
waveform crosses zero before silencing it, which prevents the click you'd otherwise hear
from an abrupt cut-off.

Muting does **not** change the stored volume level.  Calling `tlv320_set_volume()` with
`vol_pct > 0` will clear the mute bits automatically.

---

## What is not supported

- **PLL** — the chip has an internal fractional PLL that can synthesise any sample rate from
  a fixed MCLK.  This library bypasses it entirely and relies on the ESP32 to supply an
  integer-multiple MCLK.  Adding PLL support means computing P, J, D, R values and writing
  them to the PLL registers before enabling it.
- **ADC / microphone** — the TLV320DAC3100 is a DAC-only part.  (The TLV320AIC3100 adds
  an ADC and is a different chip.)
- **EQ / effects** — the chip has 25 internal DSP processing block configurations.  This
  library uses PRB_P1, the standard stereo-playback block with no effects.
- **Headset detection, GPIO1, interrupts, beep generator, MICBIAS** — see
  [COMPATIBILITY.md](COMPATIBILITY.md) for the full list versus the Adafruit library.
