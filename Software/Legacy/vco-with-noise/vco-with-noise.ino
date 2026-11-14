
#include <Arduino.h>
#include <math.h>

// -------- User settings --------

const uint8_t DAC_P   = PIN_PA6;
const uint8_t CV_P    = PIN_PA2;
const uint8_t WAVE_P  = PIN_PA1;
const uint8_t SHAPE_P = PIN_PA3;
const uint8_t AMP_P   = PIN_PA7;  // NEW

const uint32_t SAMPLE_RATE = 40000;

const float MIN_FREQ = 50.0f;
const float VOLTS_PER_OCTAVE = 0.75f;
const float SUPPLY_VOLTAGE = 5.0f;

const uint8_t LOW_LEVEL  = 8;
const uint8_t HIGH_LEVEL = 248;
const uint8_t MID_LEVEL  = 128;

// Waveform numbers
const uint8_t SINE     = 0;
const uint8_t TRIANGLE = 1;
const uint8_t SQUARE   = 2;
const uint8_t NOISE    = 3;

// -------- Quarter-sine table in flash --------

const uint8_t sineTable[65] = {
  128,131,134,137,140,143,146,149,
  151,154,157,160,163,166,168,171,
  174,177,179,182,185,187,190,192,
  195,197,199,202,204,206,209,211,
  213,215,217,219,221,223,224,226,
  228,229,231,232,234,235,236,238,
  239,240,241,242,243,244,244,245,
  246,246,247,247,247,248,248,248,
  248
};

// -------- Oscillator state --------

volatile uint32_t phase = 0;
volatile uint32_t phaseIncrement = 0;

volatile uint8_t waveform = SINE;

volatile uint16_t trianglePeak = 128;
volatile uint16_t riseScale = 480;
volatile uint16_t fallScale = 480;

volatile uint8_t pulseWidth = 128;

// NEW: amplitude, 0 to 255
volatile uint8_t amplitude = 0;

// 32-bit white noise generator
uint32_t rng = 0xACE12345;

// -------- Waveform interrupt --------


ISR(TCB0_INT_vect)
{
  phase += phaseIncrement;

  uint8_t p = phase >> 24;
  uint8_t output;

  // -------- Generate waveform --------

  if (waveform == SINE)
  {
    uint8_t q = p >> 6;
    uint8_t i = p & 63;

    switch (q)
    {
      case 0:
        output = sineTable[i];
        break;

      case 1:
        output = sineTable[64 - i];
        break;

      case 2:
        output = 256 - sineTable[i];
        break;

      default:
        output = 256 - sineTable[64 - i];
        break;
    }
  }
  else if (waveform == TRIANGLE)
  {
    if (p < trianglePeak)
    {
      output = LOW_LEVEL +
        (((uint32_t)p * riseScale) >> 8);
    }
    else
    {
      output = LOW_LEVEL +
        (((uint32_t)(256 - p) * fallScale) >> 8);
    }
  }
  else if (waveform == SQUARE)
  {
    output = (p < pulseWidth) ?
      HIGH_LEVEL : LOW_LEVEL;
  }
  else
  {
    // White noise: 32-bit xorshift
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;

    output = LOW_LEVEL +
      (((uint16_t)(uint8_t)rng * 240) >> 8);
  }

  // -------- Smooth amplitude --------

  // 16 fractional bits for precision
  static int32_t smoothAmp = 0;

  // Convert requested amplitude to fixed point
  int32_t targetAmp = (int32_t)amplitude << 16;

  // First-order low-pass filter
  // Time constant approximately 25.6 ms
  smoothAmp += (targetAmp - smoothAmp) >> 10;

  // Recover 8-bit amplitude
  uint8_t currentAmp = smoothAmp >> 16;

  // -------- Apply amplitude --------

  // Centre waveform around zero
  int16_t centred = (int16_t)output - MID_LEVEL;

  // Multiply by smoothed amplitude
  int32_t scaled = (int32_t)centred * currentAmp;

  // Restore midpoint and output to DAC
  DAC0.DATA = MID_LEVEL + (scaled >> 8);

  // Clear timer interrupt flag
  TCB0.INTFLAGS = TCB_CAPT_bm;
}


// -------- Initialisation --------

void setup()
{
  DACReference(INTERNAL4V3);
  analogWrite(DAC_P, MID_LEVEL);

  analogReference(VDD);

  pinMode(CV_P, INPUT);
  pinMode(WAVE_P, INPUT);
  pinMode(SHAPE_P, INPUT);
  pinMode(AMP_P, INPUT);  // NEW

  // Configure 40 kHz timer
  TCB0.CTRLA = 0;
  TCB0.CTRLB = TCB_CNTMODE_INT_gc;

  TCB0.CCMP = (F_CPU / SAMPLE_RATE) - 1;

  TCB0.INTFLAGS = TCB_CAPT_bm;
  TCB0.INTCTRL = TCB_CAPT_bm;

  TCB0.CTRLA =
    TCB_CLKSEL_CLKDIV1_gc | TCB_ENABLE_bm;
}

// -------- Main control loop --------

void loop()
{
  static int32_t filteredADC = -1;
  static uint8_t waveCounter = 0;
  static uint8_t selectedWave = SINE;

  // ----- Exponential frequency control -----

  uint16_t adc = analogRead(CV_P);

  int32_t target = (int32_t)adc * 256;

  if (filteredADC < 0)
  {
    filteredADC = target;
  }
  else if (abs(target - filteredADC) > 2048)
  {
    filteredADC = target;
  }
  else
  {
    filteredADC += (target - filteredADC) / 16;
  }

  float voltage =
    (filteredADC / 256.0f) *
    SUPPLY_VOLTAGE / 1023.0f;

  float frequency =
    MIN_FREQ * powf(2.0f,
      voltage / VOLTS_PER_OCTAVE);

  uint32_t increment =
    (uint32_t)(frequency *
               4294967296.0 / SAMPLE_RATE);

  // ----- Four-position waveform selector -----

  if (++waveCounter >= 20)
  {
    waveCounter = 0;

    uint16_t w = analogRead(WAVE_P);

    if (w < 170)
      selectedWave = SINE;
    else if (w < 512)
      selectedWave = TRIANGLE;
    else if (w < 852)
      selectedWave = SQUARE;
    else
      selectedWave = NOISE;
  }

  // ----- Waveform shaping -----

  uint16_t shape = analogRead(SHAPE_P);

  uint16_t peak = 128 +
    ((uint32_t)shape * 128 / 1023);

  uint16_t rising =
    ((uint32_t)240 * 256) / peak;

  uint16_t falling = 0;

  if (peak < 256)
  {
    falling =
      ((uint32_t)240 * 256) / (256 - peak);
  }

  uint8_t width = 128 -
    ((uint32_t)shape * 96 / 1023);

  // ----- NEW: amplitude control -----

  uint16_t ampADC = analogRead(AMP_P);

  // Scale 10-bit ADC to 8-bit amplitude
  uint8_t newAmplitude = ampADC >> 2;

  // ----- Atomic state update -----

  noInterrupts();

  phaseIncrement = increment;
  waveform = selectedWave;
  trianglePeak = peak;
  riseScale = rising;
  fallScale = falling;
  pulseWidth = width;
  amplitude = newAmplitude;

  interrupts();

  delay(5);
}
