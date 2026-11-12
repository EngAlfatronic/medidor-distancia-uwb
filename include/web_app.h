// Wi-Fi access point + web server for the app in data/ (LittleFS), and the status
// line on the serial port that the app can read over USB. Used by both boards.
#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>

#define AP_PASS "medidor123" // min 8 chars; app at http://192.168.4.1

// The access point is switched off if nobody joins it within this time after boot.
// Once someone has joined, it stays on until the board is powered off.
#define WIFI_IDLE_OFF_MS 60000

#define SERIAL_REPORT_MS 100 // status line on the serial port, for the app's USB mode

struct WebStatus
{
    const char *role;
    bool valid;          // at least one measurement so far
    float dist;          // last combined measurement, meters
    float avg;           // filtered combined measurement, meters
    bool a_valid;        // board A (initiator) single-sided distance available
    float a;             // smoothed, meters
    bool b_valid;        // board B (responder) single-sided distance available
    float b;             // smoothed, meters
    uint32_t last_ok_ms; // millis() of the last measurement
    uint32_t count;      // measurements so far
    uint32_t fails;      // failed exchanges
};

static WebStatus web_status = {"", false, 0, 0, false, 0, false, 0, 0, 0, 0};
static WebServer server(80);

static bool wifi_on = false;
static bool wifi_keep = false; // someone joined the AP at least once
static uint32_t wifi_start_ms = 0;
static uint32_t wifi_check_ms = 0;
static uint32_t serial_report_ms = 0;

// Same JSON for the HTTP API and the serial status line.
static void status_json(char *json, size_t size)
{
    char a[16] = "null", b[16] = "null";
    if (web_status.a_valid)
        snprintf(a, sizeof(a), "%.3f", web_status.a);
    if (web_status.b_valid)
        snprintf(b, sizeof(b), "%.3f", web_status.b);

    snprintf(json, size,
             "{\"role\":\"%s\",\"valid\":%s,\"dist\":%.3f,\"avg\":%.3f,\"a\":%s,\"b\":%s,"
             "\"age\":%lu,\"count\":%lu,\"fails\":%lu,\"wifi\":%s}",
             web_status.role, web_status.valid ? "true" : "false", web_status.dist, web_status.avg, a, b,
             (unsigned long)(millis() - web_status.last_ok_ms), (unsigned long)web_status.count,
             (unsigned long)web_status.fails, wifi_on ? "true" : "false");
}

static void send_file(const char *path, const char *type)
{
    File f = LittleFS.open(path, "r");
    if (!f)
    {
        server.send(404, "text/plain", "file not found - upload the filesystem (pio run -t uploadfs)");
        return;
    }
    // Other ESP32 projects also live at 192.168.4.1; never let the browser reuse their cached files.
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.streamFile(f, type);
    f.close();
}

static void handle_status()
{
    char json[256];
    status_json(json, sizeof(json));
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", json);
}

static void web_init(const char *ssid, const char *role)
{
    web_status.role = role;

#ifdef UWB_NO_WIFI // diagnostic build: radio off, serial only
    WiFi.mode(WIFI_OFF);
    Serial.println("Wi-Fi disabled (UWB_NO_WIFI)");
    return;
#endif

    if (!LittleFS.begin(true))
        Serial.println("LittleFS mount failed");

    WiFi.mode(WIFI_AP);
    WiFi.softAP(ssid, AP_PASS);
    Serial.printf("Wi-Fi AP \"%s\" (pass \"%s\") -> http://%s\r\n", ssid, AP_PASS, WiFi.softAPIP().toString().c_str());

    server.on("/", []() { send_file("/index.html", "text/html"); });
    server.on("/medidor.css", []() { send_file("/medidor.css", "text/css"); });
    server.on("/medidor.js", []() { send_file("/medidor.js", "application/javascript"); });
    server.on("/manifest.webmanifest", []() { send_file("/manifest.webmanifest", "application/manifest+json"); });
    server.on("/icon-192.png", []() { send_file("/icon-192.png", "image/png"); });
    server.on("/icon-512.png", []() { send_file("/icon-512.png", "image/png"); });
    server.on("/api/status", handle_status);
    server.onNotFound([]() { server.send(404, "text/plain", "not found"); });
    server.begin();

    wifi_on = true;
    wifi_start_ms = millis();
}

// Call often: serves web clients, applies the Wi-Fi idle-off rule and prints the
// serial status line. Takes well under a millisecond when there is nothing to do.
static void web_service()
{
    uint32_t now = millis();

    if (wifi_on)
    {
        if (!wifi_keep && now - wifi_check_ms >= 500)
        {
            wifi_check_ms = now;
            if (WiFi.softAPgetStationNum() > 0)
            {
                wifi_keep = true;
                Serial.println("Wi-Fi: client joined, staying on");
            }
            else if (now - wifi_start_ms >= WIFI_IDLE_OFF_MS)
            {
                server.stop();
                WiFi.softAPdisconnect(true);
                WiFi.mode(WIFI_OFF);
                wifi_on = false;
                Serial.println("Wi-Fi: nobody joined, switched off (reset the board to turn it back on)");
            }
        }

        if (wifi_on)
            server.handleClient();
    }

#ifndef UWB_DIAG // diagnostic builds print one line per measurement instead
    if (now - serial_report_ms >= SERIAL_REPORT_MS)
    {
        serial_report_ms = now;
        char json[256];
        status_json(json, sizeof(json));
        Serial.println(json);
    }
#endif
}

// Services web/serial until `until_ms` have passed since `since`.
static void web_serve_until(uint32_t since, uint32_t until_ms)
{
    do
    {
        web_service();
        delay(1);
    } while (millis() - since < until_ms);
}
