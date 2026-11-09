# LatheVision

Monitoramento em tempo real de um torno (SENAI, Romi). Uma ESP32-C3 Mini lê os sensores, publica os dados por MQTT, e um painel web, hospedado no GitHub Pages, mostra tudo no celular ou no computador.

```
Sensores ──► ESP32-C3 Mini (firmware/) ──► MQTT (broker HiveMQ) ──► Painel web (docs/)
```

Sensores ativos nesta versão: corrente elétrica (SCT-013), nível de óleo (HC-SR04), velocidade (encoder) e vibração (acelerômetro I2C). O sensor de temperatura não faz parte desta versão.

## Estrutura do projeto

```
lathevision/
├── README.md
├── .gitignore                ← impede o envio da senha do Wi-Fi ao GitHub
├── .vscode/
│   └── extensions.json       ← sugere a extensão PlatformIO ao abrir a pasta
├── firmware/                 ← código da ESP32-C3 (abrir esta pasta no VS Code)
│   ├── platformio.ini        ← placa, USB, bibliotecas e velocidade de upload
│   ├── include/
│   │   ├── config.h          ← pinos, calibrações e ID do torno
│   │   ├── secrets.example.h ← modelo das credenciais de Wi-Fi
│   │   └── sensores.h        ← interface dos sensores
│   └── src/
│       ├── main.cpp          ← Wi-Fi, MQTT e publicação a cada 1 s
│       └── sensores.cpp      ← leitura de cada sensor
└── docs/                     ← painel web (o GitHub Pages publica esta pasta)
    ├── index.html            ← login, cards e menu inferior
    ├── css/style.css         ← visual vermelho da interface
    └── js/
        ├── config.js         ← broker e limites de alerta
        └── app.js            ← conexão MQTT, gráficos e medidores
```

## 1. Ligações (pinagem da ESP32-C3 Mini)

| Medição | Sensor | Pino no ESP32-C3 | Observações |
|---|---|---|---|
| Corrente | SCT-013 | GPIO 4 (ADC) | A saída precisa estar centrada em 1,65 V (circuito de bias). Nunca pode passar de 3,3 V. |
| Vibração | Acelerômetro I2C (MPU6050) | SDA 6 · SCL 5 | Alimentação de 3,3 V, endereço 0x68. O display LCD I2C usa o mesmo barramento, sem conflito porque os endereços são diferentes. |
| Velocidade | Encoder | CANAL A = GPIO 20 | O CANAL B (GPIO 21) fica reservado para o sentido de rotação. Se o encoder sair em 5 V, use divisor de tensão. |
| Nível de óleo | HC-SR04 | TRIG 10 · ECHO 7 | Alimentação de 5 V. O ECHO sai em 5 V, então use um divisor de tensão (1 kΩ + 2 kΩ) antes do GPIO 7. |

Pinos livres: GPIO 0, 1 e 3 (eram do MAX6675, que saiu desta versão) e os demais que não aparecem na tabela.

Ligue todos os GND juntos (ESP32-C3, sensores e fonte).

## 2. Firmware no VS Code (PlatformIO)

1. Instale o VS Code e, na aba de extensões, a **PlatformIO IDE**. O arquivo `.vscode/extensions.json` já sugere a instalação.
2. Abra a pasta `firmware/` (File → Open Folder). Na primeira vez, o PlatformIO baixa a placa e as bibliotecas sozinho.
3. Crie o arquivo `firmware/include/secrets.h` copiando `secrets.example.h` e preencha o nome e a senha do Wi-Fi. Esse arquivo fica fora do GitHub de propósito.
4. Em `firmware/include/config.h`, ajuste `TORNO_ID` e as calibrações (seção 6).
5. Conecte a placa por USB e clique em **Upload** (ícone de seta na barra inferior do VS Code). Se o upload falhar, veja o modo de gravação na seção 10.
6. Abra o **Monitor Serial** (ícone de tomada) em 115200 baud. Deve aparecer um JSON por segundo, parecido com este:

```json
{"corrente_a":7.42,"vibracao_g":0.318,"rpm":1398,"distancia_cm":14.2,"oleo_pct":72.4,"rssi":-58,"uptime_s":3621}
```

## 3. Painel web

**Para testar no computador:** abra `docs/index.html` no navegador, ou instale a extensão *Live Server* e clique em "Go Live". O painel precisa de internet para conectar ao broker.

**Para publicar no GitHub Pages:**

1. Suba o projeto para um repositório no GitHub (seção 8).
2. Vá em **Settings → Pages**, em *Build and deployment*, escolha **Deploy from a branch**, branch `main` e pasta **`/docs`**. Clique em Save.
3. Depois de alguns minutos, o painel fica disponível em `https://<seu-usuario>.github.io/<nome-do-repositorio>/`.

## 4. Como o painel encontra o torno

Na tela de login, o campo "Identificação do torno" é convertido em um identificador de tópico: tudo em minúsculas, sem acentos, e espaços viram hífen.

| Você digita | O painel usa |
|---|---|
| Torno Romi 01 | `torno-romi-01` |
| Qi | `qi` |

Esse identificador precisa ser **igual** ao `TORNO_ID` do firmware. Se não bater, o painel mostra "SEM DADOS".

## 5. Dados e tópicos MQTT

| Tópico | Quem publica | Conteúdo |
|---|---|---|
| `torno/<id>/telemetria` | ESP32-C3, a cada 1 segundo | JSON com todas as leituras |
| `torno/<id>/status` | ESP32-C3 | `online` ao conectar. Se a placa cair, o próprio broker publica `offline` (testamento MQTT). |

Campos do JSON: `corrente_a` (A), `vibracao_g` (g), `rpm`, `distancia_cm` (cm), `oleo_pct` (%), `rssi` (dBm do Wi-Fi) e `uptime_s` (segundos desde que ligou). O valor `-1` significa "sem leitura" e aparece como `--` no painel.

## 6. Calibração

Os valores de fábrica são pontos de partida. Para ficar preciso, calibre cada sensor:

- **Corrente:** o SCT-013 existe em vários modelos, e a relação entre corrente e tensão depende do resistor de carga. Meça com um amperímetro de referência, com uma carga conhecida. Se o painel mostra 6 A e a referência marca 8 A, multiplique `SCT_FATOR_A_POR_V` por 8 / 6.
- **Velocidade:** `ENCODER_PULSOS_POR_VOLTA` é o número de pulsos do CANAL A por volta do eixo. Com uma engrenagem de 20 dentes e um pulso por dente, use `20`.
- **Vibração:** é o RMS da aceleração em g, sem a gravidade. Com o motor parado, deve ficar perto de zero. Comparações só fazem sentido com o sensor sempre fixado no mesmo lugar.
- **Nível de óleo:** com o tanque cheio, meça a distância do sensor até o óleo e coloque em `TANQUE_DIST_CHEIO_CM`. Faça o mesmo com o tanque vazio em `TANQUE_DIST_VAZIO_CM`.

## 7. Alertas

Os limites ficam em `docs/js/config.js`. Cada card mostra NORMAL, ALERTA ou CRÍTICO conforme o valor.

| Medida | Alerta | Crítico | Origem |
|---|---|---|---|
| Corrente | ≥ 7,2 A | ≥ 8,8 A | 90% e 110% de 8 A (nominal do motor) |
| Vibração | ≥ 0,7 g | ≥ 1,2 g | Limites definidos para o motor |
| Nível de óleo | ≤ 30% | ≤ 15% | Sugestão inicial (quanto menor, pior) |
| Velocidade | ≥ 1500 RPM | ≥ 1800 RPM | Sugestão inicial |

## 8. Enviando para o GitHub

**Pelo terminal**, dentro da pasta `lathevision/`:

```bash
git init
git add .
git commit -m "LatheVision: firmware ESP32-C3 e painel web"
git branch -M main
git remote add origin https://github.com/<seu-usuario>/<repositorio>.git
git push -u origin main
```

**Pelo GitHub Desktop:** *File → Add Local Repository*, escolha a pasta e clique em *Publish repository*.

Antes de fazer o commit, confira que `secrets.h` **não** aparece na lista de arquivos. O `.gitignore` já cuida disso.

## 9. Segurança

O broker HiveMQ público não tem senha. Qualquer pessoa que souber o identificador do torno consegue ver os dados. Para uso real na fábrica, o ideal é um broker privado, com usuário, senha e TLS. Quando isso acontecer, altere `MQTT_SERVER` e as credenciais no firmware, e `broker` em `docs/js/config.js`.

## 10. Solução de problemas

- **"Failed to connect to ESP32: No serial data received"** (ou falha de upload): coloque a placa no modo de gravação. Segure o botão **BOOT**, pressione e solte o **RESET**, solte o **BOOT** e clique em Upload logo em seguida. Use um cabo que transfira dados, não só carregue. Confira a porta em *PlatformIO → Devices*. Se continuar falhando, troque `upload_speed` para `115200` no `platformio.ini`.
- **Monitor Serial vazio na placa C3**: confira se a linha `-DARDUINO_USB_CDC_ON_BOOT=1` está no `platformio.ini`, faça um novo upload e reconecte o Monitor Serial.
- **Placa com chip conversor USB (CH340 ou CP2102) ao lado do conector USB**: nesse caso, o Monitor Serial passa pelos GPIO 20 e 21, que conflitam com o encoder. Se for o seu caso, avise para ajustarmos os pinos.
- **Erro de compilação dizendo que `secrets.h` não existe**: crie o arquivo como descrito na seção 2.
- **Painel mostra "SEM DADOS"**: o Monitor Serial está mostrando o JSON? Se sim, o `TORNO_ID` não bate com a identificação digitada no login.
- **Painel mostra "DESCONECTADO"**: verifique a internet. Abra o console do navegador (F12) para ver a mensagem de erro do MQTT.
- **Corrente oscilando muito**: confira o circuito de bias (1,65 V) e mantenha os fios do SCT-013 afastados dos cabos de potência.

## 11. Próximos passos

As abas Histórico, Alertas, Relatórios, Equipamentos e Configurações ainda são placeholders e mostram o aviso "em breve". Para guardar o histórico, o caminho natural é salvar as leituras em uma planilha ou em um banco de dados.
#   l a t h e v i s i o n  
 #   l a t h e v i s i o n  
 #   l a t h e v i s i o n  
 #   l a t h e v i s i o n  
 #   l a t h e v i s i o n  
 #   l a t h e v i s i o n  
 #   l a t h e v i s i o n  
 #   l a t h e v i s i o n  
 