/*
 * LATHEVISION - ESP32 -> WebSocket (IP local) -> painel web
 *
 * Sensores:
 *   - SCT-013 (corrente elétrica)   -> GPIO 32 (ADC)
 *   - MPU6050 (vibração)            -> I2C: SDA 21, SCL 22
 *   - Encoder (velocidade / RPM)    -> GPIO 25 (canal A)
 *   - HC-SR04 (nível de óleo)       -> TRIG 5, ECHO 18
 *
 * O ESP32 vira um servidor WebSocket na porta WS_PORTA. A cada 1 segundo
 * envia para todos os painéis conectados (ws://<IP do ESP32>:81):
 *   {"vibracao":0.350,"corrente":7.40,"rpm":1400,"oleo_cm":12.3}
 * O IP aparece no Monitor Serial depois que o Wi-Fi conecta.
 *
 * Bibliotecas: "WebSockets" de Markus Sattler (Links2004)
 *              (instale pelo Gerenciador de Bibliotecas).
 * Placa: ESP32 Dev Module (ESP32 DevKit V1).
 */

#include <WiFi.h>
#include <Wire.h>
#include <WebSocketsServer.h>

// ---------------- Rede ----------------
const char* WIFI_SSID  = "SUA_REDE";
const char* WIFI_SENHA = "SUA_SENHA";   // não envie o código com a senha real para o GitHub

// ---------------- WebSocket ----------------
const uint16_t WS_PORTA = 81;   // o painel conecta em ws://<IP do ESP32>:81

// (Opcional) IP fixo, para não precisar descobrir o IP toda vez.
// Deixe USAR_IP_FIXO = false para receber o IP do roteador (DHCP).
const bool USAR_IP_FIXO = false;
IPAddress IP_FIXO(192, 168, 0, 50);
IPAddress GATEWAY(192, 168, 0, 1);
IPAddress MASCARA(255, 255, 255, 0);

// ---------------- Pinos ----------------
const int PIN_TRIG = 5;    // HC-SR04 (óleo)
const int PIN_ECHO = 18;
const int PIN_ENC  = 25;   // encoder
const int PIN_CT   = 32;   // SCT-013 (ADC)
const int MPU_ADDR = 0x68; // MPU6050

// ---------------- Calibração ----------------
const int   PPR              = 20;    // pulsos do encoder por volta (confira na ficha do seu encoder)
const float AMPERES_POR_VOLT = 30.0;  // SCT-013-030 = 30 A : 1 V (ajuste ao seu modelo)
const float LSB_POR_G        = 16384.0; // MPU6050 na escala de ±2 g

// ---------------- Estado ----------------
volatile unsigned long pulsos = 0;
unsigned long pulsosAnteriores = 0;
unsigned long ultimoCalculo = 0;
unsigned long ultimoEnvio = 0;

WebSocketsServer ws(WS_PORTA);

void IRAM_ATTR contarPulso() {
  pulsos++;
}

// ================= Conexões =================
void conectarWiFi() {
  WiFi.mode(WIFI_STA);
  if (USAR_IP_FIXO) WiFi.config(IP_FIXO, GATEWAY, MASCARA, GATEWAY);
  WiFi.begin(WIFI_SSID, WIFI_SENHA);
  Serial.print("Conectando ao Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println(" OK");
  Serial.printf("Painel: use o endereço ws://%s:%u
", WiFi.localIP().toString().c_str(), WS_PORTA);
}

void eventoWebSocket(uint8_t num, WStype_t tipo, uint8_t* dados, size_t tam) {
  if (tipo == WStype_CONNECTED) {
    Serial.printf("[WS] Painel %u conectado (%s)
", num, ws.remoteIP(num).toString().c_str());
  } else if (tipo == WStype_DISCONNECTED) {
    Serial.printf("[WS] Painel %u desconectado
", num);
  }
}

// ================= Nível de óleo (HC-SR04) =================
float lerDistanciaCm() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);

  long duracao = pulseIn(PIN_ECHO, HIGH, 30000); // timeout de 30 ms (~5 m)
  if (duracao == 0) return -1;                    // sem eco
  return duracao * 0.0343 / 2.0;
}

// ================= Velocidade (encoder) =================
float lerRPM() {
  unsigned long agora = millis();
  unsigned long dt = agora - ultimoCalculo;

  noInterrupts();
  unsigned long atual = pulsos;
  interrupts();

  float rpm = 0;
  if (dt > 0) {
    rpm = ((atual - pulsosAnteriores) / (float)PPR) * (60000.0 / dt);
  }
  pulsosAnteriores = atual;
  ultimoCalculo = agora;
  return rpm;
}

// ================= Corrente (SCT-013) =================
// Mede o valor RMS da parte alternada do sinal (sem o nível de 1,65 V do divisor de tensão)
float lerCorrente() {
  const int N = 2000;
  double soma = 0, soma2 = 0;

  for (int i = 0; i < N; i++) {
    double mv = analogReadMilliVolts(PIN_CT);
    soma += mv;
    soma2 += mv * mv;
  }

  double media = soma / N;
  double variancia = soma2 / N - media * media;
  if (variancia < 0) variancia = 0;

  double vrms = sqrt(variancia) / 1000.0; // volts
  return vrms * AMPERES_POR_VOLT;
}

// ================= Vibração (MPU6050) =================
void iniciarMPU() {
  Wire.begin(21, 22);
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x6B);  // PWR_MGMT_1
  Wire.write(0);     // acorda o sensor
  Wire.endTransmission(true);
}

int16_t lerInt16() {
  uint8_t hi = Wire.read();
  uint8_t lo = Wire.read();
  return (int16_t)((hi << 8) | lo);
}

bool lerAceleracao(float &x, float &y, float &z) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B);  // ACCEL_XOUT_H
  if (Wire.endTransmission(false) != 0) return false;

  // Tipos explícitos: com "int" o compilador do ESP32 não sabe qual requestFrom usar (erro "ambiguous")
  Wire.requestFrom((uint8_t)MPU_ADDR, (uint8_t)6, true);
  if (Wire.available() < 6) return false;

  x = lerInt16() / LSB_POR_G;
  y = lerInt16() / LSB_POR_G;
  z = lerInt16() / LSB_POR_G;
  return true;
}

// RMS da variação da aceleração em ~200 ms, em g (tira a gravidade e a inclinação)
float lerVibracao() {
  const int N = 100;
  float ax[N], ay[N], az[N];
  int n = 0;

  for (int i = 0; i < N; i++) {
    float x, y, z;
    if (lerAceleracao(x, y, z)) {
      ax[n] = x; ay[n] = y; az[n] = z;
      n++;
    }
    delay(2);
  }
  if (n < 10) return -1;  // leitura inválida

  float mx = 0, my = 0, mz = 0;
  for (int i = 0; i < n; i++) { mx += ax[i]; my += ay[i]; mz += az[i]; }
  mx /= n; my /= n; mz /= n;

  double soma = 0;
  for (int i = 0; i < n; i++) {
    float dx = ax[i] - mx, dy = ay[i] - my, dz = az[i] - mz;
    soma += dx * dx + dy * dy + dz * dz;
  }
  return sqrt(soma / n);
}

// ================= Setup e loop =================
void setup() {
  Serial.begin(115200);

  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  pinMode(PIN_ENC, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC), contarPulso, RISING);

  analogSetPinAttenuation(PIN_CT, ADC_11db);  // faixa de 0 a ~3,1 V

  iniciarMPU();
  conectarWiFi();
  ws.begin();
  ws.onEvent(eventoWebSocket);

  ultimoCalculo = millis();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) conectarWiFi();
  ws.loop();

  if (millis() - ultimoEnvio >= 1000) {
    ultimoEnvio = millis();

    float rpm      = lerRPM();
    float corrente = lerCorrente();
    float vibracao = lerVibracao();
    float oleoCm   = lerDistanciaCm();

    char json[160];
    snprintf(json, sizeof(json),
             "{\"vibracao\":%.3f,\"corrente\":%.2f,\"rpm\":%.0f,\"oleo_cm\":%.1f}",
             vibracao, corrente, rpm, oleoCm);

    ws.broadcastTXT(json);   // envia para todos os painéis conectados
    Serial.println(json);
  }
}
