/***************************************************************************
These are the brains of Dialrhea - revolutionary shitty machine, written in
few hours during Internet Of Shit Hackathon 2017 at Technariumas, Vilnius,
Lithuania, so expect the code to be pretty shitty.

Author: Giedrius Tamulaitis, giedrius@tamulaitis.lt
Version: 1.0

This code is for Adafruit nRF51822 based Bluefruit LE modules
https://learn.adafruit.com/introducing-the-adafruit-bluefruit-le-uart-friend
***************************************************************************/

#include <Arduino.h>
#include <SPI.h>
#if not defined (_VARIANT_ARDUINO_DUE_X_) && not defined(ARDUINO_ARCH_SAMD)
  #include <SoftwareSerial.h>
#endif
#include "Adafruit_BLE.h"
#include "Adafruit_BluefruitLE_UART.h"
#include "BluefruitConfig.h"

#define DEVICE_NAME "Dialrhea"

// Rotary dial input PIN
#define ROTARY_PIN 2
// Handset input PIN
#define HANDSET_PIN 3
// Operation mode potentiometer PIN
#define OPERATION_MODE_PIN A5

// How long to wait before sending keyup message for control keys in Gaming mode
#define CONTROL_KEY_HOLD_DURATION 200
// How long to wait before sending keyup message for fire button in Gaming mode
#define FIRE_KEY_HOLD_DURATION 200
// How long to wait before sending keyup message for keys that are supposed
// to be just one clicks
#define INSTANT_KEY_HOLD_DURATION 10

// Pins for status LED RGB legs
#define STATUS_LED_RED_PIN 4
#define STATUS_LED_GREEN_PIN 6
#define STATUS_LED_BLUE_PIN 5

// Constants for colors
#define COLOR_OFF 0
#define COLOR_RED 1
#define COLOR_GREEN 2
#define COLOR_BLUE 3

// Total number of keys that support timed presses
#define KEY_COUNT 14

// Index of handset button data in data arrays (Gaming mode, we need two because
//we are sending keys for both fire and open door)
#define KEY_GAMING_MODE_HANDSET_1_INDEX 10
#define KEY_GAMING_MODE_HANDSET_2_INDEX 11
// Index of handset button data in data arrays (Emoji mode)
#define KEY_EMOJI_MODE_HANDSET_INDEX 12
// Index of handset button data in data arrays (Boring mode)
#define KEY_BORING_MODE_HANDSET_INDEX 13

// Map of values for each type of dialed number and handset click
const int keyValues[KEY_COUNT] = {
  0x42, // Number 0 in gaming mode (Currently quick load)
  0x52, // Number 1 in gaming mode (Currently "up" arrow)
  0x4F, // Number 2 in gaming mode (Currently "right" arrow)
  0x50, // Number 3 in gaming mode (Currently "left" arrow)
  0x51, // Number 4 in gaming mode (Currently "down" arrow)
  0x2A, // Number 5 in gaming mode (Currently ?, next weapon)
  0x00, // Number 6 in gaming mode
  0x00, // Number 7 in gaming mode
  0x00, // Number 8 in gaming mode
  0x00, // Number 9 in gaming mode
  0x10, // Handset click in gaming mode (KEY_GAMING_MODE_HANDSET_1_INDEX) (Currently space)
  0x2C, // Handset click in gaming mode (KEY_GAMING_MODE_HANDSET_2_INDEX) (Currently 'm')
  0x28, // Handset click in emoji mode (KEY_EMOJI_MODE_HANDSET_INDEX) (Currently Enter)
  0x29  // Handset click in boring mode (KEY_BORING_MODE_HANDSET_INDEX) (Currently Esc)
};

// Durations for each type of mey (mapping the same as for keyValues array)
const int keyHoldDurations[KEY_COUNT] = {
  INSTANT_KEY_HOLD_DURATION, 
  CONTROL_KEY_HOLD_DURATION, 
  CONTROL_KEY_HOLD_DURATION,
  CONTROL_KEY_HOLD_DURATION,
  CONTROL_KEY_HOLD_DURATION,
  INSTANT_KEY_HOLD_DURATION,
  INSTANT_KEY_HOLD_DURATION,
  INSTANT_KEY_HOLD_DURATION,
  INSTANT_KEY_HOLD_DURATION,
  INSTANT_KEY_HOLD_DURATION,
  FIRE_KEY_HOLD_DURATION,
  FIRE_KEY_HOLD_DURATION,
  INSTANT_KEY_HOLD_DURATION,
  INSTANT_KEY_HOLD_DURATION
};

// Array for storing times each key was pressed
unsigned long keyPressTimes[KEY_COUNT];
// Array for storing states for each key
bool keyPressStates[KEY_COUNT];

// Variables required for handling input from rotary dial
int rotaryHasFinishedRotatingTimeout = 100;
int rotaryDebounceTimeout = 10;
int rotaryLastValue = LOW;
int rotaryTrueValue = LOW;
unsigned long rotaryLastValueChangeTime = 0;
bool rotaryNeedToEmitEvent = 0;
int rotaryPulseCount;

// Operation modes
#define OPERATION_MODE_GAMING 0 // Gaming controls fine tuned for the best game of all times: "Doom"
#define OPERATION_MODE_EMOJI 1 // Emojis + Enter
#define OPERATION_MODE_BORING 2 // Numbers + Esc

// Current operation mode
int operationMode;

// Emojis for each dialed number
const char* emojis[] = {":-O", ":poop:",  ":-)", ":-(", ":-D", ":-\\", ";-)", ":-*", ":-P", ">:-("};

// Variables for handling handset clicker button
bool isHandsetPressed = false;
unsigned long handsetPressStartTime = 0;
unsigned long handsetPressStartTimeout = 60;

// Variable that determines weather the state of keys changed during processing of the loop (so we
// can send commands just once in the end of the loop if it is needed)
bool keyPressStateChanged;

// Config settings for Bluetooth LE module
#define FACTORYRESET_ENABLE         0
#define VERBOSE_MODE                false  // If set to 'true' enables debug output
#define MINIMUM_FIRMWARE_VERSION    "0.6.6"
#define BLUEFRUIT_HWSERIAL_NAME      Serial1

// Bluetooth LE module object
Adafruit_BluefruitLE_UART ble(BLUEFRUIT_HWSERIAL_NAME, BLUEFRUIT_UART_MODE_PIN);

void setup(void) {
  pinMode(ROTARY_PIN, INPUT);
  pinMode(HANDSET_PIN, INPUT_PULLUP);
  pinMode(STATUS_LED_RED_PIN, OUTPUT);
  pinMode(STATUS_LED_GREEN_PIN, OUTPUT);
  pinMode(STATUS_LED_BLUE_PIN, OUTPUT);

  setStatusLEDColor(COLOR_GREEN);

  // Wait while serial connection is established (required for Flora & Micro or when you want to
  // halt initialization till you open serial monitor)
  // while (!Serial);
  
  // Give some time for chip to warm up or whatever
  delay(1000);

  initializeSerialConnection();
  initializeBLEModule();

  // Delay a bit because good devices always take some time to start
  delay(100);

  setStatusLEDColor(COLOR_BLUE);
}

void loop(void) {
  keyPressStateChanged = false;
  refreshOperationMode();
  handleHandset();
  handleRotary();
  processKeyUps();

  // If state of pressed keys changed - send the new state
  if (keyPressStateChanged)
    sendCurrentlyPressedKeys();
}

// Sets the color of status LED
void setStatusLEDColor(int colorID) {
  digitalWrite(STATUS_LED_RED_PIN, colorID == COLOR_RED ? HIGH : LOW);
  digitalWrite(STATUS_LED_GREEN_PIN, colorID == COLOR_GREEN ? HIGH : LOW);
  digitalWrite(STATUS_LED_BLUE_PIN, colorID == COLOR_BLUE ? HIGH : LOW);
}

// Outputs error message and bricks the revolutionary shitty machine
void error(const __FlashStringHelper*err) {

  setStatusLEDColor(COLOR_RED);
  
  Serial.println(err);
  while (1);
}

// Blinks the status LED (only green supported for now)
void blink() {
  setStatusLEDColor(COLOR_OFF);
  delay(100);
  setStatusLEDColor(COLOR_GREEN);
}

// Opens serial connection for debugging
void initializeSerialConnection() {
  Serial.begin(9600);
  Serial.println(F("Hello, I am the Dialrhea! Ready for some dialing action?"));
  Serial.println(F("8-------------------------------------D"));
}

// Initializes Bluetooth LE module
void initializeBLEModule() {
  // Buffer for holding commands that have to be sent to BLE module
  char commandString[64];

  setStatusLEDColor(COLOR_GREEN);

  Serial.print(F("Initialising the Bluefruit LE module: "));
  if (!ble.begin(VERBOSE_MODE)) error(F("Couldn't find Bluefruit, make sure it's in CoMmanD mode & check wiring?"));
  Serial.println( F("Easy!") );

  blink();

  if (FACTORYRESET_ENABLE)
  {
    Serial.println(F("Performing a factory reset: "));
    if (!ble.factoryReset()) error(F("Couldn't factory reset. Have no idea why..."));
    Serial.println(F("Done, feeling like a virgin again!"));
  }

  blink();

  // Disable command echo from Bluefruit
  ble.echo(false);

  blink();

  Serial.println("Requesting Bluefruit info:");
  ble.info();

  blink();

  // Change the device name so the whole world knows it as Dialrhea
  Serial.print(F("Setting device name to '"));
  Serial.print(DEVICE_NAME);
  Serial.print(F("': "));
  sprintf(commandString, "AT+GAPDEVNAME=%s", DEVICE_NAME);
  if (!ble.sendCommandCheckOK(commandString)) error(F("Could not set device name for some reason. Sad."));
  Serial.println(F("It's beautiful!"));

  blink();

  Serial.print(F("Enable HID Service (including Keyboard): "));
  strcpy(commandString, ble.isVersionAtLeast(MINIMUM_FIRMWARE_VERSION) ? "AT+BleHIDEn=On" : "AT+BleKeyboardEn=On");
  if (!ble.sendCommandCheckOK(commandString)) error(F("Could not enable Keyboard, we're in deep shit..."));
  Serial.println(F("I'm now officially a keyboard!"));

  blink();

  // Make software reset (add or remove service requires a reset)
  Serial.print(F("Performing a SW reset (service changes require a reset): "));
  if (!ble.reset()) error(F("Couldn't reset?? Lame."));
  Serial.println(F("Baby I'm ready to go!"));
  Serial.println();
}

// Reads the position of operation mode select potentiometer and determines current operation mode
void refreshOperationMode() {
  operationMode = floor((float)analogRead(OPERATION_MODE_PIN) / 342.0);
}

// Handles tracking the handset state
void handleHandset() {
  // Ignore input until last action timeout passes (to filter out noise)
  if (millis() - handsetPressStartTime > handsetPressStartTimeout) {
    int ragelisCurrentValue = digitalRead(HANDSET_PIN);
    
    if (!isHandsetPressed && ragelisCurrentValue == HIGH) {
      isHandsetPressed = true;
      handsetPressStartTime = millis();
      onHandsetClicked();
    }

    else if (isHandsetPressed && ragelisCurrentValue == LOW) {
      isHandsetPressed = false;
      handsetPressStartTime = millis(); 
    }
  }
}

// Handles tracking of the rotary dial state
void handleRotary() {
  int rotaryCurrentValue = digitalRead(ROTARY_PIN);

  // If rotary isn't being dialed or it just finished being dialed
  if ((millis() - rotaryLastValueChangeTime) > rotaryHasFinishedRotatingTimeout) {
    // If rotary just finished being dialed - we need to emit the event
    if (rotaryNeedToEmitEvent) {
      // Emit the event (we mod the count by 10 because '0' will send 10 pulses)
      onRotaryNumberDialed(rotaryPulseCount % 10);
      rotaryNeedToEmitEvent = false;
      rotaryPulseCount = 0;
    }
  }

  // If rotary value has changed - register the time when it happened
  if (rotaryCurrentValue != rotaryLastValue) {
    rotaryLastValueChangeTime = millis();
  }

  // Start analyzing data only when signal stabilizes (debounce timeout passes)
  if ((millis() - rotaryLastValueChangeTime) > rotaryDebounceTimeout) {
    // This means that the switch has either just gone from closed to open or vice versa.
    if (rotaryCurrentValue != rotaryTrueValue) {
      // Register actual value change 
      rotaryTrueValue = rotaryCurrentValue;

      // If it went to HIGH - increase pulse count
      if (rotaryTrueValue == HIGH) {
        rotaryPulseCount++; 
        rotaryNeedToEmitEvent = true;
      } 
    }
  }

  // Store current value as last value
  rotaryLastValue = rotaryCurrentValue;
}

// Event handler triggered when click of the handset button is registered
void onHandsetClicked() {
  // Register state changes for handset button keys depending on the mode
  if (operationMode == OPERATION_MODE_GAMING) {
    if (keyPressStates[KEY_GAMING_MODE_HANDSET_1_INDEX] == false || keyPressStates[KEY_GAMING_MODE_HANDSET_2_INDEX] == false)
      keyPressStateChanged = true;
  
    keyPressStates[KEY_GAMING_MODE_HANDSET_1_INDEX] = true;
    keyPressTimes[KEY_GAMING_MODE_HANDSET_1_INDEX] = millis();
  
    keyPressStates[KEY_GAMING_MODE_HANDSET_2_INDEX] = true;
    keyPressTimes[KEY_GAMING_MODE_HANDSET_2_INDEX] = millis();
  } else if (operationMode == OPERATION_MODE_EMOJI) {
    if (keyPressStates[KEY_EMOJI_MODE_HANDSET_INDEX] == false)
      keyPressStateChanged = true;
  
    keyPressStates[KEY_EMOJI_MODE_HANDSET_INDEX] = true;
    keyPressTimes[KEY_EMOJI_MODE_HANDSET_INDEX] = millis();
  } else if (operationMode == OPERATION_MODE_BORING) {
    if (keyPressStates[KEY_BORING_MODE_HANDSET_INDEX] == false)
      keyPressStateChanged = true;
  
    keyPressStates[KEY_BORING_MODE_HANDSET_INDEX] = true;
    keyPressTimes[KEY_BORING_MODE_HANDSET_INDEX] = millis();
  }
}

// Event handler triggered when number was dialed on rotary dial
void onRotaryNumberDialed(int number) {
  if (operationMode == OPERATION_MODE_GAMING) {
    // Set key state for dialed key
    if (keyPressStates[number] == false)
      keyPressStateChanged = true;
  
    keyPressStates[number] = true;
    keyPressTimes[number] = millis();
  } else if (operationMode == OPERATION_MODE_EMOJI) {
    // Send emoji to happy device
    sendCharArray(emojis[number]);
  } else if (operationMode == OPERATION_MODE_BORING) {
    // Form string from number and send it to device
    char numberString[1];
    sprintf(numberString, "%d", number);
    sendCharArray(numberString);
  }
}

// Sends raw command to BLE module and prints debug output
void sendBluetoothCommand(char *commandString) {
  setStatusLEDColor(COLOR_OFF);

  Serial.print(commandString);

  ble.println(commandString);
  if (ble.waitForOK()) {
    Serial.println(F(" <- OK!")); 
    setStatusLEDColor(COLOR_BLUE);
  }
  else {
    Serial.println(F(" <- FAILED!"));
    setStatusLEDColor(COLOR_RED);
  };
}

// Sends char array (string) to BLE module
void sendCharArray(char* charArray) {
  char commandString[64];
  sprintf(commandString, "AT+BleKeyboard=%s", charArray);
  sendBluetoothCommand(commandString);
}

// Checks which keys are currently pressed and sends keycodes to BLE module
void sendCurrentlyPressedKeys() {
  char commandString[64] = "AT+BleKeyboardCode=00-00";

  for (int i=0; i<KEY_COUNT; i++) {
    if (keyPressStates[i] == true && keyValues[i] != 0x00) {
      sprintf (commandString, "%s-%02X", commandString, keyValues[i]);
    }
  }

  sendBluetoothCommand(commandString);
}

// Process timers to detect when keyup messages have to be sent for each key
void processKeyUps() {
  for (int i=0; i<KEY_COUNT; i++) {
    if (keyPressStates[i] == true) {
      if (millis() - keyPressTimes[i] > keyHoldDurations[i]) {
        keyPressStates[i] = false;
        keyPressStateChanged = true;
      }
    }
  }
}
