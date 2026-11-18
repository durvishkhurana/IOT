"""
database.py - SQLite storage for the smart door lock.

Two tables:
  events   - every event the ESP32 reports (card scans, grants, alerts, heartbeats...)
  commands - commands queued from the dashboard for the ESP32 to pick up
             (unlock / lockdown_on / lockdown_off)

We use Python's built-in sqlite3 module (no ORM) so every query is visible
and easy to explain.
"""

import os
import sqlite3
from contextlib import contextmanager
from datetime import datetime

DB_PATH = os.getenv("DB_PATH", os.path.join(os.path.dirname(__file__), "doorlock.db"))

EVENT_TYPES = {
    "card_scanned", "access_granted", "access_denied",
    "lockout_started", "lockout_ended",
    "door_opened", "door_closed",
    "entry_detected", "unauthorized_movement",
    "device_online", "heartbeat",
}
RESULTS = {"granted", "denied", "alert", "info"}
COMMANDS = {"unlock", "lockdown_on", "lockdown_off"}


def now_iso() -> str:
    """Server time in ISO format, e.g. 2026-11-18T14:05:31 (laptop local time)."""
    return datetime.now().isoformat(timespec="seconds")


@contextmanager
def get_db():
    """Open a connection, commit on success, always close."""
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row  # rows behave like dicts
    try:
        yield conn
        conn.commit()
    finally:
        conn.close()


def init_db():
    with get_db() as db:
        db.execute("""
            CREATE TABLE IF NOT EXISTS events (
                id              INTEGER PRIMARY KEY AUTOINCREMENT,
                timestamp       TEXT NOT NULL,
                event_type      TEXT NOT NULL,
                card_uid        TEXT,
                user_name       TEXT,
                result          TEXT NOT NULL,
                reason          TEXT,
                failed_attempts INTEGER DEFAULT 0
            )""")
        db.execute("CREATE INDEX IF NOT EXISTS idx_events_ts ON events(timestamp)")
        db.execute("""
            CREATE TABLE IF NOT EXISTS commands (
                id         INTEGER PRIMARY KEY AUTOINCREMENT,
                command    TEXT NOT NULL,
                created_at TEXT NOT NULL,
                status     TEXT NOT NULL DEFAULT 'pending',
                done_at    TEXT          -- when the device picked it up
            )""")


def insert_event(e: dict, timestamp: str | None = None) -> dict:
    ts = timestamp or now_iso()
    with get_db() as db:
        cur = db.execute(
            """INSERT INTO events (timestamp, event_type, card_uid, user_name, result, reason, failed_attempts)
               VALUES (?, ?, ?, ?, ?, ?, ?)""",
            (ts, e["event_type"], e.get("card_uid"), e.get("user_name"),
             e["result"], e.get("reason"), e.get("failed_attempts") or 0),
        )
        row = db.execute("SELECT * FROM events WHERE id = ?", (cur.lastrowid,)).fetchone()
    return dict(row)


def query_events(limit=100, event_type=None, date_from=None, date_to=None, newest_first=True):
    sql = "SELECT * FROM events WHERE 1=1"
    args = []
    if event_type:
        sql += " AND event_type = ?"
        args.append(event_type)
    if date_from:
        sql += " AND timestamp >= ?"
        args.append(date_from)
    if date_to:
        # a plain date (YYYY-MM-DD) means "until the end of that day"
        if len(date_to) == 10:
            date_to += "T23:59:59"
        sql += " AND timestamp <= ?"
        args.append(date_to)
    sql += " ORDER BY id DESC" if newest_first else " ORDER BY id ASC"
    if limit:
        sql += " LIMIT ?"
        args.append(limit)
    with get_db() as db:
        return [dict(r) for r in db.execute(sql, args).fetchall()]


def add_command(command: str) -> dict:
    with get_db() as db:
        cur = db.execute(
            "INSERT INTO commands (command, created_at, status) VALUES (?, ?, 'pending')",
            (command, now_iso()),
        )
        return dict(db.execute("SELECT * FROM commands WHERE id = ?", (cur.lastrowid,)).fetchone())


def pop_next_command() -> dict | None:
    """Return the oldest pending command and mark it done (the device has received it)."""
    with get_db() as db:
        row = db.execute(
            "SELECT * FROM commands WHERE status = 'pending' ORDER BY id ASC LIMIT 1"
        ).fetchone()
        if row is None:
            return None
        db.execute("UPDATE commands SET status = 'done', done_at = ? WHERE id = ?",
                   (now_iso(), row["id"]))
        return dict(row)
