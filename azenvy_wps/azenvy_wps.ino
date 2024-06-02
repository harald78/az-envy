#include <ESP8266WiFi.h>
#include <Wire.h>
#include <ClosedCube_SHT31D.h>
#include <ESP8266HTTPClient.h>
#include <time.h>
#include <ArduinoJson.h>
#include <ArduinoUniqueID.h>

ClosedCube_SHT31D sht3xd;

const char* host = "HOST-IP";
const uint16_t port = 8080;  // Port auf 8080 geändert

const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 3600; // MEZ Offset (1 Stunde)
const int   daylightOffset_sec = 3600; // Sommerzeit Offset (1 Stunde)

// Funktion zur Verbindung mit dem WiFi mithilfe von WPS
void connectToWiFi() {
    Serial.println("Starte WPS-Verbindung...");
    WiFi.beginWPSConfig();
    delay(500);

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

// Funktion zur Generierung einer eindeutigen Sensor-ID
String getUniqueID() {
    char uniqueID[UniqueIDsize * 2 + 1];
    for (size_t i = 0; i < UniqueIDsize; i++) {
        sprintf(uniqueID + i * 2, "%02X", UniqueID[i]);
    }
    return String(uniqueID);
}

// Funktion zur Bestätigung der Sensorregistrierung
void confirmSensorRegistration() {
    if (WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        WiFiClient client;
        String url = "http://" + String(host) + ":" + String(port) + "/register/confirm";
        http.begin(client, url);

        http.addHeader("Content-Type", "application/json");

        StaticJsonDocument<200> jsonDoc;
        jsonDoc["username"] = "your-username"; // Füge hier den tatsächlichen Benutzernamen ein
        jsonDoc["uuid"] = getUniqueID();

        String requestBody;
        serializeJson(jsonDoc, requestBody);

        int httpResponseCode = http.POST(requestBody);

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
}

// Funktion zum Senden von Sensordaten
void sendSensorData() {
    SHT31D result = sht3xd.readTempAndHumidity(SHT3XD_REPEATABILITY_HIGH, SHT3XD_MODE_CLOCK_STRETCH, 60000);
    float temperature = result.t;
    float humidity = result.rh;
    float voc = analogRead(A0); // MQ-2-Sensor ist an Serial-Port 0 angeschlossen

    String timestamp = getFormattedTime();
    String uniqueID = getUniqueID();

    StaticJsonDocument<200> doc;
    doc["base"] = "AZEnvy";
    doc["timestamp"] = timestamp;
    doc["id"] = uniqueID;

    JsonObject measurements = doc["measurements"].to<JsonObject>();

    JsonObject temperatureJson = measurements.createNestedObject("temperature");
    temperatureJson["type"] = "TEMPERATURE";
    temperatureJson["value"] = String(temperature, 1);
    temperatureJson["unit"] = "CELSIUS";

    JsonObject humidityJson = measurements.createNestedObject("humidity");
    humidityJson["type"] = "HUMIDITY";
    humidityJson["value"] = String(humidity, 1);
    humidityJson["unit"] = "PERCENT";

    JsonObject gasJson = measurements.createNestedObject("gas");
    gasJson["type"] = "GAS";
    gasJson["value"] = voc;
    gasJson["unit"] = "PARTICLE";

    String payload;
    serializeJson(doc, payload);

    HTTPClient http;
    WiFiClient client;
    String url = "http://" + String(host) + ":" + String(port) + "/api/measurements";

    http.begin(client, url);
    http.addHeader("Content-Type", "application/json");

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

void setup() {
    Serial.begin(115200);
    Wire.begin();
    sht3xd.begin(0x44);

    connectToWiFi();
    confirmSensorRegistration(); // Bestätige die Registrierung des Sensors nach der WiFi-Verbindung
}

void loop() {
    // Überprüfe die WiFi-Verbindung und verbinde erneut, falls getrennt
    if (WiFi.status() != WL_CONNECTED) {
        connectToWiFi();
    } else {
        sendSensorData();
    }

    delay(15000); // Sende alle 15 Sekunden Daten
}