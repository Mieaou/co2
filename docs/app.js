/**
 * =============================================================================
 * AEROSENSE — CO2 & Health Monitor Web Bluetooth Application
 * =============================================================================
 */

// Nordic UART Service (NUS) UUIDs
const NUS_SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
const NUS_TX_UUID      = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"; // Notify from board
const NUS_RX_UUID      = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"; // Write to board

// Application State
let bleDevice = null;
let bleServer = null;
let rxCharacteristic = null;
let txCharacteristic = null;
let isConnected = false;
let isDemoMode = false;
let demoTimer = null;
let incomingBuffer = "";

// Audio & Voice Alarm Settings
let isAudioEnabled = true;
let lastSpokenAlarmTime = 0;
let alarmCooldownMs = 60000; // Notify once per minute max

// History Data for Chart.js
const MAX_HISTORY_POINTS = 45;
const historyData = {
  timestamps: [],
  co2: [],
  temp: [],
  humidity: [],
  pressureMmHg: []
};
let activeChartType = "co2";
let telemetryChart = null;

// PPG Waveform Animation State
let ppgCanvas, ppgCtx;
let ppgPhase = 0;
let currentBpm = 72;
let hasFinger = false;

// =============================================================================
// INITIALIZATION
// =============================================================================

document.addEventListener("DOMContentLoaded", () => {
  initDOM();
  initChart();
  initCanvas();
  checkBluetoothSupport();

  // Register PWA Service Worker if supported
  if ("serviceWorker" in navigator && location.protocol === "https:") {
    navigator.serviceWorker.register("sw.js").catch(() => {});
  }
});

function initDOM() {
  // Connection Button
  const btnConnect = document.getElementById("btnConnect");
  btnConnect.addEventListener("click", () => {
    if (isConnected) {
      disconnectBLE();
    } else {
      connectBLE();
    }
  });

  // Quick Action Buttons
  document.getElementById("btnZeroAltitude").addEventListener("click", () => {
    sendBleCommand("z");
    showToast("Команда сброса высоты отправлена (0.0 м)");
    if (isDemoMode) {
      document.getElementById("valRelAltitude").textContent = "+0.00 м";
    }
  });

  document.getElementById("btnRefresh").addEventListener("click", () => {
    sendBleCommand("r");
    showToast("Запрос немедленного обновления отправлен");
  });

  document.getElementById("btnToggleAudio").addEventListener("click", toggleAudio);

  // Reboot Modal
  const rebootModal = document.getElementById("rebootModal");
  document.getElementById("btnReboot").addEventListener("click", () => {
    rebootModal.classList.remove("hidden");
  });

  document.getElementById("btnCancelReboot").addEventListener("click", () => {
    rebootModal.classList.add("hidden");
  });

  document.getElementById("btnConfirmReboot").addEventListener("click", () => {
    rebootModal.classList.add("hidden");
    sendBleCommand("b");
    showToast("Перезагрузка микроконтроллера выполнена!");
    setTimeout(() => {
      disconnectBLE();
    }, 1000);
  });

  // Demo Mode
  document.getElementById("btnDemoMode").addEventListener("click", toggleDemoMode);

  // Chart Tabs
  document.querySelectorAll(".chart-tab").forEach(tab => {
    tab.addEventListener("click", (e) => {
      document.querySelectorAll(".chart-tab").forEach(t => t.classList.remove("active"));
      e.target.classList.add("active");
      activeChartType = e.target.getAttribute("data-chart");
      updateChartDataset();
    });
  });
}

function checkBluetoothSupport() {
  if (!navigator.bluetooth) {
    const warning = document.getElementById("httpsWarning");
    warning.classList.remove("hidden");
  }
}

// =============================================================================
// BLUETOOTH LOW ENERGY (WEB BLUETOOTH)
// =============================================================================

async function connectBLE() {
  if (!navigator.bluetooth) {
    showToast("Web Bluetooth не поддерживается в этом браузере.");
    return;
  }

  try {
    showToast("Поиск устройства CO2-Health-Monitor...");

    bleDevice = await navigator.bluetooth.requestDevice({
      filters: [
        { name: "CO2-Health-Monitor" },
        { namePrefix: "CO2" }
      ],
      optionalServices: [NUS_SERVICE_UUID]
    });

    bleDevice.addEventListener("gattserverdisconnected", onDisconnected);

    showToast("Подключение к GATT серверу...");
    bleServer = await bleDevice.gatt.connect();

    const service = await bleServer.getPrimaryService(NUS_SERVICE_UUID);

    // TX Characteristic (incoming stream from device)
    txCharacteristic = await service.getCharacteristic(NUS_TX_UUID);
    await txCharacteristic.startNotifications();
    txCharacteristic.addEventListener("characteristicvaluechanged", handleBleNotification);

    // RX Characteristic (commands to device)
    try {
      rxCharacteristic = await service.getCharacteristic(NUS_RX_UUID);
    } catch (e) {
      console.warn("RX Characteristic unavailable:", e);
    }

    setConnectionStatus(true, bleDevice.name || "CO2-Health-Monitor");
    showToast(`Подключено к ${bleDevice.name || "устройству"}!`);

    // Stop demo if running
    if (isDemoMode) stopDemo();

  } catch (err) {
    console.error("BLE Connection error:", err);
    if (err.name !== "NotFoundError") {
      showToast("Ошибка подключения: " + err.message);
    }
    setConnectionStatus(false);
  }
}

function disconnectBLE() {
  if (bleDevice && bleDevice.gatt.connected) {
    bleDevice.gatt.disconnect();
  }
  onDisconnected();
}

function onDisconnected() {
  setConnectionStatus(false);
  showToast("Устройство отключено");
  rxCharacteristic = null;
  txCharacteristic = null;
}

function setConnectionStatus(connected, deviceName = "") {
  isConnected = connected;
  const pill = document.getElementById("connectionPill");
  const label = document.getElementById("connectionLabel");
  const btn = document.getElementById("btnConnect");
  const btnText = document.getElementById("btnConnectText");

  if (connected) {
    pill.className = "connection-status-pill connected";
    label.textContent = deviceName || "Подключено";
    btn.className = "btn btn-primary btn-active";
    btnText.textContent = "Отключить";
  } else {
    pill.className = "connection-status-pill disconnected";
    label.textContent = "Отключено";
    btn.className = "btn btn-primary";
    btnText.textContent = "Подключить";
  }
}

function handleBleNotification(event) {
  const value = event.target.value;
  const decoder = new TextDecoder("utf-8");
  const chunk = decoder.decode(value);

  incomingBuffer += chunk;

  // Split on newlines to parse complete JSON frames
  if (incomingBuffer.includes("\n")) {
    const lines = incomingBuffer.split("\n");
    // All lines except the last incomplete fragment
    for (let i = 0; i < lines.length - 1; i++) {
      const line = lines[i].trim();
      if (line.startsWith("{") && line.endsWith("}")) {
        try {
          const telemetry = JSON.parse(line);
          renderTelemetry(telemetry);
        } catch (e) {
          console.warn("JSON parse error on line:", line, e);
        }
      }
    }
    incomingBuffer = lines[lines.length - 1];
  }
}

async function sendBleCommand(cmdChar) {
  if (isDemoMode) return;
  if (!rxCharacteristic) {
    console.warn("BLE RX characteristic not available");
    return;
  }

  try {
    const encoder = new TextEncoder();
    const data = encoder.encode(cmdChar);
    await rxCharacteristic.writeValueWithoutResponse(data);
  } catch (err) {
    console.error("Failed to write command:", err);
  }
}

// =============================================================================
// TELEMETRY RENDERING & ALARM SYSTEM
// =============================================================================

function renderTelemetry(data) {
  // 1. CO2 Hero Gauge
  const co2 = data.co2_ppm || 0;
  document.getElementById("valCo2").textContent = co2 > 0 ? co2 : "---";

  updateCo2Gauge(co2, data.co2_warming_up);

  // 2. Climate Grid
  document.getElementById("valTemp").textContent = data.temp_c != null ? data.temp_c.toFixed(1) : "--.-";
  document.getElementById("valTempBmp").textContent = data.temp_bmp_c != null ? `${data.temp_bmp_c.toFixed(1)} °C` : "--.- °C";

  document.getElementById("valHum").textContent = data.humidity_pct != null ? data.humidity_pct.toFixed(1) : "--.-";
  document.getElementById("valAbsHum").textContent = data.abs_humidity_gm3 != null ? `${data.abs_humidity_gm3.toFixed(1)} г/м³` : "--.- г/м³";

  document.getElementById("valPressureMmHg").textContent = data.pressure_mmhg != null ? data.pressure_mmhg.toFixed(1) : "---.-";
  document.getElementById("valPressureHpa").textContent = data.pressure_hpa != null ? `${data.pressure_hpa.toFixed(1)} гПа` : "----.- гПа";

  document.getElementById("valAltitude").textContent = data.altitude_m != null ? data.altitude_m.toFixed(1) : "--.-";
  
  const relAlt = data.rel_altitude_m != null ? data.rel_altitude_m : 0.0;
  const relSign = relAlt >= 0 ? "+" : "";
  document.getElementById("valRelAltitude").textContent = `${relSign}${relAlt.toFixed(2)} м`;

  document.getElementById("valDewPoint").textContent = data.dew_point_c != null ? data.dew_point_c.toFixed(1) : "--.-";

  // Comfort Index based on Dew Point
  const dp = data.dew_point_c || 0;
  const comfortLabel = document.getElementById("comfortLabel");
  if (dp < 10) comfortLabel.textContent = "Сухой воздух";
  else if (dp <= 16) comfortLabel.textContent = "Идеальный комфорт";
  else if (dp <= 20) comfortLabel.textContent = "Влажно / Душно";
  else comfortLabel.textContent = "Высокая духота";

  // 3. Hardware Health Diagnostics
  updateHealthChip("chipScd", data.scd_ok);
  updateHealthChip("chipBmp", data.bmp_ok);
  updateHealthChip("chipMax", data.max_ok);

  // 4. Biometrics
  hasFinger = !!data.finger_detected;
  const fingerPill = document.getElementById("fingerStatusPill");
  const fingerDot = document.getElementById("fingerDot");
  const fingerText = document.getElementById("fingerStatusText");
  const heartIcon = document.getElementById("heartIcon");

  if (hasFinger) {
    fingerDot.classList.add("active");
  } else {
    fingerDot.classList.remove("active");
  }

  // FSM State mapping
  const fsmBadge = document.getElementById("fsmBadge");
  switch (data.ppg_state) {
    case 0:
      fingerText.textContent = "Приложите палец к сенсору";
      fsmBadge.textContent = "FSM: Ожидание";
      break;
    case 1:
      fingerText.textContent = "Авто-калибровка усиления (AGC)...";
      fsmBadge.textContent = "FSM: Калибровка";
      break;
    case 2:
      fingerText.textContent = `Накопление буфера (${data.buffer_pct || 0}%)...`;
      fsmBadge.textContent = `FSM: Буфер ${data.buffer_pct}%`;
      break;
    case 3:
      fingerText.textContent = "Активное отслеживание пульса";
      fsmBadge.textContent = "FSM: Активно";
      break;
    default:
      fingerText.textContent = "Сенсор активен";
      fsmBadge.textContent = "FSM: Норма";
  }

  // Heart Rate
  if (data.hr_valid && data.hr_bpm > 0) {
    currentBpm = data.hr_bpm;
    document.getElementById("valHeartRate").textContent = data.hr_bpm;
    heartIcon.classList.add("beating");
    // Synchronize CSS heart animation duration with actual BPM
    const beatSec = (60 / Math.max(40, Math.min(220, data.hr_bpm))).toFixed(2);
    heartIcon.style.animationDuration = `${beatSec}s`;
  } else {
    document.getElementById("valHeartRate").textContent = "--";
    heartIcon.classList.remove("beating");
  }

  // SpO2
  if (data.spo2_valid && data.spo2_pct > 0) {
    document.getElementById("valSpo2").textContent = data.spo2_pct;
  } else {
    document.getElementById("valSpo2").textContent = "--";
  }

  document.getElementById("valPi").textContent = data.perfusion_index != null ? `${data.perfusion_index.toFixed(2)} %` : "-- %";
  document.getElementById("valAgcDrive").textContent = data.led_brightness || 60;

  // 5. Append to Rolling History & Chart
  const timeLabel = new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' });
  pushHistory(timeLabel, co2, data.temp_c, data.humidity_pct, data.pressure_mmhg);

  // 6. Ventilation Voice Alarm Evaluation
  checkVentilationAlarm(co2, data.co2_warming_up);
}

function updateCo2Gauge(co2, isWarmingUp) {
  const arc = document.getElementById("co2GaugeArc");
  const badge = document.getElementById("co2Badge");
  const badgeText = document.getElementById("co2StatusText");
  const ambientGlow = document.getElementById("ambientCenterGlow");

  // Max stroke-dasharray = 398
  const MAX_ARC = 398;
  const MIN_CO2 = 400;
  const MAX_CO2 = 2200;

  let fraction = (co2 - MIN_CO2) / (MAX_CO2 - MIN_CO2);
  fraction = Math.max(0, Math.min(1, fraction));
  const offset = MAX_ARC * (1 - fraction);
  arc.style.strokeDashoffset = offset;

  if (isWarmingUp) {
    badge.className = "co2-status-badge status-good";
    badgeText.textContent = "Прогрев сенсора NDIR...";
    ambientGlow.style.background = "radial-gradient(circle, var(--color-green-glow) 0%, transparent 70%)";
    return;
  }

  if (co2 < 800) {
    badge.className = "co2-status-badge status-good";
    badgeText.textContent = "Идеальный воздух";
    ambientGlow.style.background = "radial-gradient(circle, var(--color-green-glow) 0%, transparent 70%)";
  } else if (co2 <= 1100) {
    badge.className = "co2-status-badge status-moderate";
    badgeText.textContent = "Норма (комфортно)";
    ambientGlow.style.background = "radial-gradient(circle, var(--color-yellow-glow) 0%, transparent 70%)";
  } else if (co2 <= 1500) {
    badge.className = "co2-status-badge status-warning";
    badgeText.textContent = "Внимание: Пора проветрить!";
    ambientGlow.style.background = "radial-gradient(circle, var(--color-orange-glow) 0%, transparent 70%)";
  } else {
    badge.className = "co2-status-badge status-danger";
    badgeText.textContent = "Опасно: Высокий CO₂!";
    ambientGlow.style.background = "radial-gradient(circle, var(--color-red-glow) 0%, transparent 70%)";
  }
}

function updateHealthChip(id, isOk) {
  const chip = document.getElementById(id);
  if (isOk) {
    chip.className = "health-chip chip-ok";
    chip.textContent = "В норме";
  } else {
    chip.className = "health-chip chip-off";
    chip.textContent = "Офлайн";
  }
}

// =============================================================================
// SMART VENTILATION ALARM & VOICE
// =============================================================================

function checkVentilationAlarm(co2, isWarmingUp) {
  if (isWarmingUp || !isAudioEnabled) return;

  const now = Date.now();
  if (co2 >= 1300 && (now - lastSpokenAlarmTime >= alarmCooldownMs)) {
    lastSpokenAlarmTime = now;
    playAlarmChime();
    speakAlert(`Внимание! Уровень углекислого газа ${co2} ppm. Пожалуйста, проветрите комнату.`);
  }
}

function speakAlert(text) {
  if (!("speechSynthesis" in window)) return;
  try {
    window.speechSynthesis.cancel();
    const utterance = new SpeechSynthesisUtterance(text);
    utterance.lang = "ru-RU";
    utterance.rate = 1.0;
    utterance.pitch = 1.0;
    window.speechSynthesis.speak(utterance);
  } catch (e) {
    console.warn("SpeechSynthesis error:", e);
  }
}

function playAlarmChime() {
  try {
    const audioCtx = new (window.AudioContext || window.webkitAudioContext)();
    const osc = audioCtx.createOscillator();
    const gain = audioCtx.createGain();

    osc.type = "sine";
    osc.frequency.setValueAtTime(587.33, audioCtx.currentTime); // D5
    osc.frequency.exponentialRampToValueAtTime(880, audioCtx.currentTime + 0.3); // A5

    gain.gain.setValueAtTime(0.2, audioCtx.currentTime);
    gain.gain.exponentialRampToValueAtTime(0.01, audioCtx.currentTime + 0.5);

    osc.connect(gain);
    gain.connect(audioCtx.destination);

    osc.start();
    osc.stop(audioCtx.currentTime + 0.5);
  } catch (e) {
    // AudioContext blocked before first user gesture
  }
}

function toggleAudio() {
  isAudioEnabled = !isAudioEnabled;
  const label = document.getElementById("audioLabel");
  const icon = document.getElementById("audioIcon");

  if (isAudioEnabled) {
    label.textContent = "Озвучка: Вкл";
    icon.innerHTML = `<polygon points="11 5 6 9 2 9 2 15 6 15 11 19 11 5"/><path d="M15.54 8.46a5 5 0 0 1 0 7.07M19.07 4.93a10 10 0 0 1 0 14.14"/>`;
    showToast("Голосовые предупреждения включены");
  } else {
    label.textContent = "Озвучка: Выкл";
    icon.innerHTML = `<polygon points="11 5 6 9 2 9 2 15 6 15 11 19 11 5"/><line x1="23" y1="9" x2="17" y2="15"/><line x1="17" y1="9" x2="23" y2="15"/>`;
    showToast("Голосовые предупреждения выключены");
  }
}

// =============================================================================
// CHART.JS REAL-TIME TELEMETRY PLOT
// =============================================================================

function initChart() {
  const ctx = document.getElementById("telemetryChart").getContext("2d");

  telemetryChart = new Chart(ctx, {
    type: "line",
    data: {
      labels: [],
      datasets: [
        {
          label: "CO2 (ppm)",
          data: [],
          borderColor: "#10b981",
          backgroundColor: "rgba(16, 185, 129, 0.12)",
          borderWidth: 2.5,
          tension: 0.35,
          fill: true,
          pointRadius: 2,
          pointHoverRadius: 6
        }
      ]
    },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      animation: { duration: 400 },
      plugins: {
        legend: {
          labels: { color: "#cbd5e1", font: { family: "Inter", size: 12 } }
        },
        tooltip: {
          backgroundColor: "rgba(15, 23, 42, 0.9)",
          borderColor: "rgba(255, 255, 255, 0.1)",
          borderWidth: 1,
          titleColor: "#f8fafc",
          bodyColor: "#cbd5e1"
        }
      },
      scales: {
        x: {
          grid: { color: "rgba(255, 255, 255, 0.05)" },
          ticks: { color: "#64748b", maxTicksLimit: 8, font: { size: 10 } }
        },
        y: {
          grid: { color: "rgba(255, 255, 255, 0.05)" },
          ticks: { color: "#64748b", font: { size: 11 } }
        }
      }
    }
  });
}

function pushHistory(timestamp, co2, temp, hum, press) {
  historyData.timestamps.push(timestamp);
  historyData.co2.push(co2 || 0);
  historyData.temp.push(temp || 0);
  historyData.humidity.push(hum || 0);
  historyData.pressureMmHg.push(press || 0);

  if (historyData.timestamps.length > MAX_HISTORY_POINTS) {
    historyData.timestamps.shift();
    historyData.co2.shift();
    historyData.temp.shift();
    historyData.humidity.shift();
    historyData.pressureMmHg.shift();
  }

  updateChartDataset();
}

function updateChartDataset() {
  if (!telemetryChart) return;

  telemetryChart.data.labels = [...historyData.timestamps];

  if (activeChartType === "co2") {
    telemetryChart.data.datasets = [
      {
        label: "CO2 (ppm)",
        data: [...historyData.co2],
        borderColor: "#10b981",
        backgroundColor: "rgba(16, 185, 129, 0.12)",
        borderWidth: 2.5,
        tension: 0.35,
        fill: true,
        pointRadius: 2
      }
    ];
  } else if (activeChartType === "climate") {
    telemetryChart.data.datasets = [
      {
        label: "Температура (°C)",
        data: [...historyData.temp],
        borderColor: "#fb923c",
        borderWidth: 2,
        tension: 0.35,
        fill: false,
        pointRadius: 2
      },
      {
        label: "Влажность (%)",
        data: [...historyData.humidity],
        borderColor: "#38bdf8",
        borderWidth: 2,
        tension: 0.35,
        fill: false,
        pointRadius: 2
      }
    ];
  } else if (activeChartType === "pressure") {
    telemetryChart.data.datasets = [
      {
        label: "Давление (мм рт. ст.)",
        data: [...historyData.pressureMmHg],
        borderColor: "#a855f7",
        backgroundColor: "rgba(168, 85, 247, 0.12)",
        borderWidth: 2,
        tension: 0.35,
        fill: true,
        pointRadius: 2
      }
    ];
  }

  telemetryChart.update("none");
}

// =============================================================================
// REAL-TIME PPG CANVAS WAVEFORM ANIMATION
// =============================================================================

function initCanvas() {
  ppgCanvas = document.getElementById("ppgCanvas");
  if (!ppgCanvas) return;
  ppgCtx = ppgCanvas.getContext("2d");

  // Handle Retina display sharpness
  const dpr = window.devicePixelRatio || 1;
  const rect = ppgCanvas.getBoundingClientRect();
  ppgCanvas.width = rect.width * dpr;
  ppgCanvas.height = 90 * dpr;
  ppgCtx.scale(dpr, dpr);

  requestAnimationFrame(drawPpgLoop);
}

function drawPpgLoop() {
  if (!ppgCtx) return;

  const w = ppgCanvas.clientWidth;
  const h = 90;

  ppgCtx.clearRect(0, 0, w, h);

  // Background Grid Lines
  ppgCtx.strokeStyle = "rgba(255, 255, 255, 0.04)";
  ppgCtx.lineWidth = 1;
  for (let x = 0; x < w; x += 30) {
    ppgCtx.beginPath();
    ppgCtx.moveTo(x, 0);
    ppgCtx.lineTo(x, h);
    ppgCtx.stroke();
  }
  for (let y = 0; y < h; y += 22) {
    ppgCtx.beginPath();
    ppgCtx.moveTo(0, y);
    ppgCtx.lineTo(w, y);
    ppgCtx.stroke();
  }

  if (hasFinger) {
    // Physiological PPG Pulse Waveform with Dicrotic Notch
    ppgPhase += (currentBpm / 60) * 0.08;

    ppgCtx.strokeStyle = "#ef4444";
    ppgCtx.lineWidth = 2.5;
    ppgCtx.shadowColor = "rgba(239, 68, 68, 0.6)";
    ppgCtx.shadowBlur = 10;
    ppgCtx.beginPath();

    for (let x = 0; x < w; x++) {
      const t = (x / 45) - ppgPhase;
      const cycle = (t % (2 * Math.PI) + (2 * Math.PI)) % (2 * Math.PI);

      // Physiological cardiac reflection model
      let wave = 0;
      if (cycle < Math.PI * 0.8) {
        wave = Math.sin(cycle * 1.25);
      } else if (cycle < Math.PI * 1.3) {
        // Dicrotic notch depression + secondary reflection peak
        wave = 0.25 * Math.sin((cycle - Math.PI * 0.8) * 3);
      } else {
        wave = 0;
      }

      const y = (h / 2) + 12 - (wave * (h * 0.38));
      if (x === 0) ppgCtx.moveTo(x, y);
      else ppgCtx.lineTo(x, y);
    }
    ppgCtx.stroke();
    ppgCtx.shadowBlur = 0;

  } else {
    // Quiescent flat baseline with gentle ambient shimmer
    ppgPhase += 0.02;
    ppgCtx.strokeStyle = "rgba(100, 116, 139, 0.4)";
    ppgCtx.lineWidth = 1.5;
    ppgCtx.beginPath();
    for (let x = 0; x < w; x++) {
      const y = (h / 2) + Math.sin((x * 0.04) + ppgPhase) * 2;
      if (x === 0) ppgCtx.moveTo(x, y);
      else ppgCtx.lineTo(x, y);
    }
    ppgCtx.stroke();
  }

  requestAnimationFrame(drawPpgLoop);
}

// =============================================================================
// DEMO / SIMULATION MODE (FOR TESTING WITHOUT PHYSICAL BOARD)
// =============================================================================

function toggleDemoMode() {
  if (isDemoMode) {
    stopDemo();
  } else {
    startDemo();
  }
}

function startDemo() {
  if (isConnected) disconnectBLE();

  isDemoMode = true;
  document.getElementById("btnDemoMode").classList.add("chip-danger");
  document.getElementById("demoLabel").textContent = "Остановить демо";
  setConnectionStatus(true, "Демо-эмулятор (XIAO)");
  showToast("Запущен демо-поток реалистичных данных");

  let simTimeMs = 10000;
  let simCo2 = 680;
  let simTemp = 23.4;
  let simHum = 45.0;

  demoTimer = setInterval(() => {
    simTimeMs += 3000;
    // Gradual realistic drift
    simCo2 += (Math.random() * 40 - 15);
    simTemp += (Math.random() * 0.2 - 0.1);
    simHum += (Math.random() * 0.4 - 0.2);

    const mockPacket = {
      time_ms: simTimeMs,
      co2_ppm: Math.round(simCo2),
      co2_status: simCo2 < 800 ? "Good" : (simCo2 < 1200 ? "Fair" : "Poor"),
      co2_warming_up: false,
      temp_c: simTemp,
      temp_bmp_c: simTemp - 0.3,
      humidity_pct: simHum,
      dew_point_c: 10.8,
      abs_humidity_gm3: 9.3,
      pressure_hpa: 1013.2 + (Math.random() * 0.4 - 0.2),
      pressure_mmhg: 760.0,
      altitude_m: 14.5,
      rel_altitude_m: 0.05,
      finger_detected: true,
      ppg_state: 3,
      buffer_pct: 100,
      hr_bpm: 68 + Math.round(Math.random() * 8),
      hr_bpm_raw: 69,
      hr_valid: true,
      spo2_pct: 98,
      spo2_raw: 98,
      spo2_valid: true,
      perfusion_index: 2.34,
      led_brightness: 60,
      bmp_ok: true,
      scd_ok: true,
      max_ok: true
    };

    renderTelemetry(mockPacket);
  }, 3000);
}

function stopDemo() {
  isDemoMode = false;
  clearInterval(demoTimer);
  document.getElementById("btnDemoMode").classList.remove("chip-danger");
  document.getElementById("demoLabel").textContent = "Демо-режим";
  setConnectionStatus(false);
  showToast("Демо-режим остановлен");
}

// =============================================================================
// UTILITIES
// =============================================================================

function showToast(message) {
  const toast = document.getElementById("toastNotification");
  const msg = document.getElementById("toastMessage");
  msg.textContent = message;
  toast.classList.add("show");
  clearTimeout(toast._timeout);
  toast._timeout = setTimeout(() => {
    toast.classList.remove("show");
  }, 3200);
}
