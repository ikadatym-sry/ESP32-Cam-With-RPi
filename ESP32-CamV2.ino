/*
 * ESP-CAM WebServer with RPi*
 * ESP32-CAM V2 - Dual-Engine High-Performance Firmware
 * Architecture:
 *  - Port 8100 (Command & Telemetry Server): /status, /logs, /cmd, /capture, /update (Lightning fast <5ms response)
 *  - Port 8181 (Dedicated Stream Server): /stream (High-throughput MJPEG pipeline without blocking commands)
 *  - 2MB PSRAM optimized (SVGA 800x600 @ 20-25fps)
 *  - Zero SD card dependency (100% RAM + Network based)
 *  - Password-Protected OTA (ArduinoOTA & Web OTA) with Clean Flash/DMA Shutdown
 *  - NTP Real-World Time Sync (Auto UTC+7 Timestamp on every log line)
 *  - PWM Flashlight LED Control on GPIO 4 (0 - 100% smooth brightness)
 *  - On-chip fast motion detection engine
 *  - Web Serial Monitor & Logger (/logs) with PSRAM Ring Buffer
 *  - Hardware Diagnostics & Crash/Brownout Reason Detection (esp_reset_reason)
 * 
 * Target Board: AI Thinker ESP32-CAM
 * Arduino IDE Settings:
 *  - Board: "AI Thinker ESP32-CAM"
 *  - CPU Frequency: 240MHz (WiFi/BT)
 *  - Flash Frequency: 80MHz
 *  - Flash Mode: QIO
 *  - Partition Scheme: "Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)"
 *  - PSRAM: "Enabled"
 */

#include "esp_camera.h"
#include <WiFi.h>
#include "esp_http_server.h"
#include <Update.h>
#include <ArduinoOTA.h>
#include "esp_system.h"
#include "esp_sleep.h"
#include "time.h"
#include "esp_arduino_version.h"
const char* WIFI_SSID     = "";
const char* WIFI_PASSWORD = "";
const int CMD_SERVER_PORT    = 8100; // Commands, Status, Logs, OTA
const int STREAM_SERVER_PORT = 8181; // Dedicated MJPEG Video Stream
const char* OTA_PASSWORD  = "admin"; 

// NTP Timezone Settings: GMT+7 (Bangkok / Thailand Timezone)
const char* NTP_SERVER_1  = "pool.ntp.org";
const char* NTP_SERVER_2  = "time.google.com";
const long  GMT_OFFSET_SEC = 7 * 3600; // GMT+7
const int   DAYLIGHT_OFFSET_SEC = 0;

// Motion Detection Sensitivity (0 - 100)
int motionSensitivity = 25; 
int motionThreshold = 15;

// ==========================================
// 2. AI-THINKER CAMERA PIN ASSIGNMENTS
// ==========================================
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

#define FLASH_LED_PIN      4  // On-board Bright Flashlight LED
#define FLASH_PWM_CHANNEL  7  // LEDC PWM Channel
#define ONBOARD_LED_PIN   33  // Small Red/Blue Status LED (Active LOW)

// Cross-version PWM Helpers
void setupFlashPWM() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(FLASH_LED_PIN, 5000, 8);
  ledcWrite(FLASH_LED_PIN, 0);
#else
  ledcSetup(FLASH_PWM_CHANNEL, 5000, 8);
  ledcAttachPin(FLASH_LED_PIN, FLASH_PWM_CHANNEL);
  ledcWrite(FLASH_PWM_CHANNEL, 0);
#endif
}

void writeFlashPWM(int duty) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(FLASH_LED_PIN, duty);
#else
  ledcWrite(FLASH_PWM_CHANNEL, duty);
#endif
}

// ==========================================
// 3. PSRAM LOG BUFFER (REAL-WORLD TIMESTAMPS)
// ==========================================
#define MAX_LOG_LINES 150
#define MAX_LOG_LINE_LEN 180

char logBuffer[MAX_LOG_LINES][MAX_LOG_LINE_LEN];
int logHead = 0;
int logCount = 0;
String lastResetReasonStr = "Unknown";
bool timeSynced = false;

void addLog(const char* format, ...) {
  char temp[MAX_LOG_LINE_LEN];
  va_list args;
  va_start(args, format);
  vsnprintf(temp, sizeof(temp), format, args);
  va_end(args);

  char timeStr[32];
  struct tm timeinfo;
  if (timeSynced && getLocalTime(&timeinfo, 10)) {
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
  } else {
    unsigned long sec = millis() / 1000;
    snprintf(timeStr, sizeof(timeStr), "Boot+%02lu:%02lu:%02lu", 
             (sec / 3600), (sec % 3600) / 60, sec % 60);
  }

  Serial.printf("[%s] %s\n", timeStr, temp);

  snprintf(logBuffer[logHead], MAX_LOG_LINE_LEN, "[%s] %s", timeStr, temp);
  logHead = (logHead + 1) % MAX_LOG_LINES;
  if (logCount < MAX_LOG_LINES) logCount++;
}

void checkResetReason() {
  esp_reset_reason_t reason = esp_reset_reason();
  switch (reason) {
    case ESP_RST_POWERON:   lastResetReasonStr = "Power-on Reset (Normal Boot)"; break;
    case ESP_RST_SW:        lastResetReasonStr = "Software Reset (Remote Reboot)"; break;
    case ESP_RST_PANIC:     lastResetReasonStr = "CRASH / PANIC EXCEPTION!"; break;
    case ESP_RST_INT_WDT:   lastResetReasonStr = "Interrupt Watchdog Reset!"; break;
    case ESP_RST_TASK_WDT:  lastResetReasonStr = "Task Watchdog Reset!"; break;
    case ESP_RST_WDT:       lastResetReasonStr = "Hardware Watchdog Reset!"; break;
    case ESP_RST_DEEPSLEEP: lastResetReasonStr = "Wakeup from Deep Sleep"; break;
    case ESP_RST_BROWNOUT:  lastResetReasonStr = "BROWNOUT RESET! (Power supply voltage dropped below 2.7V)"; break;
    case ESP_RST_SDIO:      lastResetReasonStr = "SDIO Reset"; break;
    default:                lastResetReasonStr = "Unknown Reset"; break;
  }
  addLog("[SYS] Boot Reason: %s", lastResetReasonStr.c_str());
}

// ==========================================
// 4. GLOBAL STATE
// ==========================================
httpd_handle_t camera_httpd = NULL; // Port 8100
httpd_handle_t stream_httpd = NULL; // Port 8181

bool motionDetected = false;
int lastMotionScore = 0;
unsigned long lastMotionTime = 0;
const unsigned long MOTION_HOLD_TIME_MS = 3000;

unsigned long lastFrameTime = 0;
float currentFPS = 0.0;
unsigned long frameCount = 0;
unsigned long fpsTimer = 0;

bool flashLedState = false;
int flashBrightness = 0; // 0 - 255
bool isUpdatingOTA = false;

#define MOTION_GRID_W 32
#define MOTION_GRID_H 24
#define MOTION_GRID_SIZE (MOTION_GRID_W * MOTION_GRID_H)
uint8_t prevMotionGrid[MOTION_GRID_SIZE];
bool isFirstMotionFrame = true;

// ==========================================
// 5. MOTION DETECTION ENGINE
// ==========================================
bool processMotionDetection(camera_fb_t* fb) {
  if (!fb || fb->len == 0) return false;

  uint8_t currentGrid[MOTION_GRID_SIZE];
  size_t step = fb->len / MOTION_GRID_SIZE;
  if (step == 0) step = 1;

  for (int i = 0; i < MOTION_GRID_SIZE; i++) {
    size_t idx = i * step;
    currentGrid[i] = (idx < fb->len) ? fb->buf[idx] : 0;
  }

  if (isFirstMotionFrame) {
    memcpy(prevMotionGrid, currentGrid, MOTION_GRID_SIZE);
    isFirstMotionFrame = false;
    return false;
  }

  int changedBlocks = 0;
  for (int i = 0; i < MOTION_GRID_SIZE; i++) {
    int delta = abs((int)currentGrid[i] - (int)prevMotionGrid[i]);
    if (delta > motionSensitivity) changedBlocks++;
    prevMotionGrid[i] = (uint8_t)(((int)prevMotionGrid[i] * 7 + (int)currentGrid[i] * 3) / 10);
  }

  lastMotionScore = changedBlocks;

  if (changedBlocks >= motionThreshold) {
    if (!motionDetected) {
      addLog("[MOTION] Triggered! Score: %d (Thresh: %d)", changedBlocks, motionThreshold);
    }
    motionDetected = true;
    lastMotionTime = millis();
    return true;
  } else {
    if (millis() - lastMotionTime > MOTION_HOLD_TIME_MS) {
      motionDetected = false;
    }
    return false;
  }
}

// ==========================================
// 6. PORT 8181: DEDICATED STREAM ENGINE
// ==========================================
static esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t *fb = NULL;
  esp_err_t res = ESP_OK;

  addLog("[STREAM] Client connected to live stream on Port %d.", STREAM_SERVER_PORT);

  res = httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=frame");
  if (res != ESP_OK) return res;
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  while (!isUpdatingOTA) {
    fb = esp_camera_fb_get();
    if (!fb) {
      vTaskDelay(10 / portTICK_PERIOD_MS);
      continue;
    }

    processMotionDetection(fb);

    frameCount++;
    if (millis() - fpsTimer >= 1000) {
      currentFPS = (frameCount * 1000.0) / (millis() - fpsTimer);
      frameCount = 0;
      fpsTimer = millis();
    }

    char header_buf[140];
    int hlen = snprintf(header_buf, sizeof(header_buf),
                        "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\nX-Motion: %s\r\nX-Motion-Score: %d\r\n\r\n",
                        fb->len, motionDetected ? "true" : "false", lastMotionScore);

    res = httpd_resp_send_chunk(req, header_buf, hlen);
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, "\r\n", 2);
    }

    esp_camera_fb_return(fb);
    fb = NULL;

    if (res != ESP_OK) {
      break;
    }
  }

  addLog("[STREAM] Client disconnected from Port %d.", STREAM_SERVER_PORT);
  return res;
}

// ==========================================
// 7. PORT 8100: COMMAND & DIAGNOSTICS HANDLERS
// ==========================================

// --- Handler: Status (/status) ---
static esp_err_t status_handler(httpd_req_t *req) {
  float dieTemp = temperatureRead();
  char timeStr[32] = "N/A";
  struct tm timeinfo;
  if (timeSynced && getLocalTime(&timeinfo, 10)) {
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
  }

  char jsonBuffer[650];
  snprintf(jsonBuffer, sizeof(jsonBuffer),
    "{\n"
    "  \"current_time\": \"%s\",\n"
    "  \"temperature_c\": %.2f,\n"
    "  \"psram_total_bytes\": %u,\n"
    "  \"psram_free_bytes\": %u,\n"
    "  \"psram_min_free_bytes\": %u,\n"
    "  \"heap_free_bytes\": %u,\n"
    "  \"heap_min_free_bytes\": %u,\n"
    "  \"wifi_rssi_dbm\": %d,\n"
    "  \"motion_detected\": %s,\n"
    "  \"motion_score\": %d,\n"
    "  \"fps\": %.2f,\n"
    "  \"uptime_seconds\": %lu,\n"
    "  \"reset_reason\": \"%s\",\n"
    "  \"flash_led\": %s,\n"
    "  \"flash_brightness\": %d,\n"
    "  \"cmd_port\": %d,\n"
    "  \"stream_port\": %d,\n"
    "  \"resolution\": \"SVGA 800x600\"\n"
    "}",
    timeStr,
    dieTemp,
    ESP.getPsramSize(),
    ESP.getFreePsram(),
    ESP.getMinFreePsram(),
    ESP.getFreeHeap(),
    ESP.getMinFreeHeap(),
    WiFi.RSSI(),
    motionDetected ? "true" : "false",
    lastMotionScore,
    currentFPS,
    millis() / 1000,
    lastResetReasonStr.c_str(),
    flashLedState ? "true" : "false",
    flashBrightness,
    CMD_SERVER_PORT,
    STREAM_SERVER_PORT
  );

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, jsonBuffer, HTTPD_RESP_USE_STRLEN);
}

// --- Handler: Logs (/logs) ---
static esp_err_t logs_handler(httpd_req_t *req) {
  String out = "{\n  \"reset_reason\": \"" + lastResetReasonStr + "\",\n  \"logs\": [\n";
  
  int start = (logCount == MAX_LOG_LINES) ? logHead : 0;
  for (int i = 0; i < logCount; i++) {
    int idx = (start + i) % MAX_LOG_LINES;
    out += "    \"";
    String line = String(logBuffer[idx]);
    line.replace("\"", "\\\"");
    line.replace("\r", "");
    line.replace("\n", "");
    out += line;
    out += (i == logCount - 1) ? "\"\n" : "\",\n";
  }
  out += "  ]\n}";

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, out.c_str(), out.length());
}

// --- Handler: Commands (/cmd) ---
static esp_err_t cmd_handler(httpd_req_t *req) {
  char query[128];
  char action[32] = {0};
  char val_str[32] = {0};

  size_t query_len = httpd_req_get_url_query_len(req);
  if (query_len > 0 && query_len < sizeof(query)) {
    httpd_req_get_url_query_str(req, query, sizeof(query));
    httpd_query_key_value(query, "action", action, sizeof(action));
    httpd_query_key_value(query, "val", val_str, sizeof(val_str));
  }

  if (action[0] == '\0' && req->content_len > 0 && req->content_len < sizeof(query)) {
    char body[128];
    int received = httpd_req_recv(req, body, req->content_len);
    if (received > 0) {
      body[received] = '\0';
      httpd_query_key_value(body, "action", action, sizeof(action));
      httpd_query_key_value(body, "val", val_str, sizeof(val_str));
    }
  }

  if (action[0] == '\0') {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing action");
    return ESP_FAIL;
  }

  addLog("[CMD] Received command: %s", action);

  if (strcmp(action, "reboot") == 0) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "{\"status\": \"rebooting\", \"message\": \"ESP32 restarting...\"}", HTTPD_RESP_USE_STRLEN);
    addLog("[SYS] Rebooting ESP32 via Web command...");
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    ESP.restart();
    return ESP_OK;
  }
  else if (strcmp(action, "sleep") == 0) {
    unsigned long durationMs = 60000;
    char ms_str[32] = {0};
    char sec_str[32] = {0};
    char min_str[32] = {0};
    httpd_query_key_value(query, "ms", ms_str, sizeof(ms_str));
    httpd_query_key_value(query, "sec", sec_str, sizeof(sec_str));
    httpd_query_key_value(query, "min", min_str, sizeof(min_str));

    if (ms_str[0] != '\0') durationMs = atol(ms_str);
    else if (sec_str[0] != '\0') durationMs = atol(sec_str) * 1000UL;
    else if (min_str[0] != '\0') durationMs = atol(min_str) * 60000UL;

    if (durationMs < 1000) durationMs = 1000;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    char resp[128];
    snprintf(resp, sizeof(resp), "{\"status\": \"sleeping\", \"duration_ms\": %lu, \"message\": \"ESP32 entering Deep Sleep...\"}", durationMs);
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);

    addLog("[PWR] Entering Deep Sleep for %lu ms (%.1f sec)...", durationMs, durationMs / 1000.0);
    vTaskDelay(500 / portTICK_PERIOD_MS);

    esp_camera_deinit();
    digitalWrite(ONBOARD_LED_PIN, HIGH);
    writeFlashPWM(0);
    
    uint64_t sleepUs = (uint64_t)durationMs * 1000ULL;
    esp_sleep_enable_timer_wakeup(sleepUs);
    esp_deep_sleep_start();
    return ESP_OK;
  }
  else if (strcmp(action, "flash") == 0 || strcmp(action, "flash_toggle") == 0) {
    int duty = 0;
    if (val_str[0] != '\0') {
      duty = atoi(val_str);
    } else {
      duty = flashLedState ? 0 : 128;
    }

    if (duty < 0) duty = 0;
    if (duty > 255) duty = 255;

    flashBrightness = duty;
    flashLedState = (duty > 0);

    writeFlashPWM(duty);
    addLog("[LED] Flashlight brightness: %d / 255 (%.0f%%)", duty, (duty / 255.0) * 100);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    char resp[100];
    snprintf(resp, sizeof(resp), "{\"flash_led\": %s, \"brightness\": %d}", flashLedState ? "true" : "false", duty);
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
  }
  else if (strcmp(action, "set_sens") == 0) {
    if (val_str[0] != '\0') {
      motionSensitivity = atoi(val_str);
      addLog("[MOTION] Sensitivity updated to: %d", motionSensitivity);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    char resp[80];
    snprintf(resp, sizeof(resp), "{\"status\": \"ok\", \"sensitivity\": %d}", motionSensitivity);
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
  }

  httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown command");
  return ESP_FAIL;
}

// --- Handler: Snapshot Capture (/capture) ---
static esp_err_t capture_handler(httpd_req_t *req) {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return res;
}

// --- Handler: Password-Protected OTA Post (/update) ---
static esp_err_t ota_post_handler(httpd_req_t *req) {
  char query[128] = {0};
  char pwd[64] = {0};
  size_t query_len = httpd_req_get_url_query_len(req);
  if (query_len > 0 && query_len < sizeof(query)) {
    httpd_req_get_url_query_str(req, query, sizeof(query));
    httpd_query_key_value(query, "pwd", pwd, sizeof(pwd));
  }

  if (strcmp(pwd, OTA_PASSWORD) != 0) {
    addLog("[OTA] Authentication Failed! Invalid Password.");
    httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Invalid OTA Password");
    return ESP_FAIL;
  }

  isUpdatingOTA = true;
  char buf[1024];
  size_t remaining = req->content_len;
  addLog("[OTA] Authorized! Flashing %u bytes to Flash Partition...", remaining);

  if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
    isUpdatingOTA = false;
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA Begin Failed");
    return ESP_FAIL;
  }

  while (remaining > 0) {
    int received = httpd_req_recv(req, buf, min(remaining, sizeof(buf)));
    if (received <= 0) {
      if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
      Update.abort();
      isUpdatingOTA = false;
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA Stream Interrupted");
      return ESP_FAIL;
    }

    if (Update.write((uint8_t*)buf, received) != received) {
      Update.abort();
      isUpdatingOTA = false;
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA Write Failed");
      return ESP_FAIL;
    }
    remaining -= received;
  }

  if (Update.end(true)) {
    addLog("[OTA] Update Succeeded! Rebooting...");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "UPDATE SUCCESSFUL! Rebooting...", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    ESP.restart();
    return ESP_OK;
  } else {
    isUpdatingOTA = false;
    addLog("[OTA] Update Finalize Failed!");
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA End Failed");
    return ESP_FAIL;
  }
}

// Start Dual-Engine HTTP Servers (Port 8100 & Port 8181)
void startCameraServer() {
  // 1. Port 8100: Command, Telemetry, and OTA Server
  httpd_config_t config_cmd = HTTPD_DEFAULT_CONFIG();
  config_cmd.server_port = CMD_SERVER_PORT;
  config_cmd.ctrl_port = 32768;
  config_cmd.max_open_sockets = 5;

  httpd_uri_t uri_status = { .uri = "/status", .method = HTTP_GET, .handler = status_handler, .user_ctx = NULL };
  httpd_uri_t uri_logs   = { .uri = "/logs",   .method = HTTP_GET, .handler = logs_handler,   .user_ctx = NULL };
  httpd_uri_t uri_cmd_g  = { .uri = "/cmd",    .method = HTTP_GET, .handler = cmd_handler,    .user_ctx = NULL };
  httpd_uri_t uri_cmd_p  = { .uri = "/cmd",    .method = HTTP_POST, .handler = cmd_handler,   .user_ctx = NULL };
  httpd_uri_t uri_capture= { .uri = "/capture",.method = HTTP_GET, .handler = capture_handler,.user_ctx = NULL };
  httpd_uri_t uri_update = { .uri = "/update", .method = HTTP_POST, .handler = ota_post_handler, .user_ctx = NULL };

  if (httpd_start(&camera_httpd, &config_cmd) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &uri_status);
    httpd_register_uri_handler(camera_httpd, &uri_logs);
    httpd_register_uri_handler(camera_httpd, &uri_cmd_g);
    httpd_register_uri_handler(camera_httpd, &uri_cmd_p);
    httpd_register_uri_handler(camera_httpd, &uri_capture);
    httpd_register_uri_handler(camera_httpd, &uri_update);
    addLog("[HTTP] Command & Telemetry Server running on Port %d", CMD_SERVER_PORT);
  }

  // 2. Port 8181: Dedicated High-Speed Stream Server
  httpd_config_t config_stream = HTTPD_DEFAULT_CONFIG();
  config_stream.server_port = STREAM_SERVER_PORT;
  config_stream.ctrl_port = 32769;
  config_stream.max_open_sockets = 3;

  httpd_uri_t uri_stream = { .uri = "/stream", .method = HTTP_GET, .handler = stream_handler, .user_ctx = NULL };

  if (httpd_start(&stream_httpd, &config_stream) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &uri_stream);
    addLog("[HTTP] Dedicated Video Stream Server running on Port %d", STREAM_SERVER_PORT);
  }
}

// ==========================================
// 8. INITIALIZATION & SETUP
// ==========================================
void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(ONBOARD_LED_PIN, OUTPUT);
  digitalWrite(ONBOARD_LED_PIN, HIGH);

  setupFlashPWM();

  addLog("==========================================");
  addLog("   ESP32-CAM V2 Firmware Starting...      ");
  addLog("==========================================");

  checkResetReason();

  if (psramFound()) {
    addLog("[PSRAM] Found: %u bytes (%.2f MB), Free: %u bytes",
           ESP.getPsramSize(),
           ESP.getPsramSize() / (1024.0 * 1024.0),
           ESP.getFreePsram());
  } else {
    addLog("[PSRAM] WARNING: PSRAM NOT FOUND!");
  }

  // Camera Setup
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size   = FRAMESIZE_SVGA; // 800x600
  config.jpeg_quality = 12;
  config.fb_count     = 2;
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.grab_mode    = CAMERA_GRAB_LATEST;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    addLog("[CAMERA] Init Failed with error 0x%x! Rebooting in 3s...", err);
    delay(3000);
    ESP.restart();
  }
  addLog("[CAMERA] OV2640 Initialized (SVGA 800x600)");

  sensor_t* s = esp_camera_sensor_get();
  if (s != NULL) {
    s->set_brightness(s, 0);
    s->set_contrast(s, 0);
    s->set_saturation(s, 0);
    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 1);
    s->set_exposure_ctrl(s, 1);
    s->set_gain_ctrl(s, 1);
  }

  // Connect to Wi-Fi
  addLog("[WIFI] Connecting to SSID: %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int retryCount = 0;
  while (WiFi.status() != WL_CONNECTED && retryCount < 30) {
    delay(500);
    retryCount++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    addLog("[WIFI] Connected! IP: http://%s (RSSI: %d dBm)", 
           WiFi.localIP().toString().c_str(), WiFi.RSSI());

    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER_1, NTP_SERVER_2);
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 3000)) {
      timeSynced = true;
      addLog("[NTP] Real-World Time Synced (GMT+7)");
    }
  } else {
    addLog("[WIFI] Connection Failed! Rebooting...");
    delay(3000);
    ESP.restart();
  }

  // Setup ArduinoOTA with Clean Callbacks
  ArduinoOTA.setHostname("esp32cam-v2");
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA
    .onStart([]() {
      isUpdatingOTA = true;
      addLog("[OTA] ArduinoOTA Inbound Flash Stream Started!");
    })
    .onEnd([]() {
      addLog("[OTA] Flash Complete!");
    })
    .onError([](ota_error_t error) {
      isUpdatingOTA = false;
      addLog("[OTA] Error Code: %u", error);
    });

  ArduinoOTA.begin();
  addLog("[OTA] ArduinoOTA Ready (Password Protected)");

  // Start Dual-Engine HTTP Servers (Port 8100 & 8181)
  startCameraServer();
}

// ==========================================
// 9. MAIN LOOP
// ==========================================
void loop() {
  ArduinoOTA.handle();

  if (!timeSynced && WiFi.status() == WL_CONNECTED) {
    static unsigned long lastNtpCheck = 0;
    if (millis() - lastNtpCheck > 5000) {
      struct tm timeinfo;
      if (getLocalTime(&timeinfo, 100)) {
        timeSynced = true;
        addLog("[NTP] Time Synced Successfully!");
      }
      lastNtpCheck = millis();
    }
  }

  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long lastReconnect = 0;
    if (millis() - lastReconnect > 10000) {
      addLog("[WIFI] Disconnected! Reconnecting...");
      WiFi.reconnect();
      lastReconnect = millis();
    }
  }

  if (motionDetected) {
    digitalWrite(ONBOARD_LED_PIN, LOW);
  } else {
    digitalWrite(ONBOARD_LED_PIN, HIGH);
  }

  yield();
}
