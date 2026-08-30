#pragma once

/* LED vermelho onboard (D5, GPIO19 -- o que fica ao lado do jack de fone).
 *
 * Existe porque um LED que ninguem sabe o que significa nao serve pra nada.
 * O mapeamento completo dos dois LEDs da placa esta no README, secao
 * "Sinalizacao por LED".
 *
 * O LED VERDE (D4) nao e controlado aqui: ele esta fisicamente no mesmo GPIO
 * do rele (PIN_RELAY_CONTROL, GPIO22), entao ja espelha o amplificador --
 * aceso = amplificador ligado. Nao da pra usa-lo para outra coisa, porque
 * pisca-lo ligaria e desligaria o amplificador de verdade. */
void status_led_init(void);
