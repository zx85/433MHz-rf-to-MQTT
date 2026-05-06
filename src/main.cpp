#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ELECHOUSE_CC1101_SRC_DRV.h>
#include "config.h"

// ── Pulse capture buffer ────────────────────────────────────
#define MAX_PULSES 150

volatile uint32_t pulseWidths[MAX_PULSES];  // HIGH pulse durations (µs)
volatile uint16_t pulseCount    = 0;
volatile bool     packetReady   = false;
volatile uint32_t lastRising    = 0;
volatile uint32_t lastFalling   = 0;

// ── Globals ─────────────────────────────────────────────────
WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);
unsigned long lastDetectedMs = 0;

// ── Forward declarations ────────────────────────────────────
void connectWifi();
void connectMqtt();
void IRAM_ATTR onGDO0Interrupt();
void initCC1101();
bool decodeAndMatch();
void publishDoorbell();

// ── Setup ────────────────────────────────────────────────────
void setup() {
    Serial.begin(SERIAL_BAUD);
    Serial.println("\n[doorbell] Starting up");

    connectWifi();

    mqtt.setServer(MQTT_BROKER, MQTT_PORT);
    connectMqtt();

    initCC1101();

    attachInterrupt(digitalPinToInterrupt(CC1101_GDO0_PIN),
                    onGDO0Interrupt, CHANGE);

    Serial.println("[doorbell] Ready - listening for signal");
}

// ── Loop ─────────────────────────────────────────────────────
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
        } else {
            // Still in lockout - discard
            pulseCount = 0;
        }
    }
}

// ── WiFi ─────────────────────────────────────────────────────
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

// ── MQTT ──────────────────────────────────────────────────────
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

// ── CC1101 init ───────────────────────────────────────────────
void initCC1101() {
    ELECHOUSE_cc1101.setSpiPin(14, 12, 13, CC1101_CSN_PIN);
    ELECHOUSE_cc1101.Init();
    ELECHOUSE_cc1101.setMHZ(CC1101_FREQUENCY);
    ELECHOUSE_cc1101.SetRx();

    Serial.printf("[cc1101] Initialised at %.3f MHz\n", CC1101_FREQUENCY);
}

// ── Interrupt ─────────────────────────────────────────────────
// Captures HIGH pulse widths. Triggers packet-ready when a
// reset gap (long LOW) is detected on the next rising edge.
void IRAM_ATTR onGDO0Interrupt() {
    uint32_t now = micros();

    if (digitalRead(CC1101_GDO0_PIN) == HIGH) {
        // Rising edge: check if gap since last falling = end of packet
        if (lastFalling > 0) {
            uint32_t gap = now - lastFalling;
            if (gap > PULSE_RESET_US && pulseCount > 10) {
                packetReady = true;
                // Don't reset pulseCount here - loop() reads it first
            } else if (gap > PULSE_RESET_US) {
                pulseCount = 0;  // Too few pulses - noise, discard
            }
        }
        lastRising = now;

    } else {
        // Falling edge: measure the HIGH pulse that just ended
        if (lastRising > 0 && !packetReady) {
            uint32_t width = now - lastRising;
            // Sanity check - ignore absurdly long or short pulses
            if (width > 20 && width < 2000) {
                if (pulseCount < MAX_PULSES) {
                    pulseWidths[pulseCount++] = width;
                }
            }
        }
        lastFalling = now;
    }
}

// ── Decode & match ────────────────────────────────────────────
bool decodeAndMatch() {
    // Snapshot the volatile buffer
    Serial.printf("Running decodeAndMatch");
    Serial.printf("pulseCount: %d", pulseCount);
    Serial.println();
    uint16_t count = pulseCount;
    uint32_t pulses[MAX_PULSES];
    for (uint16_t i = 0; i < count; i++) {
        pulses[i] = pulseWidths[i];
    }
    pulseCount = 0;  // Ready for next packet

    if (count < 20) return false;  // Too short to be our signal

    // Decode each pulse width into a bit (skip sync pulses)
    String bits = "";
    for (uint16_t i = 0; i < count; i++) {
        uint32_t w = pulses[i];

        if (abs((int32_t)w - PULSE_SYNC_NOM) < PULSE_TOLERANCE) {
            // Sync/delimiter pulse - skip
            continue;
        } else if (abs((int32_t)w - PULSE_SHORT_NOM) < PULSE_TOLERANCE) {
            bits += '0';
        } else if (abs((int32_t)w - PULSE_LONG_NOM) < PULSE_TOLERANCE) {
            bits += '1';
        }
        // Else: unrecognised width - skip (noise)
    }

    uint16_t numBits = bits.length();
    if (numBits < DOORBELL_CODE_MIN_BITS) {
        Serial.printf("[decode] Too few bits: %d\n", numBits);
        return false;
    }

    // Pack bits into hex string (4 bits per nibble)
    String hexStr = "";
    for (uint16_t i = 0; i + 3 < numBits; i += 4) {
        uint8_t nibble = 0;
        for (uint8_t j = 0; j < 4; j++) {
            nibble = (nibble << 1) | (bits[i + j] == '1' ? 1 : 0);
        }
        char buf[2];
        sprintf(buf, "%x", nibble);
        hexStr += buf;
    }

    Serial.printf("[decode] %d bits -> %s\n", numBits, hexStr.c_str());

    bool match = hexStr.startsWith(DOORBELL_CODE_PREFIX);
    if (match) {
        Serial.println("[decode] *** CODE MATCHED ***");
    }
    return match;
}

// ── Publish ───────────────────────────────────────────────────
void publishDoorbell() {
    Serial.println("[doorbell] Publishing MQTT");
    mqtt.publish(MQTT_TOPIC_DOORBELL, "ON", false);
    delay(200);
    mqtt.publish(MQTT_TOPIC_DOORBELL, "OFF", false);
}