// -----------------------
// Libraries
// -----------------------
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include "LittleFS.h"
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// -----------------------
// Hardware Pins
// -----------------------
const int buzzerPin = 12;
const int yTL = 10; // Yellow LED
const int rTL = 9;  // Red LED
const int gTL = 11; // Green LED

#define I2C_SDA 6 // blue wire
#define I2C_SCL 7 // green wire

const int wireStartPin = 18; // Digital start wire (Trigger at ~30%)
const int sensorStopPin = 4; // Analog stop sensor (Trigger at ~70%)

// -----------------------
// System Constants
// -----------------------
const float volume_gap_cm3 = 87.2;
const float collector_area_cm2 = 400.0; // 20x20 cm funnel
const int sensorStopThreshold = 1800;   // Analog value to stop timer
#define ID_FILE "/users.txt"            // LittleFS File for Telegram IDs

// Configurable Thresholds (Seconds)
const float TIME_HIGH_MAX = 10.0; // Under 10s -> HIGH ALERT
const float TIME_MID_MAX = 35.0;  // 10s to 35s -> MID CAUTION
                                  // Over 35s -> LOW TIMEOUT

// -----------------------
// Shared / Volatile Variables
// -----------------------
// Note: 'volatile' tells the compiler these variables can change across
// different threads/cores at any time, preventing optimization errors.
enum SystemMode
{
  NORMAL,
  MID,
  HIGH_ALERT,
  LOW_TIMEOUT
};

volatile SystemMode currentMode = NORMAL;
volatile int currentSpeed = 80;
volatile int MIDSpeed = 40;
volatile int HIGHSpeed = 0;


// Sensor & Timing State
volatile bool startTriggered = false;
volatile bool timingActive = false;
volatile bool measurementFinished = false;
volatile int currentStopLevel = 0;
volatile float lastMeasurementTime = 0.0;
volatile unsigned long startTime = 0;
volatile unsigned long stopTime = 0;

// Cross-Core Communication Flags
volatile bool requestHighAlertBroadcast = false;

// -----------------------
// Non-Blocking Timers & States
// -----------------------
unsigned long lastBlink = 0;
bool yTLState = false;

unsigned long lastLcdUpdate = 0;
bool showSpeedOnLCD = true;

// Non-blocking Buzzer variables
const int buzzerTones[] = {600, 800, 600, 800, 800, 600};
int currentToneIndex = -1;
unsigned long lastToneTime = 0;

// -----------------------
// Telegram Credentials & Objects
// -----------------------
const char *ssid = "qrr";
const char *password = "abcabcabc";
#define BOTtoken "8327968609:AAHG9WC2IbOO8lStQZgFSZxr2e5h-GkKqtc"
const String adminChatID = "1879407547";

WiFiClientSecure client;
UniversalTelegramBot bot(BOTtoken, client);
LiquidCrystal_I2C lcd(0x27, 16, 2);
TaskHandle_t TelegramTask;

// -----------------------
// Helper Functions
// -----------------------

// Safely converts the current enum state into a string format
// (Prevents String memory corruption across dual cores)
String getSystemStateString(SystemMode mode)
{
  switch (mode)
  {
  case NORMAL:
    return "Good Conditions";
  case MID:
    return "Mid Caution";
  case HIGH_ALERT:
    return "High Alert";
  case LOW_TIMEOUT:
    return "Needs Restart";
  default:
    return "Unknown";
  }
}

// -----------------------
// Core 0: File System & Network
// -----------------------

bool registerUser(String chat_id)
{
  bool exists = false;
  if (LittleFS.exists(ID_FILE))
  {
    File file = LittleFS.open(ID_FILE, FILE_READ);
    while (file.available())
    {
      String line = file.readStringUntil('\n');
      line.trim();
      if (line == chat_id)
      {
        exists = true;
        break;
      }
    }
    file.close();
  }

  if (!exists)
  {
    File appendFile = LittleFS.open(ID_FILE, FILE_APPEND);
    if (appendFile)
    {
      appendFile.println(chat_id);
      appendFile.close();
      Serial.println("[FILE] Registered new user: " + chat_id);
      return true;
    }
  }
  return false;
}

void broadcast(String message)
{
  Serial.println("[TELEGRAM] Broadcasting: " + message);
  if (!LittleFS.exists(ID_FILE))
    return;

  File file = LittleFS.open(ID_FILE, FILE_READ);
  while (file.available())
  {
    String chat_id = file.readStringUntil('\n');
    chat_id.trim();
    if (chat_id.length() > 0)
    {
      bot.sendMessage(chat_id, message, "");
      delay(50); // Small delay to prevent API rate-limiting
    }
  }
  file.close();
}

void handleNewMessages(int numNewMessages)
{
  for (int i = 0; i < numNewMessages; i++)
  {
    String chat_id = String(bot.messages[i].chat_id);
    String text = bot.messages[i].text;

    if (registerUser(chat_id))
    {
      bot.sendMessage(chat_id, "System Alert Registration Complete.", "");
    }

    if (text == "/state")
    {
      bot.sendMessage(chat_id, "System State: " + getSystemStateString(currentMode), "");
    }
    else if (text == "/status")
    {
      bot.sendMessage(chat_id, "System is online. IP: " + WiFi.localIP().toString(), "");
    }
    else if (text == "/getdata")
    {
      if (chat_id == adminChatID)
      {
        // Read volatiles into local variables quickly to prevent data tearing
        SystemMode snapMode = currentMode;
        int snapSpeed = currentSpeed;
        int snapStopLevel = currentStopLevel;
        float snapTime = lastMeasurementTime;

        String report = "📊 *Diagnostics Report* 📊\n\n";
        report += "🛠 *Current State*\n";
        report += "- Status: " + getSystemStateString(snapMode) + "\n";
        report += "- Speed Limit: " + String(snapSpeed) + " km/h\n\n";
        report += "💧 *Measurement Log*\n";
        report += "- Stop Level: " + String(snapStopLevel) + "\n";
        report += "- Fill Time: " + (snapTime > 0.0 ? String(snapTime) + " sec" : "N/A") + "\n\n";
        report += "🌐 *Hardware*\n";
        report += "- RSSI: " + String(WiFi.RSSI()) + " dBm\n";
        report += "- Uptime: " + String(millis() / 1000) + " sec\n";

        bot.sendMessage(chat_id, report, "Markdown");
      }
      else
      {
        bot.sendMessage(chat_id, "⛔ Unauthorized.", "");
      }
    }
  }
}

// Dedicated FreeRTOS Task for Core 0
void telegramLogic(void *pvParameters)
{
  Serial.println("[SYS] Network Task started on Core 0");
  for (;;)
  {
    // 1. Check if Core 1 requested an emergency broadcast
    if (requestHighAlertBroadcast)
    {
      broadcast("🚨 ALERT: Measurement rose too fast! High state triggered.");
      requestHighAlertBroadcast = false; // Reset flag after sending
    }

    // 2. Poll for incoming messages
    if (WiFi.status() == WL_CONNECTED)
    {
      int numNewMessages = bot.getUpdates(bot.last_message_received + 1);
      while (numNewMessages)
      {
        handleNewMessages(numNewMessages);
        numNewMessages = bot.getUpdates(bot.last_message_received + 1);
      }
    }

    // Yield to Watchdog (1 second tick)
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

// -----------------------
// Core 1: Hardware Outputs & Visuals
// -----------------------

void updateLCD()
{
  lcd.clear();
  SystemMode modeSnapshot = currentMode; // Local copy for safety

  if (modeSnapshot == LOW_TIMEOUT)
  {
    lcd.setCursor(0, 0);
    lcd.print("Sys: Restart Req");
    lcd.setCursor(0, 1);
    lcd.print("Please Empty Cup");
  }
  else
  {
    if (showSpeedOnLCD)
    {
      lcd.setCursor(0, 0);
      lcd.print("Speed Limit:");
      lcd.setCursor(0, 1);
      lcd.print(currentSpeed);
      lcd.print(" km/h");
    }
    else
    {
      lcd.setCursor(0, 0);
      lcd.print("Weather/State:");
      lcd.setCursor(0, 1);
      lcd.print(getSystemStateString(modeSnapshot));
    }
  }
}

// Non-blocking Buzzer trigger and execution
void triggerBuzzer()
{
  currentToneIndex = 0;
  tone(buzzerPin, buzzerTones[0], 150);
  lastToneTime = millis();
}

void handleBuzzerAsync()
{
  // If sequence is active, step through it without using delay()
  if (currentToneIndex >= 0)
  {
    if (millis() - lastToneTime > 350)
    { // 150ms play + 200ms gap
      currentToneIndex++;
      
      // If we reach the end of the tone array
      if (currentToneIndex >= 6)
      {
        // Loop the buzzer if we are still in HIGH_ALERT
        if (currentMode == HIGH_ALERT) {
          currentToneIndex = 0;
          tone(buzzerPin, buzzerTones[0], 150);
          lastToneTime = millis();
        } else {
          // Otherwise, stop it
          noTone(buzzerPin);
          currentToneIndex = -1; // Sequence finished
        }
      }
      else
      {
        tone(buzzerPin, buzzerTones[currentToneIndex], 150);
        lastToneTime = millis();
      }
    }
  }
}

// -----------------------
// Core 1: State Triggers
// -----------------------

void triggerNormalState()
{
  currentMode = NORMAL;
  currentSpeed = 80;
  
  digitalWrite(gTL, HIGH);
  digitalWrite(yTL, LOW);
  digitalWrite(rTL, LOW);
  
  // Force stop the buzzer and reset the sequence tracker
  noTone(buzzerPin);
  currentToneIndex = -1; 
  
  updateLCD();
}

void triggerHighState()
{
  currentMode = HIGH_ALERT;
  currentSpeed = HIGHSpeed;

  digitalWrite(gTL, LOW);
  digitalWrite(yTL, LOW);
  digitalWrite(rTL, HIGH);

  updateLCD();
  triggerBuzzer();                  // Starts non-blocking buzzer
  requestHighAlertBroadcast = true; // Signals Core 0 to send message safely
}

void triggerMidState()
{
  currentMode = MID;
  currentSpeed = MIDSpeed;
  digitalWrite(gTL, LOW);
  digitalWrite(rTL, LOW);
  // Yellow LED blinking handled in main loop
  updateLCD();
}

void triggerLowState()
{
  timingActive = false;
  measurementFinished = true;
  currentMode = LOW_TIMEOUT;
  digitalWrite(gTL, LOW);
  digitalWrite(yTL, LOW);
  digitalWrite(rTL, LOW);
  updateLCD();
}

// -----------------------
// Core 1: Sensor Logic
// -----------------------

void readSensors()
{
  // Simple 10ms debounce logic
  if (digitalRead(wireStartPin) == LOW)
  {
    delay(10);
    if (digitalRead(wireStartPin) == LOW)
    {
      startTriggered = true;
    }
  }
  else
  {
    startTriggered = false;
  }

  currentStopLevel = analogRead(sensorStopPin);
}

void handleTimingStart()
{
  if (startTriggered && !timingActive && !measurementFinished && currentMode == NORMAL)
  {
    startTime = millis();
    timingActive = true;
  }
}

void checkLowTimeout()
{
  if (timingActive && (millis() - startTime) > (TIME_MID_MAX * 1000.0))
  {
    triggerLowState();
  }
}

void handleStopLogic()
{
  if (currentStopLevel > sensorStopThreshold && timingActive)
  {
    stopTime = millis();
    timingActive = false;
    measurementFinished = true;

    float delta_t = (stopTime - startTime) / 1000.0;
    lastMeasurementTime = delta_t;

    if (delta_t <= TIME_HIGH_MAX)
    {
      triggerHighState();
    }
    else if (delta_t <= TIME_MID_MAX)
    {
      triggerMidState();
    }
  }
}

void handleSystemReset()
{
  // Wait until start wire is untriggered and cup is drained below 500
  if (digitalRead(wireStartPin) == HIGH && currentStopLevel < 500 && measurementFinished)
  {
    measurementFinished = false;
    triggerNormalState();
  }
}

// -----------------------
// Main Setup
// -----------------------

void setup()
{
  Serial.begin(115200);

  Serial.println("[INIT] Mounting LittleFS...");
  LittleFS.begin(true);

  Serial.println("[INIT] Configuring GPIO Pins...");
  pinMode(buzzerPin, OUTPUT);
  pinMode(rTL, OUTPUT);
  pinMode(gTL, OUTPUT);
  pinMode(yTL, OUTPUT);
  pinMode(wireStartPin, INPUT_PULLUP);
  pinMode(sensorStopPin, INPUT);

  Serial.println("[INIT] Initializing I2C & LCD...");
  Wire.begin(I2C_SDA, I2C_SCL);
  lcd.init();
  lcd.backlight();

  Serial.println("[WIFI] Connecting...");
  WiFi.begin(ssid, password);
  client.setInsecure(); // Allows bypass of SSL validation for Telegram

  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n[WIFI] Connected! IP: " + WiFi.localIP().toString());

  triggerNormalState();

  // Initialize Core 0 Network Task
  xTaskCreatePinnedToCore(
      telegramLogic,  // Task function
      "TelegramTask", // Task name
      8192,           // Stack size
      NULL,           // Parameters
      1,              // Priority
      &TelegramTask,  // Task handle
      0               // Pin to Core 0
  );

  Serial.println("[INIT] System Ready. Starting Sensor Loop on Core 1.");
}

// -----------------------
// Main Loop (Core 1)
// -----------------------

void loop()
{

  // 1. Asynchronous Buzzer Execution
  handleBuzzerAsync();

  // 2. Yellow LED Blinking Routine for Mid State
  if (currentMode == MID)
  {
    if (millis() - lastBlink > 150)
    {
      yTLState = !yTLState;
      digitalWrite(yTL, yTLState ? HIGH : LOW);
      lastBlink = millis();
    }
  }

  // 3. LCD Alternating Display (1-second intervals)
  if (millis() - lastLcdUpdate > 1000)
  {
    showSpeedOnLCD = !showSpeedOnLCD;
    updateLCD();
    lastLcdUpdate = millis();
  }

  // 4. Read Sensors & Run Logic Checks
  readSensors();
  handleTimingStart();
  checkLowTimeout();
  handleStopLogic();
  handleSystemReset();

  // Very short delay to maintain extreme sensor precision
  delay(10);
}