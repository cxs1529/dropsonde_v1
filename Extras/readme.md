SD\_CARD\_ROOT/

├── DATA/                       # Autonomously managed raw telemetry destination

│   ├── BMEDATA\_YYMMDDhhmmss.dat # Timestamped CSV sensor data (Temp, RH, Press, Volts)

│   └── GPSDATA\_YYMMDDhhmmss.dat # Timestamped two-line format NMEA positioning data

│

├── .META/                      # Autonomously managed hidden system cache

│   ├── GPSDATA\_...\_meta.txt    # Fast-loading pointer files for Web UI file sizes/lines

│   └── GPSDATA\_...\_note.txt    # User-submitted deployment notes from the dashboard

│

└── WEB/                        # Offline Web Assets (Must be loaded manually by user)

&#x20;   ├── 27n-50c005t.geojson     # Bathymetry depth contours for map rendering

&#x20;   ├── leaflet.css             # Leaflet mapping engine stylesheet

&#x20;   ├── leaflet.js              # Leaflet mapping engine script

&#x20;   ├── plan.html               # Official NOAA dropsonde cruise plan document

&#x20;   ├── plot.html               # Offline zero-dependency HTML5 sensor charting viewer

&#x20;   ├── targets.csv             # Custom map deployment target coordinates (uploadable via UI)

&#x20;   ├── uPlot.iife.min.js       # Ultra-lightweight plotting engine for plot.html

&#x20;   ├── uPlot.min.css           # uPlot engine stylesheet

&#x20;   └── viewer.html             # Leaflet-based GPS track visualization viewer

