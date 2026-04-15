# ESP32 Home Energy Monitor + Telegram Bot

![ESP32](https://img.shields.io/badge/ESP32--WROOM--32-blue)
![Sensor](https://img.shields.io/badge/SCT--013--30A-orange)
![Bot](https://img.shields.io/badge/Telegram_Bot-UniversalTelegramBot-purple)

Monitor de consumo eléctrico doméstico en tiempo real con ESP32. Mide corriente AC mediante un sensor SCT-013-30A, almacena historial en buffers circulares y envía alertas automáticas y gráficas PNG directamente a Telegram bajo demanda.

---

## Características

- **Medición RMS real** — 1000 muestras con bias dinámico en GPIO34
- **Historial 1 hora** — 720 muestras de corriente (A), 1 cada 5 seg
- **Historial 5 minutos** — 150 muestras de potencia (W = 127 V × I), 1 cada 2 seg
- **Alertas automáticas** — notificación al superar 0.4 A con cooldown de 5 min
- **Gráficas PNG en Telegram** — POST a QuickChart.io → descarga → envío multipart
- **Reconexión WiFi** automática ante desconexiones

---

## Comandos del bot

| Comando | Descripción |
|---|---|
| `/start` · `/ayuda` | Menú de comandos |
| `/corriente_ahora` | Corriente (A) y potencia (W) en tiempo real |
| `/grafica_hora` | Gráfica PNG — corriente última hora |
| `/grafica_watts` | Gráfica PNG — potencia últimos 5 minutos |
| `/estado` | WiFi, lecturas actuales, puntos en buffer, uptime |

---

## Hardware

| Componente | Detalle |
|---|---|
| ESP32-WROOM-32 | ADC en GPIO34 (ADC1 canal 6, solo entrada) |
| SCT-013-30A | Salida en voltaje: 1 V RMS = 30 A. Jack 3.5 mm |
| Divisor de voltaje | 2 × 10 kΩ + capacitor 10 µF (bias ~1.65 V) |
|Filtro de señal 	| 2 capacitores 10 µF|

```
SCT-013-30A tip  ──┬── R1 (10kΩ) ── 3.3V
                   ├── R2 (10kΩ) ── GND
                   ├── C1 (10µF) ── GND
                   └────────────── GPIO34
SCT-013-30A sleeve ── GND
```

---

## Librerías (Arduino IDE)

- `UniversalTelegramBot` ≥ 1.3.0 — Brian Lough
- `ArduinoJson` v6.x — Benoit Blanchon
- `WiFiClientSecure` — incluida en el core ESP32

---

## Configuración

Edita estas constantes al inicio del `.ino`:

```cpp
const char* WIFI_SSID     = "tu_red_wifi";
const char* WIFI_PASSWORD = "tu_password";
#define BOT_TOKEN           "token_de_botfather"
#define CHAT_ID             "tu_chat_id"
const float CALIBRACION_SENSOR = 30.0;  // ajustar con multímetro
```

> **Calibración:** con solo refrigerador + modem conectados debe leer ~0.20 A.
> Referencia: 0.2 A en reposo · 0.4 A umbral de alerta · 1.6 A con 10 focos.

---

## Flujo de gráficas

```
ESP32 ──POST JSON──▶ quickchart.io ──PNG binario──▶ ESP32
ESP32 ──POST multipart/form-data──▶ api.telegram.org ──▶ imagen en chat
```

---

## Especificaciones eléctricas

- Voltaje de red: **127 V RMS** (México)
- Frecuencia: **60 Hz**

---

## Licencia

MIT
