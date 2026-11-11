/*
 * LATHEVISION - ESP32 -> servidor web (HTTP) no IP local -> painel
 *
 * Sensores:
 *   - SCT-013 (corrente elétrica)   -> GPIO 32 (ADC)
 *   - MPU6050 (vibração)            -> I2C: SDA 21, SCL 22
 *   - Display LCD 16x2 I2C          -> mesmo I2C (SDA 21, SCL 22), endereço 0x27 ou 0x3F
 *   - Encoder (velocidade / RPM)    -> GPIO 25 (canal A)
 *   - HC-SR04 (nível de óleo)       -> TRIG 5, ECHO 18
 *
 * O ESP32 vira um site simples na porta 80 (HTTP comum):
 *   http://<IP do ESP32>/        -> o próprio painel LatheVision (arquivo painel.h)
 *   http://<IP do ESP32>/dados   -> as leituras em JSON, atualizadas a cada 1 segundo:
 *     {"vibracao":0.350,"corrente":7.40,"rpm":1400,"oleo_cm":12.3,"rssi":-58}
 * O painel pede /dados a cada segundo. A rota /dados libera o acesso (CORS), então
 * o painel também funciona aberto como arquivo no computador (index.html).
 *
 * O IP aparece no display LCD e no Monitor Serial depois que o Wi-Fi conecta.
 *
 * Bibliotecas (Gerenciador de Bibliotecas da Arduino IDE):
 *   - "LiquidCrystal I2C" de Frank de Brabander
 *   (WiFi, WebServer e Wire já vêm com a placa ESP32)
 * Placa: ESP32 Dev Module (ESP32 DevKit V1).
 */

#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "painel.h"   // o painel (index.html) guardado dentro do ESP32

// ---------------- Rede ----------------
const char* WIFI_SSID  = "SUA_REDE";
const char* WIFI_SENHA = "SUA_SENHA";   // não envie o código com a senha real para o GitHub

// (Opcional) IP fixo, para o IP não mudar quando o roteador reiniciar.
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
const int PIN_SDA  = 21;   // I2C (MPU6050 + LCD)
const int PIN_SCL  = 22;
const int MPU_ADDR = 0x68; // MPU6050

// ---------------- Display LCD ----------------
const int LCD_COLUNAS = 16;
const int LCD_LINHAS  = 2;
// O endereço do LCD é descoberto sozinho (0x27 ou 0x3F, os mais comuns).

// ---------------- Calibração ----------------
const int   PPR              = 20;    // pulsos do encoder por volta (confira na ficha do seu encoder)
const float AMPERES_POR_VOLT = 30.0;  // SCT-013-030 = 30 A : 1 V (ajuste ao seu modelo)
const float LSB_POR_G        = 16384.0; // MPU6050 na escala de ±2 g

// ---------------- Estado ----------------
volatile unsigned long pulsos = 0;
unsigned long pulsosAnteriores = 0;
unsigned long ultimoCalculo = 0;
unsigned long ultimaLeitura = 0;
char jsonAtual[200] = "{}";   // última leitura, entregue em /dados

WebServer servidor(80);
LiquidCrystal_I2C* lcd = nullptr;   // fica nullptr se nenhum LCD for encontrado

void IRAM_ATTR contarPulso() {
  pulsos++;
}

// ================= Display LCD =================
bool existeNoI2C(uint8_t endereco) {
  Wire.beginTransmission(endereco);
  return Wire.endTransmission() == 0;
}

void iniciarLCD() {
  uint8_t endereco = 0;
  if (existeNoI2C(0x27)) endereco = 0x27;
  else if (existeNoI2C(0x3F)) endereco = 0x3F;

  if (endereco == 0) {
    Serial.println("[LCD] Nenhum display encontrado (0x27 / 0x3F). Seguindo sem LCD.");
    return;
  }
  lcd = new LiquidCrystal_I2C(endereco, LCD_COLUNAS, LCD_LINHAS);
  lcd->init();
  lcd->backlight();
  Serial.printf("[LCD] Display encontrado no endereço 0x%02X\n", endereco);
}

// Escreve duas linhas no LCD (corta o que passar de 16 caracteres)
void lcdMostrar(const String& linha1, const String& linha2) {
  if (!lcd) return;
  lcd->clear();
  lcd->setCursor(0, 0);
  lcd->print(linha1.substring(0, LCD_COLUNAS));
  lcd->setCursor(0, 1);
  lcd->print(linha2.substring(0, LCD_COLUNAS));
}

// ================= Wi-Fi =================
void conectarWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);   // responde mais rápido ao painel
  if (USAR_IP_FIXO) WiFi.config(IP_FIXO, GATEWAY, MASCARA, GATEWAY);
  WiFi.begin(WIFI_SSID, WIFI_SENHA);

  lcdMostrar("Conectando Wi-Fi", WIFI_SSID);
  Serial.print("Conectando ao Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  String ip = WiFi.localIP().toString();
  Serial.println(" OK");
  Serial.println("Painel: digite este IP no painel ou abra http://" + ip + "/ no navegador");
  lcdMostrar("IP do torno:", ip);
}

// ================= Servidor web =================
// Cabeçalhos que deixam o painel aberto como arquivo (ou de outro endereço) ler os dados
void liberarAcesso() {
  servidor.sendHeader("Access-Control-Allow-Origin", "*");
  servidor.sendHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
  servidor.sendHeader("Access-Control-Allow-Headers", "*");
  servidor.sendHeader("Access-Control-Allow-Private-Network", "true");
  servidor.sendHeader("Cache-Control", "no-store");
}

void rotaPainel() {
  // send_P lê direto da memória flash, sem copiar o painel inteiro para a RAM
  servidor.send_P(200, "text/html; charset=utf-8", PAINEL_HTML, sizeof(PAINEL_HTML) - 1);
}

void rotaDados() {
  liberarAcesso();
  servidor.send(200, "application/json", jsonAtual);
}

void rotaPreflight() {   // pedido de "permissão" que alguns navegadores fazem antes
  liberarAcesso();
  servidor.send(204);
}

void rotaNaoEncontrada() {
  liberarAcesso();
  servidor.send(404, "text/plain; charset=utf-8", "Nao encontrado. Use / (painel) ou /dados.");
}

void iniciarServidor() {
  servidor.on("/", HTTP_GET, rotaPainel);
  servidor.on("/dados", HTTP_GET, rotaDados);
  servidor.on("/dados", HTTP_OPTIONS, rotaPreflight);
  servidor.onNotFound(rotaNaoEncontrada);
  servidor.begin();
  Serial.println("[WEB] Servidor iniciado na porta 80");
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

// ================= Leituras =================
void atualizarLeituras() {
  float rpm      = lerRPM();
  float corrente = lerCorrente();
  float vibracao = lerVibracao();
  float oleoCm   = lerDistanciaCm();

  snprintf(jsonAtual, sizeof(jsonAtual),
           "{\"vibracao\":%.3f,\"corrente\":%.2f,\"rpm\":%.0f,\"oleo_cm\":%.1f,\"rssi\":%d}",
           vibracao, corrente, rpm, oleoCm, (int)WiFi.RSSI());
  Serial.println(jsonAtual);
}

// ================= Setup e loop =================
void setup() {
  Serial.begin(115200);

  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  pinMode(PIN_ENC, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC), contarPulso, RISING);

  analogSetPinAttenuation(PIN_CT, ADC_11db);  // faixa de 0 a ~3,1 V

  Wire.begin(PIN_SDA, PIN_SCL);
  iniciarLCD();
  iniciarMPU();
  conectarWiFi();
  iniciarServidor();

  ultimoCalculo = millis();
  ultimaLeitura = millis();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    lcdMostrar("Wi-Fi caiu", "Reconectando...");
    conectarWiFi();   // mostra o IP de novo no LCD quando voltar
  }

  servidor.handleClient();

  if (millis() - ultimaLeitura >= 1000) {
    ultimaLeitura = millis();
    atualizarLeituras();
  }
}
