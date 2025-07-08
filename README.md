# `BLEacon`

---

James Cranley, July 2025

---

A lightweight ESP32-based RSSI logger and display for BLE beacons.

This project uses the Waveshare ESP32-S3-LCD-1.47 microcontroller with integrated LCD to monitor signal strength (RSSI) from a specific BLE beacon—typically a [Shelly BLU Button1](https://www.shelly.com/blogs/documentation/shellyblu-button1).  
It displays live signal strength as text and signal bars, syncs its clock over NTP, and logs data to SD card.

![BLEacon](/images/BLEacon.png)

---

## Hardware

- 📟 [**Waveshare ESP32-S3-LCD-1.47**](https://www.waveshare.com/wiki/ESP32-S3-LCD-1.47)  
  (ESP32-S3 microcontroller, 1.47" ST7789 LCD, HSPI interface)

- 🔘 [**Shelly BLU Button1**](https://www.shelly.com/blogs/documentation/shellyblu-button1)  
  (Bluetooth Low Energy beacon with long battery life)

---

## Preparing the Beacon

Ensure your Shelly BLU Button1 is in **Beacon Mode**:

1. Download the **Shelly Smart Control** app and pair your device.
2. In the app, tap on the Shelly Button1 and enable **Beacon Mode** under advanced settings.
3. Go to **Device Info** and **note the MAC address**—you'll need this for the sketch.

---

## Installation

### 1. Install Arduino IDE

- Install via Homebrew (for macOS):

  ```bash
  brew install --cask arduino-ide
  ```

- On Apple Silicon Macs, you **may need Rosetta2**:

  ```bash
  softwareupdate --install-rosetta
  ```

---

### 2. Connect and Select the Board

- Plug the **ESP32-S3-LCD-1.47** into USB.
- Open Arduino IDE.
- In the toolbar, select:
  - **Board**: “WAVESHARE ESP32-S3 LCD 1.47”
  - **Port**: Your device’s USB connection

![Connection](/images/arduino-connection.png)

---

### 3. Install ESP32 Board Support

- Open the **Boards Manager** (left sidebar).
- Search for **`esp32` by Espressif Systems**.
- Install the latest version.

![Board](/images/arduino-board.png)

---

### 4. Install Required Libraries

- Open the **Library Manager** (left sidebar).
- Install the following:
  - `Adafruit GFX Library`
  - `Adafruit ST7789 Library`

![Board](/images/arduino-library.png)

---

### 5. Load the Sketch

- Open the file: `/src/multi_MAC_with_logging.ino`
- Fill in:
  - The **MAC addresses** of your Shelly tags
  - Your **Wi-Fi credentials** for NTP time sync
- Upload to the device: Click the **Upload** button in the IDE.

---

## Output

The device will:

- Display current **UTC time**, **MAC**, **SSID**, and **RSSI**.
- It will log data to the on-baord micro SD card
---

## References

[NHS Digital](https://digital.nhs.uk/services/networks-and-connectivity-transformation-frontline-capabilities/connectivity-hub/advice-and-guidance/rtls-guidance)

[Marini2020](http://dx.doi.org/10.2196/19874)
