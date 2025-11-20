#include "mqtt.h"

static WiFiClientSecure espClient;
static PubSubClient mqttClient(espClient);
String user = "elev1";
String pass = "password";
bool MQTTIsSetup = false;

bool mqtt_setup()
{
    // Configure MQTT over TLS
    espClient.setInsecure();
    // espClient.setCACert(MQTT_CA_CERT);
    mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
    return true;
}

void mqtt_loop()
{
    // Only run if WiFi is connected
    if (!ConnectedToWifi)
    {
        return;
    }

    // Ensure MQTT connection
    if (!mqttClient.connected())
    {
        mqtt_reconnect();
    }
    mqttClient.loop();
}

void mqtt_reconnect()
{
    // Loop until reconnected
    while (!mqttClient.connected())
    {
        Serial.print("Connecting to MQTT over TLS...");

        String clientId = "ESP32-" + String(random(0xffff), HEX);

        if (mqttClient.connect(clientId.c_str(), user.c_str(), pass.c_str()))
        {
            Serial.println("connected");
        }
        else
        {
            Serial.print("failed, rc=");
            Serial.print(mqttClient.state());
            Serial.println(" - retrying in 5 seconds");
            delay(5000);
        }
    }
}

bool mqtt_publish(const String &payload)
{
    // Ensure MQTT connection
    if (!mqttClient.connected())
    {
        mqtt_reconnect();
    }
    
    // Publish the message
    return mqttClient.publish(MQTT_TOPIC, payload.c_str());
}
