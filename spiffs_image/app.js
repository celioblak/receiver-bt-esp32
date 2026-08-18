async function apiGet(path) {
    const res = await fetch(path);
    if (!res.ok) throw new Error(path + ": " + res.status);
    return res.json();
}

async function apiPost(path, body) {
    const res = await fetch(path, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body),
    });
    if (!res.ok) throw new Error(path + ": " + res.status);
    return res.json();
}

function fmtUptime(seconds) {
    const h = Math.floor(seconds / 3600);
    const m = Math.floor((seconds % 3600) / 60);
    const s = Math.floor(seconds % 60);
    return h + "h " + m + "m " + s + "s";
}

/* Janela de pareamento Bluetooth -- compartilhada pela página inicial e pela
 * de Dispositivos (mesma lógica nos dois lugares, num só arquivo).
 *
 * O receiver não fica visível o tempo todo: parear exige abrir uma janela
 * temporária. A contagem decrementa localmente a cada segundo entre as
 * atualizações do /api/status, senão o número pularia de 2 em 2 segundos. */
const btPairing = (() => {
    let restante = 0;
    let permanente = false;
    let bloqueadoPorLista = false;
    let els = null;
    let aoMudar = null;

    function janelaAtiva() {
        return permanente || restante > 0;
    }

    function fmt(s) {
        const m = Math.floor(s / 60);
        const seg = s % 60;
        return m > 0 ? m + "min " + String(seg).padStart(2, "0") + "s" : seg + "s";
    }

    function render() {
        if (!els) return;
        /* Com lista de autorizados ativa, abrir a janela NÃO basta: um
         * aparelho novo é rejeitado em pairing_is_allowed(). Avisar aqui
         * evita o pareamento falhar sem explicação. */
        if (els.aviso) {
            const precisaAutorizar = (janelaAtiva() && bloqueadoPorLista);
            els.aviso.style.display = precisaAutorizar ? "block" : "none";
        }
        if (permanente) {
            /* Com "sempre visível" ligado nas Configurações, abrir janela
             * temporária não faria diferença nenhuma -- some com o botão em
             * vez de oferecer uma ação sem efeito. */
            els.badge.textContent = "sempre visível";
            els.badge.className = "badge on";
            els.btn.style.display = "none";
        } else if (restante > 0) {
            els.badge.textContent = fmt(restante) + " restantes";
            els.badge.className = "badge on";
            els.btn.style.display = "";
            els.btn.textContent = "Encerrar agora";
        } else {
            els.badge.textContent = "desligado";
            els.badge.className = "badge off";
            els.btn.style.display = "";
            els.btn.textContent = "Permitir pareamento (3 min)";
        }
    }

    return {
        /* onChange: chamado após o clique, pra página recarregar o status na
         * hora em vez de esperar o próximo ciclo. */
        init(badgeId, btnId, onChange, avisoId) {
            const badge = document.getElementById(badgeId);
            const btn = document.getElementById(btnId);
            if (!badge || !btn) return; /* página não tem esse bloco */
            els = { badge, btn, aviso: avisoId ? document.getElementById(avisoId) : null };
            aoMudar = onChange;
            btn.addEventListener("click", async () => {
                const encerrar = (!permanente && restante > 0);
                try {
                    await apiPost("/api/bt/pairing_mode", encerrar ? { stop: true } : {});
                } catch (e) {
                    console.error(e);
                }
                if (aoMudar) aoMudar();
            });
            setInterval(() => {
                if (!permanente && restante > 0) {
                    restante--;
                    render();
                }
            }, 1000);
        },
        /* Sincroniza com o firmware. bt_discoverable=true com restante 0
         * significa visibilidade permanente (ligada nas Configurações). */
        update(s) {
            restante = s.bt_discoverable_remaining_s || 0;
            permanente = !!s.bt_discoverable && restante === 0;
            bloqueadoPorLista = (s.bt_allowed_count || 0) > 0;
            render();
        },
    };
})();
