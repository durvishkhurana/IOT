// =====================================================================
//  ESP32 RFID + PIN smart door lock  (UEC640 IoT project)
//
//  Original behaviour (unchanged):
//    - RFID card -> 4-digit PIN within 20 s (# submit, * clear)
//    - Granted: green LED, beep, servo opens 5 s then closes
//    - Denied: red LED, buzzer, +1 failure; 3 failures = alarm + 30 s lockout
//    - IR sensor: movement within 10 s of a grant = authorised entry,
//      otherwise unauthorised movement alert
//    - Blynk events "door_opened" and "unauthorized_access" (email)
//
//  NEW for the web dashboard (look for "DASHBOARD" comments):
//    - Every event is POSTed as JSON to the laptop server (/api/events)
//    - Events wait in a small in-memory queue if WiFi/server is down
//    - Heartbeat every 30 s so the dashboard can show online/offline
//    - Polls /api/commands/next every 3 s for remote unlock / lockdown
//    - WiFi, Blynk and server settings moved to secrets.h
// =====================================================================

// secrets.h MUST come first: it defines BLYNK_TEMPLATE_ID etc., which
// the Blynk library needs to see before it is included.
// Copy secrets.example.h -> secrets.h and fill in your values.
#include "secrets.h"

#define BLYNK_PRINT Serial

#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Keypad.h>
#include <ESP32Servo.h>   // Library Manager: "ESP32Servo" by Kevin Harrington
#include <HTTPClient.h>   // DASHBOARD: built into the ESP32 core
#include <ArduinoJson.h>  // DASHBOARD: Library Manager: "ArduinoJson" by Benoit Blanchon (v7)

char ssid[] = WIFI_SSID;
char pass[] = WIFI_PASSWORD;

// ---------------- RFID ----------------
#define SS_PIN 5
#define RST_PIN 4

MFRC522 rfid(SS_PIN, RST_PIN);

byte authorizedUID[] = {0x62, 0x0D, 0x9E, 0x5C};
const byte authorizedUIDLength = 4;
const char* AUTHORIZED_NAME = "Authorised user";   // DASHBOARD: name shown for this card

// ---------------- Outputs ----------------
#define BUZZER_PIN 13
#define GREEN_LED 21
#define RED_LED 22

// ---------------- Servo (MG90S) ----------------
#define SERVO_PIN 17
const int SERVO_CLOSED_ANGLE = 0;
const int SERVO_OPEN_ANGLE   = 90;     // adjust so the paper swings past the sensor
const unsigned long DOOR_OPEN_TIME = 5000;   // how long the "door" stays open

Servo doorServo;

// ---------------- IR sensor ----------------
// FC-51 / TCRT5000 module: OUT pin goes LOW when it detects an object
#define IR_PIN 34
const int IR_ACTIVE_LEVEL = LOW;
const unsigned long IR_DEBOUNCE_TIME = 50;
const unsigned long NOTIFY_COOLDOWN = 10000;     // max one email per 10 s
const unsigned long AUTHORIZED_WINDOW = 10000;   // movement within 10 s of grant = authorized entry

// ---------------- Keypad ----------------
const byte ROWS = 4;
const byte COLS = 4;

char keys[ROWS][COLS] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};

byte rowPins[ROWS] = {15, 14, 27, 26};
byte colPins[COLS] = {25, 33, 32, 16};

Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

const String correctPIN = "1234";

const int MAX_ATTEMPTS = 3;

const unsigned long AUTH_TIMEOUT = 20000;
const unsigned long LOCKOUT_TIME = 30000;

const unsigned long WIFI_RETRY_INTERVAL = 10000;
const unsigned long BLYNK_RETRY_INTERVAL = 10000;

// ---------------- DASHBOARD: timing ----------------
const unsigned long HEARTBEAT_INTERVAL = 30000;  // "I'm alive" every 30 s
const unsigned long COMMAND_POLL_INTERVAL = 3000; // ask server for commands every 3 s
const unsigned long HTTP_TIMEOUT_MS = 1500;      // never wait longer than 1.5 s for the server
const unsigned long SERVER_RETRY_INTERVAL = 5000; // after a failure, wait 5 s before trying again

// ---------------- State ----------------
String enteredPIN = "";

bool cardAuthorized = false;
bool lockedOut = false;

int failedAttempts = 0;

unsigned long cardAuthTime = 0;
unsigned long lockoutStartTime = 0;
unsigned long lastWiFiAttempt = 0;
unsigned long lastBlynkAttempt = 0;

bool doorOpen = false;
unsigned long doorOpenedAt = 0;
unsigned long lastGrantTime = 0;
bool grantedOnce = false;

int lastIrReading = HIGH;
int stableIrState = HIGH;
unsigned long lastIrChange = 0;
unsigned long lastNotifyTime = 0;
bool notifiedOnce = false;

// Function declarations with default arguments (uid/user are optional).
// Declared here explicitly so the Arduino IDE doesn't auto-generate
// conflicting prototypes.
void logEvent(const char* type, const char* result, const String& reason,
              const String& uid = "", const String& user = "");
void accessDenied(String reason, String uid = "", String user = "");
void accessGranted(String reason = "card + PIN", String uid = "", String user = "");

// ---------------- DASHBOARD: state ----------------
String lastCardUID = "";          // UID of the last scanned card, e.g. "62 0D 9E 5C"
bool lockdown = false;            // set remotely; while true every card is rejected
bool serverOk = false;            // did the last request to the server succeed?
unsigned long lastServerFail = 0;
unsigned long lastHeartbeat = 0;
unsigned long lastCommandPoll = 0;

// =====================================================================
//  DASHBOARD: event queue
//  Events are first stored here, then sent one at a time from loop().
//  If WiFi or the server is down they simply wait here (max 20). When the
//  queue is full the oldest event is dropped. This means a network problem
//  can never stop the lock from working.
// =====================================================================
struct DoorEvent {
  char type[24];      // e.g. "access_granted"
  char uid[16];       // e.g. "62 0D 9E 5C" ("" = none)
  char user[24];      // e.g. "Authorised user" ("" = none)
  char result[8];     // granted / denied / alert / info
  char reason[48];    // human readable reason
  int  failed;        // failed attempts counter at that moment
};

const int QUEUE_SIZE = 20;
DoorEvent eventQueue[QUEUE_SIZE];
int queueHead = 0;   // index of the oldest event
int queueCount = 0;  // how many events are waiting

// Add an event to the queue (never blocks, never touches the network).
void logEvent(const char* type, const char* result, const String& reason,
              const String& uid, const String& user) {
  if (queueCount == QUEUE_SIZE) {               // full: drop the oldest
    queueHead = (queueHead + 1) % QUEUE_SIZE;
    queueCount--;
    Serial.println("[dash] queue full, dropped oldest event");
  }
  DoorEvent& e = eventQueue[(queueHead + queueCount) % QUEUE_SIZE];
  strlcpy(e.type, type, sizeof(e.type));
  strlcpy(e.result, result, sizeof(e.result));
  strlcpy(e.reason, reason.c_str(), sizeof(e.reason));
  strlcpy(e.uid, uid.c_str(), sizeof(e.uid));
  strlcpy(e.user, user.c_str(), sizeof(e.user));
  e.failed = failedAttempts;
  queueCount++;
}

// Build the JSON body and POST it to /api/events. Returns true on HTTP 200.
bool postEvent(const DoorEvent& e) {
  JsonDocument doc;
  doc["event_type"] = e.type;
  doc["result"] = e.result;
  doc["reason"] = e.reason;
  doc["failed_attempts"] = e.failed;
  if (e.uid[0])  doc["card_uid"] = e.uid;   else doc["card_uid"] = nullptr;
  if (e.user[0]) doc["user_name"] = e.user; else doc["user_name"] = nullptr;

  String body;
  serializeJson(doc, body);

  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.begin(client, String(SERVER_URL) + "/api/events");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", SERVER_API_KEY);   // device authentication
  int code = http.POST(body);
  http.end();

  if (code != 200) {
    Serial.printf("[dash] POST %s failed (code %d)\n", e.type, code);
  }
  return code == 200;
}

// May we talk to the server right now?
//  - need WiFi
//  - after a failure wait SERVER_RETRY_INTERVAL before trying again
//  - while someone is typing a PIN, only use the network if the server is
//    known to be reachable, so a dead server can't make keypresses lag.
bool networkAllowed() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (!serverOk && millis() - lastServerFail < SERVER_RETRY_INTERVAL) return false;
  if (cardAuthorized && !serverOk) return false;
  return true;
}

void markServer(bool ok) {
  serverOk = ok;
  if (!ok) lastServerFail = millis();
}

// Send at most ONE queued event per loop() pass, so the loop stays fast.
void flushEventQueue() {
  if (queueCount == 0 || !networkAllowed()) return;
  bool ok = postEvent(eventQueue[queueHead]);
  markServer(ok);
  if (ok) {                                   // remove it only if it arrived
    queueHead = (queueHead + 1) % QUEUE_SIZE;
    queueCount--;
  }
}

// Heartbeat every 30 s. Not queued: an old heartbeat is useless.
void sendHeartbeat() {
  if (millis() - lastHeartbeat < HEARTBEAT_INTERVAL || !networkAllowed()) return;
  lastHeartbeat = millis();
  DoorEvent hb = {};
  strlcpy(hb.type, "heartbeat", sizeof(hb.type));
  strlcpy(hb.result, "info", sizeof(hb.result));
  strlcpy(hb.reason, lockdown ? "ok (lockdown on)" : "ok", sizeof(hb.reason));
  hb.failed = failedAttempts;
  markServer(postEvent(hb));
}

// =====================================================================
//  Buzzer
// =====================================================================
void beep(int duration) {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(duration);
  digitalWrite(BUZZER_PIN, LOW);
}

void successBeep() {
  beep(150);
  delay(100);
  beep(150);
}

void errorBeep() {
  beep(1000);
}

void securityAlarm() {
  Serial.println("SECURITY LOCKOUT ACTIVATED");

  for (int i = 0; i < 5; i++) {
    beep(300);
    delay(200);
  }
}

// ---------------- Notifications (Blynk, unchanged) ----------------
void sendBlynkEvent(const char* eventCode, String message) {
  if (Blynk.connected()) {
    Blynk.logEvent(eventCode, message);
    Serial.println(String("Blynk event sent: ") + eventCode);
  } else {
    Serial.println("Blynk offline - notification unavailable");
  }
}

void sendUnauthorizedNotification(String reason) {
  Serial.println("UNAUTHORIZED ACCESS: " + reason);
  sendBlynkEvent("unauthorized_access", "Security Alert: " + reason);
}

// ---------------- Servo ----------------
void openDoor() {
  doorServo.write(SERVO_OPEN_ANGLE);
  doorOpen = true;
  doorOpenedAt = millis();
  Serial.println("DOOR OPENED");
  logEvent("door_opened", "info", "servo unlocked");          // DASHBOARD
}

void updateDoor() {
  if (doorOpen && millis() - doorOpenedAt >= DOOR_OPEN_TIME) {
    doorServo.write(SERVO_CLOSED_ANGLE);
    doorOpen = false;
    digitalWrite(GREEN_LED, LOW);
    Serial.println("DOOR CLOSED");
    logEvent("door_closed", "info", "servo locked");          // DASHBOARD
  }
}

// ---------------- IR sensor ----------------
void checkMotionSensor() {
  int reading = digitalRead(IR_PIN);

  if (reading != lastIrReading) {
    lastIrChange = millis();
    lastIrReading = reading;
  }

  if (millis() - lastIrChange < IR_DEBOUNCE_TIME) {
    return;
  }

  if (reading == stableIrState) {
    return;
  }

  stableIrState = reading;

  // Only act when the paper ARRIVES in front of the sensor
  if (stableIrState != IR_ACTIVE_LEVEL) {
    return;
  }

  Serial.println("MOVEMENT DETECTED BY IR SENSOR");

  if (notifiedOnce && millis() - lastNotifyTime < NOTIFY_COOLDOWN) {
    Serial.println("Notification skipped (cooldown)");
    return;
  }

  lastNotifyTime = millis();
  notifiedOnce = true;

  bool authorizedEntry = grantedOnce && (millis() - lastGrantTime <= AUTHORIZED_WINDOW);

  if (authorizedEntry) {
    sendBlynkEvent("door_opened", "Someone has entered the house (RFID + PIN verified)");
    logEvent("entry_detected", "info", "IR movement within 10 s of grant");          // DASHBOARD
  } else {
    sendUnauthorizedNotification("Door movement detected without authorization");
    logEvent("unauthorized_movement", "alert", "IR movement without authorization"); // DASHBOARD
  }
}

// ---------------- RFID ----------------
int checkRFID() {
  if (!rfid.PICC_IsNewCardPresent()) {
    return 0;
  }

  if (!rfid.PICC_ReadCardSerial()) {
    return 0;
  }

  // DASHBOARD: build the UID as a hex string like "62 0D 9E 5C"
  lastCardUID = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) lastCardUID += "0";
    lastCardUID += String(rfid.uid.uidByte[i], HEX);
    if (i < rfid.uid.size - 1) lastCardUID += " ";
  }
  lastCardUID.toUpperCase();

  Serial.print("SCANNED RFID UID: ");
  Serial.println(lastCardUID);

  bool valid = (rfid.uid.size == authorizedUIDLength);

  if (valid) {
    for (byte i = 0; i < authorizedUIDLength; i++) {
      if (rfid.uid.uidByte[i] != authorizedUID[i]) {
        valid = false;
        break;
      }
    }
  }

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  // DASHBOARD: every scan is logged, known or not
  logEvent("card_scanned", "info", valid ? "authorised card" : "unknown card",
           lastCardUID, valid ? AUTHORIZED_NAME : "");

  return valid ? 1 : -1;
}

// ---------------- Access results ----------------
// uid/user are only for the dashboard log.
void accessDenied(String reason, String uid, String user) {
  Serial.println("========================");
  Serial.println("ACCESS DENIED");
  Serial.println(reason);
  Serial.println("========================");

  digitalWrite(GREEN_LED, LOW);
  digitalWrite(RED_LED, HIGH);

  sendUnauthorizedNotification(reason);

  errorBeep();

  failedAttempts++;

  Serial.print("FAILED ATTEMPTS: ");
  Serial.println(failedAttempts);

  logEvent("access_denied", "denied", reason, uid, user);          // DASHBOARD

  cardAuthorized = false;
  enteredPIN = "";

  if (failedAttempts >= MAX_ATTEMPTS) {
    logEvent("lockout_started", "alert", "3 failed attempts", uid, user);  // DASHBOARD
    securityAlarm();
    lockedOut = true;
    lockoutStartTime = millis();
    Serial.println("SYSTEM LOCKED FOR 30 SECONDS");
  }

  delay(1500);
  digitalWrite(RED_LED, LOW);
}

// DASHBOARD: lockdown rejection. The card is refused with the same red LED +
// error beep, but it does NOT count as a failed attempt (the user did nothing
// wrong; the admin locked the door) and it does not send a Blynk email.
void lockdownReject(String uid, String user) {
  Serial.println("ACCESS DENIED - LOCKDOWN ACTIVE");
  digitalWrite(GREEN_LED, LOW);
  digitalWrite(RED_LED, HIGH);
  logEvent("access_denied", "denied", "lockdown", uid, user);
  errorBeep();
  delay(500);
  digitalWrite(RED_LED, LOW);
}

// reason/uid/user are for the dashboard log ("card + PIN" or "remote unlock").
void accessGranted(String reason, String uid, String user) {
  Serial.println("========================");
  Serial.println("ACCESS GRANTED");
  Serial.println(reason);
  Serial.println("========================");

  digitalWrite(RED_LED, LOW);
  digitalWrite(GREEN_LED, HIGH);

  // Start the authorized window BEFORE the servo moves,
  // so the paper passing the sensor counts as a valid entry
  lastGrantTime = millis();
  grantedOnce = true;

  failedAttempts = 0;
  logEvent("access_granted", "granted", reason, uid, user);  // DASHBOARD

  successBeep();
  openDoor();   // non-blocking: updateDoor() closes it later

  cardAuthorized = false;
  enteredPIN = "";
}

// =====================================================================
//  DASHBOARD: remote commands
//  Every 3 s ask the server "is there a command for me?".
//  Server replies {"command": null} or {"id": 5, "command": "unlock"}.
// =====================================================================
void pollCommands() {
  if (millis() - lastCommandPoll < COMMAND_POLL_INTERVAL || !networkAllowed()) return;
  lastCommandPoll = millis();

  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.begin(client, String(SERVER_URL) + "/api/commands/next");
  http.addHeader("X-API-Key", SERVER_API_KEY);
  int code = http.GET();
  String payload = (code == 200) ? http.getString() : "";
  http.end();

  markServer(code == 200);
  if (code != 200) return;

  JsonDocument doc;
  if (deserializeJson(doc, payload)) return;   // bad JSON -> ignore
  const char* cmd = doc["command"];            // nullptr when there is no command
  if (!cmd) return;

  Serial.print("[dash] command received: ");
  Serial.println(cmd);

  if (strcmp(cmd, "unlock") == 0) {
    // Same routine as a valid card + PIN: green LED, beep, servo 5 s,
    // and starts the 10 s authorised-entry window for the IR sensor.
    if (!doorOpen) accessGranted("remote unlock");
  } else if (strcmp(cmd, "lockdown_on") == 0) {
    lockdown = true;
    cardAuthorized = false;   // cancel any PIN entry in progress
    enteredPIN = "";
    Serial.println("LOCKDOWN ON - all cards rejected");
  } else if (strcmp(cmd, "lockdown_off") == 0) {
    lockdown = false;
    Serial.println("LOCKDOWN OFF");
  }
}

// ---------------- Connectivity ----------------
void maintainConnections() {
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastWiFiAttempt >= WIFI_RETRY_INTERVAL) {
      lastWiFiAttempt = millis();
      Serial.println("Reconnecting WiFi...");
      WiFi.reconnect();
    }
    return;
  }

  if (!Blynk.connected()) {
    if (millis() - lastBlynkAttempt >= BLYNK_RETRY_INTERVAL) {
      lastBlynkAttempt = millis();
      Serial.println("Connecting to Blynk...");
      Blynk.connect(1000);
    }
  } else {
    Blynk.run();
  }
}

// ---------------- Setup ----------------
void setup() {
  Serial.begin(115200);

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(GREEN_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);
  pinMode(IR_PIN, INPUT);   // GPIO34 has no internal pull-up; the IR module drives the line

  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(GREEN_LED, LOW);
  digitalWrite(RED_LED, LOW);

  lastIrReading = digitalRead(IR_PIN);
  stableIrState = lastIrReading;

  doorServo.setPeriodHertz(50);
  doorServo.attach(SERVO_PIN, 500, 2400);   // MG90S pulse range
  doorServo.write(SERVO_CLOSED_ANGLE);

  SPI.begin(18, 19, 23, SS_PIN);
  rfid.PCD_Init();

  Serial.println();
  Serial.println("ESP32 RFID SECURITY SYSTEM");
  Serial.println("--------------------------");

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);

  Blynk.config(BLYNK_AUTH_TOKEN);

  Serial.println("Connecting to WiFi...");

  if (WiFi.waitForConnectResult(10000) == WL_CONNECTED) {
    Serial.println("WiFi Connected");
    Serial.println(WiFi.localIP());
    Blynk.connect(3000);
  } else {
    Serial.println("WiFi not connected. Retrying...");
  }

  // DASHBOARD: announce boot. It sits in the queue until the server is reachable.
  logEvent("device_online", "info", "boot");
  lastHeartbeat = millis();
  Serial.print("Dashboard server: ");
  Serial.println(SERVER_URL);

  Serial.println("--------------------------");
  Serial.println("SYSTEM READY");
  Serial.println("AUTHORIZED UID: 62 0D 9E 5C");
  Serial.println("SCAN YOUR RFID CARD");
}

// ---------------- Loop ----------------
void loop() {
  maintainConnections();

  // DASHBOARD: network jobs. Each is skipped instantly if it's not time yet
  // or the network is down, so the lock logic below always keeps running.
  flushEventQueue();
  sendHeartbeat();
  pollCommands();     // placed before the lockout check so remote commands work during lockout

  // These run all the time, even during lockout,
  // so forced movement is still reported
  updateDoor();
  checkMotionSensor();

  if (lockedOut) {
    if (millis() - lockoutStartTime >= LOCKOUT_TIME) {
      lockedOut = false;
      failedAttempts = 0;
      Serial.println("LOCKOUT FINISHED");
      Serial.println("SCAN RFID CARD AGAIN");
      logEvent("lockout_ended", "info", "30 s lockout over");   // DASHBOARD
    }
    return;
  }

  if (!cardAuthorized && !doorOpen) {
    int result = checkRFID();

    if (result != 0 && lockdown) {
      // DASHBOARD: lockdown rejects every card, known or unknown
      lockdownReject(lastCardUID, result == 1 ? AUTHORIZED_NAME : "");
    }
    else if (result == 1) {
      Serial.println("RFID ACCEPTED");

      cardAuthorized = true;
      cardAuthTime = millis();
      enteredPIN = "";

      beep(100);

      Serial.println("ENTER 4-DIGIT PIN");
      Serial.println("PRESS # TO SUBMIT");
      Serial.println("PRESS * TO CLEAR");
    }
    else if (result == -1) {
      accessDenied("Unauthorized RFID Card", lastCardUID);
    }
  }

  if (cardAuthorized) {
    if (millis() - cardAuthTime >= AUTH_TIMEOUT) {
      accessDenied("PIN Entry Timeout", lastCardUID, AUTHORIZED_NAME);
      return;
    }

    char key = keypad.getKey();

    if (key) {
      beep(50);

      if (key == '*') {
        enteredPIN = "";
        Serial.println();
        Serial.println("PIN CLEARED");
      }
      else if (key == '#') {
        Serial.println();
        Serial.println("CHECKING PIN...");

        if (enteredPIN.length() == 4 && enteredPIN == correctPIN) {
          accessGranted("card + PIN", lastCardUID, AUTHORIZED_NAME);
        } else {
          accessDenied("Incorrect PIN", lastCardUID, AUTHORIZED_NAME);
        }
      }
      else if (key >= '0' && key <= '9') {
        if (enteredPIN.length() < 4) {
          enteredPIN += key;
          Serial.print("*");
        }
      }
    }
  }
}
