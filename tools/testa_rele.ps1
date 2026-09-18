<#
.SYNOPSIS
    Alterna o rele do amplificador a cada N segundos, para conferir a ligacao.

.DESCRIPTION
    Serve para instalar/validar o modulo de rele sem precisar de audio tocando
    nem esperar o timeout. A cada ciclo imprime o que o firmware MANDOU e em
    que nivel o GPIO22 ficou -- e so ouvir o clique (ou olhar o LED do modulo)
    e comparar.

    O que voce precisa observar:
      - o rele CLICA nos dois sentidos?  -> hardware ok, ver a polaridade
      - fica sempre acionado?            -> modulo low trigger, ou GND/nivel
      - clica erratico / so as vezes     -> falta corrente ou nivel insuficiente

    O LED D4 da placa acompanha o mesmo GPIO e e ATIVO EM NIVEL BAIXO:
    apagado = nivel alto, aceso = nivel baixo.

.PARAMETER Ip
    Endereco do dispositivo. Padrao: 192.168.0.166

.PARAMETER Intervalo
    Segundos em cada estado. Padrao: 3

.EXAMPLE
    .\testa_rele.ps1
    .\testa_rele.ps1 -Ip 192.168.0.50 -Intervalo 5
#>
param(
    [string]$Ip = "192.168.0.166",
    [int]$Intervalo = 3
)

$ErrorActionPreference = "Stop"

function Set-Rele {
    param([bool]$Ligado)
    $corpo = @{ on = $Ligado } | ConvertTo-Json -Compress
    try {
        $r = Invoke-RestMethod -Uri "http://$Ip/api/amp" -Method Post `
                               -ContentType "application/json" -Body $corpo -TimeoutSec 8
        return $r
    } catch {
        Write-Host "  !! falha ao falar com o dispositivo: $($_.Exception.Message)" -ForegroundColor Red
        return $null
    }
}

Write-Host ""
Write-Host "=== Teste do rele -- $Ip ===" -ForegroundColor Cyan
Write-Host "Alternando a cada $Intervalo segundos. Ctrl+C para parar."
Write-Host ""

# Le a polaridade configurada, so para mostrar na tela
try {
    $cfg = Invoke-RestMethod -Uri "http://$Ip/api/config" -Method Get -TimeoutSec 8
    $pol = if ($cfg.relay_active_low) { "BAIXO (low trigger)" } else { "ALTO (high trigger)" }
    Write-Host "Polaridade configurada agora: aciona em nivel $pol" -ForegroundColor Yellow
    Write-Host ""
} catch {
    Write-Host "(nao consegui ler /api/config -- seguindo mesmo assim)" -ForegroundColor DarkGray
}

$ligado = $false
$ciclo = 0

while ($true) {
    $ligado = -not $ligado
    $ciclo++

    $r = Set-Rele -Ligado $ligado

    if ($null -ne $r) {
        # Nivel real no GPIO22, ja considerando a polaridade configurada
        $nivel = if ($r.amplifier -ne $r.relay_active_low) { 1 } else { 0 }
        $rotulo = if ($ligado) { "LIGADO " } else { "DESLIGADO" }
        $cor = if ($ligado) { "Green" } else { "DarkGray" }
        $led = if ($nivel -eq 1) { "D4 apagado" } else { "D4 aceso" }

        Write-Host ("[{0,3}] firmware diz: {1}   GPIO22 = {2}   ({3})" -f $ciclo, $rotulo, $nivel, $led) -ForegroundColor $cor
    }

    Start-Sleep -Seconds $Intervalo
}
