/* 台架标定页（doc/17 M1 + V1.1 §8）- proto v2 over WS。
 * 助手（crc16/buildFrame/wsUrl/hello）为 app.js 的自包含复制，不 import 共享模块，
 * 避免标定页与控制页互相回归（doc/17 §2.1）。 */
"use strict";

/* ---- proto v2 constants (mirror components/c6_proto) ---- */
const P_SYNC1 = 0xAA, P_SYNC2 = 0x55, P_VER = 0x02;
/* DPT 命令族（doc/17 §8.4）。字节 0x70 双语义：c6_car 侧叫 PROTO_CMD_DPT_ENTER
 * （components/c6_proto/proto_frames.h:78），myCar 侧叫 PROTO_CMD_DPT_CAL_DIR
 * （mw/proto/protocol.h:33）——对 myCar main 基线的实际语义是编码器判向标定。
 * 0x70 不是心跳，不能当驾驶保活用；0x75~0x79 TC275 侧未实现，本页禁止发送。 */
const CMD = { DRIVE: 0x50, TELEMETRY: 0x41,
              CAL_DIR: 0x70, MOTOR_JOG: 0x71, REC_GET: 0x72, REC_SET: 0x73, REC_CLEAR: 0x74 };
const TELEMETRY_LEN = 38;
const JOG_DUTY = 500;                            /* percent*10 = ±50%，与固件钳位一致 */
const POS_NAMES = ["前左", "前右", "后左", "后右"];   /* doc/17 §8.4 pos 编码 */
const SLOT_OF_POS = ["wh_fl", "wh_fr", "wh_rl", "wh_rr"];
const WHEEL_CX = { wh_fl: 24, wh_fr: 146, wh_rl: 24, wh_rr: 146 };
const WHEEL_CY = { wh_fl: 79, wh_fr: 79, wh_rl: 183, wh_rr: 183 };
const MOTOR_NAMES = ["A", "B", "C", "D"];
/* REC 默认位置映射（A前左/B后左/C后右/D前右）；生效值以 0x23 EVT 回传为准 */
const POS_DEFAULT = [0, 2, 3, 1];

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
function setState(msg, tone) { $("state").textContent = msg; $("state").dataset.tone = tone; }
const state = {
  token: sessionStorage.getItem("sd_token") || new URLSearchParams(location.search).get("token") || "",
  ws: null, wsOn: false, ctrl: false, tc: false,
  running: false, lastTrigger: 0, runTimer: 0,     /* 判向标定状态机 */
  calib: { done: false, status: -1, invert: null }, /* ② 最近一轮判向结果 */
  jog: { motor: -1, dir: 0, timer: 0 },            /* 逐电机点动（同一时刻至多一路） */
  jogged: false,                                   /* ③ 是否点动复核过 */
  jogGated: false,                                 /* ③ 故障锁存门禁，跃变时刷一次按钮 */
  rec: null, recPending: "", recOk: false,         /* 0x23 生效参数 + 回执提示 */
  tele: { ml: 0, mr: 0, tl: 0, tr: 0, fault: 0, ts: 0 },
};
const RUN_WINDOW_MS = 3000;                      /* 发帧后的互锁窗口（doc/17 §2.3/§2.4）；进度条 1.4s 见 calib.html #caltbl_bar */

/* ---- 流程骨架（doc/17 §9）：①前提 → ②判向 → ③复核 → ④落库 ----
 * 前提四项任一不满足就不给开始标定；步骤条只做引导，不额外引入约束。 */
const PRE = [
  { li: "pr_ws",   t: "连接",   ok: () => state.wsOn },
  { li: "pr_ctrl", t: "控制权", ok: () => state.ctrl },
  { li: "pr_tc",   t: "车在线", ok: () => state.tc },
  { li: "pr_air",  t: "四轮离地", ok: () => $("ck_airborne").checked },
];
function preOk() { return PRE.every((p) => p.ok()); }
function setStep(n, cls) { $("st_" + n).className = "step" + (cls ? " " + cls : ""); }
function refreshFlow() {
  const miss = [];
  for (const p of PRE) {
    const good = p.ok(), el = $(p.li);
    el.className = good ? "ok" : "miss";
    el.firstElementChild.textContent = good ? "✓" : "○";
    if (!good) miss.push(p.t);
  }
  $("pre_need").textContent = miss.length ? "还差：" + miss.join("、") : "四项已满足，可做第②步";
  setStep(1, preOk() ? "done" : "act");
  setStep(2, state.calib.done ? "done" : (state.calib.status > 0 ? "bad" : (preOk() ? "act" : "")));
  setStep(3, state.jogged ? "done" : (jogFaultGated() ? "bad" : (state.calib.done ? "act" : "")));
  setStep(4, state.recOk === 1 ? "done" : (state.recOk === 2 ? "bad" : (state.calib.done ? "act" : "")));
  refreshCalibBtn(); refreshJogBtns(); refreshRecBtns();
}

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
    state.wsOn = true;
    $("dot_ws").className = "dot on";
    sendDrive(0, 0, true);                       /* 建链即清零目标，等价 joyEnd */
    refreshFlow();
  };
  state.ws.onclose = () => {
    $("dot_ws").className = "dot off"; state.ctrl = false; state.wsOn = false;
    jogStop();
    calibAbortOnDisconnect();
    refreshFlow();
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

function sendCmd(cmd, data) {
  if (!state.ctrl || !state.ws || state.ws.readyState !== 1) return false;
  state.ws.send(buildFrame(cmd, data));
  return true;
}

let errT = 0;
function onCtl(m) {
  if (m.t === "hello") {
    state.ctrl = (m.role === "ctrl");
    $("ver").textContent = "fw " + m.ver;
    setState(state.ctrl ? "已连接 (控制端)" : "已连接 (观察端，需配对)", state.ctrl ? "ok" : "warn");
    refreshFlow();
    if (state.ctrl) requestRec();                /* 进入页面即拉当前生效参数（§8.3） */
  } else if (m.t === "tc") {
    state.tc = !!m.on;
    $("dot_tc").className = "dot " + (m.on ? "on" : "off");
    if (state.ctrl) setState(state.tc ? "已连接 (控制端)" : "车端未连接", state.tc ? "ok" : "dim");
    refreshFlow();
  } else if (m.t === "cal") {
    onCalibResult(m);
  } else if (m.t === "rec") {
    onRec(m);
  } else if (m.t === "err") {
    setState("错误: " + m.e, "warn");
    clearTimeout(errT);
    errT = setTimeout(() => {
      setState(state.ctrl ? (state.tc ? "已连接 (控制端)" : "车端未连接")
                          : "已连接 (观察端，需配对)",
               state.ctrl ? (state.tc ? "ok" : "dim") : "warn");
    }, 3000);
  }
}

/* ---- telemetry 0x41 (LE fields, 与控制页同偏移) ---- */
function onTelemetry(p) {
  if (p.length < TELEMETRY_LEN) return;
  const dv = (o) => p[o] | (p[o+1] << 8);
  const dl = (o) => (dv(o) << 16) >> 16;
  const tl = dl(11), tr = dl(13), ml = dl(15), mr = dl(17);
  state.tele = { ml, mr, tl, tr, fault: dv(9), ts: Date.now() };
  setBar("bar_lt", tl, 800); setBar("bar_lm", ml, 800);
  setBar("bar_rt", tr, 800); setBar("bar_rm", mr, 800);
  $("v_lm").textContent = ml; $("v_rm").textContent = mr;
}
function setBar(id, v, full) {
  const el = $(id); const w = Math.min(50, Math.abs(v) / full * 50);
  el.classList.toggle("neg", v < 0);
  el.style.width = w + "%";
  if (v >= 0) el.style.left = "50%"; else el.style.left = "auto";
}

/* ---- drive: 本页无摇杆，仅 0 目标心跳 + STOP。
 * 标定 running / jog 期间周期发送被抑制（doc/17 §2.3-4、§8.1）：抑制的是新增
 * 驾驶意图，心跳值恒为 (0,0)，标定结束瞬间 TC275 恢复执行的最新目标即 0/0，不突跳。
 * STOP 永远可用（force，不受互锁限制）。 */
function sendDrive(v, w, force) {
  if ((state.running || state.jog.motor >= 0) && !force) return;
  if (state.ctrl) sendCmd(CMD.DRIVE, new Uint8Array([...i16le(v), ...i16le(w)]));
}
setInterval(() => sendDrive(0, 0, false), 33);   /* 30 Hz 心跳（驾驶保活），与标定/jog 互斥抑制 */

/* ================= 判向标定（doc/17 §2） =================
 * idle → confirm → sending → running(3s) → done/timeout */
function refreshCalibBtn() {
  $("btn_calib").disabled =
    state.running || state.jog.motor >= 0 || !preOk() ||
    (Date.now() - state.lastTrigger < RUN_WINDOW_MS);
}
$("ck_airborne").addEventListener("change", refreshFlow);

/* 进度条：走条 1.4s（CSS transition），复位走瞬时 */
function setProgress(run) {
  const bar = $("caltbl_bar");
  if (run) {
    bar.style.width = "100%";
  } else {
    bar.style.transition = "none"; bar.style.width = "0";
    void bar.offsetWidth;                          /* 强制回流，下一次走条 transition 生效 */
    bar.style.transition = "";
  }
}

function startCalib() {
  if (state.running || state.jog.motor >= 0 || !preOk()) return;
  if (Date.now() - state.lastTrigger < RUN_WINDOW_MS) return;   /* 防连点 */
  if (!state.ws || state.ws.readyState !== 1) return;
  if (!confirm("判向标定将驱动车轮逐个转动（每轮约 250ms，共约 1.4s）。\n" +
               "请确认车辆已四轮离地、周围无人。确认开始？")) return;

  state.running = true;
  state.lastTrigger = Date.now();
  state.calib = { done: false, status: -1, invert: null };      /* 本轮结果待回传 */
  sendCmd(CMD.CAL_DIR, new Uint8Array(0));         /* 一次确认只发一帧 0x70 */
  $("calib_msg").textContent = "标定进行中：车轮将逐个短暂转动…";
  setProgress(true);
  refreshFlow();
  clearTimeout(state.runTimer);
  state.runTimer = setTimeout(calibTimeout, RUN_WINDOW_MS);
}
$("btn_calib").onclick = startCalib;

function calibEnd() {
  state.running = false;
  clearTimeout(state.runTimer);
  setProgress(0);
  refreshFlow();
}
function calibTimeout() {                           /* 无结果回传时的降级（doc/17 §2.4） */
  calibEnd();
  $("calib_msg").textContent =
    "结果回传未启用（需 myCar 固件 M2/M3'）：请在 TC275 调试串口查看 ENCCAL= 行（invert[0..3] delta[0..3]）。";
}
function calibAbortOnDisconnect() {
  if (!state.running) return;
  calibEnd();
  $("calib_msg").textContent = "WS 已断开，本次标定状态未知：请查看 TC275 串口 ENCCAL= 行。";
}

/* ---- 结果表：EVT 0x22 {"t":"cal",status,saved,invert[4],delta[4]}（doc/17 §4.2/§8.3） ---- */
const SAVED_TXT = { 0: "未持久化", 1: "已写 DFlash", 2: "DFlash 写入失败⚠" };
function onCalibResult(m) {
  const reasons = { 1: "急停中止", 2: "忙：已有标定在跑" };
  const st = m.status | 0;
  calibEnd();                                       /* 内部会 refreshFlow，需在其后落状态 */
  state.calib = { done: st === 0, status: st, invert: m.invert.slice(0, 4) };
  for (let i = 0; i < 4; i++) {
    const d = m.delta[i] | 0, inv = m.invert[i] | 0;
    $("d" + i).textContent = (d > 0 ? "+" : "") + d;
    $("j" + i).textContent = "→" + (inv < 0 ? "-1" : "+1");
    const s = $("s" + i), v = $("jv" + i);
    let txt, cls;
    $("d" + i).className = "";
    if (d === 0) {
      /* status!=0 时 delta=0 是"该轮尚未测到"（固件对未测轮填 0），不是死通道 */
      if (st === 0) {
        txt = "无计数：查编码器接线"; cls = "bad"; $("d" + i).className = "bad";
      } else {
        txt = "未测（本轮未执行）"; cls = "dim";
      }
    } else if (inv < 0) {
      txt = "已翻转"; cls = "flip";
    } else {
      txt = "正常"; cls = "okv";
    }
    s.textContent = txt; s.className = cls;
    /* 同一结论落到③的 jog 行旁，点动复核时不必回头翻表 */
    v.textContent = "② " + txt + "（计数 " + (inv < 0 ? "-1" : "+1") + "）";
    v.className = "jver " + cls;
  }
  if (st !== 0) {
    $("calib_msg").textContent = "标定未完成：status=" + st +
      "（" + (reasons[st] || "未知状态") + "）" +
      (st === 1 ? "，下表为已测轮结果" : "");
    refreshFlow();
    return;
  }
  $("calib_msg").textContent = "标定完成（" + (SAVED_TXT[m.saved | 0] || "保存状态未知") +
    "）。③ 可点动复核真轮转向，④ 生效参数刷新中…";
  requestRec();                                     /* 判向自动持久化后回读（§8.3） */
  refreshFlow();
}

/* ================= ③ 逐电机点动复核（doc/17 §8.1） =================
 * 0x71 MOTOR_JOG {motor u8, duty i16LE}，按住 30Hz 刷新，松手发 duty=0 一次；
 * 固件侧 300ms 无刷新自动停（双层保险）。与判向标定互斥。 */
function jogSend(motor, duty) {
  sendCmd(CMD.MOTOR_JOG, new Uint8Array([motor, ...i16le(duty)]));
  $("jd" + motor).textContent = duty;
}
function jogStart(motor, dir) {
  if (!state.ctrl || state.running || state.jog.motor >= 0) return;
  state.jog.motor = motor; state.jog.dir = dir;
  state.jogged = true;
  jogSend(motor, dir * JOG_DUTY);
  state.jog.timer = setInterval(() => jogSend(state.jog.motor, state.jog.dir * JOG_DUTY), 33);
  refreshFlow();
}
function jogStop() {
  if (state.jog.motor < 0) return;
  clearInterval(state.jog.timer);
  const m = state.jog.motor;
  state.jog.motor = -1; state.jog.dir = 0;
  jogSend(m, 0);                                   /* 松手补发 0（断线时 sendCmd 自带守卫） */
  refreshFlow();
}
function refreshJogBtns() {
  const gated = jogFaultGated();
  for (let m = 0; m < 4; m++) {
    const mine = state.jog.motor === m;
    $("jn" + m).classList.toggle("on", mine && state.jog.dir < 0);
    $("jp" + m).classList.toggle("on", mine && state.jog.dir > 0);
    const dis = !state.ctrl || state.running || (state.jog.motor >= 0 && !mine) || gated;
    $("jn" + m).disabled = dis;
    $("jp" + m).disabled = dis;
  }
  for (let s = 0; s < 4; s++) {
    const el = $(SLOT_OF_POS[s]);
    if (el) el.classList.toggle("jogging", state.jog.motor >= 0 &&
      motorOfSlot(s) === state.jog.motor);
  }
  const hint = $("car_hint");
  hint.textContent = gated
    ? "⚠ 故障锁存中：车端会拒绝 jog（doc/17 §8.1），请先排除故障。轮上字母按 0x23 回传的位置动态标注。"
    : "遥测超过 1s 未更新则置灰；红框为故障锁存。轮上字母按 0x23 回传的位置动态标注。";
}
/* 故障门禁只看"新鲜遥测"：台架上没跑起来时 tele 为空，不该因此锁死按钮 */
function jogFaultGated() {
  return state.tele.ts && (Date.now() - state.tele.ts <= 1000) && state.tele.fault !== 0;
}
for (let m = 0; m < 4; m++) {
  $("jn" + m).addEventListener("pointerdown", (e) => { e.preventDefault(); jogStart(m, -1); });
  $("jp" + m).addEventListener("pointerdown", (e) => { e.preventDefault(); jogStart(m, 1); });
}
["pointerup", "pointercancel"].forEach((e) =>
  window.addEventListener(e, () => jogStop()));
document.addEventListener("visibilitychange", () => { if (document.hidden) jogStop(); });

/* ================= ④ 参数与 DFlash 持久化（doc/17 §8.3） ================= */
function requestRec() { sendCmd(CMD.REC_GET, new Uint8Array(0)); }

function motorOfSlot(slot) {                        /* pos 值→电机号；无 rec 时用默认映射 */
  const pos = state.rec ? state.rec.pos : POS_DEFAULT;
  return pos.indexOf(slot);
}

/* ② 的判定与 ④ 的生效方向是否一致——不一致说明标定没落库或被 0x73 覆盖 */
function recVsCalib(m) {
  const c = state.calib.invert;
  if (!c || !m) return "";
  const bad = [];
  for (let i = 0; i < 4; i++) {
    if (((c[i] | 0) < 0) !== ((m.invert[i] | 0) < 0)) bad.push(MOTOR_NAMES[i]);
  }
  return bad.length ? " · 与②判定不一致⚠（" + bad.join("/") + "）" : " · 与②判定一致";
}

function onRec(m) {
  state.rec = m;
  const srcTxt = ["默认值", "DFlash", "在线设置"][m.src | 0] || ("src=" + m.src);
  const diff = recVsCalib(m);
  state.recOk = m.crcOk ? 1 : 2;                    /* 1 已回读且 CRC 正常，2 CRC 异常 */
  $("rec_src").textContent = "当前生效参数 · 数据来源：" + srcTxt +
    (m.crcOk ? "" : " · CRC 异常⚠") + diff +
    "（位置编码 0前左/1前右/2后左/3后右；方向 -1=已翻转，生效于编码器计数）";
  for (let i = 0; i < 4; i++) {
    $("q" + i).textContent = POS_NAMES[m.pos[i] & 3] || "?";
    $("p" + i).textContent = $("q" + i).textContent;
    const w = $("w" + i);
    w.textContent = (m.invert[i] < 0) ? "-1 已翻转" : "+1 正常";
    w.className = (m.invert[i] < 0) ? "flip" : "okv";
    $("jl" + i).textContent = MOTOR_NAMES[i] + " · " + $("q" + i).textContent;
  }
  if (document.activeElement !== $("in_fs")) $("in_fs").value = m.fullScale;
  if (document.activeElement !== $("in_wd")) $("in_wd").value = m.wheelDia;
  if (state.recPending) { $("rec_msg").textContent = state.recPending; state.recPending = ""; }
  renderCarLabels();
  refreshFlow();
}

function refreshRecBtns() {
  const dis = !state.ctrl || state.running || state.jog.motor >= 0 || !state.rec;
  $("btn_rec_set").disabled = dis;
  $("btn_rec_clear").disabled = dis;
}

$("btn_rec_set").onclick = () => {
  if (!state.rec) return;
  const fs = $("in_fs").value | 0, wd = $("in_wd").value | 0;
  if (fs < 100 || fs > 5000) { $("rec_msg").textContent = "fullScale 需 100..5000 mm/s"; return; }
  if (wd < 30 || wd > 200)   { $("rec_msg").textContent = "wheelDia 需 30..200 mm"; return; }
  /* 0x73 REC_SET {pos u8x4, invert i8x4, fullScale i16, wheelDia i16}（12B） */
  const p = new Uint8Array(12);
  for (let i = 0; i < 4; i++) { p[i] = state.rec.pos[i] & 3; p[4 + i] = state.rec.invert[i] & 0xFF; }
  p.set(i16le(fs), 8); p.set(i16le(wd), 10);
  if (sendCmd(CMD.REC_SET, p)) {
    state.recPending = "已发送 REC_SET，等待 EVT 0x23 回执…";
    $("rec_msg").textContent = state.recPending;
  }
};
$("btn_rec_clear").onclick = () => {
  if (sendCmd(CMD.REC_CLEAR, new Uint8Array(0))) {
    state.recPending = "已发送 REC_CLEAR，等待默认值回执…";
    $("rec_msg").textContent = state.recPending;
  }
};

/* ============ ③ 车辆实时监视（doc/17 §8.2，与点动复核同屏） ============
 * 遥测是侧级：左槽（前左/后左）显示 vMeasL，右槽显示 vMeasR。
 * rAF 累积角度；>1s 无遥测置灰；故障红框；jog 轮高亮。 */
function renderCarLabels() {
  for (let s = 0; s < 4; s++) {
    const m = motorOfSlot(s);
    const t = $("lb_" + ["fl", "fr", "rl", "rr"][s]);
    if (t) t.textContent = m >= 0 ? MOTOR_NAMES[m] : "?";
  }
}
const wheelAng = [0, 0, 0, 0];                     /* 按槽位累积，deg */
let animLast = 0;
function animTick(ts) {
  const dt = animLast ? Math.min(200, ts - animLast) : 0;
  animLast = ts;
  const car = $("car");
  const fresh = state.tele.ts && (Date.now() - state.tele.ts <= 1000);
  car.classList.toggle("stale", !fresh);
  car.classList.toggle("estop", fresh && state.tele.fault !== 0);
  const gated = !!jogFaultGated();                 /* 遥测每帧来，门禁只在跃变时刷按钮 */
  if (gated !== state.jogGated) { state.jogGated = gated; refreshFlow(); }
  const v = [state.tele.ml, state.tele.mr];        /* 侧级：[左, 右] */
  for (let s = 0; s < 4; s++) {
    const side = (s === 0 || s === 2) ? 0 : 1;     /* fl/rl 左，fr/rr 右 */
    wheelAng[s] += v[side] * 0.5 * (dt / 1000) * 3.6;  /* 视觉增益，非真实轮径换算 */
    const id = SLOT_OF_POS[s];
    $(id).setAttribute("transform",
      `rotate(${wheelAng[s].toFixed(1)} ${WHEEL_CX[id]} ${WHEEL_CY[id]})`);
  }
  const body = (v[0] + v[1]) / 2;
  $("arrow").classList.toggle("rev", fresh && body < -30);
  $("arrow").style.opacity = (!fresh || Math.abs(body) < 30) ? .25 : .9;
  requestAnimationFrame(animTick);
}
requestAnimationFrame(animTick);
renderCarLabels();
refreshFlow();                                     /* 首屏先把①前提清单与步骤条画出来 */

/* ---- STOP / 配对 ---- */
$("btn_stop").onclick = () => { jogStop(); sendDrive(0, 0, true); };

$("btn_pair").onclick = async () => {
  const r = await fetch("/api/pair", { method: "POST" });
  const j = await r.json();
  if (j.ok && j.token) {
    state.token = j.token; sessionStorage.setItem("sd_token", j.token);
    setState("已配对，重连中…", "ok");
    if (state.ws) state.ws.close();
  } else {
    setState("配对失败: " + (j.hint || j.e || "先按车侧键3秒"), "warn");
  }
};

fetch("/api/health").then((r) => r.json()).then((j) => {
  $("ver").textContent = "fw " + j.ver;
}).catch(() => {});
connect();
