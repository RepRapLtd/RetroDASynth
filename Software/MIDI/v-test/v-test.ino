
void setup()
{
  Serial.begin(115200);
  analogWriteResolution(12);
}

void loop()
{
  const int values[] = {512, 1024, 1536, 2048, 2560, 3072};

  for (int i = 0; i < 6; i++)
  {
    analogWrite(A0, values[i]);

    Serial.print("DAC value = ");
    Serial.println(values[i]);

    delay(5000);
  }
}
