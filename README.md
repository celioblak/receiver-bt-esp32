# Receiver Bluetooth DIY

Receiver de áudio Bluetooth A2DP baseado no **ESP32 Audio Kit V2.2** (módulo ESP32-A1S + codec ES8388), que recebe áudio de qualquer fonte Bluetooth (celular, TV, Echo Dot) e envia para um amplificador de potência externo, com integração ao Home Assistant e Music Assistant.

## Status do projeto

**v1.0.0** — as 10 etapas do roadmap original estão implementadas. **Validado em hardware real** (bring-up físico completo: pareamento, reprodução A2DP, controle de relé, interface web, MQTT). Veja o progresso por etapas na seção [Roadmap](#roadmap).

**Pós-v1.0.0:** a placa se revelou ter 8MB de PSRAM (apesar da documentação genérica do ESP32-A1S dizer que não tem) — isso resolveu uma classe inteira de crashes por heap fragmentado durante A2DP+Wi-Fi+MQTT simultâneos (ver [Notas](#notas)). Também: volume fino (escala perceptual 0-200, curva linear em dB), AGC ativável, reinício remoto, busca de Wi-Fi pela interface web, exibição do dispositivo Bluetooth conectado, gestão de pareamento (autorizar/bloquear/esquecer/desconectar), pareamento com PIN opcional e visibilidade Bluetooth configurável, controle de mídia (play/pause/próxima/anterior) e integração bidirecional com MQTT/Home Assistant — ver [Volume fino e AGC](#volume-fino-e-agc), [API REST](#api-rest) e [MQTT / Home Assistant](#mqtt--home-assistant).

**Limitação conhecida (hardware, não corrigível por firmware):** atividade de rádio (Wi-Fi e/ou Bluetooth, inclusive em idle/scan) acopla um ruído audível de baixo nível no estágio analógico do ES8388 nesta placa — mais perceptível em silêncio/transições do que durante reprodução contínua. Ver [Notas](#notas) para o diagnóstico completo e o que foi testado.

## Hardware

- **Módulo principal:** ESP32 Audio Kit V2.2 (ESP32-A1S + ES8388) — **tem 8MB de PSRAM** (confirmado por log em hardware real, `CONFIG_SPIRAM=y`; documentação genérica dessa placa às vezes diz que não tem)
- **Amplificadores suportados:** Taramps TS400x4/TD400x4, Soundigital SD400.4 EVO, Taramps TS800x4, módulos TPA3255
- Detalhes completos em [`docs/hardware_spec.md`](docs/hardware_spec.md), [`docs/pinout.md`](docs/pinout.md), [`docs/bom.md`](docs/bom.md) e [`docs/wiring_guide.md`](docs/wiring_guide.md)

## Pinagem (ESP32-A1S)

Confirmada de forma independente pelo projeto [squeezelite-esp32](https://github.com/sle118/squeezelite-esp32) (mesma placa).

| Sinal | GPIO |
|---|---|
| I2C SDA (ES8388 + expansão futura) | GPIO33 |
| I2C SCL | GPIO32 |
| I2S MCLK | GPIO0 |
| I2S BCLK | GPIO27 |
| I2S WS | GPIO25 |
| I2S DOUT (para codec) | GPIO26 |
| I2S DIN (do codec) | GPIO35 |
| PA_ENABLE (amp onboard — **não usar**) | GPIO21 |
| Controle do relé do amplificador externo (também liga o LED onboard — cosmético) | GPIO22 |

## Arquitetura de áudio/Bluetooth

Bluetooth A2DP sink e AVRCP via **Bluedroid nativo do ESP-IDF** (`esp_a2dp_api.h`, `esp_avrc_api.h`) e saída de áudio via **driver I2S nativo do ESP-IDF** (`driver/i2s_std.h`) + driver próprio do codec ES8388 (`main/es8388.c`, adaptado do driver MIT da Espressif usado no ESP-ADF).

**Por que não usar o ESP-ADF diretamente:** o `audio_pipeline`/`audio_element` do ESP-ADF foi avaliado, mas o integrador `framework = espidf` do PlatformIO calcula o caminho dos arquivos-objeto usando só o nome do arquivo (sem pasta) para qualquer código fora da pasta do próprio framework — isso causa colisões de build (`Two environments with different actions were specified for the same target`) sempre que dois componentes do ESP-ADF têm um arquivo com o mesmo nome (ex.: `es8311.c` existe em dois componentes distintos; `blufi_security.c` também). Não há como contornar isso via `EXCLUDE_COMPONENTS`, porque os componentes que colidem (`audio_stream` × `esp_codec_dev`, `esp_peripherals` × `wifi_service`) são simultaneamente necessários e mutuamente exigidos pelo grafo de dependências do ADF. A API Bluedroid + I2S nativa evita o problema por completo e mantém o build 100% dentro do PlatformIO, como pedido originalmente.

## Ambiente de desenvolvimento

- **IDE:** VS Code + extensão [PlatformIO](https://platformio.org/install/ide?install=vscode)
- **Framework:** ESP-IDF nativo (não Arduino), instalado automaticamente pelo PlatformIO na primeira compilação

### Pré-requisitos

Só o VS Code com a extensão PlatformIO. O PlatformIO baixa e gerencia o toolchain do ESP-IDF (Python, CMake, Ninja, compilador Xtensa etc.) automaticamente na primeira compilação — não é necessário instalar nada à parte, nem clonar o ESP-ADF.

> **Nota sobre espaço em disco:** o toolchain completo do ESP-IDF ocupa alguns GB. Se o drive `C:` estiver com pouco espaço livre, configure a variável de ambiente `PLATFORMIO_CORE_DIR` apontando para outro drive (ex.: `F:\.platformio`) *antes* de abrir o projeto pela primeira vez, para o PlatformIO instalar tudo lá desde o início. Neste ambiente, `C:` ficou sem espaço livre; o projeto, os pacotes do PlatformIO (`PLATFORMIO_CORE_DIR`) e o `TEMP`/`TMP` do usuário foram todos movidos para `F:` (variáveis de ambiente persistidas por usuário) — sem isso, `pio run` falha com erros de "No space left on device" ou componentes do ESP-IDF aparentemente ausentes (download truncado por falta de espaço).

### Compilar, flashar e monitorar

```bash
pio run                     # compilar
pio run --target upload     # flashar via USB
pio device monitor          # monitor serial
pio run --target uploadfs   # subir arquivos de spiffs_image/ para o SPIFFS
```

## Estrutura do projeto

```
receiver-bt-esp32/
├── platformio.ini          # PlatformIO: board, framework=espidf, partições
├── CMakeLists.txt          # Raiz do projeto ESP-IDF
├── partitions.csv          # Tabela de partições (2 slots OTA + NVS + SPIFFS)
├── sdkconfig.defaults      # Configurações padrão do menuconfig (BT, partições, etc.)
├── main/                   # Firmware (todo o código da aplicação)
├── components/             # Componentes ESP-IDF customizados (reservado para uso futuro)
├── spiffs_image/           # Arquivos da interface Web (HTML/CSS/JS)
└── docs/                   # Documentação de hardware
```

## Roadmap

- [x] Etapa 1 — Estrutura inicial do repositório
- [x] Etapa 2 — `config.h`, `storage.c` (wrapper NVS), `logger.c` (ring buffer)
- [x] Etapa 3 — `audio_codec.c` + `es8388.c`: driver I2S nativo + codec ES8388 (testado com tom de 440Hz)
- [x] Etapa 4 — `bt_audio.c`: A2DP sink + AVRCP CT (Bluedroid nativo), pareamento SSP "Just Works"
- [x] Etapa 5 — `relay_control.c`: controle do amplificador via GPIO22 (liga/desliga conforme estado do A2DP)
- [x] Etapa 6 — `wifi_manager.c`: STA (credenciais em NVS) + fallback AP de configuração + mDNS
- [x] Etapa 7 — `web_server.c`: API REST (`/api/status`, `/api/config`, `/api/volume`, `/api/logs`) + interface Web (SPIFFS)
- [x] Etapa 8 — `pairing.c`: lista de autorizados + histórico (`/api/devices`, `/api/pair`)
- [x] Etapa 9 — `mqtt_ha.c`: integração com Home Assistant via MQTT Discovery (opcional, silencioso sem broker configurado)
- [x] Etapa 10 — `ota_manager.c`: atualização OTA (`POST /ota`) — release `v1.0.0`

## Volume fino e AGC

Duas funcionalidades incrementais além das 10 etapas do roadmap original:

- **Volume fino** (`main/audio_codec.c`): a escala exposta pela API/Web é 0-200, mapeada **linearmente em dB** (não quadrática) para os 0-100 que `es8388_set_volume()` usa, restrita a uma janela de `VOLUME_MAX_ATTEN_DB` (50dB) de atenuação útil. dB já é uma escala perceptual (é assim que potenciômetros de áudio de verdade são calibrados) — uma curva `x²` por cima dobra a compressão e deixava a metade do slider praticamente muda. Persistido em NVS na chave `vol_user`.
- **AGC opcional** (`main/audio_agc.c`/`.h`): normaliza o volume percebido entre fontes diferentes (celular, TV, Echo Dot) sem o usuário precisar reajustar ao trocar de fonte. Calcula RMS sobre uma janela de 1024 amostras (~23ms) dos blocos PCM que `bt_audio.c` está prestes a tocar (entregues via `audio_agc_feed()`, chamada logo antes de `audio_codec_write()`), com suavização attack/release por modo (suave/médio/agressivo), ganho limitado a 0.2x–2x e um **gate de silêncio** (~-50dBFS): abaixo desse nível o ganho não é ajustado, só mantido — sem isso, um trecho quieto ou uma notificação do celular fazia o AGC cravar o ganho no máximo e estourar quando o áudio real voltava. Roda numa task FreeRTOS de baixa prioridade (nível 3, pilha 4096 bytes) a 20 Hz, auto-suspensa quando desligado. O ganho do AGC é aplicado via `audio_codec_apply_gain()`, que **não** persiste em NVS nem altera o volume definido pelo usuário — ao desligar o AGC, o volume original volta imediatamente.
- Ativável/desativável e configurável pela interface Web (`spiffs_image/settings.html`, seção "Volume") e pela API REST (`POST /api/agc`).

## API REST

Todos os endpoints retornam/aceitam JSON (exceto `/ota`, que recebe o `.bin` bruto).

| Método | Rota | Descrição |
|---|---|---|
| GET | `/api/status` | Estado atual: conexão BT (`bt_remote_mac`/`bt_remote_name`) ou Slimproto/Music Assistant (`slim_connected`/`slim_playing`), faixa/artista/álbum (de qualquer uma das duas fontes — BT tem prioridade), volume (0-200), AGC (`agc_enabled`/`agc_gain`/`agc_target`/`agc_mode`), amplificador, IP, uptime, `bt_discoverable`, `bt_require_pin`, `pending_pin_mac`/`pending_pin_code` (pareamento em andamento — ver [Pareamento](#gestão-de-pareamento-e-visibilidade-bluetooth)), `ma_configured`/`ma_token_valid` (token da API do Music Assistant — ver [`docs/music_assistant_integration.md`](docs/music_assistant_integration.md)) |
| GET | `/api/config` | Configurações atuais (sem senhas/token) |
| POST | `/api/config` | Salva configurações (nome, Wi-Fi, timeout do relé, MQTT, `slim_host`, `ma_token`, `bt_discoverable`, `bt_require_pin`) |
| POST | `/api/volume` | `{"volume": 0-200}` (escala perceptual — ver [Volume fino e AGC](#volume-fino-e-agc)) |
| POST | `/api/agc` | `{"enabled": bool, "target": -30 a -6 (dBFS), "mode": 0\|1\|2}` |
| POST | `/api/media` | `{"cmd": "play"\|"pause"\|"playpause"\|"stop"\|"next"\|"previous"}` — AVRCP passthrough pro celular (se BT conectado) ou protocolo LMS clássico pro Music Assistant (se Slimproto conectado, ver `main/lms_cli.c`) |
| GET | `/api/logs` | Últimas 100 entradas do log |
| GET | `/api/devices` | Histórico de dispositivos Bluetooth pareados (`allowed` = está na lista de autorizados) |
| POST | `/api/pair` | `{"mac": "...", "action": "allow"\|"block"\|"remove"\|"forget"\|"disconnect"}` — ver [Pareamento](#gestão-de-pareamento-e-visibilidade-bluetooth) |
| GET | `/api/wifi/scan` | Lista redes Wi-Fi visíveis (`ssid`, `rssi`, `secure`) |
| POST | `/api/system/restart` | Reinicia o dispositivo (responde e reinicia ~500ms depois) |
| POST | `/ota` | Corpo bruto = novo firmware (`.bin`); reinicia automaticamente |

### Gestão de pareamento e visibilidade Bluetooth

- **Lista de autorizados vazia = aceita qualquer dispositivo** (padrão). Autorizar (`action: "allow"`) pelo menos um MAC restringe pareamento *e* conexão só à lista — a checagem acontece tanto no pareamento inicial (`ESP_BT_GAP_CFM_REQ_EVT`) quanto a cada conexão nova (`ESP_A2D_CONNECTION_STATE_EVT`), porque um dispositivo já pareado antes (com link key salva no controlador BT) reconecta direto sem passar pela confirmação de novo — só checar no pareamento deixava essa brecha aberta.
- `action: "disconnect"` derruba a conexão atual sem mexer no pareamento — o dispositivo pode reconectar depois normalmente.
- `action: "forget"` remove o bond no controlador Bluetooth e desconecta agora — o dispositivo precisa parear de novo do zero pra voltar a conectar. Use pra liberar a conexão de vez pra outro dispositivo.
- **Visibilidade** (`bt_discoverable`, padrão ligado): quando desligado, o receptor não aparece mais na busca de dispositivos Bluetooth de quem ainda não pareou — dispositivos já pareados continuam conectando normalmente (`CONNECTABLE` continua sempre ligado, só `DISCOVERABLE` muda).
- **PIN de pareamento** (`bt_require_pin`, padrão desligado): ativa o fluxo "Passkey Entry" do Bluetooth (SSP, funciona em celulares modernos — diferente do PIN legado de 4 dígitos, que a maioria dos celulares ignora quando SSP está disponível). Quando alguém novo tenta parear, um código de 6 dígitos gerado na hora aparece em `/api/status` (`pending_pin_mac`/`pending_pin_code`) e na página **Dispositivos** da interface web — a pessoa digita esse código no celular pra completar o pareamento. Não afeta dispositivos já pareados.

## MQTT / Home Assistant

Opcional — sem broker configurado (`mqtt_host` vazio), a integração fica desativada silenciosamente. Publica discovery automático (`homeassistant/.../config`, retido) pra cada entidade, então aparecem sozinhas na Home Assistant depois de configurar o broker.

O nome do dispositivo na HA (device registry, não o nome de cada entidade) segue o `device_name` configurado — não fica mais travado em "Receiver Bluetooth DIY".

- **Sensor de diagnóstico** (`homeassistant/sensor/receiver_bt/config`): estado geral + todos os atributos de `/api/status` como `json_attributes` — inclui faixa/artista/álbum tanto do Bluetooth quanto do Music Assistant (via Slimproto, BT tem prioridade).
- **`binary_sensor` "Token Music Assistant"** (`device_class: problem`): acende quando um token de API foi configurado mas foi rejeitado (inválido/expirado — ver [`docs/music_assistant_integration.md`](docs/music_assistant_integration.md)). Fica apagado se nunca foi configurado (recurso opcional desligado não é "problema").
- **`sensor` "Dispositivo Conectado"**, **`sensor` "Dispositivos Pareados"** (contagem, nomes como atributo) e **`sensor` "Servidor Music Assistant"** (o `slim_host` configurado).
- **`switch` por dispositivo já pareado** (`homeassistant/switch/receiver_bt_pair_<mac sem ':'>/config`, criado/atualizado a cada publicação de estado — até 30s de atraso pra um dispositivo recém-pareado aparecer): liga = autorizado, desliga = bloqueado. Todos compartilham o tópico de comando `cmd/pair` (payload `{"mac": "...", "action": "allow"|"block"}`, montado automaticamente pelo `command_template` de cada entidade — não precisa montar isso manualmente).
- **`button` "Desconectar"** (`cmd/disconnect`): derruba o dispositivo Bluetooth conectado agora, sem mexer no pareamento.
- **Entidades de controle** (bidirecionais, tópicos de comando próprios sob `homeassistant/receiver_bt/cmd/`, documentados aqui por não existir um schema oficial de `media_player` da HA que cubra bem `next`/`previous`):
  - `number` Volume (`cmd/volume`, 0-200)
  - `number` Timeout do Amplificador (`cmd/relay_timeout`, 5-600s)
  - `switch` AGC (`cmd/agc_enabled`)
  - `switch` Visibilidade Bluetooth (`cmd/bt_discoverable`)
  - `switch` Exigir PIN (`cmd/bt_require_pin`)
  - `button` Play / Pause / Próxima / Anterior (`cmd/media`, payloads `play`/`pause`/`next`/`previous` — usa Bluetooth/AVRCP se conectado, senão Music Assistant via Slimproto)
- Qualquer mudança via MQTT publica o estado atualizado de volta em `homeassistant/sensor/receiver_bt/state` na hora (não espera o heartbeat de 30s).
- WiFi e MQTT em si ficam de fora das entidades configuráveis por MQTT de propósito — reconfigurar o próprio canal MQTT por ele mesmo é arriscado (um host/senha errado corta o único jeito de corrigir por ali); use a interface web pra isso.

## Notas

- **Patch automático no framework (modo sniff do Bluetooth)**: `scripts/patch_bt_no_sniff.py` roda como `extra_scripts = pre:` (ver `platformio.ini`) e desabilita o modo sniff do Bluedroid pro perfil A2DP/AVRCP, editando `bta_dm_cfg.c` direto em `PLATFORMIO_CORE_DIR/packages/framework-espidf` a cada build. Motivo: o Bluedroid pede sniff (economia de energia do rádio) ~7s depois do áudio pausar. O script é idempotente e reaplica sozinho se o framework for reinstalado (outra máquina, `pio pkg update`, etc.) — não precisa fazer nada manualmente, só rodar `pio run` normalmente. Evite `env.PioPlatform()` em scripts assim: usar essa API dentro de um `pre:` do builder ESP-IDF quebrou a detecção de toolchain (`No module named 'SCons.Tool.FortranCommon'`) — resolva o diretório do framework via a variável de ambiente `PLATFORMIO_CORE_DIR` direto.
- **Ruído de RF/EMI (investigação extensa em hardware real)**: além do modo sniff, o ruído de fundo ("toto toto"/"Rimmmmm") persistiu mesmo depois de eliminar todos os ângulos de firmware testados — modo sniff, volume/frequência de tráfego Wi-Fi, power-save do Wi-Fi, Wi-Fi totalmente desligado (só reduziu, não eliminou), conteúdo do buffer DMA do I2S (`auto_clear_after_cb`), preferência de coexistência Wi-Fi/BT (`esp_coex_preference_set` — **piorou** e foi revertido). O achado decisivo: **alimentar o dispositivo por fonte externa dedicada, sem o cabo USB conectado ao PC, eliminou a maior parte do ruído** — a porta USB do PC estava introduzindo ruído/loop de terra que mascarava tudo. O que sobra depois disso (ex.: o LED/relé do GPIO22 "tremendo" só quando alimentado sem o USB) é sensibilidade a RF na própria linha do relé/LED, mitigável com um capacitor de ~100nF entre o pino e o terra, ou uma fonte externa aterrada (plugue de 3 pinos). Um estalo único no ligar é inrush de corrente da fonte carregando os capacitores — acontece antes do firmware sequer rodar, não é corrigível por software.
- **AirPlay / Spotify Connect (cspot) — resolvido do lado do servidor, não precisa de firmware extra**: avaliado em detalhe se valeria implementar essas fontes diretamente no ESP32 (como o squeezelite-esp32 faz) e a resposta é não precisa. Não existe componente ESP-IDF mantido pra AirPlay ou Spotify Connect (só aplicações completas de terceiros tipo esp-airsync/airplay-esp32/cspot, que exigiriam "vendorizar" o projeto inteiro), e Wi-Fi + Bluetooth Classic simultâneos com áudio nos dois não é estável no ESP32 clássico (rádio único de 2.4GHz) — mas isso não importa aqui: o Music Assistant roda o AirPlay (como *emissor*, nunca receptor) e o Spotify Connect **do lado do servidor** (via algo tipo librespot) e reencaminha o áudio pro player Bluetooth escolhido usando o Bluetooth do próprio host.
- **Music Assistant via Slimproto/LMS (`main/slimproto.c`)**: cliente nativo do protocolo Squeezebox/Slimproto — conecta direto no IP do host MA (porta 3483), sem depender de descoberta. Requer ativar o provider "Squeezelite" em Settings → Player Providers do MA — sem outro pré-requisito: o firmware decodifica **FLAC nativamente** (via `esphome/micro-flac`, `main/flac_stream_decoder.cpp`) além de PCM/WAV, então não depende mais de nenhuma configuração/patch no lado do servidor (histórico do patch flac→wav, hoje obsoleto, em [`docs/music_assistant_integration.md`](docs/music_assistant_integration.md)). Áudio bufferizado (ring buffer + backpressure real contra a rede) e livre de corridas na troca de faixa — detalhes de cada bug corrigido no histórico do git (`fix(slimproto): ...`). Volume/mute do MA controlam o dispositivo de verdade (`audg`), play/pause/próxima/anterior funcionam via `/api/media` (protocolo LMS clássico, porta 9090), e título/artista/álbum aparecem em `/api/status`/Home Assistant via a API própria do MA (WebSocket, porta 8095 — exige um token de longa duração colado em Configurações, com aviso visível na tela e na Home Assistant se expirar).
- Uso de flash após adicionar decodificação FLAC nativa (`esphome/micro-flac`): 92,7% de 1,9 MB (partição OTA) — RAM 33,4% (109.604/327.680 bytes), Flash 92,7% (1.761.996/1.900.544 bytes). A lib em si (baixada mas não referenciada) não muda nada no binário — só passa a contar quando o código de fato chama o decoder; nesse caso, ~33 KB a mais sobre a medição anterior.
- Uso de flash atual (todas as funcionalidades pós-v1.0.0 descritas acima): 87,8% de 1,9 MB (partição OTA) — `pio run`, RAM 33,1% (108.460/327.680 bytes), Flash 87,8% (1.669.044/1.900.544 bytes). Mais baixo que a medição anterior (93,3%) apesar de mais funcionalidades — a remoção do tom de teste e código morto (round-trip de NVS legado) compensou.
- Uso de flash após volume fino + AGC (pós-v1.0.0): 93,3% de 1,9 MB (partição OTA) — ~127 KB livres. Build completo validado (`pio run`, ~10 min do zero): RAM 34,2% (112.092/327.680 bytes), Flash 93,3% (1.773.724/1.900.544 bytes). O AGC/volume fino adicionou só ~3,5 KB sobre o total anterior.
- Uso de flash final (v1.0.0, todas as 10 etapas): 93,1% de 1,9 MB (partição OTA) — ~131 KB livres. Auditado com `idf_size.py --archives`; os maiores consumidores (Bluedroid ~513 KB, Wi-Fi+lwIP+wpa_supplicant ~318 KB, mbedcrypto para o pareamento BT ~89 KB) são funcionalidades genuinamente usadas, não código morto. Se uma futura funcionalidade não couber, o próximo lugar a olhar é reduzir SPIFFS ainda mais (atualmente 320 KB, a interface web usa uma fração disso) em favor dos slots OTA.
- Uso de flash após a Etapa 9 (+ MQTT): ~93% de 1,9 MB (partição OTA), mesmo com `CONFIG_MQTT_TRANSPORT_SSL=n` e `CONFIG_MQTT_TRANSPORT_WEBSOCKET=n` (mbedtls parece vir de outro lugar — Bluedroid SSP, provavelmente — não só do MQTT). Sobram ~140 KB para a Etapa 10 (OTA); deve caber, já que a infraestrutura de OTA (`esp_ota_ops`) já é linkada pelo bootloader/partições.
- Uso de flash após a Etapa 7 (BT + Wi-Fi + mDNS + HTTP server + SPIFFS): ~87% de 1,9 MB (partição OTA). Margem apertando para as Etapas 8-10 — se necessário, revisitar o particionamento ou remover funcionalidades menos essenciais (ex.: `esp_http_server` tem `max_uri_handlers`/buffers configuráveis para reduzir RAM, mas o gargalo aqui é flash, não RAM).
- Depois de `pio run --target uploadfs` (sobe `spiffs_image/` para o SPIFFS do dispositivo), a interface web fica em `http://<ip-do-dispositivo>/` ou `http://receiver-bt.local/` (mDNS).
- Além de resolver o hostname, o dispositivo anuncia os serviços mDNS `_receiverbt._tcp` (com `fw_version` como TXT record — usado pela descoberta automática (`zeroconf`) do componente customizado da Home Assistant, ver [`docs/home_assistant_custom_component_prompt.md`](docs/home_assistant_custom_component_prompt.md)) e `_http._tcp` (genérico).
- Uso de flash após a Etapa 6 (BT + Wi-Fi + mDNS): ~82% de 1,9 MB (partição OTA). O `partitions.csv` já foi rebalanceado uma vez (SPIFFS reduzido de 960 KB para 320 KB, slots OTA aumentados de 1,5 MB para ~1,81 MB cada) — a interface web (Etapa 7) é só HTML/CSS/JS, não precisa de mais que isso. Continuar acompanhando nas próximas etapas.
- `CONFIG_ESP_WIFI_IRAM_OPT=n` e `CONFIG_ESP_WIFI_RX_IRAM_OPT=n` em `sdkconfig.defaults`: sem isso, a seção IRAM estoura com Bluetooth clássico (Bluedroid) + Wi-Fi + mDNS juntos nesta placa sem PSRAM. Custo: leve aumento no consumo de energia em modo power-save do Wi-Fi (irrelevante — o dispositivo é alimentado por fonte externa).
- Depois de mudar `sdkconfig.defaults`, pode ser necessário apagar o `sdkconfig.<env>` gerado (ex.: `sdkconfig.esp32-a1s`, já no `.gitignore`) para forçar a regeneração — o PlatformIO nem sempre detecta a mudança sozinho.

## Regras de projeto

- GPIO21 (PA_ENABLE do amplificador onboard) **nunca** é ativado.
- O sistema funciona offline: Bluetooth, controle de relé e volume não dependem de Wi-Fi.
- Toda configuração do usuário é persistida em NVS — nunca hardcoded.
