#include <ESP8266WiFi.h>
#include <Wire.h>
#include <ClosedCube_SHT31D.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <ArduinoJson.h>
#include <ArduinoUniqueID.h>

// SSL Functions and Variables
#include "SSLClient.cpp"

ClosedCube_SHT31D sht3xd;

// Konstanten
const char* APPLICATION_JSON = "application/json";
const char* NTP_SERVER = "pool.ntp.org";
const long GMT_OFFSET_SEC = 3600;
const int DAYLIGHT_OFFSET_SEC = 3600;

// WiFi-Konfiguration
const char* WIFI_PROVIDER = "WPS"; // WPS oder SSID
const char* SSID = "WIFI-SSID";
const char* PASSWORD = "WIFI-PASSWORD";

// Server-Konfiguration
const char* HOST = "HOST-IP"; // Change to your host
const uint16_t PORT = 443; // HTTPS port

// User-Konfiguration
const char* username = "default";

// API-Konfiguration
const String URL_REGISTER_SENSOR_CONFIRM = String("/api/register/sensor/confirm");
const String URL_MEASUREMENTS = String("/api/measurements");

// Funktion zur Verbindung mit dem WiFi
void connectToWiFi() {
    if (strcmp(WIFI_PROVIDER, "WPS") == 0) {
        Serial.println("Starte WPS-Verbindung...");
        WiFi.beginWPSConfig();
    } else {
        Serial.print("Verbinde mit ");
        Serial.println(SSID);
        WiFi.begin(SSID, PASSWORD);
    }

    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }

    Serial.println("");
    Serial.println("WiFi verbunden");
    Serial.println("IP-Adresse: ");
    Serial.println(WiFi.localIP());

    // Initialisiere NTP
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
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

// Funktion zur Durchführung einer HTTPS-Anfrage
int performHttpsRequest(const String& url, StaticJsonDocument<200>& doc) {
    if (connectToServer(HOST, PORT)) {
        client.println("POST " + url + " HTTP/1.1");
        client.println("Host: " + String(HOST));
        client.println("Content-Type: " + String(APPLICATION_JSON));
        client.print("Content-Length: ");
        client.println(measureJson(doc));
        client.println();

        String payload;
        serializeJson(doc, payload);
        client.println(payload);

        String response = readResponse();

        Serial.println("-- Endpunkt URL --");
        Serial.println(url);
        Serial.println("-- Payload --");
        serializeJsonPretty(doc, Serial);
        Serial.println("\n-- Response --");

        Serial.println(response);

        return 200; // Simplified for demonstration. You can parse the actual response code from the response string.
    } else {
        Serial.println("Fehler beim Verbinden mit dem Server.");
        return -1;
    }
}

// Funktion zur Bestätigung der Sensorregistrierung
void confirmSensorRegistration() {
    if (WiFi.status() == WL_CONNECTED) {
        StaticJsonDocument<200> doc;
        doc["username"] = username;
        doc["uuid"] = getUniqueID();

        performHttpsRequest(URL_REGISTER_SENSOR_CONFIRM, doc);
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

    performHttpsRequest(URL_MEASUREMENTS, doc);
}

void setup() {
    Serial.begin(115200);
    Wire.begin();
    sht3xd.begin(0x44);

    connectToWiFi();
    initializeSSL(); // Initialize SSL
    confirmSensorRegistration(); // Bestätige die Registrierung des Sensors nach der WiFi-Verbindung
}

void loop() {
    // Überprüfe die WiFi-Verbindung und verbinde erneut, falls getrennt
    if (WiFi.status() != WL_CONNECTED) {
        connectToWiFi();
    } else {
        sendSensorData();
    }

    delay(5000); // Sende alle 5 Sekunden Daten
}