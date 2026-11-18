// secrets.example.h  ->  copy to secrets.h (same folder) and fill in real values.
// secrets.h is in .gitignore so passwords/tokens never get pushed to GitHub.
#pragma once

// ---- Blynk (copy from Blynk console -> Device Info) ----
#define BLYNK_TEMPLATE_ID   "TMPLxxxxxxxx"
#define BLYNK_TEMPLATE_NAME "New Template"
#define BLYNK_AUTH_TOKEN    "your-blynk-auth-token"

// ---- WiFi (the hotspot both the laptop and ESP32 join) ----
#define WIFI_SSID     "your-hotspot-name"
#define WIFI_PASSWORD "your-hotspot-password"

// ---- Dashboard server (your laptop) ----
// Laptop's local IP on the hotspot + port 8000. No trailing slash. Plain http.
#define SERVER_URL     "http://192.168.43.100:8000"
// Must equal DEVICE_API_KEY in backend/.env
#define SERVER_API_KEY "change-me-device-key"
