// Seeed ReSpeaker XVF3800 + XIAO ESP32-S3 (SKU 114993700). Source of truth and reasoning:
// docs/hardware-bringup.md (sections 1, 3.4, 4).
//
// XVF3800 firmware: respeaker_xvf3800_i2s_slave_v1.0.9_16k.bin
//   -> XVF3800 is I2S *slave*, the ESP32 is I2S *master*, 16 kHz, 32-bit stereo, Philips.
// Fallback (i2s_master_v1.0.9_48k): ESP32 becomes slave at 48 kHz; flip GLS_I2S_ESP_IS_MASTER
// and GLS_I2S_SAMPLE_RATE.
#pragma once

// ---- I2S (one controller, TX+RX paired so they share BCLK/WS) ---------------------------
#define GLS_I2S_PORT            0
#define GLS_I2S_ESP_IS_MASTER   1
#define GLS_I2S_MCLK_GPIO       -1  /* not driven in any Seeed example (I2S_GPIO_UNUSED) */
#define GLS_I2S_BCLK_GPIO       8   /* D9 */
#define GLS_I2S_WS_GPIO         7   /* D8, LRCLK */
#define GLS_I2S_DOUT_GPIO       44  /* ESP32 -> XVF3800 DIN (AEC far-end ref) AND AIC3104 DIN (speaker) */
#define GLS_I2S_DIN_GPIO        43  /* XVF3800 processed output -> ESP32 */

#define GLS_I2S_SAMPLE_RATE     16000
#define GLS_I2S_SLOT_BITS       32  /* keep 32-bit slots both ways (BCLK = 64*fs = 1.024 MHz) */
#define GLS_I2S_CHANNELS        2   /* write identical audio to L and R; mic: ch0 = conference/comms beam */

// GPIO43/44 are UART0 TX/RX: the console must stay on USB-Serial/JTAG
// (CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y, no secondary UART console).

// ---- I2C (control) ----------------------------------------------------------------------
#define GLS_I2C_PORT            0
#define GLS_I2C_SDA_GPIO        5   /* D4 */
#define GLS_I2C_SCL_GPIO        6   /* D5 */
#define GLS_I2C_FREQ_HZ         100000
#define GLS_XVF3800_I2C_ADDR    0x2C
#define GLS_AIC3104_I2C_ADDR    0x18 /* codec; XVF3800 also masters it: avoid touching */

// ---- Misc on the XIAO module --------------------------------------------------------------
#define GLS_BOOT_BUTTON_GPIO    0
#define GLS_USER_LED_GPIO       21  /* orange user LED (polarity: verify, believed active-low) */

// ---- Audio processing -------------------------------------------------------------------
// The XVF3800 does AEC/beamforming/NS in hardware with the reference wired from GPIO44.
// Do NOT add software AEC: use esp_capture_new_audio_dev_src(), not
// esp_capture_new_audio_aec_src() as esp-webrtc-solution's openai_demo does for Korvo-2.
// Do volume digitally before I2S (keeps the echo path constant for the AEC).
#define GLS_SOFTWARE_AEC        0
