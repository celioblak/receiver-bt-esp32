"""Desabilita o modo sniff do Bluetooth (Bluedroid) para o perfil AV (A2DP/AVRCP).

O ESP-IDF Bluedroid pede modo sniff (economia de energia do radio) depois
de ~7s sem audio A2DP fluindo (ver bta_dm_cfg.c, entrada "AV : 1",
BTA_DM_PM_SNIFF_A2DP_IDX). Nesta placa (ESP32-A1S + ES8388), a transicao
de/para sniff acopla um ruido audivel ("Rimmmmm" seguido de cliques
periodicos) no estagio analogico do codec sempre que o audio esta
pausado/parado. Nao ha API publica do esp-idf pra desativar isso -- o
bitmask de modos de energia permitidos e um valor fixo dentro do proprio
arquivo do framework.

Este script roda como extra_script (pre) do PlatformIO e edita esse
arquivo direto em PLATFORMIO_CORE_DIR/packages/framework-espidf a cada
build. E idempotente (nao aplica de novo se ja aplicado) e reaplica
sozinho se o framework for reinstalado/atualizado -- por isso o patch
nao precisa (nem pode) ser versionado dentro do proprio framework, so
este script fica no repo.
"""
import os

Import("env")

def _find_framework_dir():
    """Evita env.PioPlatform() aqui: chamar essa API dentro de um extra_script
    'pre:' do builder ESP-IDF (que faz sua propria sub-build SCons aninhada)
    disparava uma reinicializacao parcial da plataforma que quebrava a
    deteccao de toolchain mais adiante ("No module named
    'SCons.Tool.FortranCommon'"). Resolver o diretorio via variavel de
    ambiente evita mexer no estado da plataforma."""
    core_dir = os.environ.get("PLATFORMIO_CORE_DIR") or os.path.expanduser(os.path.join("~", ".platformio"))
    return os.path.join(core_dir, "packages", "framework-espidf")


TARGET_REL = os.path.join(
    "components", "bt", "host", "bluedroid", "bta", "dm", "bta_dm_cfg.c"
)
OLD_SNIPPET = (
    "    /* AV : 1 */\n"
    "    {\n"
    "        (BTA_DM_PM_SNIFF),                                             /* allow sniff */\n"
)
NEW_SNIPPET = (
    "    /* AV : 1 */\n"
    "    {\n"
    "        (0),                          /* sniff DESABILITADO (patch local, ver scripts/patch_bt_no_sniff.py) */\n"
)
ALREADY_PATCHED_MARKER = "sniff DESABILITADO (patch local"


def patch_bta_dm_cfg():
    framework_dir = _find_framework_dir()
    target_path = os.path.join(framework_dir, TARGET_REL)
    if not os.path.isfile(target_path):
        print("[patch_bt_no_sniff] bta_dm_cfg.c nao encontrado em %s, pulando patch" % target_path)
        return

    with open(target_path, "r", encoding="utf-8") as f:
        content = f.read()

    if ALREADY_PATCHED_MARKER in content:
        return  # ja aplicado nesta instalacao do framework

    if OLD_SNIPPET not in content:
        print(
            "[patch_bt_no_sniff] AVISO: trecho esperado nao encontrado em bta_dm_cfg.c "
            "(versao do framework pode ter mudado) -- patch NAO aplicado, ruido de sniff "
            "pode voltar a aparecer"
        )
        return

    content = content.replace(OLD_SNIPPET, NEW_SNIPPET, 1)
    with open(target_path, "w", encoding="utf-8") as f:
        f.write(content)
    print("[patch_bt_no_sniff] modo sniff do Bluetooth (perfil AV) desabilitado em bta_dm_cfg.c")


patch_bta_dm_cfg()
