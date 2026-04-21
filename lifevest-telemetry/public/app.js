// ==========================================
// GLOBAL STATE (For Dashboard)
// ==========================================
let map = null;
let viewerMarker = null;
let wearerMarker = null;
let connectionLine = null;
let viewerLoc = null;
let wearerLoc = null;
let targetId = null;
let autoCenterEnabled = true;

let globalSettings = { update_freq: 3000, hr_threshold: 100 };
let pollingInterval = null;
let adminPin = sessionStorage.getItem('adminPin'); // Temporary session storage
let locationPermissionDenied = false; // Tracks if user blocked GPS

let navModeActive = false;
let navListener = null;

// ==========================================
// INITIALIZATION ROUTER
// ==========================================
document.addEventListener('DOMContentLoaded', () => {
    if (document.getElementById('loginForm')) {
        initLoginPage();
    } else if (document.getElementById('map')) {
        initDashboardPage();
    } else if (document.getElementById('adminDashboard')) {
        initAdminPage(); // NEW ROUTE
    }
});

// ==========================================
// LOGIN PAGE FUNCTIONS
// ==========================================
function initLoginPage() {
    const loginForm = document.getElementById('loginForm');
    const input = document.getElementById('lifevestId');
    const errorMsg = document.getElementById('loginError');
    const btn = document.getElementById('loginBtn');

    loginForm.addEventListener('submit', async (e) => {
        e.preventDefault();
        const id = input.value.trim().toUpperCase();
        if (!id) return;

        // 1. UI Loading State
        btn.innerText = "Connecting...";
        btn.style.opacity = "0.7";
        btn.disabled = true;
        errorMsg.style.display = 'none';

        try {
            // 2. Validate against the database BEFORE routing
            const res = await fetch(`/api/live/${id}`);
            
            if (!res.ok) {
                throw new Error("Provided VEST ID does not exist or has no data.");
            }

            // 3. Success! Save it and route.
            window.location.href = `/dashboard.html?id=${id}`;

        } catch (err) {
            // 4. Failure! Show error and reset button.
            errorMsg.innerText = err.message;
            errorMsg.style.display = 'block';
            
            btn.innerText = "Initiate Tracking";
            btn.style.opacity = "1";
            btn.disabled = false;
        }
    });
}

// ==========================================
// DASHBOARD ORCHESTRATION
// ==========================================
async function initDashboardPage() {
    // 1. Grab ID securely from the URL
    const urlParams = new URLSearchParams(window.location.search);
    targetId = urlParams.get('id');

    if (!targetId) {
        window.location.href = '/index.html';
        return;
    }
    
    // Clean up the URL visually (optional, keeps it looking nice)
    window.history.replaceState({}, document.title, `/dashboard.html?id=${targetId}`);
    
    document.getElementById('displayId').innerText = targetId;

    // Fetch Global Settings
    try {
        const setRes = await fetch('/api/settings');
        if (setRes.ok) globalSettings = await setRes.json();
    } catch (e) { console.warn("Using default settings."); }

    let initialData = null;
    try {
        const res = await fetch(`/api/live/${targetId}`);
        if (!res.ok) throw new Error("Invalid ID");
        initialData = await res.json();
    } catch (err) {
        alert("Session expired or invalid VEST ID. Returning to login.");
        localStorage.removeItem('targetLifevestId');
        window.location.href = '/index.html';
        return;
    }

    initializeMap(initialData);
    updateDashboardUI(initialData);
    startViewerGPS();

    // Use dynamic polling frequency
    pollingInterval = setInterval(fetchTelemetry, globalSettings.update_freq);
}

// ==========================================
// DASHBOARD: MAP SETUP
// ==========================================
function initializeMap(initialData) {
    // If no data exists yet, default the background map view to Metro Manila, 
    // but we WILL NOT place the wearer marker on the map.
    let viewLat = 14.5995; 
    let viewLng = 120.9842;
    let hasRealData = false;

    // ONLY use DB data. No proxy data.
    if (initialData && !initialData.waitingForData && initialData.lat && initialData.lng) {
        viewLat = initialData.lat;
        viewLng = initialData.lng;
        hasRealData = true;
    }

    map = L.map('map', { zoomControl: false }).setView([viewLat, viewLng], 17);
    
    L.tileLayer('https://{s}.basemaps.cartocdn.com/dark_all/{z}/{x}/{y}{r}.png', {
        attribution: '&copy; OpenStreetMap',
        updateWhenIdle: true,
        keepBuffer: 4
    }).addTo(map);

    const adminPulseInterval = 3000; 

    // Custom Viewer Icon (Updated with Directional Cone)
    // Custom Viewer Arrow (Now with the Radar Restored!)
    const viewerIconHtml = `
        <div class="viewer-arrow-container">
            <div id="viewerRadarCore" class="viewer-pulse"></div>
            
            <div id="viewerArrow" class="viewer-arrow"></div>
        </div>
    `;
    const viewerIcon = L.divIcon({ html: viewerIconHtml, className: '', iconSize: [30, 30], iconAnchor: [15, 15] });

    // Custom Wearer Icon
    const wearerIconHtml = `
        <div class="wearer-icon-container">
            <div id="wearerDotCore" class="wearer-dot"></div>
        </div>
    `;
    const wearerIcon = L.divIcon({ html: wearerIconHtml, className: '', iconSize: [16, 16], iconAnchor: [8, 8] });

    // Create markers, but DON'T add them to the map yet
    viewerMarker = L.marker([0, 0], { icon: viewerIcon });
    
    // Only set the wearer location and add it to the map IF we have real database coordinates
    if (hasRealData) {
        wearerLoc = L.latLng(viewLat, viewLng);
        wearerMarker = L.marker([viewLat, viewLng], { icon: wearerIcon }).addTo(map);
    } else {
        wearerLoc = null;
        wearerMarker = L.marker([0, 0], { icon: wearerIcon });
    }

    connectionLine = L.polyline([], { color: '#38bdf8', dashArray: '6, 8', weight: 3, opacity: 0.7 }).addTo(map);

    // User Interaction Listeners for Recenter logic
    const recenterBtn = document.getElementById('recenterBtn');
    map.on('dragstart', () => { autoCenterEnabled = false; recenterBtn.classList.remove('hidden'); });
    map.getContainer().addEventListener('wheel', () => { autoCenterEnabled = false; recenterBtn.classList.remove('hidden'); });
    recenterBtn.addEventListener('click', () => { autoCenterEnabled = true; recenterBtn.classList.add('hidden'); forceMapRecenter(); });
}

// ==========================================
// DASHBOARD: VIEWER GPS TRACKING
// ==========================================
// ==========================================
// DASHBOARD: VIEWER GPS TRACKING
// ==========================================
function startViewerGPS() {
    if (navigator.geolocation) {
        navigator.geolocation.watchPosition((pos) => {
            // Permission granted! Reset flags and update.
            locationPermissionDenied = false;
            viewerLoc = L.latLng(pos.coords.latitude, pos.coords.longitude);
            viewerMarker.setLatLng(viewerLoc);
            
            if (!map.hasLayer(viewerMarker)) viewerMarker.addTo(map); 
            
            updateMapAndDistance();
        }, (err) => {
            console.warn("GPS error:", err);
            // Error Code 1 means the user explicitly denied location access
            if (err.code === 1 || err.PERMISSION_DENIED) {
                locationPermissionDenied = true;
                viewerLoc = null; // Clear the coordinate
                
                // Remove the blue dot from the map if it was there
                if (map && map.hasLayer(viewerMarker)) {
                    map.removeLayer(viewerMarker);
                }
                
                updateMapAndDistance();
            }
        }, { enableHighAccuracy: true });
    } else {
        locationPermissionDenied = true;
        updateMapAndDistance();
    }
}

// ==========================================
// DASHBOARD: DATA FETCHING
// ==========================================
async function fetchTelemetry() {
    try {
        const res = await fetch(`/api/live/${targetId}`);
        if (!res.ok) {
            // Pull the actual error text from the backend so we aren't blind
            const errText = await res.text();
            throw new Error(`API Error ${res.status}: ${errText}`);
        }
        const data = await res.json();
        
        // Log the raw data so you can prove it reached the frontend!
        console.log("🔥 Live Data Received:", data); 
        
        updateDashboardUI(data);
        return data; 
    } catch (err) {
        // Scream the error into the browser console
        console.error("❌ Telemetry Blocked:", err.message); 
        updateStatusBadge(false);
        return null;
    }
}

// ==========================================
// DASHBOARD: UI UPDATING
// ==========================================
function updateDashboardUI(data) {
    if (!data) return;

    // 1. Handle a brand new vest with zero data
    if (data.waitingForData) {
        updateStatusBadge("WAITING_INITIAL");
        document.getElementById('lastUpdatedText').innerText = "Waiting for first ping...";
        return; 
    }

    // 2. Time-Based Status Logic
    // SQLite returns UTC time. We append 'Z' to ensure the browser converts it to your local timezone accurately.
    const timestampStr = data.timestamp.endsWith('Z') ? data.timestamp : data.timestamp + 'Z';
    const lastPingTime = new Date(timestampStr).getTime();
    const now = new Date().getTime();
    const diffMs = now - lastPingTime;
    
    let currentStatus = "LIVE";
    if (diffMs >= 300000) { // 5 minutes (300,000 ms)
        currentStatus = "OFFLINE";
    } else if (diffMs >= 60000) { // 1 minute (60,000 ms)
        currentStatus = "WAITING";
    }
    
    updateStatusBadge(currentStatus);

    // 3. Update the Last Updated Text UI
    const timeString = new Date(timestampStr).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' });
    document.getElementById('lastUpdatedText').innerText = `Last updated: ${timeString}`;

    // 4. Update Wearer Location
    // 4. Synchronized Radar & Wearer Location
    if (data.lat && data.lng) {
        wearerLoc = L.latLng(data.lat, data.lng);
        wearerMarker.setLatLng(wearerLoc);
        
        if (!map.hasLayer(wearerMarker)) wearerMarker.addTo(map);

        // --- THE SYNC LOGIC ---
        let delayMs = 0;

        // If we have both coordinates, calculate the impact time
        if (viewerLoc && wearerLoc) {
            // Convert GPS coordinates to screen pixels
            const viewerPx = map.latLngToContainerPoint(viewerLoc);
            const wearerPx = map.latLngToContainerPoint(wearerLoc);
            
            // Calculate pixel distance between the two dots
            const dx = wearerPx.x - viewerPx.x;
            const dy = wearerPx.y - viewerPx.y;
            const distPx = Math.sqrt(dx * dx + dy * dy);

            // Our radar grows to a 1500px radius over 3000ms.
            // Speed = 1500px / 3000ms = 0.5 pixels per millisecond.
            // Time = Distance / Speed
            delayMs = distPx / 0.5; 
            
            // Safety cap: If it's off-screen, don't delay longer than the ping interval
            if (delayMs > 3000) delayMs = 3000; 
        }

        // Trigger the Viewer's Radar Wave (Linear speed makes the math perfect)
        const radar = document.getElementById('viewerRadarCore');
        if (radar) {
            radar.style.animation = 'none'; // Reset
            void radar.offsetWidth;         // Force browser DOM reflow
            radar.style.animation = 'mapRadarSweep 3000ms linear forwards';
        }

        // Delay the Wearer's Flash until the exact moment the wave hits
        setTimeout(() => {
            const dot = document.getElementById('wearerDotCore');
            if (dot) {
                dot.classList.remove('flash');
                void dot.offsetWidth;
                dot.classList.add('flash');
            }
        }, delayMs);
    }

    // 5. Update Stats
    // 5. Update Stats & Trigger Distress Logic
    const hrVal = data.heart_rate || 0;
    const hrElement = document.getElementById('hrValue');
    const hrLabel = document.getElementById('hrStatusLabel');
    const distressOverlay = document.getElementById('distressOverlay');
    const wearerDot = document.getElementById('wearerDotCore');
    
    hrElement.innerText = hrVal || '--';

    const activeThreshold = data.deviceSettings?.device_hr_threshold || globalSettings.hr_threshold || 100;

    // Pre-fill the form (only if the user isn't currently typing in it)
    if (data.deviceSettings && document.getElementById('deviceSettingsModal').classList.contains('hidden')) {
        document.getElementById('setAutomateSignal').checked = !!data.deviceSettings.automate_signal;
        document.getElementById('setActuateLed').checked = !!data.deviceSettings.actuate_led;
        document.getElementById('setActuateBuzzer').checked = !!data.deviceSettings.actuate_buzzer;
        document.getElementById('setDeviceHrThreshold').value = data.deviceSettings.device_hr_threshold;
        document.getElementById('setSignalDuration').value = data.deviceSettings.signal_duration;
    }

    hrElement.innerText = hrVal || '--';

    // Distress check using activeThreshold
    if (hrVal >= activeThreshold) {
        // ACTIVATE DISTRESS MODE
        hrElement.className = "text-red";
        hrLabel.className = "hr-status-label text-red";
        hrLabel.innerText = "IN DISTRESS";
        distressOverlay.style.display = "block";
        if (wearerDot) wearerDot.className = "wearer-dot bg-red";
    } else if (hrVal > 0) {
        // NORMAL MODE
        hrElement.className = "text-green";
        hrLabel.className = "hr-status-label text-green";
        hrLabel.innerText = "NORMAL";
        distressOverlay.style.display = "none";
        if (wearerDot) wearerDot.className = "wearer-dot bg-green";
    } else {
        // NO DATA
        hrElement.className = "";
        hrLabel.className = "hr-status-label";
        hrLabel.innerText = "WAITING";
        distressOverlay.style.display = "none";
    }
    
    // 7. Update Signal Bars and Viewport
    updateSignalBars(data.rssi);
    updateMapAndDistance(); 

    // --- NEW: Universal Smart Button ACK Logic ---
    const signalBtn = document.getElementById('signalBtn');
    const syncBtn = document.getElementById('saveDeviceSettingsBtn');

    if (data.deviceSettings) {
        const cmdState = data.deviceSettings.pending_command;

        if (cmdState === 'NONE') {
            // SYSTEM IDLE: Both buttons are ready
            if (signalBtn) {
                signalBtn.innerHTML = "Activate Signaling";
                signalBtn.style.backgroundColor = ""; 
                signalBtn.style.opacity = "1";
                signalBtn.disabled = false;
            }
            if (syncBtn) {
                syncBtn.innerHTML = "Save & Sync";
                syncBtn.style.backgroundColor = "#0284c7"; // Default Blue
                syncBtn.disabled = false;
            }
        } 
        else if (cmdState === 'AWAITING_ACK') {
            // VEST PROCESSING: Lock both buttons while the radio does its job
            if (signalBtn) {
                signalBtn.innerHTML = "Waiting for Vest ACK...";
                signalBtn.style.backgroundColor = "#eab308"; // Yellow
                signalBtn.style.opacity = "1";
                signalBtn.disabled = true;
            }
            if (syncBtn) {
                syncBtn.innerHTML = "Waiting for Vest ACK...";
                syncBtn.style.backgroundColor = "#eab308"; // Yellow
                syncBtn.disabled = true;
            }
        } 
        else {
            // QUEUED IN DATABASE: Figure out which command is waiting for the next ping
            let isSignal = cmdState.includes('"cmd":1') || cmdState === 'SIGNAL';
            let isSettings = cmdState.includes('"cmd":2');

            if (signalBtn) {
                signalBtn.innerHTML = isSignal ? "Queued for next ping..." : "System Busy...";
                signalBtn.style.backgroundColor = isSignal ? "#22c55e" : "#64748b";
                signalBtn.style.opacity = isSignal ? "0.8" : "0.5";
                signalBtn.disabled = true;
            }
            if (syncBtn) {
                syncBtn.innerHTML = isSettings ? "Queued for next ping..." : "System Busy...";
                syncBtn.style.backgroundColor = isSettings ? "#22c55e" : "#64748b";
                syncBtn.disabled = true;
            }
        }
    }
}

function updateSignalBars(rssi = -120) {
    let activeBars = 0;
    if (rssi >= -95) activeBars = 4;
    else if (rssi >= -105) activeBars = 3;
    else if (rssi >= -115) activeBars = 2;
    else if (rssi > -120) activeBars = 1;

    for (let i = 1; i <= 4; i++) {
        const bar = document.getElementById(`bar${i}`);
        if (bar) {
            if (i <= activeBars) bar.classList.add('active');
            else bar.classList.remove('active');
        }
    }
}

// ==========================================
// DASHBOARD: VIEWPORT LOGIC
// ==========================================
function updateMapAndDistance() {
    if (!map) return; 

    const distDisplay = document.querySelector('.distance-display');

    if (viewerLoc && wearerLoc) {
        const distMeters = viewerLoc.distanceTo(wearerLoc);
        distDisplay.innerHTML = `<span id="distanceValue">${distMeters.toFixed(1)}</span>`;
        // Draws the dashed line!
        connectionLine.setLatLngs([viewerLoc, wearerLoc]); 
    } else if (locationPermissionDenied) {
        distDisplay.innerHTML = `
            <div style="display: flex; flex-direction: column; align-items: center; gap: 8px; margin-top: 5px;">
                <svg viewBox="0 0 24 24" width="28" height="28" stroke="#ef4444" stroke-width="2" fill="none" stroke-linecap="round" stroke-linejoin="round">
                    <line x1="2" y1="2" x2="22" y2="22"></line>
                    <path d="M8.064 3.327A6 6 0 0 1 18 8c0 3.193-2.126 6.368-4.225 9.176l-.42.56-1.57-2.094"></path>
                    <path d="M10.153 10.153c-1.896.79-3.323 2.502-4.153 4.417L12 22l1.62-2.16"></path>
                </svg>
                <span style="font-size: 11px; color: #94a3b8; font-family: system-ui, sans-serif; font-weight: normal; line-height: 1.3; max-width: 200px; text-align: center;">
                    Location access denied.<br>Enable it to track distance.
                </span>
            </div>
        `;
        connectionLine.setLatLngs([]);
    } else {
        distDisplay.innerHTML = `<span id="distanceValue" style="font-size: 24px;">Waiting...</span>`;
        connectionLine.setLatLngs([]);
    }

    // Stop here if manual control taken, UNLESS we are in locked Nav Mode
    if (!autoCenterEnabled && !navModeActive) return; 
    forceMapRecenter();
}

function updateStatusBadge(status) {
    const badge = document.getElementById('statusBadge');
    if (!badge) return;

    if (status === "LIVE") {
        badge.innerText = "● LIVE";
        badge.className = "badge badge-online";
    } else if (status === "WAITING" || status === "WAITING_INITIAL") {
        badge.innerText = "◐ WAITING";
        badge.className = "badge badge-connecting"; // Uses your grey slate CSS class
    } else if (status === "OFFLINE") {
        badge.innerText = "○ OFFLINE";
        badge.className = "badge badge-offline";
    }
}

function forceMapRecenter() {
    if (!map) return;

    if (navModeActive && viewerLoc) {
        // NAVIGATION MODE: Lock strictly onto the user, zoom in close.
        map.setView(viewerLoc, 18, { animate: true });
        return; 
    }

    const paddingValue = map.getSize().x < 600 ? 25 : 60;

    if (viewerLoc && wearerLoc) {
        const latOffset = Math.abs(viewerLoc.lat - wearerLoc.lat);
        const lngOffset = Math.abs(viewerLoc.lng - wearerLoc.lng);
        const southWest = L.latLng(viewerLoc.lat - latOffset, viewerLoc.lng - lngOffset);
        const northEast = L.latLng(viewerLoc.lat + latOffset, viewerLoc.lng + lngOffset);
        map.fitBounds(L.latLngBounds(southWest, northEast), { padding: [paddingValue, paddingValue], maxZoom: 18, animate: true });
    } else if (wearerLoc) {
        map.setView(wearerLoc, 17, { animate: true });
    } else if (viewerLoc) {
        map.setView(viewerLoc, 17, { animate: true });
    }
}

// ==========================================
// ADMIN PAGE LOGIC
// ==========================================
function initAdminPage() {
    if (!adminPin) {
        document.getElementById('adminAuthOverlay').classList.remove('hidden');
    } else {
        document.getElementById('adminAuthOverlay').classList.add('hidden');
        loadFleetData();
    }
    
    loadFleetData();

    // Physically force uppercase in the actual DOM values, not just visually
    document.getElementById('newHwId').addEventListener('input', e => e.target.value = e.target.value.toUpperCase());
    document.getElementById('newLoginId').addEventListener('input', e => e.target.value = e.target.value.toUpperCase());

    // Setup Add Device Form
    const addForm = document.getElementById('addDeviceForm');
    addForm.addEventListener('submit', async (e) => {
        e.preventDefault();
        const btn = document.getElementById('addDeviceBtn');
        const errorText = document.getElementById('adminFormError');
        
        btn.innerText = "Adding...";
        btn.disabled = true;
        errorText.style.display = 'none';

        const payload = {
            hw_id: document.getElementById('newHwId').value.trim().toUpperCase(),
            login_id: document.getElementById('newLoginId').value.trim().toUpperCase(),
            name: document.getElementById('newName').value.trim(),
            base_location: document.getElementById('newLocation').value.trim()
        };

        try {
            const res = await fetch('/api/admin/devices', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json', 'x-admin-pin': adminPin},
                body: JSON.stringify(payload)
            });

            if (!res.ok) {
                const errData = await res.json();
                throw new Error(errData.error || "Failed to add device.");
            }

            addForm.reset();
            loadFleetData(); // Refresh table immediately
        } catch (err) {
            errorText.innerText = err.message;
            errorText.style.display = 'block';
        } finally {
            btn.innerText = "Add Device to Fleet";
            btn.disabled = false;
        }
    });
    // Physically force uppercase on the Edit Modal inputs too
    document.getElementById('editHwId').addEventListener('input', e => e.target.value = e.target.value.toUpperCase());
    document.getElementById('editLoginId').addEventListener('input', e => e.target.value = e.target.value.toUpperCase());

    // Setup Edit Modal Form Submission
    const editForm = document.getElementById('editDeviceForm');
    editForm.addEventListener('submit', async (e) => {
        e.preventDefault();
        const oldHwId = document.getElementById('editOldHwId').value;
        const errorText = document.getElementById('editFormError');
        
        const payload = {
            hw_id: document.getElementById('editHwId').value.trim().toUpperCase(),
            login_id: document.getElementById('editLoginId').value.trim().toUpperCase(),
            name: document.getElementById('editName').value.trim(),
            base_location: document.getElementById('editLocation').value.trim()
        };

        try {
            const res = await fetch(`/api/admin/devices/${oldHwId}`, {
                method: 'PUT',
                headers: { 'Content-Type': 'application/json', 'x-admin-pin': adminPin },
                body: JSON.stringify(payload)
            });

            if (!res.ok) throw new Error((await res.json()).error || "Failed to update.");
            
            closeEditModal();
            loadFleetData();
        } catch (err) {
            errorText.innerText = err.message;
            errorText.style.display = 'block';
        }
    });

    // 1. Setup Delete Button (Now opens the custom warning modal)
    document.getElementById('deleteDeviceBtn').addEventListener('click', () => {
        const hwId = document.getElementById('editOldHwId').value;
        // Inject the ID into the warning text
        document.getElementById('deleteTargetHwId').innerText = hwId; 
        // Show the warning modal
        document.getElementById('deleteConfirmModal').classList.remove('hidden');
    });

    // 2. Setup the ACTUAL Final Delete Action
    document.getElementById('confirmDeleteActionBtn').addEventListener('click', async () => {
        const hwId = document.getElementById('editOldHwId').value;
        const btn = document.getElementById('confirmDeleteActionBtn');

        // Show loading state on the red button
        btn.innerText = "Deleting...";
        btn.style.opacity = "0.7";
        btn.disabled = true;

        try {
            const res = await fetch(`/api/admin/devices/${hwId}`, { 
                method: 'DELETE',
                headers: { 'x-admin-pin': adminPin }
            });
            if (!res.ok) throw new Error("Failed to delete.");
            
            // Success! Close BOTH modals and refresh the table
            closeDeleteConfirmModal();
            closeEditModal();
            loadFleetData();
        } catch (err) {
            alert(err.message);
        } finally {
            // Reset button state just in case it fails and stays open
            btn.innerText = "Yes, Delete Everything";
            btn.style.opacity = "1";
            btn.disabled = false;
        }
    });

    // Fetch and populate current settings
    fetch('/api/settings').then(r => r.json()).then(s => {
        document.getElementById('setUpdateFreq').value = s.update_freq;
        document.getElementById('setHrThreshold').value = s.hr_threshold;
    });

    // Save Settings & PIN
    document.getElementById('settingsForm').addEventListener('submit', async (e) => {
        e.preventDefault();
        const btn = document.getElementById('saveSettingsBtn');
        const newPin = document.getElementById('setAdminPin').value.trim();
        
        const payload = {
            update_freq: parseInt(document.getElementById('setUpdateFreq').value),
            hr_threshold: parseInt(document.getElementById('setHrThreshold').value)
        };

        if (newPin !== '') {
            payload.admin_pin = newPin; // Only send if they typed something
        }

        btn.innerText = "Saving...";
        try {
            const res = await fetch('/api/settings', {
                method: 'PUT',
                headers: { 'Content-Type': 'application/json', 'x-admin-pin': adminPin },
                body: JSON.stringify(payload)
            });

            if (!res.ok) throw new Error("Failed to save.");

            btn.innerText = "Saved!";
            if (newPin !== '') {
                // If they changed the PIN, update the current session so they don't get locked out!
                adminPin = newPin;
                sessionStorage.setItem('adminPin', newPin);
                document.getElementById('setAdminPin').value = ''; // Clear the input field
            }
        } catch (err) {
            btn.innerText = "Error!";
        }
        setTimeout(() => btn.innerText = "Save Settings", 2000);
    });

    // Auto-refresh the fleet online status every 10 seconds
    setInterval(loadFleetData, 10000);


}

async function loadFleetData() {
    const tbody = document.getElementById('fleetTableBody');
    try {
        const res = await fetch('/api/admin/devices', {
            headers: { 'x-admin-pin': adminPin }
        });
        const devices = await res.json();

        tbody.innerHTML = ''; // Clear loading text

        if (devices.length === 0) {
            tbody.innerHTML = '<tr><td colspan="5" style="text-align: center;">No devices found.</td></tr>';
            return;
        }

        devices.forEach(d => {
            const statusHtml = d.isOnline 
                ? '<span class="badge badge-online">● ONLINE</span>' 
                : '<span class="badge badge-offline">○ OFFLINE</span>';

            const tr = document.createElement('tr');
            tr.innerHTML = `
                <td>${statusHtml}</td>
                <td style="font-family: monospace; color: #94a3b8;">${d.hw_id}</td>
                <td style="font-weight: bold;">${d.name}</td>
                <td style="font-family: monospace; color: #38bdf8;">${d.login_id}</td>
                <td>
                    <div class="action-buttons">
                        <button class="btn-edit" onclick="openEditModal('${d.hw_id}', '${d.login_id}', '${d.name.replace(/'/g, "\\'")}', '${d.base_location.replace(/'/g, "\\'")}')">Edit Details</button>
                    </div>
                </td>
            `;
            tbody.appendChild(tr);
        });
    } catch (err) {
        console.error("Failed to load fleet data:", err);
    }
}

// Opens the modal and pre-fills current data
function openEditModal(hwId, loginId, name, location) {
    document.getElementById('editOldHwId').value = hwId;
    document.getElementById('editHwId').value = hwId;
    document.getElementById('editLoginId').value = loginId;
    document.getElementById('editName').value = name;
    document.getElementById('editLocation').value = location;
    
    document.getElementById('editFormError').style.display = 'none';
    document.getElementById('editModal').classList.remove('hidden');
}

function closeEditModal() {
    document.getElementById('editModal').classList.add('hidden');
}

function closeDeleteConfirmModal() {
    document.getElementById('deleteConfirmModal').classList.add('hidden');
}

// --- NEW ADMIN SECURITY LOGIC ---
async function verifyAdminPin() {
    const pin = document.getElementById('adminPinInput').value;
    try {
        // Test the PIN against the backend
        const res = await fetch('/api/admin/devices', { headers: { 'x-admin-pin': pin } });
        if (res.ok) {
            adminPin = pin;
            sessionStorage.setItem('adminPin', pin); // Remember for this tab session
            document.getElementById('adminAuthOverlay').classList.add('hidden');
            loadFleetData(); // Load the data now that we are clear
        } else {
            document.getElementById('adminAuthError').style.display = 'block';
        }
    } catch (err) {
        alert("Network error.");
    }
}

// ==========================================
// PIGGYBACK COMMAND TRIGGER
// ==========================================
async function triggerSignal() {
    if (!targetId) return;
    const btn = document.getElementById('signalBtn');
    
    // UI Feedback
    btn.innerHTML = "Queuing Signal...";
    btn.style.opacity = "0.7";
    btn.disabled = true;

    try {
        const res = await fetch(`/api/live/${targetId}/signal`, { method: 'POST' });
        if (!res.ok) throw new Error("Failed to queue.");
        // We do NOT reset the button here anymore. The live telemetry polling will handle it!
    } catch (err) {
        alert("Network Error: Could not reach server.");
        btn.innerHTML = "Activate Signaling";
        btn.style.opacity = "1";
        btn.disabled = false;
    } 
}

// ==========================================
// DASHBOARD: NAVIGATION ENGINE
// ==========================================
async function toggleNavMode() {
    const btn = document.getElementById('navModeBtn');
    const mapElement = document.getElementById('map');
    const arrow = document.getElementById('viewerArrow');

    // Turn OFF Navigation Mode
    if (navModeActive) {
        navModeActive = false;
        btn.classList.remove('active');
        
        window.removeEventListener('deviceorientation', navListener, true);
        window.removeEventListener('deviceorientationabsolute', navListener, true);
        
        map.dragging.enable();
        mapElement.classList.remove('nav-spinning');
        mapElement.style.transform = 'translate(-50%, -50%) rotate(0deg)';
        if (arrow) arrow.style.transform = 'rotate(0deg)';
        
        map.invalidateSize();
        forceMapRecenter(); // Snap back to viewing both dots
        return;
    }

    // Turn ON: iOS 13+ requires explicit permission via this user click
    if (typeof DeviceOrientationEvent !== 'undefined' && typeof DeviceOrientationEvent.requestPermission === 'function') {
        try {
            const permission = await DeviceOrientationEvent.requestPermission();
            if (permission !== 'granted') {
                alert('Navigation mode requires compass permission.');
                return;
            }
        } catch (err) {
            console.warn("Device does not support orientation permissions.", err);
            return;
        }
    }

    // Activate Engine
    navModeActive = true;
    btn.classList.add('active');
    
    // Disable dragging and oversize the map
    autoCenterEnabled = true; 
    document.getElementById('recenterBtn').classList.add('hidden'); 
    
    map.dragging.disable();
    mapElement.classList.add('nav-spinning');
    map.invalidateSize(); 

    forceMapRecenter(); // Lock camera to user

    // Listen to hardware magnetometer
    navListener = (e) => {
        let heading = null;
        if (e.webkitCompassHeading) {
            heading = e.webkitCompassHeading;
        } else if (e.absolute && e.alpha !== null) {
            heading = 360 - e.alpha; 
        }

        if (heading !== null) {
            // Spin the huge map container inversely
            mapElement.style.transform = `translate(-50%, -50%) rotate(${-heading}deg)`;
            // Counter-spin the arrow so it ALWAYS points UP relative to your screen
            if (arrow) arrow.style.transform = `rotate(${heading}deg)`;
        }
    };

    if ('ondeviceorientationabsolute' in window) {
        window.addEventListener('deviceorientationabsolute', navListener, true);
    } else {
        window.addEventListener('deviceorientation', navListener, true);
    }
}

// ==========================================
// DEVICE SETTINGS MODAL
// ==========================================
document.getElementById('openSettingsBtn')?.addEventListener('click', () => {
    document.getElementById('deviceSettingsModal').classList.remove('hidden');
});

document.getElementById('deviceSettingsForm')?.addEventListener('submit', async (e) => {
    e.preventDefault();
    if (!targetId) return;

    const btn = document.getElementById('saveDeviceSettingsBtn');
    btn.innerText = "Syncing...";
    btn.disabled = true;

    const payload = {
        automate_signal: document.getElementById('setAutomateSignal').checked,
        actuate_led: document.getElementById('setActuateLed').checked,
        actuate_buzzer: document.getElementById('setActuateBuzzer').checked,
        hr_threshold: parseInt(document.getElementById('setDeviceHrThreshold').value),
        signal_duration: parseInt(document.getElementById('setSignalDuration').value)
    };

    try {
        const res = await fetch(`/api/live/${targetId}/settings`, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(payload)
        });
        if (!res.ok) throw new Error("Failed to sync.");
        
        // REMOVED: document.getElementById('deviceSettingsModal').classList.add('hidden');
        // We leave the modal open so the user can see the button turn Yellow then Blue!

    } catch (err) {
        alert("Network error: Could not sync settings to device.");
        btn.innerText = "Save & Sync";
        btn.disabled = false;
    }
});