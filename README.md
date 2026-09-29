### Hosting a Discord bot on ESP32

This is an open source, lightweight project to host a Discord Bot on a ESP32.

# Features: 

*  **Standalone Operation:** Runs Entirely on the ESP32 chip with zeor hosting fees. It connects directly to Discord, without needing a background server.
*  **Low Power Consumption:** Runs on low power and is designed to run on a battery for hours.
*  **Expandable I/O Control:** Ready to be programmable to listen your discord commands that affect the physical hardware peripherals connected to the ESP32.


## Dependencies

For this project, it is highly recommended you use Arduino IDE and install the ESP32 Development board for the following libraries:
* `WiFi.h`
* `HTTPClient.h`
* `WiFiClientSecure.h`
* `esp_wifi.h`

The following libraries can be downloaded through the Library Manager Tool that comes with ArduinoIDE:
* [WebSocketsClient.h](https://github.com/Links2004/arduinoWebSockets)
* [ArduinoJson.h](https://arduinojson.org)


## Getting Started

# Step 1: Cloning this Repository
Clone this repository by clicking to download the .zip file on this page.
Alternatively, open a terminal on the path you want to download it and type in the following command:
```bash
git clone https://github.com/clbs-progc/Discord-Bot-ESP32.git
```

# Step 2: Dependencies
Make sure you installed the Dependencies listed on this document.

# Step 3: Configuration setup
Open `config.h` in your text editor and change the following values inside the quotation

```cpp
#define ssid			"YOUR_WIFI_NAME"
#define password 		"YOUR_WIFI_PASSWORD"
#define botToken 		"YOUR_BOT_TOKEN"
#define BotUsername		"YOUR_BOT_USERNAME"
#define SourceID		"SOURCE_CHANNEL_TO_PULL_FILES_FROM" 
```
If you dont plan on using the !pic command, remove it from code and delete the last line.

# Step 4: Compilation & Upload
1. Open `DiscordBot.ino` in your development environment.
2. Select your specific **ESP32 Dev Module** from your IDE board manager.
3. Choose the correct COM / Serial port for your connected board.
4. Click **Verify/Compile**, then hit **Upload**.

You can also check the Serial Monitor by changing the value of the baud rate. The default for this project is `115200`.

## Important Warning
**Do not share your `config.h` file.**
if you plan on modifying and pushing it to public, ensure `config.h` is added to `.gitignore`.



