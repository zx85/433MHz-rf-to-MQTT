#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ELECHOUSE_CC1101_SRC_DRV.h>
#include "config.h"

// ── Globals ────────────────────────────────────────────────
WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

volatile bool packetReady    = false;
unsigned long lastDetectedMs = 0;

// ── Forward declarations ────────────────────────────────────
void connectWifi();
void connectMqtt();
void IRAM_ATTR onGDO0Interrupt();
void initCC1101();
bool decodeAndMatch();
void publishDoorbell();

// ── Setup ───────────────────────────────────────────────────
void setup() {
    Serial.begin(SERIAL_BAUD);
    Serial.println("\n[doorbell] Starting up");

    connectWifi();

    mqtt.setServer(MQTT_BROKER, MQTT_PORT);
    // Last-will so HA knows if the device goes offline
    mqtt.connect(
        MQTT_CLIENT_ID,
        MQTT_USER, MQTT_PASSWORD,
        MQTT_TOPIC_STATUS, 0, true, "offline"
    );
    connectMqtt();

    initCC1101();

    attachInterrupt(digitalPinToInterrupt(CC1101_GDO0_PIN),
                    onGDO0Interrupt, CHANGE);

    Serial.println("[doorbell] Ready - listening for signal");
}

// ── Loop ────────────────────────────────────────────────────
void loop() {
    if (!mqtt.connected()) {
        connectMqtt();
    }
    mqtt.loop();

    if (packetReady) {
        packetReady = false;

        unsigned long now = millis();
        if (now - lastDetectedMs > DOORBELL_LOCKOUT_MS) {
            if (decodeAndMatch()) {
                lastDetectedMs = now;
                publishDoorbell();
            }
        }
    }
}

// ── WiFi ────────────────────────────────────────────────────
void connectWifi() {
    Serial.printf("[wifi] Connecting to %s\n", WIFI_SSID);

#if WIFI_USE_STATIC_IP
    IPAddress ip, gateway, subnet, dns;
    ip.fromString(WIFI_IP);
    gateway.fromString(WIFI_GATEWAY);
    subnet.fromString(WIFI_SUBNET);
    dns.fromString(WIFI_DNS);
    WiFi.config(ip, gateway, subnet, dns);
#endif

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.printf("\n[wifi] Connected. IP: %s\n",
                  WiFi.localIP().toString().c_str());
}

// ── MQTT ────────────────────────────────────────────────────
void connectMqtt() {
    while (!mqtt.connected()) {
        Serial.print("[mqtt] Connecting...");
        if (mqtt.connect(MQTT_CLIENT_ID,
                         MQTT_USER, MQTT_PASSWORD,
                         MQTT_TOPIC_STATUS, 0, true, "offline")) {
            Serial.println(" connected");
            mqtt.publish(MQTT_TOPIC_STATUS, "online", true);
        } else {
            Serial.printf(" failed (rc=%d), retrying in 5s\n",
                          mqtt.state());
            delay(5000);
        }
    }
}

// ── CC1101 init ─────────────────────────────────────────────
void initCC1101() {
    ELECHOUSE_cc1101.setSpiPin(14, 12, 13, CC1101_CSN_PIN); // SCK, MISO, MOSI, CSN
    ELECHOUSE_cc1101.Init();
    ELECHOUSE_cc1101.setMHZ(CC1101_FREQUENCY);
    ELECHOUSE_cc1101.SetRx();   // Start in receive mode

    Serial.printf("[cc1101] Initialised at %.3f MHz\n", CC1101_FREQUENCY);
}

// ── Interrupt ───────────────────────────────────────────────
void IRAM_ATTR onGDO0Interrupt() {
    // TODO: capture pulse timing here for PWM decode.
    // Will record micros() timestamps on RISING/FALLING edges
    // and set packetReady = true when a reset gap is detected.
    packetReady = true;  // placeholder until pulse decoder is implemented
}

// ── Decode & match ──────────────────────────────────────────
bool decodeAndMatch() {
    // TODO: implement PWM pulse decoder using the captured
    // edge timings from onGDO0Interrupt().
    // Compare decoded hex string prefix against DOORBELL_CODE_PREFIX.
    // Return true only if prefix matches and bit count >= DOORBELL_CODE_MIN_BITS.

    Serial.println("[decode] Packet received - decoder not yet implemented");
    return false;  // placeholder
}

// ── Publish ─────────────────────────────────────────────────
void publishDoorbell() {
    Serial.println("[doorbell] *** DOORBELL DETECTED - publishing MQTT ***");
    mqtt.publish(MQTT_TOPIC_DOORBELL, "ON", false);

    // HA often expects a state reset - publish OFF after short delay
    delay(500);
    mqtt.publish(MQTT_TOPIC_DOORBELL, "OFF", false);
}