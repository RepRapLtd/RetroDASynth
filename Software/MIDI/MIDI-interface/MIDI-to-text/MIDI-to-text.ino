
#include <MIDI.h>

// ============================================================
// Arduino UNO R4 WiFi - Monophonic MIDI to CV
//
// MIDI input: Serial1 (D0)
// Pitch CV:   A0 (12-bit DAC, 0-3.3V)
// Gate:       D2 (active LOW)
//
// VCO calibration:
//   50 Hz at 0V
//   0.75V per octave
// ============================================================

MIDI_CREATE_INSTANCE(HardwareSerial, Serial1, MIDI);

const int DAC_PIN  = A0;
const int GATE_PIN = 2;

const float DAC_REFERENCE = 3.3f;
const float VOLTS_PER_OCTAVE = 0.75f;
const float BASE_FREQUENCY = 50.0f;

// MIDI note 69 is A4 = 440 Hz
const int REFERENCE_NOTE = 69;
const float REFERENCE_FREQUENCY = 440.0f;

int currentNote = -1;

// ============================================================
// Convert MIDI note number to DAC value
// ============================================================

void setPitch(byte note)
{
  // One semitone is 1/12 octave.
  float voltage =
      VOLTS_PER_OCTAVE *
      (log2f(REFERENCE_FREQUENCY / BASE_FREQUENCY)
       + ((int)note - REFERENCE_NOTE) / 12.0f);

  // Keep within the DAC output range.
  voltage = constrain(voltage, 0.0f, DAC_REFERENCE);

  int dacValue =
      (int)(voltage * 4095.0f / DAC_REFERENCE + 0.5f);

  analogWrite(DAC_PIN, dacValue);
}

// ============================================================
// MIDI note handlers
// ============================================================

void handleNoteOn(byte channel, byte note, byte velocity)
{
  if (velocity == 0)
  {
    // MIDI convention: Note On with zero velocity
    // is equivalent to Note Off.
    if (currentNote == note)
    {
      digitalWrite(GATE_PIN, HIGH);
      currentNote = -1;
    }
    return;
  }

  currentNote = note;

  // Set pitch before activating the gate.
  setPitch(note);
  digitalWrite(GATE_PIN, LOW);

  Serial.print("Note ON: ");
  Serial.println(note);
}

void handleNoteOff(byte channel, byte note, byte velocity)
{
  // Ignore Note Off for an earlier note.
  if (currentNote == note)
  {
    digitalWrite(GATE_PIN, HIGH);
    currentNote = -1;
  }

  Serial.print("Note OFF: ");
  Serial.println(note);
}

// ============================================================
// Setup
// ============================================================

void setup()
{
  Serial.begin(115200);

  pinMode(GATE_PIN, OUTPUT);

  // Gate inactive at startup
  digitalWrite(GATE_PIN, HIGH);

  // Enable 12-bit DAC resolution
  analogWriteResolution(12);

  // Start with zero pitch voltage
  analogWrite(DAC_PIN, 0);

  // MIDI input
  MIDI.begin(MIDI_CHANNEL_OMNI);
  MIDI.turnThruOff();

  MIDI.setHandleNoteOn(handleNoteOn);
  MIDI.setHandleNoteOff(handleNoteOff);

  Serial.println("MIDI to CV ready");
}

// ============================================================
// Main loop
// ============================================================

void loop()
{
  MIDI.read();
}
