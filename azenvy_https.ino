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
const char *HOST = "192.168.1.6"; // Server IP
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

ClosedCube_SHT31D sht3xd; // SHT31D sensor
AsyncWebServer server(80); // AsyncWebServer for WebSerial

/**
 * Connect to WiFi
 */
void connectToWiFi() {
    WiFi.mode(WIFI_STA);
    Serial.println("");

    if (strcmp(WIFI_PROVIDER, "WPS") == 0) {
        Serial.println("Starting WPS connection...");
        WiFi.persistent(true);
        WiFi.beginWPSConfig();
        // Wait for connection to be established
        while (WiFi.status() != WL_CONNECTED) {
            delay(500);
            Serial.print(".");
        }
        // Save the WiFi connection permanently
        if (WiFi.SSID().length() > 0) {
            Serial.println("WPS connection established. Saving WiFi connection...");
            WiFi.persistent(true);
            WiFi.begin(WiFi.SSID().c_str(), WiFi.psk().c_str());
        }
    } else {
        Serial.print("Connecting to ");
        Serial.println(SSID);
        WiFi.begin(SSID, PASSWORD);
    }

    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }

    Serial.println("WiFi connected.");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    Serial.print("RSSI: ");
    Serial.println(WiFi.RSSI());

    // Initialize NTP
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
}

/**
 * Calibrate the MQ2 sensor with the given parameters
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
    /*****************************  MQ Calibration ********************************************/
}

/**
 * Format the current time
 * @return Current time as string
 */
String getFormattedTime() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        Serial.println("Time could not be retrieved");
        return "";
    }
    char timeStringBuff[50];
    strftime(timeStringBuff, sizeof(timeStringBuff), "%Y-%m-%dT%H:%M:%S%z", &timeinfo);
    return String(timeStringBuff);
}

/**
 * Generate a unique sensor ID
 * @return Unique sensor ID as string
 */
String getUniqueID() {
    char uniqueID[UniqueIDsize * 2 + 1];
    for (size_t i = 0; i < UniqueIDsize; i++) {
        sprintf(uniqueID + i * 2, "%02X", UniqueID[i]);
    }
    return String(uniqueID);
}

/**
 * Perform an HTTP/HTTPS request
 * @param url URL of the endpoint
 * @param doc JSON document to send
 * @return HTTP response code
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

    Serial.println("-- Endpoint URL --");
    Serial.println(urlString);
    Serial.println("-- Payload --");
    serializeJsonPretty(doc, Serial);
    Serial.println("\n-- Response --");

    // Output to WebSerial
    WebSerial.println("-- Endpoint URL --");
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
        Serial.print("Error sending POST:")
        Serial.println(httpResponseCode);

        WebSerial.print("Error sending POST:")
        WebSerial.println(httpResponseCode);
    }

    Serial.println("");
    WebSerial.println("");

    http.end();
    delete client;
    return httpResponseCode;
}

/**
 * Confirm sensor registration
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
 * Read the MQ2 sensor data
 */
float readMQ2SensorData(MQUnifiedsensor &type) {
    type.update(); // Update data, the arduino will read the voltage from the analog pin

    return type.readSensor();
}

/**
 * Send sensor data to the server
 */
void sendSensorData() {
    SHT31D result = sht3xd.readTempAndHumidity(SHT3XD_REPEATABILITY_HIGH, SHT3XD_MODE_CLOCK_STRETCH, 60000);
    float temperature = result.t;
    float humidity = result.rh;
    float voc = analogRead(A0); // MQ-2-Sensor is connected to Serial Port 0

    float h2 = readMQ2SensorData(MQ2_H2);
    float lpg = readMQ2SensorData(MQ2_LPG);
    float co = readMQ2SensorData(MQ2_CO);
    float alcohol = readMQ2SensorData(MQ2_Alcohol);
    float propane = readMQ2SensorData(MQ2_Propane);

    String timestamp = getFormattedTime();
    String uniqueID = getUniqueID();

    WebSerial.println("Current Sensor Data (" + String(uniqueID) + " - " + String(timestamp) + ") -> Temperature: " + String(temperature, 1) + ", Humidity: " + String(humidity, 1) +
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

    JsonObject gasJson = measurements.createNestedObject();
    gasJson["type"] = "GAS";
    gasJson["value"] = voc;
    gasJson["unit"] = "PPM";

    /*
    JsonObject vocJson = measurements.createNestedObject();
    vocJson["type"] = "VOC";
    vocJson["value"] = voc;
    vocJson["unit"] = "PPM";

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
    */

    performHttpRequest(URL_MEASUREMENTS, doc);
}

/**
 * Initialize ArduinoOTA
 */
void initializeArduinoOTA() {
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
}

/**
 * Initialize WebSerial
 */
void initializeWebSerial() {
    // WebSerial starten
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "text/plain", "You can access webserial interface at http://" +
                                         WiFi.localIP().toString() + "/webserial");
        Serial.println(
                "You can access webserial interface at http://" + WiFi.localIP().toString() +
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
}

/**
 * Initialize the device
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
    initializeArduinoOTA();
    initializeWebSerial();

    confirmSensorRegistration(); // Confirm sensor registration on server side
}

/**
 * Main loop
 */
void loop() {
    unsigned long currentMillis = millis();

    ArduinoOTA.handle();

    // Check WiFi connection every x seconds and reconnect if necessary
    if ((WiFi.status() != WL_CONNECTED) && (currentMillis - previousMillis >= interval)) {
        Serial.print(millis());
        Serial.println("Reconnecting to WiFi...");
        WiFi.disconnect();
        WiFi.reconnect();
        previousMillis = currentMillis;
    }

    // Send sensor data every x seconds
    if ((WiFi.status() == WL_CONNECTED) && (currentMillis - previousMillis >= send_sensor_data_interval)) {
        sendSensorData();
        previousMillis = currentMillis;
    }
    WebSerial.loop();
}
