// ==========================================================
// CONFIGURAÇÃO DO PAINEL LATHEVISION
// Para ajustar limites ou trocar o broker, edite apenas este arquivo.
// ==========================================================
window.LV_CONFIG = {
  // Broker MQTT por WebSocket seguro (wss). O GitHub Pages usa HTTPS,
  // então o navegador só aceita conexões wss://, nunca ws://.
  broker: 'wss://broker.hivemq.com:8884/mqtt',

  // Os tópicos seguem o formato: <topicoBase>/<identificação>/telemetria
  // e <topicoBase>/<identificação>/status. O firmware publica nos mesmos tópicos.
  topicoBase: 'torno',

  // Sem telemetria por esse tempo (em ms), o painel mostra "SEM DADOS"
  timeoutMs: 5000,

  // Quantos pontos aparecem nos gráficos de linha
  historicoPontos: 60,

  // Corrente (A). Motor de ~1,5 a 2 cv, com nominal de 8 A:
  // alerta em 90% do nominal e crítico em 110%.
  corrente: { alerta: 7.2, critico: 8.8, escalaMax: 12 },

  // Nível de óleo (%). Quanto menor, pior. Valores sugeridos, ajuste à sua necessidade.
  oleo: { alerta: 30, critico: 15 },

  // Velocidade (RPM). Valores sugeridos, ajuste ao torno.
  rpm: { max: 2000, alerta: 1500, critico: 1800 },

  // Vibração (g RMS). Limites definidos para o motor.
  vibracao: { alerta: 0.7, critico: 1.2, escalaMax: 2 }
};
