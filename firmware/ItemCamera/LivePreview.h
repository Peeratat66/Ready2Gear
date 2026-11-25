#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <freertos/semphr.h>

// Latest JPEG for preview. Captured inspection images use separate storage.
static SemaphoreHandle_t liveMutex = nullptr;
static uint8_t *liveBuffer = nullptr;
static size_t liveCapacity = 0;
static size_t liveLength = 0;
static uint32_t liveSequence = 0;
static httpd_handle_t liveServer = nullptr;
static volatile bool liveRequested = false;

static bool initLivePreview() {
  liveMutex = xSemaphoreCreateMutex();
  if (!liveMutex) return false;

  liveCapacity = psramFound() ? 128 * 1024 : 32 * 1024;
  liveBuffer = static_cast<uint8_t *>(heap_caps_malloc(
      liveCapacity, psramFound() ? MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
                               : MALLOC_CAP_8BIT));
  if (!liveBuffer) {
    vSemaphoreDelete(liveMutex);
    liveMutex = nullptr;
    return false;
  }
  return true;
}

static void publishLiveFrame(const uint8_t *jpeg, size_t length) {
  if (!liveMutex || !jpeg || !length || length > liveCapacity) return;
  if (xSemaphoreTake(liveMutex, pdMS_TO_TICKS(20)) != pdTRUE) return;
  memcpy(liveBuffer, jpeg, length);
  liveLength = length;
  ++liveSequence;
  xSemaphoreGive(liveMutex);
}

static uint8_t *copyLiveFrame(size_t &length, uint32_t &sequence) {
  length = 0;
  if (!liveMutex ||
      xSemaphoreTake(liveMutex, pdMS_TO_TICKS(30)) != pdTRUE) return nullptr;

  uint8_t *copy = nullptr;
  if (liveLength && sequence != liveSequence) {
    copy = static_cast<uint8_t *>(heap_caps_malloc(
        liveLength, psramFound() ? MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
                                : MALLOC_CAP_8BIT));
    if (copy) {
      memcpy(copy, liveBuffer, liveLength);
      length = liveLength;
      sequence = liveSequence;
    }
  }
  xSemaphoreGive(liveMutex);
  return copy;
}

static esp_err_t serveLiveStream(httpd_req_t *request) {
  if (!liveBuffer) {
    httpd_resp_set_status(request, "503 Service Unavailable");
    httpd_resp_sendstr(request, "Preview unavailable");
    return ESP_FAIL;
  }

  esp_err_t result = httpd_resp_set_type(
      request, "multipart/x-mixed-replace;boundary=frame");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  httpd_resp_set_hdr(request, "Access-Control-Allow-Origin", "*");
  liveRequested = true;
  uint32_t previousSequence = 0;
  unsigned long lastFrameAt = millis();

  while (result == ESP_OK && WiFi.status() == WL_CONNECTED) {
    size_t length = 0;
    uint8_t *jpeg = copyLiveFrame(length, previousSequence);
    if (!jpeg) {
      if (millis() - lastFrameAt > 10000) break;
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    lastFrameAt = millis();
    char header[100];
    int headerLength = snprintf(
        header, sizeof(header),
        "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
        static_cast<unsigned int>(length));
    result = httpd_resp_send_chunk(request, header, headerLength);
    if (result == ESP_OK) {
      result = httpd_resp_send_chunk(
          request, reinterpret_cast<const char *>(jpeg), length);
    }
    free(jpeg);
    if (result == ESP_OK) {
      result = httpd_resp_send_chunk(request, "\r\n", 2);
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  liveRequested = false;
  // Force a clean disconnect so the viewer can reconnect.
  return ESP_FAIL;
}

static bool startLiveServer() {
  // lwIP must be initialized by Wi-Fi before starting a socket server.
  if (liveServer) return true;
  if (WiFi.status() != WL_CONNECTED || !liveBuffer) return false;
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 81;
  config.ctrl_port = 32769;
  config.stack_size = 6144;
  config.send_wait_timeout = 2;
  config.lru_purge_enable = true;

  esp_err_t error = httpd_start(&liveServer, &config);
  if (error != ESP_OK) {
    liveServer = nullptr;
    Serial.printf("MJPEG server start failed: 0x%X\n", error);
    return false;
  }
  httpd_uri_t stream = {};
  stream.uri = "/stream";
  stream.method = HTTP_GET;
  stream.handler = serveLiveStream;
  if (httpd_register_uri_handler(liveServer, &stream) != ESP_OK) {
    httpd_stop(liveServer);
    liveServer = nullptr;
    return false;
  }
  return true;
}

// Call from loop, after Wi-Fi has connected. Failed starts retry slowly.
static void serviceLiveServer() {
  static bool attempted = false;
  static unsigned long attemptedAt = 0;
  if (liveServer || !liveBuffer || WiFi.status() != WL_CONNECTED) return;
  if (attempted && millis() - attemptedAt < 3000) return;
  attempted = true;
  attemptedAt = millis();
  if (startLiveServer()) {
    Serial.println("MJPEG ready: http://" + WiFi.localIP().toString() + ":81/stream");
  }
}

static const char LIVE_VIEW_PAGE[] =
    "<!doctype html><html><head><meta name='viewport' "
    "content='width=device-width,initial-scale=1'>"
    "<title>Camera live view</title></head>"
    "<body style='margin:0;background:#111;color:white;font-family:sans-serif'>"
    "<p>Live camera preview</p><img id='video' style='max-width:100%'>"
    "<script>const img=document.getElementById('video');"
    "function connect(){img.src='http://'+location.hostname+':81/stream?t='+Date.now()}"
    "img.onerror=()=>setTimeout(connect,1500);connect();"
    "</script></body></html>";
