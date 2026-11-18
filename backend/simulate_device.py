"""
simulate_device.py - pretends to be the ESP32 so you can demo the dashboard without hardware.

It talks to the backend exactly like the real firmware:
  * POST /api/events with the X-API-Key header
  * heartbeat every 30 s
  * polls GET /api/commands/next every 3 s and obeys unlock / lockdown_on / lockdown_off

Usage (backend must be running):
    python simulate_device.py                 # live simulation, runs until Ctrl+C
    python simulate_device.py --fast          # scenarios every ~3 s instead of ~10 s
    python simulate_device.py --seed 7        # first fill the DB with 7 days of fake history
    python simulate_device.py --url http://192.168.1.5:8000 --key mykey

--seed writes old events straight into SQLite (doorlock.db) because the API always
stamps events with the current server time. Seeded rows are only demo data.
"""

import argparse
import os
import random
import time
from datetime import datetime, timedelta

import requests
from dotenv import load_dotenv

load_dotenv(os.path.join(os.path.dirname(__file__), ".env"))

USERS = {"62 0D 9E 5C": "Authorised user"}  # same card/name as the firmware
UNKNOWN_CARDS = ["DE AD BE EF", "11 22 33 44", "9C 0A 5E 21"]


class FakeDevice:
    def __init__(self, url, key, fast):
        self.url = url.rstrip("/")
        self.headers = {"X-API-Key": key}
        self.fast = fast
        self.failed = 0
        self.lockdown = False
        self.last_heartbeat = 0
        self.last_poll = 0

    # ---- networking (same JSON the firmware sends)
    def send(self, event_type, result="info", uid=None, user=None, reason=None):
        body = {"event_type": event_type, "result": result, "card_uid": uid,
                "user_name": user, "reason": reason, "failed_attempts": self.failed}
        try:
            r = requests.post(f"{self.url}/api/events", json=body, headers=self.headers, timeout=1.5)
            r.raise_for_status()
            print(f"  -> {event_type:22} {result:8} {uid or '':12} {user or '':14} {reason or ''}")
        except requests.RequestException as e:
            print(f"  !! failed to send {event_type}: {e}")

    def wait(self, seconds):
        """Sleep, but keep heartbeats and command polling going (like the firmware loop)."""
        end = time.time() + seconds
        while time.time() < end:
            now = time.time()
            if now - self.last_heartbeat >= 30:
                self.last_heartbeat = now
                self.send("heartbeat", reason="ok (lockdown on)" if self.lockdown else "ok")
            if now - self.last_poll >= 3:
                self.last_poll = now
                self.poll_command()
            time.sleep(0.2)

    def poll_command(self):
        try:
            r = requests.get(f"{self.url}/api/commands/next", headers=self.headers, timeout=1.5)
            cmd = r.json().get("command")
        except (requests.RequestException, ValueError):
            return
        if not cmd:
            return
        print(f"<< command received: {cmd}")
        if cmd == "unlock":
            self.send("access_granted", "granted", reason="remote unlock")
            self.open_door(entry=True)
        elif cmd == "lockdown_on":
            self.lockdown = True
        elif cmd == "lockdown_off":
            self.lockdown = False

    # ---- scenarios
    def open_door(self, entry=True):
        self.send("door_opened", "info", reason="servo unlocked")
        self.wait(2)
        if entry:
            self.send("entry_detected", "info", reason="IR movement within 10 s of grant")
        self.wait(3)
        self.send("door_closed", "info", reason="servo locked")

    def denied(self, uid, user, reason):
        self.failed += 1
        self.send("access_denied", "denied", uid, user, reason)
        if self.failed >= 3:
            self.send("lockout_started", "alert", uid, user, "3 failed attempts")
            self.wait(30 if not self.fast else 8)
            self.failed = 0
            self.send("lockout_ended", "info", reason="30 s lockout over")

    def scenario(self):
        uid, user = random.choice(list(USERS.items()))
        roll = random.random()
        if roll < 0.12:
            self.send("unauthorized_movement", "alert", reason="IR movement without authorization")
            return
        if roll < 0.30:                      # someone tries an unknown card
            uid, user = random.choice(UNKNOWN_CARDS), None
        self.send("card_scanned", "info", uid, user, "authorised card" if user else "unknown card")
        if self.lockdown:
            # lockdown rejects every card and doesn't count as a failed attempt (same as firmware)
            self.send("access_denied", "denied", uid, user, "lockdown")
            return
        if not user:
            self.denied(uid, None, "Unauthorized RFID Card")
            return
        self.wait(random.uniform(1, 3))      # typing the PIN
        if roll < 0.45:
            self.denied(uid, user, "Incorrect PIN")
        elif roll < 0.52:
            self.denied(uid, user, "PIN Entry Timeout")
        else:
            self.failed = 0
            self.send("access_granted", "granted", uid, user, "card + PIN")
            self.open_door(entry=random.random() < 0.85)

    def run(self):
        print(f"Simulating ESP32 -> {self.url}  (Ctrl+C to stop)")
        self.send("device_online", "info", reason="boot")
        self.last_heartbeat = time.time()
        while True:
            self.wait(random.uniform(2, 4) if self.fast else random.uniform(8, 15))
            self.scenario()


def seed_history(days):
    """Insert realistic past events directly into SQLite for the 7-day chart."""
    import database as db

    db.init_db()
    count = 0
    today = datetime.now().replace(hour=0, minute=0, second=0, microsecond=0)
    for d in range(days - 1, 0, -1):  # past days only, today stays live
        day = today - timedelta(days=d)
        for _ in range(random.randint(6, 16)):
            t = day + timedelta(hours=random.randint(8, 20), minutes=random.randint(0, 59),
                                seconds=random.randint(0, 59))
            uid, user = random.choice(list(USERS.items()))
            r = random.random()
            if r < 0.65:
                rows = [("card_scanned", "info", uid, user, "authorised card"),
                        ("access_granted", "granted", uid, user, "card + PIN"),
                        ("door_opened", "info", None, None, "servo unlocked"),
                        ("entry_detected", "info", None, None, "IR movement within 10 s of grant"),
                        ("door_closed", "info", None, None, "servo locked")]
            elif r < 0.80:
                bad = random.choice(UNKNOWN_CARDS)
                rows = [("card_scanned", "info", bad, None, "unknown card"),
                        ("access_denied", "denied", bad, None, "Unauthorized RFID Card")]
            elif r < 0.92:
                rows = [("card_scanned", "info", uid, user, "authorised card"),
                        ("access_denied", "denied", uid, user, random.choice(["Incorrect PIN", "Incorrect PIN", "PIN Entry Timeout"]))]
            else:
                rows = [("unauthorized_movement", "alert", None, None, "IR movement without authorization")]
            for i, (et, res, u, name, reason) in enumerate(rows):
                ts = (t + timedelta(seconds=2 * i)).isoformat(timespec="seconds")
                db.insert_event({"event_type": et, "result": res, "card_uid": u, "user_name": name,
                                 "reason": reason, "failed_attempts": 1 if res == "denied" else 0}, timestamp=ts)
                count += 1
    print(f"Seeded {count} historical events over the last {days - 1} days.")


if __name__ == "__main__":
    p = argparse.ArgumentParser(description="Fake ESP32 door lock")
    p.add_argument("--url", default=os.getenv("SIM_SERVER_URL", "http://127.0.0.1:8000"))
    p.add_argument("--key", default=os.getenv("DEVICE_API_KEY", "change-me-device-key"))
    p.add_argument("--fast", action="store_true", help="run scenarios every few seconds")
    p.add_argument("--seed", type=int, metavar="DAYS", help="seed N days of history into the DB first")
    p.add_argument("--seed-only", action="store_true", help="seed and exit")
    a = p.parse_args()
    if a.seed:
        seed_history(a.seed)
        if a.seed_only:
            raise SystemExit
    try:
        FakeDevice(a.url, a.key, a.fast).run()
    except KeyboardInterrupt:
        print("\nstopped")
