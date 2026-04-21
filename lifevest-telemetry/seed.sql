-- Wipe old data
DELETE FROM Telemetry;
DELETE FROM Devices;

-- Insert Device
INSERT INTO Devices (hw_id, login_id, name, base_location) 
VALUES ('HW-ESP32-001', 'VEST-892A', 'Alpha Vest', 'Intramuros Station');

-- Insert "Close" Data (Approx 1km from typical Manila center)
INSERT INTO Telemetry (hw_id, lat, lng, heart_rate, rssi, snr) 
VALUES ('HW-ESP32-001', 14.7036, 120.9919, 78, -92, 11.2);