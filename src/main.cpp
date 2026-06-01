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
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <SD.h>
#include <SPI.h>
#include <Adafruit_Fingerprint.h>
#include <Update.h>
#include <Wire.h>
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "esp_camera.h"
#include "img_converters.h"
#include "config.h"

#define FW_VERSION     "1.0.1"
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

// OTA progress
size_t otaContentLen = 0;
size_t otaUploaded   = 0;

// ============================================================
//   WIFI CONFIG — BACA/TULIS SSID+PASS DI SPIFFS
// ============================================================

struct WifiCfg { String ssid; String pass; };

WifiCfg loadWifiCfg() {
  WifiCfg cfg = {WIFI_SSID, WIFI_PASSWORD};
  if (!SPIFFS.exists(WIFI_CFG_FILE)) return cfg;
  File f = SPIFFS.open(WIFI_CFG_FILE, FILE_READ);
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
  File f = SPIFFS.open(WIFI_CFG_FILE, FILE_WRITE);
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
  // Hotspot selalu aktif (192.168.4.1) + konek ke router
  String apName = "Absensi-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apName.c_str(), "12345678");
  Serial.printf("[AP] Hotspot: %s | IP: %s\n",
    apName.c_str(), WiFi.softAPIP().toString().c_str());

  // Konek ke router
  Serial.printf("[WiFi] Connecting: %s", cfg.ssid.c_str());
  WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < WIFI_TIMEOUT) {
    delay(400); Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] Client IP: %s\n", WiFi.localIP().toString().c_str());
    return true;
  }
  Serial.println("\n[WiFi] Router gagal, hotspot tetap aktif");
  return false;
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
//   SD CARD — 1-BIT MODE (hanya butuh GPIO 2)
// ============================================================
bool initSD() {
  // SPI mode — lebih stabil dengan WiFi AP+STA
  // Pin SPI ESP32-CAM: SCK=14, MISO=2, MOSI=15, CS=13
  SPI.begin(14, 2, 15, 13);

  // Coba mount dulu
  if (!SD.begin(13)) {
    Serial.println("[SD] Gagal mount! Coba format FAT32...");

    // Format otomatis pakai esp_vfs_fat
    // Lepas SPI dulu, coba via SD_MMC untuk format
    SD.end();

    // Pakai ff_mkfs dari fatfs untuk format
    FATFS fs;
    FRESULT res;
    const char* path = "/sdcard";

    // Mount paksa untuk format
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
      .format_if_mount_failed = true,  // ← auto format jika gagal!
      .max_files = 5,
      .allocation_unit_size = 16 * 1024
    };

    sdmmc_card_t* card;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = HSPI_HOST;

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs   = (gpio_num_t)13;
    slot_cfg.host_id   = (spi_host_device_t)host.slot;

    esp_err_t err = esp_vfs_fat_sdspi_mount(path, &host, &slot_cfg, &mount_cfg, &card);
    if (err == ESP_OK) {
      Serial.println("[SD] Format & mount berhasil!");
      esp_vfs_fat_sdcard_unmount(path, card);
      // Remount via Arduino SD library
      SPI.begin(14, 2, 15, 13);
      if (!SD.begin(13)) {
        Serial.println("[SD] Remount gagal setelah format");
        return false;
      }
    } else {
      Serial.printf("[SD] Format gagal: 0x%x\n", err);
      Serial.println("[SD] Kemungkinan: SD tidak terpasang / rusak");
      return false;
    }
  }

  uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("[SD] Tidak ada SD Card!");
    return false;
  }

  // Info SD Card
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

  File f = SD.open(path, FILE_WRITE);
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

  File f = SD.open(logFile, FILE_APPEND);
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

  File f = SD.open(logFile, FILE_READ);
  if (!f) return "[]";

  bool firstLine = true;
  bool firstEntry = true;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (firstLine) { firstLine = false; continue; } // skip header CSV
    if (line.isEmpty()) continue;

    // Parse CSV: tanggal,jam,finger_id,nama,kelas,status,foto
    int idx[7];
    int col = 0;
    int prev = 0;
    String fields[7];
    for (int i = 0; i <= line.length() && col < 7; i++) {
      if (i == line.length() || line[i] == ',') {
        fields[col++] = line.substring(prev, i);
        prev = i + 1;
      }
    }

    if (!firstEntry) result += ",";
    firstEntry = false;

    String fotoUrl = "";
    if (fields[6].length() > 0) {
      fotoUrl = "/foto?path=" + fields[6];
    }

    result += "{";
    result += "\"tanggal\":\""  + fields[0] + "\",";
    result += "\"jam\":\""      + fields[1] + "\",";
    result += "\"finger_id\":" + fields[2]  + ",";
    result += "\"nama\":\""     + fields[3] + "\",";
    result += "\"kelas\":\""    + fields[4] + "\",";
    result += "\"status\":\""   + fields[5] + "\",";
    result += "\"foto_url\":\"" + fotoUrl   + "\"";
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
    File f = SPIFFS.open(STUDENTS_FILE, FILE_WRITE);
    f.print("{\"students\":[]}");
    f.close();
  }
  File f = SPIFFS.open(STUDENTS_FILE, FILE_READ);
  String s = f.readString();
  f.close();
  return s;
}

bool writeStudents(const String& json) {
  File f = SPIFFS.open(STUDENTS_FILE, FILE_WRITE);
  if (!f) return false;
  f.print(json);
  f.close();
  return true;
}

struct SiswaInfo { String name; String nisn; String className; bool found; };

SiswaInfo getSiswaById(int id) {
  SiswaInfo info = {"Unknown", "-", "-", false};
  JsonDocument doc;
  deserializeJson(doc, readStudents());
  for (JsonObject s : doc["students"].as<JsonArray>()) {
    if (s["fingerprint_device_id"].as<int>() == id) {
      info.name      = s["name"].as<String>();
      info.nisn      = s["nisn"].as<String>();
      info.className = s["class_name"].as<String>();
      info.found     = true;
      return info;
    }
  }
  return info;
}

bool addSiswa(int fingerId, const String& name, const String& nisn, const String& className) {
  JsonDocument doc;
  deserializeJson(doc, readStudents());
  JsonArray arr = doc["students"].as<JsonArray>();
  // Update jika fingerprint_device_id sudah ada
  for (JsonObject s : arr) {
    if (s["fingerprint_device_id"].as<int>() == fingerId) {
      s["name"]       = name;
      s["nisn"]       = nisn;
      s["class_name"] = className;
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
  File f = SD.open(path, FILE_WRITE);
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
  HTTPClient http;
  String base = "https://blynk.cloud/external/api/update?token=";
  base += BLYNK_TOKEN;
  String pins[5][2] = {
    {"V0", nama}, {"V1", String(id)},
    {"V2", status}, {"V3", jam},
    {"V4", String(totalHadir)}
  };
  for (auto& p : pins) {
    http.begin(base + "&" + p[0] + "=" + p[1]);
    http.GET();
    http.end();
    delay(80);
  }
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

  // Ambil foto + convert RGB565->JPEG + simpan ke SD
  String fotoPath = ambilFotoDanSimpan(fingerId, tanggal, jam);

  // Append log ke SD (format sesuai struktur Laravel)
  appendLogSD(tanggal, fingerId, info.name, info.nisn,
              info.className, status, jam, fotoPath);

  // Blynk notifikasi
  if (info.found) {
    totalHadir++;
    kirimBlynk(info.name, fingerId, statusLabel, jam);
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

  // Hitung stats dari log
  int totalRec = 0, totalHadirToday = 0, totalTolak = 0;
  if (logJson != "[]") {
    JsonDocument doc;
    deserializeJson(doc, logJson);
    for (JsonObject r : doc.as<JsonArray>()) {
      totalRec++;
      String st = String(r["status"].as<const char*>());
      if (st == "present") totalHadirToday++;
      // sick & permission juga dihitung kehadiran (tidak alpha)
      else totalTolak++;
    }
  }

  String body = "<div class='stats'>";
  body += "<div class='stat'><div class='n'>" + String(totalHadirToday) + "</div><div class='l'>✅ Hadir Hari Ini</div></div>";
  body += "<div class='stat'><div class='n'>" + String(totalRec) + "</div><div class='l'>📋 Total Record</div></div>";
  body += "<div class='stat'><div class='n'>" + String(totalTolak) + "</div><div class='l'>❌ Ditolak</div></div>";
  body += "<div class='stat'><div class='n'>" + String(countSiswa()) + "</div><div class='l'>👥 Siswa Terdaftar</div></div>";
  body += "</div>";

  // Status hardware
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
  body += "var data=" + logJson + ";";
  body += "var el=document.getElementById('cards');";
  body += "if(!data||data.length===0){el.innerHTML='<p style=\"color:#999;padding:20px\">📭 Belum ada absensi hari ini</p>';}";
  body += "else{el.innerHTML='';data.reverse().forEach(function(d){";
  body += "var foto=d.foto_url?'<img src=\"'+d.foto_url+'\" onerror=\"this.outerHTML=\\'<div class=avi>👤</div>\\'\">':`<div class='avi'>👤</div>`;";
  body += "var bc=d.status_label==='HADIR'?'bh':(d.status_label==='SAKIT'?'bh':d.status_label==='IZIN'?'bh':'bd');";
  body += "el.innerHTML+='<div class=scard>'+foto+'<div class=nm>'+d.name+'</div><div class=kl>'+d.class_name+'</div><span class=\"badge '+bc+'\">'+d.status_label+'</span><div class=jm>🕐 '+d.tapped_at+'</div><div class=fid>ID: '+d.fingerprint_device_id+'</div></div>';";
  body += "});}";
  body += "</script></div>";

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

void handleEnrollPoll() {
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
    int p = finger.getImage();
    if (p == FINGERPRINT_OK) {
      p = finger.image2Tz(1);
      if (p == FINGERPRINT_OK) {
        enroll.step = 2;
        enroll.statusMsg = "✅ Scan 1 OK! Angkat jari, lalu tempel lagi...";
      } else {
        enroll.statusMsg = "❌ Kualitas buruk, coba lagi...";
      }
    }
  } else if (enroll.step == 2) {
    if (finger.getImage() == FINGERPRINT_NOFINGER) {
      // Tunggu jari diangkat, lalu scan kedua
      enroll.statusMsg = "👆 Tempelkan jari KEDUA (konfirmasi)...";
    } else {
      int p = finger.getImage();
      if (p == FINGERPRINT_OK) {
        p = finger.image2Tz(2);
        if (p == FINGERPRINT_OK) {
          p = finger.createModel();
          if (p == FINGERPRINT_OK) {
            p = finger.storeModel(enroll.id);
            if (p == FINGERPRINT_OK) {
              addSiswa(enroll.id, enroll.name, enroll.nisn, enroll.className);
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
            enroll.step = 1;
            enroll.statusMsg = "❌ Scan tidak cocok, ulangi dari scan pertama...";
          }
        }
      }
    }
  }

  resp["message"] = enroll.statusMsg;
  String out; serializeJson(resp, out);
  server.send(200, "application/json", out);
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
    body += "<table><tr><th>ID</th><th>Nama</th><th>Kelas</th><th>Aksi</th></tr>";
    for (JsonObject s : arr) {
      int    id   = s["id"].as<int>();
      String nama = s["nama"].as<String>();
      String kls  = s["kelas"].as<String>();
      body += "<tr><td><span class='chip'>" + String(id) + "</span></td>";
      body += "<td>" + nama + "</td><td>" + kls + "</td>";
      body += "<td><a href='/hapus?id=" + String(id) + "' class='btn br' style='padding:5px 10px;font-size:12px' ";
      body += "onclick='return confirm(\"Hapus " + nama + "?\")'>🗑️</a></td></tr>";
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
    File dir = SD.open(SD_LOG_DIR);
    if (dir) {
      body += "<table><tr><th>Tanggal</th><th>File</th><th>Aksi</th></tr>";
      File f = dir.openNextFile();
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
    File f = SD.open(logFile, FILE_READ);
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
//   ROUTE: SERVE FOTO DARI SD
// ============================================================
void handleFoto() {
  String path = server.arg("path");
  if (path.isEmpty() || !sdOk) {
    server.send(404, "text/plain", "Not found");
    return;
  }
  if (!SD.exists(path)) {
    server.send(404, "text/plain", "File not found");
    return;
  }
  File f = SD.open(path, FILE_READ);
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
  String logJson = readLogToday();
  int totalHadirToday = 0, totalTolak = 0;
  JsonDocument doc;
  deserializeJson(doc, logJson);
  for (JsonObject r : doc.as<JsonArray>()) {
    String st = String(r["status"].as<const char*>());
    if (st == "present" || st == "sick" || st == "permission") totalHadirToday++;
    else totalTolak++;
  }
  String resp = "{";
  resp += "\"total_hadir\":" + String(totalHadirToday) + ",";
  resp += "\"total_siswa\":" + String(countSiswa()) + ",";
  resp += "\"total_tolak\":" + String(totalTolak) + ",";
  resp += "\"sd_ok\":" + String(sdOk ? "true" : "false") + ",";
  resp += "\"data\":" + logJson;
  resp += "}";
  server.send(200, "application/json", resp);
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
//   ROUTE: OTA UPDATE
// ============================================================
void handleOtaPage() {
  String body = "<div class='card'><h2>🔄 OTA Firmware Update</h2>";
  body += "<p>Versi firmware saat ini: <span class='chip'>v" FW_VERSION "</span></p>";
  body += "<hr style='margin:14px 0'>";
  body += "<p style='margin-bottom:12px'>Upload file <b>.bin</b> firmware baru:</p>";

  // Form upload — pakai raw HTTP, bukan form tag
  body += "<div id='otaArea'>";
  body += "<input type='file' id='fwFile' accept='.bin' onchange='piliFile(this)'>";
  body += "<br><br>";
  body += "<button class='btn bg' id='btnUpload' onclick='uploadFW()' disabled>⬆️ Upload Firmware</button>";
  body += "</div>";

  body += "<div id='otaProgress' style='display:none;margin-top:15px'>";
  body += "<div style='background:#f0f2f5;border-radius:8px;overflow:hidden;height:24px'>";
  body += "<div id='progBar' style='background:#1a73e8;height:100%;width:0%;transition:width 0.3s;display:flex;align-items:center;justify-content:center;color:white;font-size:12px'>0%</div>";
  body += "</div>";
  body += "<p id='otaMsg' style='margin-top:8px;color:#666'>Mempersiapkan...</p>";
  body += "</div>";

  body += "</div>";

  body += "<div class='card'><h2>⚠️ Perhatian</h2>";
  body += "<ul style='line-height:2;padding-left:20px;color:#555'>";
  body += "<li>Gunakan file <b>.bin</b> dari PlatformIO (<code>.pio/build/esp32cam/firmware.bin</code>)</li>";
  body += "<li>Jangan matikan daya saat update berlangsung</li>";
  body += "<li>ESP32 akan <b>restart otomatis</b> setelah update berhasil</li>";
  body += "<li>Jika gagal, flash ulang via USB seperti biasa</li>";
  body += "</ul></div>";

  body += "<script>";
  body += "var selFile=null;";
  body += "function piliFile(input){";
  body += "  selFile=input.files[0];";
  body += "  if(selFile){";
  body += "    document.getElementById('btnUpload').disabled=false;";
  body += "    document.getElementById('btnUpload').textContent='⬆️ Upload: '+selFile.name+' ('+Math.round(selFile.size/1024)+' KB)';";
  body += "  }";
  body += "}";
  body += "function uploadFW(){";
  body += "  if(!selFile){alert('Pilih file .bin dulu!');return;}";
  body += "  if(!confirm('Upload firmware baru? ESP32 akan restart setelah selesai.')){return;}";
  body += "  document.getElementById('otaArea').style.display='none';";
  body += "  document.getElementById('otaProgress').style.display='block';";
  body += "  var xhr=new XMLHttpRequest();";
  body += "  xhr.open('POST','/ota-upload',true);";
  body += "  xhr.upload.onprogress=function(e){";
  body += "    if(e.lengthComputable){";
  body += "      var pct=Math.round(e.loaded/e.total*100);";
  body += "      document.getElementById('progBar').style.width=pct+'%';";
  body += "      document.getElementById('progBar').textContent=pct+'%';";
  body += "      document.getElementById('otaMsg').textContent='Uploading... '+pct+'%';";
  body += "    }";
  body += "  };";
  body += "  xhr.onload=function(){";
  body += "    if(xhr.status===200){";
  body += "      document.getElementById('otaMsg').textContent='✅ Update berhasil! ESP32 restart...';";
  body += "      document.getElementById('progBar').style.background='#34a853';";
  body += "      setTimeout(()=>{location.href='/';},8000);";
  body += "    } else {";
  body += "      document.getElementById('otaMsg').textContent='❌ Gagal: '+xhr.responseText;";
  body += "      document.getElementById('progBar').style.background='#ea4335';";
  body += "      document.getElementById('otaArea').style.display='block';";
  body += "    }";
  body += "  };";
  body += "  xhr.onerror=function(){";
  body += "    document.getElementById('otaMsg').textContent='❌ Koneksi terputus (mungkin sudah restart)';";
  body += "    setTimeout(()=>location.href='/',5000);";
  body += "  };";
  body += "  xhr.setRequestHeader('X-Filename', selFile.name);";
  body += "  xhr.send(selFile);";
  body += "}";
  body += "</script>";

  server.send(200, "text/html", pageWrap("OTA Update", "setting", body));
}

void handleOtaUpload() {
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("[OTA] Start: %s\n", upload.filename.c_str());
    otaUploaded = 0;

    // ⚠️ Unmount SD Card dulu — cegah timeout conflict saat OTA
    if (sdOk) {
      SD.end();
      Serial.println("[OTA] SD Card di-unmount sementara");
    }

    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
      // Remount SD jika OTA gagal start
      if (sdOk) SD.begin(SD_CS_PIN);
      return;
    }
    Serial.println("[OTA] Update dimulai...");
  }
  else if (upload.status == UPLOAD_FILE_WRITE) {
    otaUploaded += upload.currentSize;
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
      return;
    }
    // Print progress tiap 50KB biar tidak spam serial
    if (otaUploaded % 51200 < upload.currentSize) {
      Serial.printf("[OTA] %d KB diterima...\n", otaUploaded / 1024);
    }
  }
  else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      Serial.printf("[OTA] ✅ Selesai! %d bytes. Restart...\n", upload.totalSize);
      server.send(200, "text/plain", "OK! Restart dalam 3 detik...");
      delay(3000);
      ESP.restart();
    } else {
      Update.printError(Serial);
      // Remount SD jika OTA gagal
      if (sdOk) {
        SD.begin(SD_CS_PIN);
        Serial.println("[OTA] SD Card di-remount");
      }
      server.send(500, "text/plain", "OTA gagal! Coba lagi.");
    }
  }
  else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    // Remount SD jika OTA dibatalkan
    if (sdOk) {
      SD.begin(SD_CS_PIN);
      Serial.println("[OTA] Dibatalkan — SD Card di-remount");
    }
    Serial.println("[OTA] Upload dibatalkan!");
  }
}

// ============================================================
//   ROUTE: HALAMAN PENGATURAN (Hub)
// ============================================================
void handleSetting() {
  String body = "<div class='card'><h2>⚙️ Pengaturan Sistem</h2>";
  body += "<div style='display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:12px'>";

  // WiFi Card
  body += "<div style='border:1px solid #e0e0e0;border-radius:10px;padding:16px'>";
  body += "<div style='font-size:28px;margin-bottom:8px'>📶</div>";
  body += "<h3 style='margin-bottom:6px'>Konfigurasi WiFi</h3>";
  body += "<p style='color:#666;font-size:13px;margin-bottom:12px'>Ganti SSID dan password WiFi tanpa perlu upload ulang kode</p>";
  body += "<a href='/wifi-config' class='btn bb'>Buka Pengaturan</a>";
  body += "</div>";

  // OTA Card
  body += "<div style='border:1px solid #e0e0e0;border-radius:10px;padding:16px'>";
  body += "<div style='font-size:28px;margin-bottom:8px'>🔄</div>";
  body += "<h3 style='margin-bottom:6px'>OTA Firmware Update</h3>";
  body += "<p style='color:#666;font-size:13px;margin-bottom:12px'>Upload firmware baru via browser tanpa kabel USB</p>";
  body += "<a href='/ota' class='btn bg'>Buka OTA Update</a>";
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
  //Serial.begin(115200);
  Serial.begin(115200, SERIAL_8N1, -1, 1);
  Serial.println("\n===================================");
  Serial.println("  SISTEM ABSENSI PURE ESP32-CAM");
  Serial.println("===================================");

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
  server.on("/",             handleHome);
  server.on("/enroll",       handleEnroll);
  server.on("/enroll-start", handleEnrollStart);
  server.on("/enroll-poll",  handleEnrollPoll);
  server.on("/siswa",        handleSiswa);
  server.on("/hapus",        handleHapus);
  server.on("/log",          handleLog);
  server.on("/log-view",     handleLogView);
  server.on("/foto",         handleFoto);
  server.on("/api",          handleApi);
  server.on("/setting",      handleSetting);
  server.on("/wifi-config",  handleWifiConfig);
  server.on("/wifi-save",    handleWifiSave);
  server.on("/wifi-scan",    handleWifiScan);
  server.on("/ota",          handleOtaPage);
  server.on("/ota-upload",   HTTP_POST,
    []() { server.send(200, "text/plain", "OK"); },
    handleOtaUpload
  );
  server.on("/stream",       handleStream);
  server.on("/cam",          handleCamPage);
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
  Serial.println("  /setting   -> WiFi & OTA Update");
  Serial.println("=========================================");
}

// ============================================================
//   LOOP
// ============================================================
void loop() {
  server.handleClient();

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
