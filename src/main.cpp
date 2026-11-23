#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsServer.h>
#include <DHT.h>
#include <driver/i2s.h>
#include <math.h>

// ================== CONFIGURAÇÕES DE WI-FI ==================
const char* WIFI_SSID = "REDE";
const char* WIFI_PASS = "SENHA";

// ================== PINAGEM DO ESP32 ==================
#define DHT_PIN      4
#define DHT_TYPE     DHT11
#define MQ2_PIN      34   // Usar ADC1 (32-39), pois ADC2 conflita com o Wi-Fi
#define BUZZER_PIN   13

// Pinos do microfone I2S INMP441 (L/R ligado ao GND)
#define I2S_WS       25
#define I2S_SD       33
#define I2S_SCK      26
#define I2S_PORT     I2S_NUM_0

// ================== LIMITES DE ALERTA ==================
const float TEMP_THRESHOLD  = 35.0;  // em °C
const int   GAS_THRESHOLD   = 1500;  // ADC de 0 a 4095
const float SOUND_THRESHOLD = 80.0;  // nível sonoro experimental

// ================== OBJETOS E VARIÁVEIS ==================
DHT dht(DHT_PIN, DHT_TYPE);
WebSocketsServer webSocket = WebSocketsServer(81);

unsigned long lastSend = 0;
const unsigned long SEND_INTERVAL = 1000; // Envia a cada 1 segundo

// ================== CONFIGURAÇÃO DO INMP441 (I2S) ==================
void setupI2S() {
  const i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = 16000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 4,
    .dma_buf_len = 256,
    .use_apll = false
  };

  const i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_SD
  };

  i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_PORT, &pin_config);
}

// Calcula uma estimativa experimental de nível sonoro a partir do RMS
float readSoundLevel() {
  const int SAMPLES = 256;
  int32_t buffer[SAMPLES];
  size_t bytesRead = 0;

  i2s_read(I2S_PORT, &buffer, sizeof(buffer), &bytesRead, 50 / portTICK_PERIOD_MS);
  int count = bytesRead / sizeof(int32_t);
  if (count == 0) return 0.0;

  double sumSq = 0;
  for (int i = 0; i < count; i++) {
    // INMP441 entrega 24 bits úteis alinhados à esquerda em 32 bits
    int32_t sample = buffer[i] >> 8;
    sumSq += (double)sample * (double)sample;
  }

  double rms = sqrt(sumSq / count);
  if (rms < 1.0) rms = 1.0;

  // Escala logarítmica experimental,não calibrada
  float dbEstimate = 20.0 * log10(rms) - 30.0;
  return (dbEstimate < 0) ? 0.0 : dbEstimate;
}

void webSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  if (type == WStype_CONNECTED) {
    Serial.printf("Cliente [%u] conectado ao dashboard.\n", num);
  } else if (type == WStype_DISCONNECTED) {
    Serial.printf("Cliente [%u] desconectado.\n", num);
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  dht.begin();
  setupI2S();

  // Conexão Wi-Fi
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Conectando ao Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConectado!");
  Serial.print("IP do ESP32: ");
  Serial.println(WiFi.localIP());

  // Inicia servidor WebSocket na porta 81
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
}

void loop() {
  webSocket.loop();

  if (millis() - lastSend >= SEND_INTERVAL) {
    lastSend = millis();

    float temp = dht.readTemperature();
    float hum  = dht.readHumidity();
    int   gas  = analogRead(MQ2_PIN);
    float sound = readSoundLevel();

    // Evita enviar NaN (pq quebra o JSON no navegador) se o DHT11 falhar
    if (isnan(temp)) temp = 0.0;
    if (isnan(hum))  hum  = 0.0;

    // Logica de acionamento do buzzer
    bool alert = (temp >= TEMP_THRESHOLD) ||
                 (gas >= GAS_THRESHOLD) ||
                 (sound >= SOUND_THRESHOLD);

    digitalWrite(BUZZER_PIN, alert ? HIGH : LOW);

    // Monta o JSON
    char json[160];
    snprintf(json, sizeof(json),
             "{\"temp\":%.1f,\"hum\":%.0f,\"gas\":%d,\"sound\":%.1f,\"buzzer\":%s}",
             temp, hum, gas, sound, alert ? "true" : "false");

    webSocket.broadcastTXT(json);
    Serial.println(json);
  }
}
