
#include <Arduino.h>

// ============================================================
// AVR32DA28 ENVELOPE GENERATOR
// DxCore, 24 MHz internal clock
//
// PD0  Attack time
// PD3  Decay time
// PD7  Level
// PF0  Level
// PD6  Envelope output (10-bit DAC)
//
// PA0  Gate input: grounded = ON
// PA4  Grounded = one-shot; open = gate-controlled
// PA5  Envelope selection bit 0
// PA6  Envelope selection bit 1
//
// Shapes:
// 0 Piano
// 1 Trapezoid
// 2 Exponential-like
// 3 Percussion
//
// Envelope updated every millisecond.
// ============================================================

const uint16_t MIN_ATTACK = 2;
const uint16_t MAX_ATTACK = 3000;
const uint16_t MIN_DECAY  = 20;
const uint16_t MAX_DECAY  = 8000;

const uint16_t FULL = 1024;

enum Stage {
  IDLE,
  ATTACK,
  BODY,
  SUSTAIN,
  RELEASE
};

Stage stage = IDLE;

uint32_t stageStart = 0;

uint16_t attackMs = 100;
uint16_t decayMs = 1000;
uint16_t level = 1023;

uint16_t envelope = 0;
uint16_t releaseStart = 0;

uint8_t shape = 0;
bool oneShot = false;
bool previousGate = false;

// ============================================================
// HELPERS
// ============================================================

// Convert an ADC reading to a time in milliseconds.
// Quadratic response provides better control over
// short times.

uint16_t timeFromADC(
    uint16_t adc,
    uint16_t minimum,
    uint16_t maximum)
{
  uint32_t v = adc >> 4;  // 0..255

  return minimum +
    (v * v * (maximum - minimum)) / 65025UL;
}

// Fraction of a stage completed, 0..1024

uint16_t progress(
    uint32_t elapsed,
    uint16_t duration)
{
  if (elapsed >= duration)
    return FULL;

  return (elapsed * 1024UL) / duration;
}

// Quadratic and cubic decay factors

uint16_t squareFactor(uint16_t x)
{
  return ((uint32_t)x * x) >> 10;
}

uint16_t cubeFactor(uint16_t x)
{
  return squareFactor(x) * (uint32_t)x >> 10;
}

// ============================================================
// BODY SHAPE
// ============================================================

uint16_t bodyValue(uint16_t p)
{
  uint16_t r = FULL - p;

  if (oneShot)
  {
    switch (shape)
    {
      case 0:  // Piano: rapid fall, then long tail
        if (p < 205)
          return FULL - (570UL * p / 205);

        return 454UL *
          squareFactor(
            (uint32_t)(FULL - p) * FULL / 819
          ) / FULL;

      case 1:  // Trapezoid: hold then linear fall
        if (p < 341)
          return FULL;

        return (uint32_t)(FULL - p) *
          FULL / 683;

      case 2:  // Smooth exponential-like decay
        return cubeFactor(r);

      default: // Percussion: rapid falling curve
        return cubeFactor(r);
    }
  }

  // Gate-controlled: decay to a sustain level.

  uint16_t sustain;

  switch (shape)
  {
    case 0: sustain = 256; break;   // Piano
    case 1: sustain = FULL; break;  // Trapezoid
    case 2: sustain = 400; break;   // Exponential
    default: sustain = 100; break;  // Percussion
  }

  uint16_t factor =
      (shape == 0 || shape == 2 || shape == 3)
      ? cubeFactor(r)
      : r;

  return sustain +
    (uint32_t)(FULL - sustain) * factor / FULL;
}

// ============================================================
// RELEASE SHAPE
// ============================================================

uint16_t releaseValue(uint16_t p)
{
  uint16_t r = FULL - p;

  switch (shape)
  {
    case 1:
      // Trapezoid: linear release
      return (uint32_t)releaseStart * r / FULL;

    default:
      // Other envelopes: curved release
      return (uint32_t)releaseStart *
        squareFactor(r) / FULL;
  }
}

// ============================================================
// START ENVELOPE
// ============================================================

void startEnvelope(uint32_t now)
{
  // Latch control settings for this note.
  attackMs = timeFromADC(
      analogRead(PIN_PD0),
      MIN_ATTACK, MAX_ATTACK);

  decayMs = timeFromADC(
      analogRead(PIN_PD3),
      MIN_DECAY, MAX_DECAY);

  level = (analogRead(PIN_PD7) + analogRead(PIN_PF0)) >> 2;

  shape =
      (!digitalRead(PIN_PA5) ? 1 : 0) |
      (!digitalRead(PIN_PA6) ? 2 : 0);

  oneShot = !digitalRead(PIN_PA4);

  stage = ATTACK;
  stageStart = now;
  envelope = 0;
}

// ============================================================
// UPDATE ENVELOPE
// ============================================================

void updateEnvelope(uint32_t now, bool gate)
{
  // Rising gate starts a new envelope.
  if (gate && !previousGate)
    startEnvelope(now);

  // Falling gate starts release, except in
  // one-shot mode.
  if (!gate && previousGate &&
      !oneShot && stage != IDLE)
  {
    releaseStart = envelope;
    stage = RELEASE;
    stageStart = now;
  }

  previousGate = gate;

  uint32_t elapsed = now - stageStart;

  switch (stage)
  {
    case IDLE:
      envelope = 0;
      break;

    case ATTACK:
    {
      uint16_t p = progress(elapsed, attackMs);

      // Exponential-like shape has a rounded attack.
      if (shape == 2)
        envelope = squareFactor(p);
      else
        envelope = p;

      if (p == FULL)
      {
        envelope = FULL;

        // Gate-controlled trapezoid holds at peak.
        if (shape == 1 && !oneShot)
          stage = SUSTAIN;
        else
          stage = BODY;

        stageStart = now;
      }
      break;
    }

    case BODY:
    {
      uint16_t p = progress(elapsed, decayMs);
      envelope = bodyValue(p);

      if (p == FULL)
      {
        if (oneShot)
        {
          stage = IDLE;
          envelope = 0;
        }
        else
        {
          stage = SUSTAIN;
        }
      }
      break;
    }

    case SUSTAIN:
      // Hold last envelope value until gate falls.
      break;

    case RELEASE:
    {
      uint16_t p = progress(elapsed, decayMs);

      envelope = releaseValue(p);

      if (p == FULL)
      {
        envelope = 0;
        stage = IDLE;
      }
      break;
    }
  }

  // Scale the envelope to the level control.
  uint16_t output =
      (uint32_t)envelope * level / FULL;

  // DAC is 10-bit; its data occupies bits 15:6.
  DAC0.DATA = output << 6;
}

// ============================================================
// SETUP
// ============================================================

void setup()
{
  pinMode(PIN_PA0, INPUT_PULLUP);
  pinMode(PIN_PA4, INPUT_PULLUP);
  pinMode(PIN_PA5, INPUT_PULLUP);
  pinMode(PIN_PA6, INPUT_PULLUP);

  pinMode(PIN_PD0, INPUT);
  pinMode(PIN_PD3, INPUT);
  pinMode(PIN_PD7, INPUT);
  pinMode(PIN_PF0, INPUT);

  analogReadResolution(12);
  analogReference(VDD);

  // DxCore 1.6.2 ADC-reference workaround
  VREF.ADC0REF =
    (VREF.ADC0REF & ~VREF_REFSEL_gm) |
    VREF_REFSEL_VDD_gc;

  (void)analogRead(PIN_PD0);
  (void)analogRead(PIN_PD3);
  (void)analogRead(PIN_PD7);
  (void)analogRead(PIN_PF0);

  DACReference(VDD);
  analogWrite(PIN_PD6, 128);

  // Start with envelope output at zero.
  DAC0.DATA = 0;
}

// ============================================================
// MAIN LOOP
// ============================================================

void loop()
{
  static uint32_t lastUpdate = 0;

  uint32_t now = millis();

  if (now != lastUpdate)
  {
    lastUpdate = now;

    bool gate = !digitalRead(PIN_PA0);
    updateEnvelope(now, gate);
  }
}
