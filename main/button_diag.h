#pragma once

/* Diagnostico TEMPORARIO para descobrir a pinagem real dos botoes fisicos
 * onboard do ESP32 Audio Kit V2.2 (KEY1-KEY6) -- a documentacao generica
 * dessa placa diverge entre "6 GPIOs digitais separados" e "1 pino ADC com
 * escada de resistores", e este projeto ja teve mais de um caso de hardware
 * real divergindo do genérico (ex. PSRAM, ver README). Loga no logger
 * interno (visivel em /api/logs) qualquer mudanca detectada nos candidatos
 * mais citados para essa placa -- apertar cada botao fisico deve aparecer
 * ali. Depois de mapeado de verdade, isto deve ser removido/substituido por
 * uma leitura direta só do pino confirmado. */
void button_diag_init(void);
