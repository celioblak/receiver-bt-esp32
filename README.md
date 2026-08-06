# Receiver Bluetooth DIY

Receiver de áudio Bluetooth A2DP baseado no **ESP32 Audio Kit V2.2** (módulo ESP32-A1S + codec ES8388), que recebe áudio de qualquer fonte Bluetooth (celular, TV, Echo Dot) e envia para um amplificador de potência externo, com integração ao Home Assistant e Music Assistant.

## Status do projeto

Em desenvolvimento. Veja o progresso por etapas na seção [Roadmap](#roadmap).

## Hardware

- **Módulo principal:** ESP32 Audio Kit V2.2 (ESP32-A1S + ES8388)
- **Amplificadores suportados:** Taramps TS400x4/TD400x4, Soundigital SD400.4 EVO, Taramps TS800x4, módulos TPA3255
- Detalhes completos em [`docs/hardware_spec.md`](docs/hardware_spec.md), [`docs/pinout.md`](docs/pinout.md), [`docs/bom.md`](docs/bom.md) e [`docs/wiring_guide.md`](docs/wiring_guide.md)

## Pinagem (ESP32-A1S / ai_thinker)

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
| Controle do relé do amplificador externo | GPIO22 |

## Ambiente de desenvolvimento

- **IDE:** VS Code + extensão [PlatformIO](https://platformio.org/install/ide?install=vscode)
- **Framework:** ESP-IDF nativo (não Arduino)
- **Biblioteca de áudio/BT:** [ESP-ADF](https://github.com/espressif/esp-adf)

### Pré-requisitos

1. VS Code com a extensão PlatformIO instalada.
2. ESP-ADF clonado localmente com a variável de ambiente `ADF_PATH` configurada:

   ```bash
   git clone --recursive https://github.com/espressif/esp-adf.git ~/esp/esp-adf
   export ADF_PATH=~/esp/esp-adf   # adicionar ao perfil do shell / variável de ambiente do sistema
   ```

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
├── platformio.ini          # PlatformIO: board, framework=espidf, ADF_PATH
├── CMakeLists.txt          # Raiz do projeto ESP-IDF
├── sdkconfig.defaults      # Configurações padrão do menuconfig (BT, partições, etc.)
├── main/                   # Componente principal do firmware
├── components/             # Componentes customizados / ESP-ADF
├── spiffs_image/           # Arquivos da interface Web (HTML/CSS/JS)
└── docs/                   # Documentação de hardware
```

## Roadmap

- [x] Etapa 1 — Estrutura inicial do repositório
- [ ] Etapa 2 — `config.h`, `storage.c` (wrapper NVS), `logger.c` (ring buffer)
- [ ] Etapa 3 — `audio_codec.c`: pipeline ESP-ADF (BT → I2S → ES8388)
- [ ] Etapa 4 — `bt_audio.c`: A2DP sink + AVRCP
- [ ] Etapa 5 — `relay_control.c`: controle do amplificador via GPIO22
- [ ] Etapa 6 — `wifi_manager.c`: STA + AP de configuração + mDNS
- [ ] Etapa 7 — `web_server.c`: API REST + interface Web
- [ ] Etapa 8 — `pairing.c`: controle de dispositivos Bluetooth autorizados
- [ ] Etapa 9 — `mqtt_ha.c`: integração com Home Assistant (MQTT Discovery)
- [ ] Etapa 10 — `ota_manager.c`: atualização OTA — release `v1.0.0`

## Regras de projeto

- GPIO21 (PA_ENABLE do amplificador onboard) **nunca** é ativado.
- I2C/I2S do ES8388 são inicializados exclusivamente pelo pipeline do ESP-ADF.
- O sistema funciona offline: Bluetooth, controle de relé e volume não dependem de Wi-Fi.
- Toda configuração do usuário é persistida em NVS — nunca hardcoded.
