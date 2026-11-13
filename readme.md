# NOAA/AOML Dropsonde GPS Logger

ESP32-C3 based telemetry acquisition system designed for measuring depth average currents in the Gulfstream. This system captures high-frequency GPS data, logs it securely to an SD card, and hosts an interactive Captive Portal web dashboard for live monitoring, file extraction, and geographic visualization.

## Overview
The Dropsonde Logger acts as a field instrument. It features software-debounced switches (low pass filter for state changes) for logging and Wi-Fi control, ensuring reliability in marine environments. The system includes an advanced Leaflet-based map viewer capable of plotting recorded GPS tracks over bathymetric GeoJSON contours, displaying live deployment targets, and providing access to cruise plan instructions.

## Key Features
*   **1Hz GPS Logging:** Synchronizes internal RTC via NEO-6M `$GPRMC` sentences. Falls back to an uptime-based 1970 timestamp if satellite lock is lost, ensuring continuous chronological logging.
*   **Captive Portal Dashboard:** Hosts a local Wi-Fi Access Point (`DROPSONDE_WIFI`) and a DNS server. Any connected device is automatically routed to the management dashboard.
*   **File Architecture:** Uses a split-file system (`.txt` for data, `_meta.txt` for dynamic metrics, `_note.txt` for user annotations) to eliminate Read-Modify-Write bottlenecks and prevent SD card corruption.
*   **Software "Low-Pass" Filters:** Implements 5-second (Logging) and 10-second (Wi-Fi) FreeRTOS software debouncing on physical switches to ignore mechanical bounce and transient electrical noise.
*   **Interactive Map Viewer:** Plots saved `.txt` data telemetry logs onto a digital map, highlighting drift speed, heading, and GPS fix quality over local depth contours.
*   **Target Updates:** Users can upload a `targets.csv` file directly through the dashboard to update reference deployment waypoints instantly.

## Hardware Configuration
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
| **I2C**          | GPIO 5 (SDA), 10 (SCL) | Connected to BME280/BMP280 sensor |

## Software Dependencies & Build Instructions
This project is built using the **Espressif IoT Development Framework (ESP-IDF v5.0+)**.
The project uses the following components and libraries:
*   `esp_driver_uart`, `esp_driver_gpio`, `esp_driver_spi`, `esp_driver_i2c`
*   `fatfs`, `sdmmc`
*   `esp_wifi`, `esp_http_server`, `dns_server`
*   `nvs_flash`
*   `minmea` (NMEA parsing)
*   `bmp280` (Sensor driver)
*   `ema_filter` (Exponential Moving Average filter for sensor noise)

Build and flash using standard ESP-IDF commands:
```bash
idf.py build flash monitor
```

## Operations Guide
### 1. Wi-Fi & Dashboard Access
Flip the Wi-Fi switch to the **ON** position. Wait 10 seconds for the hardware debounce to validate.
* Connect your smartphone or laptop to the Wi-Fi network: `DROPSONDE_WIFI` (Password: `dropsonde`).
* A captive portal prompt should appear. If not, open a browser and navigate to `http://aomldropsonde.com` (or `http://192.168.4.1`).

### 2. Logging Data
Flip the Log switch to the **ON** position. Wait 5 seconds.
* The system will lock the RTC to the GPS and create a chronologically named file (e.g., `GPSDATA_261102143000.txt`).
* Live telemetry (Lat, Lon, PDOP, Fix Quality) and sensor data (Temp, RH, Press, Vin) will update on the dashboard every 500ms.
* Pausing the logger for more than 60 seconds will automatically trigger a new deployment file upon resuming.
* The system double-buffers data, simultaneously writing `GPSDATA_*.txt` and `BMEDATA_*.txt` files to track position and environmental metrics independently.

### 3. Adding Deployment Notes
From the dashboard, click **"+ Start New Deployment File"**. You will be prompted to enter an optional note (up to 255 characters). If you stop typing for 60 seconds, the system will auto-save and begin logging.

## SD Card File Structure
The SD Card (formatted as FAT32) uses the following directory structure:
*   `/DATA/`: Contains raw user-facing telemetry logs (`.txt`). Examples:
    *   `GPSDATA_YYMMDDhhmmss.txt`: Two-line NMEA positioning data.
    *   `BMEDATA_YYMMDDhhmmss.txt`: CSV format environmental data (Time, Temp, RH, Press, Volts).
*   `/.META/`: Hidden system folder containing file metrics and user notes to accelerate dashboard load times. Examples:
    *   `GPSDATA_..._meta.txt`: Fast-loading pointer files containing file sizes, lines, and min/max/avg sensor statistics.
    *   `GPSDATA_..._note.txt`: User-submitted deployment notes from the dashboard.
*   `/.WEB/`: Offline Web Assets (Must be loaded manually by user).
    *   `27n-50c005t.geojson`: Bathymetry depth contours for map rendering.
    *   `leaflet.css` & `leaflet.js`: Leaflet mapping engine.
    *   `plan.html`: Official NOAA dropsonde cruise plan document.
    *   `plot.html`: Offline HTML5 sensor charting viewer with derivative plots (dP/dt, dT/dt).
    *   `targets.csv`: Custom map deployment target coordinates (uploadable via UI).
    *   `uPlot.iife.min.js` & `uPlot.min.css`: Lightweight plotting engine.
    *   `viewer.html`: Leaflet-based GPS track visualization viewer.

## Extras & Testing Tools
Included in the `Extras` directory are tools for testing and visualization:
*   **`gpsdata_generator.py`**: A Python script that simulates realistic Gulf Stream drift and internal housing thermodynamics. It generates synthetic `.txt` and `_meta.txt` telemetry logs with simulated ocean currents, turbulence, pressure, and temperature curves matching a typical physical deployment. Creates unique timestamped output directories (`sim_YYYYMMDD_HHMMSS`) that can be directly dragged onto the SD card for testing.
*   **`viewer.html`**: A standalone Leaflet map viewer. Capable of reading the generated NMEA `.txt` files, calculating distance/drift speed, and rendering depth contours.
*   **`targets.csv`**: An example target waypoint file. Update this file and push it via the Dashboard's "Upload" button to change visible map targets in real-time.
*   **`plan.html`**: A formatted HTML version of the NOAA Florida Current Dropsonde Cruise Plan, accessible directly from the main dashboard for field reference.

## Images
### Main Dashboard
<img width="959" height="413" alt="dashboard_1" src="https://github.com/user-attachments/assets/eafaaaec-69a1-4121-84b1-2facc4efaae4" />
<img width="959" height="413" alt="dashboard_2" src="https://github.com/user-attachments/assets/feb8b5d4-31b8-4016-b24b-a93ab6e8210a" />

### Sensor Plots
<img width="959" height="410" alt="plot_1" src="https://github.com/user-attachments/assets/8ee7039d-e608-4664-b96c-164a013b3e92" />
<img width="959" height="411" alt="plot_2" src="https://github.com/user-attachments/assets/93380173-99e1-46cd-8303-700f54a51312" />
<img width="958" height="332" alt="plot_3" src="https://github.com/user-attachments/assets/96cd39d8-e11c-4a27-bdfd-945dbd208773" />

### New Deployment
<img width="959" height="334" alt="new-deployment-notes" src="https://github.com/user-attachments/assets/6b441f4d-f643-4b3b-ab17-5c1759081c30" />

### Bathymetry map w/ stations and GPS data
<img width="956" height="413" alt="map-viewer" src="https://github.com/user-attachments/assets/d4f72be9-10f4-4701-a8dd-d8b2d0d00697" />

### Surface velocity estimates
<img width="580" height="209" alt="gps-markers-drift-speed" src="https://github.com/user-attachments/assets/9c6d87e2-1ec0-4c50-9740-ed4dd44994b5" />
