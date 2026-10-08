/* ============================================================================
   LOGICA CLIENTE WEB - TELEMETRIA EN TIEMPO REAL CON ALARMAS Y GRAFICOS
   ============================================================================ */

// Estado del Cliente
let socket = null;
let charts = {};
let isMuted = false;
let audioInterval = null;
let historicalData = [];
let currentHoursFilter = 1;
let pollingInterval = null;
let socketRetryDelay = 1000;
const CHART_WINDOW_SIZE = 60;
const MAX_CHART_POINTS = 2000;
const CHART_ANIMATION_MS = 450;
let chartViewMode = "live";
let chartWindowStart = 0;
let chartSeries = {
    labels: [],
    temp: [],
    hum: [],
    ppm: [],
    co2: [],
    dist: []
};
let lastReadingKey = null;

// Umbrales por defecto (se recuperan de localStorage)
let thresholds = {
    gas: 50,
    dist: 90,
    tempMax: 38
};

let reactorCalibration = {
    emptyCm: 200,
    fullCm: 20
};

let distanceCalibration = {
    offsetCm: 0,
    scale: 1
};

function getDefaultServerUrl() {
    const host = window.location.hostname;
    if (!host || host === "localhost" || host === "127.0.0.1") {
        return "";
    }
    return `${window.location.protocol}//${host}:8000/api/readings`;
}

function buildCommandDeliveryMessage(result) {
    if (!result) return "No se pudo contactar el servidor web.";
    if (result.serial_sent) {
        return "Comando enviado por USB serial al ESP32.";
    }
    return "Comando guardado en cola WiFi. Si el ESP32 aun no esta conectado por WiFi, conecta el USB serial y vuelve a guardar.";
}

function parseTimestamp(timestamp) {
    if (!timestamp) return new Date();
    if (typeof timestamp === "number") return new Date(timestamp * 1000);

    const normalized = String(timestamp).includes("+") || String(timestamp).endsWith("Z")
        ? String(timestamp)
        : `${timestamp}Z`;
    return new Date(normalized);
}

function formatNumber(value, decimals = 1, fallback = "--") {
    if (value === null || value === undefined || value === "") return fallback;
    const numberValue = Number(value);
    return Number.isFinite(numberValue) ? numberValue.toFixed(decimals) : fallback;
}

function getSeriesValue(value) {
    if (value === null || value === undefined || value === "") return null;
    const numberValue = Number(value);
    return Number.isFinite(numberValue) ? numberValue : null;
}

function getReadingLabel(reading) {
    const date = reading?.timestamp ? parseTimestamp(reading.timestamp) : new Date();
    return date.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' });
}

function getReadingKey(reading) {
    if (!reading) return "";
    if (reading.uptime !== undefined && reading.uptime !== null) {
        return [
            reading.uptime,
            reading.temp,
            reading.hum,
            reading.ppm,
            reading.co2_ppm,
            reading.dist
        ].join("|");
    }
    return `${reading.timestamp ?? ""}-${reading.source ?? ""}`;
}

function getLevelPercent(data) {
    const numericLevel = Number(data?.level_pct);
    if (Number.isFinite(numericLevel)) {
        return Math.max(0, Math.min(100, numericLevel));
    }

    const dist = Number(data?.dist);
    const empty = Number(data?.reactor_empty_cm) || reactorCalibration.emptyCm;
    const full = Number(data?.reactor_full_cm) || reactorCalibration.fullCm;
    const span = empty - full;
    if (!Number.isFinite(dist) || span <= 0) return 0;

    return Math.max(0, Math.min(100, ((empty - dist) / span) * 100));
}

function setChartLiveValues(data) {
    document.getElementById("chart-temp-val").innerText = formatNumber(data.temp, 1, "--.-");
    document.getElementById("chart-hum-val").innerText = formatNumber(data.hum, 1, "--.-");
    document.getElementById("chart-gas-val").innerText = formatNumber(data.ppm, 0, "----");
    document.getElementById("chart-co2-val").innerText = formatNumber(data.co2_ppm, 0, "----");
    document.getElementById("chart-dist-val").innerText = formatNumber(getLevelPercent(data), 1, "--.-");
}

function trimChartSeries() {
    const overflow = chartSeries.labels.length - MAX_CHART_POINTS;
    if (overflow <= 0) return;

    chartSeries.labels.splice(0, overflow);
    chartSeries.temp.splice(0, overflow);
    chartSeries.hum.splice(0, overflow);
    chartSeries.ppm.splice(0, overflow);
    chartSeries.co2.splice(0, overflow);
    chartSeries.dist.splice(0, overflow);
    chartWindowStart = Math.max(0, chartWindowStart - overflow);
}

function renderChartWindow() {
    const total = chartSeries.labels.length;
    const windowSize = Math.min(CHART_WINDOW_SIZE, total);
    const maxStart = Math.max(0, total - windowSize);

    if (chartViewMode === "live") {
        chartWindowStart = maxStart;
    } else {
        chartWindowStart = Math.max(0, Math.min(chartWindowStart, maxStart));
    }

    const start = chartWindowStart;
    const end = start + windowSize;

    charts.tempHum.data.labels = chartSeries.labels.slice(start, end);
    charts.tempHum.data.datasets[0].data = chartSeries.temp.slice(start, end);
    charts.tempHum.data.datasets[1].data = chartSeries.hum.slice(start, end);
    charts.tempHum.update();

    charts.gas.data.labels = chartSeries.labels.slice(start, end);
    charts.gas.data.datasets[0].data = chartSeries.ppm.slice(start, end);
    charts.gas.update();

    charts.co2.data.labels = chartSeries.labels.slice(start, end);
    charts.co2.data.datasets[0].data = chartSeries.co2.slice(start, end);
    charts.co2.update();

    charts.dist.data.labels = chartSeries.labels.slice(start, end);
    charts.dist.data.datasets[0].data = chartSeries.dist.slice(start, end);
    charts.dist.update();

    const rangeLabel = document.getElementById("chart-range-label");
    rangeLabel.innerText = total === 0 ? "0 de 0" : `${start + 1}-${Math.min(end, total)} de ${total}`;

    document.getElementById("chart-prev").disabled = total === 0 || start === 0;
    document.getElementById("chart-next").disabled = total === 0 || chartViewMode === "live" || end >= total;
    document.getElementById("chart-live").classList.toggle("active", chartViewMode === "live");
}

function setChartsFromReadings(readings) {
    chartSeries = {
        labels: readings.map(getReadingLabel),
        temp: readings.map(r => getSeriesValue(r.temp)),
        hum: readings.map(r => getSeriesValue(r.hum)),
        ppm: readings.map(r => getSeriesValue(r.ppm)),
        co2: readings.map(r => getSeriesValue(r.co2_ppm)),
        dist: readings.map(r => getLevelPercent(r))
    };
    trimChartSeries();
    chartViewMode = "live";
    renderChartWindow();
}

function appendReadingToCharts(data) {
    chartSeries.labels.push(getReadingLabel(data));
    chartSeries.temp.push(getSeriesValue(data.temp));
    chartSeries.hum.push(getSeriesValue(data.hum));
    chartSeries.ppm.push(getSeriesValue(data.ppm));
    chartSeries.co2.push(getSeriesValue(data.co2_ppm));
    chartSeries.dist.push(getLevelPercent(data));
    trimChartSeries();
    renderChartWindow();
}

// Cargar umbrales desde localStorage al iniciar
function loadThresholds() {
    const savedGas = localStorage.getItem("th_gas_min") ?? localStorage.getItem("th_gas");
    const savedDist = localStorage.getItem("th_dist");
    const savedTempMax = localStorage.getItem("th_temp_max");
    
    if (savedGas) thresholds.gas = parseFloat(savedGas);
    if (savedDist) thresholds.dist = parseFloat(savedDist);
    if (savedTempMax) thresholds.tempMax = parseFloat(savedTempMax);

    // Actualizar inputs del DOM
    document.getElementById("threshold-gas").value = thresholds.gas;
    document.getElementById("threshold-dist").value = thresholds.dist;
    document.getElementById("threshold-temp-max").value = thresholds.tempMax;
}

function loadSavedConfig() {
    const savedWifiSsid = localStorage.getItem("wifi_ssid");
    const savedWifiPass = localStorage.getItem("wifi_pass");
    const savedServerUrl = localStorage.getItem("server_url");
    const savedIntervalMq = localStorage.getItem("interval_mq");
    const savedIntervalUs = localStorage.getItem("interval_us");
    const savedReactorEmpty = localStorage.getItem("reactor_empty_cm");
    const savedReactorFull = localStorage.getItem("reactor_full_cm");
    const savedDistanceOffset = localStorage.getItem("distance_offset_cm");
    const savedDistanceScale = localStorage.getItem("distance_scale");

    if (savedWifiSsid) document.getElementById("wifi-ssid-input").value = savedWifiSsid;
    if (savedWifiPass) document.getElementById("wifi-pass-input").value = savedWifiPass;
    if (savedServerUrl) {
        document.getElementById("server-url-input").value = savedServerUrl;
    } else {
        const defaultServerUrl = getDefaultServerUrl();
        if (defaultServerUrl) document.getElementById("server-url-input").value = defaultServerUrl;
    }
    if (savedIntervalMq) document.getElementById("interval-mq-input").value = savedIntervalMq;
    if (savedIntervalUs) document.getElementById("interval-us-input").value = savedIntervalUs;
    document.getElementById("interval-out-input").value = 1000;
    localStorage.setItem("interval_out", "1000");
    if (savedReactorEmpty) reactorCalibration.emptyCm = parseFloat(savedReactorEmpty);
    if (savedReactorFull) reactorCalibration.fullCm = parseFloat(savedReactorFull);
    if (savedDistanceOffset) distanceCalibration.offsetCm = parseFloat(savedDistanceOffset);
    if (savedDistanceScale) distanceCalibration.scale = parseFloat(savedDistanceScale);
    document.getElementById("reactor-empty-input").value = reactorCalibration.emptyCm;
    document.getElementById("reactor-full-input").value = reactorCalibration.fullCm;
    document.getElementById("distance-offset-input").value = distanceCalibration.offsetCm;
    document.getElementById("distance-scale-input").value = distanceCalibration.scale;
}

// Guardar umbrales
document.getElementById("btn-save-thresholds").addEventListener("click", () => {
    thresholds.gas = parseFloat(document.getElementById("threshold-gas").value);
    thresholds.dist = parseFloat(document.getElementById("threshold-dist").value);
    thresholds.tempMax = parseFloat(document.getElementById("threshold-temp-max").value);

    localStorage.setItem("th_gas_min", thresholds.gas);
    localStorage.setItem("th_dist", thresholds.dist);
    localStorage.setItem("th_temp_max", thresholds.tempMax);

    alert("Umbrales de alarma actualizados localmente.");
});

// Sintetizador de Sonido (Web Audio API)
function playAlarmSound(type) {
    if (isMuted) return;

    try {
        const AudioContextClass = window.AudioContext || window.webkitAudioContext;
        if (!AudioContextClass) return;

        const ctx = new AudioContextClass();
        const osc = ctx.createOscillator();
        const gain = ctx.createGain();
        osc.connect(gain);
        gain.connect(ctx.destination);

        const now = ctx.currentTime;

        if (type === 'gas') {
            // Tono alternante de sirena
            osc.type = 'sawtooth';
            osc.frequency.setValueAtTime(800, now);
            osc.frequency.linearRampToValueAtTime(400, now + 0.25);
            osc.frequency.linearRampToValueAtTime(800, now + 0.5);
            
            gain.gain.setValueAtTime(0.2, now);
            gain.gain.exponentialRampToValueAtTime(0.01, now + 0.5);
            
            osc.start(now);
            osc.stop(now + 0.5);
        } else if (type === 'obstacle') {
            // Pitido corto de proximidad
            osc.type = 'sine';
            osc.frequency.setValueAtTime(1200, now);
            
            gain.gain.setValueAtTime(0.15, now);
            gain.gain.exponentialRampToValueAtTime(0.01, now + 0.15);
            
            osc.start(now);
            osc.stop(now + 0.15);
        } else if (type === 'temp') {
            // Pitido doble
            osc.type = 'sine';
            osc.frequency.setValueAtTime(600, now);
            
            gain.gain.setValueAtTime(0.15, now);
            gain.gain.exponentialRampToValueAtTime(0.01, now + 0.3);
            
            osc.start(now);
            osc.stop(now + 0.35);
        }
    } catch (e) {
        console.error("Audio Context error: ", e);
    }
}

// Control de Silencio
const muteBtn = document.getElementById("mute-alert-btn");
muteBtn.addEventListener("click", () => {
    isMuted = !isMuted;
    if (isMuted) {
        muteBtn.innerHTML = '<i class="fa-solid fa-volume-xmark"></i> Desactivado';
        muteBtn.style.background = "rgba(255, 255, 255, 0.05)";
    } else {
        muteBtn.innerHTML = '<i class="fa-solid fa-volume-high"></i> Silenciar';
        muteBtn.style.background = "rgba(255, 255, 255, 0.2)";
    }
});

// Inicializacion de Graficos con Chart.js
function initCharts() {
    const commonGridColor = 'rgba(255, 255, 255, 0.05)';
    const commonTextColor = '#8a99ad';

    // Ocultar puntos globalmente: solo visibles al hacer hover
    Chart.defaults.datasets.line.pointRadius = 0;
    Chart.defaults.datasets.line.pointHoverRadius = 5;
    
    // 1. Grafico Temperatura + Humedad
    const ctxTempHum = document.getElementById('tempHumChart').getContext('2d');
    charts.tempHum = new Chart(ctxTempHum, {
        type: 'line',
        data: {
            labels: [],
            datasets: [
                {
                    label: 'Temperatura (C)',
                    yAxisID: 'yTemp',
                    data: [],
                    borderColor: '#ff073a',
                    backgroundColor: 'rgba(255, 7, 58, 0.05)',
                    borderWidth: 2,
                    tension: 0.3,
                    fill: false,
                    pointRadius: 0,
                    pointHoverRadius: 5,
                    pointHoverBackgroundColor: '#ff073a',
                    pointHoverBorderColor: '#fff'
                },
                {
                    label: 'Humedad (%)',
                    yAxisID: 'yHum',
                    data: [],
                    borderColor: '#00f0ff',
                    backgroundColor: 'rgba(0, 240, 255, 0.05)',
                    borderWidth: 2,
                    tension: 0.3,
                    fill: false,
                    pointRadius: 0,
                    pointHoverRadius: 5,
                    pointHoverBackgroundColor: '#00f0ff',
                    pointHoverBorderColor: '#fff'
                }
            ]
        },
        options: {
            responsive: true,
            maintainAspectRatio: false,
            animation: { duration: CHART_ANIMATION_MS, easing: "linear" },
            scales: {
                x: {
                    grid: { color: commonGridColor },
                    ticks: { color: commonTextColor }
                },
                yTemp: {
                    type: 'linear',
                    position: 'left',
                    grid: { color: commonGridColor },
                    ticks: { color: '#ff073a' },
                    title: { display: true, text: 'Temperatura (C)', color: '#ff073a' },
                    min: 0,
                    max: 60
                },
                yHum: {
                    type: 'linear',
                    position: 'right',
                    grid: { drawOnChartArea: false },
                    ticks: { color: '#00f0ff' },
                    title: { display: true, text: 'Humedad (%)', color: '#00f0ff' },
                    min: 0,
                    max: 100
                }
            },
            plugins: {
                legend: { labels: { color: '#e2e8f0' } }
            }
        }
    });

    // 2. Grafico Methane CH4 PPM
    const ctxGas = document.getElementById('gasChart').getContext('2d');
    charts.gas = new Chart(ctxGas, {
        type: 'line',
        data: {
            labels: [],
            datasets: [{
                label: 'Metano (CH4 PPM)',
                data: [],
                borderColor: '#ff9f1c',
                backgroundColor: 'rgba(255, 159, 28, 0.15)',
                borderWidth: 2,
                tension: 0.3,
                fill: true,
                pointRadius: 0,
                pointHoverRadius: 5,
                pointHoverBackgroundColor: '#ff9f1c',
                pointHoverBorderColor: '#fff'
            }]
        },
        options: {
            responsive: true,
            maintainAspectRatio: false,
            animation: { duration: CHART_ANIMATION_MS, easing: "linear" },
            scales: {
                x: {
                    grid: { color: commonGridColor },
                    ticks: { color: commonTextColor }
                },
                y: {
                    grid: { color: commonGridColor },
                    ticks: { color: commonTextColor },
                    title: { display: true, text: 'CH4 PPM', color: '#ff9f1c' },
                    min: 0
                }
            },
            plugins: {
                legend: { labels: { color: '#e2e8f0' } }
            }
        }
    });

    // 3. Grafico CO2
    const ctxCo2 = document.getElementById('co2Chart').getContext('2d');
    charts.co2 = new Chart(ctxCo2, {
        type: 'line',
        data: {
            labels: [],
            datasets: [{
                label: 'Dioxido de carbono (CO2 PPM)',
                data: [],
                borderColor: '#39ff14',
                backgroundColor: 'rgba(57, 255, 20, 0.12)',
                borderWidth: 2,
                tension: 0.3,
                fill: true,
                pointRadius: 0,
                pointHoverRadius: 5,
                pointHoverBackgroundColor: '#39ff14',
                pointHoverBorderColor: '#fff'
            }]
        },
        options: {
            responsive: true,
            maintainAspectRatio: false,
            animation: { duration: CHART_ANIMATION_MS, easing: "linear" },
            scales: {
                x: {
                    grid: { color: commonGridColor },
                    ticks: { color: commonTextColor }
                },
                y: {
                    grid: { color: commonGridColor },
                    ticks: { color: commonTextColor },
                    title: { display: true, text: 'CO2 PPM', color: '#39ff14' },
                    min: 0
                }
            },
            plugins: {
                legend: { labels: { color: '#e2e8f0' } }
            }
        }
    });

    // 4. Grafico Nivel reactor
    const ctxDist = document.getElementById('distChart').getContext('2d');
    charts.dist = new Chart(ctxDist, {
        type: 'line',
        data: {
            labels: [],
            datasets: [{
                label: 'Nivel reactor (%)',
                data: [],
                borderColor: '#00f0ff',
                backgroundColor: 'rgba(0, 240, 255, 0.05)',
                borderWidth: 2,
                tension: 0.3,
                fill: false,
                pointRadius: 0,
                pointHoverRadius: 5,
                pointHoverBackgroundColor: '#00f0ff',
                pointHoverBorderColor: '#fff'
            }]
        },
        options: {
            responsive: true,
            maintainAspectRatio: false,
            animation: { duration: CHART_ANIMATION_MS, easing: "linear" },
            scales: {
                x: {
                    grid: { color: commonGridColor },
                    ticks: { color: commonTextColor }
                },
                y: {
                    grid: { color: commonGridColor },
                    ticks: { color: commonTextColor },
                    title: { display: true, text: 'Nivel (%)', color: '#00f0ff' },
                    min: 0,
                    max: 100
                }
            },
            plugins: {
                legend: { labels: { color: '#e2e8f0' } }
            }
        }
    });
}

// Cargar datos historicos desde el API
async function loadHistoryData(hours) {
    try {
        const response = await fetch(`/api/history?hours=${hours}`);
        const data = await response.json();
        historicalData = data.history;
        
        // Poblar tablas
        updateHistoryTable(historicalData);

        // Poblar graficos y dejar la vista siguiendo la medicion mas reciente.
        setChartsFromReadings(historicalData);

        if (historicalData.length > 0 && (!socket || socket.readyState !== WebSocket.OPEN)) {
            const latestHistoryReading = { ...historicalData[historicalData.length - 1], source: historicalData[historicalData.length - 1].wifi ? "WiFi" : "Serial" };
            lastReadingKey = getReadingKey(latestHistoryReading);
            processRealtimeReading(latestHistoryReading, false);
        }
        
    } catch (error) {
        console.error("Error cargando historial: ", error);
    }
}

async function pollLatestReading() {
    try {
        const response = await fetch("/api/latest");
        if (!response.ok) return;
        const data = await response.json();
        if (!socket || socket.readyState !== WebSocket.OPEN) {
            document.getElementById("conn-status").className = "value status-connected";
            document.getElementById("conn-status").innerHTML = '<i class="fa-solid fa-circle-dot"></i> CONECTADO REST';
        }

        const readingKey = getReadingKey(data);
        const isNewReading = readingKey && readingKey !== lastReadingKey;
        processRealtimeReading(data, isNewReading);
        if (isNewReading) {
            lastReadingKey = readingKey;
        }
    } catch (error) {
        console.error("Error consultando ultima lectura: ", error);
    }
}

function startPollingFallback() {
    if (pollingInterval) return;
    pollLatestReading();
    pollingInterval = setInterval(pollLatestReading, 1000);
}

function stopPollingFallback() {
    if (!pollingInterval) return;
    clearInterval(pollingInterval);
    pollingInterval = null;
}

// Actualizar la tabla del historico
function updateHistoryTable(data) {
    const tableBody = document.getElementById("history-table-body");
    tableBody.innerHTML = "";

    if (data.length === 0) {
        tableBody.innerHTML = '<tr><td colspan="9" class="no-data">Sin lecturas registradas.</td></tr>';
        return;
    }

    // Mostrar los ultimos 15 registros en la tabla
    const subset = data.slice().reverse().slice(0, 15);
    subset.forEach(row => {
        const localTime = parseTimestamp(row.timestamp).toLocaleString();
        tableBody.innerHTML += `
            <tr>
                <td>${localTime}</td>
                <td>${formatNumber(row.temp, 1)}</td>
                <td>${formatNumber(row.hum, 1)}</td>
                <td>${formatNumber(row.ppm, 0)}</td>
                <td>${formatNumber(row.co2_ppm, 0)}</td>
                <td>${formatNumber(row.dist, 1)}</td>
                <td>${formatNumber(row.mq_r0, 0)}</td>
                <td>${row.uptime ?? "--"}</td>
                <td><span class="badge ${row.wifi ? 'badge-safe' : 'badge-warning'}">${row.wifi ? 'WiFi' : 'Serial'}</span></td>
            </tr>
        `;
    });
}

function appendHistoryRow(data) {
    const normalized = {
        ...data,
        timestamp: data.timestamp || Date.now() / 1000,
        wifi: data.source === "WiFi" || data.wifi === true
    };

    historicalData.push(normalized);
    if (historicalData.length > MAX_CHART_POINTS) {
        historicalData.shift();
    }
    updateHistoryTable(historicalData);
}

// Convertir Uptime en formato legible
function formatUptime(seconds) {
    const h = Math.floor(seconds / 3600);
    const m = Math.floor((seconds % 3600) / 60);
    const s = seconds % 60;
    return `${h.toString().padStart(2, '0')}:${m.toString().padStart(2, '0')}:${s.toString().padStart(2, '0')}`;
}

// Clasifica la calidad de senal WiFi por RSSI y pinta las barras del header
function updateWifiSignalBars(rssi) {
    const container = document.getElementById("wifi-signal-bars");
    if (!container) return;
    const bars = container.querySelectorAll(".bar");
    const value = Number(rssi);

    let activeBars = 0;
    let quality = "sin-senal";
    if (Number.isFinite(value) && value !== 0) {
        if (value >= -60) { activeBars = 4; quality = "excelente"; }
        else if (value >= -70) { activeBars = 3; quality = "buena"; }
        else if (value >= -80) { activeBars = 2; quality = "debil"; }
        else { activeBars = 1; quality = "muy-debil"; }
    }

    bars.forEach((bar, idx) => {
        bar.classList.toggle("bar-active", idx < activeBars);
    });
    container.dataset.quality = quality;
    container.title = `Calidad de senal WiFi: ${quality.replace("-", " ")}`;
}

// Actualiza la tarjeta de estado de conexion en el panel RED & SERVIDOR
function updateWifiStatusCard(data) {
    const badge = document.getElementById("wifi-status-badge");
    const ssidEl = document.getElementById("wifi-status-ssid");
    const ipEl = document.getElementById("wifi-status-ip");
    const uptimeEl = document.getElementById("wifi-status-uptime");
    const endpointEl = document.getElementById("wifi-status-endpoint");
    if (!badge) return;

    const isWifiConnected = data.source === "WiFi" && data.wifi;
    const isConfigured = data.wifi_configured !== false;
    const attempts = Number(data.wifi_reconnect_attempts) || 0;

    if (isWifiConnected) {
        badge.className = "wifi-status-badge state-connected";
        badge.innerHTML = '<i class="fa-solid fa-circle-check"></i> Conectado';
    } else if (!isConfigured) {
        badge.className = "wifi-status-badge state-unconfigured";
        badge.innerHTML = '<i class="fa-solid fa-circle-question"></i> Sin configurar (usando Serial)';
    } else if (attempts > 0) {
        badge.className = "wifi-status-badge state-reconnecting";
        badge.innerHTML = `<i class="fa-solid fa-rotate"></i> Reintentando (intento ${attempts})`;
    } else {
        badge.className = "wifi-status-badge state-disconnected";
        badge.innerHTML = '<i class="fa-solid fa-circle-xmark"></i> Desconectado';
    }

    if (ssidEl) ssidEl.innerText = document.getElementById("wifi-ssid-input").value || "--";
    if (ipEl) ipEl.innerText = data.ip || (isWifiConnected ? "Conectado sin IP reportada" : "--");
    if (uptimeEl) {
        uptimeEl.innerText = isWifiConnected ? formatUptime(Number(data.wifi_connected_since_s) || 0) : "--";
    }
    if (endpointEl) endpointEl.innerText = data.server_url || "--";
}

// Procesamiento en tiempo real de los datos
function processRealtimeReading(data, appendRealtimePoint = true) {
    if (Number.isFinite(Number(data.reactor_empty_cm)) && Number.isFinite(Number(data.reactor_full_cm))) {
        reactorCalibration.emptyCm = Number(data.reactor_empty_cm);
        reactorCalibration.fullCm = Number(data.reactor_full_cm);
        document.getElementById("reactor-empty-input").value = reactorCalibration.emptyCm;
        document.getElementById("reactor-full-input").value = reactorCalibration.fullCm;
    }

    if (Number.isFinite(Number(data.us_offset_cm))) {
        distanceCalibration.offsetCm = Number(data.us_offset_cm);
        document.getElementById("distance-offset-input").value = distanceCalibration.offsetCm;
    }

    if (Number.isFinite(Number(data.us_scale))) {
        distanceCalibration.scale = Number(data.us_scale);
        document.getElementById("distance-scale-input").value = distanceCalibration.scale;
    }

    if (data.mq4_curve_a !== undefined) {
        const isCustom = data.mq4_curve_custom === true;
        const pts = Number.isFinite(Number(data.mq4_curve_points)) ? Number(data.mq4_curve_points) : 0;
        const statusText = isCustom
            ? `Personalizada (A=${formatNumber(data.mq4_curve_a, 2)}, B=${formatNumber(data.mq4_curve_b, 3)})`
            : `Datasheet por defecto (A=${formatNumber(data.mq4_curve_a, 2)}, B=${formatNumber(data.mq4_curve_b, 3)})`;
        document.getElementById("mq4-curve-status").value =
            pts > 0 ? `${statusText} - ${pts} punto(s) sin ajustar` : statusText;
    }

    if (Number.isFinite(Number(data.dht_temp_offset))) {
        document.getElementById("dht-temp-offset-input").value = Number(data.dht_temp_offset);
    }
    if (Number.isFinite(Number(data.dht_hum_offset))) {
        document.getElementById("dht-hum-offset-input").value = Number(data.dht_hum_offset);
    }

    // Actualizar Textos
    document.getElementById("temp-val").innerText = formatNumber(data.temp, 1, "--.-");
    document.getElementById("hum-val").innerText = formatNumber(data.hum, 1, "--.-");
    document.getElementById("gas-val").innerText = formatNumber(data.ppm, 0, "----");
    document.getElementById("co2-val").innerText = formatNumber(data.co2_ppm, 0, "----");
    const co2State = document.getElementById("co2-state");
    if (co2State) {
        const co2Valid = data.co2_valid === true ||
            (data.co2_valid === undefined && getSeriesValue(data.co2_ppm) !== null);
        if (co2Valid) {
            co2State.innerText = Number(data.co2_ppm) >= 5000
                ? "Lectura válida; posible saturación"
                : "Lectura válida";
        } else if (Number(data.co2_error_count) > 0) {
            co2State.innerText = `Sin respuesta UART (${data.co2_error_count} fallos)`;
        } else if (data.co2_ready === false) {
            co2State.innerText = "Precalentando sensor";
        } else {
            co2State.innerText = "Sin lectura válida";
        }
    }
    document.getElementById("dist-val").innerText = formatNumber(data.dist, 1, "--.-");
    setChartLiveValues(data);
    
    document.getElementById("data-source").innerText = data.source || "SERIAL";
    document.getElementById("uptime-val").innerText = formatUptime(data.uptime || 0);

    // Wifi Info
    if (data.source === "WiFi") {
        document.getElementById("wifi-rssi").innerText = `${data.rssi || 0} dBm`;
        document.querySelectorAll(".wifi-only").forEach(el => el.style.display = "block");
    } else {
        document.querySelectorAll(".wifi-only").forEach(el => el.style.display = "none");
    }
    updateWifiSignalBars(data.rssi);
    updateWifiStatusCard(data);

    if (data.server_url) {
        localStorage.setItem("last_known_server_url", data.server_url);
        checkServerUrlMismatch(data.server_url);
    }

    // 1. Logica de nivel del reactor: distancia grande = vacio, distancia pequena = lleno.
    const fillPct = getLevelPercent(data);
    const tankFillBar = document.getElementById("tank-fill-bar");
    const tankPercentage = document.getElementById("tank-percentage");
    
    tankFillBar.style.height = `${fillPct}%`;
    tankPercentage.innerText = `${fillPct.toFixed(0)}%`;

    // 2. Logica del Semaforo de Alerta MQ-4
    const gasBadge = document.getElementById("gas-badge");
    const cardGas = document.getElementById("card-gas");
    const ppm = Number(data.ppm);
    const mqCalibrated = data.mq_calibrated !== false;
    const mqValid = data.mq_valid !== false && Number.isFinite(ppm);
    
    cardGas.className = "metric-card card-gas"; // Reset
    gasBadge.className = "card-status-badge";  // Reset
    
    if (!mqCalibrated) {
        gasBadge.innerText = "SIN CALIBRAR";
        gasBadge.classList.add("badge-warning");
        cardGas.classList.add("card-alert-gas");
    } else if (data.mq_valid === false || !mqValid) {
        gasBadge.innerText = "SIN LECTURA";
        gasBadge.classList.add("badge-warning");
        cardGas.classList.add("card-alert-gas");
    } else if (data.mq_ready === false) {
        gasBadge.innerText = "CALENTANDO";
        gasBadge.classList.add("badge-warning");
        cardGas.classList.add("card-alert-gas");
    } else if (ppm < thresholds.gas) {
        gasBadge.innerText = "SIN GAS";
        gasBadge.classList.add("badge-danger");
        cardGas.classList.add("card-critical-gas");
    } else {
        gasBadge.innerText = "PRODUCCION";
        gasBadge.classList.add("badge-safe");
    }

    // 3. Evaluar e inducir alarmas globales
    checkAlarmsAndAlerts(data);

    // 4. Agregar a los Graficos Dinamicamente (solo si llega una lectura nueva)
    if (!appendRealtimePoint) return;
    if (!data.timestamp && data.source === "Ninguno") return;

    appendHistoryRow(data);
    appendReadingToCharts(data);
}

// Evaluacion de Alarmas
let activeAlarms = {
    gas: false,
    obstacle: false,
    temp: false
};

function checkAlarmsAndAlerts(data) {
    const banner = document.getElementById("global-alert-banner");
    const bannerText = document.getElementById("alert-banner-text");
    const cardDist = document.getElementById("card-dist");
    const cardTemp = document.getElementById("card-temp");

    // Evaluaciones
    const levelPct = getLevelPercent(data);
    const mqReadingValid = data.mq_calibrated !== false && data.mq_valid !== false &&
        getSeriesValue(data.ppm) !== null;
    activeAlarms.gas = data.mq_ready !== false && mqReadingValid &&
        Number(data.ppm) < thresholds.gas;
    activeAlarms.obstacle = (levelPct >= thresholds.dist);
    activeAlarms.temp = (data.temp !== undefined && data.temp >= thresholds.tempMax);

    // Reset Clases
    cardDist.classList.remove("card-alert-dist");
    cardTemp.classList.remove("card-critical-gas");

    let activeAlarmCount = 0;
    let alertMsg = [];

    if (activeAlarms.gas) {
        activeAlarmCount++;
        alertMsg.push("SIN PRODUCCION DE GAS");
    }
    if (activeAlarms.obstacle) {
        activeAlarmCount++;
        alertMsg.push("REACTOR CERCA DE NIVEL MAXIMO");
        cardDist.classList.add("card-alert-dist");
        document.getElementById("tank-fill-bar").classList.add("critical");
    } else {
        document.getElementById("tank-fill-bar").classList.remove("critical");
    }
    if (activeAlarms.temp) {
        activeAlarmCount++;
        alertMsg.push("SOBRECALENTAMIENTO TEMPERATURA");
        cardTemp.classList.add("card-critical-gas");
    }

    // Gestionar el Banner
    if (activeAlarmCount > 0) {
        banner.classList.remove("hidden");
        bannerText.innerText = `ALERTA: ${alertMsg.join(" | ")}`;
        
        // Habilitar pitido periodico si no se ha silenciado
        if (!audioInterval) {
            triggerAudioAlarms();
            audioInterval = setInterval(triggerAudioAlarms, 2500);
        }
    } else {
        banner.classList.add("hidden");
        if (audioInterval) {
            clearInterval(audioInterval);
            audioInterval = null;
        }
    }
}

function triggerAudioAlarms() {
    if (activeAlarms.gas) {
        playAlarmSound('gas');
    } else if (activeAlarms.obstacle) {
        playAlarmSound('obstacle');
    } else if (activeAlarms.temp) {
        playAlarmSound('temp');
    }
}

// Conexion por WebSocket al Servidor FastAPI
function connectWebSocket() {
    const wsScheme = window.location.protocol === "https:" ? "wss" : "ws";
    const wsUrl = `${wsScheme}://${window.location.host}/ws/client`;

    console.log(`Conectando WebSocket a: ${wsUrl}`);
    socket = new WebSocket(wsUrl);

    socket.onopen = () => {
        console.log("WebSocket Conectado!");
        socketRetryDelay = 1000;
        stopPollingFallback();
        document.getElementById("conn-status").className = "value status-connected";
        document.getElementById("conn-status").innerHTML = '<i class="fa-solid fa-circle-dot"></i> CONECTADO';
    };

    socket.onmessage = (event) => {
        try {
            const data = JSON.parse(event.data);
            if (!data || typeof data !== "object" || Array.isArray(data)) return;
            const readingKey = getReadingKey(data);
            const isNewReading = readingKey && readingKey !== lastReadingKey;
            processRealtimeReading(data, isNewReading);
            if (isNewReading) {
                lastReadingKey = readingKey;
            }
        } catch (error) {
            console.warn("Mensaje WebSocket invalido:", error);
        }
    };

    socket.onclose = () => {
        console.log("WebSocket Desconectado. Reintentando en 3s...");
        document.getElementById("conn-status").className = "value status-disconnected";
        document.getElementById("conn-status").innerHTML = '<i class="fa-solid fa-circle-dot"></i> DESCONECTADO';
        startPollingFallback();
        const retryDelay = socketRetryDelay;
        socketRetryDelay = Math.min(socketRetryDelay * 2, 30000);
        setTimeout(connectWebSocket, retryDelay);
    };

    socket.onerror = (err) => {
        console.error("WebSocket Error: ", err);
        startPollingFallback();
        socket.close();
    };
}

// Envio de Comandos al API REST
async function sendCommand(commandData) {
    try {
        const response = await fetch("/api/command", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify(commandData)
        });
        const res = await response.json();
        console.log("Comando enviado: ", res);
        return res;
    } catch (e) {
        console.error("Error enviando comando: ", e);
    }
}

// Eventos de los Botones de Control
document.getElementById("btn-calibrate").addEventListener("click", () => {
    if (confirm("Confirmar calibracion de MQ-4? Asegurate de que el sensor este en AIRE LIMPIO. Este proceso dura 5 segundos.")) {
        sendCommand({ cmd: "calibrate" });
    }
});

document.getElementById("btn-reboot").addEventListener("click", () => {
    if (confirm("Reiniciar el dispositivo ESP32?")) {
        sendCommand({ cmd: "reboot" });
    }
});

// Control manual de LED
document.getElementById("led-toggle").addEventListener("change", (e) => {
    const state = e.target.checked ? 1 : 0;
    sendCommand({ cmd: "led", state: state });
});

// Mostrar/ocultar contrasena WiFi
document.getElementById("wifi-pass-toggle").addEventListener("click", () => {
    const input = document.getElementById("wifi-pass-input");
    const icon = document.querySelector("#wifi-pass-toggle i");
    const isHidden = input.type === "password";
    input.type = isHidden ? "text" : "password";
    icon.className = isHidden ? "fa-solid fa-eye-slash" : "fa-solid fa-eye";
});

// Colapsar/expandir guia rapida (recuerda la preferencia entre sesiones)
document.getElementById("quick-guide-toggle").addEventListener("click", () => {
    const body = document.getElementById("quick-guide-body");
    const arrow = document.getElementById("quick-guide-arrow");
    const collapsed = body.classList.toggle("collapsed");
    arrow.classList.toggle("collapsed", collapsed);
    localStorage.setItem("quick_guide_collapsed", collapsed ? "1" : "0");
});
if (localStorage.getItem("quick_guide_collapsed") === "1") {
    document.getElementById("quick-guide-body").classList.add("collapsed");
    document.getElementById("quick-guide-arrow").classList.add("collapsed");
}

// Guardar Config WiFi
document.getElementById("btn-save-wifi").addEventListener("click", async () => {
    const ssid = document.getElementById("wifi-ssid-input").value.trim();
    const pass = document.getElementById("wifi-pass-input").value.trim();
    if (!ssid) {
        alert("Introduce un SSID valido");
        return;
    }
    localStorage.setItem("wifi_ssid", ssid);
    localStorage.setItem("wifi_pass", pass);
    const result = await sendCommand({ cmd: "set_wifi", ssid: ssid, pass: pass });
    alert(buildCommandDeliveryMessage(result));
});

// Guardar Endpoint de Servidor
document.getElementById("btn-save-server").addEventListener("click", async () => {
    const url = document.getElementById("server-url-input").value.trim();
    if (!url) {
        alert("Introduce una URL valida");
        return;
    }
    localStorage.setItem("server_url", url);
    const result = await sendCommand({ cmd: "set_server", url: url });
    alert(buildCommandDeliveryMessage(result));
});

// Guardar intervalos de muestreo
document.getElementById("btn-save-intervals").addEventListener("click", () => {
    const mqInterval = parseInt(document.getElementById("interval-mq-input").value, 10);
    const usInterval = parseInt(document.getElementById("interval-us-input").value, 10);
    const outInterval = 1000;

    if (Number.isNaN(mqInterval) || Number.isNaN(usInterval) || Number.isNaN(outInterval)) {
        alert("Introduce valores numericos validos para los intervalos.");
        return;
    }

    localStorage.setItem("interval_mq", mqInterval);
    localStorage.setItem("interval_us", usInterval);
    localStorage.setItem("interval_out", outInterval);

    sendCommand({ cmd: "set_intervals", mq: mqInterval, us: usInterval, out: outInterval });
    alert("Intervalos de muestreo enviados al ESP32.");
});

document.getElementById("btn-save-reactor-levels").addEventListener("click", () => {
    const emptyCm = parseFloat(document.getElementById("reactor-empty-input").value);
    const fullCm = parseFloat(document.getElementById("reactor-full-input").value);

    if (!Number.isFinite(emptyCm) || !Number.isFinite(fullCm) || emptyCm <= fullCm) {
        alert("La distancia de reactor vacio debe ser mayor que la distancia de reactor lleno.");
        return;
    }

    reactorCalibration.emptyCm = emptyCm;
    reactorCalibration.fullCm = fullCm;
    localStorage.setItem("reactor_empty_cm", emptyCm);
    localStorage.setItem("reactor_full_cm", fullCm);

    sendCommand({ cmd: "set_reactor_levels", empty: Math.round(emptyCm), full: Math.round(fullCm) });
    setChartsFromReadings(historicalData);
    alert("Calibracion del reactor enviada al ESP32.");
});

document.getElementById("btn-save-distance-calibration").addEventListener("click", async () => {
    const offsetCm = parseFloat(document.getElementById("distance-offset-input").value);
    const scale = parseFloat(document.getElementById("distance-scale-input").value);

    if (!Number.isFinite(offsetCm) || !Number.isFinite(scale) || offsetCm < -100 || offsetCm > 100 || scale < 0.5 || scale > 1.5) {
        alert("Usa offset entre -100 y 100 cm, y escala entre 0.5 y 1.5.");
        return;
    }

    distanceCalibration.offsetCm = offsetCm;
    distanceCalibration.scale = scale;
    localStorage.setItem("distance_offset_cm", offsetCm);
    localStorage.setItem("distance_scale", scale);

    const result = await sendCommand({ cmd: "set_distance_calibration", offset: offsetCm, scale: scale });
    alert(buildCommandDeliveryMessage(result));
});

// Calibracion multipunto de la curva MQ-4
document.getElementById("btn-add-cal-point").addEventListener("click", async () => {
    const ppm = parseFloat(document.getElementById("cal-point-ppm-input").value);
    if (!Number.isFinite(ppm) || ppm <= 0) {
        alert("Introduce el PPM de referencia al que esta expuesto el sensor ahora mismo.");
        return;
    }
    if (!confirm(`Confirmar captura de punto a ${ppm} PPM? El sensor debe estar expuesto a esa concentracion de forma estable durante los proximos ~10 segundos.`)) {
        return;
    }
    const result = await sendCommand({ cmd: "add_calibration_point", ppm: ppm });
    alert(buildCommandDeliveryMessage(result) || "Punto en captura, revisa el log serial/consola para el resultado.");
});

document.getElementById("btn-fit-curve").addEventListener("click", async () => {
    if (!confirm("Ajustar la curva A/B con los puntos capturados? Necesitas al menos 2 puntos con concentraciones distintas.")) {
        return;
    }
    const result = await sendCommand({ cmd: "fit_calibration_curve" });
    alert(buildCommandDeliveryMessage(result) || "Ajuste solicitado, revisa el evento mq4_curve_fit para ver R2.");
});

document.getElementById("btn-clear-cal-points").addEventListener("click", () => {
    sendCommand({ cmd: "reset_calibration_points" });
});

document.getElementById("btn-reset-curve").addEventListener("click", () => {
    if (confirm("Restaurar la curva generica del datasheet (A=1012.7, B=-2.786)? Se perdera el ajuste personalizado.")) {
        sendCommand({ cmd: "reset_curve" });
    }
});

// Guardar offset DHT11
document.getElementById("btn-save-dht-offset").addEventListener("click", async () => {
    const tempOffset = parseFloat(document.getElementById("dht-temp-offset-input").value);
    const humOffset = parseFloat(document.getElementById("dht-hum-offset-input").value);

    if (!Number.isFinite(tempOffset) || !Number.isFinite(humOffset) || tempOffset < -15 || tempOffset > 15 || humOffset < -30 || humOffset > 30) {
        alert("Usa un offset de temperatura entre -15 y 15 C, y de humedad entre -30 y 30 %.");
        return;
    }

    const result = await sendCommand({ cmd: "set_dht_offset", temp: tempOffset, hum: humOffset });
    alert(buildCommandDeliveryMessage(result));
});

// Selector de Rango de Graficos
document.querySelectorAll(".time-btn").forEach(btn => {
    btn.addEventListener("click", (e) => {
        document.querySelectorAll(".time-btn").forEach(b => b.classList.remove("active"));
        e.target.classList.add("active");
        
        currentHoursFilter = parseInt(e.target.dataset.hours);
        loadHistoryData(currentHoursFilter);
    });
});

document.getElementById("chart-prev").addEventListener("click", () => {
    chartViewMode = "manual";
    chartWindowStart = Math.max(0, chartWindowStart - CHART_WINDOW_SIZE);
    renderChartWindow();
});

document.getElementById("chart-next").addEventListener("click", () => {
    chartViewMode = "manual";
    chartWindowStart += CHART_WINDOW_SIZE;
    const maxStart = Math.max(0, chartSeries.labels.length - Math.min(CHART_WINDOW_SIZE, chartSeries.labels.length));
    if (chartWindowStart >= maxStart) {
        chartViewMode = "live";
    }
    renderChartWindow();
});

document.getElementById("chart-live").addEventListener("click", () => {
    chartViewMode = "live";
    renderChartWindow();
});

// Exportacion CSV
document.getElementById("btn-export-csv").addEventListener("click", () => {
    window.location.href = `/api/export?hours=${currentHoursFilter}`;
});

// Deteccion automatica de la IP del servidor (misma IP con la que el
// navegador llego a este dashboard, ya que sirve la pagina y recibe las
// lecturas por el mismo puerto)
function initAutoIpDetection() {
    const textEl = document.getElementById("auto-ip-text");
    const btnEl = document.getElementById("btn-use-detected-ip");
    if (!textEl || !btnEl) return;

    const detected = getDefaultServerUrl();
    if (!detected) {
        textEl.innerText = "Estas entrando por \"localhost\": abri el dashboard con la IP de red de esta PC (ej. http://192.168.x.x:8000) para poder autodetectarla.";
        btnEl.disabled = true;
        btnEl.style.opacity = "0.5";
        return;
    }

    textEl.innerText = `IP detectada de este navegador: ${detected}`;
    btnEl.disabled = false;
    btnEl.style.opacity = "1";
}

document.getElementById("btn-use-detected-ip").addEventListener("click", async () => {
    const detected = getDefaultServerUrl();
    if (!detected) return;

    document.getElementById("server-url-input").value = detected;
    localStorage.setItem("server_url", detected);
    const result = await sendCommand({ cmd: "set_server", url: detected });
    document.getElementById("ip-mismatch-warning").classList.add("hidden");
    alert(buildCommandDeliveryMessage(result));
});

// Compara lo que el ESP32 reporta como endpoint activo contra la IP
// detectada del navegador, y avisa si quedaron desincronizados (tipico
// despues de cambiar de red WiFi).
function checkServerUrlMismatch(reportedUrl) {
    const warningEl = document.getElementById("ip-mismatch-warning");
    if (!warningEl) return;

    const detected = getDefaultServerUrl();
    if (!detected || !reportedUrl) {
        warningEl.classList.add("hidden");
        return;
    }

    if (reportedUrl.trim() !== detected.trim()) {
        warningEl.classList.remove("hidden");
    } else {
        warningEl.classList.add("hidden");
    }
}

// Inicializacion General
window.addEventListener("DOMContentLoaded", () => {
    loadThresholds();    loadSavedConfig();    initCharts();
    initAutoIpDetection();
    loadHistoryData(currentHoursFilter);
    startPollingFallback();
    connectWebSocket();
});
