
#include <MIDI.h>

// MIDI on the hardware UART, pins D0/D1.
// USB Serial remains available for debugging.

MIDI_CREATE_INSTANCE(HardwareSerial, Serial1, MIDI);

void setup()
{
  Serial.begin(115200);

  MIDI.begin(MIDI_CHANNEL_OMNI);

  // We only want to listen, not forward MIDI.
  MIDI.turnThruOff();

  Serial.println("MIDI monitor ready");
}

void loop()
{
  if (MIDI.read())
  {
    byte channel = MIDI.getChannel();
    byte data1 = MIDI.getData1();
    byte data2 = MIDI.getData2();

    switch (MIDI.getType())
    {
      case midi::NoteOn:
        Serial.print("Note ON   ");
        Serial.print("Channel: ");
        Serial.print(channel);
        Serial.print("  Note: ");
        Serial.print(data1);
        Serial.print("  Velocity: ");
        Serial.println(data2);
        break;

      case midi::NoteOff:
        Serial.print("Note OFF  ");
        Serial.print("Channel: ");
        Serial.print(channel);
        Serial.print("  Note: ");
        Serial.println(data1);
        break;

      case midi::ControlChange:
        Serial.print("Control   ");
        Serial.print("Channel: ");
        Serial.print(channel);
        Serial.print("  CC: ");
        Serial.print(data1);
        Serial.print("  Value: ");
        Serial.println(data2);
        break;

      case midi::PitchBend:
      {
        int bend =
          ((int)data2 << 7) | data1;

        Serial.print("Pitch bend  ");
        Serial.print("Channel: ");
        Serial.print(channel);
        Serial.print("  Value: ");
        Serial.println(bend - 8192);
        break;
      }

      default:
        break;
    }
  }
}
