
#include <Arduino.h>
#include <math.h>

// ============================================================
// AVR32DA28 VCO WITH AUXILIARY PWM REFERENCE
//
// DxCore 1.6.2, 24 MHz internal clock
//
// PD0  Pitch CV
// PD1  Pitch CV
// PD2  Pitch CV
// PD3  Volume CV
// PD4  Volume CV
// PD5  Volume CV
// PD7  Shape CV
// PF0  Shape CV
// PF1  Shape CV
// PD6  10-bit DAC output
//
// PC0  PWM reference output (93.75 kHz)
//
// PA4  Frequency range (LOW = low range)
// PA5  Waveform bit 0
// PA6  Waveform bit 1
//
// Waveforms:
//   3 Sine
//   2 Triangle / sawtooth
//   1 Square / pulse
//   0 White noise / low-frequency random walk
//
// Sample rate: 40000 Hz
// Pitch: 0.75 V/octave
// ============================================================

const uint32_t SAMPLE_RATE = 40000;

const float HIGH_BASE = 50.0f;
const float LOW_BASE = 0.05f;
const float VOLTS_PER_OCTAVE = 0.75f;

const int16_t DAC_MID = 512;
const int16_t DAC_AMP = 480;

const int16_t WALK_STEP = 20;
const float WALK_RATE_MULTIPLIER = 10.0f;

// -------- Waveform buffers --------

uint16_t waveTable[2][256];

volatile uint8_t activeTable = 0;
volatile uint8_t waveform = 0;
volatile uint8_t lowNoiseMode = 0;

// 256 = unity gain
// 512 = twice unity
volatile uint16_t volumeGain = 0;

volatile int16_t driftOffset = 0;

// -------- Oscillator state --------

volatile uint32_t phase = 0;
volatile uint32_t phaseIncrement = 0;

uint32_t rng = 0xACE12345UL;

// ============================================================
// AUDIO INTERRUPT
// ============================================================

ISR(TCB0_INT_vect)
{
  phase += phaseIncrement;

  uint16_t sample;

  if (waveform == 3)
  {
    if (lowNoiseMode)
    {
      sample = DAC_MID + driftOffset;
    }
    else
    {
      // 32-bit xorshift white noise
      rng ^= rng << 13;
      rng ^= rng >> 17;
      rng ^= rng << 5;

      sample = rng & 1023;
    }
  }
  else
  {
    uint8_t index = phase >> 24;
    sample = waveTable[activeTable][index];
  }

  // Unipolar scaling: both amplitude and
  // DC midpoint depend on volumeGain.

  int32_t output =
      ((int32_t)sample * volumeGain) >> 8;

  if (output > 1023)
    output = 1023;

  DAC0.DATA = ((uint16_t)output) << 6;

  TCB0.INTFLAGS = TCB_CAPT_bm;
}

// ============================================================
// BUILD WAVEFORM TABLE
// ============================================================

void buildWaveform(uint8_t type, uint8_t shape)
{
  uint8_t next = activeTable ^ 1;

  for (uint16_t i = 0; i < 256; i++)
  {
    int16_t value = 0;

    switch (type)
    {
      case 0:
        value = (int16_t)
          (DAC_AMP *
           sinf(2.0f * PI * i / 256.0f));
        break;

      case 1:
      {
        uint16_t peak =
          1 + (uint16_t)shape * 254 / 255;

        if (i < peak)
          value = -DAC_AMP +
            (int32_t)2 * DAC_AMP * i / peak;
        else
          value = DAC_AMP -
            (int32_t)2 * DAC_AMP *
            (i - peak) / (256 - peak);

        break;
      }

      case 2:
      {
        uint16_t duty =
          26 + (uint16_t)shape * 204 / 255;

        value =
          (i < duty) ? DAC_AMP : -DAC_AMP;
        break;
      }
    }

    waveTable[next][i] = DAC_MID + value;
  }

  noInterrupts();
  activeTable = next;
  interrupts();
}

// ============================================================
// PWM REFERENCE SETUP
//
// TCA0, single-slope PWM
// 24 MHz / 256 = 93750 Hz
//
// WO0 routed to PC0.
// ============================================================

void setupPWM()
{
  // Release TCA0 from DxCore's normal PWM
  // management and reset its registers.
  takeOverTCA0();

  // Route TCA0 outputs to PORTC
  PORTMUX.TCAROUTEA =
      PORTMUX_TCA0_PORTC_gc;

  pinMode(PIN_PC0, OUTPUT);

  TCA0.SINGLE.CTRLA = 0;

  TCA0.SINGLE.CTRLB =
      TCA_SINGLE_CMP0EN_bm |
      TCA_SINGLE_WGMODE_SINGLESLOPE_gc;

  TCA0.SINGLE.PER = 255;
  TCA0.SINGLE.CMP0 = 0;

  TCA0.SINGLE.CTRLA =
      TCA_SINGLE_CLKSEL_DIV1_gc |
      TCA_SINGLE_ENABLE_bm;
}

// ============================================================
// PWM REFERENCE UPDATE
//
// The unscaled waveform midpoint is 512.
//
// DAC midpoint after volume scaling:
//   512 * volumeGain / 256
//
// PWM duty factor needed for the same voltage:
//   volumeGain / 512
//
// PWM compare value:
//   approximately volumeGain / 2
// ============================================================

void updatePWM(uint16_t gain)
{
  uint16_t duty = (gain + 1) >> 1;

  if (duty > 255)
    duty = 255;

  // Buffered compare update avoids changing
  // duty cycle halfway through a PWM period.
  TCA0.SINGLE.CMP0BUF = duty;
}

// ============================================================
// SETUP
// ============================================================

void setup()
{
  pinMode(PIN_PA4, INPUT_PULLUP);
  pinMode(PIN_PA5, INPUT_PULLUP);
  pinMode(PIN_PA6, INPUT_PULLUP);

  pinMode(PIN_PD0, INPUT);
  pinMode(PIN_PD1, INPUT);
  pinMode(PIN_PD2, INPUT);
  pinMode(PIN_PD3, INPUT);
  pinMode(PIN_PD4, INPUT);
  pinMode(PIN_PD5, INPUT);
  pinMode(PIN_PD7, INPUT);
  pinMode(PIN_PF0, INPUT);
  pinMode(PIN_PF1, INPUT);
    
  analogReadResolution(12);
  analogReference(VDD);

  // DxCore 1.6.2 ADC-reference workaround
  VREF.ADC0REF =
      (VREF.ADC0REF & ~VREF_REFSEL_gm) |
      VREF_REFSEL_VDD_gc;

  (void)analogRead(PIN_PD0);
  (void)analogRead(PIN_PD1);
  (void)analogRead(PIN_PD2);
  (void)analogRead(PIN_PD3);
  (void)analogRead(PIN_PD4);
  (void)analogRead(PIN_PD5);
  (void)analogRead(PIN_PD7);
  (void)analogRead(PIN_PF0);
  (void)analogRead(PIN_PF1);

  // Enable DAC
  DACReference(VDD);
  analogWrite(PIN_PD6, 128);
  DAC0.DATA = 0;

  // PWM reference output
  setupPWM();

  // Initialise waveform buffers
  buildWaveform(0, 128);
  buildWaveform(0, 128);

  phaseIncrement =
      (uint32_t)(HIGH_BASE *
                 4294967296.0 / SAMPLE_RATE);

  // 40 kHz audio interrupt
  TCB0.CTRLA = 0;
  TCB0.CTRLB = TCB_CNTMODE_INT_gc;

  TCB0.CCMP = (F_CPU / SAMPLE_RATE) - 1;

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
  static uint8_t oldWaveform = 255;
  static uint8_t oldShape = 255;

  static bool pitchInitialised = false;
  static uint32_t smoothPitchQ8 = 0;
  static uint32_t smoothVolumeQ8 = 0;

  static bool wasLowNoise = false;
  static uint32_t nextDriftMs = 0;

  // ---- Read analogue controls ----

  uint16_t pitchADC = analogRead(PIN_PD0)
                  + analogRead(PIN_PD1)
                  + analogRead(PIN_PD2);

  uint16_t volumeADC =
      analogRead(PIN_PD3)
      + analogRead(PIN_PD4)
      + analogRead(PIN_PD5);
      
  uint16_t shapeADC = analogRead(PIN_PD7)
      + analogRead(PIN_PF0)
      + analogRead(PIN_PF1);

  // ---- Waveform selection ----

  uint8_t selected =
      (!digitalRead(PIN_PA5) ? 0 : 1) |
      (!digitalRead(PIN_PA6) ? 0 : 2);

  uint8_t shape = shapeADC >> 4;

  bool lowRange = !digitalRead(PIN_PA4);
  bool drifting = (selected == 3 && lowRange);

  if (selected != oldWaveform ||
      ((selected == 1 || selected == 2) &&
       shape != oldShape))
  {
    buildWaveform(selected, shape);

    oldWaveform = selected;
    oldShape = shape;
  }


  float voltage = pitchADC * (5.0f / 4095.0f);
/*
  // ---- Pitch smoothing ----

  uint32_t pitchTarget =
      (uint32_t)pitchADC << 8;

  if (!pitchInitialised)
  {
    smoothPitchQ8 = pitchTarget;
    pitchInitialised = true;
  }
  else
  {
    smoothPitchQ8 +=
      ((int32_t)pitchTarget -
       (int32_t)smoothPitchQ8) / 8;
  }

  float voltage =
      (smoothPitchQ8 / 256.0f) *
      (5.0f / 4095.0f);
*/
  float baseFrequency =
      lowRange ? LOW_BASE : HIGH_BASE;

  float frequency =
      baseFrequency *
      powf(2.0f, voltage / VOLTS_PER_OCTAVE);

  uint32_t increment =
      (uint32_t)(frequency *
                 4294967296.0 / SAMPLE_RATE);

  // ---- Volume smoothing ----

  uint16_t targetGain =
      (volumeADC + 8) >> 4;

  uint32_t targetQ8 =
      (uint32_t)targetGain << 8;

  smoothVolumeQ8 +=
      ((int32_t)targetQ8 -
       (int32_t)smoothVolumeQ8) / 4;

  uint16_t newGain = smoothVolumeQ8 >> 8;

  // ---- Atomic audio-parameter update ----

  noInterrupts();

  phaseIncrement = increment;
  volumeGain = newGain;
  waveform = selected;
  lowNoiseMode = drifting;

  interrupts();

  // ---- PWM REFERENCE UPDATE ----

  // Uses exactly the same smoothed volume
  // setting as the DAC output.
  updatePWM(newGain);

  // ---- Low-frequency random walk ----

  if (drifting)
  {
    uint32_t now = millis();

    if (!wasLowNoise)
    {
      noInterrupts();
      driftOffset = 0;
      interrupts();

      nextDriftMs = now;
    }

    if ((int32_t)(now - nextDriftMs) >= 0)
    {
      int16_t next;

      noInterrupts();
      next = driftOffset;
      interrupts();

      int8_t direction =
          random(2) ? 1 : -1;

      if (next >= DAC_AMP)
        direction = -1;
      else if (next <= -DAC_AMP)
        direction = 1;

      next += WALK_STEP * direction;

      if (next > DAC_AMP)
        next = DAC_AMP;

      if (next < -DAC_AMP)
        next = -DAC_AMP;

      noInterrupts();
      driftOffset = next;
      interrupts();

      uint32_t meanMs =
          (uint32_t)(
            1000.0f /
            (frequency * WALK_RATE_MULTIPLIER)
          );

      uint32_t interval =
          meanMs * (uint32_t)random(500, 1501)
          / 1000UL;

      if (interval < 1)
        interval = 1;

      nextDriftMs = now + interval;
    }
  }

  wasLowNoise = drifting;

  delay(5);
}
