# NOAA Dropsonde Extras & Utilities

This directory contains standalone tools and resources for testing, visualizing, and simulating the NOAA/AOML Dropsonde Logger data.

## `gpsdata_generator.py` (Telemetry Simulator)

This Python script is a comprehensive deployment simulator. Instead of waiting hours to collect physical drift data, you can use this script to instantly generate complete, realistic telemetry datasets.

**What it simulates:**

1. **Gulf Stream Surface Drift:** Uses mathematical models (Gaussian perturbations) to simulate realistic surface currents (u_east, v_north) based on climatological ADCP profiles across the 27N Florida Current transect.
2. **Thermodynamics (Descent/Ascent):** Models the temperature and pressure changes inside the sealed aluminum dropsonde housing as it descends into colder deep water and ascends back to the surface. It uses exponential decay/warming equations and applies Gay-Lussac's Law for internal pressure tracking.
3. **GPS Signal Loss:** Accurately mimics the loss of the GPS 3D fix during the underwater drop and rise phases, transitioning to a zero-coordinate state while continuing to track internal time.
4. **Sensor Noise:** Applies realistic Gaussian jitter to the generated temperature, humidity, and pressure readings.

**How to use:**

1. Run the script from your terminal: `python3 gpsdata_generator.py`
2. The script will automatically create a unique root directory for the simulation run based on the current timestamp (e.g., `sim_20261113_164100`).
3. Inside this directory, it perfectly reconstructs the ESP32's internal folder hierarchy, creating `.META/` and `DATA/` subdirectories.
4. It populates these folders with fully formatted `GPSDATA`, `BMEDATA`, `_meta`, and `_note` files.
5. To test the web dashboard, simply drag and drop the `.META/` and `DATA/` folders from the simulation directory directly onto the root of your ESP32's micro-SD card.

## Dashboard Web Assets (`.WEB` directory)

The physical SD card requires a `/.WEB/` directory containing offline HTML/JS/CSS assets to render the interactive maps and charts when connected via the Captive Portal.

*If your SD card is blank, copy the contents of the provided example `.WEB` folder to the root of your card.*

* **`viewer.html`**: A standalone Leaflet map viewer. Capable of reading the generated NMEA `GPSDATA_*.txt` files from the SD card to render the drift track over bathymetry contours.
* **`plot.html`**: A high-performance HTML5 charting tool using the `uPlot` library. It reads the `BMEDATA_*.txt` CSV logs and visualizes Temperature, Pressure, and Humidity. It also includes algorithms to calculate and plot the derivatives (dP/dt and dT/dt) to help identify phase transitions (Surface -> Descent -> Bottom -> Ascent).
* **`targets.csv`**: A CSV file containing the planned coordinate waypoints for the deployment cruise. The dashboard allows users to upload new versions of this file in the field.
* **`plan.html`**: A digital copy of the operational cruise plan and instructions.