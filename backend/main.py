"""
main.py - FastAPI backend for the ESP32 smart door lock dashboard.

Run:   uvicorn main:app --host 0.0.0.0 --port 8000

Who calls what:
  ESP32 (needs header X-API-Key):
      POST /api/events          report an event
      GET  /api/commands/next   poll for a queued command (every 3 s)
  Dashboard (needs login session cookie):
      GET  /api/events          history with filters
      GET  /api/events/export.csv
      GET  /api/stats           numbers for the status cards
      GET  /api/stats/daily     data for the charts
      POST /api/commands        queue unlock / lockdown_on / lockdown_off
      WS   /ws                  live push of new events + stats
"""

import asyncio
import csv
import io
import os
import secrets
from contextlib import asynccontextmanager
from datetime import datetime, timedelta

from dotenv import load_dotenv

load_dotenv(os.path.join(os.path.dirname(__file__), ".env"))

from fastapi import Depends, FastAPI, Header, HTTPException, Query, Request, WebSocket, WebSocketDisconnect
from fastapi.responses import HTMLResponse, Response
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel
from starlette.middleware.sessions import SessionMiddleware

import database as db

# ---------------------------------------------------------------- config (.env)
API_KEY = os.getenv("DEVICE_API_KEY", "change-me-device-key")
DASH_USER = os.getenv("DASHBOARD_USER", "admin")
DASH_PASS = os.getenv("DASHBOARD_PASSWORD", "admin123")
SESSION_SECRET = os.getenv("SESSION_SECRET", "dev-secret-change-me")
OFFLINE_AFTER_S = 60  # device counts as offline if no heartbeat for 60 s

FRONTEND_DIST = os.path.join(os.path.dirname(__file__), "..", "frontend", "dist")


# ---------------------------------------------------------------- WebSocket hub
class Hub:
    """Keeps the list of open dashboard WebSockets and broadcasts to all of them."""

    def __init__(self):
        self.clients: set[WebSocket] = set()

    async def broadcast(self, message: dict):
        dead = []
        for ws in self.clients:
            try:
                await ws.send_json(message)
            except Exception:
                dead.append(ws)
        for ws in dead:
            self.clients.discard(ws)


hub = Hub()


async def stats_ticker():
    """Every 10 s push fresh stats, so 'device offline' shows up even when no events arrive."""
    while True:
        await asyncio.sleep(10)
        if hub.clients:
            await hub.broadcast({"type": "stats", "stats": compute_stats()})


@asynccontextmanager
async def lifespan(app: FastAPI):
    db.init_db()
    task = asyncio.create_task(stats_ticker())
    yield
    task.cancel()


app = FastAPI(title="Smart Door Lock", lifespan=lifespan)
app.add_middleware(SessionMiddleware, secret_key=SESSION_SECRET, max_age=8 * 3600, same_site="lax")


# ---------------------------------------------------------------- auth helpers
def require_device(x_api_key: str | None = Header(default=None)):
    """Device authentication: the ESP32 sends the shared key in the X-API-Key header."""
    if not x_api_key or not secrets.compare_digest(x_api_key, API_KEY):
        raise HTTPException(status_code=401, detail="invalid API key")


def require_login(request: Request):
    """Dashboard authentication: a signed session cookie set by /api/login."""
    if not request.session.get("user"):
        raise HTTPException(status_code=401, detail="login required")


# ---------------------------------------------------------------- stats
def compute_stats() -> dict:
    today = datetime.now().date().isoformat()
    with db.get_db() as conn:
        def count(event_type):
            return conn.execute(
                "SELECT COUNT(*) FROM events WHERE event_type = ? AND timestamp >= ?",
                (event_type, today),
            ).fetchone()[0]

        def latest(*types):
            marks = ",".join("?" * len(types))
            row = conn.execute(
                f"SELECT * FROM events WHERE event_type IN ({marks}) ORDER BY id DESC LIMIT 1", types
            ).fetchone()
            return dict(row) if row else None

        entries_today = count("access_granted")
        denied_today = count("access_denied")
        lockouts_today = count("lockout_started")
        last_entry = latest("access_granted")
        last_door = latest("door_opened", "door_closed")
        last_seen = latest("heartbeat", "device_online")
        last_boot = latest("device_online")
        last_lockdown_cmd = conn.execute(
            "SELECT * FROM commands WHERE command IN ('lockdown_on','lockdown_off') ORDER BY id DESC LIMIT 1"
        ).fetchone()
        pending = conn.execute("SELECT COUNT(*) FROM commands WHERE status = 'pending'").fetchone()[0]

    online = False
    if last_seen:
        age = (datetime.now() - datetime.fromisoformat(last_seen["timestamp"])).total_seconds()
        online = age <= OFFLINE_AFTER_S

    # Lockdown lives in the ESP32's RAM. It is ON if the newest lockdown command is
    # lockdown_on, unless the device rebooted (device_online) after receiving it.
    lockdown = False
    lockdown_pending = False
    if last_lockdown_cmd and last_lockdown_cmd["command"] == "lockdown_on":
        lockdown = True
        lockdown_pending = last_lockdown_cmd["status"] == "pending"
        if (not lockdown_pending and last_boot
                and last_boot["timestamp"] > (last_lockdown_cmd["done_at"] or "")):
            lockdown = False

    return {
        "device_online": online,
        "last_seen": last_seen["timestamp"] if last_seen else None,
        "door": "open" if last_door and last_door["event_type"] == "door_opened" else "closed",
        "lockdown": lockdown,
        "lockdown_pending": lockdown_pending,
        "entries_today": entries_today,
        "denied_today": denied_today,
        "lockouts_today": lockouts_today,
        "last_entry": last_entry["timestamp"] if last_entry else None,
        "last_entry_user": last_entry["user_name"] if last_entry else None,
        "pending_commands": pending,
    }


# ---------------------------------------------------------------- login
class LoginBody(BaseModel):
    username: str
    password: str


@app.post("/api/login")
def login(body: LoginBody, request: Request):
    ok_user = secrets.compare_digest(body.username, DASH_USER)
    ok_pass = secrets.compare_digest(body.password, DASH_PASS)
    if not (ok_user and ok_pass):
        raise HTTPException(status_code=401, detail="wrong username or password")
    request.session["user"] = body.username
    return {"ok": True, "user": body.username}


@app.post("/api/logout")
def logout(request: Request):
    request.session.clear()
    return {"ok": True}


@app.get("/api/me")
def me(request: Request):
    user = request.session.get("user")
    if not user:
        raise HTTPException(status_code=401, detail="not logged in")
    return {"user": user}


# ---------------------------------------------------------------- device endpoints
class EventIn(BaseModel):
    event_type: str
    card_uid: str | None = None
    user_name: str | None = None
    result: str = "info"
    reason: str | None = None
    failed_attempts: int = 0


@app.post("/api/events", dependencies=[Depends(require_device)])
async def post_event(e: EventIn):
    if e.event_type not in db.EVENT_TYPES:
        raise HTTPException(status_code=400, detail=f"unknown event_type '{e.event_type}'")
    if e.result not in db.RESULTS:
        raise HTTPException(status_code=400, detail=f"unknown result '{e.result}'")
    saved = db.insert_event(e.model_dump())
    # push to every open dashboard instantly
    await hub.broadcast({"type": "event", "event": saved, "stats": compute_stats()})
    return {"ok": True, "id": saved["id"]}


@app.get("/api/commands/next", dependencies=[Depends(require_device)])
async def next_command():
    cmd = db.pop_next_command()
    if cmd is None:
        return {"command": None}
    await hub.broadcast({"type": "stats", "stats": compute_stats()})
    return {"id": cmd["id"], "command": cmd["command"]}


# ---------------------------------------------------------------- dashboard endpoints
@app.get("/api/events", dependencies=[Depends(require_login)])
def get_events(
    limit: int = Query(100, ge=1, le=5000),
    type: str | None = None,
    date_from: str | None = Query(None, alias="from"),
    date_to: str | None = Query(None, alias="to"),
):
    return db.query_events(limit=limit, event_type=type or None, date_from=date_from, date_to=date_to)


@app.get("/api/events/export.csv", dependencies=[Depends(require_login)])
def export_csv():
    rows = db.query_events(limit=None, newest_first=False)
    buf = io.StringIO()
    fields = ["id", "timestamp", "event_type", "card_uid", "user_name", "result", "reason", "failed_attempts"]
    writer = csv.DictWriter(buf, fieldnames=fields)
    writer.writeheader()
    writer.writerows(rows)
    filename = f"door_events_{datetime.now():%Y%m%d_%H%M}.csv"
    return Response(
        buf.getvalue(), media_type="text/csv",
        headers={"Content-Disposition": f'attachment; filename="{filename}"'},
    )


@app.get("/api/stats", dependencies=[Depends(require_login)])
def get_stats():
    return compute_stats()


@app.get("/api/stats/daily", dependencies=[Depends(require_login)])
def get_daily(days: int = Query(7, ge=1, le=90)):
    """Granted vs denied per day (for the bar chart) + denial reasons (for the pie)."""
    start = (datetime.now().date() - timedelta(days=days - 1))
    with db.get_db() as conn:
        rows = conn.execute(
            """SELECT substr(timestamp, 1, 10) AS day, event_type, COUNT(*) AS n
               FROM events
               WHERE event_type IN ('access_granted', 'access_denied') AND timestamp >= ?
               GROUP BY day, event_type""",
            (start.isoformat(),),
        ).fetchall()
        reasons = conn.execute(
            """SELECT COALESCE(reason, 'unknown') AS reason, COUNT(*) AS n
               FROM events WHERE event_type = 'access_denied' AND timestamp >= ?
               GROUP BY reason ORDER BY n DESC""",
            (start.isoformat(),),
        ).fetchall()

    per_day = {(start + timedelta(days=i)).isoformat(): {"granted": 0, "denied": 0} for i in range(days)}
    for r in rows:
        key = "granted" if r["event_type"] == "access_granted" else "denied"
        if r["day"] in per_day:
            per_day[r["day"]][key] = r["n"]
    return {
        "daily": [{"date": d, **v} for d, v in per_day.items()],
        "denial_reasons": [{"reason": r["reason"], "count": r["n"]} for r in reasons],
    }


class CommandIn(BaseModel):
    command: str


@app.post("/api/commands", dependencies=[Depends(require_login)])
async def post_command(body: CommandIn):
    if body.command not in db.COMMANDS:
        raise HTTPException(status_code=400, detail=f"command must be one of {sorted(db.COMMANDS)}")
    cmd = db.add_command(body.command)
    await hub.broadcast({"type": "stats", "stats": compute_stats()})
    return cmd


# ---------------------------------------------------------------- WebSocket
@app.websocket("/ws")
async def ws_endpoint(ws: WebSocket):
    # the same session cookie protects the live feed
    if not ws.session.get("user"):
        await ws.close(code=4401)
        return
    await ws.accept()
    hub.clients.add(ws)
    try:
        await ws.send_json({"type": "stats", "stats": compute_stats()})
        while True:
            await ws.receive_text()  # we don't expect messages; this just keeps it open
    except WebSocketDisconnect:
        pass
    finally:
        hub.clients.discard(ws)


# ---------------------------------------------------------------- frontend (built React app)
if os.path.isdir(FRONTEND_DIST):
    app.mount("/", StaticFiles(directory=FRONTEND_DIST, html=True), name="frontend")
else:
    @app.get("/", response_class=HTMLResponse)
    def no_frontend():
        return ("<h2>Backend is running.</h2><p>Frontend not built yet: run "
                "<code>npm install && npm run build</code> inside <code>frontend/</code>.</p>")
