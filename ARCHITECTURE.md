# Architecture: IoT four-layer model

```
┌──────────────────────────────────────────────────────────────────────────┐
│ 4. APPLICATION   React dashboard (browser / phone)     Blynk app + email   │
│                  status cards, live feed, charts,      door_opened,        │
│                  log + CSV, remote unlock, lockdown    unauthorized_access │
├──────────────────────────────────────────────────────────────────────────┤
│ 3. SERVICE       FastAPI server on the laptop          Blynk Cloud         │
│                  auth (API key / login cookie),                            │
│                  SQLite (events, commands), stats,                         │
│                  command queue, WebSocket broadcast                        │
├──────────────────────────────────────────────────────────────────────────┤
│ 2. NETWORK       WiFi hotspot (802.11)                                     │
│                  ESP32 → server: HTTP POST JSON, HTTP GET poll every 3 s   │
│                  server → browser: HTTP + WebSocket push                   │
│                  ESP32 → Blynk Cloud: Blynk protocol over TCP              │
├──────────────────────────────────────────────────────────────────────────┤
│ 1. PERCEPTION    Sensors: RC522 RFID (SPI), 4x4 keypad, IR obstacle sensor │
│                  Actuators: MG90S servo (lock), green/red LED, buzzer      │
│                  Controller: ESP32 (decides grant / deny / alert)          │
└──────────────────────────────────────────────────────────────────────────┘
```

## 1. Perception layer: sensing and acting

| Part | Pin(s) | Role |
|---|---|---|
| RC522 RFID | SPI: SS 5, RST 4, SCK 18, MISO 19, MOSI 23 | first factor: reads the card UID (e.g. `62 0D 9E 5C`) |
| 4x4 keypad | rows 15,14,27,26 · cols 25,33,32,16 | second factor: 4-digit PIN, `#` submit, `*` clear |
| IR obstacle sensor | 34 (active LOW) | detects the door moving: entry vs unauthorised movement |
| MG90S servo | 17 | the lock: opens 5 s on a grant |
| Green / red LED, buzzer | 21 / 22 / 13 | local feedback, alarm |

The ESP32 runs all decisions locally: card check, PIN check, 20 s timeout, 3-strike 30 s lockout, 10 s entry window.
**The lock works even if WiFi, the laptop or Blynk is down.** The network is only used for reporting and remote control.

## 2. Network layer: moving the data

- **WiFi**: ESP32 and laptop join the same hotspot. The laptop is reached by its local IP, port 8000.
- **HTTP/JSON (device → server)**: each event is a `POST /api/events` with a small JSON body.
  The `X-API-Key` header authenticates the device. Timeouts are 1.5 s.
- **Store-and-forward**: if the server can't be reached, events wait in a 20-slot ring buffer on the ESP32 and are resent later.
- **Polling (server → device)**: the ESP32 asks `GET /api/commands/next` every 3 s.
  Polling is simpler than keeping a connection open and works through any hotspot.
- **WebSocket (server → browser)**: a connection that stays open, so the server can push each new event to the dashboard instantly.
- **Blynk**: a separate TCP connection to Blynk Cloud for the email alerts. Unchanged from the original project.

## 3. Service layer: storing and processing

FastAPI (`backend/main.py`) with SQLite (`backend/database.py`):

- `events` table: id, timestamp, event_type, card_uid, user_name, result, reason, failed_attempts
- `commands` table: id, command, created_at, status (pending/done), done_at
- Computes the stats:
  - entries / denials / lockouts today
  - door state from the latest `door_opened`/`door_closed` event
  - online = a heartbeat in the last 60 s
  - lockdown state
- Queues commands from the dashboard. Each one is handed to the device once, oldest first.
- Two kinds of security: the API key for the device, and a signed session cookie for dashboard users.

## 4. Application layer: what the user sees

- **Web dashboard** (React, served by the same FastAPI server): live status, live feed, 7-day chart, denial-reason pie, filterable log with CSV export, Remote Unlock and Lockdown.
- **Blynk**: email notifications for `door_opened` (authorised entry) and `unauthorized_access` (wrong card or PIN, timeout, movement without access).

## Data flow example: valid card + PIN

1. RFID reads `62 0D 9E 5C` → ESP32 queues `card_scanned`.
2. PIN `1234#` → `access_granted`, green LED, beep, servo opens → `door_opened`.
3. The ESP32's loop POSTs the queued events, one per pass → FastAPI saves them to SQLite → broadcasts over WebSocket → the dashboard feed turns green instantly.
4. The IR sensor sees the door move within 10 s → `entry_detected` + Blynk `door_opened` email.
5. After 5 s the servo closes → `door_closed`. The door card shows *Closed*.

## Data flow example: Remote Unlock

Dashboard button → `POST /api/commands {"command":"unlock"}` → saved as *pending* →
within 3 s the ESP32 polls `/api/commands/next`, receives `unlock`, and the server marks it *done* →
the ESP32 runs the same grant routine → logs `access_granted` with reason `remote unlock` → it appears in the feed.

## Event types

| event_type | result | When |
|---|---|---|
| `card_scanned` | info | any card read |
| `access_granted` | granted | correct card + PIN, or remote unlock |
| `access_denied` | denied | unknown card, wrong PIN, PIN timeout, lockdown |
| `lockout_started` / `lockout_ended` | alert / info | 3rd failure / 30 s later |
| `door_opened` / `door_closed` | info | servo opens / closes |
| `entry_detected` | info | IR movement within 10 s of a grant |
| `unauthorized_movement` | alert | IR movement outside that window |
| `device_online` | info | ESP32 boot |
| `heartbeat` | info | every 30 s |
