#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_wifi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>

#include "config.h" // Loads Wifi and Discord credentials

WebSocketsClient webSocket;
unsigned long lastHeartbeat = 0;
unsigned long heartbeatInterval = 41250;
bool authenticated = false;
int lastSequenceNumber = 0;

void sendDiscordMessage(String channelId, String textContent) {
    WiFiClientSecure secureClient;
    secureClient.setInsecure();
    secureClient.setTimeout(5000);
    
    // Establish a raw secure TCP connection directly to discord.com
    if (!secureClient.connect("discord.com", 443)) {
        Serial.println("[TCP] Connection to Discord failed.");
        return;
    }
    
    // Package the message payload into a minified JSON string
    JsonDocument replyDoc;
    replyDoc["content"] = textContent;
    String requestBody;
    serializeJson(replyDoc, requestBody);

    // Manually construct and stream the HTTP POST headers and body over the socket
    secureClient.print("POST /api/v10/channels/" + channelId + "/messages HTTP/1.1\r\n");
    secureClient.print("Host: discord.com\r\n");
    secureClient.print("Authorization: Bot " + String(botToken) + "\r\n");
    secureClient.print("Content-Type: application/json\r\n");
    secureClient.print("Content-Length: " + String(requestBody.length()) + "\r\n");
    secureClient.print("Connection: close\r\n\r\n"); // Close the socket immediately after response
    secureClient.print(requestBody);

    // Read the first line of the response to verify success (Optional debug)
    while (secureClient.connected()) {
        String line = secureClient.readStringUntil('\n');
        if (line.startsWith("HTTP/1.1")) {
            Serial.printf("[TCP Response]: %s\n", line.c_str());
            break;
        }
    }
    
    // Clean up the socket
    secureClient.stop();
}

void sendRandomImageFromChannel(String sourceChannelId, String destinationChannelId) {
    setCpuFrequencyMhz(160); //Increases CPU speed so it works faster
    delay(10);
    //Serial.println(getCpuFrequencyMhz()); 
    WiFiClientSecure secureClient;
    secureClient.setInsecure();
    secureClient.setTimeout(5000);

    if (!secureClient.connect("discord.com", 443)) {
        Serial.println("[TCP] GET Connection to Discord failed.");
        return;
    }

    // Send manual HTTP GET headers to retrieve the last 50 messages
    // This replicates: https://discord.com{id}/messages?limit=50
    secureClient.print("GET /api/v10/channels/" + sourceChannelId + "/messages?limit=50 HTTP/1.0\r\n");
    secureClient.print("Host: discord.com\r\n");
    secureClient.print("Authorization: Bot " + String(botToken) + "\r\n");
    secureClient.print("Connection: close\r\n\r\n");

    // Skip past the incoming HTTP Headers to find the JSON body payload
    // An HTTP response body is always separated from its headers by an empty line (\r\n)
    while (secureClient.connected()) {
        String line = secureClient.readStringUntil('\n');
        if (line == "\r") { 
            break; // Header section ended, the next incoming data is our JSON array
        }
    }

    // Stream and parse the JSON string array on-the-fly directly from the network socket
    JsonDocument doc;
    
    String response = "";
    while (secureClient.available() || secureClient.connected()) {
        if (secureClient.available()) {
            char c = secureClient.read();
            response += c;
        }
    }
    DeserializationError error = deserializeJson(doc, response);

    // Done reading from the network, close socket immediately to release memory
    secureClient.stop();

    if (error) {
        Serial.print("[JSON] Parse failed: ");
        Serial.println(error.c_str());
        return;
    }

    // Gather all valid image attachment URLs from the 50 messages
    JsonArray messages = doc.as<JsonArray>();
    String randomUrl = ""; //only saves one link to save memory
    int imageCount = 0;

    for (JsonObject msg : messages) {
        if (msg.containsKey("attachments")) {
            JsonArray attachments = msg["attachments"].as<JsonArray>();
            for (JsonObject attachment : attachments) {
                const char* urlStr = attachment["url"];
                if(urlStr != nullptr)
                {
                    imageCount++;
                    if (random(0, imageCount) == 0) {
                        randomUrl = String(urlStr); 
                    }

                    if (imageCount >= 50) break;
                }
            }
        }
        if (imageCount >= 50) break;
    }

    // Select a random image from the vector list and forward it back to chat
    if (imageCount > 0) {
        Serial.printf("Found %d images.\n", imageCount);
                
        // Forward the picked image to the destination channel
        sendDiscordMessage(destinationChannelId, randomUrl);
    } else {
        sendDiscordMessage(destinationChannelId, "Error: No image attachments found in the source channel history.");
    }
    
    setCpuFrequencyMhz(80); //Decreases CPU speed to save battery
    delay(10); //Delay to avoid a crash
    //Serial.println(getCpuFrequencyMhz()); 
}

// Sends the identity payload (Handshake) to Discord to authorize the bot
void sendDiscordIdentity() {
    JsonDocument doc;   
    doc["op"] = 2; // Identify
    
    JsonObject d = doc["d"].to<JsonObject>();
    d["token"] = "Bot " + String(botToken);
    

    d["intents"] = 37376; // GUILD_MESSAGES (1 << 9) + DIRECT_MESSAGES (1 << 12) + MESSAGE_CONTENT (1 << 15)
                          // Check the README!
    
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
    //Serial.println("Heartbeat pulse transmitted.");
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
                //Serial.println("Heartbeat acknowledged by Discord.");
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
                        if (String(content).equalsIgnoreCase("!on")) {
                            digitalWrite(2, HIGH);
                            Serial.println("GPIO Pin 2 driven HIGH via Discord command.");
                            sendDiscordMessage(channelId, "Status Update: The built-in LED has been turned ON.");
                        } 
                        else if (String(content).equalsIgnoreCase("!off")) {
                            digitalWrite(2, LOW);
                            Serial.println("GPIO Pin 2 driven LOW via Discord command.");
                            sendDiscordMessage(channelId, "Status Update: The built-in LED has been turned OFF.");
                        }
                        else if (String(content).equalsIgnoreCase("!pic")) {
                            // Fetches a random image from the library and sends it back to the active channel
                            //sendRandomImageFromChannel(imageLibraryChannelId, channelId);
                            sendRandomImageFromChannel(SourceID, channelId);
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
    randomSeed(esp_random()); 
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

    webSocket.setReconnectInterval(5000);
    esp_wifi_set_ps(WIFI_PS_MAX_MODEM); //Maximum modem power saving
    setCpuFrequencyMhz(80); //80mhz to save battery 
}

void loop() {
    // Keep internal WebSocket polling routines alive
    webSocket.loop();

    // Dynamically handle the heartbeat intervals to prevent Discord dropping the connection
    if (authenticated && (millis() - lastHeartbeat >= heartbeatInterval)) {
        sendDiscordHeartbeat();
    }
}