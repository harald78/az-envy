/*** Includes ***/
#include <ESP8266WiFi.h>
#include <Wire.h>
#include <ClosedCube_SHT31D.h>
#include <ESP8266HTTPClient.h>
#include <time.h>
#include <ArduinoJson.h>
#include <ArduinoUniqueID.h>
#include <MQUnifiedsensor.h>

/*** User Configuration ***/
const char *ssid = "wifi-ssid";         // WiFi SSID
const char *psk = "wifi-password";      // WiFi password
const char *host = "your-host";         // Server IP
const uint16_t port = 8080;             // Server port
const char *username = "default";       // Username
const char *fingerprint = "";           // SHA-1 fingerprint of the server certificate
const char *apiKey = "your-api-key";    // API key

/*** API Configuration ***/
const String urlRegisterSensorConfirm = String(host) + ":" + port + "/api/sensor/register/confirm"; // URL for sensor registration confirmation
const String urlMeasurements = String(host) + ":" + port + "/api/sensor/measurements";              // URL for sensor measurements
const char *applicationJson = "application/json";
const char *apiKeyHeaderName = "X-API-KEY"; // Name of the API key header
bool sensorRegistered = false;

/*** Timing Configuration ***/
unsigned long initialMillis = millis();   // Initial time
const int interval = 30000;                     // Interval for checking WiFi connection (30 seconds)
const int sendSensorDataInterval = 60000;       // Interval for sending sensor data (60 seconds)
const int maxConnectAttempts = 60;

/*** NTP Configuration ***/
const char *ntpServer = "pool.ntp.org";
const long gmtOffsetSec = 3600;      // Offset for GMT+1
const int daylightOffsetSec = 3600;  // Offset for daylight saving time

/*** Board Configuration ***/
const char *board = "ESP8266";
const int pin = A0;          // Analog input 0 of your ESP8266
const int wpsButton = 0;     // Use FLASH button as WPS button
const char *type = "MQ-2";   // MQ2
const int voltageResolution = 5;
const int adcBitResolution = 10;        // For Arduino UNO/MEGA/NANO
const float ratioMQ2CleanAir = 9.83;    // RS / R0 = 9.83 ppm

/*** Sensor Calibration Constants ***/
const float MQ2_H2_A = 987.99;
const float MQ2_H2_B = -2.162;
const float MQ2_LPG_A = 574.25;
const float MQ2_LPG_B = -2.222;
const float MQ2_CO_A = 36974;
const float MQ2_CO_B = -3.109;
const float MQ2_ALCOHOL_A = 3616.1;
const float MQ2_ALCOHOL_B = -2.675;
const float MQ2_PROPANE_A = 658.71;
const float MQ2_PROPANE_B = -2.168;

/*** Globals ***/
ClosedCube_SHT31D sht3xd;
MQUnifiedsensor mq2H2(board, voltageResolution, adcBitResolution, pin, type);
MQUnifiedsensor mq2LPG(board, voltageResolution, adcBitResolution, pin, type);
MQUnifiedsensor mq2CO(board, voltageResolution, adcBitResolution, pin, type);
MQUnifiedsensor mq2Alcohol(board, voltageResolution, adcBitResolution, pin, type);
MQUnifiedsensor mq2Propane(board, voltageResolution, adcBitResolution, pin, type);

/**
 * Logs messages to Serial
 */
void logMessage(const String &message)
{
    Serial.println(message);
}

/**
 * Starts WPS Configuration
 */
bool startWPS()
{
    logMessage("Starting WPS Configuration");
    bool wpsSuccess = WiFi.beginWPSConfig();
    if (wpsSuccess)
    {
        // It doesn’t always have to be successful! After a timeout, the SSID is empty.
        String newSSID = WiFi.SSID();
        if (newSSID.length() > 0)
        {
            // Only if an SSID was found, were we successful.
            logMessage("WPS done. Successfully connected to SSID '" + newSSID + "'");
        }
        else
        {
            wpsSuccess = false;
        }
    }
    return wpsSuccess;
}

/**
 * Reconnects WiFi
 */
void reconnectWiFi()
{
    if (WiFi.status() != WL_CONNECTED)
    {
        logMessage("Reconnecting to WiFi...");
        WiFi.disconnect();
        WiFi.reconnect();
    }
}

/**
 * Connects to WiFi
 */
void connectToWiFi()
{
    int connectAttempts = 0;

    WiFi.mode(WIFI_STA);

    if (WiFi.SSID().length() > 0)
    {
        logMessage("\nAttempting connection with saved SSID '" + WiFi.SSID() + "'");
        WiFi.begin(WiFi.SSID().c_str(), WiFi.psk().c_str());    // Last saved credentials
    }
     else if (strlen(ssid) > 0)
    {
        logMessage("Attempting connection with configured SSID '" + String(ssid) + "'");
        WiFi.begin(ssid, psk);                                  // Configured credentials
    }
    else
    {
        logMessage("No SSID saved or configured. Press WPS button on your router and FLASH button on your AZ Envy.");
        return;
    }

    while ((WiFi.status() == WL_DISCONNECTED) && (connectAttempts <= maxConnectAttempts))
    {
        delay(500);
        Serial.print(".");
        connectAttempts += 1;
    }

    if (WiFi.status() == WL_CONNECTED)
    {
        logMessage("Successfully connected to SSID '" + WiFi.SSID() + "'");
    }
    else
    {
        logMessage("Cannot establish WiFi connection. Status = '" + String(WiFi.status()) + "'");
        logMessage("Press WPS button on your router and FLASH button on your AZ Envy.");
        while (digitalRead(wpsButton) != 0)
        {
            yield();
        }
        if (!startWPS())
        {
            logMessage("Cannot establish connection via WPS");
        }
    }

    logMessage("IP address: " + WiFi.localIP().toString());
    configTime(gmtOffsetSec, daylightOffsetSec, ntpServer); // Initialize NTP
}

/**
 * Calibrates the sensor and outputs important values
 */
void calibrateMQ2Sensor(MQUnifiedsensor &sensor, const char *name, float a, float b)
{
    sensor.setRegressionMethod(1);
    sensor.setA(a);
    sensor.setB(b);
    sensor.init();
    Serial.print("Calibrating " + String(name) + "...please wait.");
    float calcR0 = 0;
    const int calibrationRounds = 10;
    for (int i = 1; i <= calibrationRounds; i++)
    {
        sensor.update();
        calcR0 += sensor.calibrate(ratioMQ2CleanAir);
        Serial.print(".");
    }
    sensor.setR0(calcR0 / calibrationRounds);
    Serial.println("  done!");

    if (isinf(calcR0))
    {
        logMessage("Warning: Connection issue, R0 is infinite (Open circuit detected) please check your wiring and supply");
        while (1)
            ;
    }
    if (calcR0 == 0)
    {
        logMessage("Warning: Connection issue found, R0 is zero (Analog pin shorts to ground) please check your wiring and supply");
        while (1)
            ;
    }
}

/**
 * Formats the current time
 * @return The formatted time as a string
 */
String getFormattedTime()
{
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo))
    {
        logMessage("Failed to retrieve time");
        return "";
    }
    char timeStringBuff[50];
    strftime(timeStringBuff, sizeof(timeStringBuff), "%Y-%m-%dT%H:%M:%S%z", &timeinfo);
    return String(timeStringBuff);
}

/**
 * Generates a unique sensor ID
 * @return The unique sensor ID as a string
 */
String getUniqueID()
{
    char uniqueID[UniqueIDsize * 2 + 1];
    for (size_t i = 0; i < UniqueIDsize; i++)
    {
        sprintf(uniqueID + i * 2, "%02X", UniqueID[i]);
    }
    return String(uniqueID);
}

/**
 * Performs an HTTP/HTTPS request
 * @param url The URL of the request
 * @param doc The JSON document to be sent
 * @return The HTTP response code
 */
int performHttpRequest(const String &url, StaticJsonDocument<200> &doc)
{
    HTTPClient http;
    WiFiClient *client;
    String urlString;

    bool useHttps = (strlen(fingerprint) > 0);

    if (useHttps)
    {
        WiFiClientSecure *secureClient = new WiFiClientSecure;
        secureClient->setFingerprint(fingerprint);
        client = secureClient;
        urlString = "https://" + url;
    }
    else
    {
        WiFiClient *regularClient = new WiFiClient;
        client = regularClient;
        urlString = "http://" + url;
    }

    http.begin(*client, urlString);
    http.addHeader("Content-Type", applicationJson, true, true);
    http.addHeader(apiKeyHeaderName, apiKey, false, false);

    String payload;
    serializeJson(doc, payload);

    int httpResponseCode = http.POST(payload);

    logMessage("-- Endpoint URL --\n" + urlString + "\n-- Payload --");
    serializeJsonPretty(doc, Serial);
    logMessage("\n-- Response --\n");

    if (httpResponseCode > 0)
    {
        String response = http.getString();
        logMessage(String(httpResponseCode) + ": " + response);
    }
    else
    {
        logMessage("Error sending POST: " + String(httpResponseCode));
    }

    logMessage("");
    http.end();
    return httpResponseCode;
}

/**
 * Confirms sensor registration
 */
void confirmSensorRegistration()
{
    int sensorResult = 0;

    if (WiFi.status() == WL_CONNECTED)
    {
        StaticJsonDocument<200> doc;
        doc["username"] = username;
        doc["uuid"] = getUniqueID();

        sensorResult = performHttpRequest(urlRegisterSensorConfirm, doc);
    }

    if (sensorResult == 200)
    {
        sensorRegistered = true;
    }
}

/**
 * Reads sensor data
 */
float readMQ2SensorData(MQUnifiedsensor &sensor)
{
    sensor.update();
    return sensor.readSensor();
}

/**
 * Sends sensor data to the server
 */
void sendSensorData()
{
    SHT31D result = sht3xd.readTempAndHumidity(SHT3XD_REPEATABILITY_HIGH, SHT3XD_MODE_CLOCK_STRETCH, 60000);
    float temperature = result.t;
    float humidity = result.rh;
    float voc = analogRead(A0); // MQ-2 sensor is connected to Serial Port 0

    // Calibrate each time to prevent extremely wrong values
    calibrateMQ2Sensor(mq2H2, "MQ2_H2", MQ2_H2_A, MQ2_H2_B);
    calibrateMQ2Sensor(mq2LPG, "MQ2_LPG", MQ2_LPG_A, MQ2_LPG_B);
    calibrateMQ2Sensor(mq2CO, "MQ2_CO", MQ2_CO_A, MQ2_CO_B);
    calibrateMQ2Sensor(mq2Alcohol, "MQ2_Alcohol", MQ2_ALCOHOL_A, MQ2_ALCOHOL_B);
    calibrateMQ2Sensor(mq2Propane, "MQ2_Propane", MQ2_PROPANE_A, MQ2_PROPANE_B);

    float h2 = readMQ2SensorData(mq2H2);
    float lpg = readMQ2SensorData(mq2LPG);
    float co = readMQ2SensorData(mq2CO);
    float alcohol = readMQ2SensorData(mq2Alcohol);
    float propane = readMQ2SensorData(mq2Propane);

    String timestamp = getFormattedTime();
    String uniqueID = getUniqueID();

    logMessage("Current Sensor Data (" + uniqueID + " - " + timestamp +
                   ") -> Temperature: " + String(temperature, 1) + ", Humidity: " + String(humidity, 1) +
                   ", VOC: " + String(voc, 1) + ", H2: " + String(h2, 1) + ", LPG: " + String(lpg, 1) +
                   ", CO: " + String(co, 1) + ", Alcohol: " + String(alcohol, 1) + ", Propane: " + String(propane, 1));

    StaticJsonDocument<200> doc;
    doc["base"] = "AZEnvy";
    doc["timestamp"] = timestamp;
    doc["id"] = uniqueID;

    JsonArray measurements = doc.createNestedArray("measurements");

    JsonObject temperatureJson = measurements.createNestedObject();
    temperatureJson["type"] = "TEMPERATURE";
    temperatureJson["value"] = temperature;
    temperatureJson["unit"] = "CELSIUS";

    JsonObject humidityJson = measurements.createNestedObject();
    humidityJson["type"] = "HUMIDITY";
    humidityJson["value"] = humidity;
    humidityJson["unit"] = "PERCENT";

    JsonObject vocJson = measurements.createNestedObject();
    vocJson["type"] = "VOC";
    vocJson["value"] = voc;
    vocJson["unit"] = "PPB";

    JsonObject h2Json = measurements.createNestedObject();
    h2Json["type"] = "H2";
    h2Json["value"] = h2;
    h2Json["unit"] = "PPM";

    JsonObject lpgJson = measurements.createNestedObject();
    lpgJson["type"] = "LPG";
    lpgJson["value"] = lpg;
    lpgJson["unit"] = "PPM";

    JsonObject coJson = measurements.createNestedObject();
    coJson["type"] = "CO";
    coJson["value"] = co;
    coJson["unit"] = "PPM";

    JsonObject alcoholJson = measurements.createNestedObject();
    alcoholJson["type"] = "ALCOHOL";
    alcoholJson["value"] = alcohol;
    alcoholJson["unit"] = "PPM";

    JsonObject propaneJson = measurements.createNestedObject();
    propaneJson["type"] = "PROPANE";
    propaneJson["value"] = propane;
    propaneJson["unit"] = "PPM";

    performHttpRequest(urlMeasurements, doc);
}

/**
 * Initializes the device
 */
void setup()
{
    Serial.begin(115200);
    Wire.begin();
    sht3xd.begin(0x44);

    pinMode(wpsButton, INPUT_PULLUP); // Enable button input

    connectToWiFi();

    if (WiFi.status() == WL_CONNECTED) {
        confirmSensorRegistration();

        if (sensorRegistered)
        {
            sendSensorData();
        }
    }
}

/**
 * Main program loop
 */
void loop()
{
    unsigned long currentMillis = millis();

    if ((currentMillis - initialMillis >= sendSensorDataInterval) && (WiFi.status() == WL_CONNECTED))
    {
        if (!sensorRegistered) {
            confirmSensorRegistration(); // Confirm sensor registration after WiFi connection
        } else {
            sendSensorData();
        }
        initialMillis = currentMillis;
    }

    // Check the WiFi connection every 30 seconds and reconnect if disconnected
    if ((WiFi.status() != WL_CONNECTED) && (currentMillis - initialMillis >= interval))
    {
        logMessage(String(millis()) + ": Reconnecting to WiFi...");
        reconnectWiFi();
        initialMillis = currentMillis;
    }
}
