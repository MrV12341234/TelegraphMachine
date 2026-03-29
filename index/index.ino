#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SoftwareSerial.h>
#include <string.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDR 0x3C

// ---------------- Pins ----------------
const int BUTTON_PIN = 2;
const int BUZZER_PIN = 6;

// Board A D11 -> Board B D10
// Board B D11 -> Board A D10
const int LINK_RX_PIN = 10;
const int LINK_TX_PIN = 11;

// ---------------- Timing ----------------
const unsigned long DEBOUNCE_MS           = 25;
const unsigned long MIN_PRESS_MS          = 40;
const unsigned long DOT_DASH_THRESHOLD_MS = 250;

const unsigned long LETTER_GAP_MS   = 1000; // end of letter
const unsigned long WORD_GAP_MS     = 3000; // end of word
const unsigned long MESSAGE_END_MS  = 7000; // end of full message

const unsigned long OVERLAY_MS      = 3000;
const unsigned long RENDER_MS       = 100;

// ---------------- Morse storage ----------------
const int MESSAGE_MAX = 110;  // full committed message shown on screen
const int WORD_MAX    = 48;   // current word (hidden on receiver until complete)
const int LETTER_MAX  = 8;    // current letter in Morse

// ---------------- Buzzer ----------------
const int BEEP_FREQ = 1200;
const unsigned long DOT_BEEP_MS  = 90;
const unsigned long DASH_BEEP_MS = 220;

// ---------------- Objects ----------------
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
SoftwareSerial linkSerial(LINK_RX_PIN, LINK_TX_PIN); // RX, TX

// ---------------- State ----------------
enum Mode {
  MODE_IDLE,
  MODE_SENDING,
  MODE_RECEIVING,
  MODE_RX_DONE,
  MODE_WAIT_CLEAR
};

enum OverlayType {
  OVERLAY_NONE,
  OVERLAY_SENT,
  OVERLAY_RX_FINISHED
};

Mode mode = MODE_IDLE;
OverlayType overlayType = OVERLAY_NONE;
unsigned long overlayUntil = 0;

char messageBuffer[MESSAGE_MAX + 1]; // completed visible message
char currentWord[WORD_MAX + 1];      // current word
char currentLetter[LETTER_MAX + 1];  // current letter

bool lastRawButton = HIGH;
bool stableButton = HIGH;
unsigned long lastDebounceTime = 0;
unsigned long pressStartTime = 0;

unsigned long lastSymbolReleaseTime = 0;
bool hasMessageContent = false;
bool startTokenSent = false;
bool letterGapCommitted = false;
bool wordGapCommitted = false;

unsigned long lastRenderTime = 0;

// ---------------- Utility ----------------
void clearBuffers() {
  messageBuffer[0] = '\0';
  currentWord[0] = '\0';
  currentLetter[0] = '\0';
}

bool overlayActive() {
  return millis() < overlayUntil;
}

void showOverlay(OverlayType t) {
  overlayType = t;
  overlayUntil = millis() + OVERLAY_MS;
}

void clearOverlay() {
  overlayType = OVERLAY_NONE;
  overlayUntil = 0;
}

void appendCharSafe(char* dest, int maxLen, char c) {
  int len = strlen(dest);
  if (len < maxLen) {
    dest[len] = c;
    dest[len + 1] = '\0';
  }
}

void appendTextSafe(char* dest, int maxLen, const char* src) {
  int len = strlen(dest);
  while (*src && len < maxLen) {
    dest[len++] = *src++;
  }
  dest[len] = '\0';
}

void beepForSymbol(char symbol) {
  if (symbol == '.') {
    tone(BUZZER_PIN, BEEP_FREQ, DOT_BEEP_MS);
  } else if (symbol == '-') {
    tone(BUZZER_PIN, BEEP_FREQ, DASH_BEEP_MS);
  }
}

void sendToken(char c) {
  linkSerial.write(c);
}

// ---------------- Buffer builders ----------------
void finalizeCurrentLetterToWord() {
  if (currentLetter[0] == '\0') return;
  appendTextSafe(currentWord, WORD_MAX, currentLetter);
  appendCharSafe(currentWord, WORD_MAX, ' ');
  currentLetter[0] = '\0';
}

void appendCurrentWordToMessageWithSlash() {
  if (currentWord[0] == '\0') return;
  appendTextSafe(messageBuffer, MESSAGE_MAX, currentWord);
  appendTextSafe(messageBuffer, MESSAGE_MAX, "/ ");
  currentWord[0] = '\0';
}

void appendCurrentWordToMessageNoSlash() {
  if (currentWord[0] == '\0') return;
  appendTextSafe(messageBuffer, MESSAGE_MAX, currentWord);
  currentWord[0] = '\0';
}

// ---------------- Session control ----------------
void startSendingSession() {
  clearBuffers();
  clearOverlay();

  mode = MODE_SENDING;
  hasMessageContent = false;
  startTokenSent = false;
  letterGapCommitted = false;
  wordGapCommitted = false;
  lastSymbolReleaseTime = 0;
}

void finishSendingSession() {
  // Flush pending letter/word before ending
  if (currentLetter[0] != '\0') {
    finalizeCurrentLetterToWord();
    sendToken(' ');   // end letter
  }

  if (currentWord[0] != '\0') {
    appendCurrentWordToMessageNoSlash();
    sendToken('>');   // final word, no slash
  }

  sendToken('|');     // end of message

  mode = MODE_WAIT_CLEAR;
  showOverlay(OVERLAY_SENT);

  clearBuffers();
  hasMessageContent = false;
  startTokenSent = false;
  letterGapCommitted = false;
  wordGapCommitted = false;
}

void startReceivingSession() {
  clearBuffers();
  clearOverlay();
  mode = MODE_RECEIVING;
}

void finishReceivingSession() {
  if (currentLetter[0] != '\0') {
    finalizeCurrentLetterToWord();
  }
  if (currentWord[0] != '\0') {
    appendCurrentWordToMessageNoSlash();
  }

  mode = MODE_RX_DONE;
  showOverlay(OVERLAY_RX_FINISHED);
}

void clearReceivedMessage() {
  sendToken('C');   // tell sender it may return to idle
  clearBuffers();
  clearOverlay();
  mode = MODE_IDLE;
}

// ---------------- Incoming serial ----------------
// Tokens:
// 'S' = start message
// '.' '-' = Morse symbols
// ' ' = end of letter
// '/' = end of word
// '>' = flush final word without slash
// '|' = end of message
// 'C' = receiver cleared message
void handleIncomingToken(char token) {
  if (token == 'C') {
    if (mode == MODE_WAIT_CLEAR) {
      clearBuffers();
      clearOverlay();
      mode = MODE_IDLE;
    }
    return;
  }

  if (token == 'S') {
    if (mode == MODE_IDLE) {
      startReceivingSession();
    }
    return;
  }

  if (mode != MODE_RECEIVING) return;

  if (token == '.' || token == '-') {
    appendCharSafe(currentLetter, LETTER_MAX, token);
    beepForSymbol(token);
    return;
  }

  if (token == ' ') {
    finalizeCurrentLetterToWord();
    return;
  }

  if (token == '/') {
    finalizeCurrentLetterToWord();
    appendCurrentWordToMessageWithSlash();
    return;
  }

  if (token == '>') {
    finalizeCurrentLetterToWord();
    appendCurrentWordToMessageNoSlash();
    return;
  }

  if (token == '|') {
    finishReceivingSession();
    return;
  }
}

void processIncomingSerial() {
  while (linkSerial.available()) {
    char c = (char)linkSerial.read();
    handleIncomingToken(c);
  }
}

// ---------------- Button handling ----------------
void processButton() {
  bool raw = digitalRead(BUTTON_PIN);

  if (raw != lastRawButton) {
    lastDebounceTime = millis();
    lastRawButton = raw;
  }

  if ((millis() - lastDebounceTime) <= DEBOUNCE_MS) return;

  if (raw != stableButton) {
    stableButton = raw;

    // Button pressed
    if (stableButton == LOW) {
      if (overlayActive()) return;

      if (mode == MODE_RX_DONE) {
        clearReceivedMessage();
        return;
      }

      if (mode == MODE_RECEIVING || mode == MODE_WAIT_CLEAR) {
        return; // locked during incoming message or waiting for receiver clear
      }

      if (mode == MODE_IDLE) {
        startSendingSession();
      }

      if (mode == MODE_SENDING) {
        pressStartTime = millis();
      }
    }
    // Button released
    else {
      if (mode != MODE_SENDING) return;

      unsigned long pressDuration = millis() - pressStartTime;
      if (pressDuration < MIN_PRESS_MS) return;

      char symbol = (pressDuration < DOT_DASH_THRESHOLD_MS) ? '.' : '-';

      if (!startTokenSent) {
        sendToken('S');
        startTokenSent = true;
      }

      appendCharSafe(currentLetter, LETTER_MAX, symbol);
      sendToken(symbol);
      beepForSymbol(symbol);

      hasMessageContent = true;
      lastSymbolReleaseTime = millis();
      letterGapCommitted = false;
      wordGapCommitted = false;
    }
  }
}

// ---------------- Gap timing ----------------
void processSendingGapTimers() {
  if (mode != MODE_SENDING) return;
  if (stableButton == LOW) return; // still holding button
  if (!hasMessageContent) return;

  unsigned long gap = millis() - lastSymbolReleaseTime;

  if (!letterGapCommitted && currentLetter[0] != '\0' && gap >= LETTER_GAP_MS) {
    finalizeCurrentLetterToWord();
    sendToken(' ');
    letterGapCommitted = true;
  }

  if (!wordGapCommitted && currentWord[0] != '\0' && gap >= WORD_GAP_MS) {
    appendCurrentWordToMessageWithSlash();
    sendToken('/');
    wordGapCommitted = true;
  }

  if (gap >= MESSAGE_END_MS) {
    finishSendingSession();
  }
}

// ---------------- Display rendering ----------------
int bodyRow = 1;
int bodyCol = 0;

void beginBodyText() {
  bodyRow = 1;
  bodyCol = 0;
}

void drawBodyChar(char c) {
  if (bodyRow > 7) return;

  display.setCursor(bodyCol * 6, bodyRow * 8);
  display.write(c);

  bodyCol++;
  if (bodyCol >= 21) {
    bodyCol = 0;
    bodyRow++;
  }
}

void drawBodyText(const char* text) {
  for (int i = 0; text[i] != '\0'; i++) {
    drawBodyChar(text[i]);
  }
}

void drawOverlay() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  if (overlayType == OVERLAY_SENT) {
    display.setCursor(0, 8);
    display.println(F("Message sent"));
    display.setCursor(0, 24);
    display.println(F("Receiver clears it"));
  } else if (overlayType == OVERLAY_RX_FINISHED) {
    display.setCursor(0, 8);
    display.println(F("Message finished"));
    display.setCursor(0, 24);
    display.println(F("Press btn to clear"));
  }

  display.display();
}

void renderDisplay() {
  if (millis() - lastRenderTime < RENDER_MS) return;
  lastRenderTime = millis();

  if (overlayActive()) {
    drawOverlay();
    return;
  }

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  if (mode == MODE_IDLE) {
    display.println(F("Press btn to send"));
  } else if (mode == MODE_SENDING) {
    display.println(F("Sending..."));
  } else if (mode == MODE_RECEIVING) {
    display.println(F("Incoming message"));
  } else if (mode == MODE_RX_DONE) {
    display.println(F("Press btn to clear"));
  } else if (mode == MODE_WAIT_CLEAR) {
    display.println(F("Waiting on clear"));
  }

  beginBodyText();

  if (mode == MODE_SENDING) {
    drawBodyText(messageBuffer);
    drawBodyText(currentWord);
    drawBodyText(currentLetter);
  } else if (mode == MODE_RECEIVING || mode == MODE_RX_DONE) {
    drawBodyText(messageBuffer); // receiver only shows completed words
  } else if (mode == MODE_WAIT_CLEAR) {
    drawBodyText("Receiver decoding");
  }

  display.display();
}

// ---------------- Setup / Loop ----------------
void setup() {
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);

  Wire.begin();
  linkSerial.begin(9600);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    while (true) {}
  }

  clearBuffers();
  clearOverlay();

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(F("Press btn to send"));
  display.display();
}

void loop() {
  processIncomingSerial();
  processButton();
  processSendingGapTimers();
  renderDisplay();
}