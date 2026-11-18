// =====================================================================
//  HARDWARE TEST - no WiFi, no Blynk, no server.
//  Open Serial Monitor at 115200 baud.
//
//  On boot: green LED, red LED, buzzer and servo are tested one by one.
//  Then, live:
//    - Scan any RFID card  -> UID printed, green LED + beep
//    - Press keypad keys   -> key printed, short beep
//        'A' = open servo (90)   'B' = close servo (0)
//        'C' = beep test         'D' = LED test
//    - IR sensor           -> prints DETECTED / CLEAR, red LED while detected
// =====================================================================
#include <SPI.h>
#include <MFRC522.h>
#include <Keypad.h>
#include <ESP32Servo.h>

// ---- same pins as the main project ----
#define SS_PIN     5
#define RST_PIN    4
#define GREEN_LED  21
#define RED_LED    22
#define BUZZER_PIN 13
#define SERVO_PIN  17
#define IR_PIN     34   // active LOW

const int SERVO_CLOSED = 0;
const int SERVO_OPEN   = 90;

MFRC522 rfid(SS_PIN, RST_PIN);
Servo servo;

const byte ROWS = 4, COLS = 4;
char keys[ROWS][COLS] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};
byte rowPins[ROWS] = {15, 14, 27, 26};
byte colPins[COLS] = {25, 33, 32, 16};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

int lastIr = -1;

void beep(int ms) {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(ms);
  digitalWrite(BUZZER_PIN, LOW);
}

void ledTest() {
  Serial.println("[LED] green ON");
  digitalWrite(GREEN_LED, HIGH); delay(700); digitalWrite(GREEN_LED, LOW);
  Serial.println("[LED] red ON");
  digitalWrite(RED_LED, HIGH);   delay(700); digitalWrite(RED_LED, LOW);
}

void servoTest() {
  Serial.println("[SERVO] open (90)");
  servo.write(SERVO_OPEN);   delay(1000);
  Serial.println("[SERVO] close (0)");
  servo.write(SERVO_CLOSED); delay(1000);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n===== ESP32 DOOR LOCK HARDWARE TEST =====");

  pinMode(GREEN_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(IR_PIN, INPUT);   // GPIO34 has no internal pull-up

  servo.setPeriodHertz(50);
  servo.attach(SERVO_PIN, 500, 2400);   // MG90S pulse range
  servo.write(SERVO_CLOSED);

  // ---- RFID: check the reader answers on SPI ----
  SPI.begin(18, 19, 23, SS_PIN);
  rfid.PCD_Init();
  delay(50);
  byte v = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  Serial.print("[RFID] firmware version: 0x");
  Serial.println(v, HEX);
  if (v == 0x00 || v == 0xFF) {
    Serial.println("[RFID] NOT FOUND - check wiring (3.3V, GND, SDA=5, SCK=18, MOSI=23, MISO=19, RST=4)");
  } else {
    Serial.println("[RFID] OK (0x91 or 0x92 = genuine, 0xB2/0x12 = clone, both fine)");
  }

  ledTest();
  Serial.println("[BUZZER] beep");
  beep(300);
  servoTest();

  Serial.println("----------------------------------------");
  Serial.println("Live test: scan a card, press keys, wave at IR sensor");
  Serial.println("Keys: A=servo open  B=servo close  C=beep  D=LED test");
  Serial.println("----------------------------------------");
}

void loop() {
  // ---- RFID ----
  if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
    Serial.print("[RFID] card UID: ");
    for (byte i = 0; i < rfid.uid.size; i++) {
      if (rfid.uid.uidByte[i] < 0x10) Serial.print("0");
      Serial.print(rfid.uid.uidByte[i], HEX);
      Serial.print(" ");
    }
    Serial.println();
    digitalWrite(GREEN_LED, HIGH);
    beep(100);
    delay(300);
    digitalWrite(GREEN_LED, LOW);
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
  }

  // ---- Keypad ----
  char key = keypad.getKey();
  if (key) {
    Serial.print("[KEYPAD] pressed: ");
    Serial.println(key);
    beep(50);
    if (key == 'A') { Serial.println("[SERVO] open (90)");  servo.write(SERVO_OPEN); }
    if (key == 'B') { Serial.println("[SERVO] close (0)");  servo.write(SERVO_CLOSED); }
    if (key == 'C') { Serial.println("[BUZZER] beep");      beep(500); }
    if (key == 'D') { ledTest(); }
  }

  // ---- IR sensor (print only when it changes) ----
  int ir = digitalRead(IR_PIN);
  if (ir != lastIr) {
    lastIr = ir;
    if (ir == LOW) {
      Serial.println("[IR] DETECTED (object in front)");
      digitalWrite(RED_LED, HIGH);
    } else {
      Serial.println("[IR] clear");
      digitalWrite(RED_LED, LOW);
    }
  }
}
