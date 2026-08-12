#pragma once

/* DLNA/UPnP MediaRenderer minimo -- permite que o Music Assistant (ou
 * qualquer control point DLNA: BubbleUPnP, VLC, etc.) descubra e "veja"
 * este dispositivo como um alvo de reproducao, independente do Bluetooth.
 *
 * MARCO 1 (este arquivo): SSDP (descoberta) + descricao do dispositivo +
 * AVTransport/RenderingControl/ConnectionManager respondendo certo aos
 * comandos (SetAVTransportURI, Play/Pause/Stop, SetVolume). Ainda NAO
 * busca/decodifica o audio de verdade -- isso fica pro proximo marco,
 * depois de confirmar em hardware real que o Music Assistant enxerga e
 * controla o dispositivo. Roda num servidor HTTP proprio (porta separada
 * da interface web), sem tocar em nada do Bluetooth/audio existente. */

void dlna_renderer_init(void);
