// Shared radio/ranging settings. Both boards MUST be built with the same values.
#pragma once

#include <SPI.h>
#include "dw3000.h"

// Makerfabs ESP32 UWB DW3000 wiring (SPI on VSPI: SCK 18, MISO 19, MOSI 23)
#define UWB_PIN_RST 27
#define UWB_PIN_IRQ 34
#define UWB_PIN_SS 4

// Antenna delay (used for both TX and RX), in DW3000 time units (~15.65 ps each).
// Only the sum over the two boards matters for a pair, so the whole calibration is kept
// on the initiator: +1 unit there lowers the reading by ~4.7 mm.
// Calibrated at 1.00 m (clean line of sight) on 2026-11-11.
#define ANT_DLY_INITIATOR 16353
#define ANT_DLY_RESPONDER 16391

// The exchange (double-sided two-way ranging, three frames):
//   initiator (A)  --poll-->   responder (B)
//   initiator (A)  <--resp--   responder (B)   carries B's timestamps + B's previous results
//   initiator (A)  --final-->  responder (B)   carries A's timestamps + A's own result
// A computes a single-sided distance from poll/resp, B computes one from resp/final, and
// B also computes the double-sided distance, which does not depend on the clock offset.
//
// Timing, in UWB microseconds (~1 us). The 900 us turnarounds are what Makerfabs uses on
// the ESP32; the RX-on delays and timeouts leave room for the 28-byte frames.
#define POLL_TX_TO_RESP_RX_DLY_UUS 600
#define RESP_RX_TIMEOUT_UUS 500
#define POLL_RX_TO_RESP_TX_DLY_UUS 900
#define RESP_RX_TO_FINAL_TX_DLY_UUS 900
#define RESP_TX_TO_FINAL_RX_DLY_UUS 600
#define FINAL_RX_TIMEOUT_UUS 500

#define RNG_PERIOD_MS 25      // time between measurements (40 Hz)
#define RESULT_STALE_MS 1500  // a result older than this is no longer shown

// Frame layout: 10-byte header, payload, 2-byte CRC (added by the chip).
#define ALL_MSG_COMMON_LEN 10
#define ALL_MSG_SN_IDX 2
#define MSG_NO_RESULT INT32_MAX // "no distance available" marker, distances are sent in mm

#define RESP_MSG_POLL_RX_TS_IDX 10
#define RESP_MSG_RESP_TX_TS_IDX 14
#define RESP_MSG_DIST_B_IDX 18  // B's single-sided distance from the previous exchange
#define RESP_MSG_DIST_DS_IDX 22 // double-sided distance from the previous exchange

#define FINAL_MSG_POLL_TX_TS_IDX 10
#define FINAL_MSG_RESP_RX_TS_IDX 14
#define FINAL_MSG_FINAL_TX_TS_IDX 18
#define FINAL_MSG_DIST_A_IDX 22 // A's single-sided distance from this exchange

static uint8_t poll_msg[12] = {0x41, 0x88, 0, 0xCA, 0xDE, 'W', 'A', 'V', 'E', 0xE0};
static uint8_t resp_msg[28] = {0x41, 0x88, 0, 0xCA, 0xDE, 'V', 'E', 'W', 'A', 0xE1};
static uint8_t final_msg[28] = {0x41, 0x88, 0, 0xCA, 0xDE, 'W', 'A', 'V', 'E', 0xE2};

static inline void msg_put_u32(uint8_t *p, uint32_t v)
{
    memcpy(p, &v, sizeof(v));
}

static inline uint32_t msg_get_u32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static inline void msg_put_dist(uint8_t *p, bool valid, float meters)
{
    msg_put_u32(p, (uint32_t)(valid ? (int32_t)lroundf(meters * 1000) : MSG_NO_RESULT));
}

// Returns false if the frame carried no distance.
static inline bool msg_get_dist(const uint8_t *p, float &meters)
{
    int32_t mm = (int32_t)msg_get_u32(p);
    if (mm == MSG_NO_RESULT)
        return false;
    meters = mm / 1000.0f;
    return true;
}
static dwt_config_t uwb_config = {
    5,                // Channel number
    DWT_PLEN_128,     // Preamble length (TX)
    DWT_PAC8,         // Preamble acquisition chunk size (RX)
    9,                // TX preamble code
    9,                // RX preamble code
    1,                // SFD type: non-standard 8 symbol
    DWT_BR_6M8,       // Data rate
    DWT_PHRMODE_STD,  // PHY header mode
    DWT_PHRRATE_STD,  // PHY header rate
    (129 + 8 - 8),    // SFD timeout
    DWT_STS_MODE_OFF, // STS disabled
    DWT_STS_LEN_64,   // STS length
    DWT_PDOA_M0       // PDOA off
};

// Waits until any bit of `mask` is set in the status register. Returns false on timeout,
// so a missed event can never hang the ranging loop.
static inline bool wait_status(uint32_t mask, uint32_t &status, uint32_t timeout_us)
{
    uint32_t t0 = micros();
    while (!((status = dwt_read32bitreg(SYS_STATUS_ID)) & mask))
    {
        if (micros() - t0 > timeout_us)
            return false;
    }
    return true;
}

#define EXCHANGE_WAIT_US 5000 // far longer than any step of the exchange

extern dwt_txconfig_t txconfig_options;
extern SPISettings _fastSPI; // SPI speed the library uses (defined in dw3000_port.cpp)

// Brings the DW3000 up with the shared config. Halts with a message on failure.
static void uwb_init(const char *name, uint16_t ant_dly)
{
    Serial.setTxBufferSize(1024); // status lines must not block the ranging loop
    Serial.begin(115200);
    delay(200);
    Serial.println();
    Serial.println(name);

    spiBegin(UWB_PIN_IRQ, UWB_PIN_RST);
    spiSelect(UWB_PIN_SS);
    delay(2);

    const char *err = nullptr;
    if (!dwt_checkidlerc())
        err = "IDLE FAILED (DW3000 not responding - check SPI/power)";
    else if (dwt_initialise(DWT_DW_INIT) == DWT_ERROR)
        err = "INIT FAILED";
    else if (dwt_configure(&uwb_config))
        err = "CONFIG FAILED (PLL/RX calibration)";

    if (err)
    {
        while (true)
        {
            Serial.println(err);
            delay(2000);
        }
    }

    dwt_configuretxrf(&txconfig_options);
    dwt_setrxantennadelay(ant_dly);
    dwt_settxantennadelay(ant_dly);
    dwt_setleds(DWT_LEDS_ENABLE | DWT_LEDS_INIT_BLINK);
    dwt_setlnapamode(DWT_LNA_ENABLE | DWT_PA_ENABLE);

    Serial.printf("DW3000 OK, dev id 0x%08lX\r\n", (unsigned long)dwt_readdevid());

#ifdef UWB_DIAG // dump the factory calibration words (OTP) at two SPI speeds
    for (int pass = 0; pass < 2; pass++)
    {
        _fastSPI = SPISettings(pass == 0 ? 8000000L : 2000000L, MSBFIRST, SPI_MODE0);
        uint32_t otp[0x21];
        dwt_otpread(0, otp, 0x21);
        Serial.printf("OTP @%s: ldo_lo=%08lX ldo_hi=%08lX part=%08lX lot=%08lX vbat=%08lX temp=%08lX bias=%08lX "
                      "xtrim=%08lX rev=%08lX dgc=%08lX\r\n",
                      pass == 0 ? "8MHz" : "2MHz", (unsigned long)otp[0x04], (unsigned long)otp[0x05],
                      (unsigned long)otp[0x06], (unsigned long)otp[0x07], (unsigned long)otp[0x08],
                      (unsigned long)otp[0x09], (unsigned long)otp[0x0A], (unsigned long)otp[0x1E],
                      (unsigned long)otp[0x1F], (unsigned long)otp[0x20]);
    }
    _fastSPI = SPISettings(8000000L, MSBFIRST, SPI_MODE0);
#endif
}
