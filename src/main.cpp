#include "Arduino.h"
#include "ESP32Time.h"
#include "OneButton.h"
#include "Preferences.h"
#include "TFT_eSPI.h"
#include "WiFi.h"
#include "esp_wifi.h"
#include <WebServer.h>

#include "apps.h"
#include "lib/compile_time.h"
#include "lib/log.h"
#include "os_config.h"
#include "resources/fonts/InterRegular16.h"
#include "resources/icons.h"
#include "themes.h"

WebServer server(80);

#define USE_DMA_TO_TFT

TFT_eSPI tft = TFT_eSPI();

uint32_t brightness = 200;

hw_timer_t *uiTimer = NULL;
volatile SemaphoreHandle_t timerSemaphore;

enum State {
  Home,
  AppsList,
  InApp,
};

State cState = Home;

Preferences preferences;

ESP32Time rtc(0);

OneButton btn1 = OneButton(PIN_BUTTON_1);
OneButton btn2 = OneButton(PIN_BUTTON_2);

uint32_t batteryStatus;

String wifi_ssid = "";
String wifi_password = "";
bool show_setup = false; // Track if show setup have been triggered
bool apMode = false;     // Track whether we are in AP mode

// NTP Server & Timezone
const char *ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 1 * 3600; // Adjust for your timezone
const int daylightOffset_sec = 1 * 3600;

uint32_t sleepTimer = 0;

void WiFiConnected(WiFiEvent_t event, WiFiEventInfo_t info) { configTime(GMT_OFFSET_SEC, DAY_LIGHT_OFFSET_SEC, NTP_SERVER1); }

void ARDUINO_ISR_ATTR onTimer() { xSemaphoreGiveFromISR(timerSemaphore, NULL); }

void fadeScreen(uint32_t dly, bool reverse) {
  int t = reverse ? brightness : 0;
  int i = reverse ? -1 : 1;

  while (reverse ? t >= 0 : t < brightness) {
    t += i;
    ledcWrite(0, t);
    delay(dly);
  }
}

void refreshPreferences() {
  preferences.begin(PREFS_KEY);
  brightness = preferences.getUInt("brightness", 200);
  wifi_ssid = preferences.getString("wifi_ssid", "");
  wifi_password = preferences.getString("wifi_password", "");
  currentThemeIndex = preferences.getUInt("current_theme", 0);
  show_setup = preferences.getUInt("show_setup", 1);
  preferences.end();
}

void enterSleep() {
  // sleep sequence
  fadeScreen(2, true);
  tft.fillScreen(TFT_BLACK);
  digitalWrite(PIN_POWER_ON, LOW);
  delay(100);
  esp_wifi_stop();
  esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_BUTTON_2, 0); // 1 = High, 0 = Low
  esp_deep_sleep_start();
  // after wakeup
  esp_wifi_start();
}

// Function to serve the Wi-Fi config page
void handleHttpRoot() {
  server.send(200, "text/html",
              "<html><body><h1>T-Display S3 Clock Setup</h1>"
              "<form action='/save' method='POST'>"
              "<h2>Clock General Settings</h2>"
              "Device Name: <input type='text' name='clock_name'><br>"
              "Theme: <input type='text' name='clock_theme'><br>"
              "<h2>Clock Time Settings</h2>"
              "NTP server: <input type='text' name='clock_ntp_server'><br>"
              "<h2>Clock Weather Settings</h2>"
              "OpenWeather API Key: <input type='text' name='clock_openweather_api_key'><br>"
              "OpenWeather City: <input type='text' name='clock_openweather_city'><br>"
              "OpenWeather Zone: <input type='text' name='clock_openweather_zone'><br>"
              "<h2>Clock WIFI Settings</h2>"
              "SSID: <input type='text' name='wifi_ssid'><br>"
              "Password: <input type='password' name='wifi_password'><br>"
              "<br>"
              "<input type='submit' value='Save & Reboot'><br><br>"
              "</form>"
              "<h1>System Defaults</h1>"
              "<input type='button' value='Reset T-Display S3 Clock' onclick='resetWifi()'>"
              "<script>"
              "function resetWifi() {"
              "  if (confirm('Are you sure you want to reset the Wi-Fi settings?')) {"
              "    window.location.href = '/reset';"
              "  }"
              "}"
              "</script>"
              "</body></html>");
}

// Handle form submission
void handleHttpSave() {
  String wifi_ssid = server.arg("wifi_ssid");
  String wifi_password = server.arg("wifi_password");

  if (wifi_ssid.length() > 0 && wifi_password.length() > 0) {
    preferences.begin(PREFS_KEY);
    preferences.putString("wifi_ssid", wifi_ssid);
    preferences.putString("wifi_password", wifi_password);
    preferences.putUInt("show_setup", 0);
    preferences.end();
    server.send(200, "text/html", "<html><body><h2>Settings Saved! Rebooting...</h2></body></html>");
    delay(2000);
    ESP.restart();
  } else {
    server.send(400, "text/html", "<html><body><h2>Error: Missing SSID or Password</h2></body></html>");
  }
}

// Handle the reset request (erase Wi-Fi settings)
void handleReset() {
  preferences.clear(); // Clear stored Wi-Fi credentials
  server.send(200, "text/html", "<html><body><h2>Settings Reset. Rebooting...</h2></body></html>");
  delay(2000);   // Give time for the response to be shown
  ESP.restart(); // Restart the ESP32
}

void switchToHome();
void switchToAppsList();
void switchToApp();
bool connectToWifi();

void setup() {
  pinMode(PIN_POWER_ON, OUTPUT);
  digitalWrite(PIN_POWER_ON, HIGH);

  Serial.begin(9600);
  log(LOG_INFO, "Welcome to qlockOS!");

  refreshPreferences();
  log(LOG_SUCCESS, "Preferences loaded");

  tft.begin();
  log(LOG_SUCCESS, "TFT initiliazed");

  tft.setRotation(3);
  tft.setSwapBytes(true);
  tft.fillScreen(TFT_BLACK);

  ledcSetup(0, 2000, 8);
  ledcAttachPin(PIN_LCD_BL, 0);
  ledcWrite(0, 0);

  btn1.setDebounceTicks(10);
  btn1.setClickTicks(150);
  btn1.setPressTicks(1000);

  btn2.setDebounceTicks(10);
  btn2.setClickTicks(150);
  btn2.setPressTicks(1000);

  btn1.attachClick([]() {
    sleepTimer = 0;
    switch (cState) {
    case Home:
      break;
    case AppsList:
      if (currentAppIndex != 0)
        currentAppIndex--;
      break;
    case InApp:
      apps[currentAppIndex]->buttonTopClick();
      break;
    }
  });

  btn1.attachDoubleClick([]() {
    sleepTimer = 0;
    switch (cState) {
    case Home:
      switchToAppsList();
      break;
    case AppsList:
      switchToApp();
      break;
    case InApp:
      apps[currentAppIndex]->exit();
      switchToAppsList();
      break;
    }
  });

  btn1.attachLongPressStart([]() {
    switch (cState) {
    case Home:
      enterSleep();
      break;
    case AppsList:
      break;
    case InApp:
      apps[currentAppIndex]->buttonTopLongPress();
      break;
    }
  });

  btn2.attachClick([]() {
    sleepTimer = 0;
    switch (cState) {
    case Home:
      break;
    case AppsList:
      if (currentAppIndex != apps.size() - 1)
        currentAppIndex++;
      break;
    case InApp:
      apps[currentAppIndex]->buttonBottomClick();
      break;
    }
  });

  btn2.attachDoubleClick([]() {
    sleepTimer = 0;
    switch (cState) {
    case Home:
      switchToAppsList();
      break;
    case AppsList:
      switchToHome();
      break;
    case InApp:
      apps[currentAppIndex]->exit();
      switchToHome();
      break;
    }
  });

  btn2.attachLongPressStart([]() {
    switch (cState) {
    case Home:
      break;
    case AppsList:
      break;
    case InApp:
      apps[currentAppIndex]->buttonBottomLongPress();
      break;
    }
  });
  log(LOG_SUCCESS, "Hardware buttons initiliazed");

  // hacky workaround for setting rtc to compile time
  if (rtc.getYear() == 1970)
    rtc.setTime((long)UNIX_TIMESTAMP);
  log(LOG_SUCCESS, "RTC time configured");

  timerSemaphore = xSemaphoreCreateBinary();
  uiTimer = timerBegin(0, 80, true);
  timerAttachInterrupt(uiTimer, &onTimer, true);
  timerAlarmWrite(uiTimer, 1000000, true);
  timerAlarmEnable(uiTimer);
  log(LOG_SUCCESS, "Hardware timer configured");

  batteryStatus = constrain(map((analogRead(PIN_BAT_VOLT) * 2 * 3.3 * 1000) / 4096, 3200, 3900, 0, 100), 0, 100);

  initApps();
  log(LOG_SUCCESS, "Apps initiliazed");
  initThemes();
  log(LOG_SUCCESS, "Themes initiliazed");

  WiFi.onEvent(WiFiConnected, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_CONNECTED);

  if (wifi_ssid == "" || wifi_password == "" || show_setup) {
    Serial.println("No Wi-Fi or 'Clock Setup' triggered, Starting AP mode...");

    WiFi.softAP("ESP32_Config", "12345678");
    Serial.println("AP Started. Connect to 'ESP32_Config'");
    apMode = true;

    // Start the web server
    server.on("/", handleHttpRoot);
    server.on("/save", HTTP_POST, handleHttpSave);
    server.on("/reset", HTTP_GET, handleReset);
    server.begin();
  } else {
    Serial.println("Connecting to Wi-Fi...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifi_ssid.c_str(), wifi_password.c_str());

    // Wait for connection
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      delay(500);
      Serial.print(".");
      attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nWi-Fi Connected!");
      Serial.println(WiFi.localIP());
      // Synchronize with NTP
      configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
      struct tm timeinfo;
      if (!getLocalTime(&timeinfo)) {
        Serial.println("Failed to obtain time");
        return;
      }
      // Save to RTC
      // Convert tm to time_t
      time_t now = mktime(&timeinfo);
      // Set timeval struct
      struct timeval tv;
      tv.tv_sec = now;
      tv.tv_usec = 0;
      // Update system time
      settimeofday(&tv, NULL);
      Serial.println("Time synchronized!");
    } else {
      Serial.println("\nFailed to connect.");
    }
  }

  themes[currentThemeIndex]->drawHomeUI(tft, rtc, batteryStatus);
  log(LOG_SUCCESS, "Home UI drawn");
  fadeScreen(2, false);
  log(LOG_SUCCESS, "Screen fade-in completed");

  log(LOG_SUCCESS, "qlockOS setup completed");

  log(LOG_SUCCESS, String("This version of qlockOS was compiled at: " + String(__TIME__) + " " + String(__DATE__)).c_str());
}

void loop() {
  btn1.tick();
  btn2.tick();

  // 1 second timer
  if (xSemaphoreTake(timerSemaphore, 0) == pdTRUE) {
    if (cState != InApp && batteryStatus != 100)
      sleepTimer++;
    if (cState == Home)
      themes[currentThemeIndex]->drawHomeUI(tft, rtc, batteryStatus);
    batteryStatus = constrain(map((analogRead(PIN_BAT_VOLT) * 2 * 3.3 * 1000) / 4096, 3200, 3900, 0, 100), 0, 100);
  }

  if (sleepTimer == 30 && batteryStatus != 100)
    enterSleep();

  switch (cState) {
  case Home:
    break;
  case AppsList:
    drawAppsListUI(tft, batteryStatus);
    break;
  case InApp:
    apps[currentAppIndex]->drawUI(tft);
    break;
  }
  // **Handle Web Requests Only in AP Mode**
  if (apMode) {
    server.handleClient(); // Process HTTP requests in AP mode
  }
}

void switchToHome() {
  refreshPreferences();
  fadeScreen(1, true);
  cState = Home;
  themes[currentThemeIndex]->drawHomeUI(tft, rtc, batteryStatus);
  fadeScreen(1, false);
}

void switchToAppsList() {
  refreshPreferences();
  fadeScreen(1, true);
  cState = AppsList;
  drawAppsListUI(tft, batteryStatus);
  fadeScreen(1, false);
}

void switchToApp() {
  refreshPreferences();
  fadeScreen(1, true);
  cState = InApp;
  tft.fillScreen(TFT_BLACK);
  tft.pushImage(135, 60, 30, 30, (uint16_t *)apps[currentAppIndex]->icon);
  tft.setTextDatum(TC_DATUM);
  tft.loadFont(InterRegular16);
  tft.drawString(apps[currentAppIndex]->name.c_str(), 150, 100, 2);
  tft.unloadFont();
  fadeScreen(1, false);
  delay(250);
  apps[currentAppIndex]->setup();
  tft.fillScreen(TFT_BLACK);
  ledcWrite(0, 0);
  if (!apps[currentAppIndex]->skipFirstRefresh)
    apps[currentAppIndex]->drawUI(tft);
  fadeScreen(1, false);
}
