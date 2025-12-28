#include <SPI.h>
#include <MFRC522.h>
#include <ESP32Servo.h>
#include <Preferences.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#define SS_PIN    5      // SDA
#define RST_PIN   22     // RST

// Servo
#define SERVO_PIN 13

// IR Obstacle Sensor
#define IR_PIN    14     // OUT sensor IR (LOW = ada objek, HIGH = kosong)

// LED & Buzzer
#define LED_RED_PIN    26
#define LED_GREEN_PIN  25
#define BUZZER_PIN     27

// Sudut servo 
#define GATE_OPEN_ANGLE    90
#define GATE_CLOSED_ANGLE  0

// Delay setelah mobil lewat sebelum palang menutup (ms)
#define CLOSE_DELAY_AFTER_CLEAR  1000

// Batas maksimum kartu yang disimpan
#define MAX_CARDS  100

MFRC522 mfrc522(SS_PIN, RST_PIN);
Servo gateServo;
Preferences prefs;

// ================== UID MASTER ==================
const byte MASTER_UID[4] = {0x70, 0xD3, 0x41, 0x56};
const byte UID_SIZE = 4;
// =================================================

// Jumlah kartu yang tersimpan di database lokal
int cardCount = 0;

// ---------------- WIFI & SCRIPT ----------------
const char* ssid     = "Xiaomi14T";
const char* password = "ghosttown";

const char* apiUrl = "https://script.google.com/macros/s/AKfycbzZC5pYHibFIjaCCzbvyHqaTTPvoAP87zm2nQ_eU1QzytqvrXBbBWHklYPOqCEzC-wJ/exec";

// ------------------------------------------------
// Fungsi utilitas: konversi UID (byte[]) → String hex
// contoh hasil: "70D34156"
// ------------------------------------------------
String uidToString(byte *buffer, byte bufferSize) {
  String s = "";
  for (byte i = 0; i < bufferSize; i++) {
    if (buffer[i] < 0x10) s += "0";
    s += String(buffer[i], HEX);
  }
  s.toUpperCase();
  return s;
}

// Bandingkan UID yang dibaca dengan UID master
bool isMasterCard() {
  if (mfrc522.uid.size != UID_SIZE) return false;
  for (byte i = 0; i < UID_SIZE; i++) {
    if (mfrc522.uid.uidByte[i] != MASTER_UID[i]) return false;
  }
  return true;
}

// Cek apakah uidStr sudah ada di database lokal
bool isAuthorizedCard(String uidStr) {
  for (int i = 0; i < cardCount; i++) {
    String key = "uid" + String(i);
    String stored = prefs.getString(key.c_str(), "");
    if (stored == uidStr) {
      return true;
    }
  }
  return false;
}

// STRUKTUR UNTUK LOG ASYNC (TASK)

struct LogParams {
  String uid;
  String status;
};

// FUNGSI LOG SYNC (DIPANGGIL DARI TASK)
void sendLog(String uidStr, String status) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[LOG] WiFi tidak terhubung, log tidak dikirim.");
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();  // supaya bisa https tanpa sertifikat

  HTTPClient http;
  String url = String(apiUrl) + "?action=log&uid=" + uidStr + "&status=" + status;

  Serial.print("[LOG] Kirim ke: ");
  Serial.println(url);

  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.begin(client, url);
  int httpCode = http.GET();

  if (httpCode > 0) {
    String payload = http.getString();
    payload.trim();
    Serial.print("[LOG] HTTP ");
    Serial.print(httpCode);
    Serial.print(" Respon: ");
    Serial.println(payload);
  } else {
    Serial.print("[LOG] HTTP error: ");
    Serial.println(httpCode);
  }

  http.end();
}

// TASK UNTUK LOG ASYNC
void logTask(void *parameter) {
  LogParams *p = (LogParams*)parameter;
  if (p != nullptr) {
    sendLog(p->uid, p->status);
    delete p;  // bebaskan memori heap
  }
  vTaskDelete(NULL);
}

// WRAPPER: PANGGIL INI UNTUK LOG TANPA BLOKIR LOOP
void logAsync(String uidStr, String status) {
  LogParams *p = new LogParams();
  p->uid = uidStr;
  p->status = status;

  // Buat task di core 1, prioritas 1, stack 12288
  xTaskCreatePinnedToCore(
    logTask,
    "LogTask",
    12288,
    (void*)p,
    1,
    NULL,
    1
  );
}

// SYNC dari Google Sheet → update database lokal
void syncFromServer() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[SYNC] WiFi tidak terhubung, tidak bisa sync.");
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  String url = String(apiUrl) + "?action=getCards";

  Serial.print("[SYNC] Request ke: ");
  Serial.println(url);

  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.begin(client, url);
  int httpCode = http.GET();

  if (httpCode <= 0) {
    Serial.print("[SYNC] HTTP error: ");
    Serial.println(httpCode);
    http.end();
    return;
  }

  String payload = http.getString();
  http.end();

  Serial.print("[SYNC] HTTP ");
  Serial.println(httpCode);
  Serial.println("[SYNC] Data diterima:");
  Serial.println(payload);

  // Parse payload: satu UID per baris
  String newUids[MAX_CARDS];
  int newCount = 0;

  int start = 0;
  while (start < payload.length() && newCount < MAX_CARDS) {
    int end = payload.indexOf('\n', start);
    if (end == -1) end = payload.length();
    String line = payload.substring(start, end);
    line.trim();
    line.toUpperCase();

    if (line.length() > 0) {
      newUids[newCount++] = line;
    }

    start = end + 1;
  }

  if (newCount == 0) {
    Serial.println("[SYNC] Tidak ada UID baru (atau sheet kosong).");
    return;
  }

  // Tulis ke Preferences
  Serial.println("[SYNC] Menulis ulang database lokal...");
  prefs.clear();
  for (int i = 0; i < newCount; i++) {
    String key = "uid" + String(i);
    prefs.putString(key.c_str(), newUids[i]);
  }
  prefs.putInt("count", newCount);
  cardCount = newCount;

  Serial.print("[SYNC] Sync selesai. Total kartu: ");
  Serial.println(cardCount);
}

// FUNGSI BUKA / TUTUP PALANG + LED
void bukaPalang() {
  Serial.println("[PALANG] Membuka palang...");
  gateServo.write(GATE_OPEN_ANGLE);

  // LED hijau ON, merah OFF
  digitalWrite(LED_GREEN_PIN, HIGH);
  digitalWrite(LED_RED_PIN, LOW);

  // bip singkat
  digitalWrite(BUZZER_PIN, HIGH);
  delay(80);
  digitalWrite(BUZZER_PIN, LOW);
}

void tutupPalang() {
  Serial.println("[PALANG] Menutup palang...");
  gateServo.write(GATE_CLOSED_ANGLE);

  // LED merah ON, hijau OFF
  digitalWrite(LED_GREEN_PIN, LOW);
  digitalWrite(LED_RED_PIN, HIGH);

  // bip singkat
  digitalWrite(BUZZER_PIN, HIGH);
  delay(80);
  digitalWrite(BUZZER_PIN, LOW);
}

// FUNGSI BUZZER
void beepSuccess() {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(150);
  digitalWrite(BUZZER_PIN, LOW);
  delay(80);
  digitalWrite(BUZZER_PIN, HIGH);
  delay(150);
  digitalWrite(BUZZER_PIN, LOW);
}

void beepError() {
  for (int i = 0; i < 3; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(100);
    digitalWrite(BUZZER_PIN, LOW);
    delay(100);
  }
}

void setup() {
  Serial.begin(115200);

  // Servo
  gateServo.attach(SERVO_PIN, 500, 2400);
  gateServo.write(GATE_CLOSED_ANGLE);

  // IR sensor
  pinMode(IR_PIN, INPUT);        // HIGH = kosong, LOW = ada objek

  // LED & Buzzer
  pinMode(LED_RED_PIN, OUTPUT);
  pinMode(LED_GREEN_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  // Kondisi awal: palang tertutup → LED merah ON, hijau OFF, buzzer OFF
  digitalWrite(LED_RED_PIN, HIGH);
  digitalWrite(LED_GREEN_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  // RFID
  SPI.begin();
  mfrc522.PCD_Init();

  // Preferences (database lokal)
  prefs.begin("rfid_db", false);
  cardCount = prefs.getInt("count", 0);
  Serial.print("Jumlah kartu terdaftar di database lokal (sebelum sync): ");
  Serial.println(cardCount);

  // WiFi
  Serial.print("Menghubungkan ke WiFi: ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);

  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 10000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WIFI] Terhubung!");
    Serial.print("[WIFI] IP: ");
    Serial.println(WiFi.localIP());

    // Sync awal dari server
    syncFromServer();
  } else {
    Serial.println("\n[WIFI] Gagal terhubung, jalan pakai database lokal yang ada.");
  }

  Serial.println("Sistem palang siap.");
  Serial.println("Tap kartu MASTER untuk PAKSA SYNC dari Google Sheet.");
}

void loop() {
  // ----------- CEK ADA KARTU? ----------------
  if (!mfrc522.PICC_IsNewCardPresent()) return;
  if (!mfrc522.PICC_ReadCardSerial()) return;

  String uidStr = uidToString(mfrc522.uid.uidByte, mfrc522.uid.size);
  Serial.print("\n[RFID] Kartu terdeteksi! UID = ");
  Serial.println(uidStr);

  // --------- Kalau kartu MASTER → paksa SYNC ---------
  if (isMasterCard()) {
    Serial.println("[MASTER] Kartu master ditap → SYNC sekarang.");
    beepSuccess();

    // log master sync secara async juga
    logAsync(uidStr, "MASTER_SYNC");

    syncFromServer();

    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();
    delay(300);
    return;
  }

  // --------- MODE NORMAL: cek kartu di database lokal ---------
  if (isAuthorizedCard(uidStr)) {
    Serial.println("[AKSES] Kartu terdaftar (LOKAL) → akses DIIZINKAN.");
    beepSuccess();
    bukaPalang();

    // Tunggu mobil lewat
    Serial.println("Menunggu mobil lewat...");
    while (digitalRead(IR_PIN) == LOW) {
      delay(10);
    }
    delay(CLOSE_DELAY_AFTER_CLEAR);

    Serial.println("Mobil sudah lewat → menutup palang...");
    tutupPalang();

    // Kirim log secara ASYNC → tidak menghambat kartu berikutnya
    logAsync(uidStr, "ALLOW_LOCAL");
  } else {
    Serial.println("[AKSES] Kartu TIDAK terdaftar (LOKAL) → akses DITOLAK.");
    beepError();
    logAsync(uidStr, "DENY_LOCAL");
  }

  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
  delay(200);
}
