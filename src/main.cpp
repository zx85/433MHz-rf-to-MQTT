#include <Arduino.h>
/*
  Example for receiving
  
  https://github.com/sui77/rc-switch/
  
  If you want to visualize a telegram copy the raw data and 
  paste it into http://test.sui.li/oszi/
*/

#include <RCSwitch.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <time.h>
#include "output.h"  // <-- added include to use output()
#include "config.h"

RCSwitch mySwitch = RCSwitch();
WiFiClient espClient;
PubSubClient mqttClient(espClient);

const long gmtOffset_sec = 0;       
const int daylightOffset_sec = 3600;

void setup_wifi(unsigned long timeoutMs = 8000) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, wifi_password);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
    delay(100);
  }
}

void setupTime() {
  configTime(gmtOffset_sec, daylightOffset_sec, "pool.ntp.org", "time.nist.gov");
  Serial.print("Waiting for NTP time sync...");
  
  time_t now = time(nullptr);
  while (now < 100000) {   // crude check for valid timestamp
    delay(500);
    Serial.print(".");
    now = time(nullptr);
  }
  Serial.println(" done!");
  
  struct tm timeinfo;
  gmtime_r(&now, &timeinfo);
  Serial.printf("Current time: %02d:%02d:%02d %02d/%02d/%04d\n",
                timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec,
                timeinfo.tm_mday, timeinfo.tm_mon + 1, timeinfo.tm_year + 1900);
}

bool mqttConnect(const char *clientId = "esp_flood") {
  if (mqttClient.connected()) return true;
  mqttClient.setServer(mqtt_server, mqtt_port);

  unsigned long start = millis();
  const unsigned long timeout = 4000;
  while (!mqttClient.connected() && (millis() - start) < timeout) {
    if (mqtt_user && mqtt_pass && mqtt_user[0] != '\0') {
      if (mqttClient.connect(clientId, mqtt_user, mqtt_pass)) return true;
    } else {
      if (mqttClient.connect(clientId)) return true;
    }
    delay(200);
  }
  return mqttClient.connected();
}

void publishStatus(long value, char timeString[30]) {
  char payload[96];
  // simple JSON-ish payload
  snprintf(payload, sizeof(payload),
           "{\"rf_value\":%ld,\"timestamp\":\"%s\"}",
           value, timeString);

  mqttClient.publish(mqtt_topic, payload, true);
}


void setup() {
  Serial.begin(9600);
  mySwitch.enableReceive(D4);  // Receiver on interrupt 2 => that is pin D4
  Serial.print("c'mon then");
}

void loop() {
  if (mySwitch.available()) {
    long received_value=mySwitch.getReceivedValue();
    output(received_value, mySwitch.getReceivedBitlength(), mySwitch.getReceivedDelay(), mySwitch.getReceivedRawdata(),mySwitch.getReceivedProtocol());
    setup_wifi(7000); // 7 second timeout for WiFi
    if (WiFi.status() == WL_CONNECTED) {
      // Time sync
      setupTime();
      time_t now = time(nullptr);
      struct tm timeinfo;
      gmtime_r(&now, &timeinfo);
      char timeString[30];
      strftime(timeString, sizeof(timeString), "%Y-%m-%d %H:%M:%S", &timeinfo);

    // Connect MQTT and publish
    if (mqttConnect()) {
      publishStatus(received_value, timeString);
      mqttClient.disconnect();
    }
    // we can also WiFi.disconnect(true) to cut off quickly
    WiFi.disconnect(true);
  }

    mySwitch.resetAvailable();
  }
}

