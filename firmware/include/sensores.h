// ==========================================================
// Interface dos sensores do torno.
// ==========================================================
#pragma once

// Todas as leituras de uma vez.
// Quando um sensor não responde, o valor fica em -1 (o painel mostra "--").
struct Leituras {
  float correnteA;     // SCT-013: corrente em amperes (RMS)
  float vibracaoG;     // MPU6050: vibração total em g (RMS, sem a gravidade)
  float rpm;           // encoder: rotações por minuto
  float distanciaCm;   // HC-SR04: distância até a superfície do óleo, em cm
  float oleoPct;       // nível de óleo calculado, em %
};

// Inicializa pinos, interrupção do encoder, I2C e o acelerômetro.
void setupSensores();

// Lê todos os sensores. Leva cerca de 0,7 s, por causa das amostragens.
Leituras lerSensores();
