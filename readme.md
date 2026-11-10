# NOAA/AOML Dropsonde GPS Logger

ESP32-C3 based telemetry acquisition system designed for measuring depth average currents in the Gulfstream. This system captures high-frequency GPS data, logs it securely to an SD card, and hosts an interactive Captive Portal web dashboard for live monitoring, file extraction, and geographic visualization.

## 🌊 Overview

The Dropsonde Logger acts is a field instrument. It features software-debounced switches (low pass filter for state changes) for logging and Wi-Fi control, ensuring reliability inmarine environments. The system includes an advanced Leaflet-based map viewer capable of plotting recorded GPS tracks over bathymetric GeoJSON contours, displaying live deployment targets, and providing access to cruise plan instructions.

## ✨ Key Features

*   **1Hz GPS Logging:** Synchronizes internal RTC via NEO-6M `$GPRMC` sentences. Falls back to an uptime-based 1970 timestamp if satellite lock is lost, ensuring continuous chronological logging.
*   **Captive Portal Dashboard:** Hosts a local Wi-Fi Access Point (`DROPSONDE_WIFI`) and a DNS server. Any connected device is automatically routed to the management dashboard.
*   **File Architecture:** Uses a split-file system (`.txt` for data, `_meta.txt` for dynamic metrics, `_note.txt` for user annotations) to eliminate Read-Modify-Write bottlenecks and prevent SD card corruption.
*   **Software "Low-Pass" Filters:** Implements 5-second (Logging) and 10-second (Wi-Fi) FreeRTOS software debouncing on physical switches to ignore mechanical bounce and transient electrical noise.
*   **Interactive Map Viewer:** Plots saved `.txt` data telemetry logs onto a digital map, highlighting drift speed, heading, and GPS fix quality over local depth contours.
*   **Target Updates:** Users can upload a `targets.csv` file directly through the dashboard to update reference deployment waypoints instantly.

## 🔌 Hardware Configuration

This project is configured for the **SparkFun Pro Micro ESP32-C3**.

| Component | ESP32-C3 Pin | Note |
| :---      | :---         | :--- |
| **Log Switch**   | GPIO 8 | Active LOW (Pull to GND to Log) |
| **Wi-Fi Switch** | GPIO 2 | Active LOW (Pull to GND for AP) |
| **GPS RX**       | GPIO 6 | Connect to GPS Module TX |
| **GPS TX**       | GPIO 7 | Connect to GPS Module RX |
| **SD CLK**       | GPIO 0 | SPI Clock |
| **SD MISO**      | GPIO 1 | SPI MISO / DO |
| **SD MOSI**      | GPIO 3 | SPI MOSI / DI |
| **SD CS**        | GPIO 4 | SPI Chip Select |
| **I2C**          | GPIO 5 (SDA), 10 (SCL) | Reserved for future BME280 integration |

## 🛠️ Software Dependencies & Build Instructions

This project is built using the **Espressif IoT Development Framework (ESP-IDF v5.0+)**.


## 🚀 Operations Guide

### 1. Wi-Fi & Dashboard Access
Flip the Wi-Fi switch to the **ON** position. Wait 10 seconds for the hardware debounce to validate. 
* Connect your smartphone or laptop to the Wi-Fi network: `DROPSONDE_WIFI` (Password: `dropsonde`).
* A captive portal prompt should appear. If not, open a browser and navigate to `http://aomldropsonde.com` (or `http://192.168.4.1`).

### 2. Logging Data
Flip the Log switch to the **ON** position. Wait 5 seconds.
* The system will lock the RTC to the GPS and create a chronologically named file (e.g., `GPSDATA_261102143000.txt`).
* Live telemetry (Lat, Lon, PDOP, Fix Quality) will update on the dashboard every 500ms.
* Pausing the logger for more than 60 seconds will automatically trigger a new deployment file upon resuming.

### 3. Adding Deployment Notes
From the dashboard, click **"+ Start New Deployment File"**. You will be prompted to enter an optional note (up to 255 characters). If you stop typing for 60 seconds, the system will auto-save and begin logging.

## 📂 SD Card File Structure

The SD Card (formatted as FAT32) uses the following directory structure:

*   `/DATA/`: Contains raw user-facing telemetry logs (`.txt`).
*   `/.META/`: Hidden system folder containing file metrics and user notes to accelerate dashboard load times.
*   `/.MAP/`: Contains offline map assets (`leaflet.js`, `leaflet.css`), GeoJSON bathymetry contours, `targets.csv`, map website `viewer.html` and `plan.html`.

## 🧰 Extras & Testing Tools

Included in the `Extras` directory are tools for testing and visualization:

*   **`gpsdata_generator.py`**: A Python script that simulates realistic Gulf Stream drift. It generates synthetic `.txt` and `_meta.txt` telemetry logs with simulated ocean currents, turbulence, and heading data. Great for testing the map viewer offline.
*   **`viewer.html`**: A standalone Leaflet map viewer. Capable of reading the generated NMEA `.txt` files, calculating distance/drift speed, and rendering depth contours. 
*   **`targets.csv`**: An example target waypoint file. Update this file and push it via the Dashboard's "Upload" button to change visible map targets in real-time.
*   **`plan.html`**: A formatted HTML version of the NOAA Florida Current Dropsonde Cruise Plan, accessible directly from the main dashboard for field reference.