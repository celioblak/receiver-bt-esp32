# Prompt: duração/posição de faixa (Bluetooth AVRCP + Music Assistant)

Este arquivo é um **prompt pronto** para colar numa sessão (pode ser aqui mesmo, neste repositório
do firmware). Copie o texto entre as linhas `>>>` abaixo e cole como pedido inicial.

Contexto: `docs/home_assistant_custom_component_prompt.md` (prompt do componente Home Assistant)
listou duração/posição de faixa como recurso desejado no `media_player`, mas identificou que a API
do firmware não expõe isso hoje. Este prompt cobre a parte que falta, aqui no firmware, como
pré-requisito pro componente da Home Assistant poder mostrar barra de progresso.

---

>>>

Quero adicionar **duração e posição atual da faixa** à API do firmware, para as duas fontes de
áudio (Bluetooth A2DP/AVRCP e Music Assistant/Slimproto). Antes de mexer em qualquer coisa, siga a
memória do projeto sobre revisar impacto de RAM/stack antes de mudanças no firmware — esta mudança
é pequena (poucos campos escalares novos + uma notificação AVRCP a mais), mas confirme mesmo assim.

## Bluetooth (AVRCP) — `main/bt_audio.c`

- `request_track_metadata()` (~linha 471): o `attr_mask` hoje só pede
  `TITLE | ARTIST | ALBUM | GENRE`. Adicionar `ESP_AVRC_MD_ATTR_PLAYING_TIME` pra receber a duração
  total da faixa. A resposta chega em `ESP_AVRC_CT_METADATA_RSP_EVT` (~linha 510), no mesmo
  `switch (rc->meta_rsp.attr_id)` que já trata TITLE/ARTIST/ALBUM/GENRE — adicionar um `case` novo
  que parseia `rc->meta_rsp.attr_text` como número (duração em ms, vem como string de dígitos por
  spec AVRCP) e guarda num campo novo do `s_status` (mutex já existe, `s_status_mutex`).
- Posição atual precisa de uma notificação separada, registrada do mesmo jeito que
  `request_playback_status_notify()` (~linha 483) faz pra `ESP_AVRC_RN_PLAY_STATUS_CHANGE`: testar
  a capability bit (`ESP_AVRC_RN_PLAY_POS_CHANGED`) e chamar
  `esp_avrc_ct_send_register_notification_cmd(tl, ESP_AVRC_RN_PLAY_POS_CHANGED, interval_s)` — o
  terceiro parâmetro aqui é o intervalo em SEGUNDOS entre notificações automáticas da posição (as
  outras chamadas existentes usam `0` porque são eventos discretos, não periódicos — aqui precisa
  de um valor real, ex. `2`). Tratar o evento em `ESP_AVRC_CT_CHANGE_NOTIFY_EVT`
  (~linha 552), igual ao `PLAY_STATUS_CHANGE` já tratado ali — **e re-registrar a notificação a
  cada evento recebido**, mesmo padrão que `request_playback_status_notify()` já faz sozinha
  (senão só chega uma notificação e para).
- Ao guardar a posição, guardar também `esp_timer_get_time()` naquele instante — a Home Assistant
  precisa interpolar entre atualizações (não vai perguntar a cada segundo), então expor
  posição + timestamp da última leitura é melhor que só a posição crua.
- Zerar duração/posição/timestamp quando a faixa muda (`ESP_AVRC_RN_TRACK_CHANGE`, ~linha 553) —
  senão o valor antigo fica exposto por até `METADATA_FETCH_DELAY_US` (1.2s) até a faixa nova
  responder.

## Music Assistant — `main/lms_metadata.c`

- O payload da API MA observado ao vivo é rico (~6-8KB) mas o parser atual só extrai
  `title`/`artist`/`album` (ver comentário no topo do arquivo e o trecho perto de
  `copy_str_field(media_item, "name", ...)`, ~linha 130). **Antes de escrever o parser de
  duração/posição, logue um payload real bruto** (ou trecho relevante) pra confirmar os nomes de
  campo exatos — não adivinhar. O comentário existente já avisa que a API MA não suporta seleção de
  campos como o `tags:` do LMS clássico, então os nomes podem não ser óbvios (`duration` vs
  `duration_s` vs dentro de outro objeto aninhado).
- Depois de confirmado, extrair duração total e posição/elapsed atual (se o payload trouxer;
  Music Assistant tipicamente reporta isso na queue item ativa) pro mesmo formato usado pelo lado
  Bluetooth (duração em ms + posição em ms + timestamp da leitura).

## API — `main/web_server.c`

- Adicionar em `/api/status` (perto de `track`/`artist`/`album`, ~linha 134): algo como
  `media_duration_s` (float ou int, segundos) e `media_position_s` — já convertido de ms pra
  segundos na fronteira da API, seguindo a convenção existente (`uptime_s` já é em segundos, não
  ms). Calcular a posição interpolada no momento da resposta HTTP (posição guardada +
  `(esp_timer_get_time() - timestamp_guardado) / 1e6`, só quando `playing == true`) em vez de expor
  o timestamp bruto pro cliente — mais simples pro lado da Home Assistant, que só vai fazer
  polling.
- Funciona pras duas fontes (Bluetooth via `bt_audio_get_status()`, Music Assistant via
  `lms_metadata`) — usar o mesmo par de campos na resposta independente de qual fonte está ativa,
  igual já acontece com `track`/`artist`/`album`.

## Verificação

- Testar com celular pareado tocando música real: `/api/status` deve mostrar `media_duration_s`
  correto (bate com a duração real da faixa) e `media_position_s` subindo ao longo do tempo,
  resetando pra perto de 0 na troca de faixa.
- Testar via Music Assistant tocando uma faixa: mesma checagem.
- Conferir que nada regrediu no engasgo de áudio (ver memória do projeto sobre stutter) — esta
  mudança não deveria tocar em nada do caminho de áudio/ring buffer, só metadados, mas vale
  confirmar já que qualquer tráfego AVRCP extra compete pelo rádio BT (ver comentário existente
  sobre o atraso de 1.2s em `request_track_metadata()` — mesma preocupação se aplica aqui).

<<<
