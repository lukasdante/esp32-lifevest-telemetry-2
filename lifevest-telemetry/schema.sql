-- Drop tables if they already exist (useful for fast resetting)
DROP TABLE IF EXISTS Telemetry;
DROP TABLE IF EXISTS Devices;
DROP TABLE IF EXISTS Settings;

-- 1. Create Devices Table
CREATE TABLE Devices (
    hw_id TEXT PRIMARY KEY,
    login_id TEXT UNIQUE,
    name TEXT,
    base_location TEXT,
    is_active INTEGER DEFAULT 1,
    pending_command TEXT DEFAULT 'NONE',
    
    -- Device-Specific Parameters
    automate_signal INTEGER DEFAULT 0,
    actuate_led INTEGER DEFAULT 1,
    actuate_buzzer INTEGER DEFAULT 1,
    device_hr_threshold INTEGER DEFAULT 100,
    signal_duration INTEGER DEFAULT 2,
    
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP
);

-- 2. Create Telemetry Table
CREATE TABLE Telemetry (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    hw_id TEXT,
    lat REAL,
    lng REAL,
    heart_rate INTEGER,
    rssi INTEGER,
    snr REAL,
    timestamp DATETIME DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY(hw_id) REFERENCES Devices(hw_id) ON DELETE CASCADE
);

-- 3. Create Settings Table
CREATE TABLE Settings (
    id INTEGER PRIMARY KEY,
    update_freq INTEGER,
    hr_threshold INTEGER,
    admin_pin TEXT
);

-- 4. Inject the Default System Settings
INSERT INTO Settings (id, update_freq, hr_threshold, admin_pin) 
VALUES (1, 3000, 100, '123456');

-- 5. (Optional) Inject your first testing device
INSERT INTO Devices (hw_id, login_id, name, base_location) 
VALUES ('HW-ESP32-001', 'VEST-892A', 'Alpha Vest', 'Manila Bay');