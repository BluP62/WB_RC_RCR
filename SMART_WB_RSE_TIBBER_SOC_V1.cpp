// ---------------------------------------------------
// Diese Programm "ersetzt" die physische Verkabelung 
// zwischen einem Rundsteuerempfänger Relais (NO) 
// und den beiden RSE Pins in der Smart-WB
// Ausgaben erfolgen per OLED (muss angeschlossen sein)
// und einer Website
// ---------------------------------------------------
#define VERSION "2.0"
// SH110X-Pin,   ESP32-Pin,  Beschreibung
// VCC,          3.3V,       Spannungsversorgung
// GND,          GND,        Masse
// SCL,          GPIO 22,    I2C-Takt (kann auch GPIO 21 sein)
// SDA,          GPIO 21,    I2C-Daten (kann auch GPIO 22 sein)

//Steuerungsparameter für den OLED Type
#define OLED_TYPE_SH110X
//#define OLED_TYPE_SSD1306
#define WDT_TIMEOUT_SECONDS 10 // Standard-Timeout
#include <secrets.h>
#include <esp_task_wdt.h>
#include <esp_system.h> 
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include "driver/ledc.h"
#include <time.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#ifdef OLED_TYPE_SSD1306
  #include <Adafruit_SSD1306.h>
#else
  #include <Adafruit_SH110X.h>
#endif
// ---------------------------------------------------
// ------------- LED Betriebsarten BEGIN -------------
// ---------------------------------------------------
enum LedMode {
  LEDMODE_OFF,
  LEDMODE_ON,
  LEDMODE_FADE,
  LEDMODE_BLINK,
  LEDMODE_FLASH
};

struct LedController {
  int pin = -1;
  ledc_channel_t channel;
  ledc_timer_t timer;

  LedMode mode = LEDMODE_OFF;
  int maxBrightness = 255;  // 0..255

  // Fade
  int fadeStep = 5;
  int fadeDelay = 30;
  int brightness = 0;
  int direction = 1;
  unsigned long lastFadeUpdate = 0;

  // Blink
  int blinkInterval = 250;
  bool blinkState = false;
  unsigned long lastBlinkUpdate = 0;

  // Flash
  int flashOn = 100;
  int flashOff = 3000;
  bool flashState = false;
  unsigned long lastFlashUpdate = 0;

  void begin(int _pin, ledc_channel_t _channel, ledc_timer_t _timer,
             uint32_t freq = 5000, ledc_timer_bit_t resolution = LEDC_TIMER_8_BIT) {
    pin = _pin;
    channel = _channel;
    timer = _timer;

    // Timer konfigurieren
    ledc_timer_config_t ledc_timer = {
      .speed_mode       = LEDC_HIGH_SPEED_MODE,
      .duty_resolution  = resolution,
      .timer_num        = timer,
      .freq_hz          = freq,
      .clk_cfg          = LEDC_AUTO_CLK
    };
    ledc_timer_config(&ledc_timer);

    // Kanal konfigurieren
    ledc_channel_config_t ledc_channel = {
      .gpio_num   = pin,
      .speed_mode = LEDC_HIGH_SPEED_MODE,
      .channel    = channel,
      .intr_type  = LEDC_INTR_DISABLE,
      .timer_sel  = timer,
      .duty       = 0,
      .hpoint     = 0
    };
    ledc_channel_config(&ledc_channel);

    // Startzustand
    setDuty(0);
    mode = LEDMODE_OFF;
  }

  void setMode(LedMode m) { mode = m; }

  void setDuty(int duty) {
    ledc_set_duty(LEDC_HIGH_SPEED_MODE, channel, duty);
    ledc_update_duty(LEDC_HIGH_SPEED_MODE, channel);
  }

  void update(unsigned long now) {
    switch (mode) {
      case LEDMODE_OFF:
        setDuty(0);
        break;

      case LEDMODE_ON:
        setDuty(maxBrightness);
        break;

      case LEDMODE_FADE:
        if (now - lastFadeUpdate >= (unsigned long)fadeDelay) {
          lastFadeUpdate = now;
          brightness += direction * fadeStep;
          if (brightness >= maxBrightness) {
            brightness = maxBrightness;
            direction = -1;
          } else if (brightness <= 0) {
            brightness = 0;
            direction = 1;
          }
          setDuty(brightness);
        }
        break;

      case LEDMODE_BLINK:
        if (now - lastBlinkUpdate >= (unsigned long)blinkInterval) {
          lastBlinkUpdate = now;
          blinkState = !blinkState;
          setDuty(blinkState ? maxBrightness : 0);
        }
        break;

      case LEDMODE_FLASH:
        if (flashState) {
          if (now - lastFlashUpdate >= (unsigned long)flashOn) {
            lastFlashUpdate = now;
            flashState = false;
            setDuty(0);
          }
        } else {
          if (now - lastFlashUpdate >= (unsigned long)flashOff) {
            lastFlashUpdate = now;
            flashState = true;
            setDuty(maxBrightness);
          }
        }
        break;
    }
  }
};

// ---------------- Pinbelegung ----------------
// Ausgänge
const int LED1_PIN = 17;  // LED1: SmartWB Grün
const int LED2_PIN = 23;  // LED2: RSE aktiv Rot
const int LED3_PIN = 5;   // LED3: Watchdog Blau
// Eingänge
const int RSE        = 16; // Ereignis2: RSE aktiv = LOW

// ---------------- LED Objekte ----------------
LedController led1, led2, led3;
// ---------------------------------------------------
// -------------   LED Betriebsarten END -------------
// ---------------------------------------------------


#define SCREEN_WIDTH 128 // OLED display Breite, in Pixel
#define SCREEN_HEIGHT 64 // OLED display Höhe, in Pixel
#define CHAR_SIZE_X 6    // OLED Textzeichenbreite bei Textgröße 1
#define CHAR_SIZE_Y 8    // OLED Textzeichenhöhe bei Texztgröße 1

// Declaration for an SH1106/SSD1306 display connected to I2C (SDA, SCL pins)
#define OLED_RESET     -1 // Reset pin # (wird oft nicht benutzt)
#ifdef OLED_TYPE_SSD1306
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
#else 
Adafruit_SH1106G display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
#endif

// WLAN Zugangsdaten (bitte anpassen)
// aus secrets.h
const char* ssid     = WIFI_SSID;
const char* password = WIFI_PASSWORD;

// Tibber Login-Daten
// aus secrets.h
const char* tibberEmail = TIBBER_EMAIL;
const char* tibberPass  = TIBBER_PASSWORD;

// Tibber API Endpunkte
const char* loginUrl = "https://app.tibber.com/login.credentials";
const char* gqlUrl   = "https://app.tibber.com/v4/gql";

// GraphQL Query
const char* gqlQuery = R"(
{
  me {
    homes {
      electricVehicles {
        name
        battery { percent }
      }
    }
  }
}
)";


// IP-Adresse und URLs für lokale Requests
const char* urlOn    = "http://192.168.178.54/cm?cmnd=Power%20On";
const char* urlOff   = "http://192.168.178.54/cm?cmnd=Power%20Off";
const char* urlParam = "http://192.168.178.30/getParameters";

// Blinker-Variablen
unsigned long letzteUmschaltung = 0;
unsigned long aktuelleUmschaltung = 0;
bool rotStatus = false;
const unsigned long BLINK_INTERVAL = 250; //ms

// Uhrzeit-Anzeige Variable
unsigned long letzteUhrAnzeige = 0;
unsigned long aktuelleUhrAnzeige = 0;
const unsigned long UHR_ANZEIGE_INTERVAL = 1000; //ms
int currentProgress = 0; // für Fortschrittsbalken

// SMartWB-Anzeige Variable
unsigned long letzteSmartWBAnzeige = 0;
unsigned long aktuelleSmartWBAnzeige = 0;
const unsigned long SMARTWB_ANZEIGE_INTERVAL = 10000; //ms
unsigned long letzteUIAnzeige = 0;
unsigned long aktuelleUIAnzeige = 0;

// tibber SoC vom Fahrzeug Anzeige Variablen
unsigned long letzteSocAnzeige = 0;
unsigned long aktuelleSocAnzeige = 0;
const unsigned long SOC_ANZEIGE_INTERVAL = 120000; //ms -> alle 2min
int soc = -1;

//SmartWB Parameter die wir anzeigen möchten
int  vehicleState = 1;
bool    evseState = false;
int    maxCurrent = 16;
int actualCurrent = 6;
float actualPower = 0.0;
float   currentP1 = 0.0;
float   currentP2 = 0.0;
float   currentP3 = 0.0;
float   voltageP1 = 0.0;
float   voltageP2 = 0.0;
float   voltageP3 = 0.0;


// Zustandsvariablen
volatile bool RSEAktiv = (digitalRead(RSE) == LOW);
bool letzterRSEStatus        = !RSEAktiv;  // für Flankenerkennung
bool letzterRSEStatusSmartWB = !RSEAktiv;  //damit das Auslesen der SmartWBParameters beim RSE Flankenwechsel erzwungen wird.
int i = 1;                                 //allgemeiner Zähler um die 3 Spannungen und Ströme nacheinander anzeigen

// WebServer auf Port 80
WebServer server(80);
          

/********************* Allgemeine Funktionen ********************/

/*****************************************************************
* @brief Interrupt-Service-Routine (ISR) für RSE
* @param -
******************************************************************/
void IRAM_ATTR isrRSE() {
  RSEAktiv = (digitalRead(RSE) == LOW);
}

/*****************************************************************
* @brief Zeitstempel für Serial- und Display Ausgabe
* @param buffer wird zurückgegben,: sollte die aktuelle Zeit oder "Keine Zeit" sein
******************************************************************/
String getZeitstempel() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "[Keine Zeit]";
  }
  char buffer[30];
  strftime(buffer, sizeof(buffer), "[%Y-%m-%d %H:%M:%S]", &timeinfo);
  return String(buffer);
}

/*****************************************************************
* @brief SmartWB JSON auslesen & Werte zuweisen
* @param httpCode wird zurückgegeben
******************************************************************/
void getSmartWBParameters(int& httpCode) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(urlParam);
    httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();

      StaticJsonDocument<1024> doc;
      DeserializationError error = deserializeJson(doc, payload);

      if (!error) {
        JsonObject obj = doc["list"][0];
        
        vehicleState  = obj["vehicleState"];
        evseState     = obj["evseState"];
        maxCurrent    = obj["maxCurrent"];
        actualCurrent = obj["actualCurrent"];
        actualPower   = obj["actualPower"];
        currentP1     = obj["currentP1"];
        currentP2     = obj["currentP2"];
        currentP3     = obj["currentP3"];
        voltageP1     = obj["voltageP1"];
        voltageP2     = obj["voltageP2"];
        voltageP3     = obj["voltageP3"];

        // Ausgabe
        Serial.println(getZeitstempel());
        Serial.println("------ Parameter aktualisiert ------");
        Serial.print("maxCurrent: ");
        Serial.println(maxCurrent);
        Serial.print("actualCurrent: ");
        Serial.println(actualCurrent);
        Serial.print("actualPower: ");
        Serial.println(actualPower);
        Serial.println("-----------------------------------");
      } else {
        Serial.print(getZeitstempel());
        Serial.print(" JSON Fehler: ");
        Serial.println(error.c_str());
      }

    } else {
      Serial.print(getZeitstempel());
      Serial.print(" HTTP Fehler: ");
      Serial.println(httpCode);
    }

    http.end();
  } else {
    Serial.print(getZeitstempel());
    Serial.println(" WLAN nicht verbunden!");
  }
}

/*****************************************************************
* @brief Zeichnet einen Fortschrittsbalken auf dem OLED-Display.
* @param progress Der aktuelle Fortschritt in Prozent (0-100).
******************************************************************/
void drawProgressBar(int progress) {
  // Stellen Sie sicher, dass der Wert im Bereich 0-100 liegt
  progress = constrain(progress, 0, 100);

  // Lösche den Bereich des Fortschrittsbalkens, um ihn neu zu zeichnen
  // Zeile 3 liegt bei 24 Pixeln, da jede Textzeile (Größe 1) 8 Pixel hoch ist.
  // Zeile 1: Y=0, Zeile 2: Y=8, Zeile 3: Y=16, Zeile 4: Y=24, etc.
  #ifdef OLED_TYPE_SSD1306
  display.fillRect(0, 16, SCREEN_WIDTH, 8, SSD1306_BLACK);
  #else 
  display.fillRect(0, 16, SCREEN_WIDTH, 8, SH110X_BLACK);
  #endif
  // Berechne die Breite des gefüllten Balkens
  // Da der Balken über die gesamte Breite des Displays (128 Pixel) gehen soll.
  int filledWidth = map(progress, 0, 100, 0, SCREEN_WIDTH - 2); 

  // Zeichne den leeren Rahmen des Fortschrittsbalkens
  #ifdef OLED_TYPE_SSD1306
  display.drawRect(0, 16, SCREEN_WIDTH - 1, 8, SSD1306_WHITE);
  #else 
  display.drawRect(0, 16, SCREEN_WIDTH - 1, 8, SH110X_WHITE);
  #endif

  // Zeichne den gefüllten Teil des Fortschrittsbalkens
  #ifdef OLED_TYPE_SSD1306
  display.fillRect(1, 17, filledWidth, 6, SSD1306_WHITE);
  #else 
  display.fillRect(1, 17, filledWidth, 6, SH110X_WHITE);
  #endif

  // Schreibe den Prozentwert in die Mitte des Balkens
  String progressText = String(progress) + "%";
  int textWidth = progressText.length() * 6; // Jedes Zeichen ist 6 Pixel breit
  int textX = (SCREEN_WIDTH - textWidth) / 2;
  int textY = 16 + 1; // 1 Pixel Abstand vom Rand

  #ifdef OLED_TYPE_SSD1306
  display.setTextColor(SSD1306_BLACK, SSD1306_WHITE); // Schwarzer Text auf weißem Hintergrund
  #else 
  display.setTextColor(SH110X_BLACK, SH110X_WHITE); // Schwarzer Text auf weißem Hintergrund
  #endif

  display.setCursor(textX, textY);
  display.print(progressText);

  // Sende alle Befehle an das Display
  display.display();

  // und wieder zurück mit weiß auf schwarz
  #ifdef OLED_TYPE_SSD1306
  display.setTextColor(SSD1306_WHITE, SSD1306_BLACK); // Weißer Text auf schwarzem Hintergrund
  #else
  display.setTextColor(SH110X_WHITE, SH110X_BLACK); // Weißer Text auf schwarzem Hintergrund
  #endif
}

/*****************************************************************
* @brief Login-Token für tibber holen
* @param loginUrl: Konstante die witer oben definiert wurde, 
* @param token wird zurückgegeben
******************************************************************/
String getTibberToken() {
  HTTPClient http;
  http.begin(loginUrl);
  http.addHeader("Content-Type", "application/json");

  String body = String("{\"email\":\"") + tibberEmail + "\",\"password\":\"" + tibberPass + "\"}";

  int code = http.POST(body);
  String token = "";

  if (code == 200) {
    DynamicJsonDocument doc(1024);
    deserializeJson(doc, http.getString());
    token = doc["token"].as<String>();
  } else {
    Serial.printf("Login fehlgeschlagen, Code: %d\n", code);
  }

  http.end();
  return token;
}

/*****************************************************************
* @brief SoC und Fahrzeug abfragen
* @param token
* @param soc wird zurückgegeben
******************************************************************/
int getSoc(const String& token) {
  HTTPClient http;
  http.begin(gqlUrl);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + token);

  // ✅ Korrektes JSON für GraphQL
  String body = "{\"query\":\"{ me { homes { electricVehicles { name battery { percent } } } } }\"}";

  int code = http.POST(body);
  int soc = -1;

  if (code == 200) {
    DynamicJsonDocument doc(2048);
    if (deserializeJson(doc, http.getString()) == DeserializationError::Ok) {
      soc = doc["data"]["me"]["homes"][0]["electricVehicles"][0]["battery"]["percent"];
    } else {
      Serial.println("❌ Fehler beim JSON-Parsing");
    }
  } else {
    Serial.printf("GraphQL-Fehler (Code: %d)\n", code);
    Serial.println(http.getString()); // 🔍 Ausgabe der Fehlermeldung
  }

  http.end();
  return soc;
}

/*****************************************************************
* @brief HTTP-Handler für die Webserver-Root-Seite
* @param -
******************************************************************/
void handleRoot() {
  String html = "<!DOCTYPE html><html lang='de'><head>";
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<meta http-equiv='refresh' content='5'>"; // Auto-Refresh alle 5 Sekunden
  html += "<title>SmartWB Monitor</title>";
  html += "<style>";
  html += "body { font-family: Arial, sans-serif; background-color: #1a1a1a; color: #ffffff; margin: 20px; }";
  html += ".container { max-width: 600px; margin: 0 auto; background-color: #2a2a2a; padding: 20px; border-radius: 10px; box-shadow: 0 4px 6px rgba(0,0,0,0.3); }";
  html += "h1 { text-align: center; color: #4CAF50; margin-bottom: 20px; }";
  html += ".info-row { display: flex; justify-content: space-between; padding: 8px 0; border-bottom: 1px solid #444; }";
  html += ".label { font-weight: bold; color: #aaa; }";
  html += ".value { color: #fff; }";
  html += ".status-offline { background-color: #f44336; color: white; padding: 2px 8px; border-radius: 3px; }";
  html += ".status-ein { background-color: #4CAF50; color: white; padding: 2px 8px; border-radius: 3px; }";
  html += ".status-aus { background-color: #ff9800; color: white; padding: 2px 8px; border-radius: 3px; }";
  html += ".blink { animation: blink-animation 0.5s steps(2, start) infinite; }";
  html += "@keyframes blink-animation { to { visibility: hidden; } }";
  html += ".section { margin-top: 20px; }";
  html += ".section-title { font-size: 1.2em; color: #4CAF50; margin-bottom: 10px; border-bottom: 2px solid #4CAF50; padding-bottom: 5px; }";
  html += "</style>";
  html += "</head><body>";
  html += "<div class='container'>";
  html += "<h1>SmartWB Monitor " VERSION "</h1>";

  // Datum und Uhrzeit
  html += "<div class='info-row'><span class='label'>Datum/Uhrzeit:</span><span class='value'>" + getZeitstempel() + "</span></div>";

  // IP-Adresse
  html += "<div class='info-row'><span class='label'>IP:</span><span class='value'>" + WiFi.localIP().toString() + "</span></div>";

  // SmartWB Status
  String statusClass = "";
  String statusText = "";
  if (actualPower == 0.0 && actualCurrent == 0 && maxCurrent == 0) {
    statusClass = "status-offline";
    statusText = "OFFLINE";
  } else if (evseState) {
    statusClass = "status-ein";
    statusText = "EIN";
  } else {
    statusClass = "status-aus";
    statusText = "AUS";
  }
  html += "<div class='info-row'><span class='label'>SmartWB:</span><span class='value'><span class='" + statusClass + "'>" + statusText + "</span></span></div>";

  // SOC (nur wenn Fahrzeug angeschlossen)
  if ((vehicleState == 2 || vehicleState == 3) && soc >= 0) {
    html += "<div class='info-row'><span class='label'>SOC:</span><span class='value'>" + String(soc) + "%</span></div>";
  }

  // Stromsection
  html += "<div class='section'>";
  html += "<div class='section-title'>Ladedaten</div>";

  // Max Current
  html += "<div class='info-row'><span class='label'>Max Current:</span><span class='value'>" + String(maxCurrent) + "A</span></div>";

  // Actual Current (mit roter Anzeige wenn RSE aktiv)
  String currentDisplay = "";
  if (RSEAktiv) {
    currentDisplay = "<span style='color: red;'>RCR aktiv</span> " + String(actualCurrent) + "A";
  } else {
    currentDisplay = String(actualCurrent) + "A";
  }
  html += "<div class='info-row'><span class='label'>Actual Current:</span><span class='value'>" + currentDisplay + "</span></div>";

  // Actual Power
  html += "<div class='info-row'><span class='label'>Actual Power:</span><span class='value'>" + String(actualPower, 2) + "kW</span></div>";
  html += "</div>";

  // Spannungen und Ströme
  html += "<div class='section'>";
  html += "<div class='section-title'>Phasen</div>";
  html += "<div class='info-row'><span class='label'>U1:</span><span class='value'>" + String(voltageP1, 1) + "V</span><span class='label' style='margin-left: 20px;'>I1:</span><span class='value'>" + String(currentP1, 1) + "A</span></div>";
  html += "<div class='info-row'><span class='label'>U2:</span><span class='value'>" + String(voltageP2, 1) + "V</span><span class='label' style='margin-left: 20px;'>I2:</span><span class='value'>" + String(currentP2, 1) + "A</span></div>";
  html += "<div class='info-row'><span class='label'>U3:</span><span class='value'>" + String(voltageP3, 1) + "V</span><span class='label' style='margin-left: 20px;'>I3:</span><span class='value'>" + String(currentP3, 1) + "A</span></div>";
  html += "</div>";

  html += "</div></body></html>";

  server.send(200, "text/html", html);
}

// ### Setup Routine ###
void setup() {
  Serial.begin(115200);

  // Pins konfigurieren
  pinMode(LED1_PIN, OUTPUT);  // Grün: SmartWB Zustand: EIN bei aktiv, FADE bei nicht aktiv
  pinMode(LED2_PIN, OUTPUT);  // Rot
  pinMode(LED3_PIN, OUTPUT);  // Blau
  pinMode(RSE, INPUT_PULLUP); // Hier und an GND muss der Schließer des RSE Relais angeschlossen werden

  // LED initialisieren
  // LED1: Fade (wenn Ereignis1 nicht aktiv, sonst Konstant AN...ggf. auch AUS wenn SmartWB(evse nicht erreichbar)
  led1.begin(LED1_PIN, LEDC_CHANNEL_0, LEDC_TIMER_0);
  led1.fadeStep  = 5;
  led1.fadeDelay = 30;

  // LED2: Blink (bei Ereignis2 = RSE aktiv = LOW)
  led2.begin(LED2_PIN, LEDC_CHANNEL_1, LEDC_TIMER_1);
  led2.blinkInterval = 250; //ms

  // LED3: Flash (immer)
  led3.begin(LED3_PIN, LEDC_CHANNEL_2, LEDC_TIMER_2);
  led3.flashOn  = 100;   //ms AN 125
  led3.flashOff = 3000;  //ms AUS 2000

  // Startzustände
  led1.setMode(LEDMODE_FADE); //SmartWB ist online aber nicht aktiv
  led2.setMode(LEDMODE_OFF);  //RSE ist nicht aktiv
  led3.setMode(LEDMODE_FLASH); //WD LED blitz solange der Watchdog nicht auslöst

  // OLED Initialisieren, Preprozessor entscheidet anhand der OLED_TYPE_xxx welcher Initialisierungsteil genutzt werden soll.
  
  #ifdef OLED_TYPE_SSD1306
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { // 0x3C ist oft die Standardadresse
    Serial.println(F("SSD1306 allocation failed"));
    for(;;); // Abbruch
  }
  #else
  if(!display.begin(0x3C, true)) { // 0x3C ist oft die Standardadresse
    Serial.println(F("SH110X allocation failed"));
    for(;;); // Abbruch
  }
  #endif

  display.clearDisplay();             //OLED löschen
  display.setTextSize(1);             // Textgröße
  //Farbe anhand des OLED Typs setzten
  #ifdef OLED_TYPE_SSD1306
  display.setTextColor(SSD1306_WHITE);// Textfarbe
  #else
  display.setTextColor(SH110X_WHITE); // Textfarbe
  #endif
 
  display.setCursor(0,0);             // Startposition

  // WLAN verbinden und auf serial und OLED ausgeben
  Serial.println(getZeitstempel() + " Verbinde mit WLAN");
  display.print("Verbinde mit WLAN"); // ebenfalls auf das OLED schreiben
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    display.print("."); // auch auf das OLED schreiben
    display.display();
  }
  Serial.print("Sketch-Dateiname: ");
  Serial.println(__FILE__);
  Serial.println("Programmversion: " VERSION);

  Serial.println("");
  display.println();
  Serial.println(getZeitstempel() + " WLAN verbunden!");
  Serial.println(getZeitstempel() + " IP-Adresse: " + WiFi.localIP().toString());
  display.clearDisplay();                           // OLED Display löschen
  display.setCursor(0, 8);                         // Cursor auf die 2. Zeile setzten
  display.print("IP: "+ WiFi.localIP().toString()); // IP auf OLED anzeigen
  display.display();


  // NTP konfigurieren (für Zeitstempel)
  configTime(3600, 3600, "pool.ntp.org", "time.nist.gov");
  delay(2000);                      // kurz warten bis Zeit da ist
  display.setCursor(0,0);           //Cursor wieder oben links setzen für Zeitausgabe
  display.print(getZeitstempel());  //Zeit auf OLED schreiben
  display.display();                //

  // Webserver konfigurieren und starten
  server.on("/", handleRoot);
  server.begin();
  Serial.println(getZeitstempel() + " Webserver gestartet auf http://" + WiFi.localIP().toString());

  // Interrupt konfigurieren
  attachInterrupt(digitalPinToInterrupt(RSE), isrRSE, CHANGE);

// Ertsmaligre Initialisierrung um einen Soc zu erhalten
  String token = getTibberToken();
  soc = getSoc(token);

    // Fügt die aktuelle Task dem Watchdog hinzu. 
  // Das ESP-IDF-Framework initialisiert den Watchdog oft automatisch.
  Serial.println("Watchdog-Task wird zur Überwachung hinzugefügt...");
  esp_task_wdt_add(NULL);

  // Optional: Checke den Reset-Grund für Debug-Zwecke
  esp_reset_reason_t reason = esp_reset_reason();
  if (reason == ESP_RST_TASK_WDT) {
    Serial.println("Letzter Neustart wurde durch den Task Watchdog ausgelöst.");
  }
  Serial.println("Watchdog 1. reset..."); 
  esp_task_wdt_reset(); // Watchdog zurücksetzen...

}


void loop() {
  unsigned long now = millis();
  int httpCode;

  // RSE Flankenerkennung
  if (RSEAktiv != letzterRSEStatus) {
    letzterRSEStatus = RSEAktiv;

    if (RSEAktiv) {
      Serial.println(getZeitstempel() + " RSE wurde AKTIV → Power ON");
      if (WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        http.begin(urlOn);
        int httpCode = http.GET();
        Serial.println(getZeitstempel() + " HTTP Antwort: " + String(httpCode));
        http.end();
      }
    } else {
      Serial.println(getZeitstempel() + " RSE wurde INAKTIV → Power OFF");
      if (WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        http.begin(urlOff);
        int httpCode = http.GET();
        Serial.println(getZeitstempel() + " HTTP Antwort: " + String(httpCode));
        http.end();
      }
    }
  }

  // LED Steuerung und RSE Anzeige auf OLED
  if (RSEAktiv) {
    //digitalWrite(LED_GRUEN, LOW);
    led2.setMode(LEDMODE_BLINK); // RSE aktiv rote LED blinken
    // led1.setMode(LEDMODE_OFF);   //Grüne LED aus

    // Blink-Logik für Rot mit millis()
    aktuelleUmschaltung = millis();
    if (aktuelleUmschaltung - letzteUmschaltung >= led2.blinkInterval) {
      letzteUmschaltung = aktuelleUmschaltung;
      rotStatus = !rotStatus;
      //digitalWrite(LED_ROT, rotStatus);
      //RSE Anzeige im  OLED setzen
      display.setCursor(13 * CHAR_SIZE_X, 5 * CHAR_SIZE_Y); // y=78 (13.Spalte), x=40 (5.Zeile)
      if (rotStatus) {
        display.print("RSE akt"); //RSE Anzeige blinken lassen -> Ein
      } else {
        #ifdef OLED_TYPE_SSD1306
          display.fillRect(13 * CHAR_SIZE_X, 5 * CHAR_SIZE_Y, SCREEN_WIDTH - 13 * CHAR_SIZE_X, CHAR_SIZE_Y, SSD1306_BLACK); //RSE Anzeige blinken lassen -> Aus
        #else
          display.fillRect(13 * CHAR_SIZE_X, 5 * CHAR_SIZE_Y, SCREEN_WIDTH - 13 * CHAR_SIZE_X, CHAR_SIZE_Y, SH110X_BLACK); //RSE Anzeige blinken lassen -> Aus
        #endif        
      }    
      display.display(); 
    }

  } else {
    // Normalzustand → Rot aus, Grün an
    //digitalWrite(LED_ROT, LOW);
    //digitalWrite(LED_GRUEN, HIGH);
    led2.setMode(LEDMODE_OFF);
    if (httpCode >=0 ){ //wenn die SmartWB erreichbar ist, dann entweder FADE (bei bereit) oder ON (bei EIN)
      led1.setMode(evseState ? LEDMODE_ON : LEDMODE_FADE);
    }
    else {
      led1.setMode(LEDMODE_OFF); //SmartWB ist nicht erreichbar also AUS schalten
    }
    //RSE Anzeige im OLED löschen
    display.setCursor(13 * CHAR_SIZE_X, 5 * CHAR_SIZE_Y); // x=78 (13.Spalte), y=40 (5.Zeile)
    // RSE Anzeige wieder  löschen // x=78 (13.Spalte), y=40 (5.Zeile)
    #ifdef OLED_TYPE_SSD1306
    display.fillRect(13 * CHAR_SIZE_X, 5 * CHAR_SIZE_Y, SCREEN_WIDTH - 13 * CHAR_SIZE_X, CHAR_SIZE_Y, SSD1306_BLACK);
    #else
    display.fillRect(13 * CHAR_SIZE_X, 5 * CHAR_SIZE_Y, SCREEN_WIDTH - 13 * CHAR_SIZE_X, CHAR_SIZE_Y, SH110X_BLACK);
    #endif
    display.display();
  }
  //Uhrzeit und Fortschrittsbalken alle CLOCKCOUNT sec anzeigen
  aktuelleUhrAnzeige = millis();
  if (aktuelleUhrAnzeige - letzteUhrAnzeige >= UHR_ANZEIGE_INTERVAL) {
      letzteUhrAnzeige = aktuelleUhrAnzeige;
      display.setCursor(0,0);           //Cursor wieder oben links setzen für Zeitausgabe
      // Zeile überschreiben mit schwarzem Rechteck (löschen)
      #ifdef OLED_TYPE_SSD1306
      display.fillRect(0, 0, SCREEN_WIDTH, CHAR_SIZE_Y, SSD1306_BLACK);
      #else
      display.fillRect(0, 0, SCREEN_WIDTH, CHAR_SIZE_Y, SH110X_BLACK);
      #endif
      
      display.print(getZeitstempel());  //Zeit auf OLED schreiben
      // Vielleicht zeige ich in dem Fortschrittsbalken mal den SOC vom angeschlossenen Auto an...
      // Inkrementiere den Fortschritt und setze ihn bei 100% zurück
      // currentProgress = (currentProgress >= 10) ? 0 : currentProgress + 1; //10sec
      // Serial.print("Progress: " + String(currentProgress));
      // Übergabe des Fortschritts an die Routine
      //drawProgressBar(currentProgress*10);
      // Watchdog reset
    Serial.print("Watchdog reset..."); //Es scheint so zu sein, als wäre er Systemseitig auf 5sec eingestellt. Das führt bei Power-On schon zu einem Reboot. Daher auch schon mal in der Setup Routine zurücksetzen!
    // esp_task_wdt_reset(); // Watchdog zurücksetzen (sollte alle 1000ms passieren, da die Zeitanzeige jede Sekunde aufgerufen wird
    esp_err_t err_code = esp_task_wdt_reset(); 
    Serial.println("Ergebnis: " + String(err_code));


      display.display();                //
  }

  //SOC des EVs aus der Tibber Abfrage holen. Das machen wir aber nur alle 10min 
  // zum Testen einfach Zufallszahl
  //int soc=random(0,100);

    // Alle SOC_ANZEIGE_INTERVAL msec Soc holen (!!! sollte nicht kleiner als 60sec = 600000ms !!!)
  aktuelleSocAnzeige = millis();
  if (aktuelleSocAnzeige - letzteSocAnzeige >= SOC_ANZEIGE_INTERVAL) {
    letzteSocAnzeige = aktuelleSocAnzeige;
    // soc holen
    String token = getTibberToken();
    if (token != "") {
      soc = getSoc(token);
      Serial.printf("SoC: %d%%\n", soc);
    }
  }

  //Werte aus der SmartWB alle SMARTWBCOUNT msec anzeigen
  aktuelleSmartWBAnzeige = millis();
  if (aktuelleSmartWBAnzeige - letzteSmartWBAnzeige >= SMARTWB_ANZEIGE_INTERVAL) {
    letzteSmartWBAnzeige = aktuelleSmartWBAnzeige;
      
    // Zeile überschreiben mit schwarzem Rechteck (löschen) in Abhängigkeit des OLED Typs
    #ifdef OLED_TYPE_SSD1306
    display.fillRect(0, 3 * CHAR_SIZE_Y, SCREEN_WIDTH, 4 * CHAR_SIZE_Y, SSD1306_BLACK);    // Ab der Zeile 3 die nächsten 4 Zeilen löschen
    #else
    display.fillRect(0, 3 * CHAR_SIZE_Y, SCREEN_WIDTH, 4 * CHAR_SIZE_Y, SH110X_BLACK);    // Ab der Zeile 3 die nächsten 4 Zeilen löschen
    #endif
      
    //Check ob SmartWB online, wenn ja die Funktion SmartWB aufrufen und die geholten Werte anzeigen, 
    // sonst alle Werte auf 0 setzten und evseState als "OFFLINE" anzeigen
    
    display.setCursor(0, 3 * CHAR_SIZE_Y);                                   // Cursor auf die Zeile 3 setzen
    display.print("SmartWB: ");

    Serial.println("Watchdog reset..."); //Es scheint so zu sein, als wäre er Systemseitig auf 5sec eingestellt. Das führt bei Power-On schon zu einem Reboot. Daher auch schon mal in der Setup Routine zurücksetzen!
    // Watchdog nochmal zurücksetzten, da der getSmartWBParameters Aufruf u.U. verzögert wird...
    esp_err_t err_code = esp_task_wdt_reset(); 
    Serial.println("Ergebnis vor getSmartWBParameters: " + String(err_code));

    getSmartWBParameters(httpCode);
    //Testen ob SmartWB online ist
    if (httpCode >= 0) {
      display.println(evseState ? "EIN" : "AUS");
      // nur wenn die SmartWB ONLINE ist und das Fzg. angeschlossen (vehicleState=2) oder lädt (vehicleState=3), zeigen wir auch den SOC an, sonst nicht
      if (vehicleState==2||vehicleState==3) {
        display.setCursor( 13 * CHAR_SIZE_X, 3 * CHAR_SIZE_Y);
       display.println("SOC: " + String(soc) + "%");
      }
    }
    else {
      #ifdef OLED_TYPE_SSD1306
      display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
      #else 
      display.setTextColor(SH110X_BLACK, SH110X_WHITE);
      #endif
      display.println("OFFLINE"); //SmartWB (evse) ist nicht erreichbar , das soll INVERS angezeigt werden und alle anderen anzuzeigenden Werte auf 0 setzen
      actualPower = 0.0;
      actualCurrent = 0;
      maxCurrent = 0;
      evseState = false;
      voltageP1 = 0;
      voltageP2 = 0;
      voltageP3 = 0;
      currentP1 = 0.0;
      currentP2 = 0.0;
      currentP3 = 0.0;
      // und jetzt wieder die normale Farbdarstellung
       #ifdef OLED_TYPE_SSD1306
      display.setTextColor(SSD1306_WHITE, SSD1306_BLACK);
      #else 
      display.setTextColor(SH110X_WHITE, SH110X_BLACK);
      #endif
    }
    //Falls nur 1-stellig, führendes " " hinzufügen
    display.print("Max Cur: ");
    display.println((maxCurrent < 10 ? " " : "") + String(maxCurrent) + "A"); // maxCurrent auf Display schreiben

    display.print("Act Cur: ");
    display.println((actualCurrent < 10 ? " " : "") + String(actualCurrent) + "A");

    display.print("Act Pow: ");
    display.println((actualPower < 10 ? " " : "") + String(actualPower) + "kW"); // Die aktuelle Leistung die vom EV geladen wird
      
  }
  
  //U + I Werte aus der SmartWB alle SMARTWBCOUNT/3 sec anzeigen
  aktuelleUIAnzeige = millis();
  if (aktuelleUIAnzeige - letzteUIAnzeige >= SMARTWB_ANZEIGE_INTERVAL/3) {
    letzteUIAnzeige = aktuelleUIAnzeige;
    // Zeile überschreiben mit schwarzem Rechteck (löschen) in Abhängigkeit des OLED Typs
    #ifdef OLED_TYPE_SSD1306
    display.fillRect(0, 7 * CHAR_SIZE_Y, SCREEN_WIDTH, CHAR_SIZE_Y, SSD1306_BLACK);   // Die 7.Zeile löschen
    #else
    display.fillRect(0, 7 * CHAR_SIZE_Y, SCREEN_WIDTH, CHAR_SIZE_Y, SH110X_BLACK);    // Die 7.Zeile löschen
    #endif
    display.setCursor(0, 7 * CHAR_SIZE_Y);                                  // Cursor auf die 7. Zeile setzen
    switch (i) {
      case 1:
        display.print("U1: " + String(voltageP1, 1) + "V I1: " + (currentP1 < 10 ? " " : "") + String(currentP1, 1) + "A");
      break;
      case 2:
        display.print("U2: " + String(voltageP2, 1) + "V I2: " + (currentP2 < 10 ? " " : "") + String(currentP2, 1) + "A");
      break;
      case 3:
        display.print("U3: " + String(voltageP3, 1) + "V I3: " + (currentP3 < 10 ? " " : "") + String(currentP3, 1) + "A");
      break;
    }
     
    i = (i + 1 > 3) ? 1 : i + 1;  //Zähler +1 prüfen ob schon > 3, wenn ja, auf 1 setzten, sonst erhöhen
    

  }

  //Updates
  display.display();
  server.handleClient(); // Webserver-Anfragen bearbeiten
  led1.update(now); // SmartWB (evse) Status anzeigen: BEREIT: Grün Fade, EIN: Grün kontinuierlich an, OFFLINE: Grün aus
  led2.update(now); // RSE aktiv: Rot blinkt, RSE nicht aktiv: Rot aus
  led3.update(now); // Watchdog LED sollte immer blitzen, solange der Watchdog aufgerufen wird
      
}







