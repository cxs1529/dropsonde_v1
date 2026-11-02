import datetime
import math
import random

# --- DRIFT SIMULATION PARAMETERS ---
# Starting deployment point (e.g. Florida shelf break off Palm Beach / Jupiter)
start_lat = 27.000000
start_lon = -79.920000

# Total run time and record count
duration_hours = 1.0
num_fixes = 500

# Time setup (start at current system time)
start_time = datetime.datetime.now()
total_seconds = duration_hours * 3600.0
dt = total_seconds / (num_fixes - 1) if num_fixes > 1 else 1.0

def get_gulf_stream_velocity(lat, lon):
    """
    Returns realistic surface currents (u_east, v_north) in meters/second
    based on climatological ADCP profiles across the 27N Florida Current transect.
    """
    # Core peak longitude ~79.72W
    lon_core = -79.72
    
    # Asymmetric bell curve for northward jet velocity (V_north)
    if lon < lon_core:
        # Western side: sharp horizontal shear
        sigma = 0.16
    else:
        # Eastern side: broader decay toward Little Bahama Bank
        sigma = 0.28
        
    v_peak = 2.4  # Core peak speed: ~2.4 m/s (~4.7 knots)
    v_bg = 0.3    # Background northward drift: ~0.3 m/s (~0.6 knots)
    
    v_north = v_bg + (v_peak - v_bg) * math.exp(-((lon - lon_core) ** 2) / (2 * (sigma ** 2)))
    
    # Minor cross-stream meandering (U_east) with slight convergence toward the core
    # Plus turbulence / sub-mesoscale eddy noise
    u_mean = (lon_core - lon) * 0.4  # weak steering toward jet core
    
    # Add turbulent perturbation noise (~0.05 - 0.1 m/s)
    u_turb = random.gauss(0, 0.08)
    v_turb = random.gauss(0, 0.08)
    
    u_east = u_mean + u_turb
    v_north += v_turb
    
    return u_east, v_north

# --- SIMULATION EXECUTION ---
file_prefix = start_time.strftime("GPSDATA_%y%m%d%H%M%S")
data_filename = f"{file_prefix}.txt"
meta_filename = f"{file_prefix}_meta.txt"

cur_lat = start_lat
cur_lon = start_lon
first_record_meta = ""
last_record_meta = ""

# Earth radius in meters
R_EARTH = 6371000.0

with open(data_filename, "w") as f_data:
    for i in range(num_fixes):
        cur_time = start_time + datetime.timedelta(seconds=i * dt)
        
        # Date & Time formatting
        date_str = cur_time.strftime("%m/%d/%y")
        time_str = cur_time.strftime("%H:%M:%S")
        
        # Convert Decimal Degrees to Deg Min.Min
        lat_deg = int(abs(cur_lat))
        lat_min = (abs(cur_lat) - lat_deg) * 60.0
        lat_dir = 'N' if cur_lat >= 0 else 'S'
        
        lon_deg = int(abs(cur_lon))
        lon_min = (abs(cur_lon) - lon_deg) * 60.0
        lon_dir = 'E' if cur_lon >= 0 else 'W'
        
        # Standard double-spaced two-line output
        line1 = (f"{date_str}  {time_str}  {lat_deg:02d} {lat_min:09.6f}{lat_dir}  "
                 f"{lon_deg:03d} {lon_min:09.6f}{lon_dir}  PDOP: 01.4 FIX TYPE: 3 (3D fix)\n")
        line2 = "P= 00000  T= 00000  C= 00000\n"
        
        f_data.write(line1)
        f_data.write(line2)
        
        # Record boundary metadata
        meta_str = f"{date_str} {time_str} Lat: {cur_lat:.5f} Lon: {cur_lon:.5f}"
        if i == 0:
            first_record_meta = meta_str
        if i == num_fixes - 1:
            last_record_meta = meta_str
            
        # Advect position forward based on local current velocity
        u_east, v_north = get_gulf_stream_velocity(cur_lat, cur_lon)
        
        # Convert m/s velocity to angular displacement (radians -> degrees)
        d_lat = (v_north * dt) / R_EARTH * (180.0 / math.pi)
        d_lon = (u_east * dt) / (R_EARTH * math.cos(math.radians(cur_lat))) * (180.0 / math.pi)
        
        cur_lat += d_lat
        cur_lon += d_lon

# Write metadata file
with open(meta_filename, "w") as f_meta:
    f_meta.write(f"{num_fixes}\n")
    f_meta.write(f"{first_record_meta}\n")
    f_meta.write(f"{last_record_meta}\n")

print(f"Generated realistic Gulf Stream drift files:")
print(f" -> Data:     {data_filename} ({num_fixes} fixes, {duration_hours}h duration)")
print(f" -> Metadata: {meta_filename}")
print(f" -> Start:    {first_record_meta}")
print(f" -> End:      {last_record_meta}")