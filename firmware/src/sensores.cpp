// ==========================================================
// Leitura dos sensores do torno.
// Cada bloco abaixo cuida de um sensor e pode ser testado sozinho.
// ==========================================================
#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include "config.h"
#include "sensores.h"

// ==========================================================
// ENCODER (sensor de velocidade)
// A cada borda de subida do CANAL A, o CANAL B diz o sentido:
// B em nível baixo = um sentido, B em nível alto = o outro.
// A interrupção só soma (ou subtrai) os pulsos; o RPM é calculado
// na leitura, usando o tempo decorrido entre uma leitura e outra.
// ==========================================================
static volatile long pulsosEncoder = 0;
static unsigned long ultimaLeituraRpmMs = 0;

static void IRAM_ATTR isrEncoder() {
  if (digitalRead(PIN_ENCODER_B)) pulsosEncoder--;
  else pulsosEncoder++;
}

static float lerRpm(int &sentido) {
  // Lê e zera o contador sem que a interrupção atrapalhe
  noInterrupts();
  long pulsos = pulsosEncoder;
  pulsosEncoder = 0;
  interrupts();

  sentido = pulsos > 0 ? 1 : (pulsos < 0 ? -1 : 0);

  unsigned long agora = millis();
  float intervaloS = (agora - ultimaLeituraRpmMs) / 1000.0f;
  ultimaLeituraRpmMs = agora;
  if (intervaloS <= 0.0f) return 0.0f;

  float voltas = fabsf((float)pulsos) / ENCODER_PULSOS_POR_VOLTA;
  return voltas * 60.0f / intervaloS;   // voltas por minuto
}

// ==========================================================
// MAX6675 + termopar K (temperatura)
// SPI feito "na mão" nos pinos da placa (SO, CS, SCK).
// O chip devolve 16 bits: bits 14..3 = temperatura em passos de 0,25 °C,
// bit 2 = termopar desconectado. Precisa de ~220 ms entre conversões.
// ==========================================================
static float lerTemperaturaC() {
  digitalWrite(PIN_MAX_CS, LOW);       // para a conversão e libera a leitura
  delayMicroseconds(10);

  uint16_t valor = 0;
  for (int i = 15; i >= 0; i--) {
    digitalWrite(PIN_MAX_SCK, HIGH);
    delayMicroseconds(10);
    if (digitalRead(PIN_MAX_SO)) valor |= (1 << i);
    digitalWrite(PIN_MAX_SCK, LOW);
    delayMicroseconds(10);
  }

  digitalWrite(PIN_MAX_CS, HIGH);      // inicia a próxima conversão

  if (valor & 0x0004) return -1.0f;    // termopar aberto/desconectado
  if (valor == 0xFFFF) return -1.0f;   // chip não respondeu (SO sempre alto)
  return (valor >> 3) * 0.25f;
}

// ==========================================================
// MPU6050 (vibração)
// Lê a aceleração nos três eixos e calcula a vibração RMS,
// retirando a média (que inclui a gravidade e qualquer inclinação).
// ==========================================================
static const uint8_t MPU_ADDR = 0x68;
static const float LSB_POR_G = 16384.0f;   // escala configurada: ±2 g

static void mpuEscreve(uint8_t registro, uint8_t valor) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(registro);
  Wire.write(valor);
  Wire.endTransmission();
}

static bool mpuLeAceleracao(float &ax, float &ay, float &az) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B);   // registrador ACCEL_XOUT_H (primeiro dos seis bytes de aceleração)
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(MPU_ADDR, (uint8_t)6) != 6) return false;

  int16_t brutos[3];
  for (int i = 0; i < 3; i++) {
    uint8_t alto = Wire.read();    // leitura sequencial: a ordem importa
    uint8_t baixo = Wire.read();
    brutos[i] = (int16_t)((alto << 8) | baixo);
  }
  ax = brutos[0] / LSB_POR_G;
  ay = brutos[1] / LSB_POR_G;
  az = brutos[2] / LSB_POR_G;
  return true;
}

// Variância de um conjunto de amostras em torno da própria média.
// A raiz dela é o RMS da oscilação, sem o valor médio.
static float varianciaSemMedia(const float *v, int n) {
  float media = 0;
  for (int i = 0; i < n; i++) media += v[i];
  media /= n;

  float soma = 0;
  for (int i = 0; i < n; i++) {
    float d = v[i] - media;
    soma += d * d;
  }
  return soma / n;
}

static float lerVibracaoG() {
  // Amostra durante ~200 ms (cerca de 250 a 300 amostras por eixo)
  const int N = 400;
  static float ax[N], ay[N], az[N];
  int n = 0;

  unsigned long inicio = millis();
  while (millis() - inicio < 200 && n < N) {
    float x, y, z;
    if (mpuLeAceleracao(x, y, z)) {
      ax[n] = x;
      ay[n] = y;
      az[n] = z;
      n++;
    }
    delayMicroseconds(500);
  }

  if (n < 20) return -1.0f;   // sensor não respondeu

  // Vibração total: soma das variâncias dos três eixos, e depois a raiz
  return sqrtf(varianciaSemMedia(ax, n) + varianciaSemMedia(ay, n) + varianciaSemMedia(az, n));
}

// ==========================================================
// SCT-013 (corrente)
// Amostra o ADC durante ~200 ms (cerca de 12 ciclos de 60 Hz),
// calcula o RMS sem o offset do circuito de bias e converte para amperes.
// analogReadMilliVolts já devolve milivolts calibrados pelo ESP32,
// usando a atenuação padrão de 11 dB (faixa de ~0 a 3,1 V).
// ==========================================================
static float lerCorrenteA() {
  static uint16_t amostras[2000];
  int n = 0;

  unsigned long inicioUs = micros();
  while (micros() - inicioUs < 200000UL && n < 2000) {
    amostras[n++] = analogReadMilliVolts(PIN_CORRENTE);
  }

  double media = 0;
  for (int i = 0; i < n; i++) media += amostras[i];
  media /= n;

  double soma = 0;
  for (int i = 0; i < n; i++) {
    double d = amostras[i] - media;
    soma += d * d;
  }

  double vrms = sqrt(soma / n) / 1000.0;   // mV -> V
  return (float)(vrms * SCT_FATOR_A_POR_V);
}

// ==========================================================
// HC-SR04 (nível de óleo)
// Faz 5 medições, descarta as que não tiveram eco e tira a média.
// ==========================================================
static float lerDistanciaCm() {
  float soma = 0;
  int validas = 0;

  for (int i = 0; i < 5; i++) {
    digitalWrite(PIN_HC_TRIG, LOW);
    delayMicroseconds(2);
    digitalWrite(PIN_HC_TRIG, HIGH);   // pulso de disparo de 10 µs
    delayMicroseconds(10);
    digitalWrite(PIN_HC_TRIG, LOW);

    // Timeout de 30 ms (alcance de ~5 m). Sem eco, pulseIn devolve 0.
    unsigned long duracaoUs = pulseIn(PIN_HC_ECHO, HIGH, 30000UL);
    if (duracaoUs > 0) {
      // O som percorre 0,0343 cm/µs; como é ida e volta, divide por 2
      soma += duracaoUs * 0.0343f / 2.0f;
      validas++;
    }
    delay(40);   // deixa o eco anterior desaparecer
  }

  return validas > 0 ? soma / validas : -1.0f;
}

static float calculaNivelOleo(float distanciaCm) {
  if (distanciaCm < 0) return -1.0f;

  // Interpola entre tanque vazio (0 %) e cheio (100 %)
  float nivel = (TANQUE_DIST_VAZIO_CM - distanciaCm) /
                (TANQUE_DIST_VAZIO_CM - TANQUE_DIST_CHEIO_CM) * 100.0f;
  return constrain(nivel, 0.0f, 100.0f);
}

// ==========================================================
// Funções públicas
// ==========================================================
void setupSensores() {
  // SCT-013 (ADC): a atenuação padrão já é de 11 dB, adequada para a saída com bias de 1,65 V
  pinMode(PIN_CORRENTE, INPUT);

  // HC-SR04
  pinMode(PIN_HC_TRIG, OUTPUT);
  pinMode(PIN_HC_ECHO, INPUT);

  // Encoder: conta a borda de subida do CANAL A e lê o CANAL B para o sentido
  pinMode(PIN_ENCODER_A, INPUT_PULLUP);
  pinMode(PIN_ENCODER_B, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_ENCODER_A), isrEncoder, RISING);
  ultimaLeituraRpmMs = millis();

  // MAX6675: CS em nível alto deixa o chip convertendo
  pinMode(PIN_MAX_CS, OUTPUT);
  pinMode(PIN_MAX_SCK, OUTPUT);
  pinMode(PIN_MAX_SO, INPUT);
  digitalWrite(PIN_MAX_CS, HIGH);
  digitalWrite(PIN_MAX_SCK, LOW);

  // MPU6050: I2C a 400 kHz, acorda o sensor e configura ±2 g
  Wire.begin(PIN_MPU_SDA, PIN_MPU_SCL);
  Wire.setClock(400000);
  mpuEscreve(0x6B, 0x00);   // PWR_MGMT_1: sai do modo de espera
  mpuEscreve(0x1C, 0x00);   // ACCEL_CONFIG: ±2 g

  Serial.println("[sensores] iniciados");
}

Leituras lerSensores() {
  Leituras l;
  l.correnteA = lerCorrenteA();
  l.vibracaoG = lerVibracaoG();
  l.distanciaCm = lerDistanciaCm();
  l.oleoPct = calculaNivelOleo(l.distanciaCm);
  l.temperaturaC = lerTemperaturaC();
  l.rpm = lerRpm(l.sentido);
  return l;
}
