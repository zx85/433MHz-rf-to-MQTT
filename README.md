# 433MHz-rf-to-MQTT

A high-precision RF-to-MQTT bridge designed to sniff 433MHz OOK/ASK signals (specifically doorbells) and publish them to a Home Assistant or any MQTT-compatible automation system.

## 1. Parts Required

*   **Microcontroller:** ESP8266 (D1 Mini or D1 Mini Pro recommended).
*   **RF Module:** CC1101 Transceiver (better sensitivity and frequency control than generic SRX882 receivers) - recommended: E07-M1101D
*   **Antenna:** 433MHz spring antenna or a 17.3cm copper wire (or in-built antenna in the case of E07-M1101D)
*   **SDR (Optional but recommended):** RTL-SDR USB dongle for signal analysis.

### Wiring (CC1101 to D1 Mini)
| CC1101 Pin | ESP8266 Pin | Description |
|------------|-------------|-------------|
| VCC        | 3.3V        | Power (3.3V ONLY) |
| GND        | GND         | Ground |
| SCK        | D5 (GPIO14) | SPI Clock |
| MISO       | D6 (GPIO12) | SPI MISO |
| MOSI       | D7 (GPIO13) | SPI MOSI |
| CSN        | D8 (GPIO15) | Configurable in config.h |
| GDO0       | D4 (GPIO2)  | Configurable in config.h |

Pinout here:
[done.land/components/data/datatransmission/wireless/shortrangedevice/fm/fsk/transceiver/e07-m1101d/](https://done.land/components/data/datatransmission/wireless/shortrangedevice/fm/fsk/transceiver/e07-m1101d/)

## 2. Build Platform

This project is built using **PlatformIO**. 

1.  Install [VS Code](https://code.visualstudio.com/).
2.  Install the **PlatformIO IDE** extension.
3.  Open this folder.
4.  The necessary libraries (`SmartRC-CC1101-Driver-Lib`, `PubSubClient`) will be automatically downloaded based on the `platformio.ini` configuration.

## 3. How the Software Works

The firmware uses a hardware interrupt-driven approach to ensure no pulses are missed:

1.  **Pulse Capture:** An interrupt on the CC1101 `GDO0` pin triggers on every signal change. It measures the duration of `HIGH` pulses using `micros()`.
2.  **Packet Detection:** When the software detects a period of silence (defined by `PULSE_RESET_US`), it marks the packet as complete.
3.  **Decoding:** The captured pulses are categorized as "Short" or "Long" based on a midpoint threshold calculated from your `config.h` values.
4.  **Fuzzy Matching:** Instead of strict bit-length matching, it searches the resulting bitstream for your `DOORBELL_BIT_PATTERN`. This makes it resilient to preamble noise.
5.  **MQTT Bridge:** 
    *   **Status:** Publishes "online"/"offline" with a Last Will and Testament.
    *   **Heartbeat:** Publishes every 10 minutes to verify the bridge is alive.
    *   **Doorbell:** Publishes `ON` then `OFF` when a match is found.
    *   **Discovery:** Publishes unmatched signals to an `OTHER` topic for easy analysis of new remotes.

## 4. Signal Discovery (using rtl_433)

Before configuring the firmware, you need to know what your doorbell is sending. 

1.  Plug in your RTL-SDR dongle.
2.  Run the following command:
    ```bash
    rtl_433 -f 433.92M -A
    ```
3.  Press your doorbell button. Look for "Pulse Data" output. You should get something like:

```
Detected OOK package
short_width: 500, long_width: 1500, reset_limit: 9000, sync_width: 0
Use a flex decoder with -X 'n=doorbell,m=OOK_PWM,s=500,l=1500,r=9000,...'
```

4.  Note the **short_width** and **long_width** durations (usually in µs) and the bit pattern (e.g., `37ffa6...`).


## 5. Testing and Calibration

If you don't have an SDR, or want to verify what the ESP8266 sees:

1.  Flash the firmware with default values.
2.  Open the Serial Monitor (115200 baud).
3.  Press your doorbell. The ESP8266 will print:
    `[decode] Bits: 101010100110...`
    `[decode] 24 bits -> aab6...`
4.  Copy the long string of `1`s and `0`s that appears consistently when you press the button. This is your `DOORBELL_BIT_PATTERN`.
5.  If the device is "seeing" pulses but not matching, check the `OTHER` MQTT topic. The firmware automatically sends unknown signals there for "lazy" discovery.

## 6. Configuration (`config.h`)

Create a `src/config.h` file (referencing the constants in `main.cpp`). 

### WiFi & MQTT
*   `WIFI_SSID` / `WIFI_PASSWORD`: Your credentials.
*   `MQTT_BROKER`: IP address of your broker (e.g., Home Assistant/Mosquitto).
*   `MQTT_TOPIC_DOORBELL`: The topic your automation listens to (e.g., `home/doorbell`).

### RF Tuning
*   `CC1101_FREQUENCY`: Usually `433.92`.
*   `PULSE_SHORT_NOM`: The duration of a 'short' pulse from your `rtl_433` analysis.
*   `PULSE_LONG_NOM`: The duration of a 'long' pulse.
*   `PULSE_RESET_US`: The amount of silence (low signal) required to consider a packet "finished" (usually `5000` to `10000`).

### Matching
*   `DOORBELL_BIT_PATTERN`: The constant string of bits identified in Step 5.
*   `DOORBELL_LOCKOUT_MS`: Prevents multiple MQTT triggers from a single long button press (recommended `2000`).

## Troubleshooting

*   **No signals detected:** Ensure the CC1101 `GDO0` pin is connected to the correct GPIO and that the `CSN` pin matches your `config.h`.
*   **Inconsistent bits:** RF noise can be high. The software uses `ELECHOUSE_cc1101.setRxBW(100)` to narrow the filter. If your remote is low quality, you might need to increase this to `200` or `300`.
*   **WiFi Disconnects:** The logic includes a 15-second retry timer to prevent the ESP8266 from hanging while trying to reconnect to a weak AP.
