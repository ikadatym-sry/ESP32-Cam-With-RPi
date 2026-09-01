void setup() {
  Serial.begin(115200);
  delay(1000);
  if (psramFound()) {
    Serial.printf("PSRAM: FOUND\n");
    Serial.printf("Total PSRAM: %d bytes (%.2f MB)\n", 
                  ESP.getPsramSize(), 
                  ESP.getPsramSize() / (1024.0 * 1024.0));
    Serial.printf("Free PSRAM:  %d bytes\n", ESP.getFreePsram());
  } else {
    Serial.println("PSRAM: NOT FOUND (No external RAM or PSRAM disabled in IDE)");
  }
}
void loop() {}