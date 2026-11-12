// Initiator (board A): starts each exchange with a poll, computes its own single-sided
// distance from the response, then sends the final frame so the responder can compute
// its own distance and the double-sided one. See include/uwb_config.h for the exchange.
// Hosts the web app on the "Medidor-UWB" access point.
#include <Arduino.h>
#include "uwb_config.h"
#include "range_filter.h"
#include "web_app.h"

#define AP_SSID "Medidor-UWB"
#define SIDE_ALPHA 0.05f // smoothing of the per-board distances shown for comparison

static uint8_t frame_seq_nb = 0;
static uint8_t rx_buffer[sizeof(resp_msg)];

static RangeFilter filter;
static ClockOffsetFilter clock_offset;
static Ema ema_a, ema_b;
static uint32_t final_late = 0;

#ifdef UWB_DIAG
static float diag_fp;
static uint16_t diag_pk;
static uint32_t diag_amp;
#endif

struct Exchange
{
    float a;       // this board's single-sided distance, meters
    bool b_valid;  // responder's results (from its previous exchange) present
    float b;       // responder's single-sided distance
    bool ds_valid;
    float ds;      // double-sided distance
};

// Runs one ranging exchange. Returns false if the responder did not answer.
static bool range_once(Exchange &ex)
{
    poll_msg[ALL_MSG_SN_IDX] = frame_seq_nb++;
    dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_TXFRS_BIT_MASK);
    dwt_writetxdata(sizeof(poll_msg), poll_msg, 0);
    dwt_writetxfctrl(sizeof(poll_msg), 0, 1);
    dwt_starttx(DWT_START_TX_IMMEDIATE | DWT_RESPONSE_EXPECTED);

    uint32_t status;
    if (!wait_status(SYS_STATUS_RXFCG_BIT_MASK | SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR, status,
                     EXCHANGE_WAIT_US))
    {
        dwt_forcetrxoff(); // radio stuck: back to idle so the next poll starts clean
        return false;
    }

    if (!(status & SYS_STATUS_RXFCG_BIT_MASK))
    {
        dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR);
        return false;
    }

    // Clear RX good and the poll's TX-done flag (the final below waits on TX-done again).
    dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_RXFCG_BIT_MASK | SYS_STATUS_TXFRS_BIT_MASK);

    uint32_t frame_len = dwt_read32bitreg(RX_FINFO_ID) & RXFLEN_MASK;
    if (frame_len != sizeof(resp_msg))
        return false;

    dwt_readrxdata(rx_buffer, frame_len, 0);
    rx_buffer[ALL_MSG_SN_IDX] = 0;
    if (memcmp(rx_buffer, resp_msg, ALL_MSG_COMMON_LEN) != 0)
        return false;

    uint32_t poll_tx_ts = dwt_readtxtimestamplo32();
    uint64_t resp_rx_ts64 = get_rx_timestamp_u64();
    uint32_t resp_rx_ts = (uint32_t)resp_rx_ts64;

    // Schedule the final. Delayed TX time has 512 time-unit resolution, hence the >> 8
    // and the cleared LSB; the timestamp is the programmed time plus the antenna delay.
    uint32_t final_tx_time = (resp_rx_ts64 + (RESP_RX_TO_FINAL_TX_DLY_UUS * UUS_TO_DWT_TIME)) >> 8;
    dwt_setdelayedtrxtime(final_tx_time);
    uint32_t final_tx_ts = (uint32_t)((((uint64_t)(final_tx_time & 0xFFFFFFFEUL)) << 8) + ANT_DLY_INITIATOR);

    // This board's single-sided distance: round trip here minus the responder's reply
    // time, the latter corrected for the clock offset between the two crystals.
    float offset = clock_offset.push(((float)dwt_readclockoffset()) / (uint32_t)(1 << 26));
    uint32_t poll_rx_ts = msg_get_u32(&rx_buffer[RESP_MSG_POLL_RX_TS_IDX]);
    uint32_t resp_tx_ts = msg_get_u32(&rx_buffer[RESP_MSG_RESP_TX_TS_IDX]);
    int32_t rtd_init = resp_rx_ts - poll_tx_ts;
    int32_t rtd_resp = resp_tx_ts - poll_rx_ts;
    ex.a = (float)(((rtd_init - rtd_resp * (1.0 - (double)offset)) / 2.0) * DWT_TIME_UNITS * SPEED_OF_LIGHT);

    final_msg[ALL_MSG_SN_IDX] = frame_seq_nb++;
    msg_put_u32(&final_msg[FINAL_MSG_POLL_TX_TS_IDX], poll_tx_ts);
    msg_put_u32(&final_msg[FINAL_MSG_RESP_RX_TS_IDX], resp_rx_ts);
    msg_put_u32(&final_msg[FINAL_MSG_FINAL_TX_TS_IDX], final_tx_ts);
    msg_put_dist(&final_msg[FINAL_MSG_DIST_A_IDX], true, ex.a);
    dwt_writetxdata(sizeof(final_msg), final_msg, 0);
    dwt_writetxfctrl(sizeof(final_msg), 0, 1);

    if (dwt_starttx(DWT_START_TX_DELAYED) == DWT_SUCCESS)
    {
        if (!wait_status(SYS_STATUS_TXFRS_BIT_MASK, status, EXCHANGE_WAIT_US))
            dwt_forcetrxoff();
        dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_TXFRS_BIT_MASK);
    }
    else
    {
        final_late++; // too late for the programmed time; the responder just misses this one
    }

#ifdef UWB_DIAG // receiver diagnostics for the response frame, appended to the serial line
    dwt_rxdiag_t diag;
    dwt_readdiagnostics(&diag);
    diag_fp = diag.ipatovFpIndex / 64.0f;      // first path index (10.6 fixed point)
    diag_pk = (diag.ipatovPeak >> 21) & 0x3FF; // index of the strongest CIR sample
    diag_amp = diag.ipatovPeak & 0x1FFFFF;     // its amplitude
#endif

    ex.b_valid = msg_get_dist(&rx_buffer[RESP_MSG_DIST_B_IDX], ex.b);
    ex.ds_valid = msg_get_dist(&rx_buffer[RESP_MSG_DIST_DS_IDX], ex.ds);
    return true;
}

void setup()
{
    uwb_init("UWB INITIATOR (DS-TWR)", ANT_DLY_INITIATOR);
    dwt_setrxaftertxdelay(POLL_TX_TO_RESP_RX_DLY_UUS);
    dwt_setrxtimeout(RESP_RX_TIMEOUT_UUS);
#ifdef UWB_DIAG
    dwt_configciadiag(DW_CIA_DIAG_LOG_ALL);
#endif
    web_init(AP_SSID, "initiator");
}

void loop()
{
    uint32_t start = millis();

    Exchange ex;
    if (range_once(ex))
    {
        if (millis() - web_status.last_ok_ms > RESULT_STALE_MS)
        {
            // Old readings no longer describe where the boards are.
            filter.reset();
            ema_a.reset();
            ema_b.reset();
        }

        web_status.a = ema_a.push(ex.a, SIDE_ALPHA);
        web_status.a_valid = true;
        web_status.b_valid = ex.b_valid;
        if (ex.b_valid)
            web_status.b = ema_b.push(ex.b, SIDE_ALPHA);
        else
            ema_b.reset();

        // Main value: the double-sided distance; until the responder has one, this board's own.
        web_status.dist = ex.ds_valid ? ex.ds : ex.a;
        web_status.avg = filter.push(web_status.dist);
        web_status.last_ok_ms = millis();
        web_status.count++;
        web_status.valid = true;

#ifdef UWB_DIAG // one line per measurement; normal builds print the status JSON instead
        Serial.printf("dist: %6.3f m   avg: %6.3f m   fails: %lu   a: %6.3f b: %6.3f ds: %6.3f   ppm: %6.2f late: %lu",
                      web_status.dist, web_status.avg, (unsigned long)web_status.fails, ex.a,
                      ex.b_valid ? ex.b : NAN, ex.ds_valid ? ex.ds : NAN, clock_offset.filt * 1e6f,
                      (unsigned long)final_late);
        Serial.printf("   fp: %7.2f pk: %u amp: %lu", diag_fp, diag_pk, (unsigned long)diag_amp);
        Serial.print("\r\n");
#endif
    }
    else
    {
        web_status.fails++;
#ifdef UWB_DIAG
        if (web_status.fails % 40 == 1)
            Serial.printf("no response (fails: %lu) - is the responder powered?\r\n", (unsigned long)web_status.fails);
#endif
    }

    // Serve web clients while waiting for the next measurement.
    web_serve_until(start, RNG_PERIOD_MS);
}
