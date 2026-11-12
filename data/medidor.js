(() => {
  'use strict';

  const POLL_MS = 200;
  const HISTORY_LEN = 300;
  const STALE_MS = 1500; // no good measurement for this long -> "sem sinal"

  const $ = (id) => document.getElementById(id);
  const el = {
    link: $('link'), linkText: $('linkText'),
    value: $('value'), unitTag: $('unitTag'), tareTag: $('tareTag'),
    raw: $('sRaw'), min: $('sMin'), max: $('sMax'), rate: $('sRate'), fail: $('sFail'),
    histRange: $('histRange'),
    tape: $('tape'), chart: $('chart'),
    btnTare: $('btnTare'), btnUnit: $('btnUnit'), btnClear: $('btnClear'),
    btnSerial: $('btnSerial'), hint: $('hint'),
    readout: document.querySelector('.readout'),
  };

  const css = getComputedStyle(document.documentElement);
  const color = (name) => css.getPropertyValue(name).trim();
  const C = {
    tape: color('--tape'), tapeInk: color('--tape-ink'), hot: color('--hot'),
    dim: color('--dim'), line: color('--line'), ink: color('--ink'),
  };
  const MONO = css.getPropertyValue('--mono');

  const state = {
    unit: 'm',      // 'm' | 'cm'
    tare: 0,        // meters subtracted from every reading
    avg: null,      // latest averaged distance (m, before tare)
    raw: null,
    a: null, b: null, // each board's own single-sided distance (m, before tare)
    shown: null,    // animated value drawn on the tape (m, after tare)
    min: null, max: null,
    history: [],    // averaged values in meters (before tare)
    lastCount: null, lastCountAt: 0, rate: null,
  };

  const fmt = (m, withUnit = true) => {
    if (m === null || !isFinite(m)) return '--';
    let s = state.unit === 'm' ? m.toFixed(2) : (m * 100).toFixed(0);
    if (Number(s) === 0) s = s.replace('-', ''); // no "-0.00"
    return withUnit ? `${s} ${state.unit}` : s;
  };

  function setLink(stateName, text) {
    el.link.dataset.state = stateName;
    el.linkText.textContent = text;
    el.readout.classList.toggle('stale', stateName !== 'ok');
  }

  function render() {
    const v = state.avg === null ? null : state.avg - state.tare;
    el.value.textContent = v === null ? '--.--' : fmt(v, false);
    el.unitTag.textContent = state.unit;
    el.raw.textContent = fmt(state.raw === null ? null : state.raw - state.tare);
    el.min.textContent = fmt(state.min === null ? null : state.min - state.tare);
    el.max.textContent = fmt(state.max === null ? null : state.max - state.tare);
    el.rate.textContent = state.rate === null ? '--' : `${state.rate.toFixed(1)} Hz`;
    // Each board's own measurement and how far apart they are.
    const stale = el.link.dataset.state !== 'ok';
    const a = stale ? null : state.a, b = stale ? null : state.b;
    $('cA').textContent = fmt(a === null ? null : a - state.tare);
    $('cB').textContent = fmt(b === null ? null : b - state.tare);
    const diff = a !== null && b !== null ? a - b : null;
    const cDiff = $('cDiff');
    cDiff.textContent = diff === null ? '--' : `${(diff * 100).toFixed(1)} cm`;
    cDiff.classList.toggle('warn', diff !== null && Math.abs(diff) > 0.1);

    el.tareTag.hidden = state.tare === 0;
    el.tareTag.textContent = `tara ${fmt(state.tare)}`;
    el.btnUnit.textContent = `Unidade: ${state.unit}`;
    drawChart();
  }

  // ---------- polling ----------
  // ---------- USB serial (Web Serial API) ----------
  // The boards print the same status JSON on the serial port ~10 times a second, so the
  // app also works over the USB cable, e.g. after the board switched its Wi-Fi off.
  // Web Serial needs Chrome/Edge and a secure page: open data/index.html from the PC
  // (file://) or from localhost; it is not available on http://192.168.4.1.
  const serial = { port: null, reader: null, active: false };

  function hint(text) {
    el.hint.textContent = text;
    el.hint.hidden = !text;
  }

  // Serial data arrives in arbitrary chunks: collect complete lines and apply the JSON ones.
  let lineBuffer = '';
  let lastSerialDataAt = 0;

  function feedSerial(chunk) {
    lineBuffer += chunk;
    const lines = lineBuffer.split('\n');
    lineBuffer = lines.pop();
    if (lineBuffer.length > 4096) lineBuffer = ''; // garbage without line breaks
    for (const line of lines) {
      const text = line.trim();
      if (!text.startsWith('{')) continue; // boot messages and other text
      try {
        apply(JSON.parse(text));
        lastSerialDataAt = performance.now();
      } catch (e) { /* partial line */ }
    }
  }

  // Port open but nothing readable arriving (board off, wrong device, old firmware).
  setInterval(() => {
    if (serial.active && performance.now() - lastSerialDataAt > 2000) setLink('nosignal', 'usb sem dados');
  }, 1000);

  // ---------- USB serial inside the Android app (Capacitor + native UsbSerial plugin) ----------
  // Same plugin as the other Android serial apps: the port belongs to a service, so it
  // stays open while the app is in the background and is re-synced when it comes back.
  const cap = window.Capacitor;
  const isNative = !!(cap && cap.isNativePlatform && cap.isNativePlatform() && window.capacitorExports);
  const UsbSerial = isNative ? window.capacitorExports.registerPlugin('UsbSerial') : null;
  const native = { busy: false, wanted: false, handles: [], retryTimer: null };
  const CP210X_VENDOR_ID = 0x10C4; // USB bridge on the Makerfabs board
  const NATIVE_RETRY_MS = 1500;

  async function nativeListen() {
    await nativeUnlisten();
    native.handles.push(await UsbSerial.addListener('dataReceived', ({ data }) => feedSerial(data)));
    native.handles.push(await UsbSerial.addListener('readError', ({ error }) => nativeLost(`Erro de leitura: ${error}`)));
  }

  async function nativeUnlisten() {
    const handles = native.handles;
    native.handles = [];
    await Promise.all(handles.map((h) => h.remove().catch(() => undefined)));
  }

  function nativeSetConnected(connected) {
    serial.active = connected;
    el.btnSerial.textContent = connected ? 'Desconectar USB' : 'Conectar USB';
    if (connected) {
      lineBuffer = '';
      lastSerialDataAt = performance.now();
      setLink('wait', 'usb conectado');
    } else {
      setLink('down', 'usb desconectado');
    }
  }

  async function nativeConnect() {
    if (native.busy || serial.active) return;
    native.busy = true;
    native.wanted = true;
    clearTimeout(native.retryTimer);
    native.retryTimer = null;
    setLink('wait', 'conectando usb');
    try {
      const { devices = [] } = await UsbSerial.listDevices();
      if (!devices.length) {
        throw new Error('Nenhum dispositivo USB serial encontrado. Confira o cabo OTG e se a placa está ligada.');
      }
      // With several serial devices plugged in, prefer the board's CP210x bridge.
      const device = devices.find((d) => d.vendorId === CP210X_VENDOR_ID) || devices[0];
      const selector = { vendorId: device.vendorId, productId: device.productId };

      const permission = await UsbSerial.requestPermission(selector);
      if (!permission.granted) throw new Error('Permissão USB negada.');

      await UsbSerial.open({ ...selector, baudRate: 115200, dataBits: 8, stopBits: 1, parity: 0 });
      await nativeListen();
      nativeSetConnected(true);
      hint('');
    } catch (e) {
      const message = e && e.message ? e.message : String(e);
      nativeSetConnected(false);
      hint(message === 'Permission denied' ? 'Permissão USB negada.' : message);
    } finally {
      native.busy = false;
    }
  }

  async function nativeDisconnect() {
    native.wanted = false;
    clearTimeout(native.retryTimer);
    native.retryTimer = null;
    try {
      await nativeUnlisten();
      await UsbSerial.close();
    } catch (e) { /* already closed */ }
    nativeSetConnected(false);
  }

  // The port died (cable pulled, read error, closed in the background): retry once
  // after a short delay, like the other apps do; no retry loop.
  function nativeLost(message) {
    nativeSetConnected(false);
    if (message) hint(message);
    if (!native.wanted || native.retryTimer) return;
    native.retryTimer = setTimeout(async () => {
      native.retryTimer = null;
      try {
        const { devices = [] } = await UsbSerial.listDevices();
        if (devices.length) await nativeConnect();
      } catch (e) { /* stay disconnected */ }
    }, NATIVE_RETRY_MS);
  }

  // Coming back from the background: ask the native side what really happened to the port.
  async function nativeResync() {
    if (native.busy) return;
    try {
      const status = await UsbSerial.getStatus();
      if (status.isOpen && !serial.active) {
        await nativeListen();
        native.wanted = true;
        nativeSetConnected(true);
      } else if (!status.isOpen && serial.active) {
        nativeLost('A porta foi fechada enquanto o app estava em segundo plano.');
      }
    } catch (e) { /* plugin not ready yet */ }
  }

  async function nativeInit() {
    nativeSetConnected(false);
    await UsbSerial.addListener('deviceAttached', () => {
      hint('');
      if (!serial.active) nativeConnect(); // plug the cable -> connect
    });
    await UsbSerial.addListener('deviceDetached', () => {
      nativeUnlisten();
      nativeSetConnected(false);
      hint('Dispositivo USB removido.');
    });
    document.addEventListener('resume', nativeResync);
    document.addEventListener('visibilitychange', () => { if (!document.hidden) nativeResync(); });

    // The service may still hold the port from before (app reopened); otherwise connect
    // right away if a board is already plugged in.
    await nativeResync();
    if (!serial.active) {
      try {
        const { devices = [] } = await UsbSerial.listDevices();
        if (devices.length) await nativeConnect();
      } catch (e) { /* stay disconnected */ }
    }
  }

  async function serialConnect() {
    if (isNative) return nativeConnect();
    if (!('serial' in navigator)) {
      hint(window.isSecureContext
        ? 'Este navegador não tem Web Serial. Use Chrome ou Edge (no Brave, ative em brave://flags a opção "Web Serial API").'
        : 'A serial USB não funciona pela página servida pela placa. Abra o arquivo data/index.html do projeto direto no PC, no Chrome ou Edge.');
      return;
    }
    try {
      const port = await navigator.serial.requestPort();
      await port.open({ baudRate: 115200 });
      // Keep both lines released so opening the port does not hold the ESP32 in reset.
      await port.setSignals({ dataTerminalReady: false, requestToSend: false });
      serial.port = port;
      serial.active = true;
      el.btnSerial.textContent = 'Desconectar USB';
      hint('');
      serialRead();
    } catch (e) {
      if (e.name !== 'NotFoundError') hint(`Não foi possível abrir a porta: ${e.message}`);
    }
  }

  async function serialRead() {
    const decoder = new TextDecoder();
    lineBuffer = '';
    try {
      while (serial.active && serial.port.readable) {
        serial.reader = serial.port.readable.getReader();
        try {
          for (;;) {
            const { value, done } = await serial.reader.read();
            if (done) break;
            feedSerial(decoder.decode(value, { stream: true }));
          }
        } finally {
          serial.reader.releaseLock();
        }
        if (!serial.active) break;
      }
    } catch (e) {
      hint(`Conexão USB perdida: ${e.message}`);
    }
    serialDisconnect();
  }

  async function serialDisconnect() {
    if (!serial.port) return;
    serial.active = false;
    const port = serial.port;
    serial.port = null;
    try { if (serial.reader) await serial.reader.cancel(); } catch (e) { /* already closed */ }
    try { await port.close(); } catch (e) { /* already closed */ }
    el.btnSerial.textContent = 'Serial USB';
    if (noBoardServer) setLink('down', 'sem placa');
    else setLink('wait', 'conectando');
  }

  let apiMissing = 0;        // consecutive 404s from /api/status
  let noBoardServer = false; // true once we know no board is serving this page

  async function poll() {
    if (noBoardServer && !serial.active) return; // nothing to poll; serial is the only source
    if (serial.active || isNative) { // data arrives over USB; the Android app has no HTTP server to poll
      setTimeout(poll, POLL_MS);
      return;
    }
    try {
      const ctrl = new AbortController();
      const timer = setTimeout(() => ctrl.abort(), 1000);
      const res = await fetch('/api/status', { signal: ctrl.signal, cache: 'no-store' });
      clearTimeout(timer);
      if (res.status === 404 && ++apiMissing >= 3) {
        // The page is not being served by a board (hosted copy, e.g. GitHub Pages).
        noBoardServer = true;
        setLink('down', 'sem placa');
        hint('Esta página não está sendo servida pela placa. Ligue a placa no cabo USB e use o botão Serial USB.');
        return;
      }
      if (!res.ok) throw new Error(res.status);
      apply(await res.json());
    } catch (e) {
      if (!serial.active) setLink('down', 'sem conexão');
    } finally {
      setTimeout(poll, POLL_MS);
    }
  }

  function apply(d) {
    el.fail.textContent = d.fails;
    if (d.role) {
      const wifi = d.wifi === false ? ' · wi-fi desligado' : '';
      $('sub').textContent = `DW3000 · DS-TWR · canal 5 · ${d.role}${wifi}`;
    }
    state.a = typeof d.a === 'number' ? d.a : null;
    state.b = typeof d.b === 'number' ? d.b : null;

    const now = performance.now();
    if (state.lastCount !== null && now - state.lastCountAt >= 1000) {
      state.rate = (d.count - state.lastCount) / ((now - state.lastCountAt) / 1000);
      state.lastCount = d.count;
      state.lastCountAt = now;
    } else if (state.lastCount === null) {
      state.lastCount = d.count;
      state.lastCountAt = now;
    }

    if (!d.valid || d.age > STALE_MS) {
      setLink('nosignal', 'sem sinal uwb');
      render();
      return;
    }

    setLink('ok', serial.active ? 'medindo · usb' : 'medindo');
    state.avg = d.avg;
    state.raw = d.dist;
    state.min = state.min === null ? d.avg : Math.min(state.min, d.avg);
    state.max = state.max === null ? d.avg : Math.max(state.max, d.avg);
    state.history.push(d.avg);
    if (state.history.length > HISTORY_LEN) state.history.shift();
    render();
  }

  // ---------- canvas helpers ----------
  function fit(canvas) {
    const dpr = window.devicePixelRatio || 1;
    const w = canvas.clientWidth, h = canvas.clientHeight;
    if (canvas.width !== Math.round(w * dpr) || canvas.height !== Math.round(h * dpr)) {
      canvas.width = Math.round(w * dpr);
      canvas.height = Math.round(h * dpr);
    }
    const ctx = canvas.getContext('2d');
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    return { ctx, w, h };
  }

  // Tape measure that slides under a fixed marker.
  function drawTape() {
    const { ctx, w, h } = fit(el.tape);
    ctx.fillStyle = C.tape;
    ctx.fillRect(0, 0, w, h);

    const center = state.shown === null ? 0 : state.shown;

    // Meters: tick every 5 cm, label every 0.5 m. Centimeters: zoomed in ~7x,
    // tick every 1 cm, label every 10 cm. The zoom between the two is animated.
    const cmMode = state.unit === 'cm';
    const targetPxPerM = cmMode ? Math.max(1300, w * 3) : Math.max(180, w * 0.42);
    state.pxPerM = state.pxPerM ? state.pxPerM + (targetPxPerM - state.pxPerM) * 0.15 : targetPxPerM;
    const pxPerM = state.pxPerM;
    const stepCm = pxPerM >= 700 ? 1 : 5;
    const majorCm = stepCm === 1 ? 10 : 50;
    const midCm = stepCm === 1 ? 5 : 10;
    const first = Math.floor(((center - w / 2 / pxPerM) * 100) / stepCm) - 1;
    const last = Math.ceil(((center + w / 2 / pxPerM) * 100) / stepCm) + 1;

    ctx.fillStyle = C.tapeInk;
    ctx.font = `700 15px ${MONO}`;
    ctx.textAlign = 'center';
    ctx.textBaseline = 'bottom';

    for (let i = first; i <= last; i++) {
      const cm = i * stepCm;
      const x = Math.round(w / 2 + (cm / 100 - center) * pxPerM);
      const major = cm % majorCm === 0;
      const mid = cm % midCm === 0;
      const len = major ? 30 : mid ? 18 : 10;
      ctx.fillRect(x - (major ? 1.5 : 0.75), 0, major ? 3 : 1.5, len);
      if (major) {
        const label = cmMode ? String(cm) : (cm / 100).toFixed(1);
        ctx.fillText(label, x, h - 8);
      }
    }

    // fixed marker
    ctx.fillStyle = C.hot;
    ctx.fillRect(w / 2 - 1.5, 0, 3, h);
    ctx.beginPath();
    ctx.moveTo(w / 2 - 9, 0);
    ctx.lineTo(w / 2 + 9, 0);
    ctx.lineTo(w / 2, 12);
    ctx.closePath();
    ctx.fill();
  }

  function drawChart() {
    const { ctx, w, h } = fit(el.chart);
    ctx.clearRect(0, 0, w, h);

    const data = state.history;
    if (data.length < 2) {
      ctx.fillStyle = C.dim;
      ctx.font = `12px ${MONO}`;
      ctx.textAlign = 'center';
      ctx.fillText('aguardando medições…', w / 2, h / 2);
      el.histRange.textContent = '';
      return;
    }

    let lo = Math.min(...data), hi = Math.max(...data);
    const pad = Math.max((hi - lo) * 0.15, state.unit === 'cm' ? 0.01 : 0.05);
    lo -= pad; hi += pad;

    const X = (i) => (i / (HISTORY_LEN - 1)) * w;
    const Y = (v) => h - ((v - lo) / (hi - lo)) * h;
    const x0 = HISTORY_LEN - data.length;

    ctx.strokeStyle = C.line;
    ctx.lineWidth = 1;
    ctx.setLineDash([3, 5]);
    for (let g = 1; g < 4; g++) {
      const y = Math.round((h * g) / 4) + 0.5;
      ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(w, y); ctx.stroke();
    }
    ctx.setLineDash([]);

    ctx.beginPath();
    data.forEach((v, i) => (i ? ctx.lineTo(X(x0 + i), Y(v)) : ctx.moveTo(X(x0 + i), Y(v))));
    ctx.strokeStyle = C.tape;
    ctx.lineWidth = 2;
    ctx.lineJoin = 'round';
    ctx.stroke();

    ctx.lineTo(w, h); ctx.lineTo(X(x0), h); ctx.closePath();
    const grad = ctx.createLinearGradient(0, 0, 0, h);
    grad.addColorStop(0, 'rgba(245, 184, 31, 0.28)');
    grad.addColorStop(1, 'rgba(245, 184, 31, 0)');
    ctx.fillStyle = grad;
    ctx.fill();

    ctx.fillStyle = C.hot;
    ctx.beginPath();
    ctx.arc(w - 3, Y(data[data.length - 1]), 3.5, 0, Math.PI * 2);
    ctx.fill();

    el.histRange.textContent = `${fmt(lo + pad - state.tare)} – ${fmt(hi - pad - state.tare)}`;
  }

  function animate() {
    if (state.avg !== null) {
      const target = state.avg - state.tare;
      state.shown = state.shown === null ? target : state.shown + (target - state.shown) * 0.18;
    }
    drawTape();
    requestAnimationFrame(animate);
  }

  // ---------- controls ----------
  el.btnTare.addEventListener('click', () => {
    state.tare = state.tare === 0 && state.avg !== null ? state.avg : 0;
    el.btnTare.textContent = state.tare === 0 ? 'Zerar aqui' : 'Remover tara';
    render();
  });

  el.btnUnit.addEventListener('click', () => {
    state.unit = state.unit === 'm' ? 'cm' : 'm';
    render();
  });

  el.btnClear.addEventListener('click', () => {
    state.history = [];
    state.min = state.max = null;
    render();
  });

  el.btnSerial.addEventListener('click', () => {
    if (!serial.active) serialConnect();
    else if (isNative) nativeDisconnect();
    else serialDisconnect();
  });

  window.addEventListener('resize', drawChart);

  render();
  animate();
  poll();
  if (isNative) nativeInit();

  // Installable/offline only where the page is hosted over https (not on the boards,
  // which serve plain http, and not inside the Android app).
  if (!isNative && location.protocol === 'https:' && 'serviceWorker' in navigator) {
    navigator.serviceWorker.register('sw.js').catch(() => { /* app works without it */ });
  }
})();
