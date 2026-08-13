# Prompt: componente customizado para Home Assistant (uso futuro)

Este arquivo é um **prompt pronto** para colar em uma sessão futura (provavelmente em outro
repositório, dedicado ao componente da Home Assistant — não faz sentido misturar com o firmware
deste repo). Copie o texto entre as linhas `>>>` abaixo e cole como pedido inicial.

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

Quero criar um **componente customizado (custom_component) para Home Assistant**, instalável via
HACS, para um dispositivo ESP32 caseiro (receptor de áudio Bluetooth + Music Assistant/Slimproto).
O firmware já está pronto e documentado — antes de escrever qualquer código, leia o repositório do
firmware para se contextualizar: https://github.com/celioblak/receiver-bt-esp32 — em especial
`README.md` (seções "API REST" e "MQTT / Home Assistant") e `docs/music_assistant_integration.md`.
A API pode ter evoluído desde que este prompt foi escrito, então confirme os endpoints reais no
código-fonte (`main/web_server.c`) em vez de confiar cegamente no que está resumido abaixo.

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
| GET | `/api/wifi/scan` | Redes WiFi visíveis |
| POST | `/api/system/restart` | Reinicia o dispositivo |
| POST | `/api/system/beep` | Toca um bipe de teste (recusa se algo estiver tocando) |

## Entidades desejadas na Home Assistant

- **`media_player`**: título/artista/álbum, estado (tocando/pausado/parado), play/pause/next/
  previous, controle de volume, atributo indicando a fonte ativa (Bluetooth vs Music Assistant) e
  o nome do dispositivo Bluetooth conectado (se aplicável).
- **`sensor`**: dispositivo Bluetooth conectado, contagem de dispositivos pareados, servidor Music
  Assistant configurado, IP, uptime.
- **`binary_sensor`** (`device_class: problem`): token da API do Music Assistant inválido/expirado
  (ver `docs/music_assistant_integration.md` do firmware — token dura ~1 ano e precisa ser
  renovado manualmente; sem essa entidade visível, o usuário não percebe quando os
  título/artista/álbum somem por causa disso).
- **`switch`**: AGC, visibilidade Bluetooth, exigir PIN de pareamento, e um switch por dispositivo
  já pareado (autorizar/bloquear).
- **`number`**: volume (0-200), timeout do amplificador (segundos sem tocar até desligar o relé).
- **`button`**: desconectar Bluetooth atual, bipe de teste.
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
