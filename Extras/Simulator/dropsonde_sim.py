import os
import datetime
import math
import random

# --- SIMULATION CONFIGURATION ---
START_LAT = 27.000000    # e.g., Florida shelf break
START_LON = -79.920000

# Phases duration in seconds
SURFACE_1_DUR = 300      # 5 mins at surface before drop
DROP_DUR = 1800          # 30 mins falling
RISE_DUR = 1800          # 30 mins rising
SURFACE_2_DUR = 3600     # 1 hour drifting at surface

TOTAL_DUR = SURFACE_1_DUR + DROP_DUR + RISE_DUR + SURFACE_2_DUR
DT = 1.0                 # 1 second per reading

# Base Sensor Values
BASE_TEMP = 26.5         # Surface temp (C)
BOTTOM_TEMP = 10.0       # Temp at depth (C)
BASE_HUM = 40.0          # Base humidity (%)
BASE_PRES = 1005.0       # Base pressure inside housing (mbar)

R_EARTH = 6371000.0

def get_gulf_stream_velocity(lat, lon):
    """
    Returns realistic surface currents (u_east, v_north) in meters/second
    based on climatological ADCP profiles across the 27N Florida Current transect.
    """
    lon_core = -79.72
    sigma = 0.16 if lon < lon_core else 0.28
        
    v_peak = 2.4  # Core peak speed: ~2.4 m/s
    v_bg = 0.3    # Background northward drift: ~0.3 m/s
    
    v_north = v_bg + (v_peak - v_bg) * math.exp(-((lon - lon_core) ** 2) / (2 * (sigma ** 2)))
    u_mean = (lon_core - lon) * 0.4 
    
    # Add turbulent perturbation noise
    u_turb = random.gauss(0, 0.08)
    v_turb = random.gauss(0, 0.08)
    
    return u_mean + u_turb, v_north + v_turb

def get_sensor_readings(phase, elapsed_in_phase):
    """
    Simulates temperature and pressure inside the sealed aluminum housing.
    Pressure subtly correlates with internal air temperature (PV=nRT).
    """
    if phase == 'SURFACE_1' or phase == 'SURFACE_2':
        target_t = BASE_TEMP
    elif phase == 'DROP':
        # Exponential decay to bottom temperature
        progress = elapsed_in_phase / DROP_DUR
        target_t = BASE_TEMP - (BASE_TEMP - BOTTOM_TEMP) * (1 - math.exp(-3 * progress))
    elif phase == 'RISE':
        # Exponential warming back to surface temperature
        progress = elapsed_in_phase / RISE_DUR
        target_t = BOTTOM_TEMP + (BASE_TEMP - BOTTOM_TEMP) * (1 - math.exp(-3 * progress))
        
    # Pressure changes subtly proportional to temperature (Gay-Lussac's Law approx)
    # P2 = P1 * (T2/T1) in Kelvin
    target_p = BASE_PRES * ((target_t + 273.15) / (BASE_TEMP + 273.15))
    target_h = BASE_HUM + random.gauss(0, 0.5) 
    
    # Add Gaussian noise
    temp = target_t + random.gauss(0, 0.02)
    pres = target_p + random.gauss(0, 0.05)
    hum = target_h + random.gauss(0, 0.05)
    volt = 0.00
    
    return round(temp, 2), round(hum, 2), round(pres, 2), volt

def run_simulation():
    start_time = datetime.datetime.now()
    
    # Create a unique root directory for this simulation run based on timestamp
    root_dir = f"sim_{start_time.strftime('%Y%m%d_%H%M%S')}"
    timestamp_str = start_time.strftime("%y%m%d%H%M%S")
    
    # Setup directories inside the new timestamped folder
    os.makedirs(f"{root_dir}/.META", exist_ok=True)
    os.makedirs(f"{root_dir}/DATA", exist_ok=True)
    
    gps_filename = f"{root_dir}/DATA/GPSDATA_{timestamp_str}.txt"
    bme_filename = f"{root_dir}/DATA/BMEDATA_{timestamp_str}.txt"
    meta_filename = f"{root_dir}/.META/GPSDATA_{timestamp_str}_meta.txt"
    note_filename = f"{root_dir}/.META/GPSDATA_{timestamp_str}_note.txt"
    
    cur_lat = START_LAT
    cur_lon = START_LON
    
    bme_stats = {'t': [], 'h': [], 'p': [], 'v': []}
    first_meta = ""
    last_meta = ""
    total_records = int(TOTAL_DUR / DT)
    
    with open(gps_filename, "w") as f_gps, open(bme_filename, "w") as f_bme:
        f_bme.write("Timestamp,Temperature(C),Humidity(%),Pressure(mbar),Voltage(V)\n")
        
        for i in range(total_records):
            cur_time = start_time + datetime.timedelta(seconds=i * DT)
            date_str_gps = cur_time.strftime("%m/%d/%y")
            time_str_gps = cur_time.strftime("%H:%M:%S")
            
            # Determine deployment phase
            if i < SURFACE_1_DUR:
                phase = 'SURFACE_1'
                elapsed = i
                has_gps = True
            elif i < SURFACE_1_DUR + DROP_DUR:
                phase = 'DROP'
                elapsed = i - SURFACE_1_DUR
                has_gps = False
            elif i < SURFACE_1_DUR + DROP_DUR + RISE_DUR:
                phase = 'RISE'
                elapsed = i - (SURFACE_1_DUR + DROP_DUR)
                has_gps = False
            else:
                phase = 'SURFACE_2'
                elapsed = i - (SURFACE_1_DUR + DROP_DUR + RISE_DUR)
                has_gps = True

            # Advect position regardless of GPS fix (underwater drift approximation)
            u_east, v_north = get_gulf_stream_velocity(cur_lat, cur_lon)
            d_lat = (v_north * DT) / R_EARTH * (180.0 / math.pi)
            d_lon = (u_east * DT) / (R_EARTH * math.cos(math.radians(cur_lat))) * (180.0 / math.pi)
            
            cur_lat += d_lat
            cur_lon += d_lon
            
            # 1. Write GPS Data
            if has_gps:
                lat_deg = int(abs(cur_lat))
                lat_min = (abs(cur_lat) - lat_deg) * 60.0
                lat_dir = 'N' if cur_lat >= 0 else 'S'
                
                lon_deg = int(abs(cur_lon))
                lon_min = (abs(cur_lon) - lon_deg) * 60.0
                lon_dir = 'E' if cur_lon >= 0 else 'W'
                
                pdop = round(random.uniform(1.5, 2.5), 1)
                line1 = f"{date_str_gps}  {time_str_gps}  {lat_deg:02d} {lat_min:09.6f}{lat_dir}  {lon_deg:03d} {lon_min:09.6f}{lon_dir}  PDOP: {pdop:04.1f} FIX TYPE: 3 (3D fix)\n"
            else:
                line1 = f"{date_str_gps}  {time_str_gps}  00 00.000000N  000 00.000000E PDOP: 00.0 FIX TYPE: 0 (NO FIX)\n"
                
            f_gps.write(line1)
            f_gps.write("P= 00000  T= 00000  C= 00000\n")
            
            # 2. Write BME Sensor Data
            t, h, p, v = get_sensor_readings(phase, elapsed)
            bme_stats['t'].append(t)
            bme_stats['h'].append(h)
            bme_stats['p'].append(p)
            bme_stats['v'].append(v)
            
            f_bme.write(f"{date_str_gps} {time_str_gps},{t:.2f},{h:.2f},{p:.2f},{v:.2f}\n")
            
            # 3. Track Metadata boundaries
            meta_str = f"{date_str_gps} {time_str_gps} Lat: {cur_lat:.5f} Lon: {cur_lon:.5f}"
            if i == 0:
                first_meta = meta_str
            if i == total_records - 1:
                last_meta = meta_str

    # 4. Write Metadata Summary File
    with open(meta_filename, "w") as f_meta:
        f_meta.write(f"{total_records}\n")
        f_meta.write(f"{first_meta}\n")
        f_meta.write(f"{last_meta}\n")
        
        # Min, Sum, Max for variables
        f_meta.write(f"{min(bme_stats['t']):.2f},{sum(bme_stats['t']):.2f},{max(bme_stats['t']):.2f},{total_records}\n")
        f_meta.write(f"{min(bme_stats['h']):.2f},{sum(bme_stats['h']):.2f},{max(bme_stats['h']):.2f}\n")
        f_meta.write(f"{min(bme_stats['p']):.2f},{sum(bme_stats['p']):.2f},{max(bme_stats['p']):.2f}\n")
        f_meta.write(f"{min(bme_stats['v']):.2f},{sum(bme_stats['v']):.2f},{max(bme_stats['v']):.2f}\n")

    # 5. Write Deployment Note File
    with open(note_filename, "w") as f_note:
        f_note.write(f"Automated simulated deployment starting at {first_meta}. Features sub-surface drop/rise simulation with sealed housing PV changes and Gulf Stream drift. Format: <b>HTML</b>")

    print("--- Simulation Complete ---")
    print(f"Generated {total_records} records in directory: {root_dir}/")
    print("Files created:")
    print(f"  {gps_filename}")
    print(f"  {bme_filename}")
    print(f"  {meta_filename}")
    print(f"  {note_filename}")

if __name__ == "__main__":
    run_simulation()
