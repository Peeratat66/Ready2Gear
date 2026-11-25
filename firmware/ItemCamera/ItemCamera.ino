#include <Arduino.h>
#include <WiFi.h>
#include <esp_arduino_version.h>
#include <WebServer.h>
#include "LivePreview.h"
#include "esp_camera.h"
#define CAMERA_MODEL_AI_THINKER
#include "camera_pins.h"
const char *ssid = "YOUR_WIFI_SSID";        // ใส่ชื่อ WiFi
const char *password = "YOUR_WIFI_PASSWORD"; // ใส่รหัส WiFi

WebServer server(80);
const int flashPin=4;
const int brightness=40; // 0..255
bool lit=false;
unsigned long lastLease=0;
void lamp(bool on){
  if(on!=lit){
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWrite(flashPin,on ? brightness : 0);
#else
    ledcWrite(7,on ? brightness : 0);
#endif
  }
  lit=on;
  lastLease=millis();
}
bool initCamera() {
  camera_config_t config{};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.frame_size = FRAMESIZE_QVGA;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_LATEST;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 10;
  config.fb_count = 2;

  if (!psramFound()) {
    Serial.println("ไม่พบ PSRAM (ตั้ง Tools > PSRAM > OPI PSRAM)");
    config.frame_size = FRAMESIZE_QVGA;
    config.fb_location = CAMERA_FB_IN_DRAM;
    config.fb_count = 1;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x\n", err);
    return false;
  }

  sensor_t *s = esp_camera_sensor_get();
  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);
    s->set_brightness(s, 1);
    s->set_saturation(s, -2);
  }
  s->set_framesize(s, FRAMESIZE_QVGA);

#if defined(CAMERA_MODEL_ESP32S3_EYE)
  s->set_vflip(s, 1);
#endif

  return true;
}

void cameraProducer(void *){
  for(;;){
    camera_fb_t *frame=esp_camera_fb_get();
    if(frame){
      publishLiveFrame(frame->buf,frame->len);
      esp_camera_fb_return(frame);
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}
void setup(){
  Serial.begin(115200);
  Serial.println("Ready2Gear LivePreview NETFIX-1");
  pinMode(flashPin,OUTPUT); digitalWrite(flashPin,LOW);
  if(!initCamera()) return;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  if(!ledcAttachChannel(flashPin,5000,8,7)){Serial.println("Lamp PWM init failed");return;}
#else
  ledcSetup(7,5000,8); ledcAttachPin(flashPin,7);
#endif
  sensor_t *sensor=esp_camera_sensor_get();
  Serial.printf("Sensor PID: 0x%04X\n",sensor->id.PID);
  sensor->set_exposure_ctrl(sensor,1);
  sensor->set_gain_ctrl(sensor,1);
  sensor->set_ae_level(sensor,1);
  sensor->set_brightness(sensor,1);
  if(!initLivePreview()){
    Serial.println("Live preview init failed");
    return;
  }
  if(xTaskCreate(cameraProducer,"camera-preview",4096,nullptr,1,nullptr)!=pdPASS){
    Serial.println("Camera task init failed");
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid,password); WiFi.setSleep(false);
  server.on("/lamp",HTTP_POST,[](){
    String value=server.arg("on");
    if(value!="1" && value!="0"){server.send(400,"text/plain","Invalid on");return;}
    lamp(value=="1"); server.send(200,"text/plain",lit?"ON":"OFF");
  });
  server.on("/capture",[](){
    size_t length=0;
    uint32_t sequence=0;
    uint8_t *jpeg=copyLiveFrame(length,sequence);
    if(!jpeg){server.send(503,"text/plain","Camera not ready");return;}
    server.sendHeader("Cache-Control","no-store");
    server.send_P(200,"image/jpeg",(const char*)jpeg,length);
    free(jpeg);
  });
  server.on("/",[](){server.send(200,"text/html",LIVE_VIEW_PAGE);});
  server.begin();
}
void loop(){
  serviceLiveServer();
  static bool online=false;
  bool connected=WiFi.status()==WL_CONNECTED;
  if(connected && !online){
    Serial.println("Item CAM: http://"+WiFi.localIP().toString());
    Serial.println("Waiting for MJPEG server readiness message");
  }
  online=connected;
  if(lit && (!connected || millis()-lastLease>20000))lamp(false);
  server.handleClient();
}
