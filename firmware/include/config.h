// ==========================================================
// CONFIGURAÇÃO DO LATHEVISION
// Pinos da ESP32-C3 Mini, conforme a placa acrílica de conexões.
// Tudo que depende da montagem fica aqui.
// ==========================================================
#pragma once

#include "secrets.h"   // WIFI_SSID e WIFI_PASSWORD (crie a partir de secrets.example.h)

// ---------- Identificação do torno ----------
// Deve ser IGUAL ao campo "Identificação do torno" do painel, em formato de slug:
// minúsculas, sem acentos e com hífen no lugar dos espaços.
// Ex.: "Torno Romi 01" no painel  ->  "torno-romi-01" aqui.
#define TORNO_ID "torno-romi-01"

// ---------- Broker MQTT (conexão TCP, usada pelo ESP32) ----------
// O painel web usa a mesma rede, mas por WebSocket seguro (porta 8884).
#define MQTT_SERVER "broker.hivemq.com"
#define MQTT_PORT   1883

// ---------- Pinos (ESP32-C3 Mini) ----------
#define PIN_CORRENTE   4    // SCT-013: saída do circuito de bias (ADC1, canal 4)
#define PIN_MPU_SDA    6    // I2C: MPU6050 de vibração (o display LCD I2C usa o mesmo barramento)
#define PIN_MPU_SCL    5    // I2C: idem
#define PIN_ENCODER_A 20    // encoder (sensor de velocidade): CANAL A, conta os pulsos
#define PIN_ENCODER_B 21    // encoder: CANAL B, indica o sentido de rotação
#define PIN_HC_TRIG   10    // HC-SR04: TRIG
#define PIN_HC_ECHO    7    // HC-SR04: ECHO (precisa de divisor de tensão, o eco sai em 5 V)
#define PIN_MAX_SO     0    // MAX6675 (termopar K): SO / MISO (SPI)
#define PIN_MAX_CS     1    // MAX6675: CS (SPI)
#define PIN_MAX_SCK    3    // MAX6675: SCK (SPI)

// ---------- Calibração (ajuste com medição real) ----------
// Corrente: amperes por volt RMS medido no ADC. 30.0 vale para SCT-013-030 (30 A = 1 V RMS).
#define SCT_FATOR_A_POR_V        30.0f
// Velocidade: pulsos por volta do eixo. Com uma engrenagem de 20 dentes e 1 pulso por dente, use 20.
#define ENCODER_PULSOS_POR_VOLTA  1
// Óleo: distância (cm) do sensor até a superfície do óleo com o tanque cheio e vazio.
#define TANQUE_DIST_CHEIO_CM      5.0f
#define TANQUE_DIST_VAZIO_CM     25.0f

// ---------- Publicação ----------
#define PUBLICA_INTERVALO_MS  1000   // envia uma leitura completa a cada 1 segundo
