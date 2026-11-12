// Responder (board B): answers each poll, then uses the initiator's final frame to
// compute its own single-sided distance and the double-sided one. See
// include/uwb_config.h for the exchange.
// Hosts the web app on the "Medidor-UWB-2" access point.
#include <Arduino.h>
#include "uwb_config.h"
#include "range_filter.h"
#include "web_app.h"

#define AP_SSID "Medidor-UWB-2"
#define SIDE_ALPHA 0.05f // smoothing of the per-board distances shown for comparison

// After a poll, the next one is RNG_PERIOD_MS away: serve web clients for part of that
// gap, then go back to listening with nothing else running so the reply is on time.
#define WEB_WINDOW_MS (RNG_PERIOD_MS * 6 / 10)
#define UNSYNCED_MS (RNG_PERIOD_MS * 3) // no poll for this long -> serve web while listening

static uint8_t frame_seq_nb = 0;
static uint8_t rx_buffer[sizeof(final_msg)];

#define REARM_MS 1000 // listening this long with nothing received -> restart the receiver
static uint32_t rearm_count = 0;

static uint32_t last_poll_ms = 0;
static bool got_poll = false;

static RangeFilter filter;
static ClockOffsetFilter clock_offset;
static Ema ema_a, ema_b;

// Results of the last complete exchange, sent to the initiator in the next response.
static float last_b = 0, last_ds = 0;
static uint32_t last_result_ms = 0;
static bool have_result = false;

void setup()
{
    uwb_init("UWB RESPONDER (DS-TWR)", ANT_DLY_RESPONDER);
    dwt_setrxaftertxdelay(RESP_TX_TO_FINAL_RX_DLY_UUS);
#ifdef UWB_DIAG
    dwt_configciadiag(DW_CIA_DIAG_LOG_ALL);
#endif
    web_init(AP_SSID, "responder");
    Serial.println("waiting for polls...");
}

// Handles a received frame: if it is a poll, runs the rest of the exchange.
static void handle_frame()
{
    uint32_t frame_len = dwt_read32bitreg(RX_FINFO_ID) & RXFLEN_MASK;
    if (frame_len != sizeof(poll_msg))
        return;

    dwt_readrxdata(rx_buffer, frame_len, 0);
    rx_buffer[ALL_MSG_SN_IDX] = 0;
    if (memcmp(rx_buffer, poll_msg, ALL_MSG_COMMON_LEN) != 0)
        return;

    last_poll_ms = millis();
    got_poll = true;

    uint64_t poll_rx_ts64 = get_rx_timestamp_u64();
    uint32_t poll_rx_ts = (uint32_t)poll_rx_ts64;

    // Delayed TX time has 512 time-unit resolution, hence the >> 8 and the cleared LSB.
    uint32_t resp_tx_time = (poll_rx_ts64 + (POLL_RX_TO_RESP_TX_DLY_UUS * UUS_TO_DWT_TIME)) >> 8;
    dwt_setdelayedtrxtime(resp_tx_time);
    uint32_t resp_tx_ts = (uint32_t)((((uint64_t)(resp_tx_time & 0xFFFFFFFEUL)) << 8) + ANT_DLY_RESPONDER);

    bool fresh = have_result && (millis() - last_result_ms < RESULT_STALE_MS);
    resp_msg[ALL_MSG_SN_IDX] = frame_seq_nb++;
    msg_put_u32(&resp_msg[RESP_MSG_POLL_RX_TS_IDX], poll_rx_ts);
    msg_put_u32(&resp_msg[RESP_MSG_RESP_TX_TS_IDX], resp_tx_ts);
    msg_put_dist(&resp_msg[RESP_MSG_DIST_B_IDX], fresh, last_b);
    msg_put_dist(&resp_msg[RESP_MSG_DIST_DS_IDX], fresh, last_ds);
    dwt_writetxdata(sizeof(resp_msg), resp_msg, 0);
    dwt_writetxfctrl(sizeof(resp_msg), 0, 1);

    // Send the response and listen for the final right after it.
    dwt_setrxtimeout(FINAL_RX_TIMEOUT_UUS);
    if (dwt_starttx(DWT_START_TX_DELAYED | DWT_RESPONSE_EXPECTED) != DWT_SUCCESS)
    {
        // Too late for the programmed TX time; the initiator will time out and retry.
        web_status.fails++;
        return;
    }

    uint32_t status;
    if (!wait_status(SYS_STATUS_RXFCG_BIT_MASK | SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR, status,
                     EXCHANGE_WAIT_US))
    {
        dwt_forcetrxoff(); // radio stuck: back to idle; loop() re-arms the receiver
        status = 0;
    }
    dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_TXFRS_BIT_MASK | SYS_STATUS_RXFCG_BIT_MASK | SYS_STATUS_ALL_RX_TO |
                                         SYS_STATUS_ALL_RX_ERR);

    if (!(status & SYS_STATUS_RXFCG_BIT_MASK))
    {
        web_status.fails++; // final frame missed
        return;
    }

    frame_len = dwt_read32bitreg(RX_FINFO_ID) & RXFLEN_MASK;
    if (frame_len != sizeof(final_msg))
        return;

    dwt_readrxdata(rx_buffer, frame_len, 0);
    rx_buffer[ALL_MSG_SN_IDX] = 0;
    if (memcmp(rx_buffer, final_msg, ALL_MSG_COMMON_LEN) != 0)
        return;

    uint32_t final_rx_ts = dwt_readrxtimestamplo32();
    float offset = clock_offset.push(((float)dwt_readclockoffset()) / (uint32_t)(1 << 26));

    uint32_t poll_tx_ts = msg_get_u32(&rx_buffer[FINAL_MSG_POLL_TX_TS_IDX]);
    uint32_t resp_rx_ts = msg_get_u32(&rx_buffer[FINAL_MSG_RESP_RX_TS_IDX]);
    uint32_t final_tx_ts = msg_get_u32(&rx_buffer[FINAL_MSG_FINAL_TX_TS_IDX]);

    // Round trips (R) and reply times (D), each measured on one board's own clock.
    // 32-bit subtraction handles the timestamp wrap.
    double Ra = (double)(uint32_t)(resp_rx_ts - poll_tx_ts);   // board A
    double Da = (double)(uint32_t)(final_tx_ts - resp_rx_ts);  // board A
    double Rb = (double)(uint32_t)(final_rx_ts - resp_tx_ts);  // board B
    double Db = (double)(uint32_t)(resp_tx_ts - poll_rx_ts);   // board B

    // Double-sided: the clock offset cancels, no estimate needed.
    double tof_ds = (Ra * Rb - Da * Db) / (Ra + Rb + Da + Db);
    float ds = (float)(tof_ds * DWT_TIME_UNITS * SPEED_OF_LIGHT);

    // This board's single-sided distance (resp -> final), with clock offset correction.
    float b = (float)(((Rb - Da * (1.0 - (double)offset)) / 2.0) * DWT_TIME_UNITS * SPEED_OF_LIGHT);

    float a = 0;
    bool a_valid = msg_get_dist(&rx_buffer[FINAL_MSG_DIST_A_IDX], a);

    uint32_t now = millis();
    if (now - web_status.last_ok_ms > RESULT_STALE_MS)
    {
        // Old readings no longer describe where the boards are.
        filter.reset();
        ema_a.reset();
        ema_b.reset();
    }

    last_b = b;
    last_ds = ds;
    last_result_ms = now;
    have_result = true;

    web_status.b = ema_b.push(b, SIDE_ALPHA);
    web_status.b_valid = true;
    web_status.a_valid = a_valid;
    if (a_valid)
        web_status.a = ema_a.push(a, SIDE_ALPHA);
    web_status.dist = ds;
    web_status.avg = filter.push(ds);
    web_status.last_ok_ms = now;
    web_status.valid = true;

#ifdef UWB_DIAG // receiver diagnostics for the final frame: peak minus first-path index
    dwt_rxdiag_t diag;
    dwt_readdiagnostics(&diag);
    float pk_fp = (float)((diag.ipatovPeak >> 21) & 0x3FF) - diag.ipatovFpIndex / 64.0f;
    Serial.printf("diag: ds %6.3f a %6.3f b %6.3f pkfp %5.2f amp %lu\r\n", ds, a_valid ? a : NAN, b, pk_fp,
                  (unsigned long)(diag.ipatovPeak & 0x1FFFFF));
#endif

    web_status.count++;
}

void loop()
{
    dwt_setrxtimeout(0); // listen for a poll with no timeout
    dwt_rxenable(DWT_START_RX_IMMEDIATE);

    uint32_t status;
    uint32_t listen_start = millis();
    while (!((status = dwt_read32bitreg(SYS_STATUS_ID)) & (SYS_STATUS_RXFCG_BIT_MASK | SYS_STATUS_ALL_RX_ERR)))
    {
        // Nothing heard for a while: restart the receiver in case it silently dropped out.
        if (millis() - listen_start > REARM_MS)
        {
            dwt_forcetrxoff();
            rearm_count++;
            return;
        }

        // No initiator in sight: keep the web app alive while listening.
        if (!got_poll || millis() - last_poll_ms > UNSYNCED_MS)
        {
            web_service();
            delay(1);
        }
    }

    if (!(status & SYS_STATUS_RXFCG_BIT_MASK))
    {
        dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_ALL_RX_ERR);
        return;
    }

    dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_RXFCG_BIT_MASK);
    handle_frame();

    if (got_poll && millis() - last_poll_ms < WEB_WINDOW_MS)
        web_serve_until(last_poll_ms, WEB_WINDOW_MS);
}
