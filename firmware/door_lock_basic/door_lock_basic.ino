// =====================================================================
//  ESP32 RFID + PIN door lock  -  hardware + Blynk email (Gmail) alerts
//
//  - Scan authorised RFID card, then enter 4-digit PIN within 20 s
//    (# = submit, * = clear)
//  - Granted : beep, servo opens 5 s then closes, Blynk "door_opened" email
//  - Denied  : red LED + long beep, Blynk "unauthorized_access" email
//  - 3 failures -> alarm + 30 s lockout
//
//  Blynk console: create events with codes "door_opened" and
//  "unauthorized_access" and tick "Send email" in each event's notifications.
// =====================================================================

// ---- Blynk (copy from Blynk console -> Device Info) ----
#define BLYNK_TEMPLATE_ID   "TMPLxxxxxxxx"
#define BLYNK_TEMPLATE_NAME "New Template"
#define BLYNK_AUTH_TOKEN    "your-blynk-auth-token"

#define BLYNK_PRINT Serial

#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Keypad.h>
#include <ESP32Servo.h>   // Library Manager: "ESP32Servo" by Kevin Harrington

// ---- WiFi ----
char ssid[] = "your-hotspot-name";
char pass[] = "your-hotspot-password";

// ---------------- RFID ----------------
#define SS_PIN 5
#define RST_PIN 4

MFRC522 rfid(SS_PIN, RST_PIN);

byte authorizedUID[] = {0x62, 0x0D, 0x9E, 0x5C};
const byte authorizedUIDLength = 4;

// ---------------- Outputs ----------------
#define BUZZER_PIN 13
#define RED_LED 22

// ---------------- Servo (MG90S) ----------------
#define SERVO_PIN 21
const int SERVO_CLOSED_ANGLE = 0;
const int SERVO_OPEN_ANGLE   = 90;
const unsigned long DOOR_OPEN_TIME = 5000;   // door stays open 5 s

Servo doorServo;

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

const unsigned long AUTH_TIMEOUT = 20000;   // 20 s to enter PIN
const unsigned long LOCKOUT_TIME = 30000;   // 30 s lockout

const unsigned long WIFI_RETRY_INTERVAL = 10000;
const unsigned long BLYNK_RETRY_INTERVAL = 10000;

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

// ---------------- Buzzer ----------------
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
    digitalWrite(RED_LED, HIGH);
    beep(300);
    digitalWrite(RED_LED, LOW);
    delay(200);
  }
}

// ---------------- Blynk email notifications ----------------
void sendBlynkEvent(const char* eventCode, String message) {
  if (Blynk.connected()) {
    Blynk.logEvent(eventCode, message);
    Serial.println(String("Blynk event sent: ") + eventCode);
  } else {
    Serial.println("Blynk offline - notification unavailable");
  }
}

// ---------------- Servo ----------------
void openDoor() {
  doorServo.write(SERVO_OPEN_ANGLE);
  doorOpen = true;
  doorOpenedAt = millis();
  Serial.println("DOOR OPENED");
}

// Non-blocking: closes the door after DOOR_OPEN_TIME
void updateDoor() {
  if (doorOpen && millis() - doorOpenedAt >= DOOR_OPEN_TIME) {
    doorServo.write(SERVO_CLOSED_ANGLE);
    doorOpen = false;
    Serial.println("DOOR CLOSED");
  }
}

// ---------------- RFID ----------------
// returns 0 = no card, 1 = authorised card, -1 = unknown card
int checkRFID() {
  if (!rfid.PICC_IsNewCardPresent()) return 0;
  if (!rfid.PICC_ReadCardSerial()) return 0;

  Serial.print("SCANNED RFID UID: ");
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) Serial.print("0");
    Serial.print(rfid.uid.uidByte[i], HEX);
    Serial.print(" ");
  }
  Serial.println();

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

  return valid ? 1 : -1;
}

// ---------------- Access results ----------------
void accessDenied(String reason) {
  Serial.println("========================");
  Serial.println("ACCESS DENIED");
  Serial.println(reason);
  Serial.println("========================");

  digitalWrite(RED_LED, HIGH);

  sendBlynkEvent("unauthorized_access", "Security Alert: " + reason);

  errorBeep();

  failedAttempts++;
  Serial.print("FAILED ATTEMPTS: ");
  Serial.println(failedAttempts);

  cardAuthorized = false;
  enteredPIN = "";

  if (failedAttempts >= MAX_ATTEMPTS) {
    sendBlynkEvent("unauthorized_access", "Security Alert: 3 failed attempts, system locked for 30 s");
    securityAlarm();
    lockedOut = true;
    lockoutStartTime = millis();
    Serial.println("SYSTEM LOCKED FOR 30 SECONDS");
  }

  delay(1500);
  digitalWrite(RED_LED, LOW);
}

void accessGranted() {
  Serial.println("========================");
  Serial.println("ACCESS GRANTED");
  Serial.println("RFID + PIN VERIFIED");
  Serial.println("========================");

  digitalWrite(RED_LED, LOW);

  successBeep();
  openDoor();   // non-blocking: updateDoor() closes it later

  sendBlynkEvent("door_opened", "Door opened (RFID + PIN verified)");

  failedAttempts = 0;
  cardAuthorized = false;
  enteredPIN = "";
}

// ---------------- Connectivity ----------------
// Keeps WiFi + Blynk alive without ever blocking the lock
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
  pinMode(RED_LED, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(RED_LED, LOW);

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
    Serial.println("WiFi not connected. Lock still works, retrying in background...");
  }

  Serial.println("--------------------------");
  Serial.println("SYSTEM READY");
  Serial.println("SCAN YOUR RFID CARD");
}

// ---------------- Loop ----------------
void loop() {
  maintainConnections();
  updateDoor();

  if (lockedOut) {
    if (millis() - lockoutStartTime >= LOCKOUT_TIME) {
      lockedOut = false;
      failedAttempts = 0;
      Serial.println("LOCKOUT FINISHED");
      Serial.println("SCAN RFID CARD AGAIN");
    }
    return;
  }

  if (!cardAuthorized && !doorOpen) {
    int result = checkRFID();

    if (result == 1) {
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
      accessDenied("Unauthorized RFID Card");
    }
  }

  if (cardAuthorized) {
    if (millis() - cardAuthTime >= AUTH_TIMEOUT) {
      accessDenied("PIN Entry Timeout");
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
          accessGranted();
        } else {
          accessDenied("Incorrect PIN");
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
