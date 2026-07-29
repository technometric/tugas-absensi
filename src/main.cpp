/**
 * =====================================================
 *  SISTEM ABSENSI PURE ESP32-CAM — FULLY STANDALONE
 *  ESP32-CAM + AS608 + SD Card + Web Server + Blynk
 * =====================================================
 *  Fitur:
 *  - Web Server built-in (dashboard + enrollment)
 *  - Foto tersimpan permanen di SD Card
 *  - Log absensi harian di SD Card (CSV)
 *  - Data siswa di SPIFFS (JSON)
 *  - Notifikasi Blynk
 *  - Enrollment via browser (tanpa laptop)
 *  - Tanpa Flask, tanpa database eksternal
 * =====================================================
 */

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPUpdate.h>
// Blynk — wajib include setelah define TEMPLATE_ID di config.h
#include "config.h"  // BLYNK_TEMPLATE_ID, BLYNK_TEMPLATE_NAME, BLYNK_AUTH_TOKEN
#define BLYNK_PRINT Serial
#include <BlynkSimpleEsp32.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <SPI.h>
#include <SD.h>
// ESP32 SD library pakai fs::File — sama dengan SPIFFS
// Tidak perlu typedef, langsung pakai fs::File untuk semua
#include <Adafruit_Fingerprint.h>
#include <Wire.h>
#include "esp_camera.h"
#include "img_converters.h"
// config.h sudah diinclude di atas
#define WIFI_CFG_FILE  "/wifi.json"


// ============================================================
//   PIN KAMERA (AI Thinker ESP32-CAM)
// ============================================================
#define PWDN_GPIO_NUM   32
#define RESET_GPIO_NUM  -1
#define XCLK_GPIO_NUM    0
#define SIOD_GPIO_NUM   26
#define SIOC_GPIO_NUM   27
#define Y9_GPIO_NUM     35
#define Y8_GPIO_NUM     34
#define Y7_GPIO_NUM     39
#define Y6_GPIO_NUM     36
#define Y5_GPIO_NUM     21
#define Y4_GPIO_NUM     19
#define Y3_GPIO_NUM     18
#define Y2_GPIO_NUM      5
#define VSYNC_GPIO_NUM  25
#define HREF_GPIO_NUM   23
#define PCLK_GPIO_NUM   22

// ============================================================
//   OBJEK GLOBAL
// ============================================================

// Fingerprint pakai UART0 (GPIO1/3) — nonaktifkan Serial Monitor
// saat production. Untuk dev, swap ke UART1 dengan GPIO lain.
HardwareSerial fpSerial(2);   // UART2 — tapi ESP32-CAM tidak expose GPIO16/17
// FALLBACK: gunakan UART0
// HardwareSerial fpSerial(0);

// === GUNAKAN INI untuk development ===
// Karena ESP32-CAM AI Thinker tidak expose GPIO16/17,
// kita pakai UART0 untuk fingerprint (nonaktifkan Serial.print)
// ATAU gunakan SoftwareSerial workaround

// Untuk kemudahan: kita definisikan FP di UART1 dengan pin 14/15
// dan SD_MMC di 1-bit mode (hanya butuh GPIO 2)
HardwareSerial mySerial(1);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&mySerial);

WebServer server(HTTP_PORT);

// State
enum SysMode { MODE_ABSENSI, MODE_ENROLLMENT };
SysMode sysMode = MODE_ABSENSI;

struct EnrollState {
  bool   active    = false;
  int    id        = -1;
  String name      = "";   // sesuai Laravel: name
  String nisn      = "";   // sesuai Laravel: nisn
  String className = "";   // sesuai Laravel: class_name
  int    step      = 0;
  String statusMsg = "Idle";
};
EnrollState enroll;

int  totalHadir  = 0;
bool sdOk        = false;
bool spiffsOk    = false;
bool fpOk        = false;
bool camOk       = false;
bool wifiOk      = false;
bool apMode      = false;

// Dashboard realtime: popup absensi terbaru di browser
volatile uint32_t lastAbsensiSeq = 0;
String lastAbsensiJson = "{}";
String popupBootId = "";

// ===== OTA STATE =====
volatile int otaPercent = 0;
volatile bool otaRunning = false;
volatile bool otaDone = false;
String otaStatus = "Idle";
String latestTag = "";
String latestBinUrl = "";
String latestNotes = "";

String jsonEscape(const String &in);
void sendCorsHeaders();


// ============================================================
//   WIFI CONFIG — BACA/TULIS SSID+PASS DI SPIFFS
// ============================================================

struct WifiCfg { String ssid; String pass; };

WifiCfg loadWifiCfg() {
  WifiCfg cfg = {WIFI_SSID, WIFI_PASSWORD};
  if (!SPIFFS.exists(WIFI_CFG_FILE)) return cfg;
  fs::File f = SPIFFS.open(WIFI_CFG_FILE, FILE_READ);
  if (!f) return cfg;
  JsonDocument doc;
  if (!deserializeJson(doc, f)) {
    if (doc["ssid"].as<String>().length() > 0) cfg.ssid = doc["ssid"].as<String>();
    if (doc["pass"].as<String>().length() > 0) cfg.pass = doc["pass"].as<String>();
  }
  f.close();
  return cfg;
}

bool saveWifiCfg(const String& ssid, const String& pass) {
  fs::File f = SPIFFS.open(WIFI_CFG_FILE, FILE_WRITE);
  if (!f) return false;
  JsonDocument doc;
  doc["ssid"] = ssid;
  doc["pass"] = pass;
  serializeJson(doc, f);
  f.close();
  Serial.printf("[WiFiCfg] Tersimpan: %s\n", ssid.c_str());
  return true;
}

bool connectWifi() {
  WifiCfg cfg = loadWifiCfg();

  // Dual mode: AP + STA sekaligus
  String apName = "Absensi-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apName.c_str(), "12345678");
  Serial.printf("[AP] Hotspot: %s | IP: %s\n",
    apName.c_str(), WiFi.softAPIP().toString().c_str());

  // Konek WiFi dulu
  Serial.printf("[WiFi] Connecting: %s", cfg.ssid.c_str());
  WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < WIFI_TIMEOUT) {
    delay(400); Serial.print(".");
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\n[WiFi] Router gagal, hotspot tetap aktif");
    return false;
  }
  Serial.printf("\n[WiFi] Client IP: %s\n", WiFi.localIP().toString().c_str());

  // Koneksi ke Blynk server (non-blocking, timeout 5 detik)
  Serial.print("[BLYNK] Connecting...");
  Blynk.config(BLYNK_AUTH_TOKEN);
  bool blynkOk = Blynk.connect(5000);
  Serial.println(blynkOk ? " OK!" : " Gagal (offline mode)");

  return true;
}

void startAPMode() {
  // Tidak dipakai lagi — AP selalu aktif via WIFI_AP_STA di connectWifi()
}

// ============================================================
//   UTILITAS WAKTU
// ============================================================
String getJam() {
  struct tm ti;
  if (getLocalTime(&ti)) {
    char buf[9];
    sprintf(buf, "%02d:%02d:%02d", ti.tm_hour, ti.tm_min, ti.tm_sec);
    return String(buf);
  }
  // Fallback millis
  unsigned long s = millis() / 1000;
  char buf[9];
  sprintf(buf, "%02d:%02d:%02d", (int)(s/3600)%24, (int)(s/60)%60, (int)s%60);
  return String(buf);
}

String getTanggal() {
  struct tm ti;
  if (getLocalTime(&ti)) {
    char buf[11];
    sprintf(buf, "%04d-%02d-%02d", ti.tm_year+1900, ti.tm_mon+1, ti.tm_mday);
    return String(buf);
  }
  return "2026-01-01";
}

void ledBlink(int n, int ms = 200) {
  for (int i = 0; i < n; i++) {
    digitalWrite(LED_ONBOARD, LOW);
    delay(ms);
    digitalWrite(LED_ONBOARD, HIGH);
    delay(ms);
  }
}

// ============================================================
//   SD CARD — SPI MODE
// ============================================================
bool initSD() {
  // Pin SPI: SCK=14, MISO=2, MOSI=15, CS=13
  SPI.begin(14, 2, 15, 13);

  if (!SD.begin(13)) {
    Serial.println("[SD] Gagal mount!");
    Serial.println("[SD] Cek: format FAT32? SD terpasang?");
    return false;
  }

  uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("[SD] Tidak ada SD Card!");
    return false;
  }

  String typeStr = "UNKNOWN";
  if      (cardType == CARD_MMC)  typeStr = "MMC";
  else if (cardType == CARD_SD)   typeStr = "SD";
  else if (cardType == CARD_SDHC) typeStr = "SDHC";
  Serial.printf("[SD] Type: %s | Size: %llu MB\n",
    typeStr.c_str(), SD.cardSize() / (1024*1024));

  if (!SD.exists(SD_FOTO_DIR)) SD.mkdir(SD_FOTO_DIR);
  if (!SD.exists(SD_LOG_DIR))  SD.mkdir(SD_LOG_DIR);
  return true;
}

/**
 * Simpan foto ke SD Card
 * Return: path file atau "" jika gagal
 */
String saveFotoSD(camera_fb_t* fb, int fingerId, const String& tanggal, const String& jam) {
  if (!sdOk || !fb) return "";
  String jamClean = jam;
  jamClean.replace(":", "");
  String path = String(SD_FOTO_DIR) + "/" + tanggal + "_" + jamClean + "_id" + fingerId + ".jpg";

  fs::File f = SD.open(path, FILE_WRITE);
  if (!f) {
    Serial.println("[SD] Gagal buka file untuk tulis");
    return "";
  }
  f.write(fb->buf, fb->len);
  f.close();
  Serial.printf("[SD] Foto tersimpan: %s (%d bytes)\n", path.c_str(), fb->len);
  return path;
}

/**
 * Append log absensi ke CSV harian di SD
 */
void appendLogSD(const String& tanggal, int fingerId,
                 const String& name, const String& nisn,
                 const String& className, const String& status,
                 const String& jam, const String& fotoPath) {
  if (!sdOk) return;
  String logFile = String(SD_LOG_DIR) + "/" + tanggal + ".csv";
  bool fileExist = SD.exists(logFile);

  fs::File f = SD.open(logFile, FILE_APPEND);
  if (!f) return;

  // Header CSV sesuai struktur Laravel
  if (!fileExist) {
    f.println("attendance_date,tapped_at,fingerprint_device_id,name,nisn,class_name,status,foto");
  }
  f.printf("%s,%s %s,%d,%s,%s,%s,%s,%s\n",
    tanggal.c_str(), tanggal.c_str(), jam.c_str(), fingerId,
    name.c_str(), nisn.c_str(), className.c_str(),
    status.c_str(), fotoPath.c_str());
  f.close();
}

/**
 * Baca log CSV hari ini, return sebagai JSON array string
 */
String readLogToday() {
  String tanggal = getTanggal();
  String logFile = String(SD_LOG_DIR) + "/" + tanggal + ".csv";

  String result = "[";
  if (!sdOk || !SD.exists(logFile)) return "[]";

  fs::File f = SD.open(logFile, FILE_READ);
  if (!f) return "[]";

  bool firstLine  = true;
  bool firstEntry = true;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (firstLine) { firstLine = false; continue; }
    if (line.isEmpty()) continue;

    // CSV baru 8 kolom:
    // 0=attendance_date, 1=tapped_at, 2=fingerprint_device_id,
    // 3=name, 4=nisn, 5=class_name, 6=status, 7=foto
    String fields[8];
    int col = 0, prev = 0;
    for (int i = 0; i <= (int)line.length() && col < 8; i++) {
      if (i == (int)line.length() || line[i] == ',') {
        fields[col++] = line.substring(prev, i);
        prev = i + 1;
      }
    }

    if (!firstEntry) result += ",";
    firstEntry = false;

    // Convert status enum ke label Indonesia
    String statusLabel = fields[6];
    if      (statusLabel == "present")    statusLabel = "HADIR";
    else if (statusLabel == "sick")       statusLabel = "SAKIT";
    else if (statusLabel == "permission") statusLabel = "IZIN";
    else if (statusLabel == "absent")     statusLabel = "ALPHA";

    String fotoUrl = fields[7].length() > 0 ? "/foto?path=" + fields[7] : "";

    result += "{";
    result += "\"attendance_date\":\"" + fields[0] + "\",";
    result += "\"tapped_at\":\"" + fields[1] + "\",";
    result += "\"fingerprint_device_id\":" + fields[2] + ",";
    result += "\"name\":\"" + fields[3] + "\",";
    result += "\"nisn\":\"" + fields[4] + "\",";
    result += "\"class_name\":\"" + fields[5] + "\",";
    result += "\"status\":\"" + fields[6] + "\",";
    result += "\"status_label\":\"" + statusLabel + "\",";
    result += "\"foto_url\":\"" + fotoUrl + "\"";
    result += "}";
  }
  f.close();
  result += "]";
  return result;
}

// ============================================================
//   SPIFFS — DATA SISWA JSON
// ============================================================
String readStudents() {
  if (!SPIFFS.exists(STUDENTS_FILE)) {
    fs::File f = SPIFFS.open(STUDENTS_FILE, FILE_WRITE);
    f.print("{\"students\":[]}");
    f.close();
  }
  fs::File f = SPIFFS.open(STUDENTS_FILE, FILE_READ);
  String s = f.readString();
  f.close();
  return s;
}

bool writeStudents(const String& json) {
  fs::File f = SPIFFS.open(STUDENTS_FILE, FILE_WRITE);
  if (!f) return false;
  f.print(json);
  f.close();
  return true;
}

struct SiswaInfo { String name; String nisn; String className; String fotoUrl; bool found; };

SiswaInfo getSiswaById(int id) {
  SiswaInfo info = {"Unknown", "-", "-", "", false};
  JsonDocument doc;
  deserializeJson(doc, readStudents());
  for (JsonObject s : doc["students"].as<JsonArray>()) {
    if (s["fingerprint_device_id"].as<int>() == id) {
      info.name      = s["name"].as<String>();
      info.nisn      = s["nisn"].as<String>();
      info.className = s["class_name"].as<String>();
      info.fotoUrl   = s["foto_url"] | "";
      info.found     = true;
      return info;
    }
  }
  return info;
}

bool addSiswa(int fingerId, const String& name, const String& nisn, const String& className, const String& fotoUrl = "") {
  JsonDocument doc;
  deserializeJson(doc, readStudents());
  JsonArray arr = doc["students"].as<JsonArray>();
  // Update jika fingerprint_device_id sudah ada
  for (JsonObject s : arr) {
    if (s["fingerprint_device_id"].as<int>() == fingerId) {
      s["name"]       = name;
      s["nisn"]       = nisn;
      s["class_name"] = className;
      if (fotoUrl.length() > 0) s["foto_url"] = fotoUrl;
      String out; serializeJson(doc, out);
      return writeStudents(out);
    }
  }
  // Tambah baru
  JsonObject ns = arr.add<JsonObject>();
  ns["fingerprint_device_id"] = fingerId;
  ns["name"]       = name;
  ns["nisn"]       = nisn;
  ns["class_name"] = className;
  ns["foto_url"] = fotoUrl;
  String out; serializeJson(doc, out);
  return writeStudents(out);
}

bool deleteSiswa(int fingerId) {
  JsonDocument doc;
  deserializeJson(doc, readStudents());
  JsonDocument newDoc;
  JsonArray newArr = newDoc["students"].to<JsonArray>();
  for (JsonObject s : doc["students"].as<JsonArray>()) {
    if (s["fingerprint_device_id"].as<int>() != fingerId) {
      JsonObject ns = newArr.add<JsonObject>();
      ns["fingerprint_device_id"] = s["fingerprint_device_id"];
      ns["name"]       = s["name"];
      ns["nisn"]       = s["nisn"];
      ns["class_name"] = s["class_name"];
    }
  }
  String out; serializeJson(newDoc, out);
  return writeStudents(out);
}

int getNextId() {
  JsonDocument doc;
  deserializeJson(doc, readStudents());
  bool used[128] = {false};
  for (JsonObject s : doc["students"].as<JsonArray>()) {
    int i = s["fingerprint_device_id"].as<int>();
    if (i >= 1 && i <= 127) used[i] = true;
  }
  for (int i = 1; i <= 127; i++) if (!used[i]) return i;
  return -1;
}

int countSiswa() {
  JsonDocument doc;
  deserializeJson(doc, readStudents());
  int c = 0;
  for (JsonObject s : doc["students"].as<JsonArray>()) c++;
  return c;
}


struct AttendanceStats {
  int totalSiswa;
  int hadir;
  int sisa;
  int ditolak;
  int record;
  int persen;
};

AttendanceStats getAttendanceStatsToday() {
  AttendanceStats st;
  st.totalSiswa = countSiswa();
  st.hadir = 0;
  st.sisa = st.totalSiswa;
  st.ditolak = 0;
  st.record = 0;
  st.persen = 0;

  bool counted[128] = {false};
  String logJson = readLogToday();

  if (logJson != "[]") {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, logJson);
    if (!err) {
      for (JsonObject r : doc.as<JsonArray>()) {
        st.record++;
        String status = r["status"].as<String>();
        int fid = r["fingerprint_device_id"].as<int>();

        if (status == "present" || status == "sick" || status == "permission") {
          if (fid >= 1 && fid <= 127 && !counted[fid]) {
            counted[fid] = true;
            st.hadir++;
          }
        } else {
          st.ditolak++;
        }
      }
    }
  }

  if (st.totalSiswa > 0) {
    st.sisa = st.totalSiswa - st.hadir;
    if (st.sisa < 0) st.sisa = 0;
    st.persen = (st.hadir * 100) / st.totalSiswa;
  }

  return st;
}

void updateLastAbsensiPopup(const String& nama, int fingerId,
                            const String& statusLabel, const String& jam,
                            const String& kelas, const String& nisn,
                            const String& fotoUrl) {
  lastAbsensiSeq++;
  lastAbsensiJson = "{";
  lastAbsensiJson += "\"seq\":" + String(lastAbsensiSeq) + ",";
  lastAbsensiJson += "\"name\":\"" + jsonEscape(nama) + "\",";
  lastAbsensiJson += "\"fingerprint_device_id\":" + String(fingerId) + ",";
  lastAbsensiJson += "\"status_label\":\"" + jsonEscape(statusLabel) + "\",";
  lastAbsensiJson += "\"tapped_at\":\"" + jsonEscape(jam) + "\",";
  lastAbsensiJson += "\"class_name\":\"" + jsonEscape(kelas) + "\",";
  lastAbsensiJson += "\"nisn\":\"" + jsonEscape(nisn) + "\",";
  lastAbsensiJson += "\"foto_url\":\"" + jsonEscape(fotoUrl) + "\"";
  lastAbsensiJson += "}";
}

// ============================================================
//   KAMERA
// ============================================================
bool initCamera() {
  camera_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));

  cfg.ledc_channel = LEDC_CHANNEL_0;
  cfg.ledc_timer   = LEDC_TIMER_0;
  cfg.pin_d0 = Y2_GPIO_NUM; cfg.pin_d1 = Y3_GPIO_NUM;
  cfg.pin_d2 = Y4_GPIO_NUM; cfg.pin_d3 = Y5_GPIO_NUM;
  cfg.pin_d4 = Y6_GPIO_NUM; cfg.pin_d5 = Y7_GPIO_NUM;
  cfg.pin_d6 = Y8_GPIO_NUM; cfg.pin_d7 = Y9_GPIO_NUM;
  cfg.pin_xclk     = XCLK_GPIO_NUM;
  cfg.pin_pclk     = PCLK_GPIO_NUM;
  cfg.pin_vsync    = VSYNC_GPIO_NUM;
  cfg.pin_href     = HREF_GPIO_NUM;
  cfg.pin_sccb_sda = SIOD_GPIO_NUM;
  cfg.pin_sccb_scl = SIOC_GPIO_NUM;
  cfg.pin_pwdn     = PWDN_GPIO_NUM;
  cfg.pin_reset    = RESET_GPIO_NUM;
  cfg.xclk_freq_hz = 10000000;          // 10MHz — lebih stabil (dari kode kamu)
  cfg.pixel_format = PIXFORMAT_RGB565;  // RGB565 → convert ke JPEG (dari kode kamu)
  cfg.frame_size   = FRAMESIZE_QQVGA;  // 160x120 default, bisa diubah
  cfg.fb_count     = 1;
  cfg.fb_location  = CAMERA_FB_IN_DRAM;
  cfg.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;

  esp_err_t err = esp_camera_init(&cfg);
  if (err != ESP_OK) { Serial.printf("[CAM] Gagal: 0x%x\n", err); return false; }

  // Verifikasi sensor
  sensor_t* s = esp_camera_sensor_get();
  if (!s) { Serial.println("[CAM] Sensor get GAGAL!"); return false; }
  Serial.printf("[CAM] Sensor PID: 0x%x\n", s->id.PID);
  s->set_framesize(s, FRAMESIZE_QQVGA);
  delay(300);

  // Test frame + convert JPEG
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) { Serial.println("[CAM] Frame test GAGAL!"); return false; }
  Serial.printf("[CAM] Frame OK: %d bytes, format: %d\n", fb->len, fb->format);
  uint8_t* jpg_buf = NULL; size_t jpg_len = 0;
  bool conv = frame2jpg(fb, 80, &jpg_buf, &jpg_len);
  esp_camera_fb_return(fb);
  if (!conv || !jpg_buf) { Serial.println("[CAM] JPEG convert GAGAL!"); if(jpg_buf) free(jpg_buf); return false; }
  Serial.printf("[CAM] JPEG convert OK: %d bytes\n", jpg_len);
  free(jpg_buf);

  Serial.println("[CAM] Siap!");
  return true;
}

// Ambil foto — convert RGB565 -> JPEG, return fb (caller harus fb_return!)
// CATATAN: fb->buf berisi JPEG hasil convert, bukan raw RGB565
camera_fb_t* ambilFoto() {
  delay(PHOTO_DELAY);
  digitalWrite(LED_FLASH_PIN, HIGH);
  delay(100);
  camera_fb_t* fb = esp_camera_fb_get();
  digitalWrite(LED_FLASH_PIN, LOW);
  if (!fb) { Serial.println("[CAM] Gagal capture"); return nullptr; }
  Serial.printf("[CAM] Frame: %d bytes\n", fb->len);
  return fb;  // caller convert ke JPEG saat simpan
}

// Convert frame RGB565 ke JPEG dan simpan ke SD
// Return: path file atau ""
String ambilFotoDanSimpan(int fingerId, const String& tanggal, const String& jam) {
  camera_fb_t* fb = ambilFoto();
  if (!fb) return "";

  uint8_t* jpg_buf = NULL;
  size_t   jpg_len = 0;
  bool conv = frame2jpg(fb, 80, &jpg_buf, &jpg_len);
  esp_camera_fb_return(fb);

  if (!conv || !jpg_buf) {
    Serial.println("[CAM] Convert JPEG gagal");
    if (jpg_buf) free(jpg_buf);
    return "";
  }

  // Simpan ke SD
  String jamClean = jam; jamClean.replace(":", "");
  String path = String(SD_FOTO_DIR) + "/" + tanggal + "_" + jamClean + "_id" + fingerId + ".jpg";
  fs::File f = SD.open(path, FILE_WRITE);
  if (f) {
    f.write(jpg_buf, jpg_len);
    f.close();
    Serial.printf("[CAM] Foto simpan: %s (%d bytes)\n", path.c_str(), jpg_len);
  } else {
    Serial.println("[CAM] Gagal buka file SD");
    path = "";
  }
  free(jpg_buf);
  return path;
}

// ============================================================
//   FINGERPRINT
// ============================================================
bool initFingerprint() {
  mySerial.begin(FP_BAUD, SERIAL_8N1, FP_RX_PIN, FP_TX_PIN);
  finger.begin(FP_BAUD);
  if (finger.verifyPassword()) {
    Serial.printf("[FP] OK! Kapasitas: %d\n", finger.capacity);
    return true;
  }
  Serial.println("[FP] Sensor tidak ditemukan!");
  return false;
}

int scanFP() {
  if (finger.getImage() != FINGERPRINT_OK)      return -2;
  if (finger.image2Tz() != FINGERPRINT_OK)      return -1;
  if (finger.fingerSearch() != FINGERPRINT_OK)  return -1;
  return finger.fingerID;
}

bool deleteFP(int id) {
  return finger.deleteModel(id) == FINGERPRINT_OK;
}

// ============================================================
//   BLYNK NOTIFIKASI
// ============================================================
void kirimBlynk(const String& nama, int id,
                const String& status, const String& jam) {
  if (!wifiOk) return;
  if (!Blynk.connected()) {
    Serial.println("[BLYNK] Tidak terkoneksi, skip");
    return;
  }

  AttendanceStats st = getAttendanceStatsToday();
  String rasio = String(st.hadir) + "/" + String(st.totalSiswa) + " (" + String(st.persen) + "%)";

  // Datastream Blynk:
  // V0 = Nama_Siswa        (String)
  // V1 = ID_Fingerprint    (Integer)
  // V2 = Status            (String)
  // V3 = Jam_Absensi       (String)
  // V4 = Hadir_Hari_Ini    (Integer)
  // V5 = Foto_URL          (String, dikirim di prosesAbsensi jika ada)
  // V6 = Persen_Hadir      (Integer 0-100)
  // V7 = Rasio_Hadir       (String, contoh: 16/20 (80%))
  // V8 = Sisa_Belum_Hadir  (Integer)
  Blynk.virtualWrite(V0, nama);
  Blynk.virtualWrite(V1, id);
  Blynk.virtualWrite(V2, status);
  Blynk.virtualWrite(V3, jam);
  Blynk.virtualWrite(V4, st.hadir);
  Blynk.virtualWrite(V6, st.persen);
  Blynk.virtualWrite(V7, rasio);
  Blynk.virtualWrite(V8, st.sisa);

  Serial.printf("[BLYNK] %s | ID:%d | %s | %s | Hadir:%d/%d (%d%%) | Sisa:%d\n",
    nama.c_str(), id, status.c_str(), jam.c_str(), st.hadir, st.totalSiswa, st.persen, st.sisa);
}

// ============================================================
//   PROSES ABSENSI
// ============================================================
void prosesAbsensi(int fingerId) {
  SiswaInfo info = getSiswaById(fingerId);
  String jam     = getJam();
  String tanggal = getTanggal();
  // Status sesuai Laravel enum: present/sick/permission/absent
  String status      = info.found ? "present" : "absent";
  String statusLabel = info.found ? "HADIR"   : "TIDAK DIKENAL";

  Serial.printf("[ABSEN] ID:%d | %s | %s | %s\n",
    fingerId, info.name.c_str(), statusLabel.c_str(), jam.c_str());

  // Ambil foto + convert RGB565->JPEG
  camera_fb_t* fb = ambilFoto();
  String fotoPath = "";
  String fotoUrl  = "";

  if (fb) {
    // Convert RGB565 → JPEG
    uint8_t* jpg_buf = NULL;
    size_t   jpg_len = 0;
    bool conv = frame2jpg(fb, 80, &jpg_buf, &jpg_len);
    esp_camera_fb_return(fb);

    if (conv && jpg_buf) {
      // Simpan ke SD Card
      String jamClean = jam; jamClean.replace(":", "");
      fotoPath = String(SD_FOTO_DIR) + "/" + tanggal + "_" + jamClean + "_id" + fingerId + ".jpg";
      fs::File f = SD.open(fotoPath, FILE_WRITE);
      if (f) { f.write(jpg_buf, jpg_len); f.close(); }
      else fotoPath = "";
      free(jpg_buf);
    }
  }

  // Append log ke SD
  appendLogSD(tanggal, fingerId, info.name, info.nisn,
              info.className, status, jam, fotoPath);

  if (fotoPath.length() > 0) {
    fotoUrl = "/foto?path=" + fotoPath;
    if (info.found) addSiswa(fingerId, info.name, info.nisn, info.className, fotoUrl);
  } else if (info.fotoUrl.length() > 0) {
    fotoUrl = info.fotoUrl;
  }

  updateLastAbsensiPopup(info.name, fingerId, statusLabel, jam,
                         info.className, info.nisn, fotoUrl);

  // Blynk notifikasi
  if (info.found) {
    totalHadir++;
    kirimBlynk(info.name, fingerId, statusLabel, jam);
    // Kirim URL foto ke Blynk V5 jika ada
    if (fotoUrl.length() > 0) {
      Blynk.virtualWrite(V5, fotoUrl);
      Serial.printf("[BLYNK] Foto URL → V5: %s\n", fotoUrl.c_str());
    }
    ledBlink(2, 200);
  } else {
    kirimBlynk("TIDAK DIKENAL", fingerId, "ALPHA", jam);
    ledBlink(5, 80);
  }
}

// ============================================================
//   WEB SERVER — HTML HELPER
// ============================================================
const char CSS[] PROGMEM = R"(
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:Arial,sans-serif;background:#f0f2f5;color:#333}
.hdr{background:linear-gradient(135deg,#1a73e8,#0d47a1);color:white;padding:16px 20px;display:flex;justify-content:space-between;align-items:center}
.hdr h1{font-size:20px}.hdr small{opacity:.8;font-size:12px}
.nav{background:white;padding:10px 20px;display:flex;gap:8px;flex-wrap:wrap;box-shadow:0 2px 4px rgba(0,0,0,.1)}
.nav a{padding:8px 14px;border-radius:8px;text-decoration:none;font-size:14px;font-weight:bold}
.nav a.active,.nav a:hover{background:#1a73e8;color:white}
.nav a{background:#e8f0fe;color:#1a73e8}
.wrap{max-width:1100px;margin:0 auto;padding:16px}
.card{background:white;border-radius:12px;padding:18px;margin-bottom:16px;box-shadow:0 2px 8px rgba(0,0,0,.08)}
.card h2{font-size:17px;margin-bottom:14px;padding-bottom:10px;border-bottom:2px solid #f0f2f5}
.stats{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:12px;margin-bottom:16px}
.stat{background:white;border-radius:12px;padding:16px;text-align:center;box-shadow:0 2px 8px rgba(0,0,0,.08)}
.stat .n{font-size:38px;font-weight:bold;color:#1a73e8}
.stat .l{color:#666;font-size:13px;margin-top:4px}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(180px,1fr));gap:12px}
.scard{background:white;border-radius:12px;padding:14px;text-align:center;box-shadow:0 2px 8px rgba(0,0,0,.08);border:1px solid #eee}
.scard img,.scard .avi{width:80px;height:80px;border-radius:50%;margin-bottom:8px;object-fit:cover;border:3px solid #1a73e8}
.scard .avi{background:#e8f0fe;display:flex;align-items:center;justify-content:center;font-size:32px;margin:0 auto 8px}
.scard .nm{font-weight:bold;font-size:14px}.scard .kl{color:#888;font-size:12px;margin:2px 0}
.badge{display:inline-block;padding:3px 10px;border-radius:20px;font-size:11px;font-weight:bold}
.bh{background:#e6f4ea;color:#137333}.bd{background:#fce8e6;color:#c5221f}
.scard .jm{color:#aaa;font-size:11px;margin-top:4px}
.scard .fid{background:#e8f0fe;color:#1a73e8;padding:2px 7px;border-radius:10px;font-size:10px;margin-top:4px;display:inline-block}
input,select{width:100%;padding:10px;margin:6px 0 12px;border:1px solid #ddd;border-radius:8px;font-size:15px}
label{font-size:13px;font-weight:bold;color:#555}
.btn{display:inline-block;padding:11px 22px;border:none;border-radius:8px;cursor:pointer;font-size:15px;text-decoration:none;margin:4px}
.bb{background:#1a73e8;color:white}.bg{background:#34a853;color:white}
.br{background:#ea4335;color:white}.bgy{background:#666;color:white}
.ok{color:#34a853;font-weight:bold}.err{color:#ea4335;font-weight:bold}
#msg{padding:12px;border-radius:8px;margin:10px 0;display:none}
.info{background:#e8f0fe;color:#1a73e8}.suc{background:#e6f4ea;color:#137333}.fail{background:#fce8e6;color:#c5221f}
table{width:100%;border-collapse:collapse}
th{background:#1a73e8;color:white;padding:10px;text-align:left;font-size:13px}
td{padding:8px 10px;border-bottom:1px solid #eee;font-size:13px}
tr:hover{background:#f9f9f9}
.chip{background:#e8f0fe;color:#1a73e8;padding:2px 8px;border-radius:10px;font-size:11px}
</style>
)";

String pageWrap(const String& title, const String& active, const String& body) {
  String h = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  h += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  h += "<title>" + title + " - Sistem Absensi</title>";
  h += String(CSS);
  h += "</head><body>";
  h += "<div class='hdr'><div><h1>🏫 Sistem Presensi</h1><small>ESP32-CAM + AS608 + SD Card</small></div>";
  h += "<div style='text-align:right;font-size:13px'>";
  h += "<div id='clk' style='font-size:18px;font-weight:bold'>--:--:--</div>";
  h += "<div id='tgl' style='opacity:.8'></div></div></div>";
  h += "<div class='nav'>";
  h += "<a href='/' class='" + String(active=="home"?"active":"") + "'>🏠 Home</a>";
  h += "<a href='/enroll' class='" + String(active=="enroll"?"active":"") + "'>➕ Daftar</a>";
  h += "<a href='/siswa' class='" + String(active=="siswa"?"active":"") + "'>👥 Siswa</a>";
  h += "<a href='/log' class='" + String(active=="log"?"active":"") + "'>📋 Log</a>";
  h += "<a href='/cam' class='" + String(active=="cam"?"active":"") + "'>📷 Kamera</a>";
  h += "<a href='/setting' class='" + String(active=="setting"?"active":"") + "'>⚙️ Setting</a>";
  h += "</div><div class='wrap'>" + body + "</div>";
  h += "<script>";
  h += "function tick(){var n=new Date();";
  h += "document.getElementById('clk').textContent=n.toLocaleTimeString('id-ID');";
  h += "document.getElementById('tgl').textContent=n.toLocaleDateString('id-ID',{weekday:'long',day:'numeric',month:'long',year:'numeric'});}";
  h += "setInterval(tick,1000);tick();";
  h += "</script></body></html>";
  return h;
}

// ============================================================
//   ROUTE: HOME / DASHBOARD
// ============================================================
void handleHome() {
  String logJson = readLogToday();
  AttendanceStats st = getAttendanceStatsToday();
  String rasio = String(st.hadir) + "/" + String(st.totalSiswa);

  String body = "<div class='stats'>";
  body += "<div class='stat'><div class='n' id='statRasio'>" + rasio + "</div><div class='l'>✅ Rasio Hadir Hari Ini</div></div>";
  body += "<div class='stat'><div class='n' id='statPersen'>" + String(st.persen) + "%</div><div class='l'>📊 Persentase Kehadiran</div></div>";
  body += "<div class='stat'><div class='n' id='statSisa'>" + String(st.sisa) + "</div><div class='l'>⏳ Belum Hadir</div></div>";
  body += "<div class='stat'><div class='n' id='statTolak'>" + String(st.ditolak) + "</div><div class='l'>❌ Ditolak/Tidak Dikenal</div></div>";
  body += "</div>";

  body += "<div class='card'><h2>📊 Persentase Absensi Hari Ini</h2>";
  body += "<div style='display:flex;justify-content:space-between;font-size:14px;margin-bottom:8px'>";
  body += "<span><b id='barRasio'>" + rasio + "</b> siswa sudah absen</span><span><b id='barPersenText'>" + String(st.persen) + "%</b></span></div>";
  body += "<div style='height:20px;background:#e5e7eb;border-radius:20px;overflow:hidden'>";
  body += "<div id='barPersen' style='height:100%;width:" + String(st.persen) + "%;background:#34a853;border-radius:20px;text-align:center;color:white;font-size:12px;line-height:20px'>" + String(st.persen) + "%</div>";
  body += "</div><p style='font-size:12px;color:#666;margin-top:8px'>Sisa belum absen: <b id='barSisa'>" + String(st.sisa) + "</b> siswa dari total <b id='barTotal'>" + String(st.totalSiswa) + "</b>.</p></div>";

  // Status hardware
  // Tombol reset log hari ini
  body += "<div class='card' style='padding:12px 18px'>";
  body += "<span style='font-size:13px;color:#666'>Log hari ini: <b>" + getTanggal() + "</b></span> &nbsp;";
  body += "<a href='/reset-log' class='btn br' style='padding:5px 12px;font-size:12px' ";
  body += "onclick='return confirm(\"Hapus semua log hari ini?\")'> 🗑️ Reset Log Hari Ini</a>";
  body += "</div>";

  body += "<div class='card'><h2>⚙️ Status Hardware</h2><table>";
  body += "<tr><td>📷 Kamera</td><td>" + String(camOk ? "<span class='ok'>✅ OK</span>" : "<span class='err'>❌ Gagal</span>") + "</td></tr>";
  body += "<tr><td>👆 Fingerprint</td><td>" + String(fpOk ? "<span class='ok'>✅ OK</span>" : "<span class='err'>❌ Gagal</span>") + "</td></tr>";
  body += "<tr><td>💾 SD Card</td><td>" + String(sdOk ? "<span class='ok'>✅ OK</span>" : "<span class='err'>❌ Gagal</span>") + "</td></tr>";
  body += "<tr><td>🌐 WiFi</td><td>" + String(wifiOk ? "<span class='ok'>✅ " + WiFi.localIP().toString() + "</span>" : "<span class='err'>❌ Offline</span>") + "</td></tr>";
  body += "<tr><td>📡 Mode</td><td><span class='chip'>" + String(sysMode == MODE_ABSENSI ? "ABSENSI" : "ENROLLMENT") + "</span></td></tr>";
  body += "</table></div>";

  // Rekap absensi hari ini
  body += "<div class='card'><h2>📋 Absensi Hari Ini</h2>";
  body += "<div class='grid' id='cards'>⏳ Memuat...</div>";
  body += "<script>";
  body += "function setText(id,v){var e=document.getElementById(id);if(e)e.textContent=v;}";
  body += "function renderStats(j){if(!j)return;setText('statRasio',j.rasio||'0/0');setText('statPersen',(j.persen_hadir||0)+'%');setText('statSisa',j.total_sisa||0);setText('statTolak',j.total_tolak||0);setText('barRasio',j.rasio||'0/0');setText('barPersenText',(j.persen_hadir||0)+'%');setText('barSisa',j.total_sisa||0);setText('barTotal',j.total_siswa||0);var b=document.getElementById('barPersen');if(b){b.style.width=(j.persen_hadir||0)+'%';b.textContent=(j.persen_hadir||0)+'%';}}";
  body += "function renderCards(data){var el=document.getElementById('cards');if(!el)return;";
  body += "if(!data||data.length===0){el.innerHTML='<p style=\"color:#999;padding:20px\">📭 Belum ada absensi hari ini</p>';return;}";
  body += "el.innerHTML='';data.slice().reverse().forEach(function(d){";
  body += "var foto=d.foto_url?'<img src=\"'+d.foto_url+'\" onerror=\"this.outerHTML=\\\'<div class=avi>👤</div>\\\'\">':'<div class=\"avi\">👤</div>';";
  body += "var bc=(d.status_label==='HADIR'||d.status_label==='SAKIT'||d.status_label==='IZIN')?'bh':'bd';";
  body += "el.innerHTML+='<div class=scard>'+foto+'<div class=nm>'+d.name+'</div><div class=kl>'+d.class_name+'</div><span class=\"badge '+bc+'\">'+d.status_label+'</span><div class=jm>🕐 '+d.tapped_at+'</div><div class=fid>ID: '+d.fingerprint_device_id+'</div></div>';";
  body += "});}";
  body += "renderCards(" + logJson + ");";
  body += "</script></div>";

  body += "<div id='absenPopup' style='display:none;position:fixed;z-index:9999;left:0;top:0;width:100%;height:100%;background:rgba(0,0,0,.45);align-items:center;justify-content:center;padding:18px'>";
  body += "<div style='background:white;border-radius:18px;max-width:380px;width:100%;padding:18px;box-shadow:0 20px 60px rgba(0,0,0,.25);text-align:center;position:relative'>";
  body += "<button onclick='closeAbsenPopup()' style='position:absolute;right:12px;top:10px;border:0;background:#eee;border-radius:50%;width:28px;height:28px;font-weight:bold'>×</button>";
  body += "<h2 style='margin:6px 0 12px'>✅ Absensi Berhasil</h2>";
  body += "<div id='popFoto'><div style='width:160px;height:120px;margin:auto;border-radius:12px;background:#eee;display:flex;align-items:center;justify-content:center;font-size:50px'>👤</div></div>";
  body += "<h3 id='popNama' style='margin:12px 0 4px'>-</h3>";
  body += "<p id='popInfo' style='color:#666;font-size:14px;margin-bottom:8px'>-</p>";
  body += "<span id='popStatus' class='badge bh'>HADIR</span>";
  body += "<p id='popJam' style='margin-top:10px;color:#555'>-</p>";
  body += "</div></div>";
  body += "<script>";
  body += "var popState={boot:'',seq:0,ready:false,showing:false,timer:null};";
  body += "function savePopState(){}";
  body += "function closeAbsenPopup(){var p=document.getElementById('absenPopup');if(p)p.style.display='none';popState.showing=false;if(popState.timer)clearTimeout(popState.timer);}";
  body += "function showAbsenPopup(d){if(!d||!d.seq)return;popState.showing=true;if(popState.timer)clearTimeout(popState.timer);";
  body += "document.getElementById('popNama').textContent=d.name||'-';";
  body += "document.getElementById('popInfo').textContent='ID '+d.fingerprint_device_id+' • '+(d.class_name||'-')+' • NISN '+(d.nisn||'-');";
  body += "document.getElementById('popStatus').textContent=d.status_label||'-';";
  body += "document.getElementById('popStatus').className='badge '+((d.status_label==='HADIR'||d.status_label==='SAKIT'||d.status_label==='IZIN')?'bh':'bd');";
  body += "document.getElementById('popJam').textContent='Jam: '+(d.tapped_at||'-');";
  body += "var empty='<div style=\"width:160px;height:120px;margin:auto;border-radius:12px;background:#eee;display:flex;align-items:center;justify-content:center;font-size:50px\">👤</div>';";
  body += "var src=d.foto_url?(d.foto_url+(d.foto_url.indexOf('?')>-1?'&':'?')+'t='+Date.now()):'';";
  body += "document.getElementById('popFoto').innerHTML=src?'<img src=\"'+src+'\" style=\"width:180px;height:135px;object-fit:cover;border-radius:12px;border:2px solid #34a853\" onerror=\"this.outerHTML=\\\''+empty+'\\\'\">':empty;";
  body += "document.getElementById('absenPopup').style.display='flex';popState.timer=setTimeout(closeAbsenPopup,8000)}";
  body += "function pollDash(){fetch('/api?ts='+Date.now(),{cache:'no-store'}).then(r=>r.json()).then(j=>{renderStats(j);renderCards(j.data);var d=j.latest_absensi||{};var b=j.popup_boot_id||'';var seq=d&&d.seq?Number(d.seq):0;if(!popState.ready||popState.boot!==b){popState.boot=b;popState.seq=seq;popState.ready=true;return;}if(seq>popState.seq){popState.seq=seq;showAbsenPopup(d);}}).catch(e=>{});}";
  body += "pollDash();setInterval(pollDash,1200);";
  body += "</script>";

  server.send(200, "text/html", pageWrap("Dashboard", "home", body));
}

// ============================================================
//   ROUTE: ENROLLMENT
// ============================================================
void handleEnroll() {
  int nextId = getNextId();
  String body = "<div class='card'><h2>➕ Daftarkan Siswa Baru</h2>";

  if (nextId == -1) {
    body += "<p class='err'>❌ Kapasitas penuh (127 siswa)!</p>";
  } else if (enroll.active) {
    body += "<p class='err'>⚠️ Sedang ada enrollment aktif. Selesaikan dulu.</p>";
  } else {
    body += "<div id='msg'></div>";
    body += "<label>Nama Siswa</label>";
    body += "<input type='text' id='ename' placeholder='Contoh: Budi Santoso'>";
    body += "<label>NISN</label>";
    body += "<input type='text' id='enisn' placeholder='Contoh: 1234567890' maxlength='10'>";
    body += "<label>Kelas / Jenis Kebutuhan Khusus</label>";
    body += "<select id='eclass'>";
    body += "<option value='Autis'>Autis</option>";
    body += "<option value='Tuna Rungu'>Tuna Rungu</option>";
    body += "<option value='Tuna Grahita'>Tuna Grahita</option>";
    body += "<option value='Tuna Daksa'>Tuna Daksa</option>";
    body += "<option value='Tuna Netra'>Tuna Netra</option>";
    body += "<option value='Tuna Wicara'>Tuna Wicara</option>";
    body += "<option value='Lainnya'>Lainnya</option>";
    body += "</select>";
    body += "<label>ID Finger (1-127)</label>";
    body += "<input type='number' id='fid' value='" + String(nextId) + "' min='1' max='127'>";
    body += "<button class='btn bg' onclick='mulaiEnroll()'>🚀 Mulai Enrollment</button>";
  }

  body += "</div>";
  body += "<div class='card'><h2>📝 Petunjuk</h2>";
  body += "<ol style='line-height:2;padding-left:20px'>";
  body += "<li>Isi nama, kelas, dan ID siswa</li>";
  body += "<li>Klik <b>Mulai Enrollment</b></li>";
  body += "<li>Tempelkan jari ke sensor <b>(Scan 1)</b></li>";
  body += "<li>Angkat jari, tunggu instruksi</li>";
  body += "<li>Tempelkan jari lagi <b>(Scan 2)</b></li>";
  body += "<li>✅ Selesai!</li>";
  body += "</ol></div>";

  body += "<script>";
  body += "function showMsg(t,c){var m=document.getElementById('msg');m.textContent=t;m.className=c;m.style.display='block';}";
  body += "function mulaiEnroll(){";
  body += "  var n=document.getElementById('ename').value.trim();";
  body += "  var ns=document.getElementById('enisn').value.trim();";
  body += "  var k=document.getElementById('eclass').value;";
  body += "  var i=document.getElementById('fid').value;";
  body += "  if(!n||!i){showMsg('⚠️ Nama dan ID wajib diisi!','info msg');return;}";
  body += "  showMsg('⏳ Memulai enrollment...','info msg');";
  body += "  fetch('/enroll-start?name='+encodeURIComponent(n)+'&nisn='+encodeURIComponent(ns)+'&class_name='+encodeURIComponent(k)+'&id='+i)";
  body += "  .then(r=>r.json()).then(d=>{";
  body += "    showMsg(d.message, d.ok?'suc msg':'fail msg');";
  body += "    if(d.ok) pollEnroll();";
  body += "  });";
  body += "}";
  body += "function pollEnroll(){";
  body += "  fetch('/enroll-poll').then(r=>r.json()).then(d=>{";
  body += "    showMsg(d.message, d.done?(d.success?'suc msg':'fail msg'):'info msg');";
  body += "    if(!d.done) setTimeout(pollEnroll, 800);";
  body += "    else if(d.success) setTimeout(()=>location.href='/siswa',2000);";
  body += "  });";
  body += "}";
  body += "</script>";

  server.send(200, "text/html", pageWrap("Enrollment", "enroll", body));
}

void handleEnrollStart() {
  sendCorsHeaders();
  String name      = server.arg("name");
  String nisn      = server.arg("nisn");
  String className = server.arg("class_name");
  int id           = server.arg("id").toInt();

  JsonDocument resp;
  if (name.isEmpty() || id < 1 || id > 127) {
    resp["ok"] = false; resp["message"] = "Nama dan ID wajib diisi!";
  } else if (enroll.active) {
    resp["ok"] = false; resp["message"] = "Enrollment lain sedang berjalan!";
  } else {
    enroll.active    = true;
    enroll.id        = id;
    enroll.name      = name;
    enroll.nisn      = nisn;
    enroll.className = className;
    enroll.step      = 1;
    enroll.statusMsg = "👆 Tempelkan jari PERTAMA ke sensor...";
    sysMode = MODE_ENROLLMENT;
    resp["ok"]      = true;
    resp["message"] = enroll.statusMsg;
  }
  String out; serializeJson(resp, out);
  server.send(200, "application/json", out);
}


String captureEnrollPhotoUrl(int fingerId) {
  if (!camOk || !sdOk) return "";
  camera_fb_t* fb = ambilFoto();
  if (!fb) return "";
  uint8_t* jpg_buf = NULL;
  size_t jpg_len = 0;
  bool conv = frame2jpg(fb, 80, &jpg_buf, &jpg_len);
  esp_camera_fb_return(fb);
  if (!conv || !jpg_buf) return "";
  String path = String(SD_FOTO_DIR) + "/siswa_id" + String(fingerId) + ".jpg";
  fs::File f = SD.open(path, FILE_WRITE);
  if (f) { f.write(jpg_buf, jpg_len); f.close(); }
  else path = "";
  free(jpg_buf);
  if (path.length() == 0) return "";
  return "/foto?path=" + path;
}

void handleEnrollPoll() {
  sendCorsHeaders();
  JsonDocument resp;
  resp["done"]    = false;
  resp["success"] = false;
  resp["message"] = enroll.statusMsg;

  if (!enroll.active) {
    resp["done"] = true; resp["message"] = "Tidak ada enrollment aktif";
    String out; serializeJson(resp, out);
    server.send(200, "application/json", out);
    return;
  }

  if (enroll.step == 1) {
    // Pastikan loop absensi tidak ganggu
    sysMode = MODE_ENROLLMENT;

    int p = finger.getImage();
    if (p == FINGERPRINT_OK) {
      p = finger.image2Tz(1);
      if (p == FINGERPRINT_OK) {
        enroll.step = 2;
        enroll.statusMsg = "✅ Scan 1 OK! Angkat jari, lalu tempel lagi...";
        // Tunggu jari diangkat
        delay(500);
        while (finger.getImage() != FINGERPRINT_NOFINGER) delay(100);
      } else {
        enroll.statusMsg = "❌ Kualitas buruk, coba lagi...";
      }
    }
  } else if (enroll.step == 2) {
    // Pastikan loop absensi tidak ganggu
    sysMode = MODE_ENROLLMENT;

    int p = finger.getImage();
    if (p == FINGERPRINT_NOFINGER) {
      enroll.statusMsg = "👆 Tempelkan jari KEDUA (konfirmasi)...";
    } else if (p == FINGERPRINT_OK) {
      // Jari sudah ditempel — proses scan kedua
      p = finger.image2Tz(2);
      if (p == FINGERPRINT_OK) {
        p = finger.createModel();
        if (p == FINGERPRINT_OK) {
          p = finger.storeModel(enroll.id);
          if (p == FINGERPRINT_OK) {
            String fotoUrl = captureEnrollPhotoUrl(enroll.id);
            addSiswa(enroll.id, enroll.name, enroll.nisn, enroll.className, fotoUrl);
            enroll.active = false;
            sysMode = MODE_ABSENSI;
            resp["done"] = true; resp["success"] = true;
            resp["message"] = "✅ Enrollment berhasil! " + enroll.name + " terdaftar.";
            ledBlink(3, 150);
          } else {
            enroll.active = false; sysMode = MODE_ABSENSI;
            resp["done"] = true;
            resp["message"] = "❌ Gagal simpan ke sensor (error " + String(p) + ")";
          }
        } else {
          // Model tidak cocok — ulangi dari awal
          enroll.step = 1;
          enroll.statusMsg = "❌ Scan tidak cocok, ulangi dari scan pertama...";
          while (finger.getImage() != FINGERPRINT_NOFINGER) delay(100);
        }
      } else {
        enroll.statusMsg = "❌ Kualitas buruk, coba lagi...";
      }
    }
  }

  resp["message"] = enroll.statusMsg;
  String out; serializeJson(resp, out);
  server.send(200, "application/json", out);
}


String latestFotoUrlForSiswa(int fingerId) {
  if (!sdOk || fingerId <= 0) return "";

  String profilePath = String(SD_FOTO_DIR) + "/siswa_id" + String(fingerId) + ".jpg";
  if (SD.exists(profilePath)) return "/foto?path=" + profilePath;

  fs::File dir = SD.open(SD_FOTO_DIR);
  if (!dir || !dir.isDirectory()) return "";

  String needle = "_id" + String(fingerId) + ".jpg";
  String best = "";
  time_t bestTime = 0;

  fs::File file = dir.openNextFile();
  while (file) {
    String name = String(file.name());
    if (!file.isDirectory() && name.endsWith(needle)) {
      time_t t = file.getLastWrite();
      if (best.length() == 0 || t >= bestTime) {
        best = name;
        bestTime = t;
      }
    }
    file = dir.openNextFile();
  }

  if (best.length() == 0) return "";
  if (!best.startsWith("/")) best = String(SD_FOTO_DIR) + "/" + best;
  return "/foto?path=" + best;
}

// ============================================================
//   ROUTE: DAFTAR SISWA
// ============================================================
void handleSiswa() {
  JsonDocument doc;
  deserializeJson(doc, readStudents());
  JsonArray arr = doc["students"].as<JsonArray>();

  int cnt = 0;
  for (JsonObject s : arr) cnt++;

  String body = "<div class='card'><h2>👥 Data Siswa Terdaftar</h2>";
  body += "<p style='margin-bottom:12px'>Total: <b>" + String(cnt) + "</b> siswa";
  body += " &nbsp; <a href='/enroll' class='btn bg' style='padding:6px 12px;font-size:13px'>➕ Tambah</a></p>";

  if (cnt == 0) {
    body += "<p style='color:#999;padding:20px 0'>Belum ada siswa terdaftar.</p>";
  } else {
    body += "<table><tr><th>Foto</th><th>ID Finger</th><th>Nama</th><th>NISN</th><th>Kelas/JKK</th><th>Aksi</th></tr>";
    for (JsonObject s : arr) {
      int    fid  = s["fingerprint_device_id"].as<int>();
      String name = s["name"].as<String>();
      String nisn = s["nisn"].as<String>();
      String cls  = s["class_name"].as<String>();
      String foto = s["foto_url"] | "";
      if (foto.length() == 0) foto = latestFotoUrlForSiswa(fid);
      String fotoBust = foto;
      if (fotoBust.length() > 0) fotoBust += (fotoBust.indexOf('?') >= 0 ? "&" : "?") + String("t=") + String(millis());
      String fotoHtml = fotoBust.length() > 0
        ? "<img src='" + fotoBust + "' style='width:64px;height:52px;object-fit:cover;border-radius:8px;border:1px solid #ddd' onerror=\"this.outerHTML='<div class=avi style=\\\"width:64px;height:52px;font-size:22px\\\">👤</div>'\">"
        : "<div class='avi' style='width:64px;height:52px;font-size:22px'>👤</div>";
      body += "<tr><td>" + fotoHtml + "</td><td><span class='chip'>" + String(fid) + "</span></td>";
      body += "<td>" + name + "</td>";
      body += "<td>" + nisn + "</td>";
      body += "<td>" + cls  + "</td>";
      body += "<td><a href='/hapus?id=" + String(fid) + "' class='btn br' style='padding:5px 10px;font-size:12px' ";
      body += "onclick='return confirm(\"Hapus " + name + "?\")'> 🗑️</a></td></tr>";
    }
    body += "</table>";
  }
  body += "</div>";
  server.send(200, "text/html", pageWrap("Data Siswa", "siswa", body));
}

void handleHapus() {
  int id = server.arg("id").toInt();
  if (id > 0) { deleteSiswa(id); deleteFP(id); }
  server.sendHeader("Location", "/siswa");
  server.send(302);
}

// ============================================================
//   ROUTE: LOG HARIAN
// ============================================================
void handleLog() {
  // List file CSV di SD
  String body = "<div class='card'><h2>📋 Log Absensi Harian</h2>";

  if (!sdOk) {
    body += "<p class='err'>❌ SD Card tidak tersedia</p>";
  } else {
    body += "<p style='margin-bottom:12px'>File log tersimpan di SD Card <code>/log/</code></p>";
    fs::File dir = SD.open(SD_LOG_DIR);
    if (dir) {
      body += "<table><tr><th>Tanggal</th><th>File</th><th>Aksi</th></tr>";
      fs::File f = dir.openNextFile();
      while (f) {
        String fname = String(f.name());
        String tanggal = fname;
        tanggal.replace(".csv", "");
        body += "<tr><td>" + tanggal + "</td>";
        body += "<td><code>" + fname + "</code></td>";
        body += "<td><a href='/log-view?tgl=" + tanggal + "' class='btn bb' style='padding:5px 10px;font-size:12px'>👁️ Lihat</a></td></tr>";
        f = dir.openNextFile();
      }
      body += "</table>";
      dir.close();
    }
  }
  body += "</div>";
  server.send(200, "text/html", pageWrap("Log", "log", body));
}

void handleLogView() {
  String tanggal = server.arg("tgl");
  if (tanggal.isEmpty()) tanggal = getTanggal();

  String logFile = String(SD_LOG_DIR) + "/" + tanggal + ".csv";
  String body = "<div class='card'><h2>📋 Log: " + tanggal + "</h2>";
  body += "<a href='/log' class='btn bgy' style='padding:6px 12px;font-size:13px;margin-bottom:12px'>← Kembali</a><br><br>";

  if (!sdOk || !SD.exists(logFile)) {
    body += "<p class='err'>File tidak ditemukan</p>";
  } else {
    fs::File f = SD.open(logFile, FILE_READ);
    if (f) {
      body += "<div class='grid'>";
      bool first = true;
      while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (first) { first = false; continue; } // skip header CSV
        if (line.isEmpty()) continue;

        // CSV: attendance_date,tapped_at,finger_id,name,nisn,class_name,status,foto
        // index:      0            1          2      3    4       5        6     7
        String fields[8];  // 8 kolom!
        int col = 0, prev = 0;
        for (int i = 0; i <= (int)line.length() && col < 8; i++) {
          if (i == (int)line.length() || line[i] == ',') {
            fields[col++] = line.substring(prev, i);
            prev = i + 1;
          }
        }

        // Foto
        String fotoHtml = "";
        if (fields[7].length() > 0) {
          String fotoUrl = "/foto?path=" + fields[7];
          fotoHtml = "<img src='" + fotoUrl + "' style='width:80px;height:80px;border-radius:50%;object-fit:cover;border:3px solid #1a73e8;margin-bottom:8px'>";
        } else {
          fotoHtml = "<div class='avi'>👤</div>";
        }

        // Status label
        String stLabel = fields[6];
        if      (stLabel == "present")    stLabel = "HADIR";
        else if (stLabel == "sick")       stLabel = "SAKIT";
        else if (stLabel == "permission") stLabel = "IZIN";
        else if (stLabel == "absent")     stLabel = "ALPHA";

        String bc = (fields[6]=="present"||fields[6]=="sick"||fields[6]=="permission") ? "bh" : "bd";

        body += "<div class='scard'>" + fotoHtml;
        body += "<div class='nm'>"    + fields[3] + "</div>";
        body += "<div class='kl'>"    + fields[5] + "</div>";
        body += "<span class='badge " + bc + "'>" + stLabel + "</span>";
        body += "<div class='jm'>🕐 " + fields[1] + "</div>";
        body += "<div class='fid'>ID: " + fields[2] + " | NISN: " + fields[4] + "</div>";
        body += "</div>";  // tutup scard
      }
      body += "</div>";  // tutup grid
      f.close();
    }
  }
  body += "</div>";  // tutup card
  server.send(200, "text/html", pageWrap("Log View", "log", body));
}


// ============================================================
//   UTIL: CORS + JSON helper
// ============================================================
void sendCorsHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

void handleOptions() {
  sendCorsHeaders();
  server.send(204, "text/plain", "");
}

// ============================================================
//   ROUTE: SERVE FOTO DARI SD
// ============================================================
void handleFoto() {
  sendCorsHeaders();
  String path = server.arg("path");
  if (path.isEmpty() || !sdOk) {
    server.send(404, "text/plain", "Not found");
    return;
  }
  if (!SD.exists(path)) {
    server.send(404, "text/plain", "File not found");
    return;
  }
  fs::File f = SD.open(path, FILE_READ);
  if (!f) {
    server.send(500, "text/plain", "Open failed");
    return;
  }
  server.streamFile(f, "image/jpeg");
  f.close();
}

// ============================================================
//   ROUTE: API JSON (untuk polling dashboard)
// ============================================================
void handleApi() {
  sendCorsHeaders();
  String logJson = readLogToday();
  AttendanceStats st = getAttendanceStatsToday();

  String ip = wifiOk ? WiFi.localIP().toString() : WiFi.softAPIP().toString();

  String resp = "{";
  resp += "\"total_hadir\":" + String(st.hadir) + ",";
  resp += "\"total_siswa\":" + String(st.totalSiswa) + ",";
  resp += "\"total_sisa\":" + String(st.sisa) + ",";
  resp += "\"persen_hadir\":" + String(st.persen) + ",";
  resp += "\"total_tolak\":" + String(st.ditolak) + ",";
  resp += "\"total_record\":" + String(st.record) + ",";
  resp += "\"rasio\":\"" + String(st.hadir) + "/" + String(st.totalSiswa) + "\",";
  resp += "\"sd_ok\":" + String(sdOk ? "true" : "false") + ",";
  resp += "\"wifi_ok\":" + String(wifiOk ? "true" : "false") + ",";
  resp += "\"esp32_ip\":\"" + jsonEscape(ip) + "\",";
  resp += "\"popup_boot_id\":\"" + jsonEscape(popupBootId) + "\",";
  resp += "\"latest_absensi\":" + lastAbsensiJson + ",";
  resp += "\"data\":" + logJson;
  resp += "}";
  server.send(200, "application/json", resp);
}

void handleStatus() {
  sendCorsHeaders();
  String ip = wifiOk ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  String resp = "{";
  resp += "\"wifi_ok\":" + String(wifiOk ? "true" : "false") + ",";
  resp += "\"mode\":\"" + String(wifiOk ? "station" : "access_point") + "\",";
  resp += "\"esp32_ip\":\"" + jsonEscape(ip) + "\"";
  resp += "}";
  server.send(200, "application/json", resp);
}

void handleStudents() {
  sendCorsHeaders();
  JsonDocument doc;
  deserializeJson(doc, readStudents());
  JsonArray arr = doc["students"].as<JsonArray>();
  String out;
  serializeJson(arr, out);
  server.send(200, "application/json", out);
}

void handleNotFound() {
  sendCorsHeaders();
  server.send(404, "application/json", "{\"error\":\"Not found\"}");
}

// ============================================================
//   ROUTE: RESET LOG HARI INI
// ============================================================
void handleResetLog() {
  if (sdOk) {
    String logFile = String(SD_LOG_DIR) + "/" + getTanggal() + ".csv";
    if (SD.exists(logFile)) {
      SD.remove(logFile);
      Serial.println("[RESET] Log hari ini dihapus: " + logFile);
    }
  }
  totalHadir = 0;
  server.sendHeader("Location", "/");
  server.send(302);
}

// ============================================================
//   ROUTE: DEBUG — lihat isi SPIFFS langsung
// ============================================================
void handleDebug() {
  String raw = readStudents();
  Serial.println("[DEBUG] SPIFFS students.json:");
  Serial.println(raw);

  String body = "<div class='card'><h2>🔍 Debug SPIFFS</h2>";
  body += "<h3>students.json raw:</h3>";
  body += "<pre style='background:#f5f5f5;padding:12px;border-radius:8px;overflow:auto'>";
  body += raw;
  body += "</pre>";

  // Parse dan tampilkan
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, raw);
  if (err) {
    body += "<p style='color:red'>❌ JSON parse error: " + String(err.c_str()) + "</p>";
  } else {
    int cnt = 0;
    for (JsonObject s : doc["students"].as<JsonArray>()) cnt++;
    body += "<p>✅ JSON valid | Jumlah siswa: <b>" + String(cnt) + "</b></p>";
  }

  body += "<br><a href='/siswa' class='btn bb'>← Kembali</a>";
  body += "</div>";
  server.send(200, "text/html", pageWrap("Debug", "siswa", body));
}

// ============================================================
//   ROUTE: PENGATURAN WIFI
// ============================================================
void handleWifiConfig() {
  WifiCfg cur = loadWifiCfg();
  String ip   = wifiOk ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  String mode = apMode ? "AP Mode (Setup)" : "Client Mode";

  String body = "<div class='card'><h2>📶 Pengaturan WiFi</h2>";
  body += "<table style='margin-bottom:15px'>";
  body += "<tr><td>Mode</td><td><span class='chip'>" + mode + "</span></td></tr>";
  body += "<tr><td>IP Address</td><td><b>" + ip + "</b></td></tr>";
  body += "<tr><td>SSID Aktif</td><td>" + WiFi.SSID() + "</td></tr>";
  body += "<tr><td>Signal</td><td>" + String(WiFi.RSSI()) + " dBm</td></tr>";
  body += "</table>";

  body += "<div id='wmsg'></div>";
  body += "<label>SSID (Nama WiFi)</label>";
  body += "<input type='text' id='wssid' value='" + cur.ssid + "' placeholder='Nama WiFi kamu'>";
  body += "<label>Password WiFi</label>";
  body += "<input type='password' id='wpass' placeholder='Password WiFi'>";
  body += "<small style='color:#999;display:block;margin:-8px 0 12px'>Kosongkan jika tidak ingin mengubah password</small>";
  body += "<button class='btn bg' onclick='simpanWifi()'>💾 Simpan & Restart</button>";
  body += "</div>";

  body += "<div class='card'><h2>📡 Scan Jaringan WiFi</h2>";
  body += "<button class='btn bb' onclick='scanWifi()'>🔍 Scan Sekarang</button>";
  body += "<div id='scanResult' style='margin-top:12px'></div>";
  body += "</div>";

  body += "<script>";
  body += "function simpanWifi(){";
  body += "  var s=document.getElementById('wssid').value.trim();";
  body += "  var p=document.getElementById('wpass').value;";
  body += "  if(!s){showWMsg('⚠️ SSID tidak boleh kosong!','fail msg');return;}";
  body += "  if(p.length>0&&p.length<8){showWMsg('⚠️ Password minimal 8 karakter!','fail msg');return;}";
  body += "  showWMsg('⏳ Menyimpan dan restart...','info msg');";
  body += "  fetch('/wifi-save?ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(p))";
  body += "  .then(r=>r.json()).then(d=>{";
  body += "    showWMsg(d.message,'suc msg');";
  body += "    setTimeout(()=>location.href='/',5000);";
  body += "  }).catch(()=>showWMsg('✅ Tersimpan! Restart...','suc msg'));";
  body += "}";
  body += "function showWMsg(t,c){var m=document.getElementById('wmsg');m.textContent=t;m.className=c;m.style.display='block';}";
  body += "function scanWifi(){";
  body += "  document.getElementById('scanResult').innerHTML='⏳ Scanning...';";
  body += "  fetch('/wifi-scan').then(r=>r.json()).then(d=>{";
  body += "    var h='<table><tr><th>SSID</th><th>Signal</th><th>Enkripsi</th><th></th></tr>';";
  body += "    d.networks.forEach(function(n){";
  body += "      h+='<tr><td>'+n.ssid+'</td><td>'+n.rssi+' dBm</td><td>'+n.enc+'</td>';";
  body += "      h+='<td><button class=\"btn bb\" style=\"padding:4px 8px;font-size:12px\" onclick=\"pilihSSID(\\'' + n.ssid + '\\')\" >Pilih</button></td></tr>';";
  body += "    });";
  body += "    h+='</table>';";
  body += "    document.getElementById('scanResult').innerHTML=h;";
  body += "  });";
  body += "}";
  body += "function pilihSSID(s){document.getElementById('wssid').value=s;document.getElementById('wpass').focus();}";
  body += "</script>";

  server.send(200, "text/html", pageWrap("WiFi Config", "setting", body));
}

void handleWifiSave() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");

  // Jika pass kosong, pakai pass lama
  if (pass.isEmpty()) {
    WifiCfg cur = loadWifiCfg();
    pass = cur.pass;
  }

  saveWifiCfg(ssid, pass);

  JsonDocument resp;
  resp["ok"]      = true;
  resp["message"] = "✅ Tersimpan! ESP32 akan restart dalam 3 detik...";
  String out; serializeJson(resp, out);
  server.send(200, "application/json", out);

  delay(3000);
  ESP.restart();
}

void handleWifiScan() {
  int n = WiFi.scanNetworks();
  String resp = "{\"networks\":[";
  for (int i = 0; i < n; i++) {
    if (i > 0) resp += ",";
    String enc = WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "Open" : "WPA/WPA2";
    resp += "{\"ssid\":\"" + WiFi.SSID(i) + "\",";
    resp += "\"rssi\":"   + String(WiFi.RSSI(i)) + ",";
    resp += "\"enc\":\""  + enc + "\"}";
  }
  resp += "]}";
  server.send(200, "application/json", resp);
}


// ============================================================
//   OTA GITHUB RELEASE
// ============================================================
String jsonEscape(const String &in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '\\') out += "\\\\";
    else if (c == '"') out += "\\\"";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "";
    else out += c;
  }
  return out;
}

int versionCompare(String a, String b) {
  a.replace("v", "");
  a.replace("V", "");
  b.replace("v", "");
  b.replace("V", "");

  int av[3] = {0, 0, 0};
  int bv[3] = {0, 0, 0};

  sscanf(a.c_str(), "%d.%d.%d", &av[0], &av[1], &av[2]);
  sscanf(b.c_str(), "%d.%d.%d", &bv[0], &bv[1], &bv[2]);

  for (int i = 0; i < 3; i++) {
    if (av[i] > bv[i]) return 1;
    if (av[i] < bv[i]) return -1;
  }
  return 0;
}

bool checkGithubRelease(String &msg) {
  if (WiFi.status() != WL_CONNECTED) {
    msg = "WiFi belum konek ke router";
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  String apiUrl = "https://api.github.com/repos/" + String(GITHUB_OWNER) + "/" + String(GITHUB_REPO) + "/releases/latest";

  otaStatus = "Menghubungi GitHub...";
  http.begin(client, apiUrl);
  http.addHeader("User-Agent", "ufim-absensi-esp32");
  http.addHeader("Accept", "application/vnd.github+json");
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    msg = "GitHub API gagal. HTTP " + String(code);
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    msg = "JSON GitHub gagal dibaca";
    return false;
  }

  latestTag = doc["tag_name"].as<String>();
  latestNotes = doc["body"].as<String>();
  latestBinUrl = "";

  JsonArray assets = doc["assets"].as<JsonArray>();
  for (JsonObject asset : assets) {
    String name = asset["name"].as<String>();
    if (name == OTA_BIN_NAME || name.endsWith(".bin")) {
      latestBinUrl = asset["browser_download_url"].as<String>();
      break;
    }
  }

  if (latestTag.length() == 0) {
    msg = "Tag release tidak ditemukan";
    return false;
  }

  if (latestBinUrl.length() == 0) {
    msg = "Asset firmware .bin tidak ditemukan di release " + latestTag;
    return false;
  }

  if (versionCompare(latestTag, FW_VERSION) <= 0) {
    msg = "Firmware sudah terbaru. Current v" FW_VERSION ", Latest " + latestTag;
    return false;
  }

  msg = "Update tersedia: " + latestTag;
  return true;
}

void otaTask(void *param) {
  otaRunning = true;
  otaDone = false;
  otaPercent = 0;
  otaStatus = "Cek release terbaru...";

  String msg;
  if (!checkGithubRelease(msg)) {
    otaStatus = msg;
    otaRunning = false;
    vTaskDelete(NULL);
    return;
  }

  otaStatus = "Download firmware " + latestTag + "...";
  otaPercent = 1;

  WiFiClientSecure client;
  client.setInsecure();

  httpUpdate.rebootOnUpdate(false);
  httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

  httpUpdate.onStart([]() {
    otaStatus = "Mulai update firmware...";
    otaPercent = 2;
    Serial.println("[OTA] Start");
  });

  httpUpdate.onProgress([](int cur, int total) {
    if (total > 0) {
      otaPercent = (cur * 100) / total;
      if (otaPercent < 2) otaPercent = 2;
      if (otaPercent > 99) otaPercent = 99;
    }
    otaStatus = "Downloading " + String(otaPercent) + "%";
    Serial.printf("[OTA] %d%% (%d/%d)\n", otaPercent, cur, total);
  });

  httpUpdate.onEnd([]() {
    otaPercent = 100;
    otaDone = true;
    otaStatus = "Update selesai. Restart...";
    Serial.println("[OTA] End");
  });

  httpUpdate.onError([](int err) {
    otaStatus = "OTA error " + String(err) + ": " + httpUpdate.getLastErrorString();
    Serial.printf("[OTA] Error %d: %s\n", err, httpUpdate.getLastErrorString().c_str());
  });

  Serial.println("[OTA] URL: " + latestBinUrl);
  t_httpUpdate_return ret = httpUpdate.update(client, latestBinUrl);

  if (ret == HTTP_UPDATE_OK) {
    otaPercent = 100;
    otaStatus = "Update OK. Restart dalam 2 detik...";
    otaRunning = false;
    delay(2000);
    ESP.restart();
  } else if (ret == HTTP_UPDATE_NO_UPDATES) {
    otaStatus = "Tidak ada update";
    otaRunning = false;
  } else {
    otaStatus = "Update gagal: " + httpUpdate.getLastErrorString();
    Serial.printf(
      "[OTA] FAILED (%d): %s\n",
      httpUpdate.getLastError(),
      httpUpdate.getLastErrorString().c_str()
    );
    otaRunning = false;
  }

  vTaskDelete(NULL);
}

void handleOtaStart() {
  if (otaRunning) {
    server.send(200, "application/json", "{\"ok\":false,\"msg\":\"OTA sedang berjalan\"}");
    return;
  }

  otaPercent = 0;
  otaDone = false;
  otaStatus = "Menyiapkan OTA...";

  xTaskCreatePinnedToCore(
    otaTask,
    "otaTask",
    12288,
    NULL,
    1,
    NULL,
    0
  );

  server.send(200, "application/json", "{\"ok\":true,\"msg\":\"OTA dimulai\"}");
}

void handleOtaProgress() {
  String json = "{";
  json += "\"version\":\"v" FW_VERSION "\",";
  json += "\"latest\":\"" + jsonEscape(latestTag) + "\",";
  json += "\"percent\":" + String(otaPercent) + ",";
  json += "\"running\":" + String(otaRunning ? "true" : "false") + ",";
  json += "\"done\":" + String(otaDone ? "true" : "false") + ",";
  json += "\"status\":\"" + jsonEscape(otaStatus) + "\",";
  json += "\"notes\":\"" + jsonEscape(latestNotes) + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

// ============================================================
//   ROUTE: HALAMAN PENGATURAN (Hub)
// ============================================================
void handleSetting() {
  String body = "<div class='card'><h2>⚙️ Pengaturan Sistem</h2>";
  body += "<div style='display:grid;grid-template-columns:repeat(auto-fit,minmax(240px,1fr));gap:12px'>";

  // WiFi Card
  body += "<div style='border:1px solid #e0e0e0;border-radius:10px;padding:16px'>";
  body += "<div style='font-size:28px;margin-bottom:8px'>📶</div>";
  body += "<h3 style='margin-bottom:6px'>Konfigurasi WiFi</h3>";
  body += "<p style='color:#666;font-size:13px;margin-bottom:12px'>Ganti SSID dan password WiFi tanpa perlu upload ulang kode</p>";
  body += "<a href='/wifi-config' class='btn bb'>Buka Pengaturan</a>";
  body += "</div>";

  // OTA Card
  body += "<div style='border:1px solid #e0e0e0;border-radius:10px;padding:16px'>";
  body += "<div style='font-size:28px;margin-bottom:8px'>⬆️</div>";
  body += "<h3 style='margin-bottom:6px'>Update Firmware OTA</h3>";
  body += "<p style='color:#666;font-size:13px;margin-bottom:12px'>Update dari GitHub Release: technometric/ufim-absensi</p>";
  body += "<p style='font-size:13px'><b>Versi saat ini:</b> v" FW_VERSION "</p>";
  body += "<button class='btn bg' onclick='startOta()'>Update Firmware</button>";
  body += "<div style='width:100%;height:22px;background:#eee;border-radius:20px;overflow:hidden;margin-top:12px'>";
  body += "<div id='otaBar' style='width:0%;height:100%;background:#34a853;color:white;text-align:center;font-size:12px;line-height:22px'>0%</div>";
  body += "</div>";
  body += "<div id='otaStatus' style='font-size:13px;color:#555;margin-top:8px'>Idle</div>";
  body += "<pre id='otaNotes' style='white-space:pre-wrap;background:#f5f5f5;padding:10px;border-radius:8px;margin-top:8px;max-height:120px;overflow:auto;font-size:12px'></pre>";
  body += "</div>";

  // Info Card
  body += "<div style='border:1px solid #e0e0e0;border-radius:10px;padding:16px'>";
  body += "<div style='font-size:28px;margin-bottom:8px'>ℹ️</div>";
  body += "<h3 style='margin-bottom:6px'>Info Sistem</h3>";
  body += "<table style='font-size:13px'>";
  body += "<tr><td style='color:#666'>Firmware</td><td><b>v" FW_VERSION "</b></td></tr>";
  body += "<tr><td style='color:#666'>Free Heap</td><td>" + String(ESP.getFreeHeap()/1024) + " KB</td></tr>";
  body += "<tr><td style='color:#666'>Flash</td><td>" + String(ESP.getFlashChipSize()/1024/1024) + " MB</td></tr>";
  body += "<tr><td style='color:#666'>Uptime</td><td>" + String(millis()/60000) + " menit</td></tr>";
  body += "</table></div>";

  body += "</div></div>";

  body += "<script>";
  body += "let otaTimer=null;";
  body += "function setOta(p,s,n){document.getElementById('otaBar').style.width=p+'%';document.getElementById('otaBar').textContent=p+'%';document.getElementById('otaStatus').textContent=s||'';if(n){document.getElementById('otaNotes').textContent=n;}}";
  body += "function pollOta(){fetch('/ota-progress').then(r=>r.json()).then(j=>{setOta(j.percent,j.status,j.notes);if(!j.running&&j.percent>=100){clearInterval(otaTimer);setTimeout(()=>location.reload(),5000);}else if(!j.running&&j.percent<100&&j.status!='Idle'){clearInterval(otaTimer);}}).catch(e=>{});}";
  body += "function startOta(){if(!confirm('Update firmware dari GitHub sekarang?'))return;setOta(0,'Menyiapkan OTA...','');fetch('/ota-start').then(r=>r.json()).then(j=>{document.getElementById('otaStatus').textContent=j.msg;if(otaTimer)clearInterval(otaTimer);otaTimer=setInterval(pollOta,800);pollOta();});}";
  body += "pollOta();";
  body += "</script>";

  server.send(200, "text/html", pageWrap("Pengaturan", "setting", body));
}

// ============================================================
//   STREAM KAMERA (MJPEG via WebServer)
// ============================================================
void handleStream() {
  WiFiClient client = server.client();

  // Header multipart MJPEG
  String header =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n";
  client.print(header);

  Serial.println("[STREAM] Client konek");

  while (client.connected()) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) { delay(30); continue; }

    uint8_t* jpg_buf = NULL;
    size_t   jpg_len = 0;
    bool conv = frame2jpg(fb, 80, &jpg_buf, &jpg_len);
    esp_camera_fb_return(fb);

    if (!conv || !jpg_buf) {
      if (jpg_buf) free(jpg_buf);
      delay(30);
      continue;
    }

    // Kirim frame MJPEG
    String part =
      "--frame\r\n"
      "Content-Type: image/jpeg\r\n"
      "Content-Length: " + String(jpg_len) + "\r\n\r\n";
    client.print(part);
    client.write(jpg_buf, jpg_len);
    client.print("\r\n");

    free(jpg_buf);
    delay(50);  // ~20fps max
  }
  Serial.println("[STREAM] Client disconnect");
}

void handleCamPage() {
  String ip = wifiOk ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  String body = "<div class='card'><h2>📷 Live Camera Stream</h2>";
  body += "<p>Stream URL: <code>http://" + ip + "/stream</code></p>";
  body += "<img src='/stream' style='width:100%;max-width:640px;border-radius:8px;border:2px solid #1a73e8'>";
  body += "<br><br><small style='color:#999'>Format: MJPEG | Resolusi: QQVGA 160x120 | ~20fps</small>";
  body += "</div>";
  server.send(200, "text/html", pageWrap("Camera", "cam", body));
}

// ============================================================
//   SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n===================================");
  Serial.println("  SISTEM ABSENSI PURE ESP32-CAM");
  Serial.println("===================================");
  popupBootId = String((uint32_t)(ESP.getEfuseMac() & 0xFFFFFFFF), HEX) + "-" + String(millis());

  pinMode(LED_ONBOARD, OUTPUT);
  digitalWrite(LED_ONBOARD, HIGH);
  pinMode(LED_FLASH_PIN, OUTPUT);
  digitalWrite(LED_FLASH_PIN, LOW);

  // SPIFFS
  spiffsOk = SPIFFS.begin(true);
  Serial.println(spiffsOk ? "[SPIFFS] OK" : "[SPIFFS] GAGAL");

  // Kamera
  camOk = initCamera();

  // SD Card (1-bit mode)
  sdOk = initSD();

  // Fingerprint
  fpOk = initFingerprint();

  // WiFi — load config dari SPIFFS, fallback AP mode
  wifiOk = connectWifi();
  if (wifiOk) {
    configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov");
    Serial.println("[NTP] Sinkronisasi waktu...");
    ledBlink(3, 100);
  } else {
    ledBlink(10, 100);  // AP mode sudah aktif dari connectWifi()
  }



  // Web Server Routes
  server.enableCORS(true);
  server.on("/",             HTTP_GET, handleHome);
  server.on("/enroll",       HTTP_GET, handleEnroll);
  server.on("/siswa",        HTTP_GET, handleSiswa);
  server.on("/hapus",        HTTP_GET, handleHapus);
  server.on("/log",          HTTP_GET, handleLog);
  server.on("/log-view",     HTTP_GET, handleLogView);
  server.on("/setting",      HTTP_GET, handleSetting);
  server.on("/ota-start",    HTTP_GET, handleOtaStart);
  server.on("/ota-progress", HTTP_GET, handleOtaProgress);
  server.on("/debug",        HTTP_GET, handleDebug);
  server.on("/reset-log",    HTTP_GET, handleResetLog);
  server.on("/wifi-config",  HTTP_GET, handleWifiConfig);
  server.on("/wifi-save",    HTTP_GET, handleWifiSave);
  server.on("/wifi-scan",    HTTP_GET, handleWifiScan);
  server.on("/stream",       HTTP_GET, handleStream);
  server.on("/cam",          HTTP_GET, handleCamPage);
  server.on("/api",          HTTP_GET, handleApi);
  server.on("/foto",         HTTP_GET, handleFoto);
  server.on("/status",       HTTP_GET, handleStatus);
  server.on("/students",     HTTP_GET, handleStudents);
  server.on("/enroll-start", HTTP_GET, handleEnrollStart);
  server.on("/enroll-poll",  HTTP_GET, handleEnrollPoll);
  server.on("/api",          HTTP_OPTIONS, handleOptions);
  server.on("/foto",         HTTP_OPTIONS, handleOptions);
  server.on("/status",       HTTP_OPTIONS, handleOptions);
  server.on("/students",     HTTP_OPTIONS, handleOptions);
  server.on("/enroll-start", HTTP_OPTIONS, handleOptions);
  server.on("/enroll-poll",  HTTP_OPTIONS, handleOptions);
  server.onNotFound(handleNotFound);
  server.begin();

  // Tampilkan semua cara akses
  String staIp = WiFi.localIP().toString();
  String apIp  = WiFi.softAPIP().toString();
  String apSSID = "Absensi-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  Serial.println();
  Serial.println("=========================================");
  Serial.println("  SISTEM ABSENSI SIAP!");
  Serial.println("-----------------------------------------");
  if (wifiOk) {
    Serial.printf("  [Router]  SSID : %s\n", WiFi.SSID().c_str());
    Serial.printf("  [Router]  IP   : http://%s\n", staIp.c_str());
    } else {
    Serial.println("  [Router]  GAGAL konek");
  }
  Serial.println("-----------------------------------------");
  Serial.printf("  [Hotspot] SSID : %s\n", apSSID.c_str());
  Serial.printf("  [Hotspot] Pass : 12345678\n");
  Serial.printf("  [Hotspot] IP   : http://%s\n", apIp.c_str());
  Serial.println("-----------------------------------------");
  Serial.println("  URL Penting:");
  Serial.println("  /          -> Dashboard Absensi");
  Serial.println("  /cam       -> Live Camera");
  Serial.println("  /setting   -> Pengaturan WiFi");
  Serial.println("=========================================");
}

// ============================================================
//   LOOP
// ============================================================
void loop() {
  server.handleClient();

  // Blynk keep-alive — status online di dashboard
  if (wifiOk) Blynk.run();

  // Skip scan saat enrollment aktif
  if (sysMode == MODE_ENROLLMENT || enroll.active) {
    delay(10);
    return;
  }

  // --- MODE ABSENSI ---
  static unsigned long lastScan = 0;
  static int lastId = -1;

  int p = finger.getImage();
  if (p != FINGERPRINT_OK) { delay(50); return; }

  p = finger.image2Tz();
  if (p != FINGERPRINT_OK) { delay(50); return; }

  p = finger.fingerSearch();
  if (p == FINGERPRINT_OK) {
    int matchId = finger.fingerID;
    unsigned long now = millis();
    // Cooldown: hindari double-scan
    if (matchId != lastId || now - lastScan > SCAN_COOLDOWN) {
      lastScan = now;
      lastId   = matchId;
      prosesAbsensi(matchId);
    }
  } else if (p == FINGERPRINT_NOTFOUND) {
    Serial.println("[FP] Jari tidak dikenal!");
    kirimBlynk("TIDAK DIKENAL", 0, "DITOLAK", getJam());
    ledBlink(5, 80);
    delay(2000); // Cooldown jari tidak dikenal
  }

  delay(50);
}
