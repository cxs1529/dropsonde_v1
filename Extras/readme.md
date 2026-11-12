SD\_CARD\_ROOT/

├── DATA/                       # Autonomously managed raw telemetry destination

│   ├── BMEDATA\_YYMMDDhhmmss.dat # Timestamped CSV sensor data (Temp, RH, Press, Volts)

│   └── GPSDATA\_YYMMDDhhmmss.dat # Timestamped two-line format NMEA positioning data

│

├── .META/                      # Autonomously managed hidden system cache

│   ├── GPSDATA\_...\_meta.txt    # Fast-loading pointer files for Web UI file sizes/lines

│   └── GPSDATA\_...\_note.txt    # User-submitted deployment notes from the dashboard

│

└── .WEB/                        # Offline Web Assets (Must be loaded manually by user)

    ├── 27n-50c005t.geojson     # Bathymetry depth contours for map rendering

    ├── leaflet.css             # Leaflet mapping engine stylesheet

    ├── leaflet.js              # Leaflet mapping engine script

    ├── plan.html               # Official NOAA dropsonde cruise plan document

    ├── plot.html               # Offline zero-dependency HTML5 sensor charting viewer

    ├── targets.csv             # Custom map deployment target coordinates (uploadable via UI)

    ├── uPlot.iife.min.js       # Ultra-lightweight plotting engine for plot.html

    ├── uPlot.min.css           # uPlot engine stylesheet

    └── viewer.html             # Leaflet-based GPS track visualization viewer

