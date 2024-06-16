#include <ESP8266WiFi.h>
#include <ESPAsyncTCP.h>
#include <WiFiClient.h>
#include <Wire.h>
#include <ClosedCube_SHT31D.h>
#include <ESP8266HTTPClient.h>
#include <time.h>
#include <ArduinoJson.h>
#include <ArduinoUniqueID.h>
#include <ArduinoOTA.h>
#include <ESPAsyncWebServer.h>
#include <WebSerial.h>

// Konstanten
const char *APPLICATION_JSON = "application/json";
const char *NTP_SERVER = "pool.ntp.org";
const long GMT_OFFSET_SEC = 3600; // Offset für die Zeitzone (1 Stunde)
const int DAYLIGHT_OFFSET_SEC = 3600; // Offset für Sommerzeit (1 Stunde)
const char *API_KEY_HEADER_NAME = "X-API-KEY"; // Name des API-Schlüssel-Headers
const char *API_KEY = "api-key"; // API-Schlüssel

// WiFi-Konfiguration
const char *WIFI_PROVIDER = "SSID"; // WiFi-Anbieter: WPS oder SSID
const char *SSID = "wifi-ssid"; // WiFi-SSID
const char *PASSWORD = "wifi-password"; // WiFi-Passwort
unsigned long previousMillis = millis(); // Vorherige Millisekunden
unsigned long interval = 30000; // Intervall für WiFi-Verbindungsüberprüfung (30 Sekunden)
unsigned long send_sensor_data_interval = 60000; // Intervall für das Senden von Sensordaten (60 Sekunden)
unsigned long webSerial_interval = 2000; // Intervall für WebSerial (2 Sekunden)

// Server-Konfiguration
const char *HOST = "192.168.1.6"; // Server-Host
const uint16_t PORT = 8080; // Server-Port

// User-Konfiguration
const char *USERNAME = "default"; // Standardbenutzername

// API-Konfiguration
const String URL_REGISTER_SENSOR_CONFIRM =
        String(HOST) + ":" + PORT + "/api/sensor/register/confirm"; // URL für Sensorregistrierung
const String URL_MEASUREMENTS = String(HOST) + ":" + PORT + "/api/sensor/measurements"; // URL für Sensormessungen

// Optionale Sicherheitseinstellungen
const char *ROOT_CA = ""; /*\
"-----BEGIN CERTIFICATE-----\n" \
"MIID...." \
"-----END CERTIFICATE-----\n";*/ // Root-Zertifikat des Servers
const char *FINGERPRINT = ""; /*"XX XX XX XX XX XX XX XX XX XX XX XX XX XX XX XX XX XX XX XX";*/ // SHA-1 Fingerabdruck des Serverzertifikats

ClosedCube_SHT31D sht3xd; // SHT31D-Sensor
AsyncWebServer server(80); // AsyncWebServer auf Port 80

/**
 * Verbindet mit dem WiFi
 */
void connectToWiFi() {
    WiFi.mode(WIFI_STA);
    Serial.println("");

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

    Serial.println("WiFi verbunden");
    Serial.print("IP-Adresse: ");
    Serial.println(WiFi.localIP());
    Serial.print("RSSI: ");
    Serial.println(WiFi.RSSI());

    // Initialisiere NTP
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
}

/**
 * Formatiert die aktuelle Zeit
 * @return Die formatierte Zeit als String
 */
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

/**
 * Generiert eine eindeutige Sensor-ID
 * @return Die eindeutige Sensor-ID als String
 */
String getUniqueID() {
    char uniqueID[UniqueIDsize * 2 + 1];
    for (size_t i = 0; i < UniqueIDsize; i++) {
        sprintf(uniqueID + i * 2, "%02X", UniqueID[i]);
    }
    return String(uniqueID);
}

/**
 * Führt eine HTTP/HTTPS-Anfrage durch
 * @param url Die URL der Anfrage
 * @param doc Das JSON-Dokument, das gesendet werden soll
 * @return Der HTTP-Antwortcode
 */
int performHttpRequest(const String &url, StaticJsonDocument<200> &doc) {
    HTTPClient http;
    WiFiClient *client;
    String urlString;

    bool useHttps = (strlen(ROOT_CA) > 0 || strlen(FINGERPRINT) > 0);

    if (useHttps) {
        WiFiClientSecure *secureClient = new WiFiClientSecure;
        if (strlen(ROOT_CA) > 0) {
            BearSSL::X509List cert(ROOT_CA);
            secureClient->setTrustAnchors(&cert);
        } else if (strlen(FINGERPRINT) > 0) {
            secureClient->setFingerprint(FINGERPRINT);
        }
        client = secureClient;
        urlString = "https://" + url;
    } else {
        client = new WiFiClient;
        urlString = "http://" + url;
    }

    http.begin(*client, urlString);
    http.addHeader("Content-Type", APPLICATION_JSON, true, true);
    http.addHeader(API_KEY_HEADER_NAME, API_KEY, false, false);

    String payload;
    serializeJson(doc, payload);

    int httpResponseCode = http.POST(payload);

    Serial.println("-- Endpunkt URL --");
    Serial.println(urlString);
    Serial.println("-- Payload --");
    serializeJsonPretty(doc, Serial);
    Serial.println("\n-- Response --");

    // Zusätzlich in WebSerial ausgeben
    WebSerial.println("-- Endpunkt URL --");
    WebSerial.println(urlString);
    WebSerial.println("-- Payload --");
    serializeJsonPretty(doc, WebSerial);
    WebSerial.println("\n-- Response --");

    if (httpResponseCode > 0) {
        String response = http.getString();
        Serial.print(httpResponseCode);
        Serial.print(": ");
        Serial.println(response);

        WebSerial.print(httpResponseCode);
        WebSerial.print(": ");
        WebSerial.println(response);
    } else {
        Serial.print("Fehler beim Senden des POST: ");
        Serial.println(httpResponseCode);

        WebSerial.print("Fehler beim Senden des POST: ");
        WebSerial.println(httpResponseCode);
    }

    Serial.println("");
    WebSerial.println("");

    http.end();
    delete client;
    return httpResponseCode;
}

/**
 * Bestätigt die Sensorregistrierung
 */
void confirmSensorRegistration() {
    if (WiFi.status() == WL_CONNECTED) {
        StaticJsonDocument<200> doc;
        doc["username"] = USERNAME;
        doc["uuid"] = getUniqueID();

        performHttpRequest(URL_REGISTER_SENSOR_CONFIRM, doc);
    }
}

/**
 * Sendet Sensordaten an den Server
 */
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

    JsonArray measurements = doc.createNestedArray("measurements");

    JsonObject temperatureJson = measurements.createNestedObject();
    temperatureJson["type"] = "TEMPERATURE";
    temperatureJson["value"] = String(temperature, 1);
    temperatureJson["unit"] = "CELSIUS";

    JsonObject humidityJson = measurements.createNestedObject();
    humidityJson["type"] = "HUMIDITY";
    humidityJson["value"] = String(humidity, 1);
    humidityJson["unit"] = "PERCENT";

    JsonObject gasJson = measurements.createNestedObject();
    gasJson["type"] = "GAS";
    gasJson["value"] = voc;
    gasJson["unit"] = "PPM";

    performHttpRequest(URL_MEASUREMENTS, doc);
}

/**
 * Initialisiert das Gerät
 */
void setup() {
    Serial.begin(115200);
    Wire.begin();
    sht3xd.begin(0x44);

    connectToWiFi();

    // ArduinoOTA für OTA-Updates
    ArduinoOTA.setHostname("envy");
    ArduinoOTA.setPassword("admin");

    ArduinoOTA.onStart([]() {
        String type;
        if (ArduinoOTA.getCommand() == U_FLASH) {
            type = "sketch";
        } else {  // U_FS
            type = "filesystem";
        }

        Serial.println("Start updating " + type);
    });
    ArduinoOTA.onEnd([]() {
        Serial.println("\nEnd");
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
    });
    ArduinoOTA.onError([](ota_error_t error) {
        Serial.printf("Error[%u]: ", error);
        if (error == OTA_AUTH_ERROR) {
            Serial.println("Auth Failed");
        } else if (error == OTA_BEGIN_ERROR) {
            Serial.println("Begin Failed");
        } else if (error == OTA_CONNECT_ERROR) {
            Serial.println("Connect Failed");
        } else if (error == OTA_RECEIVE_ERROR) {
            Serial.println("Receive Failed");
        } else if (error == OTA_END_ERROR) {
            Serial.println("End Failed");
        }
    });
    ArduinoOTA.begin();

    // WebSerial starten
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "text/plain", "Hi! This is WebSerial. You can access webserial interface at http://" +
                                         WiFi.localIP().toString() + "/webserial");
        Serial.println(
                "Hi! This is WebSerial. You can access webserial interface at http://" + WiFi.localIP().toString() +
                "/webserial");
    });

    // WebSerial is accessible at "<IP Address>/webserial" in browser
    WebSerial.begin(&server);

    // Attach Message Callback
    WebSerial.onMessage([&](uint8_t *data, size_t len) {
        Serial.printf("Received %u bytes from WebSerial: ", len);
        Serial.write(data, len);
        Serial.println();
        WebSerial.println("Received Data...");
        String d = "";
        for (size_t i = 0; i < len; i++) {
            d += char(data[i]);
        }
        WebSerial.println(d);
    });

    // Start server
    server.begin();

    confirmSensorRegistration(); // Bestätige die Registrierung des Sensors nach der WiFi-Verbindung
}

/**
 * Hauptschleife des Programms
 */
void loop() {
    unsigned long currentMillis = millis();

    ArduinoOTA.handle();

    // Überprüfe die WiFi-Verbindung alle 30 Sekunden und verbinde erneut, falls getrennt
    if ((WiFi.status() != WL_CONNECTED) && (currentMillis - previousMillis >= interval)) {
        Serial.print(millis());
        Serial.println("Stelle WiFi - Verbindung wieder her...");
        WiFi.disconnect();
        WiFi.reconnect();
        previousMillis = currentMillis;
    }

    // Sende alle 60 Sekunden Sensor-Daten
    if ((WiFi.status() == WL_CONNECTED) && (currentMillis - previousMillis >= send_sensor_data_interval)) {
        sendSensorData();
        previousMillis = currentMillis;
    }
    WebSerial.loop();
}