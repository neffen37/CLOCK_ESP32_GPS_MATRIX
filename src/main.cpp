#include <Arduino.h>
#include <EEPROM.h>
#include <HardwareSerial.h>
#include <SPI.h>
#include <Ethernet.h>
#include <EthernetUdp.h>
#include <Update.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <ESP32-HUB75-VirtualMatrixPanel_T.hpp>

// ── GPS (RX + PPS seulement) ────────────────────────────────────────────────
#define GPS_RX_PIN  17
#define PPS_PIN     21

// ── W5500 ───────────────────────────────────────────────────────────────────
#define W5500_SCK   13
#define W5500_MISO  12
#define W5500_MOSI  11
#define W5500_CS    14
#define W5500_RST    9
#define W5500_INT   10

// ── HUB75 – 3 panneaux P2.5 128×64 px, disposition 3×1 = 384×64 px ─────────
#define PANEL_W      128
#define PANEL_H       64
#define PANEL_CHAIN    3
#define PANELS_WIDE    3
#define PANELS_TALL    1
#define MATRIX_W      (PANEL_W * PANELS_WIDE)   // 384
#define MATRIX_H      (PANEL_H * PANELS_TALL)   //  64

#define HUB_R1   1
#define HUB_G1   2
#define HUB_B1   3
#define HUB_R2  15
#define HUB_G2  16
#define HUB_B2  33
#define HUB_A   34
#define HUB_B   35
#define HUB_C   36
#define HUB_D   37
#define HUB_E   38   // requis pour panneaux 64 lignes
#define HUB_CLK 39
#define HUB_LAT 40
#define HUB_OE  41

// ── OTA sécurité ─────────────────────────────────────────────────────────────
// Valeur = base64("utilisateur:motdepasse")
// Pour changer : echo -n "monuser:monpass" | base64
// Exemple      : echo -n "admin:1234" | base64  →  YWRtaW46MTIzNA==
#define OTA_AUTH_B64 "YWRtaW46YWRtaW4="

bool check_auth(const String& req) {
  return req.indexOf("Authorization: Basic " OTA_AUTH_B64) >= 0;
}

void send_auth_required(EthernetClient& client) {
  client.println("HTTP/1.1 401 Unauthorized");
  client.println("WWW-Authenticate: Basic realm=\"OTA Update\"");
  client.println("Content-Type: text/plain");
  client.println("Connection: close");
  client.println();
  client.println("Acces refuse");
}

// ── Warmup ───────────────────────────────────────────────────────────────────
#define WARMUP_SECONDS 10  // temps pour 5 min 300 segonde
bool     warmup_done  = false;
uint32_t warmup_start = 0;

// ── Météo ────────────────────────────────────────────────────────────────────
float    weather_temp        = -999.0f;
uint32_t last_weather_fetch  = 0;

// ── Fuseau horaire ───────────────────────────────────────────────────────────
int UTC_OFFSET_H = 1;
#define UTC_OFFSET_M 0

// ── Luminosité matrice (par zone) ────────────────────────────────────────────
uint8_t bright_center = 200;
uint8_t bright_sides  = 200;
class MatrixPanel_I2S_DMA;
extern MatrixPanel_I2S_DMA *dma_display;

// ── Villes du monde (4 coins) ─────────────────────────────────────────────────
struct City { char name[13]; int8_t offset_h; };
City cities[4] = {
  {"NEW YORK",  -5},   // gauche haut
  {"TOKYO",      9},   // gauche bas
  {"MOSCOU",     3},   // droite haut
  {"SYDNEY",    10}    // droite bas
};

// ── EEPROM layout ─────────────────────────────────────────────────────────────
// Byte 0    : offset UTC local (1–14)
// Byte 1    : magic 0xAC  (villes déjà sauvegardées ?)
// Bytes 2–5 : offsets 4 villes, stockés + 12 en uint8_t (range 0–24)
// Bytes 6–57: noms 4 villes, 13 octets chacun (12 chars + '\0')
#define EE_MAGIC_VAL  0xAC
#define EE_MAGIC_B    1
#define EE_CITY_OFF   2
#define EE_CITY_NAME  6
#define EE_BRIGHT_C  58   // bright_center
#define EE_BRIGHT_S  59   // bright_sides
#define EE_SIZE      60

// ── GPS / NMEA ───────────────────────────────────────────────────────────────
HardwareSerial gpsSerial(1);

volatile uint32_t pps_capture_us = 0;
volatile uint8_t  gps_h = 0, gps_m = 0, gps_s = 0;
volatile uint8_t  gps_day = 1, gps_month = 1;
volatile uint16_t gps_year = 2000;
volatile bool     gps_valid = false;
volatile uint8_t  gps_satellites = 0;
volatile float    gps_speed_kmh = 0.0f;
float             gps_lat = 48.85f, gps_lon = 2.35f;  // défaut Paris

char    nmea_buf[100];
uint8_t nmea_pos = 0;

const char* nmea_field(const char* sentence, uint8_t n) {
  const char* p = sentence;
  for (uint8_t i = 0; i < n; i++) {
    p = strchr(p, ',');
    if (!p) return nullptr;
    p++;
  }
  return p;
}

void parse_nmea(const char* sentence) {
  bool is_rmc = (strncmp(sentence, "$GPRMC", 6) == 0 || strncmp(sentence, "$GNRMC", 6) == 0);
  bool is_gga = (strncmp(sentence, "$GPGGA", 6) == 0 || strncmp(sentence, "$GNGGA", 6) == 0);
  if (!is_rmc && !is_gga) return;

  const char* tf = nmea_field(sentence, 1);
  if (!tf || strlen(tf) < 6) return;

  uint8_t h = (tf[0]-'0')*10 + (tf[1]-'0');
  uint8_t m = (tf[2]-'0')*10 + (tf[3]-'0');
  uint8_t s = (tf[4]-'0')*10 + (tf[5]-'0');

  if (is_gga) {
    const char* sf = nmea_field(sentence, 7);
    if (sf) gps_satellites = atoi(sf);
  }

  if (is_rmc) {
    const char* st = nmea_field(sentence, 2);
    if (!st || *st != 'A') { gps_valid = false; return; }

    const char* vf = nmea_field(sentence, 7);
    if (vf) gps_speed_kmh = atof(vf) * 1.852f;

    const char* latf = nmea_field(sentence, 3);
    const char* nsf  = nmea_field(sentence, 4);
    const char* lonf = nmea_field(sentence, 5);
    const char* ewf  = nmea_field(sentence, 6);
    if (latf && nsf && lonf && ewf && strlen(latf) > 4) {
      float lr = atof(latf); int ld = (int)(lr / 100);
      gps_lat = ld + (lr - ld * 100) / 60.0f;
      if (*nsf == 'S') gps_lat = -gps_lat;
      float pr = atof(lonf); int pd = (int)(pr / 100);
      gps_lon = pd + (pr - pd * 100) / 60.0f;
      if (*ewf == 'W') gps_lon = -gps_lon;
    }

    const char* df = nmea_field(sentence, 9);
    if (df && strlen(df) >= 6) {
      gps_day   = (df[0]-'0')*10 + (df[1]-'0');
      gps_month = (df[2]-'0')*10 + (df[3]-'0');
      gps_year  = 2000 + (df[4]-'0')*10 + (df[5]-'0');
    }
  }

  gps_h = h; gps_m = m; gps_s = s;
  gps_valid = true;
}

// ── PPS ──────────────────────────────────────────────────────────────────────
volatile uint8_t disp_h = 0, disp_m = 0, disp_s = 0;
volatile bool    pps_triggered = false;
volatile bool    dots_on = true;

void IRAM_ATTR pps_isr() {
  if (!gps_valid) return;
  pps_capture_us = micros();

  uint8_t s = gps_s + 1, m = gps_m, h = gps_h;
  if (s >= 60) { s = 0; m++; }
  if (m >= 60) { m = 0; h++; }
  if (h >= 24)   h = 0;
  disp_h = h; disp_m = m; disp_s = s;

  dots_on = !dots_on;
  pps_triggered = true;
}

void apply_timezone(uint8_t &h, uint8_t &m) {
  int total = (int)h * 60 + (int)m + UTC_OFFSET_H * 60 + UTC_OFFSET_M;
  total = ((total % 1440) + 1440) % 1440;
  h = total / 60;
  m = total % 60;
}

// ── Timestamp Unix / NTP ─────────────────────────────────────────────────────
static const uint32_t NTP_UNIX_OFFSET = 2208988800UL;
static const uint8_t  DAYS_PER_MONTH[12] = {31,28,31,30,31,30,31,31,30,31,30,31};

bool is_leap(uint16_t y) {
  return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

uint32_t date_to_unix(uint8_t day, uint8_t month, uint16_t year,
                      uint8_t h, uint8_t m, uint8_t s) {
  uint32_t days = 0;
  for (uint16_t y = 1970; y < year; y++)
    days += is_leap(y) ? 366 : 365;
  for (uint8_t mo = 0; mo < month - 1; mo++) {
    days += DAYS_PER_MONTH[mo];
    if (mo == 1 && is_leap(year)) days++;
  }
  days += day - 1;
  return days * 86400UL + (uint32_t)h * 3600UL + (uint32_t)m * 60UL + s;
}

// ── NTP Server ───────────────────────────────────────────────────────────────
EthernetUDP udp;
bool     eth_ready       = false;
uint32_t last_pps_unix   = 0;
uint32_t last_pps_micros = 0;
uint32_t last_pps_millis = 0;

void update_ntp_ref(uint32_t unix_ts) {
  last_pps_unix   = unix_ts;
  last_pps_micros = pps_capture_us;
}

uint64_t get_ntp_timestamp() {
  uint32_t elapsed_us = micros() - last_pps_micros;
  uint32_t secs = last_pps_unix + elapsed_us / 1000000UL;
  uint32_t frac = (uint32_t)((elapsed_us % 1000000UL) * 4294.967296);
  return ((uint64_t)(secs + NTP_UNIX_OFFSET) << 32) | frac;
}

void handle_ntp() {
  int size = udp.parsePacket();
  if (size < 48) return;
  uint8_t req[48] = {0};
  udp.read(req, 48);
  if ((req[0] & 0x07) != 3) return;

  uint8_t resp[48] = {0};
  resp[0]  = (0 << 6) | (4 << 3) | 4;
  resp[1]  = 1;
  resp[2]  = req[2];
  resp[3]  = (uint8_t)(-20);
  resp[10] = 0x00; resp[11] = 0x10;
  resp[12] = 'G'; resp[13] = 'P'; resp[14] = 'S'; resp[15] = ' ';

  uint32_t ref_secs = last_pps_unix + NTP_UNIX_OFFSET;
  resp[16] = ref_secs >> 24; resp[17] = ref_secs >> 16;
  resp[18] = ref_secs >> 8;  resp[19] = ref_secs;
  memcpy(&resp[24], &req[40], 8);

  uint64_t ts = get_ntp_timestamp();
  for (int i = 0; i < 8; i++) resp[32+i] = ts >> (56 - i*8);
  ts = get_ntp_timestamp();
  for (int i = 0; i < 8; i++) resp[40+i] = ts >> (56 - i*8);

  udp.beginPacket(udp.remoteIP(), udp.remotePort());
  udp.write(resp, 48);
  udp.endPacket();
}

// ── Page HTML OTA ─────────────────────────────────────────────────────────────
const char OTA_PAGE[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="fr">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>OTA Update</title>
<style>
  :root{--bg:#0d1117;--card:#161b22;--border:#30363d;--accent:#58a6ff;--text:#e6edf3;--muted:#8b949e;}
  *{box-sizing:border-box;margin:0;padding:0;}
  body{background:var(--bg);color:var(--text);font-family:'Segoe UI',system-ui,sans-serif;
       min-height:100vh;display:flex;align-items:center;justify-content:center;padding:24px;}
  .card{background:var(--card);border:1px solid var(--border);border-radius:12px;
        padding:32px;width:100%;max-width:420px;}
  h1{font-size:1.2rem;color:var(--accent);margin-bottom:8px;}
  p{color:var(--muted);font-size:.85rem;margin-bottom:24px;line-height:1.5;}
  .drop-zone{border:2px dashed var(--border);border-radius:8px;padding:32px;
             text-align:center;cursor:pointer;transition:border-color .2s;margin-bottom:16px;}
  .drop-zone:hover,.drop-zone.active{border-color:var(--accent);}
  .drop-zone input{display:none;}
  .drop-zone label{cursor:pointer;color:var(--muted);font-size:.9rem;}
  .file-name{color:var(--accent);font-size:.85rem;margin-top:8px;}
  button{width:100%;padding:12px;border:none;border-radius:8px;background:var(--accent);
         color:#000;font-size:1rem;font-weight:700;cursor:pointer;transition:opacity .2s;}
  button:disabled{opacity:.4;cursor:not-allowed;}
  .progress-wrap{display:none;margin-top:16px;}
  .bar{height:6px;background:var(--border);border-radius:3px;overflow:hidden;}
  .fill{height:100%;background:var(--accent);width:0%;transition:width .3s;}
  .status{text-align:center;font-size:.85rem;margin-top:8px;color:var(--muted);}
  .back{display:block;text-align:center;margin-top:20px;color:var(--muted);
        font-size:.8rem;text-decoration:none;}
  .back:hover{color:var(--accent);}
  .reboot-btn{position:fixed;top:12px;right:12px;padding:5px 10px;border:1px solid #f8514944;
    border-radius:6px;background:transparent;color:#f85149;font-size:.75rem;cursor:pointer;
    white-space:nowrap;transition:border-color .2s;width:auto;}
  .reboot-btn:hover{border-color:#f85149;}
</style>
</head>
<body>
<button class="reboot-btn" onclick="reboot()">Reboot</button>
<div class="card">
  <h1>Mise a jour firmware</h1>
  <p>Fichier .bin genere par PlatformIO :<br>
     <code style="color:var(--accent)">.pio/build/esp32-s3-devkitc-1/firmware.bin</code></p>
  <form id="frm" method="POST" action="/ota/upload" enctype="multipart/form-data">
    <div class="drop-zone" id="dz">
      <input type="file" name="firmware" id="fi" accept=".bin">
      <label for="fi">Cliquez ou glissez un .bin ici</label>
      <div class="file-name" id="fn"></div>
    </div>
    <button type="submit" id="btn" disabled>Flasher le firmware</button>
  </form>
  <div class="progress-wrap" id="pw">
    <div class="bar"><div class="fill" id="fill"></div></div>
    <div class="status" id="st">Envoi en cours...</div>
  </div>
  <a href="/" class="back">Retour au tableau de bord</a>
</div>
<script>
const fi=document.getElementById('fi'),fn=document.getElementById('fn');
const btn=document.getElementById('btn'),frm=document.getElementById('frm');
const pw=document.getElementById('pw'),fill=document.getElementById('fill');
const st=document.getElementById('st'),dz=document.getElementById('dz');
fi.addEventListener('change',()=>{
  if(fi.files[0]){fn.textContent=fi.files[0].name+' ('+(fi.files[0].size/1024).toFixed(1)+' KB)';btn.disabled=false;}
});
dz.addEventListener('dragover',e=>{e.preventDefault();dz.classList.add('active');});
dz.addEventListener('dragleave',()=>dz.classList.remove('active'));
dz.addEventListener('drop',e=>{e.preventDefault();dz.classList.remove('active');fi.files=e.dataTransfer.files;fi.dispatchEvent(new Event('change'));});
frm.addEventListener('submit',e=>{
  e.preventDefault();
  const xhr=new XMLHttpRequest();
  xhr.open('POST','/ota/upload');
  xhr.upload.onprogress=ev=>{if(ev.lengthComputable){const p=Math.round(ev.loaded/ev.total*100);fill.style.width=p+'%';st.textContent='Envoi : '+p+'%';}};
  xhr.onload=()=>{if(xhr.status===200){fill.style.width='100%';st.textContent='Flash OK - Redemarrage dans 5s...';setTimeout(()=>window.location.href='/',6000);}else{st.textContent='Erreur ('+xhr.status+')';btn.disabled=false;}};
  xhr.onerror=()=>{st.textContent='Erreur reseau';btn.disabled=false;};
  pw.style.display='block';btn.disabled=true;xhr.send(new FormData(frm));
});
function reboot(){if(!confirm('Redemarrer ?'))return;fetch('/reboot').finally(()=>{document.querySelector('.reboot-btn').textContent='Redemarrage...';});}
</script>
</body>
</html>
)rawhtml";

const char HTML_PAGE[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="fr">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>GPS Clock</title>
<style>
  :root{--bg:#0d1117;--card:#161b22;--border:#30363d;--accent:#58a6ff;--green:#3fb950;--red:#f85149;--yellow:#d29922;--text:#e6edf3;--muted:#8b949e;}
  *{box-sizing:border-box;margin:0;padding:0;}
  body{background:var(--bg);color:var(--text);font-family:'Segoe UI',system-ui,sans-serif;min-height:100vh;padding:24px 16px;}
  h1{text-align:center;font-size:1.3rem;color:var(--accent);letter-spacing:.05em;margin-bottom:24px;}
  .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(280px,1fr));gap:16px;max-width:1000px;margin:0 auto;}
  .card{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:20px;}
  .card h2{font-size:.75rem;text-transform:uppercase;letter-spacing:.1em;color:var(--muted);margin-bottom:16px;}
  .time-display{font-size:3rem;font-weight:700;letter-spacing:.05em;color:var(--accent);text-align:center;font-variant-numeric:tabular-nums;}
  .date-display{text-align:center;color:var(--muted);font-size:.9rem;margin-top:6px;}
  .row{display:flex;justify-content:space-between;align-items:center;padding:8px 0;border-bottom:1px solid var(--border);font-size:.9rem;}
  .row:last-child{border-bottom:none;}
  .label{color:var(--muted);}
  .value{font-weight:600;}
  .badge{display:inline-block;padding:2px 10px;border-radius:999px;font-size:.75rem;font-weight:700;}
  .badge-green{background:rgba(63,185,80,.15);color:var(--green);}
  .badge-red{background:rgba(248,81,73,.15);color:var(--red);}
  .badge-yellow{background:rgba(210,153,34,.15);color:var(--yellow);}
  .btn-group{display:flex;gap:10px;margin-top:16px;}
  .ota-btn{position:fixed;top:16px;left:16px;background:var(--card);border:1px solid var(--border);color:var(--muted);padding:8px 14px;border-radius:8px;font-size:.8rem;text-decoration:none;transition:border-color .2s,color .2s;}
  .ota-btn:hover{border-color:var(--accent);color:var(--accent);}
  button{flex:1;padding:12px;border:1px solid var(--border);border-radius:8px;background:var(--bg);color:var(--text);font-size:.9rem;cursor:pointer;transition:border-color .2s,background .2s;}
  button:hover{border-color:var(--accent);}
  button.active{background:var(--accent);color:#000;border-color:var(--accent);font-weight:700;}
  .ntp-info{text-align:center;color:var(--muted);font-size:.8rem;margin-top:20px;}
  .ntp-info span{color:var(--accent);font-weight:600;}
  .slider-wrap{margin-top:16px;}
  .slider-label{display:flex;justify-content:space-between;font-size:.8rem;color:var(--muted);margin-bottom:6px;}
  .slider-label span{color:var(--accent);font-weight:600;}
  input[type=range]{width:100%;accent-color:var(--accent);cursor:pointer;}
  .world-grid{display:grid;grid-template-columns:1fr 1fr;gap:10px;}
  .world-city{background:var(--bg);border:1px solid var(--border);border-radius:8px;padding:10px 12px;}
  .world-city .city-name{font-size:.85rem;text-transform:uppercase;letter-spacing:.06em;color:var(--muted);margin-bottom:4px;}
  .world-city .city-time{font-size:2rem;font-weight:700;font-variant-numeric:tabular-nums;}
  .world-city .city-tz{font-size:.75rem;color:var(--muted);margin-top:2px;}
  .city-sel-lbl{font-size:.7rem;text-transform:uppercase;letter-spacing:.08em;color:var(--muted);margin-bottom:6px;}
  .city-sel-wrap{margin-bottom:4px;}
  select{width:100%;background:var(--card);color:var(--text);border:1px solid var(--border);border-radius:8px;padding:8px 28px 8px 10px;font-size:.85rem;cursor:pointer;appearance:none;-webkit-appearance:none;background-image:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='10' height='6'%3E%3Cpath d='M0 0l5 6 5-6z' fill='%238b949e'/%3E%3C/svg%3E");background-repeat:no-repeat;background-position:right 10px center;transition:border-color .2s;}
  select:hover{border-color:var(--accent);}
  select:focus{outline:none;border-color:var(--accent);}
</style>
</head>
<body>
<a href="/ota" class="ota-btn">&#9881; OTA</a>
<h1>&#128752; GPS Clock</h1>
<div class="grid">
  <div class="card">
    <h2>Heure locale</h2>
    <div class="time-display" id="local-time">--:--:--</div>
    <div class="date-display" id="local-date">--/--/----</div>
    <div class="btn-group" style="margin-top:20px">
      <button id="btn-winter" onclick="setOffset(1)">&#127769; Hiver UTC+1</button>
      <button id="btn-summer" onclick="setOffset(2)">&#9728; Ete UTC+2</button>
    </div>
  </div>
  <div class="card">
    <h2>Heure UTC</h2>
    <div class="time-display" id="utc-time" style="color:#3fb950">--:--:--</div>
    <div class="date-display" id="utc-date">--/--/----</div>
    <div class="row" style="margin-top:16px">
      <span class="label">Statut GPS</span>
      <span id="gps-status" class="badge badge-red">Attente</span>
    </div>
    <div class="row">
      <span class="label">Signal PPS</span>
      <span id="pps-status" class="badge badge-red">Inactif</span>
    </div>
    <div class="row">
      <span class="label">Satellites</span>
      <span class="value" id="sats">--</span>
    </div>
    <div class="row">
      <span class="label">IP</span>
      <span class="value" id="ntp-ip2">--</span>
    </div>
    <div class="slider-wrap">
      <div class="slider-label">
        <span>Luminosite lateraux</span>
        <span id="bright-sides-val">--</span>
      </div>
      <input type="range" id="bright-sides-slider" min="5" max="255" value="200"
             oninput="setBrightSides(this.value)">
    </div>
    <div class="slider-wrap">
      <div class="slider-label">
        <span>Luminosite centre</span>
        <span id="bright-center-val">--</span>
      </div>
      <input type="range" id="bright-center-slider" min="5" max="255" value="200"
             oninput="setBrightCenter(this.value)">
    </div>
  </div>
  <div class="card">
    <h2>Heures du monde</h2>
    <div class="world-grid">
      <div class="world-city">
        <div class="city-name" id="wn-0">---</div>
        <div class="city-time" style="color:#f0883e" id="wt-0">--:--</div>
        <div class="city-tz" id="wtz-0">---</div>
      </div>
      <div class="world-city">
        <div class="city-name" id="wn-2">---</div>
        <div class="city-time" style="color:#ff7b72" id="wt-2">--:--</div>
        <div class="city-tz" id="wtz-2">---</div>
      </div>
      <div class="world-city">
        <div class="city-name" id="wn-1">---</div>
        <div class="city-time" style="color:#f85149" id="wt-1">--:--</div>
        <div class="city-tz" id="wtz-1">---</div>
      </div>
      <div class="world-city">
        <div class="city-name" id="wn-3">---</div>
        <div class="city-time" style="color:#ffa657" id="wt-3">--:--</div>
        <div class="city-tz" id="wtz-3">---</div>
      </div>
    </div>
  </div>
  <div class="card">
    <h2>Météo GPS</h2>
    <div class="row">
      <span class="label">Position</span>
      <span class="value" id="gps-pos">--</span>
    </div>
    <div class="row">
      <span class="label">Température</span>
      <span class="value" id="weather-temp" style="color:var(--accent);font-size:1.4rem;">--</span>
    </div>
  </div>
  <div class="card">
    <h2>Configurer les villes</h2>
    <div style="display:grid;grid-template-columns:1fr 1fr;gap:12px;">
      <div class="city-sel-wrap">
        <div class="city-sel-lbl">&#9664; Gauche haut</div>
        <select id="city0" onchange="setCity(0,this.value)"></select>
      </div>
      <div class="city-sel-wrap">
        <div class="city-sel-lbl">Droite haut &#9654;</div>
        <select id="city2" onchange="setCity(2,this.value)"></select>
      </div>
      <div class="city-sel-wrap">
        <div class="city-sel-lbl">&#9664; Gauche bas</div>
        <select id="city1" onchange="setCity(1,this.value)"></select>
      </div>
      <div class="city-sel-wrap">
        <div class="city-sel-lbl">Droite bas &#9654;</div>
        <select id="city3" onchange="setCity(3,this.value)"></select>
      </div>
    </div>
  </div>
</div>
<div class="ntp-info">Serveur NTP actif sur <span id="ntp-ip">...</span> port 123</div>
<script>
let currentOffset=1;
let brightTimer=null;
let currentCities=[{n:"NEW YORK",o:-5},{n:"TOKYO",o:9},{n:"MOSCOU",o:3},{n:"SYDNEY",o:10}];
function pad(n){return String(n).padStart(2,'0');}
function setOffset(h){currentOffset=h;refreshButtons();fetch('/set_offset?h='+h);}
function setBrightSides(v){document.getElementById('bright-sides-val').textContent=v;clearTimeout(brightTimer);brightTimer=setTimeout(()=>fetch('/set_brightness_sides?v='+v),200);}
function setBrightCenter(v){document.getElementById('bright-center-val').textContent=v;clearTimeout(brightTimer);brightTimer=setTimeout(()=>fetch('/set_brightness_center?v='+v),200);}
function refreshButtons(){
  document.getElementById('btn-winter').classList.toggle('active',currentOffset===1);
  document.getElementById('btn-summer').classList.toggle('active',currentOffset===2);
}
function applyOffset(h,m,off){let total=h*60+m+off*60;total=((total%1440)+1440)%1440;return{h:Math.floor(total/60),m:total%60};}
function update(){
  fetch('/status').then(r=>r.json()).then(d=>{
    if(d.warmup){
      document.getElementById('utc-time').textContent='--:--:--';
      document.getElementById('local-time').textContent='--:--:--';
      document.getElementById('gps-status').textContent='Warmup...';
      document.getElementById('gps-status').className='badge badge-yellow';
      return;
    }
    document.getElementById('utc-time').textContent=pad(d.utc_h)+':'+pad(d.utc_m)+':'+pad(d.utc_s);
    document.getElementById('utc-date').textContent=pad(d.day)+'/'+pad(d.month)+'/'+d.year;
    const loc=applyOffset(d.utc_h,d.utc_m,currentOffset);
    document.getElementById('local-time').textContent=pad(loc.h)+':'+pad(loc.m)+':'+pad(d.utc_s);
    document.getElementById('local-date').textContent=pad(d.day)+'/'+pad(d.month)+'/'+d.year;
    if(d.offset&&d.offset!==currentOffset){currentOffset=d.offset;refreshButtons();}
    document.getElementById('sats').textContent=d.satellites;
    document.getElementById('ntp-ip').textContent=d.ip;
    document.getElementById('ntp-ip2').textContent=d.ip;
    const ss=document.getElementById('bright-sides-slider');
    if(!ss.matches(':active')){ss.value=d.bs;document.getElementById('bright-sides-val').textContent=d.bs;}
    const sc=document.getElementById('bright-center-slider');
    if(!sc.matches(':active')){sc.value=d.bc;document.getElementById('bright-center-val').textContent=d.bc;}
    const gpsEl=document.getElementById('gps-status');
    gpsEl.textContent=d.gps_valid?'Fix OK':'Attente fix';
    gpsEl.className='badge '+(d.gps_valid?'badge-green':'badge-red');
    const ppsEl=document.getElementById('pps-status');
    ppsEl.textContent=d.pps_active?'Actif':'Inactif';
    ppsEl.className='badge '+(d.pps_active?'badge-green':'badge-yellow');
    if(d.weather_temp>-900){const s=d.weather_temp>=0?'+':'';document.getElementById('weather-temp').textContent=s+d.weather_temp.toFixed(1)+'°C';}
    if(d.lat&&d.lon)document.getElementById('gps-pos').textContent=d.lat.toFixed(4)+' / '+d.lon.toFixed(4);
    for(let i=0;i<4;i++){const t=applyOffset(d.utc_h,d.utc_m,currentCities[i].o);document.getElementById('wt-'+i).textContent=pad(t.h)+':'+pad(t.m);}
  }).catch(()=>{});
}
const CITIES=[
  {n:"LOS ANGELES",o:-8},{n:"DENVER",o:-7},{n:"CHICAGO",o:-6},
  {n:"NEW YORK",o:-5},{n:"TORONTO",o:-5},{n:"BOGOTA",o:-5},
  {n:"SANTIAGO",o:-4},{n:"CARACAS",o:-4},{n:"BUENOS AIRES",o:-3},
  {n:"SAO PAULO",o:-3},{n:"ACORES",o:-1},
  {n:"LONDRES",o:0},{n:"LISBONNE",o:0},{n:"ACCRA",o:0},
  {n:"PARIS",o:1},{n:"BERLIN",o:1},{n:"MADRID",o:1},{n:"ROME",o:1},{n:"ALGER",o:1},
  {n:"ATHENES",o:2},{n:"LE CAIRE",o:2},{n:"JOHANNESBURG",o:2},{n:"HELSINKI",o:2},
  {n:"MOSCOU",o:3},{n:"ISTANBUL",o:3},{n:"RIYAD",o:3},{n:"NAIROBI",o:3},
  {n:"DUBAI",o:4},{n:"BAKOU",o:4},
  {n:"KARACHI",o:5},{n:"TACHKENT",o:5},
  {n:"DACCA",o:6},{n:"ALMATY",o:6},
  {n:"BANGKOK",o:7},{n:"HANOI",o:7},{n:"JAKARTA",o:7},
  {n:"PEKIN",o:8},{n:"SINGAPOUR",o:8},{n:"HONG KONG",o:8},{n:"PERTH",o:8},
  {n:"TOKYO",o:9},{n:"SEOUL",o:9},
  {n:"SYDNEY",o:10},{n:"MELBOURNE",o:10},{n:"BRISBANE",o:10},
  {n:"NOUMEA",o:11},
  {n:"AUCKLAND",o:12},{n:"FIDJI",o:12}
];
function buildCitySelects(){
  const opts=CITIES.map(function(c){return'<option value="'+c.n+'|'+c.o+'">'+c.n+' (UTC'+(c.o>=0?'+':'')+c.o+')</option>';}).join('');
  for(let i=0;i<4;i++)document.getElementById('city'+i).innerHTML=opts;
}
function fmtUTC(o){return'UTC'+(o>=0?'+':'')+o;}
function updateWorldDisplay(){
  for(let i=0;i<4;i++){
    document.getElementById('wn-'+i).textContent=currentCities[i].n;
    document.getElementById('wtz-'+i).textContent=fmtUTC(currentCities[i].o);
  }
}
function setCity(idx,val){
  const p=val.split('|');
  currentCities[idx]={n:p[0],o:parseInt(p[1])};
  updateWorldDisplay();
  fetch('/set_city?idx='+idx+'&name='+encodeURIComponent(p[0])+'&off='+p[1]);
}
function loadCities(){
  fetch('/cities').then(function(r){return r.json();}).then(function(d){
    d.cities.forEach(function(c,i){
      currentCities[i]={n:c.name,o:c.off};
      const sel=document.getElementById('city'+i);
      const v=c.name+'|'+c.off;
      for(let j=0;j<sel.options.length;j++){if(sel.options[j].value===v){sel.value=v;break;}}
    });
    updateWorldDisplay();
  }).catch(function(){});
}
buildCitySelects();loadCities();update();setInterval(update,1000);refreshButtons();
</script>
</body>
</html>
)rawhtml";

// ── Web server ────────────────────────────────────────────────────────────────
class MyWebServer : public EthernetServer {
public:
  MyWebServer(uint16_t port) : EthernetServer(port) {}
  void begin()                       { EthernetServer::begin(); }
  void begin(uint16_t port) override { EthernetServer::begin(); }
};
MyWebServer webServer(80);

void http_send(EthernetClient& client, int code, const char* ctype, const String& body) {
  client.print("HTTP/1.1 "); client.print(code); client.println(" OK");
  client.print("Content-Type: "); client.println(ctype);
  client.print("Content-Length: "); client.println(body.length());
  client.println("Connection: close");
  client.println();
  client.print(body);
}

void http_send_progmem(EthernetClient& client, const char* html) {
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html");
  client.println("Connection: close");
  client.println();
  char buf[64];
  size_t len = strlen_P(html);
  size_t pos = 0;
  while (pos < len) {
    size_t chunk = min((size_t)64, len - pos);
    memcpy_P(buf, html + pos, chunk);
    client.write((uint8_t*)buf, chunk);
    pos += chunk;
  }
}

void handle_ota_upload(EthernetClient& client, const String& headers) {
  int clIdx = headers.indexOf("Content-Length: ");
  if (clIdx < 0) { client.println("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n"); return; }
  int contentLength = headers.substring(clIdx + 16, headers.indexOf("\r\n", clIdx)).toInt();

  int bIdx = headers.indexOf("boundary=");
  if (bIdx < 0) { client.println("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n"); return; }
  String boundary = headers.substring(bIdx + 9, headers.indexOf("\r\n", bIdx));
  boundary.trim();

  String partHeader = "";
  uint32_t t = millis();
  while (client.connected() && millis() - t < 10000) {
    if (client.available()) {
      partHeader += (char)client.read();
      t = millis();
      if (partHeader.endsWith("\r\n\r\n")) break;
    }
  }

  String closing = "\r\n--" + boundary + "--\r\n";
  int firmwareSize = contentLength - (int)partHeader.length() - (int)closing.length();

  if (firmwareSize <= 0) {
    client.println("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\nTaille invalide");
    return;
  }

  Serial.printf("OTA: debut flash, taille = %d octets\n", firmwareSize);

  if (!Update.begin(firmwareSize)) {
    Update.printError(Serial);
    client.println("HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\nErreur begin");
    return;
  }

  uint8_t buf[512];
  int remaining = firmwareSize;
  t = millis();

  while (remaining > 0 && client.connected() && millis() - t < 60000) {
    int avail = client.available();
    if (avail > 0) {
      int n = client.read(buf, min((int)sizeof(buf), min(remaining, avail)));
      if (n > 0) {
        Update.write(buf, n);
        remaining -= n;
        t = millis();
        Serial.printf("OTA: %d / %d octets\n", firmwareSize - remaining, firmwareSize);
      }
    }
  }

  if (remaining == 0 && Update.end(true)) {
    Serial.println("OTA: flash OK, redemarrage...");
    client.println("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nOK");
    delay(500);
    client.stop();
    ESP.restart();
  } else {
    Update.printError(Serial);
    client.println("HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\nErreur flash");
  }
}

void url_decode(const String& src, char* dst, int maxlen) {
  int di = 0;
  for (int i = 0; i < (int)src.length() && di < maxlen - 1; i++) {
    char c = src[i];
    if (c == '+') {
      dst[di++] = ' ';
    } else if (c == '%' && i + 2 < (int)src.length()) {
      char h = src[i+1], l = src[i+2];
      uint8_t hv = (h>='0'&&h<='9')?(h-'0'):(h>='A'&&h<='F')?(h-'A'+10):(h>='a'&&h<='f')?(h-'a'+10):0;
      uint8_t lv = (l>='0'&&l<='9')?(l-'0'):(l>='A'&&l<='F')?(l-'A'+10):(l>='a'&&l<='f')?(l-'a'+10):0;
      dst[di++] = (char)((hv << 4) | lv);
      i += 2;
    } else {
      dst[di++] = c;
    }
  }
  dst[di] = '\0';
}

void save_cities() {
  EEPROM.write(EE_MAGIC_B, EE_MAGIC_VAL);
  for (int i = 0; i < 4; i++) {
    EEPROM.write(EE_CITY_OFF + i, (uint8_t)(cities[i].offset_h + 12));
    for (int j = 0; j < 13; j++)
      EEPROM.write(EE_CITY_NAME + i * 13 + j, (uint8_t)cities[i].name[j]);
  }
  EEPROM.commit();
}

void load_cities() {
  if (EEPROM.read(EE_MAGIC_B) != EE_MAGIC_VAL) return;
  for (int i = 0; i < 4; i++) {
    uint8_t v = EEPROM.read(EE_CITY_OFF + i);
    if (v <= 24) cities[i].offset_h = (int8_t)(v - 12);
    for (int j = 0; j < 12; j++) {
      char c = (char)EEPROM.read(EE_CITY_NAME + i * 13 + j);
      cities[i].name[j] = (c >= 32 && c < 127) ? c : '\0';
    }
    cities[i].name[12] = '\0';
  }
}

void handle_web_client() {
  EthernetClient client = webServer.available();
  if (!client) return;

  String req = "";
  uint32_t t = millis();
  while (client.connected()) {
    if (millis() - t > 1000) break;
    if (client.available()) {
      char c = client.read();
      req += c;
      if (req.endsWith("\r\n\r\n")) break;
    }
  }

  String path = "";
  int s = req.indexOf(' ');
  int e = req.indexOf(' ', s + 1);
  if (s >= 0 && e > s) path = req.substring(s + 1, e);

  if (path == "/cities") {
    char json[220];
    snprintf(json, sizeof(json),
      "{\"cities\":["
      "{\"name\":\"%s\",\"off\":%d},"
      "{\"name\":\"%s\",\"off\":%d},"
      "{\"name\":\"%s\",\"off\":%d},"
      "{\"name\":\"%s\",\"off\":%d}"
      "]}",
      cities[0].name, cities[0].offset_h,
      cities[1].name, cities[1].offset_h,
      cities[2].name, cities[2].offset_h,
      cities[3].name, cities[3].offset_h
    );
    http_send(client, 200, "application/json", String(json));
  }
  else if (path.startsWith("/set_city")) {
    int ii = path.indexOf("idx=");
    int ni = path.indexOf("name=");
    int oi = path.indexOf("off=");
    if (ii >= 0 && ni >= 0 && oi >= 0) {
      int idx = path.substring(ii + 4).toInt();
      if (idx >= 0 && idx <= 3) {
        int ns = ni + 5;
        int ne = path.indexOf('&', ns);
        String rawname = (ne > ns) ? path.substring(ns, ne) : path.substring(ns);
        int off = path.substring(oi + 4).toInt();
        if (off >= -12 && off <= 12) {
          url_decode(rawname, cities[idx].name, sizeof(cities[idx].name));
          cities[idx].offset_h = (int8_t)off;
          save_cities();
        }
      }
    }
    http_send(client, 200, "text/plain", "OK");
  }
  else if (path.startsWith("/set_brightness")) {
    int idx = path.indexOf("v=");
    if (idx >= 0) {
      int v = path.substring(idx + 2).toInt();
      if (v >= 5 && v <= 255) {
        if (path.startsWith("/set_brightness_center")) {
          bright_center = (uint8_t)v;
          EEPROM.write(EE_BRIGHT_C, bright_center);
        } else {
          bright_sides = (uint8_t)v;
          EEPROM.write(EE_BRIGHT_S, bright_sides);
        }
        EEPROM.commit();
      }
    }
    http_send(client, 200, "text/plain", "OK");
  }
  else if (path.startsWith("/set_offset")) {
    int idx = path.indexOf("h=");
    if (idx >= 0) {
      int h = path.substring(idx + 2).toInt();
      if (h >= 0 && h <= 14) {
        UTC_OFFSET_H = h;
        EEPROM.write(0, (uint8_t)h);
        EEPROM.commit();
      }
    }
    http_send(client, 200, "text/plain", "OK");
  }
  else if (path.startsWith("/status")) {
    uint8_t lh = disp_h, lm = disp_m;
    apply_timezone(lh, lm);
    bool pps_active = (millis() - last_pps_millis) < 2000;
    IPAddress ip = Ethernet.localIP();
    char json[560];
    snprintf(json, sizeof(json),
      "{\"utc_h\":%d,\"utc_m\":%d,\"utc_s\":%d,"
      "\"loc_h\":%d,\"loc_m\":%d,"
      "\"day\":%d,\"month\":%d,\"year\":%d,"
      "\"satellites\":%d,\"speed\":%.1f,"
      "\"gps_valid\":%s,\"pps_active\":%s,"
      "\"offset\":%d,\"bc\":%d,\"bs\":%d,\"ip\":\"%d.%d.%d.%d\","
      "\"weather_temp\":%.1f,\"lat\":%.4f,\"lon\":%.4f,\"warmup\":%s}",
      disp_h, disp_m, disp_s,
      lh, lm,
      gps_day, gps_month, gps_year,
      gps_satellites, gps_speed_kmh,
      gps_valid  ? "true" : "false",
      pps_active ? "true" : "false",
      UTC_OFFSET_H, bright_center, bright_sides,
      ip[0], ip[1], ip[2], ip[3],
      weather_temp, gps_lat, gps_lon,
      warmup_done ? "false" : "true"
    );
    http_send(client, 200, "application/json", String(json));
  }
  else if (path == "/ota") {
    if (!check_auth(req)) send_auth_required(client);
    else http_send_progmem(client, OTA_PAGE);
  }
  else if (path.startsWith("/ota/upload")) {
    if (!check_auth(req)) send_auth_required(client);
    else handle_ota_upload(client, req);
  }
  else if (path == "/reboot") {
    client.println("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nOK");
    delay(200);
    client.stop();
    ESP.restart();
  }
  else {
    http_send_progmem(client, HTML_PAGE);
  }

  delay(1);
  client.stop();
}

// ── Fetch météo (open-meteo HTTP, sans clé) ───────────────────────────────────
bool fetch_weather() {
  EthernetClient wc;
  if (!wc.connect("api.open-meteo.com", 80)) { wc.stop(); return false; }

  char req[220];
  snprintf(req, sizeof(req),
    "GET /v1/forecast?latitude=%.4f&longitude=%.4f&current_weather=true HTTP/1.0\r\n"
    "Host: api.open-meteo.com\r\n"
    "Connection: close\r\n\r\n",
    gps_lat, gps_lon);
  wc.print(req);

  const char* TARGET = "\"temperature\":";
  int  match = 0;
  char valbuf[16] = {0};
  int  valpos = 0;
  bool found  = false;
  uint32_t t  = millis();

  while (wc.connected() && millis() - t < 5000) {
    if (wc.available()) {
      char c = wc.read(); t = millis();
      if (!found) {
        if (c == TARGET[match]) { if (++match == 14) found = true; }
        else match = (c == TARGET[0]) ? 1 : 0;
      } else {
        if (valpos == 0 && c == '"') { found = false; match = 0; } // valeur string, pas un nombre
        else if (valpos < 15 && (c == '-' || c == '.' || (c >= '0' && c <= '9')))
          valbuf[valpos++] = c;
        else if (valpos > 0) break;
      }
    }
  }
  wc.stop();
  if (!found || valpos == 0) return false;
  valbuf[valpos] = '\0';
  weather_temp = atof(valbuf);
  Serial.printf("Meteo: %.1f C (%.4f, %.4f)\n", weather_temp, gps_lat, gps_lon);
  return true;
}

// ── LED Matrix ────────────────────────────────────────────────────────────────
MatrixPanel_I2S_DMA                      *dma_display = nullptr;
VirtualMatrixPanel_T<CHAIN_TOP_LEFT_DOWN> *matrix      = nullptr;

// Palette (initialisée dans init_matrix)
uint16_t COL_BLACK, COL_CYAN, COL_GREEN, COL_RED, COL_YELLOW, COL_WHITE, COL_DIM;

void init_matrix() {
  HUB75_I2S_CFG mxconfig(PANEL_W, PANEL_H, PANEL_CHAIN);
  mxconfig.gpio.r1  = HUB_R1;
  mxconfig.gpio.g1  = HUB_G1;
  mxconfig.gpio.b1  = HUB_B1;
  mxconfig.gpio.r2  = HUB_R2;
  mxconfig.gpio.g2  = HUB_G2;
  mxconfig.gpio.b2  = HUB_B2;
  mxconfig.gpio.a   = HUB_A;
  mxconfig.gpio.b   = HUB_B;
  mxconfig.gpio.c   = HUB_C;
  mxconfig.gpio.d   = HUB_D;
  mxconfig.gpio.e   = HUB_E;
  mxconfig.gpio.clk = HUB_CLK;
  mxconfig.gpio.lat = HUB_LAT;
  mxconfig.gpio.oe  = HUB_OE;
  mxconfig.clkphase    = false;
  mxconfig.driver      = HUB75_I2S_CFG::FM6126A;
  mxconfig.double_buff = false;

  dma_display = new MatrixPanel_I2S_DMA(mxconfig);
  dma_display->begin();
  dma_display->setBrightness8(255);
  dma_display->clearScreen();

  matrix = new VirtualMatrixPanel_T<CHAIN_TOP_LEFT_DOWN>(PANELS_TALL, PANELS_WIDE, PANEL_W, PANEL_H);
  matrix->setDisplay(*dma_display);

  COL_BLACK  = matrix->color565(  0,   0,   0);
  COL_CYAN   = matrix->color565(  0, 210, 210);
  COL_GREEN  = matrix->color565( 50, 200,  50);
  COL_RED    = matrix->color565(200,  40,  40);
  COL_YELLOW = matrix->color565(200, 170,   0);
  COL_WHITE  = matrix->color565(255, 255, 255);
  COL_DIM    = matrix->color565(160, 160, 160);
}

// ── Scaling couleur par zone ──────────────────────────────────────────────────
static inline uint16_t sc(uint16_t c, uint8_t b) {
  if (b == 255) return c;
  uint8_t r = ((c >> 11) & 0x1F) * b / 255;
  uint8_t g = ((c >>  5) & 0x3F) * b / 255;
  uint8_t bl= ( c        & 0x1F) * b / 255;
  return ((uint16_t)r << 11) | ((uint16_t)g << 5) | bl;
}

// ── Helpers panneaux ─────────────────────────────────────────────────────────
#define PANEL_LEFT_X   0
#define PANEL_MID_X    PANEL_W
#define PANEL_RIGHT_X  (PANEL_W * 2)

void mx_at(const char* text, int x, int y, uint16_t color, uint8_t size) {
  matrix->setTextSize(size);
  matrix->setTextColor(color, COL_BLACK);
  matrix->setCursor(x, y);
  matrix->print(text);
}

void mx_panel_center(const char* text, int px, int py, uint16_t color, uint8_t size) {
  int w = strlen(text) * 6 * size;
  mx_at(text, px + (PANEL_W - w) / 2, py, color, size);
}

void world_time(uint8_t utc_h, uint8_t utc_m, int8_t off_h,
                uint8_t &out_h, uint8_t &out_m) {
  int total = (int)utc_h * 60 + (int)utc_m + off_h * 60;
  total = ((total % 1440) + 1440) % 1440;
  out_h = total / 60;
  out_m = total % 60;
}

// ── Rendu complet ─────────────────────────────────────────────────────────────
void render_all(uint8_t loc_h, uint8_t loc_m, uint8_t s, bool colon_on,
                bool gps_ok, bool pps_ok) {
  char sep = colon_on ? ':' : ' ';
  uint8_t ch, cm;
  char tbuf[10];
  char lbl[22];

  // ═══ PANNEAU GAUCHE : villes 0 & 1 ═════════════════════════════════════════
  snprintf(lbl, sizeof(lbl), "%s UTC%+d", cities[0].name, cities[0].offset_h);
  mx_panel_center(lbl, PANEL_LEFT_X,  2, sc(COL_DIM, bright_sides), 1);
  world_time(disp_h, disp_m, cities[0].offset_h, ch, cm);
  snprintf(tbuf, sizeof(tbuf), "%02d:%02d", ch, cm);
  mx_panel_center(tbuf, PANEL_LEFT_X, 11, sc(matrix->color565(96, 56, 20), bright_sides), 2);
  matrix->drawFastHLine(PANEL_LEFT_X, 32, PANEL_W, sc(COL_DIM, bright_sides));
  snprintf(lbl, sizeof(lbl), "%s UTC%+d", cities[1].name, cities[1].offset_h);
  mx_panel_center(lbl, PANEL_LEFT_X, 35, sc(COL_DIM, bright_sides), 1);
  world_time(disp_h, disp_m, cities[1].offset_h, ch, cm);
  snprintf(tbuf, sizeof(tbuf), "%02d:%02d", ch, cm);
  mx_panel_center(tbuf, PANEL_LEFT_X, 44, sc(matrix->color565(99, 32, 29), bright_sides), 2);

  // ═══ PANNEAU MILIEU : heure locale + UTC ════════════════════════════════════
  snprintf(lbl, sizeof(lbl), "LOCAL UTC%+d", UTC_OFFSET_H);
  mx_panel_center(lbl, PANEL_MID_X, 2, sc(COL_DIM, bright_center), 1);
  snprintf(tbuf, sizeof(tbuf), "%02d%c%02d%c%02d", loc_h, sep, loc_m, sep, s);
  mx_panel_center(tbuf, PANEL_MID_X, 11, sc(gps_ok ? COL_CYAN : COL_YELLOW, bright_center), 2);
  matrix->drawFastHLine(PANEL_MID_X,      32, 52, sc(COL_DIM, bright_center));
  mx_panel_center("UTC", PANEL_MID_X, 28, sc(COL_DIM, bright_center), 1);
  matrix->drawFastHLine(PANEL_MID_X + 76, 32, 52, sc(COL_DIM, bright_center));
  snprintf(tbuf, sizeof(tbuf), "%02d%c%02d%c%02d", disp_h, sep, disp_m, sep, s);
  mx_panel_center(tbuf, PANEL_MID_X, 38, sc(COL_GREEN, bright_center), 2);
  if (weather_temp > -900.0f) {
    uint16_t wc = sc(COL_DIM, bright_center);
    char numpart[8];
    snprintf(numpart, sizeof(numpart), "%+.0f", weather_temp);
    matrix->setTextSize(1);
    matrix->setTextColor(wc, COL_BLACK);
    matrix->setCursor(PANEL_MID_X + 2, 56);
    matrix->print(numpart);
    int16_t cx = matrix->getCursorX();
    int16_t cy = matrix->getCursorY();
    matrix->fillRect(cx, cy, 6, 8, COL_BLACK);
    matrix->drawCircle(cx + 2, cy + 1, 1, wc);
    matrix->setCursor(cx + 5, cy);
    matrix->setTextColor(wc, COL_BLACK);
    matrix->print("C");
  } else {
    mx_at("      ", PANEL_MID_X + 2, 56, COL_BLACK, 1);
  }
  uint8_t  loc_day = gps_day, loc_mon = gps_month;
  uint16_t loc_yr  = gps_year;
  int loc_mins = (int)disp_h * 60 + (int)disp_m + UTC_OFFSET_H * 60;
  if (loc_mins >= 1440) {
    const uint8_t dim[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (++loc_day > (loc_mon==2 && is_leap(loc_yr) ? 29 : dim[loc_mon-1]))
      { loc_day = 1; if (++loc_mon > 12) { loc_mon = 1; loc_yr++; } }
  } else if (loc_mins < 0) {
    if (--loc_day == 0) {
      if (--loc_mon == 0) { loc_mon = 12; loc_yr--; }
      const uint8_t dim[] = {31,28,31,30,31,30,31,31,30,31,30,31};
      loc_day = (loc_mon==2 && is_leap(loc_yr)) ? 29 : dim[loc_mon-1];
    }
  }
  char datebuf[9];
  snprintf(datebuf, sizeof(datebuf), "%02d/%02d/%02d", loc_day, loc_mon, (uint8_t)(loc_yr % 100));
  mx_panel_center(datebuf, PANEL_MID_X, 56, sc(COL_DIM, bright_center), 1);
  mx_at("PPS", PANEL_MID_X + PANEL_W - 20, 56, sc(pps_ok ? COL_GREEN : COL_RED, bright_center), 1);

  // ═══ PANNEAU DROIT : villes 2 & 3 ══════════════════════════════════════════
  snprintf(lbl, sizeof(lbl), "%s UTC%+d", cities[2].name, cities[2].offset_h);
  mx_panel_center(lbl, PANEL_RIGHT_X,  2, sc(COL_DIM, bright_sides), 1);
  world_time(disp_h, disp_m, cities[2].offset_h, ch, cm);
  snprintf(tbuf, sizeof(tbuf), "%02d:%02d", ch, cm);
  mx_panel_center(tbuf, PANEL_RIGHT_X, 11, sc(matrix->color565(102, 80, 20), bright_sides), 2);
  matrix->drawFastHLine(PANEL_RIGHT_X, 32, PANEL_W, sc(COL_DIM, bright_sides));
  snprintf(lbl, sizeof(lbl), "%s UTC%+d", cities[3].name, cities[3].offset_h);
  mx_panel_center(lbl, PANEL_RIGHT_X, 35, sc(COL_DIM, bright_sides), 1);
  world_time(disp_h, disp_m, cities[3].offset_h, ch, cm);
  snprintf(tbuf, sizeof(tbuf), "%02d:%02d", ch, cm);
  mx_panel_center(tbuf, PANEL_RIGHT_X, 44, sc(matrix->color565(20, 84, 84), bright_sides), 2);

}

void draw_static() {
  matrix->fillScreen(COL_BLACK);
}

void draw_clock(uint8_t loc_h, uint8_t loc_m, uint8_t s, bool colon_on,
                bool gps_ok, bool pps_ok) {
  render_all(loc_h, loc_m, s, colon_on, gps_ok, pps_ok);
}

// ── Affichage warmup – centré sur le panneau milieu ───────────────────────────
void draw_warmup(uint32_t remaining_s) {
  mx_panel_center("GPS NTP SERVER", PANEL_MID_X,  2, COL_DIM,    1);
  mx_panel_center("WARMUP",         PANEL_MID_X, 12, COL_YELLOW, 2);

  char cbuf[8];
  snprintf(cbuf, sizeof(cbuf), "%02d:%02d",
           (uint8_t)(remaining_s / 60), (uint8_t)(remaining_s % 60));
  mx_panel_center(cbuf, PANEL_MID_X, 30, COL_YELLOW, 3);

  if (eth_ready) {
    IPAddress ip = Ethernet.localIP();
    char ipbuf[18];
    snprintf(ipbuf, sizeof(ipbuf), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
    mx_panel_center(ipbuf, PANEL_MID_X, 56, COL_DIM, 1);
  } else {
    mx_panel_center("GPS acquisition...", PANEL_MID_X, 56, COL_DIM, 1);
  }

}

// ── Setup / Loop ─────────────────────────────────────────────────────────────
#define HOSTNAME "clock-esp32-gps-matrix"
byte mac[] = { 0x24, 0x6F, 0x28, 0xAA, 0xBB, 0x01 };

void setup() {
  delay(2000);
  Serial.begin(115200);
  Serial.println("=== Demarrage ===");

  // Restauration offset UTC depuis EEPROM
  EEPROM.begin(EE_SIZE);
  uint8_t stored = EEPROM.read(0);
  if (stored >= 1 && stored <= 14) UTC_OFFSET_H = stored;
  Serial.printf("Offset UTC restaure: %+d\n", UTC_OFFSET_H);
  load_cities();
  uint8_t bc = EEPROM.read(EE_BRIGHT_C);
  uint8_t bs = EEPROM.read(EE_BRIGHT_S);
  if (bc >= 5) bright_center = bc;
  if (bs >= 5) bright_sides  = bs;

  // GPS – RX seulement (pas de TX vers le module)
  gpsSerial.begin(9600, SERIAL_8N1, GPS_RX_PIN, -1);

  pinMode(PPS_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(PPS_PIN), pps_isr, RISING);

  // Matrice LED
  init_matrix();
  Serial.println("Matrice LED initialisee");

  warmup_start = millis();

  SPI.begin(W5500_SCK, W5500_MISO, W5500_MOSI, W5500_CS);
  Ethernet.init(W5500_CS);
  Serial.print("DHCP...");
  if (Ethernet.begin(mac, 10000)) {
    Serial.print(" OK -> ");
    Serial.println(Ethernet.localIP());
  } else {
    Serial.println(" DHCP echoue, IP fixe 192.168.10.10");
    Ethernet.begin(mac, IPAddress(192, 168, 10, 10),
                        IPAddress(8, 8, 8, 8),
                        IPAddress(192, 168, 10, 1),
                        IPAddress(255, 255, 255, 0));
    Serial.print("IP fixe -> ");
    Serial.println(Ethernet.localIP());
  }
  udp.begin(123);
  webServer.begin();
  eth_ready = true;
  Serial.println("NTP port 123 + Web port 80 demarres");
}

void loop() {
  // Lecture NMEA GPS
  while (gpsSerial.available()) {
    char c = gpsSerial.read();
    if (c == '$') nmea_pos = 0;
    if (nmea_pos < sizeof(nmea_buf) - 1) nmea_buf[nmea_pos++] = c;
    if (c == '\n') {
      nmea_buf[nmea_pos] = '\0';
      parse_nmea(nmea_buf);
      nmea_pos = 0;
    }
  }

  // Phase warmup
  if (!warmup_done) {
    uint32_t elapsed   = (millis() - warmup_start) / 1000;
    uint32_t remaining = (elapsed >= WARMUP_SECONDS) ? 0 : WARMUP_SECONDS - elapsed;

    if (elapsed >= WARMUP_SECONDS) {
      warmup_done = true;
      draw_static();  // prépare le fond fixe avant le premier draw_clock
    } else {
      // Rafraîchit l'affichage warmup une fois par seconde
      static uint32_t last_warmup_draw = 0;
      if (millis() - last_warmup_draw >= 1000) {
        last_warmup_draw = millis();
        draw_warmup(remaining);
      }
    }

    if (eth_ready) {
      static uint32_t last_maintain = 0;
      if (millis() - last_maintain >= 5000) { last_maintain = millis(); Ethernet.maintain(); }
      handle_ntp();
      handle_web_client();
    }
    return;
  }

  // Mode normal – mise à jour sur PPS
  if (pps_triggered) {
    pps_triggered   = false;
    last_pps_millis = millis();

    uint32_t unix_ts = date_to_unix(gps_day, gps_month, gps_year,
                                    gps_h, gps_m, gps_s);
    update_ntp_ref(unix_ts);

    uint8_t h = disp_h, m = disp_m;
    apply_timezone(h, m);
    draw_clock(h, m, disp_s, dots_on, gps_valid, true);

    Serial.printf("PPS -> UTC %02d:%02d:%02d | Local %02d:%02d | Sats: %d\n",
                  disp_h, disp_m, disp_s, h, m, gps_satellites);
  }

  // Redraw périodique pour mettre à jour les statuts GPS/PPS si le PPS s'arrête
  static uint32_t last_periodic = 0;
  if (millis() - last_periodic >= 2000) {
    last_periodic = millis();
    bool pps_active = (millis() - last_pps_millis) < 3000;
    uint8_t h = disp_h, m = disp_m;
    apply_timezone(h, m);
    draw_clock(h, m, disp_s, dots_on, gps_valid, pps_active);
  }

  if (eth_ready) {
    static uint32_t last_maintain2 = 0;
    if (millis() - last_maintain2 >= 5000) { last_maintain2 = millis(); Ethernet.maintain(); }
    handle_ntp();
    handle_web_client();
    if (millis() - last_weather_fetch >= 600000UL || last_weather_fetch == 0) {
      last_weather_fetch = millis();
      fetch_weather();
    }
  }
}
