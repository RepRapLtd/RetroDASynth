
#include <Arduino.h>
#include <math.h>

// ============================================================
// AVR32DA28 FOUR-MODE RESONANT VCF
// With optional self-oscillation
//
// DxCore 1.6.2, 24 MHz internal clock
//
// PD0 : Cutoff frequency CV
// PF1 : Audio input
// PD3 : Resonance / bandwidth CV
// PD6 : DAC output
//
// PA5 : Filter mode bit 0
// PA6 : Filter mode bit 1
// PA4 : Self-oscillation enable (ground = ON)
//
// Modes:
//  0 Low-pass
//  1 High-pass
//  2 Band-pass
//  3 Notch
//
// Sampling rate: 32000 Hz
// ============================================================

const uint32_t SAMPLE_RATE = 32000;

const uint8_t CUTOFF_PIN = PIN_PD0;
const uint8_t AUDIO_PIN  = PIN_PF1;
const uint8_t RES_PIN    = PIN_PD3;
const uint8_t DAC_PIN    = PIN_PD6;

const float MIN_CUTOFF = 30.0f;
const float MAX_CUTOFF = 3000.0f;

const float MIN_Q = 0.7f;
const float MAX_Q = 6.0f;

const int32_t SCALE = 16384L;  // Q14

// Maximum permitted integrator state.
// Acts as a simple limiter during self-oscillation.
const int32_t STATE_LIMIT = 4096;

// ---- Active filter coefficients ----
// a1, a2, a3 are positive.
// k is signed to permit negative damping.

volatile uint16_t a1 = SCALE;
volatile uint16_t a2 = 0;
volatile uint16_t a3 = 0;
volatile int16_t k = 16384;

// ---- Integrator states ----

int32_t ic1eq = 0;
int32_t ic2eq = 0;

// ---- Control values sampled by interrupt ----

volatile uint8_t cutoffIndex = 0;
volatile uint8_t resonanceIndex = 0;

// ============================================================
// LIMIT FILTER STATE
// ============================================================

static inline int32_t limitState(int32_t x)
{
  if (x > STATE_LIMIT)
    return STATE_LIMIT;

  if (x < -STATE_LIMIT)
    return -STATE_LIMIT;

  return x;
}

// ============================================================
// AUDIO INTERRUPT
// ============================================================

ISR(TCB0_INT_vect)
{
  static uint8_t controlCounter = 0;
  static uint8_t controlChannel = 0;

  // Read audio, 12-bit ADC
  int32_t x =
      (int32_t)analogRead(AUDIO_PIN) - 2048;

  // ---- TPT state-variable filter ----

  int32_t v3 = x - ic2eq;

  int32_t v1 =
      ((int32_t)a1 * ic1eq +
       (int32_t)a2 * v3) / SCALE;

  int32_t v2 =
      ic2eq +
      (((int32_t)a2 * ic1eq +
        (int32_t)a3 * v3) / SCALE);

  // Update the two filter states
  ic1eq = limitState(2 * v1 - ic1eq);
  ic2eq = limitState(2 * v2 - ic2eq);

  // ---- Four simultaneous responses ----

  int32_t band = v1;
  int32_t low  = v2;

  int32_t dampedBand =
      ((int32_t)k * band) / SCALE;

  int32_t high = x - dampedBand - low;

  int32_t notch = x - dampedBand;

  // ---- Filter selection ----

  uint8_t mode =
      (!digitalRead(PIN_PA5) ? 1 : 0) |
      (!digitalRead(PIN_PA6) ? 2 : 0);

  int32_t output;

  switch (mode)
  {
    case 0:
      output = low;
      break;

    case 1:
      output = high;
      break;

    case 2:
      output = band;
      break;

    default:
      output = notch;
      break;
  }

  // Convert centred 12-bit audio to 10-bit DAC
  output = (output >> 2) + 512;

  // DAC output limits
  if (output < 0)
    output = 0;

  if (output > 1023)
    output = 1023;

  DAC0.DATA = ((uint16_t)output) << 6;

  // ---- Control voltage sampling ----
  // One extra ADC read every 64 samples.
  // Each control is refreshed 250 times/sec.

  if (++controlCounter == 64)
  {
    controlCounter = 0;

    if (controlChannel == 0)
    {
      cutoffIndex = analogRead(CUTOFF_PIN) >> 5;
      controlChannel = 1;
    }
    else
    {
      resonanceIndex = analogRead(RES_PIN) >> 5;
      controlChannel = 0;
    }
  }

  TCB0.INTFLAGS = TCB_CAPT_bm;
}

// ============================================================
// CALCULATE FILTER COEFFICIENTS
// ============================================================

void updateCoefficients(
    uint8_t cutoffControl,
    uint8_t resonanceControl,
    bool selfOscillation)
{
  // ---- Cutoff frequency ----

  float c = cutoffControl / 127.0f;

  float cutoff =
      MIN_CUTOFF *
      powf(MAX_CUTOFF / MIN_CUTOFF, c);

  // ---- Resonance ----

  float r = resonanceControl / 127.0f;

  float q =
      MIN_Q * powf(MAX_Q / MIN_Q, r);

  // Normal damping: k = 1/Q
  float damping = 1.0f / q;

  // ---- Self-oscillation ----
  //
  // With PA4 enabled, introduce negative
  // damping only near maximum resonance.
  //
  // The damping crosses zero close to the
  // top of the resonance control range.

  if (selfOscillation && r > 0.75f)
  {
    float amount = (r - 0.75f) / 0.25f;

    //damping -= 0.20f * amount;
    damping -= 0.18f * amount;
  }

  // ---- TPT coefficients ----

  float g = tanf(PI * cutoff / SAMPLE_RATE);

  float denominator =
      1.0f + g * (g + damping);

  float c1 = 1.0f / denominator;
  float c2 = g * c1;
  float c3 = g * c2;

  uint16_t newA1 = (uint16_t)(c1 * SCALE);
  uint16_t newA2 = (uint16_t)(c2 * SCALE);
  uint16_t newA3 = (uint16_t)(c3 * SCALE);

  int16_t newK =
      (int16_t)(damping * SCALE);

  // Update all coefficients together so
  // the ISR never sees a mixed set.

  noInterrupts();

  a1 = newA1;
  a2 = newA2;
  a3 = newA3;
  k  = newK;
  
  // Kick-start the oscillation only when
  // negative damping is actually enabled.
  static bool wasUnstable = false;
  
  bool unstable = (newK < 0);
  
  if (unstable && !wasUnstable)
  {
    ic1eq = 1024;
    ic2eq = 0;
  }
  
  wasUnstable = unstable;

 /* a1 = newA1;
  a2 = newA2;
  a3 = newA3;
  k  = newK;*/

  interrupts();
}

// ============================================================
// SETUP
// ============================================================

void setup()
{
  // Digital switches, active LOW
  pinMode(PIN_PA4, INPUT_PULLUP);
  pinMode(PIN_PA5, INPUT_PULLUP);
  pinMode(PIN_PA6, INPUT_PULLUP);

  // Analogue inputs
  pinMode(CUTOFF_PIN, INPUT);
  pinMode(AUDIO_PIN, INPUT);
  pinMode(RES_PIN, INPUT);

  analogReadResolution(12);
  analogReference(VDD);

  // DxCore 1.6.2 reference workaround
  VREF.ADC0REF =
      (VREF.ADC0REF & ~VREF_REFSEL_gm) |
      VREF_REFSEL_VDD_gc;

  // Shorter ADC acquisition time
  analogSampleDuration(4);

  // Discard first conversions
  (void)analogRead(CUTOFF_PIN);
  (void)analogRead(RES_PIN);
  (void)analogRead(AUDIO_PIN);

  // DAC initialisation
  DACReference(VDD);
  analogWrite(DAC_PIN, 128);
  DAC0.DATA = 512 << 6;

  // Read initial controls
  cutoffIndex =
      analogRead(CUTOFF_PIN) >> 5;

  resonanceIndex =
      analogRead(RES_PIN) >> 5;

  updateCoefficients(
      cutoffIndex,
      resonanceIndex,
      !digitalRead(PIN_PA4));

  // ---- 32 kHz timer ----

  TCB0.CTRLA = 0;

  TCB0.CTRLB = TCB_CNTMODE_INT_gc;

  TCB0.CCMP =
      (F_CPU / SAMPLE_RATE) - 1;

  TCB0.INTFLAGS = TCB_CAPT_bm;
  TCB0.INTCTRL = TCB_CAPT_bm;

  TCB0.CTRLA =
      TCB_CLKSEL_CLKDIV1_gc |
      TCB_ENABLE_bm;
}

// ============================================================
// MAIN LOOP
// ============================================================

void loop()
{
  static uint8_t oldCutoff = 255;
  static uint8_t oldResonance = 255;
  static bool oldOscillation = false;

  // Copy controls atomically
  uint8_t cutoff;
  uint8_t resonance;

  noInterrupts();

  cutoff = cutoffIndex;
  resonance = resonanceIndex;

  interrupts();

  bool oscillation = !digitalRead(PIN_PA4);

  if (cutoff != oldCutoff ||
      resonance != oldResonance ||
      oscillation != oldOscillation)
  {
    updateCoefficients(
        cutoff,
        resonance,
        oscillation);

    oldCutoff = cutoff;
    oldResonance = resonance;
    oldOscillation = oscillation;
  }
}
