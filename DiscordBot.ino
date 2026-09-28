#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_wifi.h>

// --- Configuration ---

const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";
const char* botToken = "YOUR_DISCORD_BOT_TOKEN";
const char* BotUsername = "YourBotUsername";
const char* imageLibraryChannelId = "Your_channel_with_saved_images_ID_here"; 	//alternatively, jump to !pic command 
										//and set the ID string there directly to save memory

WebSocketsClient webSocket;
unsigned long lastHeartbeat = 0;
unsigned long heartbeatInterval = 40000; // Fallback default (Discord explicitly sends this value on connect)
bool authenticated = false;
int lastSequenceNumber = 0;
WiFiClientSecure secureClient;

void sendDiscordMessage(String channelId, String textContent) {
    HTTPClient http;
    
    // Discord Create Message endpoint API URL
    
    http.begin(secureClient, String("https://discord.com/api/v10/channels/" + channelId + "/messages"));
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
}

void sendRandomImageFromChannel(String sourceChannelId, String destinationChannelId) {
    setCpuFrequencyMhz(160); //Increases CPU speed so it works faster
    delay(10);
    HTTPClient http;
    
    // Request the last 50 messages from the source channel
    //String url = "https://discord.com/api/v10/channels/" + sourceChannelId + "/messages?limit=50";
    
    http.begin(secureClient, String("https://discord.com/api/v10/channels/" + sourceChannelId + "/messages?limit=50"));
    http.addHeader("Authorization", "Bot " + String(botToken));
    
    int httpResponseCode = http.GET();
    
    if (httpResponseCode == 200) {
        String response = http.getString();
        
        // Allocate enough memory for a history payload. 
        // 50 messages can be large, so we use a dynamic document.
        DynamicJsonDocument doc(24576); 
        DeserializationError error = deserializeJson(doc, response);
        
        if (!error && doc.is<JsonArray>()) {
            JsonArray messages = doc.as<JsonArray>();
            
            //you can create an array/list to hold found image URLs, but that uses a lot of memory
            //String imageUrls[50];
            String randomUrl = ""; //only saves one link to save memory
            int imageCount = 0;
            
            // Step 1: Scan the history for images
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
            
            // Step 2: Pick a random image if any were found
            if (imageCount > 0) {
                Serial.printf("Found %d images.", imageCount);
                
                // Forward the picked image to the destination channel
                sendDiscordMessage(destinationChannelId, randomUrl);
            } else {
                sendDiscordMessage(destinationChannelId, "Error: No image attachments found in the source channel history.");
            }
        } else {
            Serial.printf("JSON parse failed: %s\n", error.c_str());
        }
    } else {
        Serial.printf("[HTTP] Failed to fetch history. Code: %d\n", httpResponseCode);
    }
    
    http.end();
    setCpuFrequencyMhz(80); //Decreases CPU speed to save battery
    delay(10); //Delay to avoid a crash
    lastHeartbeat = millis(); //resync
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
                            sendRandomImageFromChannel(imageLibraryChannelId, channelId);
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
    secureClient.setInsecure();
    webSocket.beginSSL("gateway.discord.gg", 443, "/?v=10&encoding=json");
    webSocket.onEvent(webSocketEvent);
    
    // Enable reconnection parameters if connection drops
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