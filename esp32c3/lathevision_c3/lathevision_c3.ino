/*
 * =====================================================================
 *  LATHEVISION - ESP32-C3 Mini (torno Romi - SENAI)
 *  Painel servido pelo próprio ESP32, em HTTP simples na porta 80.
 * =====================================================================
 *
 *  Endereços (troque IP pelo IP que aparece no LCD / Monitor Serial):
 *    http://IP/        -> painel LatheVision (fica guardado neste arquivo)
 *    ws://IP/ws        -> WebSocket: envia as leituras em JSON a cada 1 s
 *    http://IP/dados   -> as mesmas leituras em JSON (para consulta ou teste)
 *    http://IP/zerar   -> zera o contador de voltas do encoder
 *
 *  JSON enviado:
 *    {"corrente_a":7.42,"vibracao_g":0.318,"rpm":1398,"sentido":1,
 *     "distancia_cm":14.2,"oleo_pct":54.0,"voltas":1234,"rssi":-58,"uptime_s":3621}
 *    Valor -1 = sensor sem leitura (o painel mostra "--").
 *
 *  Sobre HTTPS: este ESP32 NUNCA usa https, NUNCA redireciona para https e
 *  NUNCA envia o cabeçalho Strict-Transport-Security. Se o Chrome mostrar
 *  "O site não pode fazer uma conexão segura", digite o endereço com
 *  http:// na frente (veja o passo a passo no README).
 *
 *  Ligações (ESP32-C3 Mini):
 *    SCT-013 (corrente) ........ GPIO 4 (ADC)
 *    MPU6050 (vibração) ........ SDA GPIO 6, SCL GPIO 5
 *    Display LCD 16x2 I2C ...... mesmo I2C (SDA 6, SCL 5), endereço 0x27 ou 0x3F
 *    HC-SR04 (nível de óleo) ... TRIG GPIO 10, ECHO GPIO 7 (ECHO com divisor de tensão!)
 *    Encoder (velocidade) ...... canal A GPIO 20, canal B GPIO 21
 *
 *  Bibliotecas (Arduino IDE > Ferramentas > Gerenciar Bibliotecas):
 *    - "ESP Async WebServer" de ESP32Async (instala junto a "Async TCP")
 *    - "LiquidCrystal I2C" de Frank de Brabander
 *  Placa: "ESP32C3 Dev Module", com "USB CDC On Boot: Enabled".
 * =====================================================================
 */

#include <WiFi.h>
#include <Wire.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <LiquidCrystal_I2C.h>

// =====================================================================
// CONFIGURAÇÃO - ajuste aqui
// =====================================================================

// ---------- Wi-Fi da oficina ----------
const char* WIFI_SSID  = "WIFI_IOT_CFP601";
const char* WIFI_SENHA = "SENHA_AQUI";      // não suba a senha real para o GitHub
const unsigned long WIFI_TEMPO_MAX_MS = 20000;  // tempo tentando antes de criar a rede própria

// ---------- Rede própria (ponto de acesso), usada se o Wi-Fi acima falhar ----------
// Conecte o celular/computador na rede "LATHEVISION" e abra http://192.168.4.1/
const char* AP_NOME  = "LATHEVISION";
const char* AP_SENHA = "lathevision";       // mínimo de 8 caracteres

// Algumas placas C3 Mini não conectam no Wi-Fi com a potência máxima.
// Se o Wi-Fi nunca conectar, troque para true.
const bool REDUZIR_POTENCIA_WIFI = false;

// ---------- Pinos ----------
const int PIN_CORRENTE  = 4;    // SCT-013 (ADC)
const int PIN_SDA       = 6;    // I2C: MPU6050 + LCD
const int PIN_SCL       = 5;
const int PIN_HC_TRIG   = 10;   // HC-SR04
const int PIN_HC_ECHO   = 7;
const int PIN_ENCODER_A = 20;   // encoder: conta os pulsos
const int PIN_ENCODER_B = 21;   // encoder: sentido de rotação

// ---------- Calibração (ajuste com medição real) ----------
const float SCT_FATOR_A_POR_V        = 30.0;  // SCT-013-030: 30 A = 1 V RMS
const int   ENCODER_PULSOS_POR_VOLTA = 1;     // pulsos do canal A por volta do eixo
const float TANQUE_DIST_CHEIO_CM     = 5.0;   // distância sensor -> óleo com tanque cheio
const float TANQUE_DIST_VAZIO_CM     = 25.0;  // distância sensor -> óleo com tanque vazio

// ---------- Envio ----------
const unsigned long INTERVALO_ENVIO_MS = 1000;  // uma leitura completa por segundo

// ---------- LCD ----------
const int LCD_COLUNAS = 16;
const int LCD_LINHAS  = 2;

// =====================================================================
// OBJETOS E ESTADO
// =====================================================================
AsyncWebServer servidor(80);       // HTTP comum, porta 80 (sem https)
AsyncWebSocket websocket("/ws");   // WebSocket no mesmo endereço: ws://IP/ws
LiquidCrystal_I2C* lcd = nullptr;  // fica nullptr se nenhum LCD for encontrado

extern const char PAINEL_HTML[];   // o painel fica no fim deste arquivo

String ipAtual = "";               // IP mostrado no LCD e no Monitor Serial
bool modoPontoDeAcesso = false;    // true = rede própria LATHEVISION (192.168.4.1)
bool wifiEstavaConectado = false;

// Última leitura em JSON. O servidor responde em outra tarefa, por isso o
// acesso a este texto é protegido (seção crítica) para não ler pela metade.
char jsonAtual[320] = "{}";
portMUX_TYPE travaJson = portMUX_INITIALIZER_UNLOCKED;

// Encoder
volatile long pulsosEncoder = 0;
unsigned long ultimaLeituraRpmMs = 0;
float voltasTotais = 0;
volatile bool pedidoZerar = false;   // marcado pela rota /zerar, executado no loop

// =====================================================================
// DISPLAY LCD
// =====================================================================
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

// =====================================================================
// WI-FI (tenta a rede da oficina; se falhar, cria a rede própria)
// =====================================================================
void anunciarIp() {
  Serial.println();
  Serial.println("IP: " + ipAtual);
  Serial.println("Abra no navegador: http://" + ipAtual + "/");
  if (modoPontoDeAcesso) lcdMostrar("Rede LATHEVISION", ipAtual);
  else                   lcdMostrar("IP do torno:", ipAtual);
}

bool conectarWiFi() {
  WiFi.mode(WIFI_STA);
  if (REDUZIR_POTENCIA_WIFI) WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.setSleep(false);          // responde mais rápido ao painel
  WiFi.setAutoReconnect(true);   // se cair, o próprio ESP32 tenta voltar
  WiFi.begin(WIFI_SSID, WIFI_SENHA);

  lcdMostrar("Conectando Wi-Fi", WIFI_SSID);
  Serial.printf("Conectando ao Wi-Fi %s", WIFI_SSID);

  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < WIFI_TEMPO_MAX_MS) {
    delay(500);
    Serial.print('.');
  }
  return WiFi.status() == WL_CONNECTED;
}

void iniciarPontoDeAcesso() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_NOME, AP_SENHA);
  modoPontoDeAcesso = true;
  ipAtual = WiFi.softAPIP().toString();   // normalmente 192.168.4.1
  Serial.printf("\nWi-Fi da oficina não respondeu. Rede própria criada: %s (senha: %s)\n",
                AP_NOME, AP_SENHA);
}

void iniciarRede() {
  if (conectarWiFi()) {
    ipAtual = WiFi.localIP().toString();
    wifiEstavaConectado = true;
    Serial.println(" OK");
  } else {
    iniciarPontoDeAcesso();
  }
  anunciarIp();
}

// Chamado no loop: avisa no LCD se o Wi-Fi cair e mostra o IP de novo quando voltar
void vigiarWiFi() {
  if (modoPontoDeAcesso) return;
  bool conectado = WiFi.status() == WL_CONNECTED;
  if (conectado == wifiEstavaConectado) return;

  wifiEstavaConectado = conectado;
  if (conectado) {
    ipAtual = WiFi.localIP().toString();   // o roteador pode ter dado outro IP
    anunciarIp();
  } else {
    Serial.println("Wi-Fi caiu. Reconectando...");
    lcdMostrar("Wi-Fi caiu", "Reconectando...");
  }
}

// =====================================================================
// SERVIDOR WEB E WEBSOCKET
// Nenhuma rota redireciona para https e nenhuma envia Strict-Transport-Security.
// =====================================================================

// Copia a última leitura com segurança (o servidor roda em outra tarefa)
String copiaJson() {
  char copia[sizeof(jsonAtual)];
  taskENTER_CRITICAL(&travaJson);
  memcpy(copia, jsonAtual, sizeof(copia));
  taskEXIT_CRITICAL(&travaJson);
  return String(copia);
}

// Cabeçalhos que deixam o painel aberto como arquivo (ou de outro endereço) ler os dados
void liberarAcesso(AsyncWebServerResponse* resposta) {
  resposta->addHeader("Access-Control-Allow-Origin", "*");
  resposta->addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  resposta->addHeader("Access-Control-Allow-Headers", "*");
  resposta->addHeader("Access-Control-Allow-Private-Network", "true");
  resposta->addHeader("Cache-Control", "no-store");
}

void responderJson(AsyncWebServerRequest* pedido, int codigo, const String& json) {
  AsyncWebServerResponse* resposta = pedido->beginResponse(codigo, "application/json", json);
  liberarAcesso(resposta);
  pedido->send(resposta);
}

// Eventos do WebSocket: ao conectar, já manda a última leitura
void eventoWebSocket(AsyncWebSocket* ws, AsyncWebSocketClient* cliente, AwsEventType tipo,
                     void* arg, uint8_t* dados, size_t tamanho) {
  if (tipo == WS_EVT_CONNECT) {
    Serial.printf("[WS] Painel %u conectado (%s)\n", cliente->id(),
                  cliente->remoteIP().toString().c_str());
    cliente->text(copiaJson());
  } else if (tipo == WS_EVT_DISCONNECT) {
    Serial.printf("[WS] Painel %u desconectado\n", cliente->id());
  }
}

void iniciarServidor() {
  websocket.onEvent(eventoWebSocket);
  servidor.addHandler(&websocket);

  // "/" -> o painel, lido direto da memória flash
  servidor.on("/", HTTP_GET, [](AsyncWebServerRequest* pedido) {
    pedido->send(200, "text/html; charset=utf-8",
                 (const uint8_t*)PAINEL_HTML, strlen(PAINEL_HTML));
  });

  // "/dados" -> última leitura em JSON
  servidor.on("/dados", HTTP_GET, [](AsyncWebServerRequest* pedido) {
    responderJson(pedido, 200, copiaJson());
  });

  // "/zerar" -> zera o contador de voltas do encoder (o loop executa o pedido)
  servidor.on("/zerar", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest* pedido) {
    pedidoZerar = true;
    responderJson(pedido, 200, "{\"ok\":true,\"zerado\":\"voltas\"}");
  });

  // Pedido de "permissão" que alguns navegadores fazem antes de ler os dados
  servidor.on("/dados", HTTP_OPTIONS, [](AsyncWebServerRequest* pedido) {
    AsyncWebServerResponse* resposta = pedido->beginResponse(204);
    liberarAcesso(resposta);
    pedido->send(resposta);
  });
  servidor.on("/zerar", HTTP_OPTIONS, [](AsyncWebServerRequest* pedido) {
    AsyncWebServerResponse* resposta = pedido->beginResponse(204);
    liberarAcesso(resposta);
    pedido->send(resposta);
  });

  servidor.onNotFound([](AsyncWebServerRequest* pedido) {
    AsyncWebServerResponse* resposta = pedido->beginResponse(
        404, "text/plain; charset=utf-8", "Nao encontrado. Use /, /dados, /zerar ou /ws.");
    liberarAcesso(resposta);
    pedido->send(resposta);
  });

  servidor.begin();
  Serial.println("[WEB] Servidor HTTP iniciado na porta 80 (WebSocket em /ws)");
}

// =====================================================================
// SENSORES
// =====================================================================

// ---------- Encoder (velocidade) ----------
// A cada borda de subida do canal A, o canal B diz o sentido de rotação.
void IRAM_ATTR isrEncoder() {
  if (digitalRead(PIN_ENCODER_B)) pulsosEncoder--;
  else pulsosEncoder++;
}

float lerRpm(int& sentido) {
  noInterrupts();               // lê e zera o contador sem a interrupção atrapalhar
  long pulsos = pulsosEncoder;
  pulsosEncoder = 0;
  interrupts();

  sentido = pulsos > 0 ? 1 : (pulsos < 0 ? -1 : 0);

  unsigned long agora = millis();
  float intervaloS = (agora - ultimaLeituraRpmMs) / 1000.0;
  ultimaLeituraRpmMs = agora;

  float voltas = labs(pulsos) / (float)ENCODER_PULSOS_POR_VOLTA;
  voltasTotais += voltas;
  if (intervaloS <= 0) return 0;
  return voltas * 60.0 / intervaloS;   // voltas por minuto
}

// ---------- MPU6050 (vibração) ----------
const uint8_t MPU_ADDR  = 0x68;
const float   LSB_POR_G = 16384.0;   // escala de ±2 g

void mpuEscreve(uint8_t registro, uint8_t valor) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(registro);
  Wire.write(valor);
  Wire.endTransmission();
}

bool mpuLeAceleracao(float& ax, float& ay, float& az) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B);   // ACCEL_XOUT_H: primeiro dos 6 bytes de aceleração
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(MPU_ADDR, (uint8_t)6) != 6) return false;

  int16_t brutos[3];
  for (int i = 0; i < 3; i++) {
    uint8_t alto  = Wire.read();
    uint8_t baixo = Wire.read();
    brutos[i] = (int16_t)((alto << 8) | baixo);
  }
  ax = brutos[0] / LSB_POR_G;
  ay = brutos[1] / LSB_POR_G;
  az = brutos[2] / LSB_POR_G;
  return true;
}

// Variância em torno da média: tira a gravidade e a inclinação do sensor
float varianciaSemMedia(const float* v, int n) {
  float media = 0;
  for (int i = 0; i < n; i++) media += v[i];
  media /= n;
  float soma = 0;
  for (int i = 0; i < n; i++) soma += (v[i] - media) * (v[i] - media);
  return soma / n;
}

// Vibração total em g (RMS), amostrando por ~200 ms
float lerVibracaoG() {
  const int N = 400;
  static float ax[N], ay[N], az[N];
  int n = 0;

  unsigned long inicio = millis();
  while (millis() - inicio < 200 && n < N) {
    float x, y, z;
    if (mpuLeAceleracao(x, y, z)) {
      ax[n] = x; ay[n] = y; az[n] = z;
      n++;
    }
    delayMicroseconds(500);
  }
  if (n < 20) return -1;   // sensor não respondeu

  return sqrt(varianciaSemMedia(ax, n) + varianciaSemMedia(ay, n) + varianciaSemMedia(az, n));
}

// ---------- SCT-013 (corrente) ----------
// Amostra ~200 ms (12 ciclos de 60 Hz) e calcula o RMS sem o nível de 1,65 V do bias
float lerCorrenteA() {
  static uint16_t amostras[2000];
  int n = 0;

  unsigned long inicioUs = micros();
  while (micros() - inicioUs < 200000UL && n < 2000) {
    amostras[n++] = analogReadMilliVolts(PIN_CORRENTE);
  }
  if (n == 0) return -1;

  double media = 0;
  for (int i = 0; i < n; i++) media += amostras[i];
  media /= n;

  double soma = 0;
  for (int i = 0; i < n; i++) soma += (amostras[i] - media) * (amostras[i] - media);

  double vrms = sqrt(soma / n) / 1000.0;   // mV -> V
  return vrms * SCT_FATOR_A_POR_V;
}

// ---------- HC-SR04 (nível de óleo) ----------
// 5 medições, descarta as sem eco e tira a média
float lerDistanciaCm() {
  float soma = 0;
  int validas = 0;

  for (int i = 0; i < 5; i++) {
    digitalWrite(PIN_HC_TRIG, LOW);
    delayMicroseconds(2);
    digitalWrite(PIN_HC_TRIG, HIGH);   // pulso de disparo de 10 µs
    delayMicroseconds(10);
    digitalWrite(PIN_HC_TRIG, LOW);

    unsigned long duracaoUs = pulseIn(PIN_HC_ECHO, HIGH, 30000UL);   // até ~5 m
    if (duracaoUs > 0) {
      soma += duracaoUs * 0.0343 / 2.0;   // ida e volta do som
      validas++;
    }
    delay(40);   // deixa o eco anterior sumir
  }
  return validas > 0 ? soma / validas : -1;
}

float calculaNivelOleo(float distanciaCm) {
  if (distanciaCm < 0) return -1;
  float nivel = (TANQUE_DIST_VAZIO_CM - distanciaCm) /
                (TANQUE_DIST_VAZIO_CM - TANQUE_DIST_CHEIO_CM) * 100.0;
  return constrain(nivel, 0.0f, 100.0f);
}

void iniciarSensores() {
  pinMode(PIN_CORRENTE, INPUT);
  pinMode(PIN_HC_TRIG, OUTPUT);
  pinMode(PIN_HC_ECHO, INPUT);
  pinMode(PIN_ENCODER_A, INPUT_PULLUP);
  pinMode(PIN_ENCODER_B, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_ENCODER_A), isrEncoder, RISING);
  ultimaLeituraRpmMs = millis();

  mpuEscreve(0x6B, 0x00);   // PWR_MGMT_1: acorda o MPU6050
  mpuEscreve(0x1C, 0x00);   // ACCEL_CONFIG: escala de ±2 g
}

// Lê os 4 sensores, monta o JSON e envia para todos os painéis conectados
void lerEEnviar() {
  float correnteA   = lerCorrenteA();
  float vibracaoG   = lerVibracaoG();
  float distanciaCm = lerDistanciaCm();
  float oleoPct     = calculaNivelOleo(distanciaCm);
  int   sentido     = 0;
  float rpm         = lerRpm(sentido);

  char json[sizeof(jsonAtual)];
  snprintf(json, sizeof(json),
           "{\"corrente_a\":%.2f,\"vibracao_g\":%.3f,\"rpm\":%.0f,\"sentido\":%d,"
           "\"distancia_cm\":%.1f,\"oleo_pct\":%.1f,\"voltas\":%.0f,"
           "\"rssi\":%d,\"uptime_s\":%lu}",
           correnteA, vibracaoG, rpm, sentido, distanciaCm, oleoPct, voltasTotais,
           modoPontoDeAcesso ? 0 : (int)WiFi.RSSI(), millis() / 1000UL);

  taskENTER_CRITICAL(&travaJson);
  memcpy(jsonAtual, json, sizeof(jsonAtual));
  taskEXIT_CRITICAL(&travaJson);

  websocket.textAll(json);   // tempo real para todos os painéis abertos
  Serial.println(json);
}

// =====================================================================
// SETUP E LOOP
// =====================================================================
void setup() {
  Serial.begin(115200);
  delay(2000);   // dá tempo de o Monitor Serial abrir pela USB da placa C3

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(100000);   // 100 kHz: velocidade segura para o LCD e o MPU6050

  iniciarLCD();
  lcdMostrar("LatheVision", "Iniciando...");
  iniciarSensores();
  iniciarRede();
  iniciarServidor();
}

void loop() {
  vigiarWiFi();

  if (pedidoZerar) {
    pedidoZerar = false;
    noInterrupts();
    pulsosEncoder = 0;
    interrupts();
    voltasTotais = 0;
    ultimaLeituraRpmMs = millis();
    Serial.println("[ZERAR] Contador de voltas zerado");
  }

  static unsigned long ultimoEnvio = 0;
  if (millis() - ultimoEnvio >= INTERVALO_ENVIO_MS) {
    ultimoEnvio = millis();
    lerEEnviar();
    websocket.cleanupClients();   // libera painéis que fecharam a aba
  }
}

// =====================================================================
// PAINEL (cópia do index.html da pasta principal do projeto)
// Não edite à mão: edite o index.html e dê dois cliques em
// esp32\atualizar_painel.bat, que reescreve o trecho entre as marcas abaixo.
// =====================================================================
// >>> INICIO DO PAINEL
const char PAINEL_HTML[] PROGMEM = R"lvpainel(
<!DOCTYPE html>
<html lang="pt-BR">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1.0, viewport-fit=cover" />
  <title>LATHEVISION · Monitoramento Remoto</title>


  <style>
    :root {
      --vermelho: #e8141c;
      --vermelho-vivo: #ff2a2a;
      --muted: #d9a3a3;
      --borda: #5a0000;
      --ok: #3ddc6b;
      --alerta: #f4c430;
      --critico: #ff3b3b;
    }

    * { box-sizing: border-box; margin: 0; padding: 0; }

    body {
      font-family: "Helvetica Neue", Helvetica, Arial, sans-serif;
      color: #fff;
      min-height: 100vh;
      background-color: #140000;
      background-image: radial-gradient(ellipse 90% 60% at 50% 0%, #5c0000 0%, #2a0000 45%, #140000 100%);
      background-attachment: fixed;
      -webkit-font-smoothing: antialiased;
    }

    .hidden { display: none !important; }

    /* ---------- LOGIN ---------- */
    .login {
      min-height: 100vh;
      display: grid;
      place-items: center;
      padding: 24px;
    }

    .login-card {
      width: min(92vw, 380px);
      padding: 28px 24px 32px;
      border: 1px solid var(--borda);
      border-radius: 16px;
      background: linear-gradient(180deg, #3e0000 0%, #1c0000 100%);
      box-shadow: 0 20px 60px rgba(0, 0, 0, 0.5);
      text-align: center;
    }

    .logo { font-size: 2.1rem; font-weight: 800; letter-spacing: 0.06em; }
    .logo span, .logo-sm span { color: var(--vermelho-vivo); }

    .tagline {
      margin-top: 8px;
      font-size: 0.68rem;
      letter-spacing: 0.32em;
      color: var(--muted);
    }

    .login-titulo { margin: 28px 0 18px; font-size: 1.25rem; }

    .campo {
      display: block;
      text-align: left;
      font-size: 0.95rem;
      margin: 14px 0 6px;
    }

    input {
      width: 100%;
      padding: 14px;
      font-size: 16px; /* evita zoom automático no iPhone */
      color: #fff;
      background: #120000;
      border: 1px solid var(--borda);
      border-radius: 10px;
      outline: none;
    }

    input:focus { border-color: var(--vermelho-vivo); }
    input::placeholder { color: #8a6b6b; }

    .btn-principal {
      width: 100%;
      margin-top: 24px;
      padding: 16px;
      border: 0;
      border-radius: 10px;
      font: 700 1rem "Helvetica Neue", Arial, sans-serif;
      color: #fff;
      letter-spacing: 0.02em;
      background: linear-gradient(90deg, #a00000, #ff1f1f);
      cursor: pointer;
    }

    .msg-erro {
      min-height: 1.2em;
      margin-top: 12px;
      font-size: 0.85rem;
      color: var(--critico);
    }

    /* ---------- TOPO ---------- */
    .topo {
      position: sticky;
      top: 0;
      z-index: 10;
      display: grid;
      grid-template-columns: 1fr auto 1fr;
      align-items: center;
      gap: 12px;
      padding: calc(14px + env(safe-area-inset-top, 0px)) 20px 14px;
      background: linear-gradient(90deg, #6a0000 0%, #3a0000 50%, #6a0000 100%);
      border-bottom: 1px solid #8a0a0a;
      box-shadow: 0 6px 20px rgba(0, 0, 0, 0.35);
    }

    .topo-tagline {
      font-size: 0.68rem;
      letter-spacing: 0.3em;
      color: #f0d6d6;
      white-space: nowrap;
    }

    .topo .status { justify-self: end; }

    @media (max-width: 720px) {
      .topo { grid-template-columns: 1fr auto; }
      .topo-tagline { display: none; }
    }

    .logo-sm { font-size: 1.35rem; font-weight: 800; letter-spacing: 0.05em; }

    .status {
      display: flex;
      align-items: center;
      gap: 10px;
      font-size: 0.7rem;
      color: var(--muted);
    }

    .status small { display: block; }
    .status b { font-size: 0.8rem; color: var(--st); }

    .dot {
      width: 10px;
      height: 10px;
      border-radius: 50%;
      flex: none;
      background: var(--st);
      box-shadow: 0 0 10px var(--st);
    }

    .st-ok { --st: var(--ok); }
    .st-alerta { --st: var(--alerta); }
    .st-critico { --st: var(--critico); }
    .st-off { --st: #888; }

    /* ---------- CONTEÚDO ---------- */
    .conteudo {
      width: 100%;
      max-width: 1560px;
      margin: 0 auto;
      padding: 8px clamp(16px, 2vw, 28px) 120px;
    }

    /* Grade dos gráficos: 1 coluna no celular, 2 colunas largas a partir do tablet */
    .grade-cards {
      display: grid;
      grid-template-columns: 1fr;
      gap: 20px;
      margin-top: 20px;
    }

    .grade-cards .card {
      margin-top: 0;
      display: flex;
      flex-direction: column;
      min-width: 0;
    }

    .grade-cards .card > svg { margin-top: auto; padding-top: 10px; }

    @media (min-width: 900px) {
      .grade-cards { grid-template-columns: repeat(2, minmax(0, 1fr)); }
    }

    /* Texto mais legível em telas largas */
    .conteudo > section > .subtitulo,
    .lista-alertas { max-width: 900px; }

    .titulo-pagina {
      margin-top: 18px;
      font-size: clamp(1.25rem, 1rem + 1.2vw, 1.8rem);
      font-weight: 800;
      letter-spacing: 0.01em;
    }

    .subtitulo {
      margin-top: 8px;
      font-size: 0.95rem;
      line-height: 1.45;
      color: var(--muted);
    }

    .info {
      margin-top: 18px;
      padding: 18px 22px;
      border: 1px solid var(--borda);
      border-radius: 14px;
      background: linear-gradient(90deg, #3a0000, #140000);
      font-size: 1.1rem;
      line-height: 1.7;
    }

    .info small {
      display: block;
      margin-top: 8px;
      font-size: 0.72rem;
      color: var(--muted);
      word-break: break-all;
    }

    .card {
      margin-top: 16px;
      padding: 22px 22px 20px;
      border: 1px solid #7a0a0a;
      border-radius: 14px;
      background: linear-gradient(135deg, #600000 0%, #300000 50%, #180000 100%);
      box-shadow: 0 10px 30px rgba(0, 0, 0, 0.35);
    }

    .card-topo {
      display: flex;
      flex-wrap: wrap;
      justify-content: space-between;
      align-items: flex-start;
      gap: 12px;
    }

    .card-titulo {
      display: flex;
      align-items: center;
      gap: 8px;
      font-size: 1rem;
      font-weight: 700;
      letter-spacing: 0.02em;
      color: #fff;
    }

    .valor {
      font-size: clamp(1.5rem, 1.2rem + 1vw, 1.9rem);
      font-weight: 800;
      white-space: nowrap;
    }

    .valor small {
      font-size: 0.8rem;
      font-weight: 500;
      color: var(--muted);
      margin-left: 6px;
    }

    .badge {
      display: inline-block;
      align-self: flex-start;
      margin-top: 10px;
      padding: 5px 10px;
      border-radius: 999px;
      font-size: 0.7rem;
      font-weight: 800;
      letter-spacing: 0.02em;
    }

    .badge.normal { color: var(--ok); background: rgba(0, 0, 0, 0.35); }
    .badge.alerta { color: var(--alerta); background: rgba(244, 196, 48, 0.14); }
    .badge.critico { color: var(--critico); background: rgba(255, 59, 59, 0.16); }
    .badge.sem { color: var(--muted); background: rgba(255, 255, 255, 0.06); }

    .limites {
      display: flex;
      flex-wrap: wrap;
      gap: 6px 14px;
      margin-top: 10px;
      font-size: 0.78rem;
      color: var(--muted);
    }

    .limites b { font-weight: 700; }
    .limites .lim-alerta b { color: var(--alerta); }
    .limites .lim-critico b { color: var(--critico); }

    select {
      width: 100%;
      padding: 14px;
      font-size: 16px;
      color: #fff;
      background: #120000;
      border: 1px solid var(--borda);
      border-radius: 10px;
      outline: none;
    }

    select:focus { border-color: var(--vermelho-vivo); }

    .grafico {
      display: block;
      width: 100%;
      height: auto;
      margin-top: 10px;
    }

    .medidor {
      display: block;
      width: 100%;
      max-width: 300px;
      height: auto;
      margin: 6px auto 0;
    }

    .rosca {
      display: block;
      width: 100%;
      max-width: 230px;
      height: auto;
      margin: 10px auto 0;
    }

    .tendencia {
      margin-top: 8px;
      font-size: 0.78rem;
      color: var(--muted);
    }

    .tendencia b { color: #fff; }

    /* ---------- CONFIGURAÇÕES ---------- */
    .card-config { padding: 8px 26px; margin-top: 22px; }

    /* Parte específica para o IP do ESP32 (no painel) */
    .card-ip .linha-ip { display: flex; gap: 10px; margin-top: 12px; }
    .card-ip .linha-ip input { flex: 1; min-width: 0; }
    .card-ip .btn-principal { width: auto; margin-top: 0; padding: 14px 22px; }
    .card-ip .msg-erro { margin-top: 8px; }
    .card-ip .aviso-ip { font-size: 1.4rem; color: #fff; }
    .btn-acao { width: auto; padding: 14px 22px; }
    @media (max-width: 480px) {
      .card-ip .linha-ip { flex-direction: column; }
      .card-ip .btn-principal { width: 100%; }
    }

    .opcao {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 16px;
      padding: 22px 0;
      border-bottom: 1px solid rgba(255, 255, 255, 0.06);
      cursor: pointer;
    }

    .opcao:last-child { border-bottom: 0; }
    .opcao b { display: block; font-size: 1.1rem; }
    .opcao small { display: block; margin-top: 6px; font-size: 0.8rem; color: var(--muted); }

    .chave {
      position: relative;
      flex: none;
      width: 48px;
      height: 26px;
    }

    .chave input {
      position: absolute;
      inset: 0;
      width: 100%;
      height: 100%;
      margin: 0;
      padding: 0;
      opacity: 0;
      cursor: pointer;
    }

    .chave span {
      position: absolute;
      inset: 0;
      border-radius: 999px;
      background: #3a0000;
      transition: background 0.2s;
      pointer-events: none;
    }

    .chave span::after {
      content: "";
      position: absolute;
      top: 4px;
      left: 4px;
      width: 18px;
      height: 18px;
      border-radius: 50%;
      background: #fff;
      transition: transform 0.2s;
    }

    .chave input:checked + span { background: #c00000; }
    .chave input:checked + span::after { transform: translateX(22px); }
    .chave input:focus-visible + span { outline: 2px solid #fff; outline-offset: 2px; }

    .svg-valor {
      font: 800 34px "Helvetica Neue", Arial, sans-serif;
      fill: #fff;
    }

    .svg-sub {
      font: 600 12px "Helvetica Neue", Arial, sans-serif;
      fill: var(--muted);
      letter-spacing: 0.08em;
    }

    /* ---------- ALERTAS / HISTÓRICO ---------- */
    .lista-alertas { margin-top: 16px; display: grid; gap: 10px; }

    .item-alerta {
      padding: 14px 16px;
      border-radius: 12px;
      border-left: 4px solid var(--muted);
      background: #2a0000;
    }

    .item-alerta.alerta { border-color: var(--alerta); }
    .item-alerta.critico { border-color: var(--critico); }

    .item-alerta small {
      display: block;
      margin-bottom: 4px;
      font-size: 0.75rem;
      color: var(--muted);
    }

    .vazio {
      margin-top: 24px;
      text-align: center;
      color: var(--muted);
    }

    .tabela-wrap {
      margin-top: 16px;
      padding: 0 12px;
      overflow-x: auto;
      border: 1px solid var(--borda);
      border-radius: 14px;
      background: #1c0000;
    }

    .subtitulo-secao {
      margin-top: 26px;
      font-size: 1.05rem;
      font-weight: 800;
      letter-spacing: 0.03em;
    }

    .tabela-limites { max-width: 900px; }
    .tabela-limites td:nth-child(2) { color: var(--alerta); font-weight: 700; }
    .tabela-limites td:nth-child(3) { color: var(--critico); font-weight: 700; }
    .tabela-limites tr.inativa td { opacity: 0.45; }
    .tabela-limites tr:last-child td { border-bottom: 0; }
    .tag-ativa {
      margin-left: 6px;
      padding: 2px 6px;
      border-radius: 6px;
      font-size: 0.68rem;
      color: #fff;
      background: #8a0000;
    }

    table { width: 100%; border-collapse: collapse; font-size: 0.85rem; }
    th, td {
      padding: 10px 6px;
      text-align: left;
      white-space: nowrap;
      border-bottom: 1px solid #3a0000;
    }
    th { color: var(--muted); font-weight: 600; }

    /* ---------- MENU INFERIOR ---------- */
    .nav {
      position: fixed;
      left: 0;
      right: 0;
      bottom: 0;
      z-index: 10;
      display: flex;
      gap: 4px;
      overflow-x: auto;
      padding: 8px 6px calc(8px + env(safe-area-inset-bottom, 0px));
      background: #1a0000;
      border-top: 1px solid var(--borda);
    }

    .nav-btn {
      flex: 0 0 auto;
      min-width: 66px;
      padding: 8px 6px;
      border: 0;
      border-radius: 10px;
      background: transparent;
      color: var(--muted);
      font: 700 0.58rem "Helvetica Neue", Arial, sans-serif;
      letter-spacing: 0.03em;
      cursor: pointer;
    }

    .nav-btn .ico {
      display: block;
      margin-bottom: 4px;
      font-size: 1.2rem;
    }

    .nav-btn.ativo { color: #fff; background: #8a0000; }

    /* Menu centralizado e mais confortável em telas maiores */
    @media (min-width: 760px) {
      .nav { justify-content: center; gap: 8px; }
      .nav-btn { min-width: 96px; font-size: 0.68rem; padding: 10px 10px; }
    }

    @media (max-width: 380px) {
      .topo { padding-left: 14px; padding-right: 14px; }
      .logo-sm { font-size: 1.1rem; }
      .card { padding: 16px 14px 18px; }
      .info { padding: 14px 16px; font-size: 1rem; }
    }
  </style>
</head>
<body>

  <!-- ================= LOGIN ================= -->
  <section id="login" class="login">
    <div class="login-card">
      <h1 class="logo">LATHE<span>VISION</span></h1>
      <p class="tagline">SUA PRODUÇÃO, SEMPRE À VISTA</p>
      <h2 class="login-titulo">Monitoramento Remoto</h2>

      <label class="campo" for="operador">Nome do operador</label>
      <input id="operador" type="text" placeholder="Digite seu nome" autocomplete="off" />

      <label class="campo" for="torno">Identificação do torno</label>
      <input id="torno" type="text" placeholder="Ex.: Torno Mecânico 01" autocomplete="off" />

      <label class="campo" for="tensao">Tensão de alimentação</label>
      <select id="tensao">
        <option value="220">220 V</option>
        <option value="380">380 V</option>
      </select>

      <button id="btnIniciar" class="btn-principal" type="button">INICIAR MONITORAMENTO</button>
      <p id="msgLogin" class="msg-erro"></p>
    </div>
  </section>

  <!-- ================= APLICATIVO ================= -->
  <div id="app" class="hidden">

    <header class="topo">
      <div class="logo-sm">LATHE<span>VISION</span></div>
      <div class="topo-tagline">SUA PRODUÇÃO, SEMPRE À VISTA</div>
      <div id="status" class="status st-off">
        <span class="dot"></span>
        <div>
          <small>STATUS</small>
          <b id="statusTxt">CONECTANDO...</b>
        </div>
      </div>
    </header>

    <main class="conteudo">

      <!-- PAINEL -->
      <section id="view-painel">
        <h1 class="titulo-pagina">PAINEL DE MONITORAMENTO</h1>
        <p class="subtitulo">Monitoramento em tempo real dos principais parâmetros do torno.</p>

        <div class="info">
          <div>Operador: <b id="infoOperador">-</b></div>
          <div>Torno: <b id="infoTorno">-</b></div>
          <div>Tensão: <b id="infoTensao">-</b></div>
          <small>Conexão: <span id="infoTopico">não conectado</span></small>
        </div>

        <!-- Conexão com o ESP32 -->
        <article class="card card-ip">
          <div id="ipTitulo" class="card-titulo aviso-ip">📡 INFORME O IP</div>
          <label class="campo" for="ipEsp">IP do ESP32 (aparece no display LCD do torno)</label>
          <div class="linha-ip">
            <input id="ipEsp" type="text" placeholder="Ex.: 192.168.0.50" autocomplete="off" inputmode="decimal" />
            <button id="btnConectar" class="btn-principal" type="button">CONECTAR</button>
          </div>
          <p id="msgIp" class="msg-erro"></p>
        </article>

        <div class="grade-cards">
        <!-- Vibração -->
        <article class="card">
          <div class="card-topo">
            <div class="card-titulo">〰️ VIBRAÇÃO</div>
            <div class="valor"><span id="vibValor">--</span><small>G</small></div>
          </div>
          <span id="vibBadge" class="badge sem">AGUARDANDO DADOS</span>
          <p id="vibLim" class="limites"></p>
          <p id="vibTend" class="tendencia hidden"></p>
          <svg id="vibGrafico" class="grafico" viewBox="0 0 640 230" role="img" aria-label="Gráfico de barras da vibração"></svg>
        </article>

        <!-- Corrente -->
        <article class="card">
          <div class="card-topo">
            <div class="card-titulo">⚡ CORRENTE ELÉTRICA</div>
            <div class="valor"><span id="corrValor">--</span><small>A</small></div>
          </div>
          <span id="corrBadge" class="badge sem">AGUARDANDO DADOS</span>
          <p id="corrLim" class="limites"></p>
          <p id="corrTend" class="tendencia hidden"></p>
          <svg id="corrGrafico" class="grafico" viewBox="0 0 640 230" role="img" aria-label="Gráfico de linha da corrente"></svg>
        </article>

        <!-- Nível de óleo -->
        <article class="card">
          <div class="card-topo">
            <div class="card-titulo">🛢️ NÍVEL DE ÓLEO</div>
            <div class="valor"><span id="oleoValor">--</span><small>%</small></div>
          </div>
          <span id="oleoBadge" class="badge sem">AGUARDANDO DADOS</span>
          <p id="oleoLim" class="limites"></p>
          <svg id="oleoRosca" class="rosca" viewBox="0 0 220 220" role="img" aria-label="Nível de óleo"></svg>
        </article>

        <!-- Velocidade -->
        <article class="card">
          <div class="card-topo">
            <div class="card-titulo">⚙️ VELOCIDADE</div>
            <div class="valor"><span id="rpmValor">--</span><small>RPM</small></div>
          </div>
          <span id="rpmBadge" class="badge sem">AGUARDANDO DADOS</span>
          <p id="rpmLim" class="limites"></p>
          <p class="tendencia">Sensor: <b>encoder</b> · Sentido de rotação: <b id="rpmSentido">--</b></p>
          <svg id="rpmMedidor" class="medidor" viewBox="0 0 260 215" role="img" aria-label="Velocidade em RPM"></svg>
        </article>
        </div>
      </section>

      <!-- ALERTAS -->
      <section id="view-alertas" class="hidden">
        <h1 class="titulo-pagina">ALERTAS</h1>
        <p class="subtitulo">Limites usados para gerar os alertas de cada sensor.</p>

        <div class="tabela-wrap tabela-limites">
          <table>
            <thead>
              <tr><th>Sensor</th><th>Alerta</th><th>Crítico</th></tr>
            </thead>
            <tbody id="tabelaLimites"></tbody>
          </table>
        </div>

        <h2 class="subtitulo-secao">Alertas desta sessão</h2>
        <div id="listaAlertas" class="lista-alertas"></div>
      </section>

      <!-- HISTÓRICO -->
      <section id="view-historico" class="hidden">
        <h1 class="titulo-pagina">HISTÓRICO</h1>
        <p class="subtitulo">Últimas leituras recebidas do torno.</p>
        <div class="tabela-wrap">
          <table>
            <thead>
              <tr>
                <th>Hora</th>
                <th>Vibração (G)</th>
                <th>Corrente (A)</th>
                <th>RPM</th>
                <th>Óleo (%)</th>
              </tr>
            </thead>
            <tbody id="tabelaHist"></tbody>
          </table>
        </div>
      </section>

      <!-- RELATÓRIOS -->
      <section id="view-relatorios" class="hidden">
        <h1 class="titulo-pagina">RELATÓRIOS</h1>
        <p class="subtitulo">Resumo das leituras reais recebidas do ESP32 desde que o painel foi aberto.</p>
        <div class="tabela-wrap">
          <table>
            <thead>
              <tr><th>Sensor</th><th>Leituras</th><th>Mínimo</th><th>Média</th><th>Máximo</th><th>Última</th></tr>
            </thead>
            <tbody id="tabelaRelatorio"></tbody>
          </table>
        </div>
        <button id="btnCsv" class="btn-principal btn-acao" type="button">BAIXAR HISTÓRICO (CSV)</button>
      </section>

      <!-- EQUIPAMENTOS -->
      <section id="view-equipamentos" class="hidden">
        <h1 class="titulo-pagina">EQUIPAMENTOS</h1>
        <p class="subtitulo">Situação do ESP32 e dos sensores instalados no torno.</p>
        <div class="tabela-wrap">
          <table><tbody id="tabelaEsp"></tbody></table>
        </div>
        <h2 class="subtitulo-secao">Sensores</h2>
        <div class="tabela-wrap">
          <table>
            <thead><tr><th>Medição</th><th>Sensor</th><th>Pinos (ESP32-C3)</th></tr></thead>
            <tbody>
              <tr><td>Corrente</td><td>SCT-013</td><td>GPIO 4</td></tr>
              <tr><td>Vibração</td><td>MPU6050</td><td>SDA 6 · SCL 5</td></tr>
              <tr><td>Nível de óleo</td><td>HC-SR04</td><td>TRIG 10 · ECHO 7</td></tr>
              <tr><td>Velocidade</td><td>Encoder</td><td>A 20 · B 21</td></tr>
              <tr><td>Display</td><td>LCD 16x2 I2C</td><td>SDA 6 · SCL 5</td></tr>
            </tbody>
          </table>
        </div>
        <button id="btnZerar" class="btn-principal btn-acao" type="button">ZERAR CONTADOR DE VOLTAS</button>
        <p id="msgZerar" class="msg-erro"></p>
      </section>

      <!-- CONFIGURAÇÕES -->
      <section id="view-config" class="hidden">
        <h1 class="titulo-pagina">CONFIGURAÇÕES</h1>
        <p class="subtitulo">Configure o sistema de monitoramento.</p>

        <div class="card card-config">
          <label class="opcao">
            <div>
              <b>Monitoramento automático</b>
              <small>Atualizar os dados automaticamente.</small>
            </div>
            <span class="chave"><input type="checkbox" id="cfgAuto" checked /><span></span></span>
          </label>
          <label class="opcao">
            <div>
              <b>Alertas sonoros</b>
              <small>Emitir alerta quando houver uma condição crítica.</small>
            </div>
            <span class="chave"><input type="checkbox" id="cfgSom" checked /><span></span></span>
          </label>
          <label class="opcao">
            <div>
              <b>Manutenção preditiva</b>
              <small>Identificar possíveis falhas.</small>
            </div>
            <span class="chave"><input type="checkbox" id="cfgPreditiva" checked /><span></span></span>
          </label>
        </div>
      </section>

    </main>

    <!-- MENU INFERIOR -->
    <nav class="nav">
      <button class="nav-btn ativo" data-view="painel"><span class="ico">⌂</span>PAINEL</button>
      <button class="nav-btn" data-view="historico"><span class="ico">📈</span>HISTÓRICO</button>
      <button class="nav-btn" data-view="alertas"><span class="ico">⚠️</span>ALERTAS</button>
      <button class="nav-btn" data-view="relatorios"><span class="ico">☰</span>RELATÓRIOS</button>
      <button class="nav-btn" data-view="equipamentos"><span class="ico">⚙️</span>EQUIPAMENTOS</button>
      <button class="nav-btn" data-view="config"><span class="ico">🛠️</span>CONFIGURAÇÕES</button>
      <button class="nav-btn" data-view="sair"><span class="ico">⇥</span>SAIR</button>
    </nav>
  </div>

  <script>
  /* =====================================================================
     CONFIGURAÇÃO — ajuste aqui
     ===================================================================== */
  const CONFIG = {
    INTERVALO_MS: 1000,                            // de quanto em quanto tempo o painel pede os dados ao ESP32
    TIMEOUT_PEDIDO_MS: 3000,                       // pedido sem resposta por mais que isso = falhou
    FALHAS_PARA_DESCONECTAR: 3,                    // pedidos seguidos sem resposta até mostrar DESCONECTADO
    PONTOS_GRAFICO: 30,                            // quantas leituras aparecem nos gráficos
    TIMEOUT_SEM_SINAL_MS: 10000,                   // sem dados por mais que isso = SEM SINAL

    // Limites: alerta e crítico. Acima do limite = pior, exceto onde inverso: true (abaixo = pior)
    VIBRACAO: { unidade: 'G', alerta: 0.7, critico: 1.2 },

    // Corrente depende da tensão escolhida no login
    CORRENTE_POR_TENSAO: {
      220: { unidade: 'A', alerta: 7.2, critico: 8.8 },
      380: { unidade: 'A', alerta: 4.5, critico: 5.5 },
    },

    // Velocidade: alerta acima da rotação máx. do spindle, crítico 10% acima dela
    RPM_MAX_SPINDLE: 2000,

    // Distância do HC-SR04 até o óleo (cm): tanque cheio = distância pequena
    OLEO: { cheioCm: 5, vazioCm: 25, alerta: 25, critico: 10, inverso: true }, // alerta/crítico em %

  };

  CONFIG.CORRENTE = CONFIG.CORRENTE_POR_TENSAO[220];
  CONFIG.RPM = {
    alerta: CONFIG.RPM_MAX_SPINDLE,
    critico: CONFIG.RPM_MAX_SPINDLE * 1.1,
    maximo: CONFIG.RPM_MAX_SPINDLE * 1.2, // fundo de escala do medidor
  };

  /* =====================================================================
     ESTADO E UTILIDADES
     ===================================================================== */
  const $ = (id) => document.getElementById(id);

  const ROTULO = { normal: '✓ NORMAL', alerta: '⚠ ALERTA', critico: '✖ CRÍTICO', sem: 'AGUARDANDO DADOS' };
  const NOME = { vibracao: 'Vibração', corrente: 'Corrente elétrica', rpm: 'Velocidade', oleo: 'Nível de óleo' };
  const COR = { normal: '#3ddc6b', alerta: '#f4c430', critico: '#ff3b3b', vermelho: '#e8141c' };

  const estado = {
    base: '',            // endereço do ESP32, ex.: http://192.168.0.50
    ws: null,            // WebSocket aberto com o ESP32 (ws://IP/ws)
    busca: null,         // timer do próximo pedido de dados / nova tentativa
    falhas: 0,           // pedidos seguidos que falharam
    conectado: false,
    ultimaLeitura: 0,
    leitura: { vibracao: null, corrente: null, rpm: null, oleo: null },
    sentido: null,
    series: { vibracao: [], corrente: [] },
    registros: [],
    // Resumo da sessão para a aba Relatórios (só leituras reais)
    resumo: {
      vibracao: { n: 0, soma: 0, min: null, max: null },
      corrente: { n: 0, soma: 0, min: null, max: null },
      rpm: { n: 0, soma: 0, min: null, max: null },
      oleo: { n: 0, soma: 0, min: null, max: null },
    },
    esp: { rssi: null, uptime: null, voltas: null, hora: null },   // dados do ESP32 (aba Equipamentos)
    alertas: [],
    nivelAnterior: {},
    timer: null,
    opcoes: { auto: true, som: true, preditiva: true },
  };

  function numero(x) {
    const n = typeof x === 'number' ? x : parseFloat(x);
    return Number.isFinite(n) ? n : null;
  }

  function fmt(v, casas = 0) {
    return v === null ? '--' : v.toFixed(casas).replace('.', ',');
  }

  function hora() {
    return new Date().toLocaleTimeString('pt-BR');
  }

  function empurra(arr, valor) {
    arr.push(valor);
    if (arr.length > CONFIG.PONTOS_GRAFICO) arr.shift();
  }

  function classificar(valor, lim) {
    if (valor === null) return 'sem';
    if (lim.inverso) return valor < lim.critico ? 'critico' : valor < lim.alerta ? 'alerta' : 'normal';
    return valor > lim.critico ? 'critico' : valor > lim.alerta ? 'alerta' : 'normal';
  }

  function oleoPct(cm) {
    const { cheioCm, vazioCm } = CONFIG.OLEO;
    const pct = ((vazioCm - cm) / (vazioCm - cheioCm)) * 100;
    return Math.round(Math.min(100, Math.max(0, pct)));
  }

  function niveis() {
    const L = estado.leitura;
    return {
      vibracao: classificar(L.vibracao, CONFIG.VIBRACAO),
      corrente: classificar(L.corrente, CONFIG.CORRENTE),
      rpm: classificar(L.rpm, CONFIG.RPM),
      oleo: classificar(L.oleo, CONFIG.OLEO),
    };
  }

  function valorTexto(chave) {
    const L = estado.leitura;
    if (chave === 'vibracao') return `${fmt(L.vibracao, 2)} ${CONFIG.VIBRACAO.unidade}`;
    if (chave === 'corrente') return `${fmt(L.corrente, 1)} ${CONFIG.CORRENTE.unidade}`;
    if (chave === 'rpm') return `${fmt(L.rpm, 0)} RPM`;
    return `${fmt(L.oleo, 0)}%`;
  }

  /* =====================================================================
     RECEBIMENTO DOS DADOS (http://<IP do ESP32>/dados)
     Formato do firmware: {"vibracao":0.35,"corrente":7.4,"rpm":1400,"oleo_cm":12.3,"rssi":-58}
     Valor -1 = sensor sem leitura (é ignorado)
     (também aceita: vibracao_g, corrente_a, sentido, distancia_cm, oleo_pct)
     ===================================================================== */
  function processarDados(d) {
    const v = numero(d.vibracao_g ?? d.vibracao);
    const c = numero(d.corrente_a ?? d.corrente);
    const r = numero(d.rpm);
    const sent = numero(d.sentido);
    const op = numero(d.oleo_pct);
    const oc = numero(d.distancia_cm ?? d.oleo_cm);

    if (v !== null && v >= 0) { estado.leitura.vibracao = v; empurra(estado.series.vibracao, v); }
    if (c !== null && c >= 0) { estado.leitura.corrente = c; empurra(estado.series.corrente, c); }
    if (r !== null && r >= 0) estado.leitura.rpm = r;
    if (sent !== null) estado.sentido = sent;
    if (op !== null && op >= 0) estado.leitura.oleo = Math.round(Math.min(100, op));
    else if (oc !== null && oc > 0) estado.leitura.oleo = oleoPct(oc); // valor <= 0 = leitura inválida

    // Resumo da sessão: entra só o que chegou válido NESTA mensagem
    const oleoNovo = (op !== null && op >= 0) || (oc !== null && oc > 0);
    acumula('vibracao', v !== null && v >= 0);
    acumula('corrente', c !== null && c >= 0);
    acumula('rpm', r !== null && r >= 0);
    acumula('oleo', oleoNovo);

    // Dados do próprio ESP32 (aba Equipamentos)
    estado.esp.rssi = numero(d.rssi);
    estado.esp.uptime = numero(d.uptime_s);
    estado.esp.voltas = numero(d.voltas);
    estado.esp.hora = hora();

    estado.ultimaLeitura = Date.now();
    estado.registros.push({ hora: hora(), ...estado.leitura });
    if (estado.registros.length > 50) estado.registros.shift();

    verificarAlertas();
    if (estado.opcoes.auto) {
      renderizar();
      // Atualiza em tempo real a aba que estiver aberta
      if (!$('view-historico').classList.contains('hidden')) renderHistorico();
      if (!$('view-alertas').classList.contains('hidden')) renderAlertas();
      if (!$('view-relatorios').classList.contains('hidden')) renderRelatorio();
      if (!$('view-equipamentos').classList.contains('hidden')) renderEquipamentos();
    }
  }

  function acumula(chave, chegou) {
    if (!chegou) return;
    const valor = estado.leitura[chave];
    const s = estado.resumo[chave];
    s.n++;
    s.soma += valor;
    s.min = s.min === null ? valor : Math.min(s.min, valor);
    s.max = s.max === null ? valor : Math.max(s.max, valor);
  }

  /* =====================================================================
     RELATÓRIOS E EQUIPAMENTOS
     ===================================================================== */
  function renderRelatorio() {
    const linhas = [
      ['Corrente (A)', 'corrente', 1],
      ['Vibração (G)', 'vibracao', 2],
      ['Velocidade (RPM)', 'rpm', 0],
      ['Nível de óleo (%)', 'oleo', 0],
    ].map(([nome, chave, casas]) => {
      const s = estado.resumo[chave];
      const media = s.n ? s.soma / s.n : null;
      return `<tr><td>${nome}</td><td>${s.n}</td><td>${fmt(s.min, casas)}</td>
        <td>${fmt(media, casas)}</td><td>${fmt(s.max, casas)}</td><td>${fmt(estado.leitura[chave], casas)}</td></tr>`;
    }).join('');
    $('tabelaRelatorio').innerHTML = linhas;
  }

  function baixarCsv() {
    if (!estado.registros.length) {
      alert('Ainda não chegou nenhuma leitura do ESP32.');
      return;
    }
    const cab = 'Hora;Vibração (G);Corrente (A);RPM;Óleo (%)';
    const linhas = estado.registros.map((r) =>
      [r.hora, fmt(r.vibracao, 2), fmt(r.corrente, 1), fmt(r.rpm, 0), fmt(r.oleo, 0)].join(';'));
    const blob = new Blob(['﻿' + [cab, ...linhas].join('\r\n')], { type: 'text/csv;charset=utf-8' });
    const link = document.createElement('a');
    link.href = URL.createObjectURL(blob);
    link.download = `lathevision-${new Date().toISOString().slice(0, 19).replace(/[:T]/g, '-')}.csv`;
    link.click();
    URL.revokeObjectURL(link.href);
  }

  function renderEquipamentos() {
    const E = estado.esp;
    const tempo = E.uptime === null ? '--'
      : `${Math.floor(E.uptime / 3600)} h ${Math.floor((E.uptime % 3600) / 60)} min`;
    const linhas = [
      ['Torno', $('infoTorno').textContent],
      ['Endereço do ESP32', estado.base || 'não conectado'],
      ['Conexão', estado.ws ? 'WebSocket (tempo real)' : estado.base ? 'HTTP (/dados)' : '--'],
      ['Situação', $('statusTxt').textContent],
      ['Sinal Wi-Fi', E.rssi ? `${E.rssi} dBm` : (E.rssi === 0 ? 'rede própria do ESP32' : '--')],
      ['Tempo ligado', tempo],
      ['Voltas do encoder', fmt(E.voltas, 0)],
      ['Última leitura', E.hora || 'aguardando dados'],
    ];
    $('tabelaEsp').innerHTML = linhas.map(([a, b]) => `<tr><th>${a}</th><td>${b}</td></tr>`).join('');
  }

  function zerarVoltas() {
    if (!estado.base) {
      $('msgZerar').textContent = 'Conecte ao ESP32 primeiro (aba Painel).';
      return;
    }
    fetch(estado.base + '/zerar', { method: 'POST', cache: 'no-store' })
      .then((r) => { if (!r.ok) throw new Error('HTTP ' + r.status); })
      .then(() => { $('msgZerar').textContent = 'Contador de voltas zerado.'; })
      .catch(() => { $('msgZerar').textContent = 'O ESP32 não respondeu ao pedido de zerar.'; });
  }

  function verificarAlertas() {
    const atuais = niveis();
    Object.keys(atuais).forEach((chave) => {
      const nivel = atuais[chave];
      if (nivel !== estado.nivelAnterior[chave] && (nivel === 'alerta' || nivel === 'critico')) {
        const grau = nivel === 'critico' ? 'crítico' : 'alerta';
        estado.alertas.unshift({
          hora: hora(),
          nivel,
          texto: `${NOME[chave]}: ${valorTexto(chave)} (${grau})`,
        });
        if (estado.alertas.length > 100) estado.alertas.pop();
        if (nivel === 'critico' && estado.opcoes.som) bipe();
      }
    });
    estado.nivelAnterior = atuais;
  }

  /* =====================================================================
     RENDERIZAÇÃO
     ===================================================================== */
  function renderizar() {
    const L = estado.leitura;
    const N = niveis();

    $('vibValor').textContent = fmt(L.vibracao, 2);
    $('corrValor').textContent = fmt(L.corrente, 1);
    $('rpmValor').textContent = fmt(L.rpm, 0);
    $('oleoValor').textContent = fmt(L.oleo, 0);

    definirBadge('vibBadge', N.vibracao);
    definirBadge('corrBadge', N.corrente);
    definirBadge('rpmBadge', N.rpm);
    definirBadge('oleoBadge', N.oleo);

    desenharBarras($('vibGrafico'), estado.series.vibracao);
    desenharLinha($('corrGrafico'), estado.series.corrente);
    renderTendencias();
    const SENTIDO = { 1: 'horário', '-1': 'anti-horário', 0: 'parado' };
    $('rpmSentido').textContent = estado.sentido === null ? '--' : (SENTIDO[estado.sentido] || '--');
    desenharMedidor($('rpmMedidor'), L.rpm);
    desenharRosca($('oleoRosca'), L.oleo, N.oleo);

    atualizarStatus();
  }

  function linhaLimite(id, alerta, critico) {
    $(id).innerHTML =
      `<span class="lim-alerta">Alerta: <b>${alerta}</b></span>` +
      `<span class="lim-critico">Crítico: <b>${critico}</b></span>`;
  }

  function renderLimites() {
    const V = CONFIG.VIBRACAO, C = CONFIG.CORRENTE, O = CONFIG.OLEO;
    linhaLimite('vibLim', `> ${fmt(V.alerta, 1)} ${V.unidade}`, `> ${fmt(V.critico, 1)} ${V.unidade}`);
    linhaLimite('corrLim', `> ${fmt(C.alerta, 1)} A`, `> ${fmt(C.critico, 1)} A`);
    linhaLimite('oleoLim', `< ${O.alerta}%`, `< ${O.critico}%`);
    linhaLimite('rpmLim',
      `> ${fmt(CONFIG.RPM.alerta, 0)} RPM (máx. spindle)`,
      `> ${fmt(CONFIG.RPM.critico, 0)} RPM (+10%)`);
  }

  function definirBadge(id, nivel) {
    const el = $(id);
    el.className = 'badge ' + nivel;
    el.textContent = ROTULO[nivel];
  }

  function gradeSvg(W, H, P) {
    let grade = '';
    for (let i = 0; i <= 4; i++) {
      const y = P + (i * (H - 2 * P)) / 4;
      grade += `<line x1="${P}" x2="${W - P}" y1="${y}" y2="${y}" stroke="rgba(255,255,255,0.07)" stroke-width="1"/>`;
    }
    return grade;
  }

  // Gráfico de barras (vibração): últimas 10 leituras, com degradê vermelho
  function desenharBarras(svg, todos) {
    const W = 640, H = 230, P = 24, QTD = 10;
    const valores = todos.slice(-QTD);
    const grade = gradeSvg(W, H, P);
    const defs = `<defs><linearGradient id="degBarra" x1="0" y1="0" x2="0" y2="1">
        <stop offset="0" stop-color="#ff3b3b"/><stop offset="1" stop-color="#8a0a0a"/>
      </linearGradient></defs>`;

    if (!valores.length) { svg.innerHTML = defs + grade; return; }

    const max = Math.max(...valores, CONFIG.VIBRACAO.alerta) * 1.15;
    const passo = (W - 2 * P) / QTD;
    const largura = passo * 0.56;
    const barras = valores.map((v, i) => {
      const h = Math.max(2, (v / max) * (H - 2 * P));
      const x = P + i * passo + (passo - largura) / 2;
      const cor = classificar(v, CONFIG.VIBRACAO);
      const fill = cor === 'critico' ? COR.critico : cor === 'alerta' ? COR.alerta : 'url(#degBarra)';
      return `<rect x="${x}" y="${H - P - h}" width="${largura}" height="${h}" rx="2" fill="${fill}"/>`;
    }).join('');

    svg.innerHTML = defs + grade + barras;
  }

  function desenharLinha(svg, valores) {
    const W = 640, H = 230, P = 24, N = CONFIG.PONTOS_GRAFICO;
    const grade = gradeSvg(W, H, P);

    if (valores.length < 2) {
      svg.innerHTML = grade;
      return;
    }

    let min = Math.min(...valores);
    let max = Math.max(...valores);
    if (max - min < 1e-6) { min -= 0.5; max += 0.5; }
    const folga = (max - min) * 0.15;
    min -= folga;
    max += folga;

    const pontos = valores.map((v, i) => [
      P + (i * (W - 2 * P)) / (N - 1),
      H - P - ((v - min) / (max - min)) * (H - 2 * P),
    ]);

    const linha = pontos.map((p) => p.join(',')).join(' ');
    const bolas = pontos
      .map(([x, y]) => `<circle cx="${x}" cy="${y}" r="4" fill="#ff3b3b"/>`)
      .join('');

    svg.innerHTML = `${grade}
      <polyline points="${linha}" fill="none" stroke="#ff3b3b" stroke-width="2.5"
                stroke-linejoin="round" stroke-linecap="round"/>
      ${bolas}`;
  }

  // Desenha um arco do medidor: f1 e f2 vão de 0 a 1 ao longo de 270° (de baixo-esquerda até baixo-direita)
  function arco(cx, cy, r, f1, f2) {
    const a1 = ((135 + 270 * f1) * Math.PI) / 180;
    const a2 = ((135 + 270 * f2) * Math.PI) / 180;
    const x1 = cx + r * Math.cos(a1), y1 = cy + r * Math.sin(a1);
    const x2 = cx + r * Math.cos(a2), y2 = cy + r * Math.sin(a2);
    const grande = (f2 - f1) * 270 > 180 ? 1 : 0;
    return `M${x1} ${y1} A${r} ${r} 0 ${grande} 1 ${x2} ${y2}`;
  }

  function desenharMedidor(svg, valor) {
    const { maximo, alerta, critico } = CONFIG.RPM;
    const cx = 130, cy = 120, r = 92;
    const fA = alerta / maximo;
    const fC = critico / maximo;
    const f = valor === null ? 0 : Math.min(1, Math.max(0, valor / maximo));
    const ang = ((135 + 270 * f) * Math.PI) / 180;
    const px = cx + (r - 14) * Math.cos(ang);
    const py = cy + (r - 14) * Math.sin(ang);

    const ponteiro = valor === null
      ? ''
      : `<line x1="${cx}" y1="${cy}" x2="${px}" y2="${py}" stroke="${COR.vermelho}" stroke-width="4" stroke-linecap="round"/>`;

    svg.innerHTML = `
      <path d="${arco(cx, cy, r, 0, fA)}" stroke="${COR.normal}" stroke-width="16" fill="none"/>
      <path d="${arco(cx, cy, r, fA, fC)}" stroke="${COR.alerta}" stroke-width="16" fill="none"/>
      <path d="${arco(cx, cy, r, fC, 1)}" stroke="${COR.vermelho}" stroke-width="16" fill="none"/>
      ${ponteiro}
      <circle cx="${cx}" cy="${cy}" r="9" fill="${COR.vermelho}"/>
      <text x="${cx}" y="${cy + 46}" text-anchor="middle" class="svg-valor">${fmt(valor, 0)}</text>
      <text x="${cx}" y="${cy + 66}" text-anchor="middle" class="svg-sub">RPM</text>`;
  }

  function desenharRosca(svg, pct, nivel) {
    const r = 80, c = 110;
    const C = 2 * Math.PI * r;
    const f = pct === null ? 0 : pct / 100;
    const cor = nivel === 'critico' ? COR.critico : nivel === 'alerta' ? COR.alerta : COR.vermelho;

    svg.innerHTML = `
      <circle cx="${c}" cy="${c}" r="${r}" fill="none" stroke="#3a0000" stroke-width="22"/>
      <circle cx="${c}" cy="${c}" r="${r}" fill="none" stroke="${cor}" stroke-width="22"
              stroke-dasharray="${C}" stroke-dashoffset="${C * (1 - f)}"
              transform="rotate(-90 ${c} ${c})"/>
      <circle cx="${c}" cy="${c}" r="68" fill="#140000"/>
      <text x="${c}" y="${c + 8}" text-anchor="middle" class="svg-valor">${pct === null ? '--' : pct + '%'}</text>
      <text x="${c}" y="${c + 30}" text-anchor="middle" class="svg-sub">NÍVEL DE ÓLEO</text>`;
  }

  // Manutenção preditiva: compara a média das leituras mais recentes com as anteriores
  function tendencia(serie, lim) {
    if (serie.length < 10) return null;
    const metade = Math.floor(serie.length / 2);
    const media = (a) => a.reduce((x, y) => x + y, 0) / a.length;
    const antes = media(serie.slice(0, metade));
    const agora = media(serie.slice(metade));
    const variacao = (agora - antes) / (Math.abs(antes) || 1);
    if (variacao > 0.05) {
      const perto = agora >= lim.alerta * 0.9;
      return `Tendência: <b>subindo</b>${perto ? ' · próximo do limite de alerta, programe uma inspeção' : ''}`;
    }
    if (variacao < -0.05) return 'Tendência: <b>caindo</b>';
    return 'Tendência: <b>estável</b>';
  }

  function renderTendencias() {
    [['vibTend', estado.series.vibracao, CONFIG.VIBRACAO], ['corrTend', estado.series.corrente, CONFIG.CORRENTE]]
      .forEach(([id, serie, lim]) => {
        const texto = estado.opcoes.preditiva ? tendencia(serie, lim) : null;
        $(id).classList.toggle('hidden', !texto);
        $(id).innerHTML = texto || '';
      });
  }

  let audio = null;
  function bipe() {
    try {
      audio = audio || new (window.AudioContext || window.webkitAudioContext)();
      [0, 0.3, 0.6].forEach((t) => {
        const osc = audio.createOscillator();
        const vol = audio.createGain();
        osc.type = 'square';
        osc.frequency.value = 880;
        vol.gain.value = 0.08;
        osc.connect(vol).connect(audio.destination);
        osc.start(audio.currentTime + t);
        osc.stop(audio.currentTime + t + 0.18);
      });
    } catch (e) { /* navegador sem suporte a áudio */ }
  }

  function carregarOpcoes() {
    try {
      const salvas = JSON.parse(localStorage.getItem('lathevision-opcoes') || '{}');
      Object.assign(estado.opcoes, salvas);
    } catch (e) { /* sem armazenamento local */ }
    $('cfgAuto').checked = estado.opcoes.auto;
    $('cfgSom').checked = estado.opcoes.som;
    $('cfgPreditiva').checked = estado.opcoes.preditiva;
  }

  function salvarOpcoes() {
    estado.opcoes.auto = $('cfgAuto').checked;
    estado.opcoes.som = $('cfgSom').checked;
    estado.opcoes.preditiva = $('cfgPreditiva').checked;
    try { localStorage.setItem('lathevision-opcoes', JSON.stringify(estado.opcoes)); } catch (e) { /* ignora */ }
    if (estado.opcoes.som && audio && audio.state === 'suspended') audio.resume();
    renderizar();
  }

  function atualizarStatus() {
    let st;
    if (!estado.base) {
      st = 'semip';
    } else if (!estado.conectado) {
      st = estado.falhas >= CONFIG.FALHAS_PARA_DESCONECTAR ? 'off' : 'conectando';
    } else if (!estado.ultimaLeitura || Date.now() - estado.ultimaLeitura > CONFIG.TIMEOUT_SEM_SINAL_MS) {
      st = 'semsinal';
    } else {
      const vals = Object.values(niveis());
      st = vals.includes('critico') ? 'critico' : vals.includes('alerta') ? 'alerta' : 'ok';
    }

    const TEXTO = {
      ok: 'OPERACIONAL', alerta: 'ATENÇÃO', critico: 'CRÍTICO',
      semsinal: 'SEM SINAL', off: 'DESCONECTADO', conectando: 'CONECTANDO...', semip: 'INFORME O IP',
    };
    const CLASSE = {
      ok: 'st-ok', alerta: 'st-alerta', critico: 'st-critico',
      semsinal: 'st-alerta', off: 'st-off', conectando: 'st-off', semip: 'st-off',
    };

    $('status').className = 'status ' + CLASSE[st];
    $('statusTxt').textContent = TEXTO[st];
  }

  function renderTabelaLimites() {
    const tensaoAtiva = $('tensao').value;
    const V = CONFIG.VIBRACAO, O = CONFIG.OLEO, R = CONFIG.RPM;
    const linhas = [
      ...Object.entries(CONFIG.CORRENTE_POR_TENSAO).map(([tensao, C]) => ({
        sensor: `Corrente (${tensao} V)` + (tensao === tensaoAtiva ? '<span class="tag-ativa">EM USO</span>' : ''),
        alerta: `> ${fmt(C.alerta, 1)} A`,
        critico: `> ${fmt(C.critico, 1)} A`,
        inativa: tensao !== tensaoAtiva,
      })),
      { sensor: 'Nível de óleo', alerta: `< ${O.alerta}%`, critico: `< ${O.critico}%` },
      { sensor: 'Vibração', alerta: `> ${fmt(V.alerta, 1)} ${V.unidade}`, critico: `> ${fmt(V.critico, 1)} ${V.unidade}` },
      {
        sensor: 'Velocidade',
        alerta: `> rotação máx. do spindle (${fmt(R.alerta, 0)} RPM)`,
        critico: `> 10% acima da máx. (${fmt(R.critico, 0)} RPM)`,
      },
    ];
    $('tabelaLimites').innerHTML = linhas.map((l) => `
      <tr class="${l.inativa ? 'inativa' : ''}">
        <td>${l.sensor}</td><td>${l.alerta}</td><td>${l.critico}</td>
      </tr>`).join('');
  }

  function renderAlertas() {
    renderTabelaLimites();
    const box = $('listaAlertas');
    if (!estado.alertas.length) {
      box.innerHTML = '<p class="vazio">Nenhum alerta nesta sessão.</p>';
      return;
    }
    box.innerHTML = estado.alertas
      .map((a) => `<div class="item-alerta ${a.nivel}"><small>${a.hora}</small>${a.texto}</div>`)
      .join('');
  }

  function renderHistorico() {
    const linhas = [...estado.registros].reverse().map((r) => `
      <tr>
        <td>${r.hora}</td>
        <td>${fmt(r.vibracao, 2)}</td>
        <td>${fmt(r.corrente, 1)}</td>
        <td>${fmt(r.rpm, 0)}</td>
        <td>${fmt(r.oleo, 0)}</td>
      </tr>`).join('');
    $('tabelaHist').innerHTML = linhas || '<tr><td colspan="5">Aguardando leituras...</td></tr>';
  }

  /* =====================================================================
     CONEXÃO COM O ESP32 (direto pelo IP, sem https)
     1º tenta o WebSocket ws://<IP>/ws (tempo real).
     Se o ESP32 não tiver WebSocket, pede http://<IP>/dados a cada segundo.
     Funciona com o painel aberto como arquivo ou pelo próprio IP do ESP32.
     ===================================================================== */
  function pararConexao() {
    clearTimeout(estado.busca);
    const ws = estado.ws;
    estado.ws = null;             // evita que o "fechou" abaixo tente reconectar
    if (ws) ws.close();
  }

  function conectarWebSocket() {
    const base = estado.base;
    if (!base) return;
    pararConexao();

    const ws = new WebSocket(base.replace(/^http/, 'ws') + '/ws');
    estado.ws = ws;
    let abriu = false;
    const desistir = setTimeout(() => { if (!abriu) ws.close(); }, CONFIG.TIMEOUT_PEDIDO_MS);

    ws.onopen = () => {
      if (estado.ws !== ws) return;
      abriu = true;
      clearTimeout(desistir);
      estado.conectado = true;
      estado.falhas = 0;
      $('msgIp').textContent = '';
      $('infoTopico').textContent = ws.url + ' (WebSocket)';
      atualizarStatus();
    };

    ws.onmessage = (ev) => {
      if (estado.ws !== ws) return;
      try {
        processarDados(JSON.parse(ev.data));
      } catch (e) {
        console.warn('Mensagem ignorada (JSON inválido):', ev.data);
      }
    };

    ws.onclose = () => {
      clearTimeout(desistir);
      if (estado.ws !== ws) return;   // conexão antiga (IP trocado ou saída)
      estado.ws = null;
      if (abriu) {
        // Caiu depois de funcionar: tenta de novo em instantes
        estado.conectado = false;
        atualizarStatus();
        estado.busca = setTimeout(conectarWebSocket, CONFIG.INTERVALO_MS * 2);
      } else {
        // ESP32 sem WebSocket (ou fora do ar): segue com pedidos em /dados
        $('infoTopico').textContent = base + '/dados (HTTP)';
        buscarDados();
      }
    };
  }

  function buscarDados() {
    const base = estado.base;
    if (!base || estado.ws) return;

    const controle = new AbortController();
    const limite = setTimeout(() => controle.abort(), CONFIG.TIMEOUT_PEDIDO_MS);

    fetch(base + '/dados', { cache: 'no-store', signal: controle.signal })
      .then((resp) => {
        if (!resp.ok) throw new Error('HTTP ' + resp.status);
        return resp.json();
      })
      .then((dados) => {
        if (estado.base !== base) return;      // o IP foi trocado nesse meio-tempo
        estado.conectado = true;
        estado.falhas = 0;
        $('msgIp').textContent = '';
        processarDados(dados);
      })
      .catch((erro) => {
        if (estado.base !== base) return;
        estado.falhas++;
        console.warn('ESP32: sem resposta de', base + '/dados', erro);
        if (estado.falhas >= CONFIG.FALHAS_PARA_DESCONECTAR) {
          estado.conectado = false;
          $('msgIp').textContent =
            `Sem resposta do ESP32 em ${base}. Confira o IP no display LCD e se este aparelho ` +
            'está na mesma rede Wi-Fi do torno. O painel continua tentando sozinho.';
        }
        atualizarStatus();
      })
      .finally(() => {
        clearTimeout(limite);
        if (estado.base === base) estado.busca = setTimeout(buscarDados, CONFIG.INTERVALO_MS);
      });
  }

  // Aceita "192.168.0.50", "http://192.168.0.50/", "192.168.0.50:8080" etc.
  function limparIp(texto) {
    return texto.trim().replace(/^[a-z]+:\/\//i, '').replace(/\/.*$/, '');
  }

  // Se o painel foi aberto pelo próprio ESP32 (http://<IP>/), o IP já é o endereço da página
  function ipDaPagina() {
    const h = location.hostname;
    if (location.protocol !== 'http:' || !h || h === 'localhost' || h.startsWith('127.')) return '';
    return location.host;
  }

  /* =====================================================================
     NAVEGAÇÃO E EVENTOS
     ===================================================================== */
  // Passo 1: login (operador, torno e tensão)
  function iniciar() {
    const operador = $('operador').value.trim();
    const torno = $('torno').value.trim();

    if (!operador || !torno) {
      $('msgLogin').textContent = 'Preencha o nome do operador e a identificação do torno.';
      return;
    }
    $('msgLogin').textContent = '';

    $('infoOperador').textContent = operador;
    $('infoTorno').textContent = torno;

    const tensao = $('tensao').value;
    CONFIG.CORRENTE = CONFIG.CORRENTE_POR_TENSAO[tensao];
    $('infoTensao').textContent = tensao + ' V';
    renderLimites();

    $('login').classList.add('hidden');
    $('app').classList.remove('hidden');

    renderizar();
    estado.timer = setInterval(atualizarStatus, 1000);
    atualizarStatus();

    // Painel aberto pelo próprio ESP32: conecta sozinho em ws://${location.host}/ws
    if (ipDaPagina() && !estado.base) {
      $('ipEsp').value = ipDaPagina();
      conectarIp();
    } else {
      $('ipEsp').focus();
    }
  }

  // Passo 2: IP do ESP32 (card "Conexão com o ESP32" no painel)
  function conectarIp() {
    const ip = limparIp($('ipEsp').value);
    if (!ip) {
      $('msgIp').textContent = 'Informe o IP do ESP32 (aparece no display LCD do torno).';
      return;
    }
    try { localStorage.setItem('lathevision-ip', ip); } catch (e) { /* ignora */ }

    // Página https (ex.: GitHub Pages): o navegador bloqueia pedidos para http://.
    // Em vez de mostrar erro, abre o painel que o próprio ESP32 entrega, já logado e conectado.
    if (location.protocol === 'https:') {
      const sessao = new URLSearchParams({
        op: $('operador').value.trim(),
        torno: $('torno').value.trim(),
        tensao: $('tensao').value,
        ip,
      });
      $('msgIp').textContent = 'Abrindo o painel direto no ESP32...';
      location.href = `http://${ip}/#${sessao}`;
      return;
    }

    // Se já havia uma conexão (troca de IP), fecha a do IP antigo
    pararConexao();
    estado.conectado = false;
    estado.falhas = 0;
    estado.base = 'http://' + ip;

    $('infoTopico').textContent = estado.base;
    $('msgIp').textContent = '';
    $('btnConectar').textContent = 'RECONECTAR';
    $('ipTitulo').textContent = '📡 CONEXÃO COM O ESP32';
    $('ipTitulo').classList.remove('aviso-ip');

    conectarWebSocket();
    atualizarStatus();
  }

  // Quando o painel https redireciona para o ESP32, a sessão vem no endereço (#op=...&ip=...)
  function retomarSessaoDoEndereco() {
    if (!location.hash.includes('ip=')) return;
    const p = new URLSearchParams(location.hash.slice(1));
    history.replaceState(null, '', location.pathname);   // limpa o endereço
    if (!p.get('op') || !p.get('torno')) return;

    $('operador').value = p.get('op');
    $('torno').value = p.get('torno');
    if (p.get('tensao')) $('tensao').value = p.get('tensao');
    iniciar();                     // já conecta se a página veio do ESP32
    if (!estado.base) {
      $('ipEsp').value = p.get('ip') || ipDaPagina();
      conectarIp();
    }
  }

  function mostrarView(view, botao) {
    ['painel', 'alertas', 'historico', 'relatorios', 'equipamentos', 'config'].forEach((id) => {
      $('view-' + id).classList.toggle('hidden', id !== view);
    });
    document.querySelectorAll('.nav-btn').forEach((b) => b.classList.toggle('ativo', b === botao));

    if (view === 'alertas') renderAlertas();
    if (view === 'historico') renderHistorico();
    if (view === 'relatorios') renderRelatorio();
    if (view === 'equipamentos') { $('msgZerar').textContent = ''; renderEquipamentos(); }

    window.scrollTo(0, 0);
  }

  function sair() {
    pararConexao();
    estado.base = '';              // para de buscar dados
    location.reload();
  }

  carregarOpcoes();
  // IP sugerido: o da própria página (painel aberto pelo ESP32) ou o último usado
  try { $('ipEsp').value = ipDaPagina() || localStorage.getItem('lathevision-ip') || ''; } catch (e) { /* ignora */ }
  ['cfgAuto', 'cfgSom', 'cfgPreditiva'].forEach((id) => $(id).addEventListener('change', salvarOpcoes));

  $('btnIniciar').addEventListener('click', () => {
    // O navegador só libera o som depois de um clique do usuário
    if (estado.opcoes.som) {
      try { audio = audio || new (window.AudioContext || window.webkitAudioContext)(); } catch (e) { /* ignora */ }
    }
    iniciar();
  });
  $('torno').addEventListener('keydown', (e) => { if (e.key === 'Enter') iniciar(); });
  $('btnConectar').addEventListener('click', conectarIp);
  $('btnCsv').addEventListener('click', baixarCsv);
  $('btnZerar').addEventListener('click', zerarVoltas);
  $('ipEsp').addEventListener('keydown', (e) => { if (e.key === 'Enter') conectarIp(); });

  document.querySelectorAll('.nav-btn').forEach((btn) => {
    btn.addEventListener('click', () => {
      if (btn.dataset.view === 'sair') return sair();
      mostrarView(btn.dataset.view, btn);
    });
  });

  retomarSessaoDoEndereco();

  // Painel aberto pelo próprio ESP32 (http://IP/): já abre o WebSocket em
  // ws://${location.host}/ws ao carregar a página, mesmo antes do login.
  // Assim, depois do login, os gráficos já aparecem com dados reais.
  if (ipDaPagina() && !estado.base) {
    $('ipEsp').value = ipDaPagina();
    conectarIp();
  }
  </script>
</body>
</html>

)lvpainel";
// <<< FIM DO PAINEL
