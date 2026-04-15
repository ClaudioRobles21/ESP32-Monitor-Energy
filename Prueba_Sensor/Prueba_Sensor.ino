// ===============================
// Graficar voltaje en GPIO34
// ESP32 - Arduino IDE
// ===============================

#define PIN_SENSOR 34

const float VREF = 3.3;    // Voltaje de referencia del ESP32
const int ADC_RES = 4095;  // Resolución ADC (12 bits)

void setup() {
  Serial.begin(115200);
NEW SKETCH

  analogReadResolution(12);
}

void loop() {
  int lecturaADC = analogRead(PIN_SENSOR);

  // Convertir a voltaje
  float voltaje = (lecturaADC * VREF) / ADC_RES;

  // Enviar al Serial Plotter
  Serial.println(voltaje);

  delay(0.03);  // Ajusta para cambiar velocidad de la gráfica
}