import { Hono } from 'hono'

type Bindings = {
  DB: D1Database
}

const app = new Hono<{ Bindings: Bindings }>()

// 1. ESP32 Base Station POST Endpoint (Telemetry + Piggyback Check)
app.post('/api/telemetry', async (c) => {
  const payload = await c.req.json();
  const { vest_id, latitude, longitude, heart_rate, rssi, snr } = payload; 

  // Format the ID to match the database ('1' -> 'HW-ESP32-001')
  const formattedHwId = `HW-ESP32-${String(vest_id).padStart(3, '0')}`;

  // Save Telemetry
  await c.env.DB.prepare(
    `INSERT INTO Telemetry (hw_id, lat, lng, heart_rate, rssi, snr) 
     VALUES (?, ?, ?, ?, ?, ?)`
  ).bind(formattedHwId, latitude, longitude, heart_rate, rssi || 0, snr || 0).run();

  // Check for Piggyback Commands
  const device = await c.env.DB.prepare(
    `SELECT pending_command FROM Devices WHERE hw_id = ?`
  ).bind(formattedHwId).first();

  // Explicitly declare variables here
  const commandStr = (device?.pending_command as string) || 'NONE';
  let piggybackData = {};

  // If there is a command, mark it as 'AWAITING_ACK' and parse the JSON
  if (commandStr !== 'NONE' && commandStr !== 'AWAITING_ACK') {
    await c.env.DB.prepare(
      `UPDATE Devices SET pending_command = 'AWAITING_ACK' WHERE hw_id = ?`
    ).bind(formattedHwId).run();

    try {
      if (commandStr === 'SIGNAL') {
        piggybackData = { cmd: 1 }; // Fallback for old database entries
      } else {
        piggybackData = JSON.parse(commandStr);
      }
    } catch (e) {
      console.error("Failed to parse command payload");
    }
  }

  // Return the flat JSON response
  return c.json({ success: true, ...piggybackData });
});

// 2. Web Client POST Endpoint: Receiver confirms command actuation
app.post('/api/telemetry/ack', async (c) => {
  const payload = await c.req.json();
  const { vest_id } = payload;
  
  const formattedHwId = `HW-ESP32-${String(vest_id).padStart(3, '0')}`;

  // Log it so you can see it in the wrangler terminal
  console.log(`\n[WEB BACKEND] 🟢 ACK Received for ${formattedHwId}. Clearing database queue!\n`);

  // Clear the queue! The cycle is complete.
  await c.env.DB.prepare(
    `UPDATE Devices SET pending_command = 'NONE' WHERE hw_id = ?`
  ).bind(formattedHwId).run();

  return c.json({ success: true });
});

// Web Client GET Endpoint
app.get('/api/live/:login_id', async (c) => {
  const loginId = c.req.param('login_id')
  
  // 1. Fetch device AND its specific settings
  const device = await c.env.DB.prepare(
    `SELECT hw_id, automate_signal, actuate_led, actuate_buzzer, device_hr_threshold, signal_duration, pending_command 
     FROM Devices WHERE login_id = ? AND is_active = 1`
  ).bind(loginId).first()

  if (!device) return c.json({ error: "Invalid VEST ID", isOnline: false }, 404)

  const data = await c.env.DB.prepare(
    `SELECT * FROM Telemetry WHERE hw_id = ? ORDER BY timestamp DESC LIMIT 1`
  ).bind(device.hw_id as string).first()

  if (!data) return c.json({ isOnline: false, waitingForData: true, message: "Waiting for first ping..." })

  const lastSeen = new Date(data.timestamp as string).getTime()
  const isOnline = (new Date().getTime() - lastSeen) < 30000

  // Inject device settings into the payload
  return c.json({ ...data, isOnline, waitingForData: false, deviceSettings: device })
});

// Web Client POST Endpoint: Queue a signal command
app.post('/api/live/:login_id/signal', async (c) => {
  const loginId = c.req.param('login_id');
  
  // CHANGED: We now save the exact JSON string the ESP32 expects for Command 1
  await c.env.DB.prepare(
    `UPDATE Devices SET pending_command = '{"cmd":1}' WHERE login_id = ?`
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

// Web Client POST Endpoint: Update device settings & queue Command 2
app.post('/api/live/:login_id/settings', async (c) => {
  const loginId = c.req.param('login_id');
  const payload = await c.req.json();
  const { automate_signal, actuate_led, actuate_buzzer, hr_threshold, signal_duration } = payload;

  // Format the command string payload for the ESP32 (cmd: 2 designates the action)
  const cmd2Payload = JSON.stringify({
    cmd: 2,
    auto: automate_signal ? 1 : 0,
    led: actuate_led ? 1 : 0,
    buz: actuate_buzzer ? 1 : 0,
    hr: hr_threshold,
    dur: signal_duration
  });

  await c.env.DB.prepare(
    `UPDATE Devices 
     SET automate_signal = ?, actuate_led = ?, actuate_buzzer = ?, device_hr_threshold = ?, signal_duration = ?, pending_command = ?
     WHERE login_id = ?`
  ).bind(
    automate_signal ? 1 : 0, 
    actuate_led ? 1 : 0, 
    actuate_buzzer ? 1 : 0, 
    hr_threshold, 
    signal_duration, 
    cmd2Payload, 
    loginId
  ).run();

  return c.json({ success: true, message: "Settings saved and Command 2 queued." });
});