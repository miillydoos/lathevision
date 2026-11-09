/* ==========================================================
   LATHEVISION - lógica do painel
   - login (nome do operador + identificação do torno)
   - conexão MQTT por WebSocket seguro (wss)
   - atualização dos cards, gráficos, medidores e alertas
   ========================================================== */
(function () {
  'use strict';

  const CFG = window.LV_CONFIG;
  const CHAVE_SESSAO = 'lathevision_sessao';
  const ABAS_EM_BREVE = ['historico', 'alertas', 'relatorios', 'equipamentos', 'configuracoes'];
  const ROTULOS = {
    normal: '✓ NORMAL',
    alerta: '⚠ ALERTA',
    critico: '✖ CRÍTICO',
    sem: 'SEM LEITURA',
  };
  const ESTADOS = {
    operacional:  { texto: 'OPERACIONAL',   cor: 'verde' },
    'sem-dados':  { texto: 'SEM DADOS',     cor: 'amarelo' },
    offline:      { texto: 'TORNO OFFLINE', cor: 'amarelo' },
    desconectado: { texto: 'DESCONECTADO',  cor: 'vermelho' },
  };

  // Geometria do medidor de velocidade (graus, sentido horário, 0° = direita)
  const MEDIDOR_INICIO = 135;      // canto inferior esquerdo
  const MEDIDOR_VARREDURA = 270;   // até o canto inferior direito

  const $ = (id) => document.getElementById(id);

  let cliente = null;
  let ultimaTelemetria = 0;
  let estado = 'desconectado';
  let timerToast = null;
  const historico = { corrente: [], vibracao: [] };

  // ==========================================================
  // Utilidades
  // ==========================================================

  // "Torno Romi 01" -> "torno-romi-01" (o mesmo formato usado no firmware)
  function slugify(texto) {
    return texto
      .normalize('NFD').replace(/[\u0300-\u036f]/g, '')   // tira acentos
      .toLowerCase().trim()
      .replace(/[^a-z0-9]+/g, '-')
      .replace(/^-+|-+$/g, '');
  }

  function formata(valor, casas) {
    if (typeof valor !== 'number' || valor < 0) return '--';
    return valor.toFixed(casas);
  }

  // Classifica um valor como normal, alerta, crítico ou sem leitura.
  // "inverso" = quanto menor, pior (usado no nível de óleo).
  function classifica(valor, alerta, critico, inverso) {
    if (typeof valor !== 'number' || valor < 0) return 'sem';
    if (inverso) {
      if (valor <= critico) return 'critico';
      if (valor <= alerta) return 'alerta';
      return 'normal';
    }
    if (valor >= critico) return 'critico';
    if (valor >= alerta) return 'alerta';
    return 'normal';
  }

  function aplicaBadge(id, nivel) {
    const el = $(id);
    el.className = 'badge ' + nivel;
    el.textContent = ROTULOS[nivel];
  }

  function defineEstado(chave) {
    estado = chave;
    const { texto, cor } = ESTADOS[chave];
    $('status-texto').textContent = texto;
    $('status-texto').className = 'status-texto ' + cor;
    $('ponto').className = 'ponto ' + cor;
  }

  function mostraToast(texto) {
    const t = $('toast');
    t.textContent = texto;
    t.classList.add('visivel');
    clearTimeout(timerToast);
    timerToast = setTimeout(() => t.classList.remove('visivel'), 2200);
  }

  // ==========================================================
  // Desenho dos gráficos (SVG feito à mão, sem bibliotecas extras)
  // ==========================================================

  function polar(cx, cy, raio, grau) {
    const rad = (grau * Math.PI) / 180;
    return [cx + raio * Math.cos(rad), cy + raio * Math.sin(rad)];
  }

  function arco(cx, cy, raio, grauIni, grauFim) {
    const [x1, y1] = polar(cx, cy, raio, grauIni);
    const [x2, y2] = polar(cx, cy, raio, grauFim);
    const grande = grauFim - grauIni > 180 ? 1 : 0;
    return `M${x1.toFixed(2)} ${y1.toFixed(2)} A${raio} ${raio} 0 ${grande} 1 ${x2.toFixed(2)} ${y2.toFixed(2)}`;
  }

  // Medidor semicircular com zonas verde, amarela e vermelha
  function desenhaMedidor(svg, valor, cfg) {
    const cx = 150, cy = 150, raio = 115;
    const grauDe = (v) =>
      MEDIDOR_INICIO + Math.min(Math.max(v / cfg.max, 0), 1) * MEDIDOR_VARREDURA;

    const zonas = [
      [0, cfg.alerta, '#6bd67a'],
      [cfg.alerta, cfg.critico, '#e8c24a'],
      [cfg.critico, cfg.max, '#e0413a'],
    ];

    let s = `<path d="${arco(cx, cy, raio, MEDIDOR_INICIO, MEDIDOR_INICIO + MEDIDOR_VARREDURA)}"
                   fill="none" stroke="#2a0a08" stroke-width="22"/>`;
    for (const [de, ate, cor] of zonas) {
      s += `<path d="${arco(cx, cy, raio, grauDe(de), grauDe(ate))}"
                   fill="none" stroke="${cor}" stroke-width="22"/>`;
    }

    if (valor >= 0) {
      const [px, py] = polar(cx, cy, raio - 28, grauDe(valor));
      s += `<line x1="${cx}" y1="${cy}" x2="${px.toFixed(1)}" y2="${py.toFixed(1)}"
                  stroke="#e0413a" stroke-width="5" stroke-linecap="round"/>`;
    }
    s += `<circle cx="${cx}" cy="${cy}" r="11" fill="#e0413a"/>`;
    s += `<text x="${cx}" y="195" text-anchor="middle" class="svg-valor">${valor >= 0 ? Math.round(valor) : '--'}</text>`;
    s += `<text x="${cx}" y="222" text-anchor="middle" class="svg-rotulo">RPM</text>`;
    svg.innerHTML = s;
  }

  // Anel de porcentagem (nível de óleo)
  function desenhaRosca(svg, pct, nivel) {
    const raio = 110;
    const circ = 2 * Math.PI * raio;
    const cor = { normal: '#d93a32', alerta: '#e8c24a', critico: '#ff6b63', sem: '#5a4f4f' }[nivel];
    const p = pct >= 0 ? Math.min(pct, 100) : 0;

    svg.innerHTML = `
      <circle cx="150" cy="150" r="${raio}" fill="none" stroke="#2e0a08" stroke-width="34"/>
      <circle cx="150" cy="150" r="${raio}" fill="none" stroke="${cor}" stroke-width="34"
              stroke-dasharray="${circ.toFixed(2)}"
              stroke-dashoffset="${(circ * (1 - p / 100)).toFixed(2)}"
              transform="rotate(-90 150 150)"/>
      <text x="150" y="162" text-anchor="middle" class="svg-valor">${pct >= 0 ? Math.round(pct) + '%' : '--'}</text>
      <text x="150" y="196" text-anchor="middle" class="svg-rotulo">NÍVEL DE ÓLEO</text>`;
  }

  // Gráfico de linha em tempo real. Os pontos entram pela direita e vão para a esquerda.
  function desenhaLinha(svg, valores, min, max) {
    const W = 300, H = 140, margemX = 10, margemY = 12, n = CFG.historicoPontos;
    const passo = (W - 2 * margemX) / (n - 1);

    let s = '';
    for (let i = 0; i <= 4; i++) {
      const y = margemY + (i * (H - 2 * margemY)) / 4;
      s += `<line class="grade" x1="0" y1="${y}" x2="${W}" y2="${y}"/>`;
    }

    if (valores.length > 0) {
      const pontos = valores.map((v, i) => {
        const x = W - margemX - (valores.length - 1 - i) * passo;
        const t = Math.min(Math.max((v - min) / (max - min), 0), 1);
        const y = H - margemY - t * (H - 2 * margemY);
        return [x, y];
      });
      s += `<polyline class="linha" points="${pontos.map(([x, y]) => `${x.toFixed(1)},${y.toFixed(1)}`).join(' ')}"/>`;
      s += pontos.map(([x, y]) =>
        `<circle class="ponto-linha" cx="${x.toFixed(1)}" cy="${y.toFixed(1)}" r="4"/>`
      ).join('');
    }
    svg.innerHTML = s;
  }

  function registra(lista, valor) {
    if (typeof valor !== 'number' || valor < 0) return;
    lista.push(valor);
    if (lista.length > CFG.historicoPontos) lista.shift();
  }

  // Estado inicial dos cards, antes de chegar a primeira leitura
  function renderizaVazio() {
    desenhaLinha($('grafico-corrente'), [], 0, CFG.corrente.escalaMax);
    desenhaLinha($('grafico-vibracao'), [], 0, CFG.vibracao.escalaMax);
    desenhaRosca($('rosca-oleo'), -1, 'sem');
    desenhaMedidor($('medidor-rpm'), -1, CFG.rpm);
    ['badge-corrente', 'badge-oleo', 'badge-rpm', 'badge-vibracao']
      .forEach((id) => aplicaBadge(id, 'sem'));
  }

  // ==========================================================
  // Atualização com cada telemetria recebida
  // ==========================================================
  function atualizaPainel(d) {
    ultimaTelemetria = Date.now();
    defineEstado('operacional');

    const c = CFG.corrente, o = CFG.oleo, r = CFG.rpm, v = CFG.vibracao;

    // Corrente
    $('val-corrente').textContent = formata(d.corrente_a, 1);
    aplicaBadge('badge-corrente', classifica(d.corrente_a, c.alerta, c.critico));
    registra(historico.corrente, d.corrente_a);
    desenhaLinha($('grafico-corrente'), historico.corrente, 0, c.escalaMax);

    // Nível de óleo
    const nivelOleo = classifica(d.oleo_pct, o.alerta, o.critico, true);
    $('val-oleo').textContent = formata(d.oleo_pct, 0);
    aplicaBadge('badge-oleo', nivelOleo);
    desenhaRosca($('rosca-oleo'), d.oleo_pct, nivelOleo);

    // Velocidade
    $('val-rpm').textContent = formata(d.rpm, 0);
    aplicaBadge('badge-rpm', classifica(d.rpm, r.alerta, r.critico));
    desenhaMedidor($('medidor-rpm'), d.rpm, r);

    // Vibração
    $('val-vibracao').textContent = formata(d.vibracao_g, 2);
    aplicaBadge('badge-vibracao', classifica(d.vibracao_g, v.alerta, v.critico));
    registra(historico.vibracao, d.vibracao_g);
    desenhaLinha($('grafico-vibracao'), historico.vibracao, 0, v.escalaMax);

    // Sinal Wi-Fi do ESP32
    if (typeof d.rssi === 'number') $('info-rssi').textContent = `${d.rssi} dBm`;
  }

  // ==========================================================
  // Conexão MQTT
  // ==========================================================
  function conecta(torno) {
    if (typeof mqtt === 'undefined') {
      mostraToast('Biblioteca MQTT não carregou. Verifique a internet.');
      return;
    }

    const slug = slugify(torno);
    const topicoTelemetria = `${CFG.topicoBase}/${slug}/telemetria`;
    const topicoStatus = `${CFG.topicoBase}/${slug}/status`;

    if (cliente) cliente.end(true);
    defineEstado('desconectado');

    const este = mqtt.connect(CFG.broker, {
      clientId: 'painel-' + Math.random().toString(16).slice(2, 10),
      clean: true,
      connectTimeout: 10000,
      reconnectPeriod: 3000,
    });
    cliente = este;

    este.on('connect', () => {
      if (cliente !== este) return;
      este.subscribe([topicoTelemetria, topicoStatus]);
      defineEstado('sem-dados');   // até a primeira leitura chegar
    });

    este.on('close', () => { if (cliente === este) defineEstado('desconectado'); });
    este.on('offline', () => { if (cliente === este) defineEstado('desconectado'); });
    este.on('error', (err) => console.warn('[LatheVision] MQTT:', err && err.message));

    este.on('message', (topico, mensagem) => {
      if (cliente !== este) return;
      const texto = mensagem.toString();

      if (topico === topicoTelemetria) {
        try {
          atualizaPainel(JSON.parse(texto));
        } catch (err) {
          console.warn('[LatheVision] Telemetria inválida:', texto);
        }
      } else if (topico === topicoStatus) {
        if (texto === 'offline') defineEstado('offline');
        if (texto === 'online' && estado !== 'operacional') defineEstado('sem-dados');
      }
    });
  }

  // Se a telemetria parar por mais tempo que o limite, avisa "SEM DADOS"
  setInterval(() => {
    if (estado === 'operacional' && Date.now() - ultimaTelemetria > CFG.timeoutMs) {
      defineEstado('sem-dados');
    }
  }, 1000);

  // ==========================================================
  // Telas: login e painel
  // ==========================================================
  function mostraPainel(sessao) {
    $('tela-login').classList.add('hidden');
    $('tela-painel').classList.remove('hidden');
    $('info-operador').textContent = sessao.operador;
    $('info-torno').textContent = sessao.torno;
    conecta(sessao.torno);
  }

  function mostraLogin() {
    if (cliente) {
      cliente.end(true);
      cliente = null;
    }
    historico.corrente = [];
    historico.vibracao = [];
    renderizaVazio();
    defineEstado('desconectado');
    $('tela-painel').classList.add('hidden');
    $('tela-login').classList.remove('hidden');
  }

  $('form-login').addEventListener('submit', (ev) => {
    ev.preventDefault();
    const operador = $('operador').value.trim();
    const torno = $('torno').value.trim();

    if (!operador || !slugify(torno)) {
      $('erro-login').textContent = 'Preencha o nome do operador e a identificação do torno.';
      return;
    }

    $('erro-login').textContent = '';
    const sessao = { operador, torno };
    sessionStorage.setItem(CHAVE_SESSAO, JSON.stringify(sessao));
    mostraPainel(sessao);
  });

  // Menu inferior: "Sair" volta ao login; as abas futuras mostram um aviso
  document.querySelectorAll('.menu button').forEach((botao) => {
    botao.addEventListener('click', () => {
      const aba = botao.dataset.aba;
      if (aba === 'sair') {
        sessionStorage.removeItem(CHAVE_SESSAO);
        mostraLogin();
      } else if (ABAS_EM_BREVE.includes(aba)) {
        mostraToast('Esta aba ainda será implementada.');
      }
    });
  });

  // ==========================================================
  // Início
  // ==========================================================
  renderizaVazio();
  defineEstado('desconectado');

  // Se a página foi recarregada no meio do monitoramento, retoma a sessão
  const salva = sessionStorage.getItem(CHAVE_SESSAO);
  if (salva) {
    try {
      mostraPainel(JSON.parse(salva));
    } catch (err) {
      sessionStorage.removeItem(CHAVE_SESSAO);
    }
  }
})();
