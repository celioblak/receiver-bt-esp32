# Integração com Music Assistant (Slimproto/LMS)

O dispositivo conecta no Music Assistant como um player **Squeezelite** nativo,
via protocolo Slimproto (o mesmo do Logitech Media Server/Squeezebox), implementado em
`main/slimproto.c`. Diferente de DLNA (`main/dlna_renderer.c`, mantido no código mas nunca
descoberto pelo MA via SSDP/multicast em Docker), o Slimproto não depende de
descoberta: o ESP32 conecta direto no IP do host MA, porta 3483, do mesmo jeito que
já faz com o broker MQTT.

Ver também a memória de projeto `project_slimproto_music_assistant` (histórico completo
de bugs encontrados e corrigidos, tanto no firmware quanto no lado do servidor).

## Passo necessário no Music Assistant

1. Em **Settings → Player Providers**, adicionar/ativar o provider **Squeezelite** — não vem
   ativado por padrão (diferente do DLNA, que é carregado automaticamente).
2. O dispositivo aparece na lista de players assim que envia o primeiro `HELO`.

## ⚠️ Patch necessário no container do Music Assistant (NÃO é persistente)

O provider Squeezelite do MA nunca expõe a opção "Output Codec" na UI de configuração do
player (`get_config_entries()` do provider não inclui `CONF_ENTRY_OUTPUT_CODEC`) — então ele
sempre usa o default hardcoded, que é **FLAC**. Este firmware só decodifica PCM/WAV, não FLAC,
então **sem esse patch o Slimproto nunca recebe áudio reproduzível**.

**Fix aplicado**: dentro do container do Music Assistant, localizar `CONF_ENTRY_OUTPUT_CODEC`
no código-fonte do `aioslimproto` (pacote Python instalado no container, arquivo `constants.py`)
e mudar o `default_value` de `"flac"` para `"wav"`:

```sh
# de dentro do container (ex.: via console do Portainer, ou docker exec):
python3 -c "
import aioslimproto, pathlib
p = pathlib.Path(aioslimproto.__file__).parent / 'constants.py'
print(p)  # confirma o caminho real antes de editar
"
# depois de confirmar o caminho:
sed -i 's/CONF_ENTRY_OUTPUT_CODEC = ConfigEntry(key=CONF_OUTPUT_CODEC, type=ConfigEntryType.STRING, default_value="flac"/CONF_ENTRY_OUTPUT_CODEC = ConfigEntry(key=CONF_OUTPUT_CODEC, type=ConfigEntryType.STRING, default_value="wav"/' <caminho-de-constants.py>
```

Ajuste o `sed` para o texto exato encontrado na versão instalada — o nome do arquivo e a
assinatura exata de `ConfigEntry` podem variar entre versões do `aioslimproto`. O importante é
localizar a definição de `CONF_ENTRY_OUTPUT_CODEC` e trocar `default_value="flac"` por
`default_value="wav"`, depois reiniciar o container do Music Assistant.

**Por que isso precisa ser redocumentado aqui:** esse patch edita um arquivo Python **dentro do
container em execução** — não é um volume montado, não é uma imagem customizada. Qualquer
recriação do container (`docker compose up --force-recreate`, atualização de imagem, reinstalação)
**apaga o patch silenciosamente**, e o sintoma volta a ser "Music Assistant mostra tocando, mas
nenhum áudio chega no dispositivo" — sem nenhum erro óbvio nos logs do firmware (o Slimproto
simplesmente nunca recebe um `strm` com formato que ele aceite).

### TODO: tornar o patch persistente

Ainda não implementado — opções a avaliar quando isso for revisitado:

- **Imagem customizada**: `Dockerfile` próprio com `FROM ghcr.io/music-assistant/server:<tag>` +
  um `RUN sed -i ...` no build, versionado neste repo (ex.: `docker/music-assistant.Dockerfile`).
- **Patch na inicialização**: script de entrypoint customizado que aplica o `sed` toda vez que o
  container sobe, antes do processo principal — mais simples de manter que uma imagem própria,
  mas ainda depende de reconstruir/trocar a imagem de deploy.
- **Contribuir a correção upstream**: abrir uma issue/PR no Music Assistant expondo
  `CONF_ENTRY_OUTPUT_CODEC` na config do provider Squeezelite (a causa raiz real) — eliminaria a
  necessidade do patch por completo, mas depende do time do MA aceitar.

Até isso ser feito, **depois de qualquer atualização/recriação do container do Music Assistant,
reaplicar o patch acima antes de testar o Slimproto** — se o áudio parar de chegar sem motivo
aparente, este é o primeiro lugar a checar.

## Status ("o que está tocando") e controle (play/pause/próxima/anterior)

O protocolo Slimproto em si só transporta áudio — não título/artista/álbum nem volume/mute
vindos do MA. Isso é resolvido por três peças separadas:

- **Volume/mute do MA (`main/slimproto.c`, `handle_audg`)**: o servidor manda isso pelo opcode
  `audg` na própria conexão Slimproto (payload de 18 bytes, ganho em ponto fixo 16.16 — 65536 =
  0dB/ganho unitário). Até essa correção, `audg` era descartado junto com qualquer opcode que não
  fosse `strm`/`setd` — por isso mudar o volume ou mutar pelo Music Assistant não tinha nenhum
  efeito no dispositivo. Agora `handle_audg()` converte o ganho pra escala 0-`VOLUME_STEPS` e
  chama `audio_codec_set_volume()`, a mesma função usada pelo volume manual.
- **Controle (play/pause/próxima/anterior) — `main/lms_cli.c`**: usa o protocolo clássico de
  controle do LMS (porta 9090, texto simples), que o Music Assistant também implementa por
  compatibilidade. Cada comando abre uma conexão TCP curta, manda uma linha
  (`<player_id> play`/`pause 1`/`playlist index +1`/etc.) e fecha — sem estado persistente. O
  `player_id` é o MAC WiFi STA formatado com `:` como `%3A` (confirmado ao vivo). Roteado por
  `POST /api/media` (mesmo endpoint que já existia pro Bluetooth/AVRCP): usa BT se conectado,
  senão Slimproto/MA se conectado.
- **Título/artista/álbum — `main/lms_metadata.c`**: **não** vem pela porta 9090 (testado ao vivo:
  `player_queues` sempre reporta 0 faixas por ali, mesmo tocando) — só pela API própria do MA
  (WebSocket, porta 8095, path `/ws`), consultando `player_queues/get` com `queue_id` = `"up" +
  MAC sem ':'` (o ID que o "Universal Player" do MA usa internamente pro nosso dispositivo,
  confirmado ao vivo). Os campos relevantes ficam em
  `result.current_item.media_item.{name,artists[0].name,album.name}`. Consultado por polling a
  cada 3s (não assina eventos push — mais simples, suficiente pra exibição de status).

### Token de API do Music Assistant (obrigatório só pra metadados, não pro controle)

A API WebSocket exige autenticação. Esta versão do MA (2.9.x) **não tem um token de API fixo
gerável nas configurações** — só login usuário/senha (que devolve um JWT de sessão) ou, no perfil
do usuário, a opção de gerar um **token de longa duração** (JWT com `is_long_lived: true`, válido
por ~1 ano a partir da geração). É esse token de longa duração que o firmware usa — cole em
**Configurações → Token da API do Music Assistant**. Guardado em NVS (`NVS_KEY_MA_TOKEN`), nunca
devolvido por `GET /api/config` (mesmo tratamento das senhas de WiFi/MQTT).

**Sem token configurado**: o recurso fica desativado em silêncio — controle continua funcionando
normalmente (não depende do token), só título/artista/álbum não aparecem.

**Token expirado/inválido (depois de ~1 ano, ou se for revogado no MA)**: diferente de "não
configurado", isso é tratado como um problema visível de propósito (senão vira um mistério — o
metadado simplesmente some e ninguém entende por quê):
- Banner de aviso na tela inicial e em Configurações (`ma_configured && !ma_token_valid` em
  `GET /api/status`).
- Entidade `binary_sensor` (`device_class: problem`) "Receiver BT Token Music Assistant" na Home
  Assistant, via MQTT Discovery — acende quando o token foi rejeitado.

Quando isso acontecer: gerar um novo token de longa duração no perfil do usuário do MA e colar de
novo em Configurações.

## Diagnóstico sem precisar de serial

O firmware expõe `GET /api/logs` (últimas 100 entradas do log interno) — inclui, para cada troca
de faixa: número de sessão, chunks WAV parseados (`fmt `, `data`, taxa/bits/canais reais) e
qualquer pacote descartado por sessão superada. Suficiente para depurar a maioria dos problemas
de reprodução sem precisar abrir a serial (que reseta o dispositivo de forma não confiável — ver
`feedback_verify_upload_took_effect`).

## DLNA (`dlna_renderer.c`) — status

Implementa só descoberta (SSDP) + controle (AVTransport/RenderingControl) — nunca chegou a buscar
o áudio de verdade (marco 2, nunca implementado). Decisão consciente: como o Music Assistant nunca
descobriu o dispositivo via DLNA/SSDP em Docker (problema de multicast, não resolvido) e o
Slimproto já resolve o objetivo real, não vale investir em terminar o marco 2 só por causa do MA.
O código continua no repositório e funciona para descoberta/controle genéricos (BubbleUPnP, VLC,
etc.), só não reproduz áudio.

**Ideia levantada para o futuro (não implementada)**: em vez de depender de descoberta broadcast
(SSDP), investigar como forçar o Music Assistant a "procurar" o dispositivo Slimproto diretamente
pelo IP, sem descoberta — já é essencialmente o que o Slimproto faz hoje (o ESP32 que inicia a
conexão TCP pro host), então isso já está resolvido nesse sentido; se a ideia for sobre DLNA
especificamente, precisaria investigar se o MA aceita adicionar um player DLNA manualmente por
IP/URL de descrição, sem depender do SSDP.
