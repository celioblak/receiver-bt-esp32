# Prompt: componente customizado para Home Assistant (uso futuro)

Este arquivo é um **prompt pronto** para colar em uma sessão futura (provavelmente em outro
repositório, dedicado ao componente da Home Assistant — não faz sentido misturar com o firmware
deste repo). Copie o texto entre as linhas `>>>` abaixo e cole como pedido inicial.

Atualizado em 2026-08-15 para refletir o estado atual de `main/web_server.c` (endpoints novos:
`/api/bt/pairing_mode`; campos novos em `/api/status`: `slim_enabled`/`slim_connected`/
`slim_playing`, `ma_configured`/`ma_token_valid`, `bt_remote_name`, `pending_pin_mac`/
`pending_pin_code`). Se o componente já existir num repo separado, use este prompt como uma
**atualização**: peça pra sessão comparar com o que já está implementado lá antes de escrever
código, em vez de recriar do zero.

Contexto de por que isso existe: o dispositivo já se integra à Home Assistant via **MQTT
Discovery genérico** (`main/mqtt_ha.c`, já funcional — sensores, switches, botões aparecem
sozinhos assim que o broker MQTT é configurado). O que falta é uma **integração nativa**
(`custom_component`, instalável via HACS), que permite coisas que o MQTT Discovery genérico não
cobre bem: um `media_player` completo e nativo (com todos os atributos padrão da HA, não só um
sensor de texto), `config_flow` com descoberta automática por mDNS, tratamento melhor de
disponibilidade/reconexão, e não depender de o usuário ter um broker MQTT configurado (pode falar
direto com a API REST do dispositivo).

---

>>>

Quero criar (ou, se já existir neste repositório, **atualizar**) um **componente customizado
(custom_component) para Home Assistant**, instalável via HACS, para um dispositivo ESP32 caseiro
(receptor de áudio Bluetooth + Music Assistant/Slimproto). Se já houver um `custom_components/`
neste repo, **primeiro** compare o que já está implementado com a lista de endpoints/entidades
abaixo e trate isso como um diff a aplicar, não como criação do zero. O firmware já está pronto e
documentado — antes de escrever qualquer código, leia o repositório do firmware para se
contextualizar: https://github.com/celioblak/receiver-bt-esp32 — em especial `README.md` (seções
"API REST" e "MQTT / Home Assistant") e `docs/music_assistant_integration.md`. A API pode ter
evoluído desde que este prompt foi escrito, então confirme os endpoints reais no código-fonte
(`main/web_server.c`) em vez de confiar cegamente no que está resumido abaixo.

## O que o dispositivo é

Um receptor de áudio Bluetooth A2DP (ESP32 Audio Kit V2.2 + codec ES8388) que também funciona como
player Music Assistant nativo via protocolo Slimproto/LMS. Tem exatamente **uma fonte de áudio
ativa por vez**: Bluetooth (celular pareado) tem prioridade; Music Assistant só toca quando não há
celular conectado. O dispositivo expõe:

- Uma **API REST em HTTP**, sem autenticação (uso só em rede local doméstica), na porta 80.
- **mDNS**: `<hostname>.local`, onde `<hostname>` é derivado do nome configurado do dispositivo
  (sanitizado). Use isso para descoberta automática via `zeroconf` no `config_flow`, em vez de
  pedir IP/host manualmente.
- Integração MQTT Discovery genérica já existente e funcional (não precisa ser substituída — o
  custom_component pode conviver com ela, ou vocês decidem se um substitui o outro).

## Endpoints REST relevantes (conferir no código antes de usar — podem ter mudado)

| Método | Rota | O que faz |
|---|---|---|
| GET | `/api/status` | Estado atual: conexão BT, faixa/artista/álbum (de BT ou Music Assistant, o que estiver ativo), volume 0-200, AGC, amplificador ligado/desligado, IP, uptime, status do token do Music Assistant, status do Slimproto |
| GET | `/api/config` | Configurações atuais (sem senhas/tokens) |
| POST | `/api/config` | Salva configurações |
| POST | `/api/volume` | `{"volume": 0-200}` |
| POST | `/api/agc` | `{"enabled": bool, "target": -30 a -6, "mode": 0\|1\|2}` |
| POST | `/api/media` | `{"cmd": "play"\|"pause"\|"playpause"\|"stop"\|"next"\|"previous"}` — funciona tanto para BT (AVRCP) quanto Music Assistant (Slimproto), o firmware decide sozinho qual usar |
| GET | `/api/logs` | Últimas 100 entradas do log interno do dispositivo |
| GET | `/api/devices` | Histórico de dispositivos Bluetooth pareados |
| POST | `/api/pair` | `{"mac": "...", "action": "allow"\|"block"\|"remove"\|"forget"\|"disconnect"}` |
| POST | `/api/bt/pairing_mode` | `{"duration_s": N}` — liga "descobrível" Bluetooth por tempo limitado (não persiste; desliga sozinho ao vencer o prazo). Resposta: `{"ok": true, "duration_s": N}` |
| GET | `/api/wifi/scan` | Redes WiFi visíveis |
| POST | `/api/system/restart` | Reinicia o dispositivo |
| POST | `/api/system/beep` | Toca um bipe de teste (recusa se algo estiver tocando) |

## Campos relevantes de `/api/status` (conferir no código antes de usar — podem ter mudado)

`bt_connected`, `bt_remote_mac`, `bt_remote_name`, `track`/`artist`/`album`, `playing`, `amplifier`
(estado do relé), `volume` (0-200), `agc_enabled`/`agc_gain`/`agc_target`/`agc_mode`, `wifi_ip`,
`uptime_s`, `bt_discoverable`, `bt_require_pin`, `pending_pin_mac`/`pending_pin_code` (preenchidos
durante um pareamento Passkey Entry em andamento), `slim_enabled`/`slim_connected`/`slim_playing`
(estado da conexão Slimproto/Music Assistant), `ma_configured`/`ma_token_valid` (base do
`binary_sensor` de token expirado, abaixo).

## Entidades desejadas na Home Assistant

- **`media_player`**: título/artista/álbum, estado (tocando/pausado/parado), play/pause/next/
  previous, controle de volume, atributo indicando a fonte ativa (Bluetooth vs Music Assistant) e
  o nome do dispositivo Bluetooth conectado (se aplicável). **Também desejado: duração/posição da
  faixa (`media_duration`/`media_position`, pra barra de progresso) e capa do álbum
  (`entity_picture`).** ⚠️ Nenhum dos dois existe hoje na API do firmware — checar antes de
  assumir que dá pra implementar direto:
  - **Duração/posição**: para Bluetooth, o firmware registra notificação AVRCP só de
    `PLAY_STATUS_CHANGE` (`main/bt_audio.c`) e o `attr_mask` das notificações pedidas é só
    `TITLE | ARTIST | ALBUM | GENRE` — falta pedir/tratar `ESP_AVRC_MD_ATTR_PLAYING_TIME` (e o
    evento `ESP_AVRC_RN_PLAY_POS_CHANGED`) se quiser posição via AVRCP. Para Music Assistant, o
    payload da API MA já visto ao vivo (`main/lms_metadata.c`, comentário no topo do arquivo) é
    rico (~6-8KB, inclui várias imagens e provavelmente duração) mas o parser atual só extrai
    `title`/`artist`/`album` e descarta o resto — precisaria estender esse parser.
  - **Capa do álbum — abordagem preferida: buscar do lado da Home Assistant, não do firmware.**
    O firmware já expõe `artist`/`album`/`track` em `/api/status` pras duas fontes (Bluetooth via
    AVRCP e Music Assistant); em vez de fazer o ESP32 (recursos limitados, sem BIP/OBEX pra
    Bluetooth) entregar a imagem em si, o `custom_component` deve usar esse texto como chave de
    busca contra um serviço externo de capas e resolver `entity_picture` sozinho — isso cobre
    **Bluetooth e Music Assistant da mesma forma**, sem depender de nenhum campo novo na API do
    dispositivo:
    - Opções de serviço pra pesquisa por artista+álbum (avaliar na hora, sem custo/sem API key
      idealmente): iTunes Search API (`itunes.apple.com/search`, simples, sem autenticação) ou
      MusicBrainz + Cover Art Archive (dois passos: busca o release no MusicBrainz, depois a capa
      no Cover Art Archive pelo MBID).
    - Cachear o resultado (por `artist`+`album`, não por request) pra não bater no serviço externo
      a cada poll do `DataUpdateCoordinator" — só buscar de novo quando `artist`/`album` mudar.
    - Tratar "sem resultado" graciosamente (`entity_picture` ausente, não um ícone quebrado) —
      áudio ao vivo/rádio ou álbuns muito obscuros podem não ter match.
    - Se no futuro o Music Assistant expuser uma URL de capa própria mais precisa (ver payload rico
      citado acima), essa pode virar um segundo fallback só pra essa fonte — mas não é
      pré-requisito, a busca externa já resolve as duas fontes hoje.
  - Se esses campos não existirem na API no momento em que este prompt for executado, **trate como
    um pré-requisito de firmware**: sinalize de volta em vez de simular/inventar os campos no lado
    da Home Assistant.
- **`sensor`**: dispositivo Bluetooth conectado, contagem de dispositivos pareados, servidor Music
  Assistant configurado, IP, uptime.
- **`binary_sensor`** (`device_class: problem`): token da API do Music Assistant inválido/expirado
  (ver `docs/music_assistant_integration.md` do firmware — token dura ~1 ano e precisa ser
  renovado manualmente; sem essa entidade visível, o usuário não percebe quando os
  título/artista/álbum somem por causa disso).
- **`switch`**: AGC, visibilidade Bluetooth persistente, exigir PIN de pareamento, e um switch por
  dispositivo já pareado (autorizar/bloquear).
- **`number`**: volume (0-200), timeout do amplificador (segundos sem tocar até desligar o relé).
- **`button`**: desconectar Bluetooth atual, bipe de teste, **ativar modo de pareamento temporário**
  (chama `/api/bt/pairing_mode` com uma duração fixa, ex. 180s — cobre o caso de querer parear um
  aparelho novo sem deixar o Bluetooth descobrível ligado o tempo todo, que compete por rádio/CPU
  com a decodificação de áudio).
- **`config_flow`**: descoberta automática via `zeroconf`/mDNS quando possível, com opção de
  configurar o IP/host manualmente como alternativa. Sem autenticação a configurar (API é aberta
  na rede local).

## Decisões técnicas em aberto (avaliar na hora, não decidir aqui)

- **REST (polling via `DataUpdateCoordinator`) vs. consumir os tópicos MQTT que o firmware já
  publica**: REST é mais simples de implementar e não depende de o usuário ter MQTT configurado;
  MQTT evita polling mas exige broker. Recomendo REST como abordagem principal do
  `custom_component`, mantendo a integração MQTT existente como alternativa independente (quem
  não quiser instalar o HACS ainda tem MQTT Discovery funcionando).
- **Idioma**: o uso é pessoal/doméstico em português — decidir se `strings.json`/traduções ficam
  só em `pt-BR` ou seguem o padrão HACS de ter `en` como fallback obrigatório mais `pt-BR` traduzido.
- Estrutura de repositório HACS padrão: `custom_components/<domain>/`, `manifest.json`,
  `config_flow.py`, `coordinator.py`, entidades por plataforma (`media_player.py`, `sensor.py`
  etc.), `strings.json`/`translations/`.

<<<
