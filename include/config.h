#pragma once

// ============================================================
//   KONFIGURASI — SESUAIKAN SEBELUM UPLOAD
// ============================================================

// --- WiFi ---
#define WIFI_SSID        "Devanda"
#define WIFI_PASSWORD    "13061906"
#define BLYNK_TEMPLATE_ID "TMPL67t1bjQQr"
#define BLYNK_TEMPLATE_NAME "Presensi SLB"
// --- Blynk (notifikasi saja) ---
#define BLYNK_AUTH_TOKEN      "RzElXtyz9bjqAcBDtpV5Ah9ABPDUnFJf" //RzElXtyz9bjqAcBDtpV5Ah9ABPDUnFJf
#define BLYNK_VPIN_NAMA     V0
#define BLYNK_VPIN_ID       V1
#define BLYNK_VPIN_STATUS   V2
#define BLYNK_VPIN_JAM      V3
#define BLYNK_VPIN_TOTAL    V4

// --- Pin AS608 Fingerprint ---
// UART1 ESP32-CAM — pin aman yang tidak konflik kamera
#define FP_RX_PIN   3    // GPIO14 → TX AS608
#define FP_TX_PIN   12   // GPIO15 → RX AS608
#define FP_BAUD     57600

// --- SD Card (ESP32-CAM AI Thinker built-in) ---
// SD pakai HSPI: CLK=14, MISO=2, MOSI=15, CS=13
// CATATAN: Saat pakai SD, GPIO 14 & 15 dipakai SD!
// Fingerprint TIDAK bisa bersamaan dengan SD di pin default.
// Solusi: Fingerprint pakai SoftwareSerial atau UART2
#define SD_CS_PIN   13

// --- Pin Alternatif Fingerprint saat pakai SD ---
// Pakai UART2 (GPIO 16/17 tidak tersedia di ESP32-CAM)
// Gunakan SoftwareSerial di GPIO 12 & 13... 
// TAPI GPIO 12 & 13 juga konflik SD.
// === SOLUSI TERBAIK: Fingerprint via UART0 saat tidak debug ===
// atau gunakan GPIO 2 (hati-hati, juga boot pin)
// 
// REKOMENDASI WIRING AMAN:
// Fingerprint TX → GPIO 3 (U0RXD, tapi disable serial monitor)
// Fingerprint RX → GPIO 1 (U0TXD, disable serial monitor)
// ATAU: Tidak pakai serial monitor saat production
//
// Untuk development (dengan serial monitor), gunakan:
// Fingerprint TX → GPIO 14 (nonaktifkan SD sementara)
//
// === KONFIGURASI MODE ===
#define USE_SDCARD      true   // true = pakai SD, false = SPIFFS only
#define USE_SERIAL_FP   false  // false = FP pakai UART0 (no serial monitor)
                               // true  = FP pakai UART1 GPIO14/15 (no SD)

// --- LED ---
#define LED_FLASH_PIN   4    // Flash LED ESP32-CAM
#define LED_ONBOARD     33   // LED merah onboard (active LOW)

// --- Kamera ---
#define CAM_QUALITY     12   // 0-63, makin kecil makin bagus (lebih besar file)
#define CAM_FRAMESIZE   FRAMESIZE_QVGA  // 320x240

// --- Folder SD Card ---
#define SD_FOTO_DIR     "/foto"
#define SD_LOG_DIR      "/log"

// --- SPIFFS ---
#define STUDENTS_FILE   "/students.json"

// --- Timing ---
#define WIFI_TIMEOUT    15000
#define SCAN_COOLDOWN   3000   // ms antara scan yang sama
#define PHOTO_DELAY     300    // ms sebelum capture

// --- Web Server ---
#define HTTP_PORT       80

#define FW_VERSION "1.5.3"

#define GITHUB_OWNER "technometric"
#define GITHUB_REPO  "ufim-absensi"
#define OTA_BIN_NAME "firmware.bin"