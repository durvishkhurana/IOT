# ESP32 Smart Door Lock + Web Dashboard (UEC640 IoT project)

Two-factor (RFID card + 4-digit PIN) door lock on an ESP32 with a servo latch,
IR entry sensor, Blynk email alerts, and a live web dashboard running on a laptop.

```
firmware/rfid_door_servo/   Arduino sketch for the ESP32 (+ secrets.example.h)
backend/                    FastAPI + SQLite server, serves the dashboard too
  ├─ main.py                API endpoints + WebSocket
  ├─ database.py            SQLite tables and queries
  └─ simulate_device.py     fake ESP32 for demos without hardware
frontend/                   React + Vite dashboard (Recharts charts)
ARCHITECTURE.md             IoT four-layer model explanation (for the viva)
```

---

## 1. Install (once)

You need **Python 3.10+**, **Node.js 18+** and the **Arduino IDE 2.x**.

```bash
# backend
cd backend
python -m venv .venv
# Windows:  .venv\Scripts\activate        macOS/Linux:  source .venv/bin/activate
pip install -r requirements.txt

# frontend
cd ../frontend
npm install
```

## 2. Configure the backend (`.env`)

```bash
cd backend
cp .env.example .env        # Windows: copy .env.example .env
```

Edit `backend/.env`:

| Key | Meaning |
|---|---|
| `DEVICE_API_KEY` | Secret the ESP32 sends in the `X-API-Key` header. Pick any long string. |
| `DASHBOARD_USER` / `DASHBOARD_PASSWORD` | Dashboard login. |
| `SESSION_SECRET` | Any long random string (signs the login cookie). |

## 3. Build the frontend

```bash
cd frontend
npm run build               # creates frontend/dist, which FastAPI serves
```

(Re-run this only if you change the React code.)

## 4. Run the server

```bash
cd backend
uvicorn main:app --host 0.0.0.0 --port 8000
```

`--host 0.0.0.0` matters: it lets the ESP32 on the hotspot reach your laptop.
Open **http://localhost:8000** and log in with the user and password you set in `.env`.

> If Windows Firewall asks, allow Python on **private networks**. If the ESP32
> can't connect, this firewall is the most common reason.

## 5. Find your laptop's local IP

Connect the laptop to the **same hotspot** the ESP32 will use, then:

- **Windows:** `ipconfig` → under *Wireless LAN adapter Wi-Fi*, read **IPv4 Address** (e.g. `192.168.43.120`)
- **macOS:** `ipconfig getifaddr en0`
- **Linux:** `hostname -I`

Check it from your phone (also on the hotspot): open `http://<that-ip>:8000`. The login page should load.
The IP can change each time you reconnect to the hotspot, so check it again before every demo.

## 6. Configure and flash the ESP32

1. In Arduino IDE install (Library Manager): **Blynk**, **MFRC522** (GithubCommunity),
   **Keypad** (Mark Stanley), **ESP32Servo** (Kevin Harrington), **ArduinoJson** (Benoit Blanchon, v7).
   Board package: **esp32 by Espressif**. `HTTPClient` comes with it.
2. In `firmware/rfid_door_servo/` copy `secrets.example.h` → **`secrets.h`** and fill in:
   - Blynk template ID / name / auth token (same values as before)
   - Hotspot SSID and password
   - `SERVER_URL` = `http://<laptop-ip>:8000` (no trailing slash)
   - `SERVER_API_KEY` = the same value as `DEVICE_API_KEY` in `backend/.env`
3. Open `firmware/rfid_door_servo/rfid_door_servo.ino`, select **ESP32 Dev Module** and the COM port, then click **Upload**.
4. Open Serial Monitor at 115200 baud. You should see `WiFi Connected`, and the dashboard's **Device** card turns **Online** within a few seconds.

`secrets.h` is in `.gitignore`, so your WiFi password and Blynk token are never pushed to GitHub.

## 7. Demo without hardware (simulator)

With the server running, in a second terminal:

```bash
cd backend
python simulate_device.py --seed 7     # adds 7 days of history for the chart, then runs live
python simulate_device.py --fast       # live only, an event every few seconds
```

The simulator uses the same API as the ESP32: API key, heartbeats every 30 s, and command polling every 3 s.
So **Remote Unlock** and **Lockdown** on the dashboard work against it as well.
`--seed` writes old rows straight into `doorlock.db`, because the API always stamps events with the current time.
To start from an empty log, stop the server and delete `backend/doorlock.db`.

## Dashboard features

- **Status cards**: device online/offline (offline after 60 s without a heartbeat), door open/closed, lockdown, entries today, denied today (+ lockouts), last entry.
- **Live feed**: pushed over WebSocket, no refresh. Green = granted, red = denied/alert, grey = info.
- **Charts**: granted vs denied per day (7 days), denial reasons pie.
- **Event log**: filter by type and date, CSV export.
- **Controls**: Remote Unlock (asks for confirmation), Lockdown toggle.

## API summary

| Method | Path | Who | Purpose |
|---|---|---|---|
| POST | `/api/events` | ESP32 (`X-API-Key`) | store event + broadcast over WebSocket |
| GET | `/api/commands/next` | ESP32 (`X-API-Key`) | oldest pending command, marks it done |
| GET | `/api/events?limit=&type=&from=&to=` | dashboard | history (dates as `YYYY-MM-DD`) |
| GET | `/api/events/export.csv` | dashboard | full log as CSV |
| GET | `/api/stats` | dashboard | status card numbers |
| GET | `/api/stats/daily?days=7` | dashboard | per-day granted/denied + denial reasons |
| POST | `/api/commands` | dashboard | `{"command": "unlock" \| "lockdown_on" \| "lockdown_off"}` |
| WS | `/ws` | dashboard | live `{type: "event" \| "stats", ...}` messages |
| POST | `/api/login`, `/api/logout`, GET `/api/me` | dashboard | session cookie login |

Quick device test from a terminal (replace the key):

```bash
curl -X POST http://localhost:8000/api/events -H "X-API-Key: change-me-device-key" \
     -H "Content-Type: application/json" \
     -d '{"event_type":"card_scanned","result":"info","card_uid":"62 0D 9E 5C","reason":"test"}'
```

## Frontend development (optional)

`npm run dev` inside `frontend/` serves the React app with hot reload on http://localhost:5173.
It forwards `/api` and `/ws` to the backend on port 8000, so keep uvicorn running.

## Notes and limitations

- Event time is **server time when the event arrives**. Events that waited in the ESP32's offline queue get the time they were delivered, not the time they happened.
- Lockdown is kept in the ESP32's RAM. If the ESP32 reboots, lockdown turns off, and the dashboard shows that.
- Lockdown rejections don't count towards the 3-failure lockout and don't send a Blynk email. The admin caused them, not an intruder.
- Remote Unlock also works during the 30 s lockout (admin override). The lockout timer keeps running.
- While a PIN is being typed, the ESP32 only uses the network if the server answered last time. That way a dead server can't slow down the keypad.
- Plain HTTP on a local hotspot is fine for a demo. In a real deployment you'd use HTTPS.
