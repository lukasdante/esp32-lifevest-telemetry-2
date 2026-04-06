-- 1. Nuke everything in the correct order to avoid Foreign Key locks
DROP TABLE IF EXISTS Telemetry;
DROP TABLE IF EXISTS MappedSessions; 
DROP TABLE IF EXISTS Devices;

-- 2. Build the new merged Devices table
CREATE TABLE Devices (
    hw_id TEXT PRIMARY KEY,
    login_id TEXT UNIQUE,
    name TEXT NOT NULL,
    base_location TEXT,
    is_active BOOLEAN DEFAULT 1,
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP
);

-- 3. Build the Telemetry table
CREATE TABLE Telemetry (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    hw_id TEXT NOT NULL,
    lat REAL,
    lng REAL,
    heart_rate INTEGER,
    battery_pct INTEGER,
    rssi INTEGER,
    snr REAL,
    timestamp DATETIME DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY(hw_id) REFERENCES Devices(hw_id)
);