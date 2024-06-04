#include <WiFiClientSecure.h>

WiFiClientSecure client;

void initializeSSL() {
    client.setInsecure(); // You can replace this with `client.setCACert(cert);` if you have the CA certificate.
}

bool connectToServer(const char* host, uint16_t port) {
    if (!client.connect(host, port)) {
        Serial.println("Connection failed!");
        return false;
    }
    Serial.println("Connected to server.");
    return true;
}

int sendRequest(const char* request) {
    client.print(request);
    return client.println(request);
}

String readResponse() {
    String response = "";
    while (client.connected()) {
        while (client.available()) {
            response += client.readStringUntil('\n');
        }
    }
    return response;
}