# Compatibility with the Adafruit TLV320_I2S library

This document compares `tlv320dac3100-esp-idf` with the
[Adafruit_TLV320_I2S](https://github.com/adafruit/Adafruit_TLV320_I2S) Arduino library
by Limor Fried / Adafruit Industries.

---

## Background

The Adafruit library is a full-featured Arduino C++ driver for the TLV320DAC3100.  It
exposes nearly every register in the chip and is designed to run on any Arduino-compatible
board via the `Wire` / `Adafruit_BusIO` abstraction layer.

This ESP-IDF library was produced by an AI-assisted rewrite of the Adafruit library's
concepts into pure C targeting the ESP-IDF framework.  The goal was a narrow, opinionated
API that covers the common stereo-playback use-case with minimal boilerplate.  No source
code was copied; register addresses come from the TI datasheet.

---

## Platform differences

| Aspect | Adafruit library | This library |
|--------|-----------------|-------------|
| Language | C++ (Arduino) | C (ESP-IDF) |
| I2C API | `TwoWire` / `Adafruit_BusIO` | `i2c_master_*` (ESP-IDF ≥ 5.x) |
| Build system | Arduino Library Manager | ESP-IDF component (CMake) |
| Target | Any Arduino platform | ESP32 family |
| Dependency | `Adafruit_BusIO` | None beyond ESP-IDF |

---

## Feature comparison

### Core initialisation

| Feature | Adafruit | This library |
|---------|----------|-------------|
| Software reset | `reset()` | inside `tlv320_init()` |
| I2C device setup | `begin(addr, wire)` | `tlv320_init(handle, cfg)` |
| Page-register tracking | yes (no caching) | yes (cached, skips write if page unchanged) |

### Clock configuration

| Feature | Adafruit | This library |
|---------|----------|-------------|
| CODEC_CLKIN source | `setCodecClockInput()` | fixed to MCLK |
| PLL | `powerPLL()`, `setPLLValues()`, `configurePLL()` | not supported |
| NDAC / MDAC / DOSR | individual setters | lookup table keyed by `(mclk_hz, sample_rate)` |
| Supported sample rates | any (via PLL or manual) | 32 000 / 44 100 / 48 000 Hz |
| MCLK multiples supported | any | 256× and 512× of sample rate |
| Runtime reconfigure | manual (call each setter) | `tlv320_configure(handle, rate, bits)` |

### Audio interface

| Feature | Adafruit | This library |
|---------|----------|-------------|
| I2S slave mode | `setCodecInterface()` | default, always enabled |
| Bit depth | 16 / 20 / 24 / 32 | 16 / 24 |
| Data formats | I2S / DSP / RJF / LJF | I2S only |
| BCLK master mode | `setCodecInterface(…, bclk_out=true)` | not supported |

### DAC data path

| Feature | Adafruit | This library |
|---------|----------|-------------|
| L/R power | `setDACDataPath()` | both on in `tlv320_init()` |
| Channel swap / mix | `setDACDataPath()` | not supported (normal path only) |
| Soft-step | `setDACDataPath()` | 1-sample soft-step, always enabled |
| Processing block | `setDACProcessingBlock()` | PRB_P1, always |

### Volume and mute

| Feature | Adafruit | This library |
|---------|----------|-------------|
| Digital volume | `setChannelVolume(ch, dB)` per channel | `tlv320_set_volume(handle, 0–100)` both channels |
| Volume units | float dB (−63.5 to +24 dB) | integer percent (0–100) |
| Per-channel control | yes | no (L and R always matched) |
| Soft mute | `setDACVolumeControl(mute, …)` | `tlv320_set_mute(handle, bool)` |
| Zero-crossing safe mute | yes | yes |

### Analog output routing

| Feature | Adafruit | This library |
|---------|----------|-------------|
| DAC → mixer routing | `configureAnalogInputs()` | L→HPL+SPK, R→HPR, fixed |
| HPL / HPR analog volume | `setHPLVolume()`, `setHPRVolume()` | 0 dB, fixed |
| Speaker analog volume | `setSPKVolume()` | 0 dB, fixed |
| HPL / HPR driver gain | `configureHPL_PGA()` | 0 dB, fixed |
| Speaker driver gain | `configureSPK_PGA()` | 6 dB, fixed |
| HP line-out mode | `headphoneLineout()` | always enabled |
| Output enable/disable | `configureHeadphoneDriver()`, `enableSpeaker()` | `tlv320_set_output(handle, mask)` |

---

## Features not available in this library

The following Adafruit library features are **not implemented**:

- **PLL** — the Adafruit library can compute and configure the on-chip fractional PLL
  (`configurePLL()`, `setPLLValues()`).  This library requires the ESP32 to supply an
  integer-multiple MCLK and uses the PLL bypass path.
- **Beep generator** — the chip has a built-in sine-wave tone generator
  (`enableBeep()`, `configureBeepTone()`).
- **Headset detection** — insertion/removal detection and button-press events
  (`setHeadsetDetect()`, `getHeadsetStatus()`).
- **Volume ADC** — the VOL/MICDET pin can read a hardware volume potentiometer
  (`configVolADC()`, `readVolADCdB()`).
- **GPIO1 / DIN pins** — configurable as general-purpose I/O or clock outputs.
- **Interrupts** — INT1 / INT2 routing and IRQ flag reading.
- **MICBIAS** — microphone bias voltage output.
- **BCLK_N divider** and **CLKOUT_M** — for deriving clocks from the chip.
- **Over-temperature flag** — `isOvertemperature()`.
- **Short-circuit detection** — `isHeadphoneShorted()`, `isSpeakerShorted()`.
- **20-bit and 32-bit I2S** — only 16 and 24 are supported.
- **Per-channel volume** — both channels are always set to the same level.

---

## Migrating from the Adafruit library

If you are porting an Arduino project to ESP-IDF and were previously using the Adafruit
library, the rough mapping is:

```
Adafruit                                      → This library
─────────────────────────────────────────────────────────────
begin(addr, wire)                             → tlv320_init(&handle, &cfg)
reset()                                       → (called by tlv320_init)
setNDAC / setMDAC / setDOSR                   → tlv320_configure(handle, rate, bits)
setCodecInterface(FORMAT_I2S, LEN_16, …)     → tlv320_configure(handle, rate, bits)
setDACDataPath(true, true, NORMAL, NORMAL)    → (default after tlv320_init)
setDACVolumeControl(false, false)             → tlv320_set_mute(handle, false)
setChannelVolume(LEFT, -6.0)                  → tlv320_set_volume(handle, 50) (approx)
configureHeadphoneDriver(true, true)          → tlv320_set_output(handle, TLV320_OUTPUT_LINEOUT)
enableSpeaker(true)                           → tlv320_set_output(handle, TLV320_OUTPUT_SPEAKER)
```

The key difference is that this library collapses the multi-step Adafruit init sequence
into a single `tlv320_init()` call that configures a sensible default (48 kHz / 16-bit
I2S slave, both outputs on, 0 dB volume).  Call `tlv320_configure()` if the stream
format differs.
