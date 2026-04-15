// =============================================================================
//  MONITOR DE CONSUMO ELÉCTRICO - ESP32 WROOM-32
//  Sensor: SCT-013-30A (salida en voltaje, con resistencia interna)
//  Bot de Telegram: UniversalTelegramBot
//  Gráficas: QuickChart.io (generadas como imagen via HTTP GET)
// =============================================================================
//
//  LIBRERÍAS NECESARIAS (instalar desde el gestor de librerías de Arduino IDE):
//    - UniversalTelegramBot  by Brian Lough  (v1.3.0 o superior)
//    - ArduinoJson           by Benoit Blanchon (v6.x)
//    - WiFiClientSecure      (incluida en el core ESP32)
//
//  CONEXIÓN HARDWARE:
//    SCT-013-30A  →  Divisor de voltaje  →  GPIO34 (ADC1_CH6, solo entrada)
//    Divisor de voltaje recomendado:
//      - R1 = 10kΩ entre 3.3V y el pin de señal del SCT
//      - R2 = 10kΩ entre el pin de señal del SCT y GND
//      - C1 = 10µF entre el nodo central y GND (filtro de CC para bias ~1.65V)
//    El jack 3.5mm del SCT: punta (tip) = señal, cuerpo (sleeve) = GND
//
//  CALIBRACIÓN:
//    El valor CALIBRACION_SENSOR puede ajustarse según tu instalación.
//    El SCT-013-30A (salida en voltaje) tiene una salida de 1V RMS a 30A.
//    Relación: 30A / 1V = 30.0 → valor base de calibración.
//    Ajusta OFFSET_CORRIENTE si el valor en reposo no es 0.
// =============================================================================

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <math.h>

// -----------------------------------------------------------------------------
//  CONFIGURACIÓN - MODIFICA ESTOS VALORES
// -----------------------------------------------------------------------------

const char* WIFI_SSID     = "TU_RED_WIFI";          // Nombre de tu red WiFi
const char* WIFI_PASSWORD = "TU_PASSWORD_WIFI";       // Contraseña de tu red WiFi

#define BOT_TOKEN   "TOKEN_DE_BOTFATHER"                    // Token de tu bot de Telegram
#define CHAT_ID     "TU_CHAT_ID"                      // Tu chat ID de Telegram

// Voltaje de la red eléctrica en México
const float VOLTAJE_RED = 127.0;                      // Voltios RMS

// Umbral de alerta (en Amperios)
// 0.2A  = solo refrigerador + modem (todo apagado)
// 1.6A  = los 10 focos prendidos
// 0.4A  = umbral de alerta configurado
const float UMBRAL_ALERTA_AMPERES = 0.4;

// Calibración del sensor SCT-013-30A
// El sensor genera 1V RMS a 30A → factor = 30.0
// Ajusta este valor si las lecturas no coinciden con un multímetro
const float CALIBRACION_SENSOR = 30.0;

// Offset de corriente (ajusta si el valor en reposo no es ~0.0)
const float OFFSET_CORRIENTE = -0.4;

// -----------------------------------------------------------------------------
//  PINES Y CONSTANTES ADC
// -----------------------------------------------------------------------------

#define PIN_SCT        34                             // GPIO34 - ADC1 canal 6
#define ADC_RESOLUCION 4096                           // 12 bits
#define ADC_VREF       3.3                            // Voltaje de referencia del ESP32
#define MUESTRAS_RMS   1000                           // Muestras para calcular RMS (a 60Hz, ~16 ciclos)
#define ADC_BIAS       2048                           // Punto medio teórico (½ de 4096)

// -----------------------------------------------------------------------------
//  ALMACENAMIENTO DE DATOS HISTÓRICOS
// -----------------------------------------------------------------------------

// Última hora: 1 muestra cada 5 segundos → 720 puntos
#define MUESTRAS_1H     720
#define INTERVALO_5S    5000UL                        // ms

// Últimos 5 minutos para gráfica de watts: 1 muestra cada 2 segundos → 150 puntos
#define MUESTRAS_5MIN   150
#define INTERVALO_2S    2000UL                        // ms

float historial_corriente[MUESTRAS_1H];              // Corriente (A) para gráfica 1h
float historial_watts[MUESTRAS_5MIN];                // Potencia (W) para gráfica 5min

int indice_1h   = 0;
int indice_5min = 0;
bool buffer_1h_lleno   = false;
bool buffer_5min_lleno = false;

unsigned long ultimo_muestreo_5s  = 0;
unsigned long ultimo_muestreo_2s  = 0;
unsigned long ultimo_chequeo_bot  = 0;
unsigned long ultima_alerta       = 0;

// Intervalo para revisar mensajes del bot (ms)
#define INTERVALO_BOT  2000UL

// Tiempo mínimo entre alertas repetidas (ms) - 5 minutos para no saturar
#define INTERVALO_ENTRE_ALERTAS 300000UL

// -----------------------------------------------------------------------------
//  OBJETOS GLOBALES
// -----------------------------------------------------------------------------

WiFiClientSecure clienteSeguro;
UniversalTelegramBot bot(BOT_TOKEN, clienteSeguro);

// Estado de alerta para no enviar repetidamente
bool alerta_activa = false;

// -----------------------------------------------------------------------------
//  FUNCIÓN: Medir corriente RMS con el SCT-013-30A
// -----------------------------------------------------------------------------
float medirCorrienteRMS() {
  long suma_cuadrados = 0;
  int muestra_raw     = 0;
  int bias_dinamico   = 0;
  long suma_bias      = 0;

  // Primera pasada: calcular bias real (promedio de muestras)
  for (int i = 0; i < MUESTRAS_RMS; i++) {
    suma_bias += analogRead(PIN_SCT);
    delayMicroseconds(100);
  }
  bias_dinamico = suma_bias / MUESTRAS_RMS;

  // Segunda pasada: calcular RMS con el bias real
  for (int i = 0; i < MUESTRAS_RMS; i++) {
    muestra_raw = analogRead(PIN_SCT) - bias_dinamico;
    suma_cuadrados += (long)muestra_raw * muestra_raw;
    delayMicroseconds(100);
  }

  // Voltaje RMS en el ADC
  float voltaje_rms_adc = sqrt((float)suma_cuadrados / MUESTRAS_RMS)
                          * (ADC_VREF / ADC_RESOLUCION);

  // Convertir voltaje ADC a corriente usando el factor de calibración
  // SCT-013-30A: 1V RMS → 30A → factor = 30.0
  float corriente_rms = (voltaje_rms_adc * CALIBRACION_SENSOR) + OFFSET_CORRIENTE;

  // Filtrar ruido: si la corriente es muy baja, considerarla 0
  if (corriente_rms < 0.05) corriente_rms = 0.0;

  return corriente_rms;
}

// -----------------------------------------------------------------------------
//  FUNCIÓN: Construir JSON para QuickChart (POST) - Corriente 1 hora
//  Se usa POST en lugar de GET para evitar el límite de ~2048 chars en la URL
// -----------------------------------------------------------------------------
String construirJSONGraficaCorriente() {
  // Cuántos puntos válidos tenemos en el buffer circular
  int n = buffer_1h_lleno ? MUESTRAS_1H : indice_1h;
  if (n == 0) return "";

  // Máximo 30 puntos para no saturar la RAM del ESP32 con el String
  int paso   = max(1, n / 30);
  int inicio = buffer_1h_lleno ? indice_1h : 0;   // índice más antiguo

  String etiquetas = "";
  String datos     = "";

  for (int i = 0; i < n; i += paso) {
    int idx           = (inicio + i) % MUESTRAS_1H;
    int seg_atras     = (n - 1 - i) * 5;           // cada muestra = 5 seg
    int min_atras     = seg_atras / 60;

    if (etiquetas.length() > 0) {
      etiquetas += ",";
      datos     += ",";
    }
    etiquetas += "\"-" + String(min_atras) + "m\"";
    datos     += String(historial_corriente[idx], 2);
  }

  // JSON compatible con Chart.js v2 (el que usa QuickChart)
  String json =
    "{\"chart\":{\"type\":\"line\","
    "\"data\":{\"labels\":[" + etiquetas + "],"
    "\"datasets\":[{\"label\":\"Corriente (A)\","
    "\"data\":[" + datos + "],"
    "\"borderColor\":\"rgb(255,99,132)\","
    "\"backgroundColor\":\"rgba(255,99,132,0.15)\","
    "\"fill\":true,\"tension\":0.3,\"pointRadius\":3}]},"
    "\"options\":{\"title\":{\"display\":true,\"text\":\"Consumo ultima hora\"},"
    "\"scales\":{\"yAxes\":[{\"ticks\":{\"beginAtZero\":true},"
    "\"scaleLabel\":{\"display\":true,\"labelString\":\"Amperes (A)\"}}]}}}}";

  return json;
}

// -----------------------------------------------------------------------------
//  FUNCIÓN: Construir JSON para QuickChart (POST) - Watts últimos 5 minutos
// -----------------------------------------------------------------------------
String construirJSONGraficaWatts() {
  int n = buffer_5min_lleno ? MUESTRAS_5MIN : indice_5min;
  if (n == 0) return "";

  int paso   = max(1, n / 30);
  int inicio = buffer_5min_lleno ? indice_5min : 0;

  String etiquetas = "";
  String datos     = "";

  for (int i = 0; i < n; i += paso) {
    int idx        = (inicio + i) % MUESTRAS_5MIN;
    int seg_atras  = (n - 1 - i) * 2;              // cada muestra = 2 seg

    if (etiquetas.length() > 0) {
      etiquetas += ",";
      datos     += ",";
    }
    etiquetas += "\"-" + String(seg_atras) + "s\"";
    datos     += String(historial_watts[idx], 1);
  }

  String json =
    "{\"chart\":{\"type\":\"line\","
    "\"data\":{\"labels\":[" + etiquetas + "],"
    "\"datasets\":[{\"label\":\"Potencia (W)\","
    "\"data\":[" + datos + "],"
    "\"borderColor\":\"rgb(54,162,235)\","
    "\"backgroundColor\":\"rgba(54,162,235,0.15)\","
    "\"fill\":true,\"tension\":0.3,\"pointRadius\":3}]},"
    "\"options\":{\"title\":{\"display\":true,\"text\":\"Potencia ultimos 5 minutos\"},"
    "\"scales\":{\"yAxes\":[{\"ticks\":{\"beginAtZero\":true},"
    "\"scaleLabel\":{\"display\":true,\"labelString\":\"Watts (W)\"}}]}}}}";

  return json;
}

// -----------------------------------------------------------------------------
//  FUNCIÓN: Descargar imagen de QuickChart via POST y enviarla a Telegram
//
//  Flujo:
//    1. HTTP POST a quickchart.io/chart  → recibe imagen PNG en binario
//    2. Guarda la imagen en un buffer en RAM
//    3. HTTP POST multipart a api.telegram.org/sendPhoto → entrega la imagen
//
//  Limitación de RAM: el ESP32 tiene ~300KB libres en heap.
//  Una imagen de QuickChart suele pesar entre 20-60 KB → entra sin problemas.
// -----------------------------------------------------------------------------
void enviarGrafica(String jsonChart, String caption) {
  if (jsonChart.length() == 0) {
    bot.sendMessage(CHAT_ID,
      "⚠️ No hay suficientes datos aún. Espera al menos 10 segundos.", "");
    return;
  }

  // ── PASO 1: Descargar PNG de QuickChart ────────────────────────────────────
  WiFiClientSecure clienteQC;
  clienteQC.setInsecure();                          // sin verificación TLS

  Serial.println("[Gráfica] Conectando a quickchart.io...");
  if (!clienteQC.connect("quickchart.io", 443)) {
    bot.sendMessage(CHAT_ID, "❌ Error al conectar con QuickChart.", "");
    Serial.println("[Gráfica] ERROR: no se pudo conectar a quickchart.io");
    return;
  }

  // Petición POST
  String peticion =
    "POST /chart HTTP/1.1\r\n"
    "Host: quickchart.io\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: " + String(jsonChart.length()) + "\r\n"
    "Connection: close\r\n\r\n"
    + jsonChart;

  clienteQC.print(peticion);
  Serial.println("[Gráfica] Petición POST enviada a QuickChart.");

  // Esperar respuesta (máx 10 segundos)
  unsigned long t0 = millis();
  while (!clienteQC.available() && millis() - t0 < 10000) delay(50);

  if (!clienteQC.available()) {
    bot.sendMessage(CHAT_ID, "❌ QuickChart no respondió a tiempo.", "");
    clienteQC.stop();
    return;
  }

  // Leer cabeceras HTTP para encontrar Content-Length y saltar al cuerpo
  int contentLength = 0;
  bool esImagen     = false;
  String linea      = "";

  while (clienteQC.connected()) {
    linea = clienteQC.readStringUntil('\n');
    linea.trim();

    if (linea.startsWith("Content-Length:")) {
      contentLength = linea.substring(16).toInt();
      Serial.println("[Gráfica] Content-Length: " + String(contentLength));
    }
    if (linea.startsWith("Content-Type: image")) {
      esImagen = true;
    }
    if (linea.length() == 0) break;                // fin de cabeceras (línea vacía)
  }

  if (!esImagen || contentLength == 0) {
    // Leer body como texto para depuración
    String cuerpo = clienteQC.readString();
    Serial.println("[Gráfica] Respuesta inesperada: " + cuerpo.substring(0, 200));
    bot.sendMessage(CHAT_ID, "❌ QuickChart devolvió una respuesta inválida.", "");
    clienteQC.stop();
    return;
  }

  // Reservar buffer para la imagen
  uint8_t* imgBuffer = (uint8_t*)malloc(contentLength);
  if (!imgBuffer) {
    bot.sendMessage(CHAT_ID, "❌ Sin memoria para descargar la imagen.", "");
    clienteQC.stop();
    return;
  }

  // Leer bytes de la imagen
  int leidos = 0;
  t0 = millis();
  while (leidos < contentLength && millis() - t0 < 15000) {
    if (clienteQC.available()) {
      imgBuffer[leidos++] = clienteQC.read();
    }
  }
  clienteQC.stop();
  Serial.println("[Gráfica] Imagen descargada: " + String(leidos) + " bytes.");

  if (leidos != contentLength) {
    bot.sendMessage(CHAT_ID, "❌ Imagen incompleta al descargar.", "");
    free(imgBuffer);
    return;
  }

  // ── PASO 2: Enviar imagen a Telegram via multipart/form-data ───────────────
  WiFiClientSecure clienteTG;
  clienteTG.setInsecure();

  Serial.println("[Gráfica] Conectando a api.telegram.org...");
  if (!clienteTG.connect("api.telegram.org", 443)) {
    bot.sendMessage(CHAT_ID, "❌ Error al conectar con Telegram.", "");
    free(imgBuffer);
    return;
  }

  // Construir partes del multipart
  String boundary    = "ESP32GraficaBoundary";
  String partInicio  =
    "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n"
    + String(CHAT_ID) + "\r\n"
    "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"caption\"\r\n\r\n"
    + caption + "\r\n"
    "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"photo\"; filename=\"grafica.png\"\r\n"
    "Content-Type: image/png\r\n\r\n";

  String partFin = "\r\n--" + boundary + "--\r\n";

  int totalBody = partInicio.length() + leidos + partFin.length();

  // Cabeceras HTTP
  clienteTG.println("POST /bot" + String(BOT_TOKEN) + "/sendPhoto HTTP/1.1");
  clienteTG.println("Host: api.telegram.org");
  clienteTG.println("Content-Type: multipart/form-data; boundary=" + boundary);
  clienteTG.println("Content-Length: " + String(totalBody));
  clienteTG.println("Connection: close");
  clienteTG.println();

  // Cuerpo: texto inicial + bytes de imagen + cierre
  clienteTG.print(partInicio);
  clienteTG.write(imgBuffer, leidos);
  clienteTG.print(partFin);

  free(imgBuffer);                                  // liberar RAM inmediatamente

  // Esperar confirmación de Telegram
  t0 = millis();
  while (!clienteTG.available() && millis() - t0 < 10000) delay(50);

  String respuestaTG = clienteTG.readString();
  clienteTG.stop();

  if (respuestaTG.indexOf("\"ok\":true") >= 0) {
    Serial.println("[Gráfica] ✅ Imagen enviada correctamente a Telegram.");
  } else {
    Serial.println("[Gráfica] ❌ Error Telegram: " + respuestaTG.substring(0, 300));
    bot.sendMessage(CHAT_ID, "❌ Error al enviar la imagen a Telegram.", "");
  }
}

// -----------------------------------------------------------------------------
//  FUNCIÓN: Procesar comandos recibidos del bot de Telegram
// -----------------------------------------------------------------------------
void procesarMensajesBot() {
  int n = bot.getUpdates(bot.last_message_received + 1);

  while (n > 0) {
    for (int i = 0; i < n; i++) {
      String texto    = bot.messages[i].text;
      String chat_id  = bot.messages[i].chat_id;

      Serial.println("Mensaje recibido: " + texto + " de chat_id: " + chat_id);

      // Solo responder al chat autorizado
      if (chat_id != String(CHAT_ID)) {
        bot.sendMessage(chat_id, "⛔ No estás autorizado para usar este bot.", "");
        continue;
      }

      // ---- COMANDO: /start ----
      if (texto == "/start") {
        String bienvenida =
          "⚡ *Monitor de Consumo Eléctrico*\n\n"
          "Comandos disponibles:\n"
          "/corriente\\_ahora — Lectura actual de corriente y potencia\n"
          "/grafica\\_hora    — Gráfica de corriente (última hora)\n"
          "/grafica\\_watts   — Gráfica de potencia en W (últimos 5 min)\n"
          "/estado           — Resumen del sistema\n"
          "/ayuda            — Mostrar este menú";
        bot.sendMessage(CHAT_ID, bienvenida, "Markdown");
      }

      // ---- COMANDO: /ayuda ----
      else if (texto == "/ayuda") {
        String ayuda =
          "📋 *Comandos del monitor:*\n\n"
          "🔌 /corriente\\_ahora → Medición instantánea\n"
          "📈 /grafica\\_hora    → Gráfica corriente 1h\n"
          "⚡ /grafica\\_watts   → Gráfica potencia 5min\n"
          "🖥️ /estado           → Info del sistema";
        bot.sendMessage(CHAT_ID, ayuda, "Markdown");
      }

      // ---- COMANDO: /corriente_ahora ----
      else if (texto == "/corriente_ahora") {
        float corriente = medirCorrienteRMS();
        float potencia  = corriente * VOLTAJE_RED;

        String nivel = "";
        if (corriente <= 0.25)      nivel = "🟢 Mínimo (solo refrigerador/modem)";
        else if (corriente <= 0.4)  nivel = "🟡 Bajo";
        else if (corriente <= 1.0)  nivel = "🟠 Moderado";
        else if (corriente <= 1.7)  nivel = "🔴 Alto (focos encendidos)";
        else                        nivel = "🚨 Muy alto";

        String respuesta =
          "⚡ *Medición actual:*\n\n"
          "🔌 Corriente: `" + String(corriente, 3) + " A`\n"
          "💡 Potencia:  `" + String(potencia, 1) + " W`\n"
          "🏠 Voltaje:   `" + String(VOLTAJE_RED, 0) + " V`\n"
          "📊 Nivel:      " + nivel;
        bot.sendMessage(CHAT_ID, respuesta, "Markdown");
      }

      // ---- COMANDO: /grafica_hora ----
      else if (texto == "/grafica_hora") {
        int pts1h = buffer_1h_lleno ? MUESTRAS_1H : indice_1h;
        if (pts1h < 2) {
          bot.sendMessage(CHAT_ID, "Aun no hay datos suficientes. Espera al menos 10 segundos.", "");
        } else {
          bot.sendMessage(CHAT_ID, "Generando grafica de la ultima hora...", "");
          String json1h = construirJSONGraficaCorriente();
          enviarGrafica(json1h, "Consumo de corriente - Ultima hora");
        }
      }

      // ---- COMANDO: /grafica_watts ----
      else if (texto == "/grafica_watts") {
        int pts5m = buffer_5min_lleno ? MUESTRAS_5MIN : indice_5min;
        if (pts5m < 2) {
          bot.sendMessage(CHAT_ID, "Aun no hay datos suficientes. Espera al menos 4 segundos.", "");
        } else {
          bot.sendMessage(CHAT_ID, "Generando grafica de potencia (ultimos 5 min)...", "");
          String json5m = construirJSONGraficaWatts();
          enviarGrafica(json5m, "Potencia en Watts - Ultimos 5 minutos");
        }
      }

      // ---- COMANDO: /estado ----
      else if (texto == "/estado") {
        float corriente_actual = medirCorrienteRMS();
        float watts_actual     = corriente_actual * VOLTAJE_RED;
        int puntos_1h    = buffer_1h_lleno   ? MUESTRAS_1H   : indice_1h;
        int puntos_5min  = buffer_5min_lleno ? MUESTRAS_5MIN : indice_5min;

        // Tiempo en buffer 1h en minutos
        float minutos_1h = (puntos_1h * 5.0) / 60.0;

        String estado =
          "🖥️ *Estado del sistema:*\n\n"
          "📡 WiFi: Conectado ✅\n"
          "⚡ Corriente: `" + String(corriente_actual, 3) + " A`\n"
          "💡 Potencia:  `" + String(watts_actual, 1) + " W`\n"
          "📦 Datos 1h:    " + String(puntos_1h) + " muestras (~" + String(minutos_1h, 0) + " min)\n"
          "📦 Datos 5min:  " + String(puntos_5min) + " muestras\n"
          "🚨 Umbral alerta: `" + String(UMBRAL_ALERTA_AMPERES) + " A`\n"
          "⏱️ Uptime: " + String(millis() / 60000) + " minutos";
        bot.sendMessage(CHAT_ID, estado, "Markdown");
      }

      // ---- COMANDO DESCONOCIDO ----
      else {
        bot.sendMessage(CHAT_ID,
          "❓ Comando no reconocido. Escribe /ayuda para ver los comandos disponibles.", "");
      }
    }
    n = bot.getUpdates(bot.last_message_received + 1);
  }
}

// -----------------------------------------------------------------------------
//  FUNCIÓN: Verificar umbral y enviar alerta automática
// -----------------------------------------------------------------------------
void verificarAlerta(float corriente) {
  unsigned long ahora = millis();

  if (corriente > UMBRAL_ALERTA_AMPERES) {
    // Si la alerta no estaba activa O ya pasaron 5 minutos desde la última
    if (!alerta_activa || (ahora - ultima_alerta > INTERVALO_ENTRE_ALERTAS)) {
      float potencia = corriente * VOLTAJE_RED;
      String alerta =
        "🚨 *ALERTA DE CONSUMO ELEVADO*\n\n"
        "Se detectó consumo superior al umbral configurado:\n\n"
        "🔌 Corriente: `" + String(corriente, 3) + " A`\n"
        "💡 Potencia:  `" + String(potencia, 1) + " W`\n"
        "⚠️ Umbral:    `" + String(UMBRAL_ALERTA_AMPERES) + " A`\n\n"
        "Usa /corriente\\_ahora para ver el estado actual.";
      bot.sendMessage(CHAT_ID, alerta, "Markdown");
      alerta_activa = true;
      ultima_alerta = ahora;
      Serial.println("ALERTA enviada: " + String(corriente, 3) + " A");
    }
  } else {
    // Si la corriente bajó del umbral y la alerta estaba activa → notificar normalización
    if (alerta_activa) {
      float potencia = corriente * VOLTAJE_RED;
      String normalizado =
        "✅ *Consumo normalizado*\n\n"
        "La corriente volvió a niveles normales:\n"
        "🔌 Corriente: `" + String(corriente, 3) + " A`\n"
        "💡 Potencia:  `" + String(potencia, 1) + " W`";
      bot.sendMessage(CHAT_ID, normalizado, "Markdown");
      alerta_activa = false;
      Serial.println("Consumo normalizado: " + String(corriente, 3) + " A");
    }
  }
}

// -----------------------------------------------------------------------------
//  SETUP
// -----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== Monitor de Consumo Eléctrico ===");

  // Configurar ADC
  analogReadResolution(12);                           // 12 bits = 0 a 4095
  analogSetAttenuation(ADC_11db);                    // Rango 0 a 3.3V
  pinMode(PIN_SCT, INPUT);

  // Inicializar buffers a 0
  memset(historial_corriente, 0, sizeof(historial_corriente));
  memset(historial_watts,     0, sizeof(historial_watts));

  // Conectar a WiFi
  Serial.print("Conectando a WiFi: ");
  Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int intentos = 0;
  while (WiFi.status() != WL_CONNECTED && intentos < 30) {
    delay(500);
    Serial.print(".");
    intentos++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ WiFi conectado!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\n❌ No se pudo conectar al WiFi. Reiniciando...");
    ESP.restart();
  }

  // Configurar cliente HTTPS (sin verificación de certificado para simplificar)
  clienteSeguro.setInsecure();

  // Notificación de inicio al bot
  bot.sendMessage(CHAT_ID,
    "🟢 *Monitor de consumo iniciado*\n\n"
    "El sistema está monitoreando el consumo eléctrico.\n"
    "Escribe /ayuda para ver los comandos disponibles.", "Markdown");

  Serial.println("Bot de Telegram conectado. Sistema listo.");

  // Inicializar tiempos
  ultimo_muestreo_5s = millis();
  ultimo_muestreo_2s = millis();
  ultimo_chequeo_bot = millis();
}

// -----------------------------------------------------------------------------
//  LOOP PRINCIPAL
// -----------------------------------------------------------------------------
void loop() {
  unsigned long ahora = millis();

  // ---- 1. Revisar mensajes del bot cada 2 segundos ----
  if (ahora - ultimo_chequeo_bot >= INTERVALO_BOT) {
    if (WiFi.status() == WL_CONNECTED) {
      procesarMensajesBot();
    } else {
      Serial.println("WiFi desconectado. Reconectando...");
      WiFi.reconnect();
    }
    ultimo_chequeo_bot = ahora;
  }

  // ---- 2. Muestreo cada 2 segundos → historial de watts (5 min) ----
  if (ahora - ultimo_muestreo_2s >= INTERVALO_2S) {
    float corriente = medirCorrienteRMS();
    float watts     = corriente * VOLTAJE_RED;

    // Guardar en buffer circular de 5 min
    historial_watts[indice_5min] = watts;
    indice_5min++;
    if (indice_5min >= MUESTRAS_5MIN) {
      indice_5min = 0;
      buffer_5min_lleno = true;          // se activa ANTES de resetear el índice
    }

    // Verificar alerta de consumo
    verificarAlerta(corriente);

    Serial.printf("Muestra 2s: %.3f A → %.1f W\n", corriente, watts);
    ultimo_muestreo_2s = ahora;
  }

  // ---- 3. Muestreo cada 5 segundos → historial de corriente (1 hora) ----
  if (ahora - ultimo_muestreo_5s >= INTERVALO_5S) {
    float corriente = medirCorrienteRMS();

    // Guardar en buffer circular de 1 hora
    historial_corriente[indice_1h] = corriente;
    indice_1h++;
    if (indice_1h >= MUESTRAS_1H) {
      indice_1h = 0;
      buffer_1h_lleno = true;            // se activa ANTES de resetear el índice
    }

    Serial.printf("Muestra 5s: %.3f A (guardada en historial 1h)\n", corriente);
    ultimo_muestreo_5s = ahora;
  }

  // Pequeña pausa para no saturar el CPU y dar tiempo al watchdog
  delay(10);
}
