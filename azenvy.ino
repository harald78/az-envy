#include <ESP8266WiFi.h>
#include <Wire.h>
#include <ClosedCube_SHT31D.h>
#include <ESP8266HTTPClient.h>
#include <time.h>
#include <ArduinoJson.h>
#include <ArduinoUniqueID.h>

ClosedCube_SHT31D sht3xd;

#ifndef STASSID
#define STASSID "WLAN-SSID"
#define STAPSK "WLAN-PSK"
#endif

const char* ssid = STASSID;
const char* password = STAPSK;

const char* host = "SERVER";
const uint16_t port = 3001;  // Port auf 3001 geändert

const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 3600; // MEZ Offset (1 Stunde)
const int   daylightOffset_sec = 3600; // Sommerzeit Offset (1 Stunde)

//const char* bearerToken = "your_bearer_token"; // Füge hier deinen Bearer-Token ein

// Funktion zur Verbindung mit dem WiFi
void connectToWiFi() {
    Serial.print("Verbinde mit ");
    Serial.println(ssid);
    WiFi.begin(ssid, password);

    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }

    Serial.println("");
    Serial.println("WiFi verbunden");
    Serial.println("IP-Adresse: ");
    Serial.println(WiFi.localIP());

    // Initialisiere NTP
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
}

// Funktion zur Formatierung der aktuellen Zeit
String getFormattedTime() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        Serial.println("Zeit konnte nicht abgerufen werden");
        return "";
    }
    char timeStringBuff[50];
    strftime(timeStringBuff, sizeof(timeStringBuff), "%Y-%m-%dT%H:%M:%S%z", &timeinfo);
    return String(timeStringBuff);
}

void setup() {
    Serial.begin(115200);
    Wire.begin();
    sht3xd.begin(0x44);

    connectToWiFi();
}

void loop() {
    // Überprüfe die WiFi-Verbindung und verbinde erneut, falls getrennt
    if (WiFi.status() != WL_CONNECTED) {
        connectToWiFi();
    } else {
        SHT31D result = sht3xd.readTempAndHumidity(SHT3XD_REPEATABILITY_HIGH, SHT3XD_MODE_CLOCK_STRETCH, 60000);
        float temperature = result.t;
        float humidity = result.rh;
        float voc = analogRead(A0); // MQ-2-Sensor ist an Serial-Port 0 angeschlossen

        String timestamp = getFormattedTime();

        // Generiere eine eindeutige Sensor-ID
        char uniqueID[UniqueIDsize * 2 + 1];
        for (size_t i = 0; i < UniqueIDsize; i++) {
            sprintf(uniqueID + i * 2, "%02X", UniqueID[i]);
        }

        // Erstelle JSON-Payload
        StaticJsonDocument<200> doc;
        doc["temperature"] = String(temperature, 1);
        doc["humidity"] = String(humidity, 1);
        doc["voc"] = voc;
        doc["timestamp"] = timestamp;
        doc["id"] = uniqueID;

        String payload;
        serializeJson(doc, payload);

        HTTPClient http;
        WiFiClient client;

        String url = "http://" + String(host) + ":" + String(port) + "/azenvy";

        http.begin(client, url);
        http.addHeader("Content-Type", "application/json");
        //http.addHeader("Authorization", "Bearer " + String(bearerToken)); // Füge den Bearer-Token hinzu

        int httpResponseCode = http.POST(payload);

        if (httpResponseCode > 0) {
            String response = http.getString();
            Serial.println(httpResponseCode);
            Serial.println(response);
        } else {
            Serial.print("Fehler beim Senden des POST: ");
            Serial.println(httpResponseCode);
        }

        http.end();
    }

    delay(15000); // Sende alle 15 Sekunden Daten
}