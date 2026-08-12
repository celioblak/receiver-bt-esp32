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
