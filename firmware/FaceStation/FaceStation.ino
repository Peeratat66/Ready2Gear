// =====================================================================
// หมายเหตุการแก้ไข (Ready2Gear, 22 พ.ย. 2026)
//   // >>>>> [R2G-ADD] ... // <<<<< [R2G-END]  = ส่วนที่เพิ่มเข้ามาใหม่
//   // >>>>> [R2G-OFF] ... // <<<<< [R2G-END]  = โค้ดเดิมที่ปิดไว้ (comment ไม่ได้ลบ)
//   ทุกจุดมีเหตุผลกำกับไว้ ถ้าจะคืนค่าเดิม: ลบส่วน ADD และเอา comment ออกจากส่วน OFF
//   อัปโหลดต้องตั้ง Tools > PSRAM > OPI PSRAM
// =====================================================================
#include <Arduino.h>
#include <Preferences.h>
#include <TJpg_Decoder.h>
#include "esp_camera.h"
#include "board_config.h"  // Copy the camera configuration from your WORKING S3 sketch.
#include <WiFi.h>
#include <WebServer.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <Adafruit_ILI9341.h>
#include <HTTPClient.h>
#include <esp_heap_caps.h>
#include "LivePreview.h"

const uint32_t TFT_SPI_HZ = 10000000; // try 5000000 if wiring is unstable

const bool SERIAL_TEST_MODE = false; // false = live CiRA + database

const char *ssid = "YOUR_WIFI_SSID";        // ใส่ชื่อ WiFi
const char *password = "YOUR_WIFI_PASSWORD"; // ใส่รหัส WiFi
const char *MQTT_HOST = "broker.emqx.io";
const int MQTT_PORT = 1883;

const char *TOPIC_REQ = "cpe477-peerach/req";
const char *TOPIC_RESULT = "cpe477-peerach/result";

const String itemCamBase = "http://172.20.10.3";
uint8_t *faceData=nullptr;
size_t faceSize=0;
Adafruit_ILI9341 tft(&SPI,42,41,14);
WebServer web(80);
WiFiClient net;
PubSubClient mqtt(net);
Preferences prefs;
bool faceLayout=true;
void statusLine(const String &msg) {
  int footer = 180;
  if (faceLayout) footer=216;
  tft.fillRect(0, footer, 320, 240-footer, ILI9341_BLACK);
  tft.setTextSize(2);
  tft.setCursor(0, footer+2);
  tft.print(msg);
  Serial.println(msg);
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
bool getItemImage() {
  HTTPClient http;
  http.setConnectTimeout(1500); http.setTimeout(2500);
  if (!http.begin(itemCamBase+"/capture")) return false;
  int status=http.GET(), len=http.getSize();
  if (status==200 && len>2 && len<=512*1024) {
    faceData=(uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if (!faceData) faceData=(uint8_t*)malloc(len);
    if (faceData) faceSize=http.getStream().readBytes(faceData,len);
  }
  http.end();
  if (faceData && faceSize==(size_t)len && faceData[0]==0xff && faceData[1]==0xd8) return true;
  free(faceData); faceData=nullptr; faceSize=0; return false;
}
enum Phase { FACE, WAIT_FACE, ITEM, WAIT_ITEM, COMPLETE, CONFIRM, UNDO, SAVING, DONE };
Phase phase=FACE;
const int scanPin=38, undoPin=-1, resetPin=-1;
const char *saveTopic="cpe477-peerach/save", *ackTopic="cpe477-peerach/saved";
String employee, employeeId, label[12], roundId, request, photoId, pending;
int need[12], got[12], history[100], kinds=0, total=0, count=0;
unsigned long phaseAt, previewAt, connectAt, sendAt, seq=0;
bool ready=false;
const int ledRed=39, ledGreen=48, ledOrange=40, buzzerPin=1;
const unsigned short beepMs=55; // Active-low active buzzer; duration, not volume.
unsigned long signalAt=0;
int signalColor=-1;
bool deviceFault=false;
unsigned long saveStarted=0;

void soundTask(void *arg){
  uint32_t pattern;
  for(;;){
    xTaskNotifyWait(0,UINT32_MAX,&pattern,portMAX_DELAY);
    for(unsigned i=0;i<(pattern==2?2U:1U);++i){
      digitalWrite(buzzerPin,LOW);
      vTaskDelay(pdMS_TO_TICKS(pattern==3?180:beepMs));
      digitalWrite(buzzerPin,HIGH);
      if(pattern==2 && i==0) vTaskDelay(pdMS_TO_TICKS(95));
    }
  }
}
TaskHandle_t soundHandle=nullptr;
void signalResult(int color,int pattern){
  signalColor=color; signalAt=millis();
  if(pattern && soundHandle) xTaskNotify(soundHandle,pattern,eSetValueWithOverwrite);
}
// HTTP lamp traffic runs independently from display and Serial.
portMUX_TYPE lampLock = portMUX_INITIALIZER_UNLOCKED;
bool lampWanted=false, lampAcknowledged=false;
uint32_t lampGeneration=0;
TaskHandle_t lampTaskHandle=nullptr;
bool lampOK=false;

void lampWorker(void *) {
  for (;;) {
    bool wanted;
    uint32_t generation;
    portENTER_CRITICAL(&lampLock);
    wanted=lampWanted;
    generation=lampGeneration;
    portEXIT_CRITICAL(&lampLock);
    bool success=false;
    if(WiFi.status()==WL_CONNECTED) {
      HTTPClient http;
      http.setConnectTimeout(400);
      http.setTimeout(700);
      if(http.begin(itemCamBase+String("/lamp?on=")+(wanted?"1":"0"))) {
        int status=http.POST("");
        success=status==200 && http.getString()==(wanted?"ON":"OFF");
        http.end();
      }
    }
    portENTER_CRITICAL(&lampLock);
    if(generation==lampGeneration) lampAcknowledged=success;
    portEXIT_CRITICAL(&lampLock);
    ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(success?4000:700));
  }
}
void syncLamp() {
  bool wanted=phase==ITEM || phase==WAIT_ITEM || phase==COMPLETE ||
              phase==CONFIRM || phase==UNDO;
  bool changed=false;
  portENTER_CRITICAL(&lampLock);
  if(wanted!=lampWanted) {
    lampWanted=wanted;
    lampAcknowledged=false;
    lampGeneration++;
    changed=true;
  }
  lampOK=lampAcknowledged && WiFi.status()==WL_CONNECTED;
  portEXIT_CRITICAL(&lampLock);
  if(changed && lampTaskHandle) xTaskNotifyGive(lampTaskHandle);
}
void updateLights(){
  bool busy=phase==WAIT_FACE || phase==WAIT_ITEM || phase==SAVING;
  bool offline=deviceFault || WiFi.status()!=WL_CONNECTED ||
    (!SERIAL_TEST_MODE && !mqtt.connected()) ||
    (phase==SAVING && !SERIAL_TEST_MODE && millis()-saveStarted>8000);
  bool complete=phase==COMPLETE || phase==CONFIRM || phase==DONE;
  bool red=signalColor==ledRed && millis()-signalAt<2000;
  bool green=signalColor==ledGreen && millis()-signalAt<1000;
  digitalWrite(ledRed,!busy && red);
  digitalWrite(ledGreen,!busy && !red && (complete || green));
  digitalWrite(ledOrange,offline ? (millis()/450)%2 : busy);
}
struct Key {
  explicit Key(int gpio):pin(gpio){}
  int pin; bool raw=false, down=false; unsigned long changed=0;
  bool poll() {
    if (pin < 0) return false;
    bool value=digitalRead(pin)==LOW;
    if(value!=raw){raw=value;changed=millis();}
    if(raw!=down && millis()-changed>=35){down=raw;return down;}
    return false;
  }
};
Key scan{scanPin}, back{undoPin}, resetKey{resetPin};
bool paint(int16_t x,int16_t y,uint16_t w,uint16_t h,uint16_t *pixels){
  const int right=phase==FACE ? 320 : 160;
  const int bottom=phase==FACE ? 216 : 150;
  if(y>=bottom) return false;
  if(x<0 || y<0 || x+w>right) return true;
  uint16_t rows=min((int)h,bottom-y);
  // Send each decoded JPEG block as one SPI transaction.
  tft.startWrite();
  tft.setAddrWindow(x,y,w,rows);
  tft.writePixels(pixels,(uint32_t)w*rows);
  tft.endWrite();
  return true;
}
void screen(const String &message){
  faceLayout=(phase==FACE || phase==WAIT_FACE);
  phaseAt=millis();
  tft.fillScreen(ILI9341_BLACK); tft.setCursor(0,0); tft.setTextSize(1);
  tft.print(employee.length()?employee:"Ready2Gear");
  if(kinds){tft.printf("   %d/%d",count,total);}
  if(kinds<=4){
    tft.setTextSize(2);
    for(int i=0;i<kinds;i++){
      tft.setCursor(165,18+i*40);tft.print(label[i].substring(0,12));
      tft.setCursor(165,34+i*40);tft.printf("%d/%d",got[i],need[i]);
    }
  }else{
    for(int i=0;i<kinds;i++){tft.setCursor(165,18+i*12);tft.printf("%s %d/%d",label[i].c_str(),got[i],need[i]);}
  }
  statusLine(message);
}
void startRound(){
  signalColor=-1; deviceFault=false;
  employee=""; employeeId=""; kinds=total=count=0; phase=FACE; request="";
  roundId=String((uint32_t)(ESP.getEfuseMac()>>16),HEX)+"-"+String(esp_random(),HEX)+"-"+String(esp_random(),HEX);
  syncLamp();
  screen("Scan face");
}
bool getFrame(bool face){
  if(!face) return getItemImage();
  camera_fb_t *f=esp_camera_fb_get();
  if(!f) return false;
  faceData=(uint8_t*)malloc(f->len);
  if(faceData){faceSize=f->len;memcpy(faceData,f->buf,faceSize);}
  esp_camera_fb_return(f);
  return faceData!=nullptr;
}
uint8_t *photo=nullptr; size_t bytes=0;
void frame(bool capture){
  bool face=phase==FACE;
  if(capture && !SERIAL_TEST_MODE && !mqtt.connected()){statusLine("MQTT offline - retry");return;}
  if(!face && !lampOK){
    if(capture)statusLine("Waiting for item CAM lamp");
    return;
  }
  if(face && !capture){
    camera_fb_t *preview=esp_camera_fb_get();
    if(preview){
      publishLiveFrame(preview->buf,preview->len);
      TJpgDec.setJpgScale(1);
      TJpgDec.drawJpg(0,0,preview->buf,preview->len);
      esp_camera_fb_return(preview);
    }
    return;
  }
  if(!getFrame(face)){deviceFault=true;if(capture){signalResult(ledRed,2);statusLine("Camera failed - retry");}return;}
  deviceFault=false;
  if(face)publishLiveFrame(faceData,faceSize);
  TJpgDec.setJpgScale(face ? 1 : 2);
  TJpgDec.drawJpg(0,face ? 0 : 20,faceData,faceSize);
  if(capture){
    free(photo);photo=faceData;bytes=faceSize;
    photoId=roundId+"-"+String(++seq); request=photoId;
    JsonDocument d; d["cmd"]=face?"face":"item"; d["request_id"]=request;
    d["image_url"]="http://"+WiFi.localIP().toString()+"/shot?id="+photoId;
    String msg;serializeJson(d,msg);Serial.println(msg);
    if(SERIAL_TEST_MODE || mqtt.publish(TOPIC_REQ,msg.c_str())){phase=face?WAIT_FACE:WAIT_ITEM;phaseAt=millis();statusLine("Checking...");}
    else statusLine("Send failed - retry");
  } else free(faceData);
  faceData=nullptr;faceSize=0;
}
void result(char *topic, byte *data,unsigned int len){
  JsonDocument d;if(deserializeJson(d,data,len))return;
  if(strcmp(topic,ackTopic)==0){
    if(phase==SAVING && roundId==(d["round_id"]|"") && d["saved"]==true){
      prefs.remove("pending");pending="";phase=DONE;signalResult(ledGreen,3);screen(SERIAL_TEST_MODE ? "TEST saved - hold RESET" : "Saved - hold RESET");
    }
    return;
  }
  if(request!=(d["request_id"]|""))return;
  if(phase==WAIT_FACE){
    phase=FACE;
    JsonArray list=d["required"].as<JsonArray>();
    String name=d["name"]|"", id=d["employee_id"]|"";
    if(!id.length()||!name.length()||name=="unknown"||!list.size()||list.size()>12){signalResult(ledRed,2);screen("Face / list invalid. Retry");return;}
    int sum=0,n=0;
    for(JsonObject entry:list){
      String key=entry["label"]|"";int qty=entry["qty"]|0;
      for(int j=0;j<n;j++)if(key==label[j])qty=0;
      if(!key.length()||qty<=0||qty>100-sum){screen("Invalid checklist");return;}
      label[n]=key;need[n]=qty;got[n++]=0;sum+=qty;
    }
    if(n!=(int)list.size()){screen("Invalid checklist");return;}
    kinds=n;total=sum;employee=name;employeeId=id;phase=ITEM;signalResult(ledGreen,0);screen("Scan one item");return;
  }
  if(phase!=WAIT_ITEM)return;
  phase=ITEM;String detected=d["label"]|"";
  for(int i=0;i<kinds;i++)if(label[i]==detected && got[i]<need[i]){
    signalResult(ledGreen,1);
    history[count++]=i;got[i]++;
    if(count==total)phase=COMPLETE;
    screen(phase==COMPLETE?"Checklist complete":"OK - remove item");return;
  }
  signalResult(ledRed,2);
  screen(detected.length()?"Unexpected / extra":"Unclear - scan again");
}
void saveRound(){
  JsonDocument d;d["round_id"]=roundId;d["name"]=employee;d["employee_id"]=employeeId;
  for(int i=0;i<kinds;i++){JsonObject row=d["items"].add<JsonObject>();row["label"]=label[i];row["qty"]=got[i];}
  pending="";serializeJson(d,pending);
  if(!prefs.putString("pending",pending)){pending="";screen("Storage error - retry");return;}
  phase=SAVING;syncLamp();saveStarted=millis();screen("Saving...");sendAt=millis()-5000;
}

String serialCommand;
bool commandOverflow = false;
bool serialScan = false, serialBack = false, serialReset = false;

void testCommand(String command) {
  command.trim();
  if (command == "help") {
    Serial.println("TEST: scan | person Somchai | unknown | item helmet | item hammer | item screwdriver | back | reset | saved");
    Serial.println("Use scan before each simulated result. Test checklist = helmet 1, hammer 1, screwdriver 2.");
    return;
  }
  if(command=="back"){serialBack=true;return;}
  if(command=="reset"){serialReset=true;return;}
  // >>>>> [R2G-ADD] พิมพ์ scan ใน Serial Monitor = กดปุ่ม SCAN (ใช้ได้ใน LIVE MODE ด้วย)
  if(command=="scan"){serialScan=true;return;}
  // <<<<< [R2G-END]
  if (!SERIAL_TEST_MODE) {
    Serial.println("Set SERIAL_TEST_MODE=true and upload to use test commands.");
    return;
  }
  // >>>>> [R2G-OFF] ย้ายขึ้นไปไว้ก่อนเช็ก SERIAL_TEST_MODE แล้ว (ดูด้านบน)
  // if (command == "scan") { serialScan = true; return; }
  // <<<<< [R2G-END]

  JsonDocument response;
  response["request_id"] = request;
  const char *topic = TOPIC_RESULT;
  if (command == "saved" && phase == SAVING) {
    response["round_id"] = roundId;
    response["saved"] = true;
    topic = ackTopic;
  } else if (phase == WAIT_FACE && command.startsWith("person ")) {
    String name = command.substring(7);
    name.trim();
    if (!name.length()) { Serial.println("Use: person Somchai"); return; }
    response["employee_id"] = "TEST-E001";
    response["name"] = name;
    const char *names[] = {"helmet", "hammer", "screwdriver"};
    for (int i = 0; i < 3; i++) {
      JsonObject row = response["required"].add<JsonObject>();
      row["label"] = names[i];
      row["qty"] = i == 2 ? 2 : 1;
    }
  } else if (phase == WAIT_FACE && command == "unknown") {
    response["name"] = "unknown";
  } else if (phase == WAIT_ITEM && command.startsWith("item ")) {
    String value = command.substring(5);
    value.trim();
    response["label"] = value;
  } else {
    Serial.printf("Command unavailable. Phase=%d. Use scan, wait for Checking, then send result.\n",(int)phase);
    return;
  }
  String json;
  serializeJson(response, json);
  Serial.println("SIMULATED result: " + json);
  result(const_cast<char *>(topic), reinterpret_cast<byte *>(const_cast<char *>(json.c_str())), json.length());
}

void readTestSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (commandOverflow) Serial.println("Command too long.");
      else if (serialCommand.length()) testCommand(serialCommand);
      serialCommand = "";
      commandOverflow = false;
      return;
    } else if (serialCommand.length() < 160) {
      serialCommand += c;
    } else commandOverflow = true;
  }
}

// >>>>> [R2G-ADD] ตัวช่วยหาจุดค้าง: task แยกจาก loop พิมพ์สถานะทุก 1 วินาที
// stage = loop ทำถึงขั้นไหนล่าสุด ถ้าค้างคำเดิมตลอด = ค้างที่ขั้นนั้น
// แก้ปัญหาเสร็จแล้วปิดได้ โดยตั้ง R2G_DEBUG เป็น false
const bool R2G_DEBUG = false;
const char * volatile dbgStage = "boot";
void dbgTask(void*){
  for(;;){
    Serial.printf("[dbg] stage=%s BTN38=%d phase=%d mqtt=%d wifi=%d\n",
      dbgStage, digitalRead(scanPin), (int)phase, mqtt.connected(), WiFi.status()==WL_CONNECTED);
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
// <<<<< [R2G-END]

void setup(){
  Serial.begin(115200);
  Serial.println("Ready2Gear LivePreview NETFIX-1");pinMode(scanPin,INPUT_PULLUP);if(undoPin>=0) pinMode(undoPin,INPUT_PULLUP); if(resetPin>=0) pinMode(resetPin,INPUT_PULLUP);
  digitalWrite(buzzerPin,HIGH); pinMode(buzzerPin,OUTPUT); digitalWrite(buzzerPin,HIGH);
  for(int pin : {ledRed,ledGreen,ledOrange}) {pinMode(pin,OUTPUT);digitalWrite(pin,LOW);}
  xTaskCreate(soundTask,"buzzer",2048,nullptr,2,&soundHandle);
  SPI.begin(47,-1,21,41);tft.begin(TFT_SPI_HZ);tft.setRotation(1);tft.setTextColor(ILI9341_WHITE);
  if(!initCamera()){statusLine("Camera init failed");return;}
  if(!initLivePreview()){
    Serial.println("Live preview unavailable; inspection can continue");
  }
  TJpgDec.setJpgScale(2);TJpgDec.setCallback(paint);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid,password);WiFi.setSleep(false);mqtt.setServer(MQTT_HOST,MQTT_PORT);mqtt.setCallback(result);mqtt.setBufferSize(4096);
  web.on("/",[](){web.send(200,"text/html",LIVE_VIEW_PAGE);});
  // >>>>> [R2G-ADD] ปุ่มบนเว็บแทนปุ่มจริง: เปิด http://<IP S3>/btn บนมือถือ/คอม (เน็ตเดียวกัน)
  web.on("/btn",[](){web.send(200,"text/html","<meta name=viewport content='width=device-width'><body style='font-family:sans-serif;text-align:center'><button style='font-size:40px;width:90%;height:160px;margin-top:20px' onclick=\"fetch('/press')\">SCAN</button><br><br><button style='font-size:24px;width:90%;height:80px' onclick=\"fetch('/reset')\">RESET (new round)</button></body>");});
  web.on("/press",[](){serialScan=true;web.send(200,"text/plain","pressed");});
  web.on("/reset",[](){serialReset=true;web.send(200,"text/plain","reset");});
  // <<<<< [R2G-END]
  web.on("/shot",[](){
    if(!photo||(web.hasArg("id")&&web.arg("id")!=photoId)){web.send(404,"text/plain","No matching photo");return;}
    web.sendHeader("Cache-Control","no-store");web.send_P(200,"image/jpeg",(const char*)photo,bytes);
  });web.begin();
  prefs.begin(SERIAL_TEST_MODE ? "r2g-test" : "ready2gear",false);startRound();pending=prefs.getString("pending","");
  if(pending.length()){JsonDocument d;deserializeJson(d,pending);roundId=d["round_id"].as<String>();phase=SAVING;saveStarted=millis();screen("Checking saved record");}
  if(xTaskCreate(lampWorker,"lamp-http",6144,nullptr,1,&lampTaskHandle)!=pdPASS){
    statusLine("Lamp task failed - restart");
    return;
  }
  // >>>>> [R2G-ADD] เริ่ม task หาจุดค้าง
  if(R2G_DEBUG) xTaskCreate(dbgTask,"dbg",4096,nullptr,1,nullptr);
  // <<<<< [R2G-END]
  ready=true;
  Serial.println(SERIAL_TEST_MODE ? "SERIAL TEST MODE - type help" : "LIVE MODE");
}
void loop(){
  if(!ready)return;
  dbgStage="live";  // [R2G-ADD] จุดเช็กค้าง
  serviceLiveServer();
  static bool announced=false;
  bool connected=WiFi.status()==WL_CONNECTED;
  if(connected && !announced){
    Serial.println("Waiting for MJPEG server readiness message");
    Serial.println("S3 viewer: http://"+WiFi.localIP().toString()+"/");
  }
  announced=connected;
  // Refresh the S3 face preview for a viewer even while inspecting items.
  // The frozen inspection JPEG in photo/bytes is never modified here.
  static unsigned long backgroundPreviewAt=0;
  if(liveRequested && phase!=FACE && millis()-backgroundPreviewAt>=100){
    dbgStage="bgcam";  // [R2G-ADD] จุดเช็กค้าง
    camera_fb_t *live=esp_camera_fb_get();
    if(live){
      publishLiveFrame(live->buf,live->len);
      esp_camera_fb_return(live);
    }
    backgroundPreviewAt=millis();
  }
  dbgStage="lamp";  // [R2G-ADD] จุดเช็กค้าง
  syncLamp();
  updateLights();
  dbgStage="web+mqtt";  // [R2G-ADD] จุดเช็กค้าง
  web.handleClient();mqtt.loop();
  if(!SERIAL_TEST_MODE&&!mqtt.connected()&&WiFi.status()==WL_CONNECTED&&millis()-connectAt>=3000){
    dbgStage="mqtt-connect";  // [R2G-ADD] จุดเช็กค้าง
    connectAt=millis();String client="s3-"+String((uint32_t)ESP.getEfuseMac(),HEX);
    if(mqtt.connect(client.c_str())){mqtt.subscribe(TOPIC_RESULT);mqtt.subscribe(ackTopic);}
  }
  dbgStage="serial";  // [R2G-ADD] จุดเช็กค้าง
  readTestSerial();
  dbgStage="keys";  // [R2G-ADD] จุดเช็กค้าง
  bool pressed=scan.poll(), edit=back.poll();resetKey.poll();
  // >>>>> [R2G-ADD] บอกใน Serial ว่ากดปุ่มมาจากไหน
  if(pressed) Serial.println(">> PRESSED (button GPIO38)");
  if(serialScan) Serial.println(">> PRESSED (web/serial)");
  // <<<<< [R2G-END]
  pressed = pressed || serialScan;
  edit = edit || serialBack;
  serialScan = serialBack = false;
  if (serialReset) {
    serialReset = false;
    if (phase != SAVING) { startRound(); return; }
    Serial.println("Pending save: send saved to acknowledge the test record.");
  }
  static bool resetDone=false;
  if(!resetKey.down)resetDone=false;
  if(resetKey.down&&!resetDone&&millis()-resetKey.changed>=2000){
    resetDone=true;if(phase!=SAVING){startRound();return;}
  }
  if(!SERIAL_TEST_MODE && phase==SAVING && mqtt.connected()&&millis()-sendAt>=5000){mqtt.publish(saveTopic,pending.c_str());sendAt=millis();}
  if((phase==WAIT_FACE||phase==WAIT_ITEM)&&millis()-phaseAt>=30000){phase=phase==WAIT_FACE?FACE:ITEM;signalResult(ledRed,2);screen("Timed out - retry");}
  // >>>>> [R2G-ADD] บอร์ดนี้ไม่มีปุ่ม RESET (resetPin=-1) บันทึกเสร็จ 3 วิ เริ่มรอบใหม่เอง
  if(phase==DONE && millis()-phaseAt>=3000){startRound();return;}
  // <<<<< [R2G-END]
  if(phase==COMPLETE && millis()-phaseAt>=2000){phase=CONFIRM;screen("Confirm checkout? SCAN=yes");return;}
  if(edit && count && (phase==ITEM||phase==CONFIRM)){phase=UNDO;screen("Undo "+label[history[count-1]]+"? SCAN=yes BACK=no");return;}
  if(edit&&phase==UNDO){phase=count==total?CONFIRM:ITEM;screen(phase==CONFIRM?"Confirm checkout?":"Scan item");return;}
  if(pressed){
    dbgStage="capture";  // [R2G-ADD] จุดเช็กค้าง
    if(phase==CONFIRM)saveRound();
    else if(phase==UNDO){got[history[--count]]--;phase=ITEM;screen("Removed. Scan item");}
    else if(phase==FACE||phase==ITEM)frame(true);
  } else if((phase==FACE||phase==ITEM)&&millis()-previewAt>=20){
    dbgStage="preview";  // [R2G-ADD] จุดเช็กค้าง
    frame(false);previewAt=millis();
  }
  dbgStage="end";  // [R2G-ADD] จุดเช็กค้าง (loop จบรอบได้ปกติ)
}
