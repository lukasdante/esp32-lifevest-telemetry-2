// simulate.js
// This simulates your ESP32 Base Station sending data to your local server

const ENDPOINT = "http://localhost:8787/api/telemetry"; // Your local Wrangler address
const VEST_ID = "HW-ESP32-001"; // IMPORTANT: Change this to match a Hardware ID in your database!

// Starting coordinates (Manila Bay area)
let currentLat = 14.599500;
let currentLng = 120.984200;

console.log(`🚨 Starting hardware simulator for ${VEST_ID}...`);

// Run this loop every 3 seconds
setInterval(async () => {
    // 1. Simulate walking (moves slightly North-East every tick)
    currentLat += 0.000050; 
    currentLng += 0.000050;

    // 2. Simulate fluctuating heart rate between 70 and 115 BPM
    const currentHr = Math.floor(Math.random() * (115 - 70 + 1)) + 70;

    const payload = {
        vest_id: VEST_ID,
        latitude: currentLat,
        longitude: currentLng,
        heart_rate: currentHr,
        batt: 89,
        rssi: -90,
        snr: 10.5
    };

    try {
        const res = await fetch(ENDPOINT, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(payload)
        });
        
        const responseData = await res.json();
        
        console.log(`📍 Ping Sent! Lat: ${currentLat.toFixed(5)}, Lng: ${currentLng.toFixed(5)} | HR: ${currentHr}`);
        
        // This will test your Piggyback command system!
        if (responseData.command === "SIGNAL") {
            console.log("🔥 SERVER SENT PIGGYBACK COMMAND: ACTIVATE SIGNAL! 🔥");
        }

    } catch (err) {
        console.error("❌ Failed to reach local server. Is Wrangler running?", err.message);
    }
}, 3000);