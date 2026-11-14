
#include <Arduino.h>
#include <math.h>

// ============================================================
// AVR32DA28 VCO
// DxCore, 24 MHz internal clock
//
// PD0  Pitch CV
// PD3  Volume CV
// PD7  Shape CV
// PD6  DAC output
//
// PA5  Waveform bit 0
// PA6  Waveform bit 1
// PA4  Low-frequency range (grounded = low)
//
// Waveforms:
// 0 Sine
// 1 Triangle / sawtooth
// 2 Square / pulse
// 3 White noise (high range)
//   Bounded random walk (low range)
// ============================================================

const uint8_t EXTRA_VOLUME_PIN = PIN_PD4;

const uint32_t SAMPLE_RATE = 40000;

const float HIGH_BASE = 50.0f;
const float LOW_BASE = 0.05f;
const float VOLTS_PER_OCTAVE = 0.75f;

const int16_t DAC_MID = 512;
const int16_t DAC_AMP = 480;

uint16_t waveTable[2][256];

volatile uint8_t activeTable = 0;
volatile uint8_t waveform = 0;
volatile uint8_t lowNoiseMode = 0;

volatile uint16_t volumeGain = 0;
volatile int16_t driftOffset = 0;

volatile uint32_t phase = 0;
volatile uint32_t phaseIncrement = 0;

// White noise PRNG, used only in interrupt
uint32_t rng = 0xACE12345UL;

// ============================================================
// AUDIO INTERRUPT
// ============================================================

ISR(TCB0_INT_vect)
{
  phase += phaseIncrement;

  int32_t output;

  if (waveform == 3 && lowNoiseMode)
  {
    // Random walk is already limited by volume.
    // Preserve exact one-DAC-count steps.
    output = DAC_MID + driftOffset;
  }
  else
  {
    uint16_t sample;

    if (waveform == 3)
    {
      // 32-bit white noise
      rng ^= rng << 13;
      rng ^= rng >> 17;
      rng ^= rng << 5;

      sample = rng & 1023;
    }
    else
    {
      uint8_t index = phase >> 24;
      sample = waveTable[activeTable][index];
    }

    int16_t centred = (int16_t)sample - DAC_MID;

    int32_t scaled =
        (int32_t)centred * volumeGain;

    //output = DAC_MID + (scaled >> 8);
    output = DAC_MID + (scaled >> 8);

    if (output < 0)
      output = 0;
    
    if (output > 1023)
      output = 1023;
  }

  DAC0.DATA = output << 6;

  TCB0.INTFLAGS = TCB_CAPT_bm;
}

// ============================================================
// BUILD WAVEFORM
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
          (DAC_AMP * sinf(2.0f * PI * i / 256.0f));
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

        value = (i < duty) ? DAC_AMP : -DAC_AMP;
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
// SETUP
// ============================================================

void setup()
{
  pinMode(PIN_PA4, INPUT_PULLUP);
  pinMode(PIN_PA5, INPUT_PULLUP);
  pinMode(PIN_PA6, INPUT_PULLUP);

  pinMode(PIN_PD0, INPUT);
  pinMode(PIN_PD3, INPUT);
  pinMode(PIN_PD7, INPUT);
  pinMode(EXTRA_VOLUME_PIN, INPUT);

  analogReadResolution(12);
  analogReference(VDD);

  // DxCore 1.6.2 ADC reference workaround
  VREF.ADC0REF =
      (VREF.ADC0REF & ~VREF_REFSEL_gm) |
      VREF_REFSEL_VDD_gc;

  (void)analogRead(PIN_PD0);
  (void)analogRead(PIN_PD3);
  (void)analogRead(PIN_PD7);
  (void)analogRead(EXTRA_VOLUME_PIN);

  DACReference(VDD);
  analogWrite(PIN_PD6, 128);
  DAC0.DATA = DAC_MID << 6;

  buildWaveform(0, 128);
  buildWaveform(0, 128);

  phaseIncrement =
      (uint32_t)(HIGH_BASE *
                 4294967296.0 / SAMPLE_RATE);

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

  // Random-walk scheduling
  static bool wasLowNoise = false;
  static uint32_t nextDriftMs = 0;

  // ---- Read controls ----

  uint16_t pitchADC = analogRead(PIN_PD0);
  uint16_t volumeADC = analogRead(PIN_PD3);
  volumeADC += analogRead(EXTRA_VOLUME_PIN);
  uint16_t shapeADC = analogRead(PIN_PD7);

  uint8_t selected =
      (!digitalRead(PIN_PA5) ? 1 : 0) |
      (!digitalRead(PIN_PA6) ? 2 : 0);

  uint8_t shape = shapeADC >> 4;

  bool lowRange = !digitalRead(PIN_PA4);
  bool drifting = (selected == 3 && lowRange);

  // ---- Waveform table ----

  if (selected != oldWaveform ||
      ((selected == 1 || selected == 2) &&
       shape != oldShape))
  {
    buildWaveform(selected, shape);

    oldWaveform = selected;
    oldShape = shape;
  }

  // ---- Pitch smoothing ----

  uint32_t pitchTarget = (uint32_t)pitchADC << 8;

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

  float baseFrequency =
      lowRange ? LOW_BASE : HIGH_BASE;

  float frequency =
      baseFrequency *
      powf(2.0f, voltage / VOLTS_PER_OCTAVE);

  uint32_t increment =
      (uint32_t)(frequency *
                 4294967296.0 / SAMPLE_RATE);

  // ---- Volume smoothing ----

  uint16_t targetGain = (volumeADC + 8) >> 4;
  uint32_t targetQ8 = (uint32_t)targetGain << 8;

  smoothVolumeQ8 +=
      ((int32_t)targetQ8 -
       (int32_t)smoothVolumeQ8) / 4;

  uint16_t newGain = smoothVolumeQ8 >> 8;

  // Random-walk boundaries depend on volume.
  int16_t limit =
      (int32_t)DAC_AMP * newGain / 256;
  if (limit > 511)
    limit = 511;

  // ---- Update shared variables atomically ----

  noInterrupts();

  phaseIncrement = increment;
  volumeGain = newGain;
  waveform = selected;
  lowNoiseMode = drifting;

  // Keep existing walk inside new volume bounds
  if (driftOffset > limit)
    driftOffset = limit;

  if (driftOffset < -limit)
    driftOffset = -limit;

  interrupts();

  // ---- Low-frequency random walk ----

  if (drifting)
  {
    uint32_t now = millis();

    if (!wasLowNoise)
    {
      // Begin a new walk from the midpoint
      noInterrupts();
      driftOffset = 0;
      interrupts();

      nextDriftMs = now;
    }

    if ((int32_t)(now - nextDriftMs) >= 0)
    {
      // Choose random direction, reflecting
      // at boundaries.
      int16_t next;

      noInterrupts();

      next = driftOffset;

      if (limit > 0)
      {
        int8_t direction = random(2) ? 1 : -1;

        if (next >= limit)
          direction = -1;
        else if (next <= -limit)
          direction = 1;

        next += 20 * direction;

        // Keep the walk inside the volume limits
        if (next > limit)
          next = limit;
        if (next < -limit)
          next = -limit;
      }
      else
      {
        next = 0;
      }

      driftOffset = next;

      interrupts();

      // Mean interval = 1 / frequency.
      // Random multiplier: 0.5 to 1.5.
      uint32_t meanMs =
       (uint32_t)(100.0f / frequency);

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
