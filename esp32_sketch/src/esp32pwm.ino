#include <DNSServer.h>
#include <driver/adc.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "Preferences.h"
#include "AsyncJson.h"
#include <ArduinoJson.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <SPIFFS.h>
#include "ESPAsyncWebServer.h"

const int ledpin = 19;
const int sensepin = 36;

int sensevalue = 0;
int brightness;
int brightnesstarget;

int minbrightness;
int maxbrightness;
int transitiontime;
int minresponseval;
int maxresponseval;
int peaksensevalue;
String str_ssid;
String str_ssidpw;

DNSServer dnsServer;
Preferences preferences;
AsyncWebServer server(80);

class CaptiveRequestHandler : public AsyncWebHandler {
public:
  CaptiveRequestHandler() {}
  virtual ~CaptiveRequestHandler() {}

  bool canHandle(AsyncWebServerRequest *request){
    //request->addInterestingHeader("ANY");
    return true;
  }

  void handleRequest(AsyncWebServerRequest *request) {
    request->send(SPIFFS, "/index.html");
  }
};

String lightvalue(){
  int i;
  int value = 0;
  int numReadings = 20;
  for (i = 0; i < numReadings; i++){
    value = value + adc1_get_raw(ADC1_CHANNEL_0);
    delay(1);
  }
  value = value / numReadings;
  value = value / 4;
  return String(value);
}

String backlightvalue(){
  return String(brightnesstarget);
}

void load_settings(){
  minbrightness = preferences.getInt("minbrightness", 40);
  maxbrightness = preferences.getInt("maxbrightness", 255);
  transitiontime = preferences.getInt("transitiontime", 3);
  minresponseval = preferences.getInt("minresponseval", 40);
  maxresponseval = preferences.getInt("maxresponseval", 150);
  peaksensevalue = preferences.getInt("peaksensevalue", 1);
  str_ssid = preferences.getString("str_ssid", "espbl");
  str_ssidpw = preferences.getString("str_ssidpw", "");
}
void setup(){
  //your other setup stuff...
  Serial.begin(115200);
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); //disable brownout detector 
  
  preferences.begin("espbl", false); 
  load_settings();
  brightnesstarget = minbrightness;

  WiFi.softAP(str_ssid.c_str(), str_ssidpw.c_str());
  //WiFi.softAP("espbl");
  ledcSetup(0, 40000, 8); // 12 kHz PWM, 8-bit resolution
  ledcAttachPin(ledpin, 0);
  adc1_config_channel_atten(ADC1_CHANNEL_0,ADC_ATTEN_DB_11);
  adc1_config_width(ADC_WIDTH_10Bit);
  if(!SPIFFS.begin()){
    Serial.println("An Error has occurred while mounting SPIFFS");
    return;
  }
  
  dnsServer.start(53, "*", WiFi.softAPIP());
  server.on("/test", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(SPIFFS, "/index.html");
  });
    server.on("^\\/.*.\.(js|css)$", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(SPIFFS, request->url());
  });
  server.on("/lightvalue", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/plain", lightvalue().c_str());
  });
  server.on("/backlightvalue", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/plain", backlightvalue().c_str());
  });
    server.on("/backlightchart", HTTP_GET, [](AsyncWebServerRequest *request){
    String charvals;
    charvals = "[" + backlightvalue() + "," + lightvalue() + "]";
    request->send_P(200, "text/plain", charvals.c_str());
  });

  server.on("/", HTTP_POST, [](AsyncWebServerRequest *request) {
    String message;
    bool reboot = false;
    int params = request->params();
    Serial.printf("%d params sent in\n", params);
    for (int i = 0; i < params; i++){
        AsyncWebParameter *p = request->getParam(i);
        if (p->name() =="set_reboot") {
          reboot = true;
          continue;
        }
        if (p->name() =="set_resetpeak") {
          update_peaksense(0,true);
          continue;
        }

        if (p->value() == 0) {
          preferences.remove(p->name().c_str());
          continue;
        }
        if (p->name().startsWith("str_")) {
          preferences.putString(p->name().c_str(), p->value().c_str());
          continue;
        }
        preferences.putInt(p->name().c_str(), p->value().toInt());
        Serial.printf("%s: %s \n", p->name().c_str(), p->value().c_str());
    }
    if (reboot == true) {
      ESP.restart();
    }
    load_settings();
    request->redirect("/");
  });

  server.on("/settings", HTTP_GET, [](AsyncWebServerRequest *request){
    StaticJsonDocument<512>  settings;
    settings["str_ssid"] = str_ssid;
    settings["str_ssidpw"] = str_ssidpw;
    settings["minbrightness"] = minbrightness;
    settings["maxbrightness"] = maxbrightness;
    settings["transitiontime"] = transitiontime;
    settings["minresponseval"] = minresponseval;
    settings["maxresponseval"] = maxresponseval;
    settings["peaksensevalue"] = peaksensevalue;
    String response;
    serializeJson(settings, response);
    request->send(200, "application/json", response);
  });
  server.addHandler(new CaptiveRequestHandler()).setFilter(ON_AP_FILTER);//only when requested from AP
  
  //more handlers...
  server.begin();
}

void setbacklight() {
  
  int steps;
  if (brightness == brightnesstarget) {
    return;
  }
  if (brightnesstarget > maxbrightness) {
    brightnesstarget = maxbrightness;
  }
  if (brightness < brightnesstarget) {
    steps = brightnesstarget - brightness;
  }

  if (brightness > brightnesstarget) {
    steps = brightness - brightnesstarget;
  }

  int step_interval = (1000 * transitiontime) / steps;
  Serial.println(steps);

  while (brightnesstarget != brightness) {
    if (brightnesstarget > brightness) {
      brightness = brightness + 1;
    }
    if (brightnesstarget < brightness) {
      brightness = brightness - 1;
    }
    delay(step_interval);
    Serial.println(brightness);
    ledcWrite(0, brightness);
    if (brightness == brightnesstarget) {
      return;
    }
  }
  return;
}

void update_peaksense(int current_value, bool reset) {
  if (reset) {
    preferences.putInt("peaksensevalue", 0);
  }
  if (current_value > peaksensevalue) {
    preferences.putInt("peaksensevalue", current_value);
  }
  peaksensevalue = preferences.getInt("peaksensevalue", 1);
}

void loop(){
  dnsServer.processNextRequest();
  setbacklight();
  int sensorval = lightvalue().toInt();
  int calcbrightness = sensorval*2;
  if (calcbrightness > maxbrightness) {
    calcbrightness = maxbrightness;
  }
  if (calcbrightness < minbrightness) {
    brightnesstarget = minbrightness;
  }
  if (calcbrightness > minbrightness) {
    brightnesstarget = calcbrightness;
  }
  sensevalue = lightvalue().toInt();
  update_peaksense(sensevalue, false);
  delay(1000);
}