// ═══════════════════════════════════════════════════════════════
//  ota.h — OTA update via GitHub Releases
// ═══════════════════════════════════════════════════════════════
#pragma once
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include <ArduinoJson.h>

// ─── KONFIGURASI OTA (WAJIB diganti) ───
#define OTA_REPO         "sintink/S3_speaker"
#define OTA_CHECK_HOURS  6
#define OTA_TAG_PREFIX   "v"

#define OTA_API_URL  "https://api.github.com/repos/" OTA_REPO "/releases/latest"
#define OTA_FW_URL   "https://github.com/" OTA_REPO "/releases/latest/download/firmware.bin"

#ifndef FW_VERSION
#error "Definisikan FW_VERSION di .ino sebelum #include \"ota.h\""
#endif

extern volatile bool g_audioIdle;
extern volatile bool g_otaBusy;

static bool otaGetLatestTag(String& outTag) {
    WiFiClientSecure c;
    c.setInsecure();
    HTTPClient http;
    http.setConnectTimeout(8000);
    http.setTimeout(8000);
    http.setUserAgent("ESP32-OTA");
    http.addHeader("Accept", "application/vnd.github+json");

    if (!http.begin(c, OTA_API_URL)) return false;
    int code = http.GET();
    if (code != HTTP_CODE_OK) { http.end(); return false; }

    JsonDocument filter;
    filter["tag_name"] = true;
    JsonDocument doc;
    DeserializationError err = deserializeJson(
        doc, http.getStream(), DeserializationOption::Filter(filter));
    http.end();
    if (err) return false;

    outTag = String((const char*)(doc["tag_name"] | ""));
    outTag.trim();
    if (outTag.startsWith(OTA_TAG_PREFIX)) outTag.remove(0, strlen(OTA_TAG_PREFIX));
    return !outTag.isEmpty();
}

static bool otaDownloadAndFlash() {
    WiFiClientSecure c;
    c.setInsecure();
    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setConnectTimeout(10000);
    http.setTimeout(30000);
    http.setUserAgent("ESP32-OTA");

    if (!http.begin(c, OTA_FW_URL)) return false;
    int code = http.GET();
    if (code != HTTP_CODE_OK) { http.end(); return false; }
    int len = http.getSize();
    if (len <= 0) { http.end(); return false; }

    if (!Update.begin(len)) { http.end(); return false; }
    size_t written = Update.writeStream(*http.getStreamPtr());
    if (written != (size_t)len) { Update.abort(); http.end(); return false; }
    if (!Update.end()) { http.end(); return false; }
    http.end();
    return true;
}

static void otaCheckOnce() {
    if (WiFi.status() != WL_CONNECTED) return;
    String tag;
    if (!otaGetLatestTag(tag)) return;
    if (tag == FW_VERSION) return;

    uint32_t t0 = millis();
    while (!g_audioIdle && millis() - t0 < 5UL * 60 * 1000) {
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    if (!g_audioIdle) return;

    g_otaBusy = true;
    if (otaDownloadAndFlash()) {
        delay(500);
        ESP.restart();
    }
    g_otaBusy = false;
}

static void otaTask(void* arg) {
    while (WiFi.status() != WL_CONNECTED) vTaskDelay(pdMS_TO_TICKS(5000));
    vTaskDelay(pdMS_TO_TICKS(15000));
    for (;;) {
        otaCheckOnce();
        vTaskDelay(pdMS_TO_TICKS((uint32_t)OTA_CHECK_HOURS * 3600UL * 1000UL));
    }
}
