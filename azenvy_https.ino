#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <Wire.h>
#include <ClosedCube_SHT31D.h>
#include <ESP8266HTTPClient.h>
#include <time.h>
#include <ArduinoJson.h>
#include <ArduinoUniqueID.h>
#include <MQUnifiedsensor.h>
/************************Hardware Related Macros************************************/
#define         Board                   ("ESP8266")
#define         Pin                     (A0)  //Analog input 0 of your ESP8266
/***********************Software Related Macros************************************/
#define         Type                    ("MQ-2") //MQ2
#define         Voltage_Resolution      (5)
#define         ADC_Bit_Resolution      (10) // For arduino UNO/MEGA/NANO
#define         RatioMQ2CleanAir        (9.83) //RS / R0 = 9.83 ppm

/*****************************Globals***********************************************/
MQUnifiedsensor MQ2_H2(Board, Voltage_Resolution, ADC_Bit_Resolution, Pin, Type);
MQUnifiedsensor MQ2_LPG(Board, Voltage_Resolution, ADC_Bit_Resolution, Pin, Type);
MQUnifiedsensor MQ2_CO(Board, Voltage_Resolution, ADC_Bit_Resolution, Pin, Type);
MQUnifiedsensor MQ2_Alcohol(Board, Voltage_Resolution, ADC_Bit_Resolution, Pin, Type);
MQUnifiedsensor MQ2_Propane(Board, Voltage_Resolution, ADC_Bit_Resolution, Pin, Type);
/*****************************Globals***********************************************/

// Konstanten
const char *APPLICATION_JSON = "application/json";
const char *NTP_SERVER = "pool.ntp.org";
const long GMT_OFFSET_SEC = 3600; // Offset for GMT+1
const int DAYLIGHT_OFFSET_SEC = 3600; // Offset for daylight saving time
const char *API_KEY_HEADER_NAME = "X-API-KEY"; // Name of the API key header
const char *API_KEY = "api-key"; // API key

// WiFi configuration
const char *WIFI_PROVIDER = "SSID"; // WiFi provider (WPS or SSID)
const char *SSID = "wifi-ssid"; // WiFi-SSID
const char *PASSWORD = "wifi-password"; // WiFi password
unsigned long previousMillis = millis(); // Previous time
unsigned long interval = 30000; // Interval for checking WiFi connection (30 seconds)
unsigned long send_sensor_data_interval = 60000; // Interval for sending sensor data (60 seconds)
unsigned long webSerial_interval = 2000; // Interval for WebSerial (2 seconds)

// Server configuration
const char *HOST = "server-ip"; // Server IP
const uint16_t PORT = 8080; // Server port

// User configuration
const char *USERNAME = "default"; // Username

// API Configuration
const String URL_REGISTER_SENSOR_CONFIRM =
        String(HOST) + ":" + PORT + "/api/sensor/register/confirm"; // URL for sensor registration confirmation
const String URL_MEASUREMENTS = String(HOST) + ":" + PORT + "/api/sensor/measurements"; // URL for sensor measurements

// Optional server certificate verification
const char *ROOT_CA = ""; /*\
"-----BEGIN CERTIFICATE-----\n" \
"MIID...." \
"-----END CERTIFICATE-----\n";*/ // Root CA certificate of the server
const char *FINGERPRINT = ""; /*"XX XX XX XX XX XX XX XX XX XX XX XX XX XX XX XX XX XX XX XX";*/ // SHA-1 fingerprint of the server certificate

ClosedCube_SHT31D sht3xd; // SHT31D-Sensor

/**
 * Verbindet mit dem WiFi
 */
void connectToWiFi() {
    WiFi.mode(WIFI_STA);
    Serial.println("");

    if (strcmp(WIFI_PROVIDER, "WPS") == 0) {
        Serial.println("Starte WPS-Verbindung...");
        WiFi.persistent(true); // Einstellungen persistent speichern
        WiFi.beginWPSConfig();
        // Warte, bis die WPS-Konfiguration abgeschlossen ist
        while (WiFi.status() != WL_CONNECTED) {
            delay(500);
            Serial.print(".");
        }
        // Speicher die WiFi-Verbindung dauerhaft
        if (WiFi.SSID().length() > 0) {
            Serial.println("WPS-Verbindung erfolgreich, speichere die Verbindung.");
            WiFi.persistent(true);
            WiFi.begin(WiFi.SSID().c_str(), WiFi.psk().c_str());
        }
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
 * Kalibriert den Sensor und gibt wichtige Werte aus
 */
void calibrateMQ2Sensor(MQUnifiedsensor &type, const char* name, float a, float b) {
    type.setRegressionMethod(1);
    type.setA(a);
    type.setB(b); // Configure the equation to to calculate concentration
    /*
      Exponential regression:
      Gas    | a      | b
      H2     | 987.99 | -2.162
      LPG    | 574.25 | -2.222
      CO     | 36974  | -3.109
      Alcohol| 3616.1 | -2.675
      Propane| 658.71 | -2.168
    */

    type.init();
    /*
      //If the RL value is different from 10K please assign your RL value with the following method:
      MQ2.setRL(10);
    */
    /*****************************  MQ CAlibration ********************************************/
    // Explanation:
    // In this routine the sensor will measure the resistance of the sensor supposedly before being pre-heated
    // and on clean air (Calibration conditions), setting up R0 value.
    // We recomend executing this routine only on setup in laboratory conditions.
    // This routine does not need to be executed on each restart, you can load your R0 value from eeprom.
    // Acknowledgements: https://jayconsystems.com/blog/understanding-a-gas-sensor

    Serial.print("\nCalibrating " + String(name) + "...please wait.");
    float calcR0 = 0;
    for(int i = 1; i<=10; i ++)
    {
        type.update(); // Update data, the arduino will read the voltage from the analog pin
        calcR0 += type.calibrate(RatioMQ2CleanAir);
        Serial.print(".");
    }
    type.setR0(calcR0/10);
    Serial.println("  done!.");


    if(isinf(calcR0)) {Serial.println("Warning: Connection issue, R0 is infinite (Open circuit detected) please check your wiring and supply"); while(1);}
    if(calcR0 == 0){Serial.println("Warning: Connection issue found, R0 is zero (Analog pin shorts to ground) please check your wiring and supply"); while(1);}
    /*****************************  MQ CAlibration ********************************************/
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

    if (httpResponseCode > 0) {
        String response = http.getString();
        Serial.print(httpResponseCode);
        Serial.print(": ");
        Serial.println(response);
    } else {
        Serial.print("Fehler beim Senden des POST: ");
        Serial.println(httpResponseCode);
    }

    Serial.println("");

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
 * Liest Sensordaten
 */
float readMQ2SensorData(MQUnifiedsensor &type) {
    type.update(); // Update data, the arduino will read the voltage from the analog pin

    return type.readSensor();
}

/**
 * Sendet Sensordaten an den Server
 */
void sendSensorData() {
    SHT31D result = sht3xd.readTempAndHumidity(SHT3XD_REPEATABILITY_HIGH, SHT3XD_MODE_CLOCK_STRETCH, 60000);
    float temperature = result.t;
    float humidity = result.rh;
    float voc = analogRead(A0); // MQ-2-Sensor ist an Serial-Port 0 angeschlossen

    float h2 = readMQ2SensorData(MQ2_H2);
    float lpg = readMQ2SensorData(MQ2_LPG);
    float co = readMQ2SensorData(MQ2_CO);
    float alcohol = readMQ2SensorData(MQ2_Alcohol);
    float propane = readMQ2SensorData(MQ2_Propane);

    String timestamp = getFormattedTime();
    String uniqueID = getUniqueID();

    Serial.println("Current Sensor Data (" + String(uniqueID) + " - " + String(timestamp) + ") -> Temperature: " + String(temperature, 1) + ", Humidity: " + String(humidity, 1) +
                   ", VOC: " + String(voc, 1) + ", H2: " + String(h2, 1) + ", LPG: " + String(lpg, 1) + ", CO: " + String(co, 1) +
                   ", Alcohol: " + String(alcohol, 1) + ", Propane: " + String(propane, 1));

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

    JsonObject vocJson = measurements.createNestedObject();
    vocJson["type"] = "VOC";
    vocJson["value"] = String(voc, 0);
    vocJson["unit"] = "PPM";

    JsonObject h2Json = measurements.createNestedObject();
    h2Json["type"] = "H2";
    h2Json["value"] = String(h2, 1);
    h2Json["unit"] = "PPM";

    JsonObject lpgJson = measurements.createNestedObject();
    lpgJson["type"] = "LPG";
    lpgJson["value"] = String(lpg, 1);
    lpgJson["unit"] = "PPM";

    JsonObject coJson = measurements.createNestedObject();
    coJson["type"] = "CO";
    coJson["value"] = String(co, 1);
    coJson["unit"] = "PPM";

    JsonObject alcoholJson = measurements.createNestedObject();
    alcoholJson["type"] = "ALCOHOL";
    alcoholJson["value"] = String(alcohol, 1);
    alcoholJson["unit"] = "PPM";

    JsonObject propaneJson = measurements.createNestedObject();
    propaneJson["type"] = "PROPANE";
    propaneJson["value"] = String(propane, 1);
    propaneJson["unit"] = "PPM";

    performHttpRequest(URL_MEASUREMENTS, doc);
}

/**
 * Initialisiert das Gerät
 */
void setup() {
    Serial.begin(115200);
    Wire.begin();
    sht3xd.begin(0x44);

    calibrateMQ2Sensor(MQ2_H2, "MQ2_H2", 987.99, -2.162);
    calibrateMQ2Sensor(MQ2_LPG, "MQ2_LPG", 574.25, -2.222);
    calibrateMQ2Sensor(MQ2_CO, "MQ2_CO", 36974, -3.109);
    calibrateMQ2Sensor(MQ2_Alcohol, "MQ2_Alcohol", 3616.1, -2.675);
    calibrateMQ2Sensor(MQ2_Propane, "MQ2_Propane", 658.71, -2.168);

    connectToWiFi();

    confirmSensorRegistration(); // Bestätige die Registrierung des Sensors nach der WiFi-Verbindung
}

/**
 * Hauptschleife des Programms
 */
void loop() {
    unsigned long currentMillis = millis();

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
}
