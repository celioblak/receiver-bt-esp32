# Receiver Bluetooth DIY

Receiver de áudio Bluetooth A2DP baseado no **ESP32 Audio Kit V2.2** (módulo ESP32-A1S + codec ES8388), que recebe áudio de qualquer fonte Bluetooth (celular, TV, Echo Dot) e envia para um amplificador de potência externo, com integração ao Home Assistant e Music Assistant.

## Status do projeto

Em desenvolvimento. Veja o progresso por etapas na seção [Roadmap](#roadmap).

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
- [ ] Etapa 6 — `wifi_manager.c`: STA + AP de configuração + mDNS
- [ ] Etapa 7 — `web_server.c`: API REST + interface Web
- [ ] Etapa 8 — `pairing.c`: controle de dispositivos Bluetooth autorizados
- [ ] Etapa 9 — `mqtt_ha.c`: integração com Home Assistant (MQTT Discovery)
- [ ] Etapa 10 — `ota_manager.c`: atualização OTA — release `v1.0.0`

## Notas

- Uso de flash após a Etapa 4: ~66% de 1,5 MB (partição OTA) — a pilha Bluetooth clássica (Bluedroid) é grande. Acompanhar esse número nas próximas etapas (Wi-Fi, web server, MQTT, OTA); se necessário, ajustar `partitions.csv` para slots OTA maiores (reduzindo o SPIFFS).

## Regras de projeto

- GPIO21 (PA_ENABLE do amplificador onboard) **nunca** é ativado.
- O sistema funciona offline: Bluetooth, controle de relé e volume não dependem de Wi-Fi.
- Toda configuração do usuário é persistida em NVS — nunca hardcoded.
