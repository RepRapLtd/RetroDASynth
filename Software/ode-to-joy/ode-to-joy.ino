
#include <Arduino.h>

// -------- User settings --------

const uint8_t DAC_P = PIN_PA6;
const uint8_t ENV_P = PIN_PA3;

const uint16_t BPM = 60;       // Was 120
const uint16_t ATTACK_MS = 200; // Was 25

//const uint16_t BPM = 120;
const uint16_t PAUSE_MS = 1500;
const uint16_t NOTE_GAP_MS = 25;


// Envelope settings
const uint16_t ENV_STEP_MS = 5;
//const uint16_t ATTACK_MS = 25;

const uint16_t FAST_DECAY_MS = 240; 
const uint8_t FAST_DECAY_FACTOR = 40;
const uint8_t SLOW_DECAY_FACTOR = 150;
// Two-stage decay
//const uint16_t FAST_DECAY_MS = 120;
//const uint8_t FAST_DECAY_FACTOR = 20;
//const uint8_t SLOW_DECAY_FACTOR = 150;

// Larger value = slower decay
const uint8_t DECAY_FACTOR = 64;

// -------- Pitch voltages --------
//
// VCO: 50 Hz at 0 V, 0.75 V/octave
// DAC: 4.3 V reference, 8-bit

const uint8_t N_C4 = 107;
const uint8_t N_D4 = 114;
const uint8_t N_E4 = 121;
const uint8_t N_F4 = 125;
const uint8_t N_G4 = 133;

// -------- Melody in flash --------

struct Note {
  uint8_t pitch;
  uint8_t duration;
};

const Note melody[] = {
  {N_E4,1}, {N_E4,1}, {N_F4,1}, {N_G4,1},
  {N_G4,1}, {N_F4,1}, {N_E4,1}, {N_D4,1},
  {N_C4,1}, {N_C4,1}, {N_D4,1}, {N_E4,1},
  {N_E4,1}, {N_D4,1}, {N_D4,2},

  {N_E4,1}, {N_E4,1}, {N_F4,1}, {N_G4,1},
  {N_G4,1}, {N_F4,1}, {N_E4,1}, {N_D4,1},
  {N_C4,1}, {N_C4,1}, {N_D4,1}, {N_E4,1},
  {N_D4,1}, {N_C4,1}, {N_C4,2}
};

const uint8_t NOTE_COUNT =
  sizeof(melody) / sizeof(melody[0]);

// -------- Initialisation --------

void setup()
{
  // Pitch DAC
  DACReference(INTERNAL4V3);
  analogWrite(DAC_P, 0);

  // Envelope PWM
  pinMode(ENV_P, OUTPUT);
  analogWrite(ENV_P, 0);
}

// -------- Play one note --------


void playNote(uint8_t pitch, uint8_t duration)
{
  uint32_t length =
    (uint32_t)60000 * duration / BPM;

  uint32_t soundingTime = length - NOTE_GAP_MS;

analogWrite(ENV_P, 0);
DAC0.DATA = pitch;

// Allow pitch-control circuitry to settle
delay(20);

  //DAC0.DATA = pitch;
  //analogWrite(ENV_P, 0);

  uint32_t envelope = 0;
  uint32_t elapsed = 0;

  while (elapsed < soundingTime)
  {
    if (elapsed < ATTACK_MS)
    {
      // Smooth raised-cosine attack
      float t = (float)(elapsed + ENV_STEP_MS)
                / ATTACK_MS;

      if (t > 1.0f) t = 1.0f;

      envelope = (uint32_t)(
        65535.0f * 0.5f *
        (1.0f - cosf(PI * t))
      );
    }
    else if (elapsed < ATTACK_MS + FAST_DECAY_MS)
    {
      // Rapid initial decay
      envelope -= envelope / FAST_DECAY_FACTOR;
    }
    else
    {
      // Slower decay for lingering tail
      envelope -= envelope / SLOW_DECAY_FACTOR;
    }

    analogWrite(ENV_P, envelope >> 8);

    uint16_t step = ENV_STEP_MS;

    if (elapsed + step > soundingTime)
      step = soundingTime - elapsed;

    delay(step);
    elapsed += step;
  }

  // Note off
  analogWrite(ENV_P, 0);
  //DAC0.DATA = 0;

  delay(NOTE_GAP_MS);
}


// -------- Main loop --------

void loop()
{
  for (uint8_t i = 0; i < NOTE_COUNT; i++)
  {
    playNote(melody[i].pitch,
             melody[i].duration);
  }

  analogWrite(ENV_P, 0);
  DAC0.DATA = 0;

  delay(PAUSE_MS);
}
