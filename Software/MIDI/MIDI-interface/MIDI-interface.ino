#include <MIDI.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <math.h>

// ------------------------------------
// Hardware
// ------------------------------------

const int GATE_PIN = 2;
const int DAC_CS   = 10;

const int OLED_WIDTH  = 128;
const int OLED_HEIGHT = 64;
const int OLED_ADDR   = 0x3C;

// ------------------------------------
// DAC and pitch settings
// ------------------------------------

// MCP4822: 12-bit, internal 2.048 V
// reference, configured for 2x gain.
//
// Each DAC count = 1 mV nominally.

const float DAC_FULL_SCALE = 4.096f;
const int DAC_MAX_CODE = 4095;

const float VOLTS_PER_OCTAVE = 0.75f;

// ------------------------------------
// MIDI and OLED
// ------------------------------------

MIDI_CREATE_INSTANCE(HardwareSerial, Serial1, MIDI);

Adafruit_SSD1306 display(
  OLED_WIDTH, OLED_HEIGHT, &Wire, -1
);

// ------------------------------------
// State
// ------------------------------------

bool notePlaying = false;
int currentNote = -1;
int lastNote = -1;
int lastVelocity = 0;

bool displayDirty = true;
bool oledAvailable = false;
unsigned long lastDisplayTime = 0;

// ------------------------------------
// Write to MCP4822
//
// channel = 0: VOUTA (pitch)
// channel = 1: VOUTB (velocity)
//
// 2x gain, output enabled.
// LDAC is permanently grounded.
// ------------------------------------

void writeDAC(byte channel, uint16_t value) {

  // Bit 15: DAC channel (0=A, 1=B)
  // Bit 14: Unbuffered reference
  // Bit 13: Gain (0=2x)
  // Bit 12: Shutdown (1=active)
  // Bits 11..0: 12-bit DAC value

  uint16_t command = 0x1000;

  if (channel == 1) {
    command |= 0x8000;
  }

  command |= (value & 0x0FFF);

  SPI.beginTransaction(
    SPISettings(8000000, MSBFIRST, SPI_MODE0)
  );

  digitalWrite(DAC_CS, LOW);

  SPI.transfer(highByte(command));
  SPI.transfer(lowByte(command));

  digitalWrite(DAC_CS, HIGH);

  SPI.endTransaction();
}

// ------------------------------------
// Convert MIDI note to pitch voltage
// ------------------------------------

void setPitch(int note) {

  float volts = VOLTS_PER_OCTAVE *
    (log2f(440.0f / 50.0f) +
     (note - 69) / 12.0f);

  volts = constrain(
    volts, 0.0f, DAC_FULL_SCALE
  );

  int value = (int)roundf(
    volts * 4096.0f / DAC_FULL_SCALE
  );

  value = constrain(value, 0, DAC_MAX_CODE);

  writeDAC(0, value);
}

// ------------------------------------
// Convert MIDI velocity to CV
// ------------------------------------

void setVelocity(int velocity) {

  velocity = constrain(velocity, 0, 127);

  int value = (velocity * 4095L + 63) / 127;

  writeDAC(1, value);
}

// ------------------------------------
// MIDI note name
// ------------------------------------

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

  sprintf(
    buffer, "%s%d",
    names[note % 12], octave
  );
}

// ------------------------------------
// MIDI callbacks
// ------------------------------------

void handleNoteOff(byte channel,
                   byte note,
                   byte velocity) {

  if (notePlaying && currentNote == note) {

    digitalWrite(GATE_PIN, HIGH);

    notePlaying = false;
    displayDirty = true;
  }
}

void handleNoteOn(byte channel,
                  byte note,
                  byte velocity) {

  if (velocity == 0) {
    handleNoteOff(channel, note, velocity);
    return;
  }

  currentNote = note;
  lastNote = note;
  lastVelocity = velocity;

  setPitch(note);
  setVelocity(velocity);

  // Retrigger envelope
  digitalWrite(GATE_PIN, HIGH);
  digitalWrite(GATE_PIN, LOW);

  notePlaying = true;
  displayDirty = true;
}

// ------------------------------------
// OLED display
// ------------------------------------

void updateDisplay() {

  if (!oledAvailable) return;

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

  display.drawLine(
    0, 11, 127, 11, SSD1306_WHITE
  );

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
  display.drawRect(
    0, 47, 128, 5, SSD1306_WHITE
  );

  if (notePlaying) {
    display.fillRect(
      1, 48, 126, 3, SSD1306_WHITE
    );
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

// ------------------------------------
// Setup
// ------------------------------------

void setup() {

  pinMode(GATE_PIN, OUTPUT);
  digitalWrite(GATE_PIN, HIGH);

  pinMode(DAC_CS, OUTPUT);
  digitalWrite(DAC_CS, HIGH);

  Serial.begin(115200);

  // SPI DAC
  SPI.begin();

  writeDAC(0, 0);
  writeDAC(1, 0);

  // MIDI input
  MIDI.begin(MIDI_CHANNEL_OMNI);
  MIDI.turnThruOff();

  MIDI.setHandleNoteOn(handleNoteOn);
  MIDI.setHandleNoteOff(handleNoteOff);

  // OLED on A4/A5
  Wire.begin();

  oledAvailable = display.begin(
    SSD1306_SWITCHCAPVCC, OLED_ADDR
  );

  if (!oledAvailable) {
    Serial.println("OLED not found");
  } else {
    display.clearDisplay();
    display.display();
  }

  displayDirty = true;
}

// ------------------------------------
// Main loop
// ------------------------------------

void loop() {

  MIDI.read();

  if (displayDirty &&
      millis() - lastDisplayTime >= 100) {

    updateDisplay();

    displayDirty = false;
    lastDisplayTime = millis();
  }
}
