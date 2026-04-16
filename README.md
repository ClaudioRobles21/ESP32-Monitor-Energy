# ⚡ ESP32 Home Energy Monitor + Telegram Bot

![Arduino](https://img.shields.io/badge/Arduino_IDE-00979D?style=for-the-badge&logo=arduino&logoColor=white)
![ESP32](https://img.shields.io/badge/ESP32--WROOM--32-E7352C?style=for-the-badge&logo=espressif&logoColor=white)
![Telegram](https://img.shields.io/badge/Telegram_Bot-26A5E4?style=for-the-badge&logo=telegram&logoColor=white)
![License](https://img.shields.io/badge/Licencia-MIT-green?style=for-the-badge)

Monitor de consumo eléctrico doméstico en tiempo real construido con un ESP32-WROOM-32 y un sensor de corriente SCT-013-30A. El sistema mide la corriente que circula por la instalación eléctrica de la casa, almacena el historial en memoria y se comunica mediante un bot de Telegram: envía alertas automáticas cuando el consumo supera un umbral definido y genera gráficas PNG bajo demanda.

---

## ¿Qué problema resuelve?

En una vivienda típica es difícil saber en tiempo real cuánta energía se está consumiendo sin instalar hardware costoso. Este proyecto ofrece una solución económica y no invasiva: el sensor SCT-013-30A se pinza alrededor del cable de fase sin cortar ningún cable, el ESP32 procesa la señal y el bot de Telegram actúa como interfaz desde cualquier smartphone, sin necesidad de una app adicional.

**Contexto de uso (valores de referencia):**
| Estado de la casa | Corriente medida |
|---|---|
| Solo refrigerador + módem | ~0.20 A |
| Umbral de alerta configurado | 0.40 A |
| 10 focos encendidos | ~1.60 A |

---

## ¿Cómo funciona internamente?

### Arquitectura general

```
Instalación eléctrica AC
        │
   SCT-013-30A          ← pinzado en el cable de fase
        │  señal AC 0–1V RMS
   Divisor de voltaje   ← eleva el bias a ~1.65V para el ADC
        │  señal 0.65V–2.65V
     GPIO34 (ADC)
        │
     ESP32-WROOM-32
        ├─ Medición RMS (1000 muestras, bias dinámico)
        ├─ Buffer circular 1h  (720 pts × 5s = corriente A)
        ├─ Buffer circular 5min (150 pts × 2s = potencia W)
        ├─ Verificación de umbral → alerta Telegram
        └─ WiFi / HTTPS
              ├─ POST JSON → quickchart.io → PNG binario → Telegram (sendPhoto multipart)
              └─ Polling bot cada 2 s → respuesta a comandos
```

### Flujo de generación de gráficas

```
ESP32
  │── POST JSON ──▶ quickchart.io  ──▶ PNG binario (20–60 KB)
  │◀──────────────────────────────────────────────────────────
  │── POST multipart/form-data ──▶ api.telegram.org
                                         │
                                    Imagen en el chat
```

> Se usa HTTP **POST** (no GET) a QuickChart para evitar el límite de ~2048 caracteres en URL y para no tener que codificar caracteres especiales del JSON.

### Decisiones técnicas clave

- **Bias dinámico:** en cada medición se calculan 1000 muestras para estimar el punto medio real del ADC, eliminando el offset de CC sin depender de valores fijos.
- **Buffers circulares:** los historiales de 1 hora y 5 minutos se almacenan en RAM con índices que rotan; el flag `buffer_X_lleno` se activa **antes** de resetear el índice para evitar un bug clásico de off-by-one.
- **Envío de imagen en dos pasos:** el ESP32 descarga el PNG de QuickChart como bytes binarios y luego lo sube a Telegram via `multipart/form-data`, porque `UniversalTelegramBot` no soporta envío de fotos desde URL externa directamente.
- **Cooldown de alertas:** 5 minutos mínimo entre notificaciones del mismo evento para no saturar el chat.

### Estructura de carpetas

```
esp32-energy-monitor/
├── monitor_energia_esp32_v2.ino   ← código principal (producción)
├── Prueba_Sensor.ino              ← sketch de verificación del sensor
├── Imagenes/
│   ├── Esquema_de_circuito_electrico.png
│   ├── Diagrama_de_salida_jack_3_5_mm.png
│   └── Esquema_sensor_SCT-013-30A.png
└── README.md
```

---

## Requisitos e instalación

### Hardware necesario

| Componente | Especificación |
|---|---|
| ESP32-WROOM-32 | Cualquier placa de desarrollo con este módulo |
| SCT-013-30A | Salida en **voltaje** (1 V RMS = 30 A). Plug 3.5 mm TRS |
| R1, R2 | 2 × resistencia 10 kΩ |
| C1, C2 | 2 × capacitor electrolítico 10 µF (filtro de alimentación) |
| C3 | 1 × capacitor electrolítico 10 µF (acoplamiento AC) |
| Protoboard + cables | - |
|Fuente de alimentacion de 5 V| — |

### Esquema de conexión

El divisor de voltaje crea un punto de bias de ~1.65 V para que la señal AC del sensor (que oscila en torno a 0 V) quede centrada dentro del rango 0–3.3 V del ADC del ESP32.

```
3.3V ──── R1 (10kΩ) ────┬──── C3 (10µF) ──── TIP  (+) jack 3.5mm
                         │                    RING (-) ── GND
                    nodo bias ~1.65V           SLEEVE   ── GND
                         │
                        R2 (10kΩ)
                         │
                        GND
                         │
                   C1, C2 (10µF) [filtro]
                         │
                        GND

nodo bias ──── GPIO34 (solo entrada, ADC1 canal 6)
```

> Ver imagen `Imagenes/Esquema_de_circuito_electrico.png` para el diagrama completo con colores.

**Conexión del jack 3.5 mm del SCT-013-30A:**

| Contacto | Conexión |
|---|---|
| TIP (punta) | Nodo bias (unión R1-R2) a través de C3 |
| RING (anillo) | GND |
| SLEEVE (manga) | GND (unir con RING) |

### Librerías (Arduino IDE)

Instalar desde **Herramientas → Administrar bibliotecas**:

| Librería | Autor | Versión |
|---|---|---|
| `UniversalTelegramBot` | Brian Lough | ≥ 1.3.0 |
| `ArduinoJson` | Benoit Blanchon | v6.x |
| `WiFiClientSecure` | — | incluida en el core ESP32 |

### Configurar la placa ESP32 en Arduino IDE

1. Ir a **Archivo → Preferencias** y agregar esta URL en "Gestor de URLs adicionales":
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
2. Ir a **Herramientas → Placa → Gestor de tarjetas**, buscar `esp32` e instalar el paquete de Espressif.
3. Seleccionar **ESP32 Dev Module** como placa.

---

## Cómo usarlo

### 1. Verificar el sensor primero

Antes de cargar el código principal, usa `Prueba_Sensor.ino` para confirmar que el circuito funciona correctamente:

1. Carga `Prueba_Sensor.ino` al ESP32.
2. Abre **Herramientas → Serial Plotter** a 115200 baudios.
3. Con el sensor sin cablear al cable de fase deberías ver una línea plana cerca de **1.65 V** (el bias).
4. Al pasar el cable de fase por el sensor deberías ver una onda senoidal oscilando alrededor de ese valor.

### 2. Configurar credenciales

Abre `monitor_energia_esp32_v2.ino` y edita estas líneas al inicio del archivo:

```cpp
const char* WIFI_SSID     = "TU_RED_WIFI";
const char* WIFI_PASSWORD = "TU_PASSWORD";

#define BOT_TOKEN   "TOKEN_DE_BOTFATHER"
#define CHAT_ID     "TU_CHAT_ID"
```

> **¿Cómo obtener el token y el chat_id?**
> - Crea un bot en Telegram hablando con [@BotFather](https://t.me/BotFather) y usa `/newbot`.
> - Para obtener tu `CHAT_ID`, habla con [@userinfobot](https://t.me/userinfobot).

### 3. Ajustar la calibración

```cpp
const float CALIBRACION_SENSOR = 30.0;  // 30A / 1V = factor base del SCT-013-30A
const float OFFSET_CORRIENTE   = 0.0;   // Ajustar si el valor en reposo no es ~0.0 A
```

Con **solo el refrigerador y el módem** conectados, el monitor serial debe mostrar ~0.20 A. Ajusta `OFFSET_CORRIENTE` o `CALIBRACION_SENSOR` hasta lograrlo.

### 4. Cargar y ejecutar

1. Conecta el ESP32 por USB.
2. Selecciona el puerto correcto en **Herramientas → Puerto**.
3. Carga el sketch. El monitor serial mostrará el proceso de conexión WiFi.
4. Al conectarse, el bot enviará el mensaje: `🟢 Monitor de consumo iniciado`.

### Comandos del bot

| Comando | Descripción |
|---|---|
| `/start` · `/ayuda` | Muestra el menú de comandos disponibles |
| `/corriente_ahora` | Medición instantánea: corriente (A), potencia (W) y nivel de consumo |
| `/grafica_hora` | Imagen PNG con la gráfica de corriente de la **última hora** |
| `/grafica_watts` | Imagen PNG con la gráfica de potencia (W) de los **últimos 5 minutos** |
| `/estado` | Resumen del sistema: WiFi, corriente actual, puntos en buffer y uptime |

---

## Diagrama de secuencia — flujo completo

```
Usuario (Telegram)          ESP32                     QuickChart.io
       │                      │                             │
       │── /grafica_hora ────▶│                             │
       │                      │── POST JSON ───────────────▶│
       │                      │◀── PNG binario (20-60 KB) ──│
       │                      │                             
       │                      │── POST multipart ──▶ api.telegram.org
       │◀── imagen PNG ────────────────────────────────────────────
```

---

## Resumen técnico del sistema

| Parámetro | Valor |
|---|---|
| Microcontrolador | ESP32-WROOM-32 (240 MHz, 520 KB RAM) |
| Pin ADC | GPIO34 — ADC1 canal 6 (solo entrada, 12 bits) |
| Sensor | SCT-013-30A, salida 1 V RMS a 30 A, burden interna ~62 Ω |
| Muestras por lectura RMS | 1000 (≈16 ciclos a 60 Hz) |
| Frecuencia de muestreo | Cada 2 s (watts) / cada 5 s (corriente) |
| Capacidad buffer 1h | 720 puntos (buffer circular en RAM) |
| Capacidad buffer 5min | 150 puntos (buffer circular en RAM) |
| Umbral de alerta | 0.40 A (configurable) |
| Cooldown entre alertas | 5 minutos |
| Voltaje de red | 127 V RMS / 60 Hz (México) |
| API de gráficas | QuickChart.io (Chart.js v2, HTTP POST) |
| Protocolo Telegram | HTTPS, polling cada 2 s, `multipart/form-data` para imágenes |

---

## FAQ — Dudas frecuentes

**¿El sensor es peligroso de instalar?**
No, el SCT-013-30A es no invasivo: simplemente se abre y se pinza alrededor del cable de fase sin necesidad de cortar nada ni tocar conductores con tensión. Siempre trabaja con el interruptor general apagado por precaución.

**¿Por qué usar GPIO34 y no otro pin?**
GPIO34 es un pin de solo entrada en el ESP32, ideal para ADC porque no tiene resistencias pull-up internas que puedan interferir con la medición. Pertenece al canal ADC1, que funciona correctamente incluso con WiFi activo (ADC2 no es confiable con WiFi).

**La lectura en reposo no es 0, ¿qué hago?**
Ajusta el valor de `OFFSET_CORRIENTE` en el código. Si con solo el refrigerador + módem lees 0.35 A en lugar de 0.20 A, pon `OFFSET_CORRIENTE = -0.15`.

**¿Por qué no llegan las gráficas?**
Las causas más comunes son: (1) memoria RAM insuficiente — el ESP32 descarga el PNG en RAM, asegúrate de no tener otras variables grandes; (2) QuickChart sin respuesta — verifica conexión WiFi; (3) menos de 2 muestras en el buffer — espera al menos 10 segundos desde el arranque.

**¿Puedo cambiar el umbral de alerta?**
Sí, modifica esta línea en el código:
```cpp
const float UMBRAL_ALERTA_AMPERES = 0.4;  // cambiar al valor que necesites
```

**¿Funciona en otros países con 220 V / 50 Hz?**
Sí, cambia `VOLTAJE_RED` al voltaje de tu país. Para 50 Hz es recomendable aumentar `MUESTRAS_RMS` a 1200 para capturar el mismo número de ciclos completos.

---
**¿Por qué el sensor siempre lee 0 A aunque haya carga conectada?**
La causa más común es que los dos conductores del cable (fase y neutro) están pasando juntos por el interior del sensor. Los campos magnéticos de ambos se cancelan entre sí. Asegúrate de que por el sensor pase **únicamente el cable de fase**, no el par completo.

**¿Por qué las lecturas son muy altas o muy bajas comparadas con un multímetro?**
El valor de calibración no coincide con tu instalación. Conecta un aparato de consumo conocido (por ejemplo, un calentador eléctrico con potencia indicada en la etiqueta), mide con el sistema y ajusta `CALIBRACION_SENSOR` hasta que el valor calculado coincida. La fórmula es: `factor = corriente_real_A / voltaje_rms_adc`.

**El ADC siempre devuelve el valor máximo (4095), ¿qué significa?**
Significa que el voltaje en GPIO34 está superando los 3.3 V, lo que satura el ADC y puede dañar el pin a largo plazo. Verifica que el divisor de voltaje esté bien conectado (R1 y R2 de 10 kΩ en su lugar) y que los capacitores de filtro (C1, C2) estén correctamente polarizados.

**El ESP32 no se conecta al WiFi, ¿qué reviso?**
Primero confirma que el SSID y la contraseña en el código no tengan espacios extra ni caracteres especiales mal escritos. Si las credenciales son correctas, verifica que el router esté transmitiendo en 2.4 GHz — el ESP32 no soporta redes de 5 GHz. También prueba acercar el ESP32 al router para descartar señal débil.

**Las alertas de Telegram no llegan aunque el consumo supera el umbral, ¿por qué?**
Revisa dos cosas: (1) que el `BOT_TOKEN` y el `CHAT_ID` en el código sean exactamente los que corresponden a tu bot y tu cuenta — un solo carácter incorrecto los invalida; (2) que el ESP32 tenga conexión a internet activa — envía `/estado` al bot y observa si responde. Si no responde ningún comando, el problema es de conectividad WiFi o DNS, no del umbral.

**Las lecturas tienen mucho ruido y varían mucho entre mediciones, ¿cómo lo reduzco?**
El ruido eléctrico suele deberse a capacitores mal colocados o ausentes. Verifica que C1 y C2 (10 µF cada uno) estén conectados entre el nodo de GND y la alimentación, y que C3 (acoplamiento AC) esté correctamente en serie entre el TIP del jack y el nodo bias. Como solución por software, puedes agregar un promedio móvil de 10 muestras consecutivas antes de usar el valor de corriente.

**ACLARACIÓN**
El potenciómetro integrado en el circuito es un componente opcional; su inclusión en este diseño responde a la necesidad de ajustar con precisión el punto de polarización (bias), dado que los valores fijos de las resistencias disponibles no permitían obtener la polarización adecuada para el correcto funcionamiento del circuito.

## Contribuir

1. Haz fork del repositorio.
2. Crea una rama: `git checkout -b feature/nueva-funcionalidad`
3. Haz commit de tus cambios: `git commit -m 'Agrega nueva funcionalidad'`
4. Haz push: `git push origin feature/nueva-funcionalidad`
5. Abre un Pull Request.

---

## Licencia

MIT © 2025 — Libre para uso personal y comercial con atribución.

---
