#pragma once

/* -----------------------------------------------------------------------
 * TLV320DAC3100 register map
 *
 * The device uses a paged register space.  Write 0x00 to register 0x00
 * to select page 0; write 0x01 to select page 1.  All addresses below
 * are within-page offsets.
 * --------------------------------------------------------------------- */

/* ── Page 0 ─────────────────────────────────────────────────────────── */
#define R_PAGE_SELECT       0x00
#define R_RESET             0x01  /* Write 1 to bit 0 to reset */
#define R_CLOCK_MUX1        0x04  /* D1:D0 = CODEC_CLKIN, D3:D2 = PLL_CLKIN */
#define R_PLL_PR            0x05  /* D7=PLL_en, D6:D4=P, D3:D0=R */
#define R_PLL_J             0x06  /* D5:D0 = J (1–63) */
#define R_PLL_D_MSB         0x07  /* D5:D0 = D[13:8] */
#define R_PLL_D_LSB         0x08  /* D7:D0 = D[7:0] */
#define R_NDAC              0x0B  /* D7=power, D6:D0=NDAC (1–128, 0→128) */
#define R_MDAC              0x0C  /* D7=power, D6:D0=MDAC */
#define R_DOSR_MSB          0x0D  /* D1:D0 = DOSR[9:8] */
#define R_DOSR_LSB          0x0E  /* D7:D0 = DOSR[7:0] */
#define R_CODEC_IF_CTRL1    0x1B  /* D7:D6=fmt, D5:D4=len, D3=BCLK_dir, D2=WCLK_dir */
#define R_DAC_PRB           0x3C  /* D4:D0 = processing block (1–25) */
#define R_DAC_DATAPATH      0x3F  /* D7=L_pwr, D6=R_pwr, D5:D4=L_path, D3:D2=R_path, D1:D0=softstep */
#define R_DAC_VOL_CTRL      0x40  /* D3=L_mute, D2=R_mute, D1:D0=vol_ctrl_mode */
#define R_DAC_LVOL          0x41  /* Signed 8-bit: 0x00=0 dB, 0xFF=−0.5 dB, 0x81=−63.5 dB */
#define R_DAC_RVOL          0x42

/* ── Page 1 ─────────────────────────────────────────────────────────── */
#define R1_HP_DRIVERS       0x1F  /* D7=HPL_en, D6=HPR_en, D4:D3=CM_volt, D2=MUST_BE_1, D1=SCD_mode */
#define R1_SPK_AMP          0x20  /* D7=Class-D_en, D0=short_circuit_status (RO) */
#define R1_HP_POP           0x21  /* Pop removal: D7=wait_pdn, D6:D3=pwr_time, D2:D1=ramp */
#define R1_PGA_RAMP         0x22  /* D6:D4=SPK_wait_time */
#define R1_OUT_ROUTING      0x23  /* D7:D6=L_DAC_route, D3:D2=R_DAC_route (0=none,1=mixer,2=HP) */
#define R1_HPL_VOL          0x24  /* D7=route_to_HPL, D6:D0=attenuation (0=0 dB, 127=−78.3 dB) */
#define R1_HPR_VOL          0x25
#define R1_SPK_VOL          0x26  /* D7=route_to_SPK, D6:D0=attenuation */
#define R1_HPL_DRIVER       0x28  /* D6:D3=gain (0–9 dB), D2=1→unmuted */
#define R1_HPR_DRIVER       0x29
#define R1_SPK_DRIVER       0x2A  /* D4:D3=gain (00=6,01=12,10=18,11=24 dB), D2=1→unmuted */
#define R1_HP_DRIVER_CTRL   0x2C  /* D2=HPL_lineout, D1=HPR_lineout */

/* ── CODEC_IF_CTRL1 field values ─────────────────────────────────────── */
#define IF_FMT_I2S          0x00  /* bits 7:6 */
#define IF_LEN_16           0x00  /* bits 5:4 */
#define IF_LEN_24           0x20
#define IF_BCLK_IN          0x00  /* bit 3: 0 = input (slave) */
#define IF_WCLK_IN          0x00  /* bit 2: 0 = input (slave) */

/* ── DAC_DATAPATH field values ───────────────────────────────────────── */
#define DP_L_PWR            0x80
#define DP_R_PWR            0x40
#define DP_L_NORMAL         0x10  /* left data → left DAC  (bits 5:4 = 01) */
#define DP_R_NORMAL         0x04  /* right data → right DAC (bits 3:2 = 01) */
#define DP_SOFTSTEP_1SAMP   0x00  /* bits 1:0 = 00 */

/* ── HP_DRIVERS values ───────────────────────────────────────────────── */
/* common-mode voltage options for bits D4:D3 */
#define CM_1V35             (0x00 << 3)
#define CM_1V50             (0x01 << 3)
#define CM_1V65             (0x02 << 3)
#define CM_1V80             (0x03 << 3)
#define HP_BIT2_RESERVED    0x04  /* must always be 1 */
