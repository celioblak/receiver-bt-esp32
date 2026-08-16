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

## FLAC nativo (decodificado no próprio firmware)

O provider Squeezelite do MA nunca expõe a opção "Output Codec" na UI de configuração do
player (`get_config_entries()` do provider não inclui `CONF_ENTRY_OUTPUT_CODEC`) — então ele
sempre usa o default hardcoded, que é **FLAC**. Pior: mesmo forçando esse default via patch (ver
histórico abaixo), um bug real do próprio Music Assistant (`providers/squeezelite/player.py`,
`_handle_play_url_for_slimplayer()`) extrai o `mime_type` cortando a URL de streaming pelo último
`.` — mas essa URL não tem extensão de arquivo, só os pontos do IP do servidor, então a extração
sempre pega lixo e cai no fallback FLAC de qualquer forma, **ignorando por completo** o parâmetro
`fmt=` da URL. Ou seja: nenhum patch do lado do servidor resolve isso de forma confiável.

A solução definitiva foi decodificar FLAC direto no dispositivo: `main/slimproto.c` agora aceita
`format == 'f'` no `strm` (além do `'p'` de PCM/WAV já suportado) e usa o componente
[`esphome/micro-flac`](https://github.com/esphome/micro-flac) (via `main/flac_stream_decoder.cpp`,
um wrapper C sobre a API C++ da lib) para decodificar o stream comprimido em tempo real, amostra a
amostra, direto pro mesmo ring buffer/pipeline de I2S já usado pelo caminho PCM. Buffers de
entrada/saída do decoder ficam em PSRAM (~33KB de flash a mais, ~0 de RAM interna adicional —
medido: 91,0% → 92,7% de uso de flash). Resultado prático: **o dispositivo agora toca o que quer
que o Music Assistant mande, sem depender de nenhuma configuração ou patch no servidor.**

### Histórico: patch flac→wav (não é mais necessário)

Antes da decodificação nativa, a única forma de tocar algo era forçar o servidor a mandar WAV em
vez de FLAC, editando `CONF_ENTRY_OUTPUT_CODEC` dentro do container em execução (não persistente —
qualquer recriação do container apagava o patch silenciosamente). Esse patch está obsoleto e não
precisa mais ser aplicado — mantido aqui só como referência histórica, caso o firmware precise
voltar a rodar sem suporte a FLAC por algum motivo:

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
de faixa: número de sessão, formato recebido (`strm ... format='p'` ou `format='f'`), chunks WAV
parseados (`fmt `, `data`, taxa/bits/canais reais) ou, no caminho FLAC, a linha "FLAC iniciado (N
Hz, N bits, N canal(is))" assim que o STREAMINFO é decodificado, além de qualquer pacote
descartado por sessão superada. Suficiente para depurar a maioria dos problemas de reprodução sem
precisar abrir a serial (que reseta o dispositivo de forma não confiável — ver
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
