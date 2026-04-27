# tlv320dac3100-esp-idf

ESP-IDF component for the TI TLV320DAC3100 stereo audio DAC.
Lets an ESP32 play audio over I2S and control volume, mute, and output selection over I2C — in plain C, no Arduino, no ESP-ADF required.

---

## A note from a human

Hi, this is Kevin, a real human. I'm writing this section of text, but I have NOT written much else in this repository. This was almost entirely generated using Claude (there is another disclosure about this below - but even this was written by Claude). That said, the general idea here was that I want to use this TLV320 DAC in a project using ESP-IDF and I wanted a library written in C that I could easily drop into a IDF project with minimal extra steps. The Adafruit library seemed like a simple place to start from, so I fed that along with various TLV320 documentation to Claude and this repository is the result of that. For these reasons, I wanted to call out these things:

1. **Don't be fooled by convincing words from Claude e.g. documentation below, at this point this library is almost entirely untested** - _ASSUME_ it will not work (I'll update this readme once I've fully implemented it)
2. More of #1, but comments in code are unchecked / unverified - as I go through a full implementation with this "library" I'll update the docs as I get a better understanding of the TLV320, for now this is just a starting point - I'll update this section of the docs to reflect my real world, human lived experience once I have more

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

## What this does

The TLV320DAC3100 takes a digital audio stream (I2S) from the ESP32 and turns it into
analog audio.  It has two output paths you can use at the same time: a **Class-D amplifier**
that can drive a small speaker directly, and a **line-out** (the HP pins, configured here
in line-out mode) suitable for feeding an external amplifier or audio jack.

This library handles the I2C control side — setting up the chip, adjusting volume, muting,
and switching outputs.  You set up the I2S audio stream separately using the standard ESP-IDF
I2S driver, as shown in the examples below.

---

## Installation

Clone or copy this repository somewhere on your machine, then tell ESP-IDF where to find it.

**1. Register the component directory**

In your project's root `CMakeLists.txt`, add this line before the `include(...)` call:

```cmake
list(APPEND EXTRA_COMPONENT_DIRS "/path/to/tlv320dac3100-esp-idf")
```

Or, if you prefer, place the folder inside your project's `components/` directory — ESP-IDF
will find it automatically with no CMake changes needed.

**2. Add the dependency**

In your app or component's `CMakeLists.txt`:

```cmake
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES tlv320dac3100-esp-idf
)
```

**3. Include the header**

```c
#include "tlv320dac3100.h"
```

---

## Wiring

The chip needs two connections to the ESP32: I2C for control commands, and I2S for audio data.
The pin numbers below are examples — pin flexibility varies by signal and chip variant, so
read the notes before choosing your own.

| Function | ESP32 GPIO (example) | Notes |
|----------|---------------------|-------|
| I2C SDA | 8 | Most bidirectional GPIOs work; needs 4.7 kΩ pull-up to 3.3 V |
| I2C SCL | 9 | Most bidirectional GPIOs work; needs 4.7 kΩ pull-up to 3.3 V |
| I2S MCLK | 0 | **Restricted — see note below** |
| I2S BCLK | 5 | Most GPIOs work via the GPIO matrix |
| I2S LRCK | 6 | Most GPIOs work via the GPIO matrix |
| I2S DOUT | 7 | Most GPIOs work via the GPIO matrix |

The TLV320 is the I2S slave — the ESP32 generates all clocks and sends the audio.

**MCLK pin constraint:** On the original ESP32, MCLK output is only available on GPIO 0, 1,
or 3 — you cannot route it to an arbitrary pin.  GPIO 0 is the most common choice, but it
is also a strapping pin that affects boot mode, so check your board schematic.  On the
ESP32-S3 and other newer variants, MCLK is more flexible; check the GPIO matrix table in
your chip's datasheet to confirm.

**Input-only pins:** On the original ESP32, GPIO 34–39 are input-only and cannot be used
for any of the output signals above.

---

## Examples

### Example 1: Minimal setup

Shows the skeleton — I2C bus, codec, and I2S stream initialised, then a single write.
This is the starting point; in a real project you replace the single write with a loop
(see Example 2).

```c
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "tlv320dac3100.h"

// Change these to match your board.
// MCLK is restricted on the original ESP32 — only GPIO 0, 1, or 3 are valid.
// On ESP32-S3 and newer, MCLK can go on more pins; check your chip's datasheet.
#define PIN_I2C_SDA   GPIO_NUM_8
#define PIN_I2C_SCL   GPIO_NUM_9
#define PIN_I2S_MCLK  GPIO_NUM_0   // original ESP32: must be 0, 1, or 3
#define PIN_I2S_BCLK  GPIO_NUM_5
#define PIN_I2S_LRCK  GPIO_NUM_6
#define PIN_I2S_DOUT  GPIO_NUM_7

void app_main(void)
{
    // 1. Create the I2C bus the codec hangs off.
    //    You can share this bus with other I2C devices (sensors, displays, etc.)
    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port          = I2C_NUM_0,
        .sda_io_num        = PIN_I2C_SDA,
        .scl_io_num        = PIN_I2C_SCL,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
    };
    i2c_master_bus_handle_t i2c_bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_cfg, &i2c_bus));

    // 2. Initialise the codec.
    //    This resets the chip, sets up clocks for 48 kHz / 16-bit, and
    //    powers up both the line-out and the speaker amplifier.
    tlv320_config_t codec_cfg = {
        .i2c_bus  = i2c_bus,
        .i2c_addr = TLV320_I2C_ADDR_DEFAULT,  // 0x18 when ADDR pin is low
        .mclk_hz  = 0,                         // 0 = auto (256 × sample rate)
    };
    tlv320_handle_t codec;
    ESP_ERROR_CHECK(tlv320_init(&codec, &codec_cfg));

    // 3. Set the initial volume (0–100)
    tlv320_set_volume(codec, 75);

    // 4. Create the I2S transmit channel for audio data.
    i2s_chan_handle_t i2s_tx;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &i2s_tx, NULL));

    i2s_std_config_t i2s_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(48000),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                     I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = PIN_I2S_MCLK,
            .bclk = PIN_I2S_BCLK,
            .ws   = PIN_I2S_LRCK,
            .dout = PIN_I2S_DOUT,
            .din  = I2S_GPIO_UNUSED,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_tx, &i2s_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(i2s_tx));

    // 5. If your stream's sample rate or bit depth differs from the default
    //    (48 kHz / 16-bit), tell the codec here.  Otherwise this line is optional.
    tlv320_configure(codec, 48000, 16);

    // 6. Write audio.
    //    buf must contain raw PCM samples — the uncompressed numbers that
    //    represent the audio waveform, 16 bits per sample, stereo interleaved
    //    (left sample then right sample, repeating): [L0, R0, L1, R1, ...]
    //    See "Where does the audio data come from?" below for practical options.
    int16_t buf[512];
    // ... fill buf with your audio data ...
    size_t written;
    i2s_channel_write(i2s_tx, buf, sizeof(buf), &written, portMAX_DELAY);
}
```

---

### Example 2: Continuous playback

In a real project the audio write runs in its own FreeRTOS task in an infinite loop.
Setup happens once; the task then runs forever, feeding audio to the chip as fast as
it can accept it.  `i2s_channel_write` naturally throttles to the audio sample rate —
it blocks internally until the I2S hardware is ready for more data, so there's no
need for sleeps or timers.

The example below plays a block of PCM data (e.g. a sound clip loaded from flash)
on repeat.  Swap `audio_data` / `audio_data_len` for whatever source you have.

```c
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "tlv320dac3100.h"

#define PIN_I2C_SDA   GPIO_NUM_8
#define PIN_I2C_SCL   GPIO_NUM_9
#define PIN_I2S_MCLK  GPIO_NUM_0
#define PIN_I2S_BCLK  GPIO_NUM_5
#define PIN_I2S_LRCK  GPIO_NUM_6
#define PIN_I2S_DOUT  GPIO_NUM_7

#define AUDIO_BUF_BYTES 2048   // how many bytes to hand to I2S per iteration

// Your PCM audio data.  One way to get this: convert a WAV file to a raw C array
// using a tool like `xxd -i audio.raw > audio_data.c`, then declare it here.
extern const uint8_t audio_data[];
extern const size_t  audio_data_len;

// The audio task receives both handles so it can play audio and react to errors.
typedef struct {
    i2s_chan_handle_t i2s_tx;
    tlv320_handle_t   codec;
} audio_task_args_t;

static void audio_task(void *arg)
{
    audio_task_args_t *a = (audio_task_args_t *)arg;
    static const char *TAG = "audio_task";

    while (true) {
        // Walk through the audio data in chunks, looping back to the start.
        const uint8_t *p   = audio_data;
        size_t         rem = audio_data_len;

        while (rem > 0) {
            size_t chunk = (rem > AUDIO_BUF_BYTES) ? AUDIO_BUF_BYTES : rem;
            size_t written = 0;

            // i2s_channel_write blocks until the I2S hardware accepts the data —
            // this is what keeps playback at the correct speed without any sleep.
            esp_err_t err = i2s_channel_write(a->i2s_tx, p, chunk,
                                              &written, pdMS_TO_TICKS(1000));
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "i2s_channel_write failed: %s", esp_err_to_name(err));
                vTaskDelay(pdMS_TO_TICKS(100));  // brief pause before retrying
                continue;
            }

            p   += written;
            rem -= written;
        }
        // reached the end of the clip — loop back to the start automatically
    }
}

void app_main(void)
{
    // ── I2C bus ──────────────────────────────────────────────────────────
    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port          = I2C_NUM_0,
        .sda_io_num        = PIN_I2C_SDA,
        .scl_io_num        = PIN_I2C_SCL,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
    };
    i2c_master_bus_handle_t i2c_bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_cfg, &i2c_bus));

    // ── Codec ─────────────────────────────────────────────────────────────
    tlv320_config_t codec_cfg = {
        .i2c_bus  = i2c_bus,
        .i2c_addr = TLV320_I2C_ADDR_DEFAULT,
        .mclk_hz  = 0,
    };
    tlv320_handle_t codec;
    ESP_ERROR_CHECK(tlv320_init(&codec, &codec_cfg));
    tlv320_set_volume(codec, 75);

    // ── I2S stream ────────────────────────────────────────────────────────
    i2s_chan_handle_t i2s_tx;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &i2s_tx, NULL));

    i2s_std_config_t i2s_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(48000),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                     I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = PIN_I2S_MCLK,
            .bclk = PIN_I2S_BCLK,
            .ws   = PIN_I2S_LRCK,
            .dout = PIN_I2S_DOUT,
            .din  = I2S_GPIO_UNUSED,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_tx, &i2s_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(i2s_tx));
    tlv320_configure(codec, 48000, 16);

    // ── Start the audio task ──────────────────────────────────────────────
    // A dedicated FreeRTOS task handles writing — it runs forever while
    // app_main (and any other tasks you create) continue in parallel.
    static audio_task_args_t task_args;   // static so it outlives app_main's stack
    task_args.i2s_tx = i2s_tx;
    task_args.codec  = codec;

    xTaskCreate(audio_task, "audio", 4096, &task_args, 5, NULL);

    // app_main can now do other work — button polling, Wi-Fi, display, etc.
    // The audio task runs in the background continuously.
}
```

---

## Where does the audio data come from?

The `i2s_channel_write` call above expects raw **PCM** audio — PCM stands for
*Pulse-Code Modulation*, which is just the plain uncompressed numbers that represent
a sound wave.  Think of it as the ESP32 equivalent of a WAV file without the file header.
MP3, AAC, and similar formats are compressed and need to be decoded into PCM first before
you can send them to the I2S bus.

Here are the most common ways to get PCM data on an ESP32:

**Stored audio (simple, good for short clips)**
Encode your audio as a raw PCM file (e.g. using Audacity: File → Export → Other formats → RAW)
and embed it in your firmware as a C array or store it in flash using ESP-IDF's SPIFFS or
LittleFS component.  Then read from the array/file in a loop and write to `i2s_channel_write`.

**HTTP audio stream**
Fetch audio over Wi-Fi using the `esp_http_client` API, buffer the incoming data, and
feed it to a decoder or directly to I2S if the source is already raw PCM.  For MP3 streams
you will need a decoder library (e.g. the `minimp3` or `helix-mp3` libraries that are
commonly used with ESP-IDF).

**ESP-ADF (Espressif Audio Development Framework)**
[ESP-ADF](https://github.com/espressif/esp-adf) is Espressif's full audio framework — it
handles HTTP streams, decoders (MP3, AAC, FLAC, etc.), pipelines, and codec drivers all in
one.  If you are building a full-featured audio player or internet radio, ESP-ADF is the
higher-level starting point and will save a lot of work.  This library (`tlv320dac3100-esp-idf`)
can be used alongside ESP-ADF: use ESP-ADF for the pipeline and decoder, and this library
to configure and control the TLV320 DAC over I2C.

**Bluetooth A2DP**
The `esp-idf-a2dp-sink` example (or similar community libraries) can receive audio over
Bluetooth and expose it as a PCM callback, which you then pass to `i2s_channel_write`.

---

## Controlling volume and outputs from GPIO buttons

A common pattern is to wire a few buttons to GPIO pins and use them to adjust volume,
mute, or switch between the speaker and line-out.  Here's a simple polling example:

```c
#include "driver/gpio.h"
#include "tlv320dac3100.h"

// Buttons wired between the GPIO pin and GND (active-low)
#define PIN_VOL_UP    GPIO_NUM_10
#define PIN_VOL_DOWN  GPIO_NUM_11
#define PIN_MUTE      GPIO_NUM_12
#define PIN_OUTPUT    GPIO_NUM_13   // cycle through speaker / line-out / both

static int  volume = 75;
static bool muted  = false;
static tlv320_output_t output_mode = TLV320_OUTPUT_BOTH;

void buttons_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = BIT64(PIN_VOL_UP) | BIT64(PIN_VOL_DOWN)
                      | BIT64(PIN_MUTE)   | BIT64(PIN_OUTPUT),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
}

void buttons_poll(tlv320_handle_t codec)
{
    if (!gpio_get_level(PIN_VOL_UP)) {
        volume = (volume + 10 > 100) ? 100 : volume + 10;
        tlv320_set_volume(codec, volume);
    }

    if (!gpio_get_level(PIN_VOL_DOWN)) {
        volume = (volume - 10 < 0) ? 0 : volume - 10;
        tlv320_set_volume(codec, volume);
    }

    if (!gpio_get_level(PIN_MUTE)) {
        muted = !muted;
        tlv320_set_mute(codec, muted);
    }

    if (!gpio_get_level(PIN_OUTPUT)) {
        // Cycle: BOTH → SPEAKER → LINEOUT → BOTH
        if      (output_mode == TLV320_OUTPUT_BOTH)    output_mode = TLV320_OUTPUT_SPEAKER;
        else if (output_mode == TLV320_OUTPUT_SPEAKER) output_mode = TLV320_OUTPUT_LINEOUT;
        else                                            output_mode = TLV320_OUTPUT_BOTH;
        tlv320_set_output(codec, output_mode);
    }
}
```

Add debouncing (or use GPIO interrupts) in a real project — the example above is kept
short to show the library calls clearly.

---

## API overview

| Function | What it does |
|----------|-------------|
| `tlv320_init(handle, cfg)` | Reset the chip, configure clocks, power up both outputs. Call once at startup. |
| `tlv320_deinit(handle)` | Mute, power down outputs, free resources. |
| `tlv320_configure(handle, sample_rate, bits)` | Update the codec when the stream format changes (e.g. switching from 44.1 kHz to 48 kHz). |
| `tlv320_set_volume(handle, 0–100)` | Set volume as a percentage. 0 = muted, 100 = full. |
| `tlv320_set_mute(handle, bool)` | Mute or unmute without changing the stored volume level. |
| `tlv320_set_output(handle, output)` | Choose which outputs are active: `TLV320_OUTPUT_SPEAKER`, `TLV320_OUTPUT_LINEOUT`, or `TLV320_OUTPUT_BOTH`. |

Supported sample rates: **32 000, 44 100, 48 000 Hz**.  Supported bit depths: **16 and 24**.

---

## Further reading

- [COMPATIBILITY.md](COMPATIBILITY.md) — how this library compares to the Adafruit Arduino library, feature by feature, and how to migrate from it.
- [TECHNICAL.md](TECHNICAL.md) — internals: how the paged register space works, clock divider maths, the full clock table, audio signal path, and volume register detail.
