#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_Fingerprint.h>
#include "esp_camera.h"
#include "img_converters.h"
#include <WiFi.h>
#include "esp_http_server.h"

// ===== WIFI =====
const char* ssid     = "Toko Melati";
const char* password = "13061906";

// ===== FINGERPRINT =====
#define FINGER_RX 13
#define FINGER_TX 12

HardwareSerial fingerSerial(2);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&fingerSerial);

// ===== CAMERA PIN AI THINKER =====
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27

#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

httpd_handle_t camera_httpd = NULL;

// ===== ROOT PAGE =====
static esp_err_t index_handler(httpd_req_t *req) {
  const char* html =
    "<html>"
    "<head><title>ESP32-CAM</title></head>"
    "<body>"
    "<h2>ESP32-CAM Stream</h2>"
    "<img src='/stream' width='640'>"
    "</body>"
    "</html>";

  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, html, strlen(html));
}

// ===== STREAM RGB565 -> JPEG =====
static esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t *fb = NULL;
  esp_err_t res = ESP_OK;

  uint8_t *jpg_buf = NULL;
  size_t jpg_len = 0;

  char part_buf[96];

  res = httpd_resp_set_type(
    req,
    "multipart/x-mixed-replace; boundary=frame"
  );

  if (res != ESP_OK) return res;

  while (true) {
    fb = esp_camera_fb_get();

    if (!fb) {
      Serial.println("Camera capture failed");
      res = ESP_FAIL;
      break;
    }

    jpg_buf = NULL;
    jpg_len = 0;

    bool converted = frame2jpg(fb, 80, &jpg_buf, &jpg_len);

    esp_camera_fb_return(fb);
    fb = NULL;

    if (!converted || jpg_buf == NULL || jpg_len == 0) {
      Serial.println("Convert RGB565 to JPEG failed");
      if (jpg_buf) free(jpg_buf);
      res = ESP_FAIL;
      break;
    }

    size_t hlen = snprintf(
      part_buf,
      sizeof(part_buf),
      "--frame\r\n"
      "Content-Type: image/jpeg\r\n"
      "Content-Length: %u\r\n\r\n",
      (unsigned int)jpg_len
    );

    res = httpd_resp_send_chunk(req, part_buf, hlen);

    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(
        req,
        (const char *)jpg_buf,
        jpg_len
      );
    }

    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, "\r\n", 2);
    }

    free(jpg_buf);
    jpg_buf = NULL;

    if (res != ESP_OK) break;

    delay(50);
  }

  return res;
}

// ===== START CAMERA SERVER =====
void startCameraServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;

  httpd_uri_t index_uri = {
    .uri      = "/",
    .method   = HTTP_GET,
    .handler  = index_handler,
    .user_ctx = NULL
  };

  httpd_uri_t stream_uri = {
    .uri      = "/stream",
    .method   = HTTP_GET,
    .handler  = stream_handler,
    .user_ctx = NULL
  };

  esp_err_t err = httpd_start(&camera_httpd, &config);

  if (err == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &index_uri);
    httpd_register_uri_handler(camera_httpd, &stream_uri);
    Serial.println("Camera server started");
  } else {
    Serial.printf("Camera server FAILED: 0x%x\n", err);
  }
}

// ===== INIT CAMERA =====
bool initCamera() {
  camera_config_t config;
  memset(&config, 0, sizeof(config));

  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;

  config.pin_d0    = Y2_GPIO_NUM;
  config.pin_d1    = Y3_GPIO_NUM;
  config.pin_d2    = Y4_GPIO_NUM;
  config.pin_d3    = Y5_GPIO_NUM;
  config.pin_d4    = Y6_GPIO_NUM;
  config.pin_d5    = Y7_GPIO_NUM;
  config.pin_d6    = Y8_GPIO_NUM;
  config.pin_d7    = Y9_GPIO_NUM;

  config.pin_xclk  = XCLK_GPIO_NUM;
  config.pin_pclk  = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href  = HREF_GPIO_NUM;

  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;

  config.pin_pwdn  = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;

  config.xclk_freq_hz = 10000000;
  config.pixel_format = PIXFORMAT_RGB565;

  config.frame_size   = FRAMESIZE_QQVGA;   // 160x120
  config.fb_count     = 1;
  config.fb_location  = CAMERA_FB_IN_DRAM;
  config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;

  esp_err_t err = esp_camera_init(&config);

  if (err != ESP_OK) {
    Serial.printf("Camera init FAILED: 0x%x\n", err);
    return false;
  }

  sensor_t *s = esp_camera_sensor_get();

  if (!s) {
    Serial.println("Sensor get FAILED");
    return false;
  }

  Serial.printf("Sensor PID: 0x%x\n", s->id.PID);

  s->set_framesize(s, FRAMESIZE_QQVGA);

  delay(500);

  Serial.println("Testing camera frame...");

  camera_fb_t *fb = esp_camera_fb_get();

  if (!fb) {
    Serial.println("Frame test FAILED");
    return false;
  }

  Serial.printf("Frame OK, size: %d bytes\n", fb->len);
  Serial.printf("Frame format: %d\n", fb->format);

  uint8_t *jpg_buf = NULL;
  size_t jpg_len = 0;

  bool converted = frame2jpg(fb, 80, &jpg_buf, &jpg_len);

  esp_camera_fb_return(fb);

  if (!converted || jpg_buf == NULL || jpg_len == 0) {
    Serial.println("JPEG convert test FAILED");
    if (jpg_buf) free(jpg_buf);
    return false;
  }

  Serial.printf("JPEG convert OK, size: %d bytes\n", jpg_len);
  free(jpg_buf);

  Serial.println("Camera siap.");
  return true;
}

// ===== INIT FINGERPRINT =====
bool initFingerprint() {
  fingerSerial.begin(57600, SERIAL_8N1, FINGER_RX, FINGER_TX);
  finger.begin(57600);

  if (finger.verifyPassword()) {
    Serial.println("Sensor fingerprint terdeteksi.");
    return true;
  }

  Serial.println("Sensor fingerprint TIDAK terdeteksi.");
  return false;
}

// ===== INIT WIFI =====
void initWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  Serial.print("Connecting WiFi");

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("WiFi connected");

  Serial.print("Camera page: http://");
  Serial.println(WiFi.localIP());

  Serial.print("Camera stream: http://");
  Serial.print(WiFi.localIP());
  Serial.println("/stream");
}

// ===== SCAN I2C CAMERA =====
void scanI2C() {
  Wire.begin(SIOD_GPIO_NUM, SIOC_GPIO_NUM);
  Wire.setClock(100000);

  Serial.println("Scanning I2C...");

  for (byte addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    byte err = Wire.endTransmission();

    if (err == 0) {
      Serial.printf("Device found at 0x%02X\n", addr);
    }
  }

  Serial.println("Scan done.");
}

// ===== BACA FINGERPRINT =====
void checkFingerprint() {
  uint8_t p = finger.getImage();

  if (p == FINGERPRINT_OK) {
    Serial.println("Gambar jari terbaca.");
  } else if (p == FINGERPRINT_NOFINGER) {
    Serial.println("Belum ada jari.");
  } else {
    Serial.print("Error baca fingerprint: ");
    Serial.println(p);
  }
}

// ===== SETUP =====
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("Absensi Fingerprint + ESP32-CAM");

  initFingerprint();

  scanI2C();

  if (initCamera()) {
    initWiFi();
    startCameraServer();
  } else {
    Serial.println("Camera gagal start, WiFi server tidak dijalankan.");
  }
}

// ===== LOOP =====
void loop() {
  checkFingerprint();
  delay(1000);
}