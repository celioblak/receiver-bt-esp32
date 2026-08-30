# Prompt: componente customizado para Home Assistant (uso futuro)

Este arquivo é um **prompt pronto** para colar em uma sessão futura (provavelmente em outro
repositório, dedicado ao componente da Home Assistant — não faz sentido misturar com o firmware
deste repo). Copie o texto entre as linhas `>>>` abaixo e cole como pedido inicial.

**Atualizado em 2026-08-25** — reescrita completa após uma rodada grande de mudanças no firmware
desde a última atualização deste prompt (2026-08-20). Principais diffs em relação à versão
anterior:

- **Slimproto/LMS/Music Assistant (protocolo nativo) foi removido por completo** em 2026-08-16 e
  substituído por **DLNA/UPnP** (o dispositivo agora é um `MediaRenderer` DLNA de verdade). Toda
  menção a `ma_token`, `ma_configured`, "Slimproto" na versão anterior deste prompt está **obsoleta**
  — se o componente já existir e ainda tiver esses campos, é dívida técnica a remover, não a manter.
- **Pareamento por PIN foi removido** (2026-08-21) — não agregava segurança real (ver README). Se o
  componente já tiver um switch "Exigir PIN", ele deve ser removido também.
- **Novo**: microfone/karaokê (`mic_enabled`, `mic_auto_gate`), diagnóstico de Wi-Fi temporário
  (`/api/wifi/disable_temp`), campos de posição/duração reais para DLNA (`dlna_position`/
  `dlna_duration` — algo que a versão anterior deste prompt listava como "não existe ainda").

Se o componente já existir num repo separado, use este prompt como uma **atualização**: peça pra
sessão comparar com o que já está implementado lá antes de escrever código, tratando a lista abaixo
como a fonte da verdade atual, não como criação do zero.

Contexto de por que isso existe: o dispositivo já se integra à Home Assistant via **MQTT
Discovery genérico** (`main/mqtt_ha.c`, funcional — sensores, switches, botões aparecem sozinhos
assim que o broker MQTT é configurado, já com entidades separadas entre "controles" do dia a dia,
"config" e "diagnostic"). O que falta é uma **integração nativa** (`custom_component`, instalável
via HACS), que cobre o que o MQTT Discovery genérico não cobre bem: um `media_player` completo e
nativo (não só um sensor de texto), `config_flow` com descoberta automática por mDNS, tratamento
de disponibilidade/reconexão no padrão da HA (coordinator + device registry), e não depender de o
usuário ter um broker MQTT configurado — pode falar direto com a API REST do dispositivo.

---

>>>

Quero criar (ou, se já existir neste repositório, **atualizar**) um **componente customizado
(custom_component) para Home Assistant**, instalável via HACS, para um dispositivo ESP32 caseiro
(receptor de áudio Bluetooth A2DP + renderer DLNA/UPnP). Se já houver um `custom_components/`
neste repo, **primeiro** compare o que já está implementado com a lista de endpoints/entidades
abaixo e trate isso como um diff a aplicar, não como criação do zero — em especial, remova qualquer
resquício de PIN de pareamento ou de Slimproto/Music Assistant nativo/`ma_token`, que não existem
mais no firmware. O firmware já está pronto e documentado — antes de escrever qualquer código, leia
o repositório do firmware para se contextualizar: https://github.com/celioblak/receiver-bt-esp32 —
em especial `README.md` (seções "API REST" e "MQTT / Home Assistant"). A API pode ter evoluído
desde que este prompt foi escrito, então **confirme os endpoints reais no código-fonte**
(`main/web_server.c`) em vez de confiar cegamente no que está resumido abaixo.

## O que o dispositivo é

Um receptor de áudio Bluetooth A2DP (ESP32 Audio Kit V2.2 + codec ES8388) que também é um
**DLNA/UPnP MediaRenderer** completo (SSDP + SOAP), recebendo áudio empurrado por um control point
(tipicamente o provider DLNA do Music Assistant, mas qualquer control point UPnP genérico funciona
— BubbleUPnP, VLC, etc.). Tem exatamente **uma fonte de áudio ativa por vez**: Bluetooth tem
prioridade — DLNA só toca quando não há celular conectado (`dlna_should_abort()` no firmware). Além
disso, tem uma entrada de microfone opcional que mistura voz/canto por cima do que estiver tocando
("modo karaokê"). O dispositivo expõe:

- Uma **API REST em HTTP**, sem autenticação (uso só em rede local doméstica), na porta 80.
- **mDNS**: `<hostname>.local`, onde `<hostname>` é derivado do nome configurado do dispositivo
  (sanitizado), mais os serviços `_receiverbt._tcp` e `_http._tcp` anunciados. Use isso para
  descoberta automática via `zeroconf` no `config_flow`, em vez de pedir IP/host manualmente.
- Integração MQTT Discovery genérica já existente e funcional (não precisa ser substituída — o
  custom_component pode conviver com ela, ou vocês decidem se um substitui o outro).

## Endpoints REST (conferir no código antes de usar — podem ter mudado)

| Método | Rota | O que faz |
|---|---|---|
| GET | `/api/status` | Estado atual completo — ver campos abaixo |
| GET | `/api/config` | Configurações atuais (sem senhas) |
| POST | `/api/config` | Salva configurações — ver quais campos exigem reiniciar |
| POST | `/api/volume` | `{"volume": 0-100}` |
| POST | `/api/media` | `{"cmd": "play"\|"pause"\|"playpause"\|"stop"\|"next"\|"previous"}` — roteado pro BT (AVRCP) se conectado, senão pro DLNA. `next`/`previous` retornam 400 quando a fonte ativa é DLNA (a fila pertence ao control point, não ao renderer — limitação do protocolo, não do firmware) |
| POST | `/api/agc` | `{"enabled"?: bool, "target"?: -30 a -6, "mode"?: 0\|1\|2}` |
| GET | `/api/logs` | Últimas ~100 entradas do log interno (`[{"timestamp_ms","level","msg"}]`) |
| GET | `/api/devices` | Histórico de pareamento + listas de autorizados/bloqueados — ver formato abaixo |
| POST | `/api/pair` | `{"mac","action"}`, `action` ∈ `allow`\|`block`\|`remove`\|`forget`\|`disconnect` — ver semântica abaixo |
| POST | `/api/bt/pairing_mode` | `{"duration_s"?}` (padrão 180s) ou `{"stop": true}` — abre/fecha a janela de "descobrível" do Bluetooth. Resposta: `{"ok","duration_s","remaining_s"}` |
| GET | `/api/wifi/scan` | `[{"ssid","rssi","secure"}]` |
| POST | `/api/wifi/disable_temp` | `{"duration_s"?}` (padrão 180s) — **diagnóstico**, não é pra uso normal do componente: desliga o Wi-Fi por um tempo (o dispositivo reinicia sozinho no fim). Mencionado aqui só pra não ser confundido com um endpoint de operação; não expor como entidade a menos que peçam explicitamente |
| POST | `/api/system/restart` | Reinicia o dispositivo |
| GET/POST | `/api/system/beep` | Toca um bipe de teste; responde 409 se algo estiver tocando |
| POST | `/ota` | Upload de firmware (.bin) — fora de escopo pro componente HA |

## Campos de `GET /api/status`

- `device_name` (string), `device_mac` (string, MAC do próprio dispositivo — **usar como
  `unique_id`/`device_info` no HA**, ver seção de qualidade abaixo)
- `bt_connected` (bool), `bt_remote_mac`, `bt_remote_name` (strings, vazias se nada conectado)
- `track`, `artist`, `album` (strings) — do BT se conectado, senão do DLNA se houver faixa carregada
- `playing` (bool) — estado real de reprodução, mesma prioridade de fonte acima
- `dlna_state` (`"stopped"|"playing"|"paused"`) — independente do BT estar conectado
- `dlna_subscribed` (bool) — control point tem assinatura GENA ativa (proxy pra "cliente DLNA
  presente", análogo ao `bt_connected`)
- `dlna_client_ip`, `dlna_client_agent` (strings) — do último SOAP AVTransport recebido
- `dlna_position`, `dlna_duration` (strings `"H:MM:SS"`) — **já existem de verdade hoje** (a versão
  anterior deste prompt listava isso como "não implementado" — não é mais o caso, mas só vale pra
  fonte DLNA; posição/duração via Bluetooth AVRCP continuam sem implementar, ver seção
  `media_player` abaixo)
- `amplifier` (bool) — estado do relé do amplificador externo
- `volume` (number, 0-100)
- `agc_enabled` (bool), `agc_gain` (number), `agc_target` (number), `agc_mode` (0/1/2)
- `wifi_ip` (string), `uptime_s` (number)
- `bt_discoverable` (bool), `bt_discoverable_remaining_s` (number — 0 com `bt_discoverable=true`
  significa "visível permanentemente", não "acabou de expirar")
- `bt_allowed_count` (number) — >0 indica que a lista de autorizados está ativa (só quem está nela
  pode parear; 0 = qualquer um pode)

## `GET`/`POST` `/api/config`

Campos (todos opcionais no POST):

| Campo | Tipo | Aplica |
|---|---|---|
| `device_name` | string | na hora (atualiza mDNS também) |
| `wifi_ssid`, `wifi_pass` | string | **precisa reiniciar** |
| `relay_timeout_s` | number | na hora |
| `mqtt_host`, `mqtt_port`, `mqtt_user`, `mqtt_pass` | string/number | **precisa reiniciar** |
| `bt_discoverable` | bool | na hora |
| `mic_enabled` | bool | **precisa reiniciar** — decidido uma única vez no boot (aloca o canal I2S do mic); escrever sem reiniciar só grava a preferência, o valor lido em `/api/config`/MQTT continua refletindo o estado real (antigo) até reiniciar de verdade |
| `mic_auto_gate` | bool | **na hora**, sem reiniciar — ver seção mic/karaokê |
| `pairing_lock` | bool | na hora |

`wifi_pass`/`mqtt_pass` nunca são devolvidas pelo GET (write-only).

## `/api/devices` e `/api/pair`

`GET /api/devices` → array de:
```json
{"mac": "aa:bb:cc:dd:ee:ff", "name": "...", "last_seen_ms": 0, "allowed": true}
```
Inclui tanto o histórico de pareamento quanto MACs só presentes nas listas de
autorizado/bloqueado sem histórico (aparecem com `name` vazio).

`POST /api/pair` `{"mac","action"}`, `action`:
- `allow` — autoriza (adiciona à allow-list)
- `block` — bloqueia (remove da allow-list) **e desconecta na hora** se estiver conectado
- `remove` — reset completo: limpa histórico + allow-list + block-list pra esse MAC
- `forget` — remove o bond do controlador BT e desconecta (não reconecta sozinho depois)
- `disconnect` — só derruba a conexão atual, mantém o bond (pode reconectar depois)

## Microfone / karaokê

- `mic_enabled` — liga o hardware de captura (I2S RX + ADC do ES8388). Decidido só no boot,
  precisa reiniciar pra mudar (ver tabela acima).
- `mic_auto_gate` — **portão automático de voz**, ligado por padrão: só mistura o áudio do
  microfone quando o nível capta alguém falando/cantando de perto; fora isso, mistura silêncio em
  vez do ruído de fundo constante do microfone. Aplica em runtime, sem reiniciar.
- Não há campo de ganho do microfone exposto na API hoje (fixo em firmware) — se for expor um
  `number` pra isso no futuro, é mudança de firmware primeiro, não assumir que já existe.

## Entidades desejadas na Home Assistant

- **`media_player`**:
  - Atributos padrão: título/artista/álbum, estado (tocando/pausado/parado), play/pause/next/
    previous, controle de volume.
  - Atributo customizado indicando a fonte ativa (`bluetooth` vs `dlna`) e o nome do dispositivo
    Bluetooth conectado, quando aplicável.
  - `next`/`previous`: **desabilitar/ocultar quando a fonte ativa for DLNA** (o firmware recusa com
    400 — não silenciar o erro, refletir a limitação real na UI).
  - **Posição/duração** (`media_position`/`media_duration`): usar `dlna_position`/`dlna_duration`
    quando a fonte ativa for DLNA (já existem e são reais). Quando a fonte for Bluetooth, **não
    existe ainda** no firmware (AVRCP só notifica mudança de faixa, não posição — ver
    `main/bt_audio.c`) — não simular/inventar, deixar esses atributos ausentes nesse caso.
  - **Capa do álbum (`entity_picture`)**: não existe campo de imagem na API (nem para BT nem para
    DLNA). Abordagem recomendada: buscar do lado da Home Assistant, usando `artist`+`album` como
    chave de busca contra um serviço externo (ex. iTunes Search API, sem autenticação, ou
    MusicBrainz + Cover Art Archive), cacheado por `artist`+`album` (não por poll) pra não bater no
    serviço a cada ciclo do `DataUpdateCoordinator`. Tratar "sem resultado" graciosamente (sem
    `entity_picture`, não um ícone quebrado).
- **`sensor`**: dispositivo Bluetooth conectado, contagem de dispositivos pareados/autorizados, IP,
  uptime.
- **`switch`**: AGC, visibilidade Bluetooth persistente, microfone (`mic_enabled`), detecção
  automática de voz (`mic_auto_gate`), controle de dispositivo (`pairing_lock`), e um switch por
  dispositivo já pareado (autorizar/bloquear via `/api/pair`).
- **`number`**: volume (0-100), timeout do amplificador (segundos sem tocar até desligar o relé).
- **`button`**: desconectar Bluetooth atual, bipe de teste, ativar/encerrar modo de pareamento
  temporário (`/api/bt/pairing_mode`).
- **`config_flow`**: descoberta automática via `zeroconf`/mDNS quando possível, com opção de
  configurar o IP/host manualmente como alternativa. Sem autenticação a configurar (API é aberta na
  rede local).

Não existe mais nenhum `binary_sensor` de "token do Music Assistant expirado" — isso pertencia à
integração nativa Slimproto/LMS, removida. Não recriar.

## Paridade com o "Player Card" do Music Assistant quando MA está conectado

Pedido do usuário: quando o **Music Assistant** estiver integrado à Home Assistant e usando este
dispositivo como alvo DLNA (`dlna_client_agent` contendo `"Music Assistant"`), quer os mesmos
controles que o card nativo de player do MA oferece — seek (arrastar a posição), shuffle, repeat,
fila de reprodução ("tocando a seguir"), agrupamento de players. Antes de implementar, entenda por
que isso não é tão simples quanto parece:

- **Este dispositivo não é dono da fila.** Ele é um *renderer* DLNA — só toca o que o control point
  (MA) manda via `SetAVTransportURI`/Play/Pause/Stop. Shuffle, repeat e fila **são conceitos que
  vivem no servidor do Music Assistant**, não no firmware deste dispositivo. Não tem como
  implementar isso lendo só `/api/status` — não existe (nem devia existir) esse estado aqui.
- **A boa notícia: o Music Assistant já resolve isso sozinho.** Quando o MA está configurado na
  Home Assistant e tem este dispositivo como um player (via DLNA), a **própria integração nativa do
  Music Assistant já cria um `media_player` completo pra esse player** — com seek, shuffle, repeat,
  fila e agrupamento, exatamente o "Player Card" que o usuário quer. Isso já existe hoje, sem
  precisar de nenhuma linha de código deste `custom_component`.

**Recomendação**: não duplicar isso no nosso próprio `media_player`. A abordagem certa é:

1. O `media_player` deste `custom_component` continua simples e fiel ao que o **dispositivo**
   realmente sabe (BT + DLNA básico, ver seção anterior) — ele representa o *hardware*, não a sessão
   de reprodução do MA.
2. Quando `dlna_client_agent` indicar que quem está tocando é o Music Assistant, o jeito certo do
   usuário ter os controles ricos é usando **o `media_player` que o próprio MA já criou** (aparece
   como uma entidade separada, tipicamente `media_player.<nome_do_player_no_ma>`) — orientar isso no
   `README`/`strings.json` do componente (“para controles de fila/shuffle/repeat, use o player
   correspondente do Music Assistant”), em vez de reimplementar.
3. **Opcional, só se pedirem explicitamente depois**: dá pra ir além e fazer nosso próprio
   `media_player` **detectar e delegar** — se `dlna_client_agent` bater com Music Assistant, buscar
   o player correspondente na API do MA (precisa doutro fluxo de configuração: host/token do
   servidor MA, mapeamento pelo nome/UUID do player) e *proxyar* comandos de seek/shuffle/repeat pra
   lá. Isso é uma integração com **outro sistema** (a API do Music Assistant, não a API deste
   dispositivo) — escopo bem maior, com autenticação própria e mais uma fonte de falha. Só vale a
   pena se o usuário decidir que quer uma única entidade "canônica" em vez de duas (a do device e a
   do MA) — não é pré-requisito, e não deve ser feito sem alinhar essa decisão antes de codar.

## Qualidade esperada do componente (pra ficar "nível profissional", não um protótipo)

- **`device_info`**: usar `device_mac` (de `/api/status`) como identificador único de verdade
  (`connections={(dr.CONNECTION_NETWORK_MAC, device_mac)}`), não o IP (muda com DHCP) nem o nome
  (o usuário pode trocar). `manufacturer`/`model`/`sw_version` preenchidos.
- **`DataUpdateCoordinator`** com polling razoável (o dispositivo é um ESP32 com RAM interna
  escassa — não fazer polling agressivo tipo 1x/segundo; algo como 5-10s é adequado pra um
  media_player doméstico) e tratamento de indisponibilidade de verdade: se o polling falhar
  algumas vezes seguidas, marcar entidades como `unavailable`, não travar mostrando o último estado
  como se fosse atual.
- **Tratamento de erro em cada chamada POST** (timeout, 4xx, 5xx) — não assumir que a rede local
  nunca falha; o próprio firmware já loga instabilidades de Wi-Fi ocasionais.
- **`config_flow` com validação real**: testar `GET /api/status` no host informado antes de criar a
  entry, com mensagem de erro clara se não responder (não só "falha genérica").
- **`strings.json`/`translations/`**: `en` como fallback obrigatório (padrão HACS), `pt-BR`
  traduzido (uso real é doméstico em português).
- **Testes antes de considerar pronto**: `hassfest` (validação de manifest/estrutura) e um teste
  manual do `config_flow` (incluindo o caminho de erro) antes de publicar/atualizar via HACS.

## Decisões técnicas em aberto (avaliar na hora, não decidir aqui)

- **REST (polling via `DataUpdateCoordinator`) vs. consumir os tópicos MQTT que o firmware já
  publica**: REST é mais simples e não depende de o usuário ter MQTT configurado; MQTT evita
  polling mas exige broker. Recomendo REST como abordagem principal, mantendo a integração MQTT
  existente como alternativa independente (quem não quiser HACS ainda tem MQTT Discovery
  funcionando, já com entidades separadas em controles/config/diagnóstico).
- Estrutura de repositório HACS padrão: `custom_components/<domain>/`, `manifest.json`,
  `config_flow.py`, `coordinator.py`, entidades por plataforma (`media_player.py`, `sensor.py`,
  `switch.py`, `number.py`, `button.py`), `strings.json`/`translations/`.

<<<
