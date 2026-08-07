# Receiver Bluetooth DIY

Receiver de áudio Bluetooth A2DP baseado no **ESP32 Audio Kit V2.2** (módulo ESP32-A1S + codec ES8388), que recebe áudio de qualquer fonte Bluetooth (celular, TV, Echo Dot) e envia para um amplificador de potência externo, com integração ao Home Assistant e Music Assistant.

## Status do projeto

**v1.0.0** — as 10 etapas do roadmap original estão implementadas e compilando. Ainda não testado em hardware real (bring-up físico fica para uma sessão com o dispositivo em mãos). Veja o progresso por etapas na seção [Roadmap](#roadmap).

**Pós-v1.0.0:** volume fino (escala perceptual 0-200) e AGC ativável (normalização de volume entre fontes) — ver [Volume fino e AGC](#volume-fino-e-agc).

## Hardware

- **Módulo principal:** ESP32 Audio Kit V2.2 (ESP32-A1S + ES8388)
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

> **Nota sobre espaço em disco:** o toolchain completo do ESP-IDF ocupa alguns GB. Se o drive `C:` estiver com pouco espaço livre, configure a variável de ambiente `PLATFORMIO_CORE_DIR` apontando para outro drive (ex.: `F:\.platformio`) *antes* de abrir o projeto pela primeira vez, para o PlatformIO instalar tudo lá desde o início.

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

- **Volume fino** (`main/audio_codec.c`): a escala exposta pela API/Web passou de 0-100 (linear) para 0-200, com curva perceptual quadrática (`x²`) mapeando para os 0-100 que o ES8388 realmente usa. O ouvido humano é logarítmico e a atenuação do DAC é linear em dB — sem a curva, os primeiros passos do slider pareciam não fazer nada e os últimos disparavam o volume. Persistido em NVS na chave `vol_user`.
- **AGC opcional** (`main/audio_agc.c`/`.h`): normaliza o volume percebido entre fontes diferentes (celular, TV, Echo Dot) sem o usuário precisar reajustar ao trocar de fonte. Calcula RMS sobre os blocos PCM que `bt_audio.c` está prestes a tocar (entregues via `audio_agc_feed()`, chamada logo antes de `audio_codec_write()` — este projeto não usa ESP-ADF, então não há um pipeline com buffer de leitura para o AGC amostrar; os mesmos bytes que serão tocados é que servem de amostra), com suavização attack/release por modo (suave/médio/agressivo) para evitar "bombeamento". Roda numa task FreeRTOS de baixa prioridade (nível 3) a 20 Hz, auto-suspensa quando desligado. O ganho do AGC é aplicado via `audio_codec_apply_gain()`, que **não** persiste em NVS nem altera o volume definido pelo usuário — ao desligar o AGC, o volume original volta imediatamente.
- Ativável/desativável e configurável pela interface Web (`spiffs_image/settings.html`, seção "Volume") e pela API REST (`POST /api/agc`).

## API REST

Todos os endpoints retornam/aceitam JSON (exceto `/ota`, que recebe o `.bin` bruto).

| Método | Rota | Descrição |
|---|---|---|
| GET | `/api/status` | Estado atual: conexão BT, faixa/artista/álbum, volume (0-200), AGC (`agc_enabled`/`agc_gain`/`agc_target`/`agc_mode`), amplificador, IP, uptime |
| GET | `/api/config` | Configurações atuais (sem senhas) |
| POST | `/api/config` | Salva configurações (nome, Wi-Fi, timeout do relé, MQTT) |
| POST | `/api/volume` | `{"volume": 0-200}` (escala perceptual — ver [Volume fino e AGC](#volume-fino-e-agc)) |
| POST | `/api/agc` | `{"enabled": bool, "target": -30 a -6 (dBFS), "mode": 0\|1\|2}` |
| GET | `/api/logs` | Últimas 100 entradas do log |
| GET | `/api/devices` | Histórico de dispositivos Bluetooth pareados |
| POST | `/api/pair` | `{"mac": "...", "action": "allow"\|"block"\|"remove"}` |
| POST | `/ota` | Corpo bruto = novo firmware (`.bin`); reinicia automaticamente |

## Notas

- Uso de flash final (v1.0.0, todas as 10 etapas): 93,1% de 1,9 MB (partição OTA) — ~131 KB livres. Auditado com `idf_size.py --archives`; os maiores consumidores (Bluedroid ~513 KB, Wi-Fi+lwIP+wpa_supplicant ~318 KB, mbedcrypto para o pareamento BT ~89 KB) são funcionalidades genuinamente usadas, não código morto. Se uma futura funcionalidade não couber, o próximo lugar a olhar é reduzir SPIFFS ainda mais (atualmente 320 KB, a interface web usa uma fração disso) em favor dos slots OTA.
- Uso de flash após a Etapa 9 (+ MQTT): ~93% de 1,9 MB (partição OTA), mesmo com `CONFIG_MQTT_TRANSPORT_SSL=n` e `CONFIG_MQTT_TRANSPORT_WEBSOCKET=n` (mbedtls parece vir de outro lugar — Bluedroid SSP, provavelmente — não só do MQTT). Sobram ~140 KB para a Etapa 10 (OTA); deve caber, já que a infraestrutura de OTA (`esp_ota_ops`) já é linkada pelo bootloader/partições.
- Uso de flash após a Etapa 7 (BT + Wi-Fi + mDNS + HTTP server + SPIFFS): ~87% de 1,9 MB (partição OTA). Margem apertando para as Etapas 8-10 — se necessário, revisitar o particionamento ou remover funcionalidades menos essenciais (ex.: `esp_http_server` tem `max_uri_handlers`/buffers configuráveis para reduzir RAM, mas o gargalo aqui é flash, não RAM).
- Depois de `pio run --target uploadfs` (sobe `spiffs_image/` para o SPIFFS do dispositivo), a interface web fica em `http://<ip-do-dispositivo>/` ou `http://receiver-bt.local/` (mDNS).
- Uso de flash após a Etapa 6 (BT + Wi-Fi + mDNS): ~82% de 1,9 MB (partição OTA). O `partitions.csv` já foi rebalanceado uma vez (SPIFFS reduzido de 960 KB para 320 KB, slots OTA aumentados de 1,5 MB para ~1,81 MB cada) — a interface web (Etapa 7) é só HTML/CSS/JS, não precisa de mais que isso. Continuar acompanhando nas próximas etapas.
- `CONFIG_ESP_WIFI_IRAM_OPT=n` e `CONFIG_ESP_WIFI_RX_IRAM_OPT=n` em `sdkconfig.defaults`: sem isso, a seção IRAM estoura com Bluetooth clássico (Bluedroid) + Wi-Fi + mDNS juntos nesta placa sem PSRAM. Custo: leve aumento no consumo de energia em modo power-save do Wi-Fi (irrelevante — o dispositivo é alimentado por fonte externa).
- Depois de mudar `sdkconfig.defaults`, pode ser necessário apagar o `sdkconfig.<env>` gerado (ex.: `sdkconfig.esp32-a1s`, já no `.gitignore`) para forçar a regeneração — o PlatformIO nem sempre detecta a mudança sozinho.

## Regras de projeto

- GPIO21 (PA_ENABLE do amplificador onboard) **nunca** é ativado.
- O sistema funciona offline: Bluetooth, controle de relé e volume não dependem de Wi-Fi.
- Toda configuração do usuário é persistida em NVS — nunca hardcoded.
