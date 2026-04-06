import { Hono } from 'hono'

type Bindings = {
  DB: D1Database
}

const app = new Hono<{ Bindings: Bindings }>()

// 🛑 Notice how we completely removed serveStatic and the manifest imports!
// Cloudflare handles serving the HTML automatically now.

// ESP32 Base Station POST Endpoint
// ESP32 Base Station POST Endpoint
app.post('/api/telemetry', async (c) => {
  const payload = await c.req.json();
  // Using vest_id to match your new C++ payload format
  const { vest_id, latitude, longitude, heart_rate, batt, rssi, snr } = payload; 

  // 1. Save Telemetry (Assuming vest_id is mapped to hw_id or similar in your DB logic)
  // Note: Adjust the column names here if your C++ payload keys differ from your DB keys!
  await c.env.DB.prepare(
    `INSERT INTO Telemetry (hw_id, lat, lng, heart_rate, battery_pct, rssi, snr) 
     VALUES (?, ?, ?, ?, ?, ?, ?)`
  ).bind(vest_id, latitude, longitude, heart_rate, batt || 100, rssi || 0, snr || 0).run();

  // 2. Check for Piggyback Commands
  const device = await c.env.DB.prepare(
    `SELECT pending_command FROM Devices WHERE hw_id = ?`
  ).bind(vest_id).first();

  const command = device?.pending_command || 'NONE';

  // 3. If there was a command, clear it from the queue so we don't spam the vest
  if (command !== 'NONE') {
    await c.env.DB.prepare(
      `UPDATE Devices SET pending_command = 'NONE' WHERE hw_id = ?`
    ).bind(vest_id).run();
  }

  // 4. Return the piggyback JSON!
  return c.json({ success: true, command: command });
});

// Web Client GET Endpoint
app.get('/api/live/:login_id', async (c) => {
  const loginId = c.req.param('login_id')
  
  // 1. FIRST, check if the device actually exists and is active
  const device = await c.env.DB.prepare(
    `SELECT hw_id FROM Devices WHERE login_id = ? AND is_active = 1`
  ).bind(loginId).first()

  // If the device doesn't exist, THEN we throw the 404 error
  if (!device) {
    return c.json({ error: "Invalid VEST ID", isOnline: false }, 404)
  }

  // 2. SECOND, get the latest telemetry for this specific hardware
  const data = await c.env.DB.prepare(
    `SELECT * FROM Telemetry WHERE hw_id = ? ORDER BY timestamp DESC LIMIT 1`
  ).bind(device.hw_id as string).first()

  // 3. If the device exists, but has NO data yet, let the user in but tell the UI to wait
  if (!data) {
    return c.json({ 
      isOnline: false, 
      waitingForData: true, 
      message: "Waiting for first ping from hardware..." 
    })
  }

  const lastSeen = new Date(data.timestamp as string).getTime()
  const now = new Date().getTime()
  const isOnline = (now - lastSeen) < 30000

  return c.json({ ...data, isOnline, waitingForData: false })
})

// Web Client POST Endpoint: Queue a signal command
app.post('/api/live/:login_id/signal', async (c) => {
  const loginId = c.req.param('login_id');
  // Put the SIGNAL command in the queue for this specific vest
  await c.env.DB.prepare(
    `UPDATE Devices SET pending_command = 'SIGNAL' WHERE login_id = ?`
  ).bind(loginId).run();
  
  return c.json({ success: true, message: "Signal queued for next ping." });
});

// ==========================================
// SECURITY MIDDLEWARE
// ==========================================
const adminAuth = async (c: any, next: any) => {
  const pin = c.req.header('x-admin-pin');
  
  // Fetch the secure PIN from the database
  const settings = await c.env.DB.prepare('SELECT admin_pin FROM Settings WHERE id = 1').first();
  const validPin = settings?.admin_pin || '123456'; // Fallback just in case

  if (pin !== validPin) {
    return c.json({ error: "Unauthorized Access" }, 401);
  }
  await next();
};

// Protect all admin routes and the settings update route
app.use('/api/admin/*', adminAuth);
app.put('/api/settings', adminAuth);


// ==========================================
// ADMIN ENDPOINTS
// ==========================================

// 1. Get all devices + their online status
app.get('/api/admin/devices', async (c) => {
  // We use a subquery to find the absolute latest ping for each hardware device
  const query = `
    SELECT d.*, 
           (SELECT MAX(timestamp) FROM Telemetry WHERE hw_id = d.hw_id) as last_seen
    FROM Devices d
    ORDER BY d.created_at DESC
  `;
  const { results } = await c.env.DB.prepare(query).all();

  const now = new Date().getTime();
  const devices = results.map(d => {
    const lastSeen = d.last_seen ? new Date(d.last_seen as string).getTime() : 0;
    return {
      ...d,
      isOnline: (now - lastSeen) < 30000 // 30 seconds threshold
    };
  });

  return c.json(devices);
});

// 2. Add a new hardware device
app.post('/api/admin/devices', async (c) => {
  const { hw_id, login_id, name, base_location } = await c.req.json();
  try {
    await c.env.DB.prepare(
      `INSERT INTO Devices (hw_id, login_id, name, base_location) VALUES (?, ?, ?, ?)`
    ).bind(hw_id, login_id, name, base_location).run();
    return c.json({ success: true });
  } catch (err: any) {
    return c.json({ error: "Failed to add device. ID might already exist." }, 400);
  }
});

// 4. Update ALL Device Details (Including Swapping Hardware ID)
app.put('/api/admin/devices/:old_hw_id', async (c) => {
  const oldHwId = c.req.param('old_hw_id');
  const { hw_id, login_id, name, base_location } = await c.req.json();
  
  try {
    // We use a batch transaction. If the HW ID changes, we must update the Telemetry 
    // history so it doesn't get disconnected from the device.
    await c.env.DB.batch([
       c.env.DB.prepare(`UPDATE Telemetry SET hw_id = ? WHERE hw_id = ?`).bind(hw_id, oldHwId),
       c.env.DB.prepare(`UPDATE Devices SET hw_id = ?, login_id = ?, name = ?, base_location = ? WHERE hw_id = ?`).bind(hw_id, login_id, name, base_location, oldHwId)
    ]);
    return c.json({ success: true });
  } catch (err: any) {
    return c.json({ error: "Update failed. The HW ID or Login ID might already be in use." }, 400);
  }
});

// 5. Delete a Device and its Telemetry History
app.delete('/api/admin/devices/:hw_id', async (c) => {
  const hwId = c.req.param('hw_id');
  try {
    // Delete telemetry first, then the device itself
    await c.env.DB.batch([
        c.env.DB.prepare(`DELETE FROM Telemetry WHERE hw_id = ?`).bind(hwId),
        c.env.DB.prepare(`DELETE FROM Devices WHERE hw_id = ?`).bind(hwId)
    ]);
    return c.json({ success: true });
  } catch (err: any) {
    return c.json({ error: "Failed to delete device." }, 400);
  }
});

// ==========================================
// GLOBAL SETTINGS ENDPOINTS
// ==========================================
app.get('/api/settings', async (c) => {
  const settings = await c.env.DB.prepare('SELECT * FROM Settings WHERE id = 1').first();
  // Fallback to defaults if something goes wrong
  return c.json(settings || { update_freq: 3000, hr_threshold: 100 });
});

app.put('/api/settings', async (c) => {
  const { update_freq, hr_threshold, admin_pin } = await c.req.json();
  
  // If they provided a new PIN, update everything. Otherwise, just update the stats.
  if (admin_pin) {
    await c.env.DB.prepare(
      'UPDATE Settings SET update_freq = ?, hr_threshold = ?, admin_pin = ? WHERE id = 1'
    ).bind(update_freq, hr_threshold, admin_pin).run();
  } else {
    await c.env.DB.prepare(
      'UPDATE Settings SET update_freq = ?, hr_threshold = ? WHERE id = 1'
    ).bind(update_freq, hr_threshold).run();
  }
  return c.json({ success: true });
});


// Export the app for web requests, AND a scheduled task for database cleanup
export default {
  fetch: app.fetch,
  
  // This runs automatically based on your wrangler.toml CRON schedule
  async scheduled(event: any, env: Bindings, ctx: any) {
    console.log("Running daily telemetry cleanup...");
    // Deletes any telemetry rows older than 24 hours
    ctx.waitUntil(
      env.DB.prepare(`DELETE FROM Telemetry WHERE timestamp < datetime('now', '-1 day')`).run()
    );
  }
};