#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "RTClib.h"
#include <Servo.h>
#include <OneWire.h>
#include <DallasTemperature.h>

#define BUZZER_PIN    8
#define RED_LED_PIN   4      // danger  (use a 220 ohm resistor)
#define GREEN_LED_PIN 5      // safe    (use a 220 ohm resistor)
#define PH_SENSOR_PIN A1

// ---------- Safe Range Settings  ----------
const float TEMP_MIN = 24.0;   // deg C
const float TEMP_MAX = 28.0;   // deg C
const float PH_MIN   = 5.5;
const float PH_MAX   = 7.5;

// ---------- pH Calibration Constants (two-point) ----------
// Replace these PLACEHOLDERS with the voltages you measure in the buffers.
const float PH_V7 = 2.50;   // voltage measured in pH 7.00 buffer
const float PH_V4 = 3.05;   // voltage measured in pH 4.00 buffer
const float PH_SLOPE = (7.0 - 4.0) / (PH_V7 - PH_V4);  // pH units per volt

// ---------- Buzzer pattern: beep-beep ... pause ----------
const unsigned long BEEP_ON    = 200;    // ms each beep
const unsigned long BEEP_GAP   = 150;    // ms between the two beeps
const unsigned long BEEP_PAUSE = 1000;   // ms pause after the pair
const unsigned long BEEP_CYCLE = BEEP_ON + BEEP_GAP + BEEP_ON + BEEP_PAUSE;

RTC_DS1307 rtc;                      // use RTC_DS3231 rtc; for a DS3231
LiquidCrystal_I2C lcd(0x27, 16, 2);  // try 0x3F if the screen stays blank
Servo myServo;

OneWire oneWire(2);                  // DS18B20 data pin = 2
DallasTemperature sensors(&oneWire);

// ----- Feeding settings (2 times a day, 24-hour format) -----
const int SERVO_PIN = 9;
const int FEED_COUNT = 2;
const int FEED_HOUR[FEED_COUNT]   = {8, 15};   // 08:00 and 18:00  <-- change to your times
const int FEED_MINUTE[FEED_COUNT] = {0, 32};
const unsigned long ROTATE_TIME = 2000;  // 2 seconds
const int ANGLE_ON  = 180;
const int ANGLE_OFF = 0;
// ------------------------------------------------------------

bool servoActive = false;
bool alreadyFed[FEED_COUNT] = {false, false};
unsigned long servoStart = 0;
unsigned long lastUpdate = 0;
float tempC = DEVICE_DISCONNECTED_C;
float phValue = 0;
float phVoltage = 0;
bool alarmActive = false;

// Print text on an LCD row, padded to 16 characters (clears old text)
void lcdPrintLine(uint8_t row, const char *text) {
  char buf[17];
  snprintf(buf, sizeof(buf), "%-16s", text);
  lcd.setCursor(0, row);
  lcd.print(buf);
}

// Read pH sensor (average of 10 samples)
float readPH() {
  long sum = 0;
  for (int i = 0; i < 10; i++) {
    sum += analogRead(PH_SENSOR_PIN);
    delay(5);
  }
  float rawValue = sum / 10.0;
  phVoltage = rawValue * (5.0 / 1023.0);

  // Two-point calibration: pH 7 at PH_V7, slope from the pH 4 point
  return 7.0 + PH_SLOPE * (phVoltage - PH_V7);
}

// Non-blocking buzzer: beep-beep, then a pause, repeating while alarmActive
void updateBuzzer() {
  if (!alarmActive) {
    digitalWrite(BUZZER_PIN, LOW);
    return;
  }
  unsigned long t = millis() % BEEP_CYCLE;
  bool firstBeep  = (t < BEEP_ON);
  bool secondBeep = (t >= BEEP_ON + BEEP_GAP && t < BEEP_ON + BEEP_GAP + BEEP_ON);
  digitalWrite(BUZZER_PIN, (firstBeep || secondBeep) ? HIGH : LOW);
}

void setup() {
  Serial.begin(9600);

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(RED_LED_PIN, OUTPUT);
  pinMode(GREEN_LED_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(RED_LED_PIN, LOW);
  digitalWrite(GREEN_LED_PIN, LOW);

  lcd.init();
  lcd.backlight();
  lcdPrintLine(0, "Aquarium Monitor");
  lcdPrintLine(1, "Initializing...");
  delay(2000);
  lcd.clear();

  myServo.attach(SERVO_PIN);
  myServo.write(ANGLE_OFF);

  sensors.begin();
  sensors.setWaitForConversion(false);  // non-blocking, keeps servo timing accurate
  sensors.requestTemperatures();

  if (!rtc.begin()) {
    lcdPrintLine(0, "RTC not found");
    Serial.println("RTC not found");
    while (true);
  }

  if (!rtc.isrunning()) {            // DS3231: use if (rtc.lostPower())
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }
}

void loop() {
  DateTime now = rtc.now();

  // ----- Feeding trigger check (two times a day) -----
  for (int i = 0; i < FEED_COUNT; i++) {
    bool inFeedMinute = (now.hour() == FEED_HOUR[i] && now.minute() == FEED_MINUTE[i]);

    if (inFeedMinute && !alreadyFed[i]) {
      alreadyFed[i] = true;
      servoActive = true;
      servoStart = millis();
      myServo.write(ANGLE_ON);
    }
    if (!inFeedMinute) {
      alreadyFed[i] = false;         // ready for the next feeding
    }
  }

  // ----- Stop servo after 2 seconds -----
  if (servoActive && millis() - servoStart >= ROTATE_TIME) {
    myServo.write(ANGLE_OFF);
    servoActive = false;
  }

  // ----- Buzzer pattern runs every loop so the timing stays smooth -----
  updateBuzzer();

  // ----- Update everything once per second -----
  if (millis() - lastUpdate >= 1000) {
    lastUpdate = millis();

    // Read temperature (last conversion), then start the next one
    tempC = sensors.getTempCByIndex(0);
    sensors.requestTemperatures();

    // Read pH
    phValue = readPH();

    // ---- Serial Monitor: pH voltage (use this for calibration) ----
    Serial.print("pH voltage: ");
    Serial.print(phVoltage, 3);
    Serial.print(" V   pH: ");
    Serial.println(phValue, 2);

    // ---- Serial Monitor: RTC date and time ----
    char line[48];
    snprintf(line, sizeof(line), "Date: %02d/%02d/%04d  Time: %02d:%02d:%02d",
             now.day(), now.month(), now.year(),
             now.hour(), now.minute(), now.second());
    Serial.println(line);

    // ---- Temperature status: 0 = normal, 1 = high, -1 = low ----
    bool tempSensorError = (tempC == DEVICE_DISCONNECTED_C);
    int tempStatus = 0;
    if (!tempSensorError) {
      if (tempC > TEMP_MAX) tempStatus = 1;
      else if (tempC < TEMP_MIN) tempStatus = -1;
    }

    // ---- pH status ----
    // Voltage almost 0 V or almost 5 V means the sensor is unplugged or faulty
    bool phSensorError = (phVoltage < 0.1 || phVoltage > 4.9);
    int phStatus = 0;
    if (!phSensorError) {
      if (phValue > PH_MAX) phStatus = 1;
      else if (phValue < PH_MIN) phStatus = -1;
    }

    bool tempDanger = (tempStatus != 0);
    bool phDanger   = (phStatus != 0);

    // ---- LCD ----
    // Row 0: temperature + pH, with -H or -L after the value that is out of range
    // Row 1: Status Normal / DANGER
    char tbuf[8];
    char tPart[12];
    char pPart[12];
    char row[20];

    if (tempSensorError) {
      strcpy(tPart, "T--");
    } else {
      dtostrf(tempC, 1, 1, tbuf);
      snprintf(tPart, sizeof(tPart), "T:%s%s", tbuf,
               (tempStatus == 1) ? "-H" : (tempStatus == -1) ? "-L" : "");
    }

    if (phSensorError) {
      strcpy(pPart, "pH--");
    } else {
      dtostrf(phValue, 1, 1, tbuf);
      snprintf(pPart, sizeof(pPart), "P:%s%s", tbuf,
               (phStatus == 1) ? "-H" : (phStatus == -1) ? "-L" : "");
    }

    // Add a space between the two readings only if it fits in 16 columns
    if (strlen(tPart) + strlen(pPart) < 16) {
      snprintf(row, sizeof(row), "%s %s", tPart, pPart);
    } else {
      snprintf(row, sizeof(row), "%s%s", tPart, pPart);
    }
    lcdPrintLine(0, row);

    bool anyDanger = tempDanger || tempSensorError || phDanger || phSensorError;
    lcdPrintLine(1, anyDanger ? "Status: DANGER" : "Status: Normal");

    // ---- LEDs: red = any sensor in danger, green = everything safe ----
    digitalWrite(RED_LED_PIN,   anyDanger ? HIGH : LOW);
    digitalWrite(GREEN_LED_PIN, anyDanger ? LOW : HIGH);

    // ---- Buzzer: pattern is handled by updateBuzzer() ----
    alarmActive = anyDanger;
  }
}

