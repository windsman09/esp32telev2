
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "esp_task_wdt.h"

/* ================= CONFIG ================= */
const char* ssid     = "Binus-IoT";
const char* password = "BINUS-10T!";
const char* botToken = "8074543177:AAFRVMV8YKCw8Rb0i0tjrGS_P9R6OUPLMNQ"; // <bot_id>:<secret>
const String CHAT_ID = "-4991238648"; // ganti: untuk grup biasanya NEGATIF (contoh: "-987654321")

// Set true jika rangkaian LED/relay Anda active-low (menyala di LOW, mati di HIGH)
const bool LED_ACTIVE_LOW = false;

/* ================= PIN ================= */
#define BTN1 13
#define BTN2 25
#define BTN3 26

#define LED1 16
#define LED2 17
#define LED3 18

/* ================= STATE ================= */
volatile bool ledState[3] = {false, false, false};

/* ================= QUEUE ================= */
typedef struct {
  uint8_t idx;
} AlertMsg;

QueueHandle_t alertQueue;

/* ================= TELEGRAM STATE ================= */
static long tgOffset = 0; // simpan update_id terakhir yang diproses

/* ================= HELPER ================= */
void setLed(uint8_t idx, bool on) {
  ledState[idx - 1] = on;
  uint8_t pin = (idx == 1) ? LED1 : (idx == 2) ? LED2 : LED3;

  // Active-low: ON=LOW, OFF=HIGH. Active-high: ON=HIGH, OFF=LOW.
  int level = LED_ACTIVE_LOW ? (on ? LOW : HIGH) : (on ? HIGH : LOW);
  digitalWrite(pin, level);

  Serial.printf("[LED] #%u -> %s (write=%s)\n",
                idx, on ? "ON" : "OFF",
                (level == LOW ? "LOW" : "HIGH"));
}

// URL encoder sederhana
String urlencode(const String& s) {
  const char* hex = "0123456789ABCDEF";
  String out;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s.charAt(i);
    if (('a' <= c && c <= 'z') ||
        ('A' <= c && c <= 'Z') ||
        ('0' <= c && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~') {
      out += c;
    } else if (c == ' ') {
      out += "%20"; // hindari '+'
    } else {
      out += '%';
      out += hex[(c >> 4) & 0xF];
      out += hex[c & 0xF];
    }
  }
  return out;
}

/* ================= TELEGRAM TEST: getMe ================= */
void tgTestGetMe() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[TG] getMe: WiFi not connected");
    return;
  }

  WiFiClientSecure c;
  c.setInsecure();          // PRODUKSI: gunakan c.setCACert(<cert api.telegram.org>)
  c.setHandshakeTimeout(4);
  c.setTimeout(3000);

  HTTPClient https;
  String url = "https://api.telegram.org/bot" + String(botToken) + "/getMe";
  if (!https.begin(c, url)) {
    Serial.println("[TG] getMe begin failed");
    return;
  }
  int code = https.GET();
  String payload = (code > 0) ? https.getString() : "";
  https.end();

  Serial.printf("[TG] getMe: code=%d, payload=%s\n", code, payload.c_str());
}

/* ================= TELEGRAM SEND (raw HTTP, debug) ================= */
bool tgSendRawDebug(const String& chatId, const String& text) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[TG] Send: WiFi not connected");
    return false;
  }

  WiFiClientSecure c;
  c.setInsecure();          // PRODUKSI: gunakan c.setCACert()
  c.setHandshakeTimeout(4);
  c.setTimeout(3000);

  HTTPClient https;
  String url = "https://api.telegram.org/bot" + String(botToken) + "/sendMessage";
  if (!https.begin(c, url)) {
    Serial.println("[TG] Send begin failed");
    return false;
  }
  https.addHeader("Content-Type", "application/x-www-form-urlencoded");
  String body = "chat_id=" + chatId + "&text=" + urlencode(text);

  int code = https.POST(body);
  String payload = (code > 0) ? https.getString() : "";
  https.end();

  Serial.printf("[TG] Send: code=%d, body=%s\n", code, body.c_str());
  Serial.printf("[TG] Send: payload=%s\n", payload.c_str());
  return (code == 200);
}

/* ================= TELEGRAM POLL & HANDLE (raw HTTP, debug) ================= */
void tgPollAndHandleDebug(uint16_t budgetMs = 1800) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[TG] Poll: WiFi not connected");
    return;
  }

  WiFiClientSecure c;
  c.setInsecure();          // PRODUKSI: gunakan CA cert
  c.setHandshakeTimeout(4);
  c.setTimeout(3000);

  HTTPClient https;
  String url = "https://api.telegram.org/bot" + String(botToken) +
               "/getUpdates?timeout=1&offset=" + String(tgOffset + 1);

  uint32_t t0 = millis();
  if (!https.begin(c, url)) {
    Serial.println("[TG] Poll begin failed");
    return;
  }
  int code = https.GET();
  uint32_t dur = millis() - t0;
  String payload = (code == 200) ? https.getString() : "";
  https.end();

  Serial.printf("[TG] Poll: code=%d, dur=%lu ms\n", code, (unsigned long)dur);
  Serial.printf("[TG] Poll: url=%s\n", url.c_str());
  Serial.printf("[TG] Poll: payload=%s\n", payload.c_str());

  if (code != 200) return;

  StaticJsonDocument<8 * 1024> doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("[TG] Poll JSON error: %s\n", err.c_str());
    return;
  }
  if (!doc["ok"].as<bool>()) {
    Serial.println("[TG] Poll ok=false");
    return;
  }

  JsonArray results = doc["result"].as<JsonArray>();
  for (JsonObject upd : results) {
    long updId = upd["update_id"] | 0;
    tgOffset = max(tgOffset, updId);

    JsonObject msg = upd["message"];
    if (msg.isNull()) continue;

    String text = msg["text"] | "";

    long long cidRaw = msg["chat"]["id"] | 0LL; // dukung negatif (grup)
    String cid = String(cidRaw);

    Serial.printf("[TG] Incoming cid=%s text=%s\n", cid.c_str(), text.c_str());

    if (cid != CHAT_ID) continue;

    if (text == "/resolve1") {
      Serial.println("[TG] resolve1 received");
      setLed(1, false);  // OFF saat resolve
      tgSendRawDebug(CHAT_ID, "Laporan #1 ditutup");
    }
    else if (text == "/resolve2") {
      Serial.println("[TG] resolve2 received");
      setLed(2, false);
      tgSendRawDebug(CHAT_ID, "Laporan #2 ditutup");
    }
    else if (text == "/resolve3") {
      Serial.println("[TG] resolve3 received");
      setLed(3, false);
      tgSendRawDebug(CHAT_ID, "Laporan #3 ditutup");
    }
    else if (text == "/status") {
      String s = "STATUS:\n";
      s += "LED1: " + String(ledState[0] ? "ON" : "OFF") + "\n";
      s += "LED2: " + String(ledState[1] ? "ON" : "OFF") + "\n";
      s += "LED3: " + String(ledState[2] ? "ON" : "OFF") + "\n";
      s += "WiFi RSSI: " + String(WiFi.RSSI()) + " dBm\n";
      tgSendRawDebug(CHAT_ID, s);
    }
  }
}

/* ================= COMPOSE ALERT MESSAGE ================= */
String composeAlert(uint8_t idx) {
  String msg;
  if (idx == 1) msg = "TOILET MAMPET";
  else if (idx == 2) msg = "TOILET KOTOR";
  else if (idx == 3) msg = "TOILET RUSAK";

  msg += "\n\nLaporan diterima";
  msg += "\nIndikator #" + String(idx) + " Toilet L1 Pria menyala";
  msg += "\nKetik /resolve" + String(idx);
  return msg;
}

/* ================= TASK INPUT (CORE 1) ================= */
void taskInput(void* pv) {
  bool last[3] = {HIGH, HIGH, HIGH};
  unsigned long lastTime[3] = {0, 0, 0};
  const unsigned long debounceMs = 50;

  for (;;) {
    int r[3] = {
      digitalRead(BTN1),
      digitalRead(BTN2),
      digitalRead(BTN3)
    };

    for (int i = 0; i < 3; i++) {
      if (r[i] != last[i]) lastTime[i] = millis();

      if ((millis() - lastTime[i]) > debounceMs) {
        // Tekan (LOW karena input pull-up) & hanya nyalakan jika sebelumnya OFF
        if (r[i] == LOW && !ledState[i]) {
          setLed(i + 1, true);  // ON saat ditekan
          AlertMsg m;
          m.idx = i + 1;
          xQueueSend(alertQueue, &m, 0);
          Serial.printf("BTN %d PRESSED\n", i + 1);
        }
      }
      last[i] = r[i];
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

/* ================= TASK TELEGRAM (CORE 1) ================= */
void taskTelegram(void* pv) {
  AlertMsg msg;
  static unsigned long lastTelegram = 0;

  // Register task ke WDT (inisialisasi WDT ada di setup)
  esp_task_wdt_add(NULL);

  for (;;) {
    esp_task_wdt_reset();

    // Kirim alert dari queue
    if (xQueueReceive(alertQueue, &msg, 0) == pdTRUE) {
      String text = composeAlert(msg.idx);
      Serial.println("SEND TELEGRAM:");
      Serial.println(text);
      tgSendRawDebug(CHAT_ID, text);
      esp_task_wdt_reset();
    }

    // Poll Telegram tiap 2 detik
    if (WiFi.status() == WL_CONNECTED && (millis() - lastTelegram) > 2000) {
      tgPollAndHandleDebug(1800);
      lastTelegram = millis();
      esp_task_wdt_reset();
    }

    vTaskDelay(pdMS_TO_TICKS(50)); // beri waktu scheduler
  }
}

/* ================= SETUP ================= */
void setup() {
  Serial.begin(115200);

  pinMode(BTN1, INPUT_PULLUP);
  pinMode(BTN2, INPUT_PULLUP);
  pinMode(BTN3, INPUT_PULLUP);

  pinMode(LED1, OUTPUT);
  pinMode(LED2, OUTPUT);
  pinMode(LED3, OUTPUT);

  // Default: semua indikator OFF
  setLed(1, false);
  setLed(2, false);
  setLed(3, false);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  Serial.print("Connecting WiFi");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - t0) < 10000) {
    delay(500);
    Serial.print(".");
  }

  // Inisialisasi WDT global (6 detik)
  esp_task_wdt_init(6, true);

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi OK");
    tgTestGetMe(); // validasi token
    tgSendRawDebug(CHAT_ID, "Toilet L1 Pria ONLINE");
  } else {
    Serial.println("\nWiFi FAILED");
  }

  alertQueue = xQueueCreate(5, sizeof(AlertMsg));

  // Prioritas seimbang agar dua task jalan mulus
  xTaskCreatePinnedToCore(taskInput,    "INPUT",    4096,  NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(taskTelegram, "TELEGRAM", 12288, NULL, 2, NULL, 1);
}

/* ================= LOOP ================= */
void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
