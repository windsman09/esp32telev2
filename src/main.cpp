#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>

/* ================= CONFIG ================= */
const char* ssid     = "BINUS-IoT";
const char* password = "Syahdan-10T!";
const char* botToken = "token";
const String CHAT_ID = "ID";

/* ================= TELEGRAM ================= */
WiFiClientSecure client;
UniversalTelegramBot bot(botToken, client);

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

/* ================= HELPER ================= */
void setLed(uint8_t idx, bool on) {
  ledState[idx - 1] = on;
  uint8_t pin = (idx == 1) ? LED1 : (idx == 2) ? LED2 : LED3;
  digitalWrite(pin, on ? HIGH : LOW);
}

/* ================= TELEGRAM SEND ================= */
void sendAlert(uint8_t idx) {
  if (WiFi.status() != WL_CONNECTED) return;

  String msg;
  if (idx == 1) msg = "TOILET MAMPET";
  else if (idx == 2) msg = "TOILET KOTOR";
  else if (idx == 3) msg = "TOILET RUSAK";

  msg += "\n\nLaporan diterima";
  msg += "\nIndikator #" + String(idx) + " Toilet L1 Pria menyala";
  msg += "\nKetik /resolve" + String(idx);

  Serial.println("SEND TELEGRAM:");
  Serial.println(msg);

  bot.sendMessage(CHAT_ID, msg, "");
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
        if (r[i] == LOW && !ledState[i]) {
          setLed(i + 1, true);

          AlertMsg m;
          m.idx = i + 1;
          xQueueSend(alertQueue, &m, 0);

          Serial.printf("BTN %d PRESSED\n", i + 1);
        }
      }
      last[i] = r[i];
    }

    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

/* ================= TASK TELEGRAM (CORE 0) ================= */
void taskTelegram(void* pv) {
  AlertMsg msg;

  for (;;) {
    /* Kirim alert dari queue */
    if (xQueueReceive(alertQueue, &msg, 0) == pdTRUE) {
      sendAlert(msg.idx);
    }

    /* Handle command Telegram */
    if (WiFi.status() == WL_CONNECTED) {
      int n = bot.getUpdates(bot.last_message_received + 1);
      for (int i = 0; i < n; i++) {
        String text = bot.messages[i].text;
        String cid  = bot.messages[i].chat_id;

        if (cid != CHAT_ID) continue;

        if (text == "/resolve1") {
          setLed(1, false);
          bot.sendMessage(CHAT_ID, "Laporan #1 ditutup", "");
        }
        else if (text == "/resolve2") {
          setLed(2, false);
          bot.sendMessage(CHAT_ID, "Laporan #2 ditutup", "");
        }
        else if (text == "/resolve3") {
          setLed(3, false);
          bot.sendMessage(CHAT_ID, "Laporan #3 ditutup", "");
        }
        else if (text == "/status") {
          String s = "STATUS:\n";
          s += "LED1: " + String(ledState[0] ? "ON" : "OFF") + "\n";
          s += "LED2: " + String(ledState[1] ? "ON" : "OFF") + "\n";
          s += "LED3: " + String(ledState[2] ? "ON" : "OFF") + "\n";
          s += "WiFi: " + String(WiFi.status() == WL_CONNECTED ? "OK" : "OFF");
          bot.sendMessage(CHAT_ID, s, "");
        }
      }
    }

    vTaskDelay(1500 / portTICK_PERIOD_MS);
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

  setLed(1, false);
  setLed(2, false);
  setLed(3, false);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  Serial.print("Connecting WiFi");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) {
    delay(500);
    Serial.print(".");
  }

  client.setInsecure();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi OK");
    bot.sendMessage(CHAT_ID, "ESP32 ONLINE", "");
  } else {
    Serial.println("\nWiFi FAILED");
  }

  alertQueue = xQueueCreate(5, sizeof(AlertMsg));

  xTaskCreatePinnedToCore(taskInput, "INPUT", 4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(taskTelegram, "TELEGRAM", 12288, NULL, 1, NULL, 0);
}

/* ================= LOOP ================= */
void loop() {
  vTaskDelay(1000 / portTICK_PERIOD_MS);
}
