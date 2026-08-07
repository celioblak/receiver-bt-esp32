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
