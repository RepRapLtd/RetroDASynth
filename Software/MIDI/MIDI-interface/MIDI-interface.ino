
#include <MIDI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <math.h>

// Hardware
const int GATE_PIN = 2;
const int DAC_PIN  = A0;

const int OLED_WIDTH  = 128;
const int OLED_HEIGHT = 64;
const int OLED_ADDR   = 0x3C;

// MIDI and voltage settings
const float DAC_REFERENCE = 5.0f;
const float VOLTS_PER_OCTAVE = 0.75f;

MIDI_CREATE_INSTANCE(HardwareSerial, Serial1, MIDI);

Adafruit_SSD1306 display(
  OLED_WIDTH, OLED_HEIGHT, &Wire, -1
);

// State
bool notePlaying = false;
int currentNote = -1;
int lastNote = -1;
int lastVelocity = 0;
int lastChannel = 0;
bool displayDirty = true;

unsigned long lastDisplayTime = 0;

// Convert MIDI note number to pitch voltage
void setPitch(int note) {
  float volts = VOLTS_PER_OCTAVE *
    (log2f(440.0f / 50.0f) +
     (note - 69) / 12.0f);

  volts = constrain(volts, 0.0f, DAC_REFERENCE);

  int value = (int)roundf(
    volts * 4095.0f / DAC_REFERENCE
  );

  analogWrite(DAC_PIN, value);
}

// Convert MIDI note to name, e.g. C4
void noteName(int note, char *buffer) {
  if (note < 0) {
    strcpy(buffer, "---");
    return;
  }

  const char *names[] = {
    "C", "C#", "D", "D#", "E", "F",
    "F#", "G", "G#", "A", "A#", "B"
  };

  int octave = note / 12 - 1;
  sprintf(buffer, "%s%d", names[note % 12], octave);
}

// MIDI callbacks
void handleNoteOn(byte channel, byte note, byte velocity) {
  if (velocity == 0) {
    if (notePlaying && currentNote == note) {
      digitalWrite(GATE_PIN, HIGH);
      notePlaying = false;
      displayDirty = true;
    }
    return;
  }

  currentNote = note;
  lastNote = note;
  lastVelocity = velocity;
  lastChannel = channel;

  setPitch(note);

  // Retrigger envelope for a new note
  digitalWrite(GATE_PIN, HIGH);
  digitalWrite(GATE_PIN, LOW);

  notePlaying = true;
  displayDirty = true;
}

void handleNoteOff(byte channel, byte note, byte velocity) {
  if (notePlaying && currentNote == note) {
    digitalWrite(GATE_PIN, HIGH);
    notePlaying = false;
    displayDirty = true;
  }
}

// Draw display
void updateDisplay() {
  char name[8];
  noteName(lastNote, name);

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // Header
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("MIDI CH OMNI");

  display.setCursor(86, 0);
  display.print("POLY 1");

  display.drawLine(0, 11, 127, 11, SSD1306_WHITE);

  // Labels
  display.setCursor(0, 16);
  display.print("LAST NOTE");

  display.setCursor(79, 16);
  display.print("VOICES");

  // Large values
  display.setTextSize(2);
  display.setCursor(0, 28);
  display.print(name);

  display.setCursor(82, 28);
  display.print(notePlaying ? "1/1" : "0/1");

  // Voice indicator
  display.drawRect(0, 47, 128, 5, SSD1306_WHITE);
  if (notePlaying) {
    display.fillRect(1, 48, 126, 3, SSD1306_WHITE);
  }

  // Bottom status line
  display.setTextSize(1);
  display.setCursor(0, 56);
  display.print("Vel:");
  display.print(lastVelocity);

  display.setCursor(75, 56);
  display.print("Bend:0");

  display.display();
}

void setup() {
  pinMode(GATE_PIN, OUTPUT);
  digitalWrite(GATE_PIN, HIGH);

  analogWriteResolution(12);
  analogWrite(DAC_PIN, 0);

  Serial.begin(115200);

  // MIDI DIN input on Serial1 (D0/RX)
  MIDI.begin(MIDI_CHANNEL_OMNI);
  MIDI.turnThruOff();

  MIDI.setHandleNoteOn(handleNoteOn);
  MIDI.setHandleNoteOff(handleNoteOff);

  // OLED on A4/A5
  Wire.begin();

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED not found");
  } else {
    display.clearDisplay();
    display.display();
  }

  displayDirty = true;
}

void loop() {
  MIDI.read();

  // Limit screen updates to 10 per second
  if (displayDirty &&
      millis() - lastDisplayTime >= 100) {
    updateDisplay();
    displayDirty = false;
    lastDisplayTime = millis();
  }
}
