int analogInPin = A0;    
int sensorValue; 
float voltage;
int bat_percentage;
float calibration = 0.0;   // Adjust after calibration step

// Battery voltage range for percentage mapping
const float BATTERY_MIN_VOLTAGE = 3.0;
const float BATTERY_MAX_VOLTAGE = 4.2;

void setup() {
  Serial.begin(9600);
  delay(1500);
}

void loop() {
  sensorValue = analogRead(analogInPin);

  // Convert ADC reading to voltage considering voltage divider and calibration
  voltage = (((sensorValue) / 2)/100)*6 + calibration;

  // Map voltage to battery percentage
  bat_percentage = (int)mapfloat(voltage, BATTERY_MIN_VOLTAGE, BATTERY_MAX_VOLTAGE, 0, 100);
  bat_percentage = constrain(bat_percentage, 0, 100);

  // Print exact battery voltage and percentage
  Serial.print("ADC = ");
  Serial.print(sensorValue);
  Serial.print("\tVoltage = ");
  Serial.print(voltage, 3);
  Serial.print(" V\tBattery % = ");
  Serial.print(bat_percentage);
  Serial.println("%");

  delay(1000);
}

float mapfloat(float x, float in_min, float in_max, float out_min, float out_max) {
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}