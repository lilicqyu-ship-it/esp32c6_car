/* SmartDrive control page - speaks proto v2 over WS binary (SDD 6) */
"use strict";

/* ---- proto v2 constants (mirror components/c6_proto) ---- */
const P_SYNC1 = 0xAA, P_SYNC2 = 0x55, P_VER = 0x02;
const CMD = { DRIVE:0x50, TELEMETRY:0x41 };
const TELEMETRY_LEN = 38;

/* CRC16-CCITT-FALSE, check("123456789")==0x29B1 */
function crc16(buf) {
  let crc = 0xFFFF;
  for (const b of buf) {
    crc ^= b << 8;
    for (let i = 0; i < 8; i++) crc = (crc & 0x8000) ? ((crc << 1) ^ 0x1021) : (crc << 1);
    crc &= 0xFFFF;
  }
  return crc;
}

let seq = 0;
function buildFrame(cmd, data) {
  const head = [P_SYNC1, P_SYNC2, P_VER, cmd, (++seq) & 0xFF, data.length];
  const body = head.concat(Array.from(data));
  const c = crc16(body);
  return new Uint8Array(body.concat([(c >> 8) & 0xFF, c & 0xFF]));
}

function u16le(v) { return [v & 0xFF, (v >> 8) & 0xFF]; }
function i16le(v) { v = Math.max(-32768, Math.min(32767, v|0)); return u16le(v & 0xFFFF); }

/* ---- state ---- */
const $ = (id) => document.getElementById(id);
/* state line text + tone (CSS: .state[data-tone=ok|warn|bad|dim]) */
function setState(msg, tone) { $("state").textContent = msg; $("state").dataset.tone = tone; }
const state = {
  token: sessionStorage.getItem("sd_token") || new URLSearchParams(location.search).get("token") || "",
  ws: null, ctrl: false, tc: false,
  driveTimer: null, joyV: 0, joyW: 0,
};

/* ---- WebSocket ---- */
function wsUrl() {
  const p = (location.protocol === "https:") ? "wss://" : "ws://";
  return p + location.host + "/ws" + (state.token ? ("?token=" + state.token) : "");
}
let wsBackoff = 1000;
function connect() {
  state.ws = new WebSocket(wsUrl());
  state.ws.binaryType = "arraybuffer";
  state.ws.onopen = () => {
    wsBackoff = 1000;
    $("dot_ws").className = "dot on";
    sendDrive(0, 0);
  };
  state.ws.onclose = () => {
    $("dot_ws").className = "dot off"; state.ctrl = false;
    // fixed 1 s retries churn sockets on the device while it is struggling;
    // back off so recovery is not fought by the page itself
    setTimeout(connect, wsBackoff);
    wsBackoff = Math.min(wsBackoff * 2, 8000);
  };
  state.ws.onmessage = (ev) => {
    if (typeof ev.data === "string") { onCtl(JSON.parse(ev.data)); return; }
    const d = new Uint8Array(ev.data);
    if (d.length >= 8 && d[0] === P_SYNC1 && d[1] === P_SYNC2 && d[3] === CMD.TELEMETRY) {
      onTelemetry(d.subarray(6, 6 + d[5]));
    }
  };
}

let errT = 0;                     /* pending "错误:" auto-clear timer */
function onCtl(m) {
  if (m.t === "hello") {    state.ctrl = (m.role === "ctrl");
    $("ver").textContent = "fw " + m.ver;
  } else if (m.t === "tc") {
    state.tc = !!m.on;
    $("dot_tc").className = "dot " + (m.on ? "on" : "off");
    setState(m.on ? "待命" : "车端未连接", m.on ? "ok" : "dim");
  } else if (m.t === "otastatus") {
    $("ota_progress").textContent = "OTA " + m.pct + "%";
    $("ota_bar").style.width = Math.max(0, Math.min(100, m.pct)) + "%";
  } else if (m.t === "otaswap") {
    $("ota_progress").textContent = "TC275 切槽重启...";
    $("ota_bar").style.width = "100%";
  } else if (m.t === "otaerror") {
    $("ota_progress").textContent = "OTA 失败 " + m.e;
    $("ota_bar").style.width = "0%";
  } else if (m.t === "err") {
    // transient hiccup (e.g. command queue momentarily full): show it, then
    // let the state line recover instead of latching a stale error forever
    setState("错误: " + m.e, "warn");
    clearTimeout(errT);
    errT = setTimeout(() => {
      setState(state.tc ? "待命" : "车端未连接", state.tc ? "ok" : "dim");
    }, 3000);
  }
}

/* ---- telemetry 0x41 (LE fields, SDD 6.3) ---- */
function onTelemetry(p) {
  if (p.length < TELEMETRY_LEN) return;
  const dv = (o) => p[o] | (p[o+1] << 8);
  const dl = (o) => (dv(o) << 16) >> 16;                       // sign
  const d32 = (o) => (p[o] | (p[o+1] << 8) | (p[o+2] << 16) | (p[o+3] << 24)) >>> 0;
  const tl = dl(11), tr = dl(13), ml = dl(15), mr = dl(17);
  const pct = p[21], fault = dv(9);
  renderSpeed(ml, mr);
  setBar("bar_lt", tl, 800); setBar("bar_lm", ml, 800);
  setBar("bar_rt", tr, 800); setBar("bar_rm", mr, 800);
  $("v_lt").textContent = ml; $("v_rt").textContent = mr;
  $("batt_pct").textContent = pct + "%";
  $("batt_v").textContent = (dv(19) / 1000).toFixed(2) + "V";
  $("batt_pill").style.color = pct <= 10 ? "var(--bad)" : pct <= 20 ? "var(--warn)" : "var(--ok)";
  $("odo").textContent = (d32(26) / 1000).toFixed(1);
  $("rtt").textContent = "rtt " + dv(30) + "ms";
  if (fault) setState("故障 0x" + fault.toString(16), "bad");
  else setState(state.tc ? "待命" : "车端未连接", state.tc ? "ok" : "dim");
}
function setBar(id, v, full) {
  const el = $(id); const w = Math.min(50, Math.abs(v) / full * 50);
  el.classList.toggle("neg", v < 0);
  el.style.width = w + "%";
  if (v >= 0) el.style.left = "50%"; else el.style.left = "auto";
}

/* ---- speedometer: body speed = mean of measured wheel speeds (mm/s) ----
 * signed average -> in-place rotation reads 0; km/h with 1 decimal because
 * full joystick deflection is only 600 mm/s = 2.2 km/h.  The display value is
 * a dt-aware EMA (tau 150 ms): one 0.1 km/h digit is ~28 mm/s, so raw 50 Hz
 * readings flicker the digit constantly and the direction line flaps near
 * zero.  Snap instead of ramp on the first frame, after a transmission gap,
 * and whenever crossing in/out of rest. */
const STOP_MM_S = 30;                          /* < 0.1 km/h counts as stopped */
const SPEED_TAU_MS = 150;                      /* EMA time constant */
let teleTs = 0, dispV = 0, dispSeeded = false;
const speedCache = { v: "", d: "" };
function renderSpeed(ml, mr) {
  const now = Date.now();
  const dt = teleTs ? Math.min(1000, now - teleTs) : 0;
  teleTs = now;
  const v = (ml + mr) / 2;
  if (!dispSeeded || dt > 400 || Math.abs(v) < STOP_MM_S) {
    dispV = v; dispSeeded = true;
  } else {
    dispV += (v - dispV) * (1 - Math.exp(-dt / SPEED_TAU_MS));
  }
  const stopped = Math.abs(dispV) < STOP_MM_S;
  const sv = stopped ? "0.0" : (Math.abs(dispV) * 0.0036).toFixed(1);
  const dir = stopped ? "" : (dispV > 0 ? "▲ 前进" : "▼ 倒车");
  if (sv !== speedCache.v) { $("speed_val").textContent = sv; speedCache.v = sv; }
  if (dir !== speedCache.d) {
    $("speed_dir").textContent = dir || "\u00a0";
    $("speed_dir").className = stopped ? "" : (dispV > 0 ? "fwd" : "rev");
    speedCache.d = dir;
  }
  $("speed_val").classList.remove("stale");
}
/* staleness guard: 50 Hz nominal, so 1 s without telemetry means the chain
 * (TC275 -> SPI -> C6 -> WS) is broken somewhere - freeze the number as "--"
 * instead of letting a stale speed keep looking live */
setInterval(() => {
  if (Date.now() - teleTs <= 1000 || teleTs === 0) return;
  if (speedCache.v !== "--") {
    speedCache.v = "--"; speedCache.d = "";
    dispSeeded = false;                        /* resync snaps on next frame */
    $("speed_val").textContent = "--";
    $("speed_val").classList.add("stale");
    $("speed_dir").textContent = "\u00a0";
    $("speed_dir").className = "";
  }
}, 500);

/* ---- drive: joystick -> DRIVE 0x50 {v:i16, w:i16} at 30 Hz ---- */
function sendDrive(v, w) {
  if (state.ws && state.ws.readyState === 1 && state.ctrl) {
    state.ws.send(buildFrame(CMD.DRIVE, new Uint8Array([...i16le(v), ...i16le(w)])));
  }
}
const joy = $("joy"), knob = $("knob");
function joyMove(ev) {
  const r = joy.getBoundingClientRect();
  const t = ev.touches ? ev.touches[0] : ev;
  let dx = t.clientX - (r.left + r.width / 2), dy = t.clientY - (r.top + r.height / 2);
  const max = r.width / 2 - knob.offsetWidth / 2 - 2, len = Math.hypot(dx, dy);
  if (len > max) { dx *= max / len; dy *= max / len; }
  knob.style.transform = `translate(${dx}px,${dy}px)`;
  state.joyV = Math.round(-dy / max * 600);       // mm/s, up = forward
  state.joyW = Math.round(-dx / max * 300);       // deg/s
}
function joyEnd() {
  knob.style.transform = ""; state.joyV = 0; state.joyW = 0;
}
["pointerdown", "pointermove", "pointerup", "pointerleave"].forEach((e) => {
  joy.addEventListener(e, (ev) => {
    if (e === "pointerdown") { joy.setPointerCapture(ev.pointerId); joy.classList.add("active"); }
    if (e === "pointerup" || e === "pointerleave") { joyEnd(); joy.classList.remove("active"); }
    else joyMove(ev);
  });
});
setInterval(() => sendDrive(state.joyV, state.joyW), 33);   // 30 Hz, doubles as heartbeat

/* ---- pairing ---- */
$("btn_pair").onclick = async () => {
  const r = await fetch("/api/pair", { method: "POST" });
  const j = await r.json();
  if (j.ok && j.token) {
    state.token = j.token; sessionStorage.setItem("sd_token", j.token);
    setState("已配对 (控制端)", "ok");
    if (state.ws) state.ws.close();
  } else {
    setState("配对失败: " + (j.hint || j.e || "先按车侧键3秒"), "warn");
  }
};

/* ---- OTA upload (control token required) ---- */
async function upload(file, uri, btn) {
  if (!file) return;
  btn.disabled = true;
  try {
    const r = await fetch(uri + (state.token ? ("?token=" + state.token) : ""),
      { method: "POST", body: file });
    const j = await r.json();
    $("ota_progress").textContent = j.ok ? "完成，设备将重启" : "失败 " + j.e;
    $("ota_bar").style.width = j.ok ? "100%" : "0%";
  } catch (e) { $("ota_progress").textContent = "上传中断"; $("ota_bar").style.width = "0%"; }
  btn.disabled = false;
}
$("btn_ota_c6").onclick = () => upload($("file_c6").files[0], "/ota/c6", $("btn_ota_c6"));
$("btn_ota_tc").onclick = () => upload($("file_tc").files[0], "/ota/tc275", $("btn_ota_tc"));

/* picked firmware name inside the styled drop label */
[["file_c6", "lbl_c6", "选择 C6 固件 (.bin)"],
 ["file_tc", "lbl_tc", "选择 TC275 固件 (.bin)"]].forEach(([f, l, d]) => {
  $(f).addEventListener("change", () => { $(l).textContent = $(f).files[0] ? $(f).files[0].name : d; });
});

/* ---- stop ---- */
$("btn_stop").onclick = () => sendDrive(0, 0);

fetch("/api/health").then((r) => r.json()).then((j) => {
  $("ssid").textContent = "SmartDrive";
  $("ver").textContent = "fw " + j.ver;
}).catch(() => {});
connect();
