#include <WiFi.h>
#include <WebSocketsClient.h>  // From Markus Sattler's library
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_wifi.h>

// --- Configuration ---
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";
const char* botToken = "YOUR_DISCORD_BOT_TOKEN";
const char* BotUsername = "YourBotUsername";

WebSocketsClient webSocket;

unsigned long lastHeartbeat = 0;
unsigned long heartbeatInterval = 40000; // Fallback default (Discord explicitly sends this value on connect)
bool authenticated = false;
int lastSequenceNumber = 0;


void sendDiscordMessage(String channelId, String textContent) {
    WiFiClientSecure *secureClient = new WiFiClientSecure();
    secureClient->setInsecure(); // Skips loading root SSL certificates to save ESP32 memory
    
    HTTPClient http;
    
    // Discord Create Message endpoint API URL
    String url = "https://discord.com/api/v10/channels/" + channelId + "/messages";
    
    http.begin(*secureClient, url);
    http.addHeader("Authorization", "Bot " + String(botToken));
    http.addHeader("Content-Type", "application/json");
    
    // Package our response text payload
    JsonDocument replyDoc;
    replyDoc["content"] = textContent;
    
    String requestBody;
    serializeJson(replyDoc, requestBody);
    
    // Execute the POST request
    int httpResponseCode = http.POST(requestBody);
    
    if (httpResponseCode > 0) {
        Serial.printf("[HTTP] Message sent! Code: %d\n", httpResponseCode);
    } else {
        Serial.printf("[HTTP] Error sending message: %s\n", http.errorToString(httpResponseCode).c_str());
    }
    
    http.end();
    delete secureClient; // Clean up memory allocation from the heap
}

// Sends the identity payload (Handshake) to Discord to authorize the bot
void sendDiscordIdentity() {
    JsonDocument doc;
    doc["op"] = 2; // Identify
    
    JsonObject d = doc["d"].to<JsonObject>();
    d["token"] = "Bot " + String(botToken);
    
    // --- UPDATE THIS EXACT LINE ---
    d["intents"] = 37376;
    
    JsonObject properties = d["properties"].to<JsonObject>();
    properties["$os"] = "esp32";
    properties["$browser"] = "arduino";
    properties["$device"] = "esp32";
    
    String output;
    serializeJson(doc, output);
    webSocket.sendTXT(output);
}


// Sends the heartbeat sequence required by Discord to prevent immediate disconnection
void sendDiscordHeartbeat() {
    JsonDocument doc;
    doc["op"] = 1; // Opcode 1: Heartbeat
    if (lastSequenceNumber == 0) {
        doc["d"] = nullptr;
    } else {
        doc["d"] = lastSequenceNumber;
    }
    
    String output;
    serializeJson(doc, output);
    webSocket.sendTXT(output);
    Serial.println("Heartbeat pulse transmitted.");
    lastHeartbeat = millis();
}

// The main event callback handler for Markus Sattler's library
void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
    switch(type) {
        case WStype_DISCONNECTED:
            Serial.println("[WS] Disconnected from Discord Gateway!");
            authenticated = false;
            break;
            
        case WStype_CONNECTED:
            Serial.printf("[WS] Connected to URL: %s\n", payload);
            // Secure connection established, but we must wait for Discord's Opcode 10 (Hello) packet 
            // before pushing the Identity configuration payload.
            break;
            
        case WStype_TEXT: {
            // Process the incoming message stream
            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, payload);
            if (error) {
                Serial.print("JSON deserialization error: ");
                Serial.println(error.c_str());
                return;
            }

            int op = doc["op"]; // Opcode
            
            // Keep track of the message sequence number if provided
            if (doc.containsKey("s") && !doc["s"].isNull()) {
                lastSequenceNumber = doc["s"];
            }

            // Opcode 10: HELLO (Sent by Discord immediately upon websocket connection)
            if (op == 10) {
                heartbeatInterval = doc["d"]["heartbeat_interval"];
                Serial.printf("Hello received. Heartbeat Interval configured to %lu ms\n", heartbeatInterval);
                
                // Immediately send first heartbeat pulse & request authentication identity
                sendDiscordHeartbeat();
                sendDiscordIdentity();
                authenticated = true;
            }
            
            // Opcode 11: HEARTBEAT ACK (Discord acknowledging our pulse)
            else if (op == 11) {
                Serial.println("Heartbeat acknowledged by Discord.");
            }
            
            // Opcode 0: Dispatch Events (Chat messages, server updates, etc.)
            else if (op == 0) {
                const char* eventType = doc["t"];
                
                if (String(eventType) == "MESSAGE_CREATE") {
                    const char* channelId = doc["d"]["channel_id"];
                    const char* content = doc["d"]["content"];
                    const char* username = doc["d"]["author"]["username"];
                    
                    // Prevent the bot from replying to its own code queries
                    if (String(username) != String(BotUsername)) {
                        Serial.printf("[%s]: %s\n", username, content);
                        
                        // Example Action: Trigger a physical LED pin from a chat command
                        if (String(content) == "!on") {
                            digitalWrite(2, HIGH);
                            Serial.println("GPIO Pin 2 driven HIGH via Discord command.");
                            sendDiscordMessage(channelId, "Status Update: The built-in LED has been turned ON.");
                        } 
                        else if (String(content) == "!off") {
                            digitalWrite(2, LOW);
                            Serial.println("GPIO Pin 2 driven LOW via Discord command.");
                            sendDiscordMessage(channelId, "Status Update: The built-in LED has been turned OFF.");
                        }
                    }
                }
            }
            break;
        }
        
        case WStype_BIN:
        case WStype_ERROR:
        case WStype_FRAGMENT_TEXT_START:
        case WStype_FRAGMENT_BIN_START:
        case WStype_FRAGMENT:
        case WStype_FRAGMENT_FIN:
            break;
    }
}

void setup() {
    Serial.begin(115200);
    setCpuFrequencyMhz(80); //80mhz to save battery 
    pinMode(2, OUTPUT); // Built-in Blue LED on standard dev kits

    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWi-Fi network connection achieved!");

    // Initialize the WebSocket client to access Discord's secure gateway over port 443
    // Note: Markus Sattler's library handles WSS wrapping implicitly on port 443 for ESP32.
    webSocket.beginSSL("gateway.discord.gg", 443, "/?v=10&encoding=json");
    webSocket.onEvent(webSocketEvent);
    
    // Enable reconnection parameters if connection drops
    webSocket.setReconnectInterval(5000);
    esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
}

void loop() {
    // Keep internal WebSocket polling routines alive
    webSocket.loop();

    // Dynamically handle the heartbeat intervals to prevent Discord dropping the connection
    if (authenticated && (millis() - lastHeartbeat >= heartbeatInterval)) {
        sendDiscordHeartbeat();
    }
}