// ═══════════════════════════════════════════════════════════════
//  Board B (S3) — Speaker Cuaca (TTS) + Musik via STB + OTA
//  ESP32-S3 N16R8 + MAX98357A
//
//  Board Settings (ArduinoDroid): PSRAM = OPI PSRAM
//
//  Arsitektur (dual-core S3 + FreeRTOS):
//    Core 0 : netTask (polling STB) + otaTask (cek update)
//    Core 1 : loop() → SATU-SATUNYA yang menyentuh objek `audio`
//    Penghubung : FreeRTOS queue (netTask kirim perintah ke loop)
// ═══════════════════════════════════════════════════════════════

// ── Versi firmware — WAJIB naikkan sebelum rilis OTA ──
#define FW_VERSION "1.0.0"

#include <WiFi.h>
#include <WiFiClient.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "Audio_nopsram.h"
#include "ota.h"                 // ← modul OTA (setelah FW_VERSION)

// ─── WiFi ───
const char* WIFI_SSID = "SRI TUWU NETWORK";
const char* WIFI_PASS = "srituwu707";

// ─── I2S Pin (S3) ───
#define I2S_BCLK 15
#define I2S_LRC  16
#define I2S_DIN  17
#define AMP_SD_PIN -1

// ─── STB ───
const char* STB_BASE      = "http://192.168.1.199";
const char* STB_AUDIO_API = "http://192.168.1.199/api/cuaca-audio.php";
const char* STB_MUSIC_API = "http://192.168.1.199/api/musik-status.php";

// ─── Pengaturan ───
const uint32_t POLL_INTERVAL_MS  = 5000;
const uint32_t HTTP_CONNECT_MS   = 3000;
const uint32_t HTTP_READ_MS      = 4000;
const uint16_t STREAM_TIMEOUT_MS = 30000;
const uint32_t PLAY_GRACE_MS     = 3000;
const uint32_t RETRY_DELAY_MS    = 3000;
const uint8_t  MAX_RETRY         = 2;
const uint32_t WIFI_DEAD_RESTART = 300000;
const uint32_t NET_STALL_RESTART = 120000;
const uint32_t HEAP_MIN_BLOCK    = 20000;
const size_t   PSRAM_BUF_BYTES   = 512 * 1024;
const uint8_t  VOLUME_TTS        = 15;
const uint8_t  VOLUME_MUSIK      = 15;

// ─── Tipe data ───
enum CmdType : uint8_t { CMD_PLAY_TTS, CMD_PLAY_MUSIK, CMD_STOP_MUSIK };
enum Source  : uint8_t { SRC_NONE, SRC_TTS, SRC_MUSIK };

struct AudioCmd {
    CmdType type;
    char    url[256];
    char    title[64];
};

// ─── Global ───
Audio audio;
QueueHandle_t cmdQueue;

// Dimiliki loop() (core 1)
Source   g_src         = SRC_NONE;
uint32_t g_playStartMs = 0;
bool     g_hasPending  = false;
AudioCmd g_pending;
AudioCmd g_retryCmd;
uint8_t  g_retryLeft   = 0;
uint32_t g_retryAt     = 0;

// Dibagi antar core
volatile uint32_t g_netBeat   = 0;
volatile bool     g_audioIdle = true;   // dibaca otaTask
volatile bool     g_otaBusy   = false;  // ditulis otaTask

// ─── Forward ───
void startPlayback(const AudioCmd& c, uint8_t retryLeft);
void handleCmd(const AudioCmd& c);
bool fetchJson(const char* url, JsonDocument& doc);
void pollTTS(uint32_t& last, bool& synced);
void pollMusik(uint32_t& last, bool& synced);
void netTask(void* arg);

// ═══════════════════════════════════════════════════════════════
//  Util
// ═══════════════════════════════════════════════════════════════
void ampOn(bool on) {
#if AMP_SD_PIN >= 0
    if (on) {
        pinMode(AMP_SD_PIN, INPUT);
    } else {
        pinMode(AMP_SD_PIN, OUTPUT);
        digitalWrite(AMP_SD_PIN, LOW);
    }
#else
    (void)on;
#endif
}

void buildUrl(const char* raw, char* out, size_t outSize) {
    if (strncmp(raw, "http://", 7) == 0 || strncmp(raw, "https://", 8) == 0) {
        snprintf(out, outSize, "%s", raw);
    } else {
        snprintf(out, outSize, "%s%s%s", STB_BASE, raw[0] == '/' ? "" : "/", raw);
    }
}

void onWiFiEvent(WiFiEvent_t event) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            Serial.printf("[WIFI] OK, IP: %s, RSSI: %d\n",
                          WiFi.localIP().toString().c_str(), WiFi.RSSI());
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            Serial.println("[WIFI] Putus");
            break;
        default: break;
    }
}

// ═══════════════════════════════════════════════════════════════
//  Setup
// ═══════════════════════════════════════════════════════════════
void setup() {
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 3000) delay(10);
    Serial.printf("\n\n=== S3 Speaker v%s Booting ===\n", FW_VERSION);

    ampOn(false);

    if (psramFound()) {
        Serial.printf("[BOOT] PSRAM bebas: %lu KB\n",
                      (unsigned long)(ESP.getFreePsram() / 1024));
    } else {
        Serial.println("[BOOT] PSRAM TIDAK ketemu → set Board Settings: PSRAM = OPI PSRAM");
    }

    // WiFi non-blocking
    WiFi.onEvent(onWiFiEvent);
    WiFi.mode(WIFI_STA);
    WiFi.persistent(false);
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(false);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    // Audio
    audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DIN);
    audio.setConnectionTimeout(STREAM_TIMEOUT_MS, 10000);
    //audio.setAudioTaskCore(1);
    if (psramFound()) audio.setBufsize(0, PSRAM_BUF_BYTES);
    audio.setVolume(VOLUME_MUSIK);

    // Queue + task
    cmdQueue  = xQueueCreate(4, sizeof(AudioCmd));
    g_netBeat = millis();

    xTaskCreatePinnedToCore(netTask, "netTask", 10240, nullptr, 1, nullptr, 0);
    xTaskCreatePinnedToCore(otaTask, "otaTask",  8192, nullptr, 1, nullptr, 0);

    Serial.println("[BOOT] Siap. Masuk loop...\n");
}

// ═══════════════════════════════════════════════════════════════
//  Loop (core 1): audio + eksekusi perintah
// ═══════════════════════════════════════════════════════════════
void loop() {
    audio.loop();

    // Update flag untuk otaTask
    g_audioIdle = (g_src == SRC_NONE) && (g_retryLeft == 0) && !g_hasPending;

    // 1) Terima perintah dari netTask
    AudioCmd cmd;
    while (xQueueReceive(cmdQueue, &cmd, 0) == pdTRUE) {
        handleCmd(cmd);
    }

    // 2) Retry connect
    if (g_retryLeft > 0 && (int32_t)(millis() - g_retryAt) >= 0) {
        uint8_t left = g_retryLeft - 1;
        g_retryLeft = 0;
        Serial.println("[AUDIO] Retry connect...");
        startPlayback(g_retryCmd, left);
    }

    // 3) Deteksi selesai
    if (g_src != SRC_NONE && millis() - g_playStartMs > PLAY_GRACE_MS && !audio.isRunning()) {
        Serial.println("[AUDIO] Selesai / putus");
        g_src = SRC_NONE;
        ampOn(false);
        if (g_hasPending) {
            g_hasPending = false;
            startPlayback(g_pending, MAX_RETRY);
        }
    }

    // 4) Supervisor
    static uint32_t lastStat = 0;
    static uint8_t  lowHeapCount = 0;
    uint32_t now = millis();

    if (now - g_netBeat > NET_STALL_RESTART) {
        Serial.println("[SUPERVISOR] netTask macet → restart");
        delay(200);
        ESP.restart();
    }

    if (now - lastStat >= 30000) {
        lastStat = now;
        uint32_t maxBlk = ESP.getMaxAllocHeap();
        Serial.printf("[STAT] heap=%lu blokMax=%lu psram=%lu rssi=%d src=%d v=%s\n",
                      (unsigned long)ESP.getFreeHeap(), (unsigned long)maxBlk,
                      (unsigned long)ESP.getFreePsram(), WiFi.RSSI(),
                      (int)g_src, FW_VERSION);
        if (maxBlk < HEAP_MIN_BLOCK && g_src == SRC_NONE) {
            if (++lowHeapCount >= 3) {
                Serial.println("[SUPERVISOR] Heap terfragmentasi → restart");
                delay(200);
                ESP.restart();
            }
        } else {
            lowHeapCount = 0;
        }
    }

    delay(1);
}

// ═══════════════════════════════════════════════════════════════
//  Eksekusi perintah audio
// ═══════════════════════════════════════════════════════════════
void startPlayback(const AudioCmd& c, uint8_t retryLeft) {
    bool isTts = (c.type == CMD_PLAY_TTS);

    audio.setVolume(isTts ? VOLUME_TTS : VOLUME_MUSIK);
    ampOn(true);
    Serial.printf("[%s] %s%s%s\n", isTts ? "TTS" : "MUSIK",
                  c.url, c.title[0] ? "  | " : "", c.title);

    if (audio.connecttohost(c.url)) {
        g_src = isTts ? SRC_TTS : SRC_MUSIK;
        g_playStartMs = millis();
        return;
    }

    g_src = SRC_NONE;
    ampOn(false);
    if (retryLeft > 0) {
        g_retryCmd  = c;
        g_retryLeft = retryLeft;
        g_retryAt   = millis() + RETRY_DELAY_MS;
        Serial.printf("[AUDIO] Gagal connect, retry %u kali lagi\n", retryLeft);
    } else {
        Serial.println("[AUDIO] Gagal connect, menyerah");
    }
}

void handleCmd(const AudioCmd& c) {
    if (g_otaBusy) {         // tolak perintah saat OTA sedang flash
        Serial.println("[AUDIO] OTA sedang jalan, perintah diabaikan");
        return;
    }

    switch (c.type) {
        case CMD_PLAY_TTS:
            g_retryLeft = 0;
            if (audio.isRunning()) audio.stopSong();
            startPlayback(c, MAX_RETRY);
            break;

        case CMD_PLAY_MUSIK: {
            bool ttsBusy = (g_src == SRC_TTS) ||
                           (g_retryLeft > 0 && g_retryCmd.type == CMD_PLAY_TTS);
            if (ttsBusy) {
                g_pending = c;
                g_hasPending = true;
                Serial.println("[MUSIK] Menunggu TTS selesai");
            } else {
                g_retryLeft = 0;
                if (audio.isRunning()) audio.stopSong();
                startPlayback(c, MAX_RETRY);
            }
            break;
        }

        case CMD_STOP_MUSIK:
            Serial.println("[MUSIK] Stop");
            g_hasPending = false;
            if (g_retryLeft > 0 && g_retryCmd.type == CMD_PLAY_MUSIK) g_retryLeft = 0;
            if (g_src == SRC_MUSIK) {
                audio.stopSong();
                g_src = SRC_NONE;
                ampOn(false);
            }
            break;
    }
}

// ═══════════════════════════════════════════════════════════════
//  netTask (core 0)
// ═══════════════════════════════════════════════════════════════
bool fetchJson(const char* url, JsonDocument& doc) {
    WiFiClient client;
    HTTPClient http;
    http.setConnectTimeout(HTTP_CONNECT_MS);
    http.setTimeout(HTTP_READ_MS);
    http.useHTTP10(true);

    if (!http.begin(client, url)) return false;
    int code = http.GET();
    if (code != HTTP_CODE_OK) { http.end(); return false; }

    DeserializationError err = deserializeJson(doc, http.getStream());
    http.end();
    return !err;
}

void pollTTS(uint32_t& last, bool& synced) {
    JsonDocument doc;
    if (!fetchJson(STB_AUDIO_API, doc)) return;

    const char* url = doc["audio_url"] | "";
    uint32_t stamp  = doc["audio_updated_at"] | 0UL;

    if (!synced) {
        synced = true;
        last = stamp;
        Serial.printf("[TTS] Sync awal, stamp=%lu\n", (unsigned long)stamp);
        return;
    }
    if (stamp <= last) return;
    if (url[0] == '\0') { last = stamp; return; }

    AudioCmd cmd = {};
    cmd.type = CMD_PLAY_TTS;
    buildUrl(url, cmd.url, sizeof(cmd.url));
    if (xQueueSend(cmdQueue, &cmd, 0) == pdTRUE) last = stamp;
}

void pollMusik(uint32_t& last, bool& synced) {
    JsonDocument doc;
    if (!fetchJson(STB_MUSIC_API, doc)) return;

    const char* status = doc["status"] | "";
    const char* url    = doc["audio_url"] | "";
    const char* title  = doc["title"] | "";
    uint32_t stamp     = doc["audio_updated_at"] | 0UL;

    if (!synced) {
        synced = true;
        last = stamp;
        Serial.printf("[MUSIK] Sync awal, stamp=%lu\n", (unsigned long)stamp);
        return;
    }
    if (stamp <= last) return;

    AudioCmd cmd = {};
    if (strcmp(status, "stop") == 0) {
        cmd.type = CMD_STOP_MUSIK;
    } else if (url[0] != '\0') {
        cmd.type = CMD_PLAY_MUSIK;
        buildUrl(url, cmd.url, sizeof(cmd.url));
        strlcpy(cmd.title, title, sizeof(cmd.title));
    } else {
        last = stamp;
        return;
    }
    if (xQueueSend(cmdQueue, &cmd, 0) == pdTRUE) last = stamp;
}

void netTask(void* arg) {
    uint32_t lastTts = 0, lastMusik = 0;
    bool ttsSynced = false, musikSynced = false;
    uint32_t wifiDownSince = 0, lastReconnect = 0;

    for (;;) {
        g_netBeat = millis();

        if (WiFi.status() != WL_CONNECTED) {
            uint32_t now = millis();
            if (wifiDownSince == 0) wifiDownSince = now;
            if (now - lastReconnect > 15000) {
                lastReconnect = now;
                Serial.println("[WIFI] Reconnect...");
                WiFi.reconnect();
            }
            if (now - wifiDownSince > WIFI_DEAD_RESTART) {
                Serial.println("[WIFI] Mati terlalu lama → restart");
                delay(200);
                ESP.restart();
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        wifiDownSince = 0;

        pollTTS(lastTts, ttsSynced);
        pollMusik(lastMusik, musikSynced);

        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

// ═══════════════════════════════════════════════════════════════
//  Callback library Audio
// ═══════════════════════════════════════════════════════════════
void audio_info(const char* info)       { Serial.printf("[AUDIO] %s\n", info); }
void audio_eof_mp3(const char* info)    { Serial.printf("[AUDIO] EOF mp3: %s\n", info); }
void audio_eof_opus(const char* info)   { Serial.printf("[AUDIO] EOF opus: %s\n", info); }
void audio_eof_stream(const char* info) { Serial.printf("[AUDIO] Stream berakhir: %s\n", info); }
