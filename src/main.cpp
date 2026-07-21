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
unsigned long lastHeartbeatMs = 0;
const unsigned long HEARTBEAT_INTERVAL_MS = 10 * 60 * 1000; // 10 minutes
unsigned long lastMqttRetryMs = 0;
unsigned long lastWifiRetryMs = 0;
const unsigned long WIFI_RETRY_INTERVAL_MS = 15000; // 15 seconds

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
    // [a] WiFi Resilience: Ensure WiFi is connected before MQTT
    if (WiFi.status() == WL_CONNECTED) {
        connectMqtt();
    } else {
        connectWifi();
    }
    mqtt.loop();

    // [b] Heartbeat: Every 10 minutes validate connection and send status
    unsigned long now = millis();
    if (now - lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS) {
        lastHeartbeatMs = now;
        if (mqtt.connected()) {
            mqtt.publish(MQTT_TOPIC_STATUS, "heartbeat", true);
        }
    }

    // Check for "Idle" timeout: If no pulse has arrived for PULSE_RESET_US, 
    // the packet is finished. This fixes the bit-count inconsistency.
    if (pulseCount >= 20 && !packetReady) {
        if (micros() - lastFalling > PULSE_RESET_US) {
            packetReady = true;
        }
    }

    if (packetReady) {
        packetReady = false;

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
    if (WiFi.status() == WL_CONNECTED) return;
    
    unsigned long now = millis();
    // Only attempt to (re)connect every 15 seconds to avoid spamming the WiFi stack
    if (lastWifiRetryMs == 0 || now - lastWifiRetryMs > WIFI_RETRY_INTERVAL_MS) {
        lastWifiRetryMs = now;
        Serial.printf("[wifi] Connecting to %s...\n", WIFI_SSID);
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
    }
}

// ── MQTT ──────────────────────────────────────────────────────
void connectMqtt() {
    if (mqtt.connected()) return;

    unsigned long now = millis();
    if (now - lastMqttRetryMs > 5000) {
        lastMqttRetryMs = now;
        Serial.print("[mqtt] Connecting...");
        if (mqtt.connect(MQTT_CLIENT_ID,
                         MQTT_USER, MQTT_PASSWORD,
                         MQTT_TOPIC_STATUS, 0, true, "offline")) {
            Serial.println(" connected");
            mqtt.publish(MQTT_TOPIC_STATUS, "online", true);
        } else {
            Serial.printf(" failed (rc=%d)\n", mqtt.state());
        }
    }
}

// ── CC1101 init ───────────────────────────────────────────────
void initCC1101() {
    // D1 mini pins
    //ELECHOUSE_cc1101.setSpiPin(14, 12, 13, CC1101_CSN_PIN);
    // ESP32-C3 Supermini pins
    SPI.begin();
    ELECHOUSE_cc1101.setSpiPin(CC1101_SCK_PIN, CC1101_MISO_PIN, CC1101_MOSI_PIN, CC1101_CSN_PIN);
    // Carry on
    ELECHOUSE_cc1101.Init();
    ELECHOUSE_cc1101.setModulation(2);      // Explicitly set ASK/OOK modulation
    ELECHOUSE_cc1101.setMHZ(CC1101_FREQUENCY);
    // Narrower bandwidth (100kHz) reduces noise floor significantly.
    ELECHOUSE_cc1101.setRxBW(200);
    ELECHOUSE_cc1101.SetRx();

    Serial.printf("[cc1101] Initialised at %.3f MHz\n", CC1101_FREQUENCY);
}

// ── Interrupt ─────────────────────────────────────────────────
// Captures HIGH pulse widths. Triggers packet-ready when a
// reset gap (long LOW) is detected on the next rising edge.
void IRAM_ATTR onGDO0Interrupt() {
    uint32_t now = micros();

    if (digitalRead(CC1101_GDO0_PIN) == HIGH) {
        // Rising edge - check gap since last falling for packet reset
        if (lastFalling > 0) {
            uint32_t gap = now - lastFalling;
            if (gap > PULSE_RESET_US && pulseCount > 10) {
                packetReady = true;
            } else if (gap > PULSE_RESET_US) {
                pulseCount = 0;  // Too few pulses - noise
            }
        }
        lastRising = now;

    } else {
        // Falling edge - measure the HIGH pulse that just ended
        if (lastRising > 0 && !packetReady) {
            uint32_t width = now - lastRising;
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
    // Atomic snapshot
    noInterrupts();
    uint16_t count = pulseCount;
    uint32_t pulses[MAX_PULSES];
    for (uint16_t i = 0; i < count; i++) {
        pulses[i] = pulseWidths[i];
    }
    pulseCount = 0;
    interrupts();

    Serial.printf("[decode] Processing %d pulses\n", count);
    if (count < 20) return false;

    String bits = "";
    for (uint16_t i = 0; i < count; i++) {
        uint32_t w = pulses[i];

        if (abs((int32_t)w - PULSE_SYNC_NOM) < PULSE_SYNC_TOL) {
            // Sync/delimiter pulse - skip entirely
            continue;
        } else if (abs((int32_t)w - PULSE_SHORT_NOM) < PULSE_SHORT_TOL) {
            bits += '1';   // was '0' - SHORT pulse = 1 in this protocol
        } else if (abs((int32_t)w - PULSE_LONG_NOM) < PULSE_LONG_TOL) {
            bits += '0';   // was '1' - LONG pulse = 0 in this protocol
        }
        // Anything outside all windows = unrecognised noise, skip silently
    }

    uint16_t numBits = bits.length();

    // Pack bits into hex string
    String hexStr = "";
    for (uint16_t i = 0; i < numBits; i += 4) {
        uint8_t nibble = 0;
        for (uint8_t j = 0; j < 4; j++) {
            if (i + j < numBits) {
                nibble = (nibble << 1) | (bits[i + j] == '1' ? 1 : 0);
            } else {
                nibble <<= 1;  // Pad trailing nibble
            }
        }
        char buf[2];
        sprintf(buf, "%x", nibble);
        hexStr += buf;
    }

    Serial.printf("[decode] Bits: %s\n", bits.c_str());
    Serial.printf("[decode] %d bits -> %s\n", numBits, hexStr.c_str());

    if (numBits < DOORBELL_CODE_MIN_BITS) {
        Serial.printf("[decode] Too few bits: %d\n", numBits);
        return false;
    }

    // ── Noise pre-filter ──────────────────────────────────────
    // Genuine doorbell code is ~70% ones. Pure noise and other
    // 433MHz sensors tend to produce >85% or <15% ones.
    // Reject anything outside that window before running the
    // expensive Hamming search.
    int oneCount = 0;
    for (int i = 0; i < numBits; i++) {
        if (bits[i] == '1') oneCount++;
    }
    int onePct = oneCount * 100 / numBits;
    if (onePct > 85 || onePct < 15) {
        Serial.printf("[decode] Rejected - %d%% ones (noise/other device)\n", onePct);
        return false;
    }

    // Build doubled known pattern to handle wrap-around captures
    // (end of repetition N into start of repetition N+1)
    String known = String(DOORBELL_BIT_PATTERN) + String(DOORBELL_BIT_PATTERN);

    // Slide a window across the decoded bits, comparing against
    // every position in the known doubled pattern.
    // Accept if Hamming distance is within tolerance.
    bool match = false;
    int windowSize = min((int)numBits, MATCH_WINDOW_BITS);

    for (int k = 0; k <= (int)known.length() - windowSize; k++) {
        int errors = 0;
        for (int j = 0; j < windowSize; j++) {
            if (bits[j] != known[k + j]) errors++;
            if (errors > MATCH_MAX_ERRORS) break;  // Early exit
        }
        if (errors <= MATCH_MAX_ERRORS) {
            Serial.printf("[decode] Matched at offset %d with %d errors\n",
                          k, errors);
            match = true;
            break;
        }
    }

    String payload = "bits:" + bits + "|hex:" + hexStr;
    if (match) {
        Serial.println("[decode] *** CODE MATCHED ***");
        mqtt.publish(MQTT_TOPIC_MATCH, payload.c_str());
    } else {
        mqtt.publish(MQTT_TOPIC_OTHER, payload.c_str());
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