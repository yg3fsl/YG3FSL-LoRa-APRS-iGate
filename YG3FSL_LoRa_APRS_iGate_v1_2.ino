/*
 * YG3FSL LoRa APRS iGate v1.2  (RX-only, WiFi -> APRS-IS Asia)
 *
 * Hardware : ESP32-C3 SuperMini + RA-02 (SX1278)
 * Library  : LoRa by Sandeep Mistry  (WiFi, SPI dari Arduino-ESP32 core)
 * Board    : ESP32C3 Dev Module, USB CDC On Boot = Enabled, Serial 115200
 *
 * Wiring RA-02 (sama dengan tracker YG3FSL-2 v1.2):
 *   VCC->3V3  GND->GND  SCK->GPIO4  MISO->GPIO5  MOSI->GPIO6
 *   NSS->GPIO7  RESET->GPIO10  DIO0->GPIO3
 *   (GPS dan OLED tidak dipakai pada iGate)
 *
 * Fungsi:
 *   - Menerima paket LoRa APRS (432.150 MHz, SF12, BW125, CR4/5, sync 0x12)
 *   - Membuang prefix 3C FF 01, lalu meneruskan ke APRS-IS (asia.aprs2.net:14580)
 *     dengan penambahan ",qAO,<callsign iGate>"
 *   - Mengirim posisi iGate (koordinat manual) ke APRS-IS secara berkala
 *   - Perangkat ini TIDAK memancar (receive-only)
 */

#include <WiFi.h>
#include <SPI.h>
#include <LoRa.h>

// ======================= KONFIGURASI PENGGUNA =======================
#define WIFI_SSID        "NAMA_WIFI_ANDA"
#define WIFI_PASS        "PASSWORD_WIFI_ANDA"   // kosongkan ("") untuk WiFi terbuka tanpa password

#define IGATE_CALL       "YG3FSL-10"     // callsign-SSID iGate (beda dari tracker -2)
#define WIFI_TX_POWER    WIFI_POWER_8_5dBm   // turunkan jika gagal konek (coba 5dBm / 11dBm)
#define APRS_PASSCODE    -1L             // -1 = hitung otomatis dari callsign

#define APRSIS_HOST      "asia.aprs2.net"
#define APRSIS_PORT      14580

// Posisi iGate (isi manual, derajat desimal)
#define IGATE_LAT        (-6.985547669038758)
#define IGATE_LON        (107.5693955311255)

// Simbol: overlay 'L' pada simbol gateway '&'  ->  "L&"
#define SYMBOL_TABLE     'L'
#define SYMBOL_CODE      '&'
#define BEACON_COMMENT   "LoRa iGate 432.150MHz SF12"
#define BEACON_TOCALL    "APZLRG"        // APZ = experimental
#define BEACON_PATH      "TCPIP*"        // path beacon ke APRS-IS
#define BEACON_INTERVAL_MS (15UL * 60UL * 1000UL)   // 15 menit

// ======================= PARAMETER RADIO (sama dengan tracker) ======
#define LORA_SCK         4
#define LORA_MISO        5
#define LORA_MOSI        6
#define LORA_NSS         7
#define LORA_RESET       10
#define LORA_DIO0        3

#define LORA_FREQ        432150000L
#define LORA_BW          125E3
#define LORA_SF          12
#define LORA_CR          5
#define LORA_SYNC_WORD   0x12
#define LORA_PREAMBLE    8

// LED onboard ESP32-C3 SuperMini (GPIO8, aktif LOW) - kedip saat paket di-gate
#define STATUS_LED       8
#define LED_ACTIVE_LOW   true

#define SW_NAME          "ESP32C3-LoRa-iGate"
#define SW_VERS          "1.2"

// ======================= VARIABEL GLOBAL ============================
WiFiClient aprs;

static long     passcode        = 0;
static char     srvBuf[300];
static size_t   srvLen          = 0;

static uint32_t lastServerRx    = 0;
static uint32_t lastKeepalive   = 0;
static uint32_t lastWifiTry     = 0;
static uint32_t lastAprsTry     = 0;
static uint32_t lastStats       = 0;
static uint32_t nextBeaconAt    = 0;
static uint32_t ledOffAt        = 0;

static uint32_t cntRx = 0, cntGated = 0, cntDropped = 0;

// ======================= UTILITAS ===================================
long computePasscode(const char *call) {
  char base[10];
  int n = 0;
  while (call[n] && call[n] != '-' && n < 9) {
    base[n] = toupper((unsigned char)call[n]);
    n++;
  }
  base[n] = 0;
  uint16_t hash = 0x73e2;
  for (int i = 0; i < n; i += 2) {
    hash ^= (uint16_t)base[i] << 8;
    if (i + 1 < n) hash ^= (uint8_t)base[i + 1];
  }
  return hash & 0x7fff;
}

void ledBlink(uint16_t ms = 80) {
  digitalWrite(STATUS_LED, LED_ACTIVE_LOW ? LOW : HIGH);
  ledOffAt = millis() + ms;
}

void ledService() {
  if (ledOffAt && (int32_t)(millis() - ledOffAt) >= 0) {
    digitalWrite(STATUS_LED, LED_ACTIVE_LOW ? HIGH : LOW);
    ledOffAt = 0;
  }
}

// Format posisi APRS: lat "DDMM.mmN" , lon "DDDMM.mmE"
void fmtLat(double lat, char *out, size_t sz) {
  char h = (lat >= 0) ? 'N' : 'S';
  long t = lround(fabs(lat) * 6000.0);          // satuan 1/100 menit
  snprintf(out, sz, "%02ld%02ld.%02ld%c", t / 6000, (t % 6000) / 100, t % 100, h);
}

void fmtLon(double lon, char *out, size_t sz) {
  char h = (lon >= 0) ? 'E' : 'W';
  long t = lround(fabs(lon) * 6000.0);
  snprintf(out, sz, "%03ld%02ld.%02ld%c", t / 6000, (t % 6000) / 100, t % 100, h);
}

// ======================= WIFI =======================================
void wifiBegin() {
  if (strlen(WIFI_PASS) == 0) WiFi.begin(WIFI_SSID);
  else                        WiFi.begin(WIFI_SSID, WIFI_PASS);
  WiFi.setTxPower(WIFI_TX_POWER);   // harus setelah begin(); mengatasi masalah SuperMini
}

const char *wifiStatusText(wl_status_t st) {
  switch (st) {
    case WL_NO_SSID_AVAIL:   return "SSID tidak ditemukan (cek 2.4GHz / nama / jarak)";
    case WL_CONNECT_FAILED:  return "Gagal konek (password / tipe keamanan salah)";
    case WL_CONNECTION_LOST: return "Koneksi hilang";
    case WL_DISCONNECTED:    return "Disconnected (handshake gagal / sinyal / TX power)";
    case WL_IDLE_STATUS:     return "Idle";
    case WL_CONNECTED:       return "Terhubung";
    default:                 return "Tidak diketahui";
  }
}

void scanWiFi() {
  Serial.println("[WiFi] Scan jaringan 2.4GHz di sekitar...");
  int n = WiFi.scanNetworks();
  if (n <= 0) {
    Serial.println("[WiFi] Tidak ada jaringan terdeteksi (antena/jarak?)");
    return;
  }
  bool found = false;
  for (int i = 0; i < n; i++) {
    bool match = (WiFi.SSID(i) == WIFI_SSID);
    if (match) found = true;
    Serial.printf("  %2d. %-32s ch%-2d %4d dBm %s%s\n", i + 1,
                  WiFi.SSID(i).c_str(), WiFi.channel(i), WiFi.RSSI(i),
                  WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "OPEN" : "SECURED",
                  match ? "  <== target" : "");
  }
  if (!found) Serial.printf("[WiFi] SSID '%s' TIDAK ada di hasil scan\n", WIFI_SSID);
  WiFi.scanDelete();
}

void startWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  scanWiFi();
  wifiBegin();
  lastWifiTry = millis();
}

void maintainWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWifiTry > 15000UL) {
    wl_status_t st = WiFi.status();
    Serial.printf("[WiFi] status=%d : %s\n", (int)st, wifiStatusText(st));
    Serial.println("[WiFi] Mencoba konek ulang...");
    WiFi.disconnect();
    wifiBegin();
    lastWifiTry = millis();
  }
}

// ======================= APRS-IS ====================================
bool connectAPRSIS() {
  Serial.printf("[APRS-IS] Menghubungkan ke %s:%d ...\n", APRSIS_HOST, APRSIS_PORT);
  if (!aprs.connect(APRSIS_HOST, APRSIS_PORT, 5000)) {
    Serial.println("[APRS-IS] Gagal konek");
    return false;
  }
  aprs.setNoDelay(true);
  srvLen = 0;
  aprs.printf("user %s pass %ld vers %s %s\r\n", IGATE_CALL, passcode, SW_NAME, SW_VERS);
  lastServerRx  = millis();
  lastKeepalive = millis();
  nextBeaconAt  = millis() + 2500UL;   // beacon pertama sesaat setelah login
  Serial.println("[APRS-IS] Terhubung, login dikirim");
  return true;
}

void maintainAPRSIS() {
  if (WiFi.status() != WL_CONNECTED) {
    if (aprs.connected()) aprs.stop();
    return;
  }
  if (aprs.connected()) {
    // server mengirim komentar tiap ~20 detik; diam >90 detik = anggap mati
    if (millis() - lastServerRx > 90000UL) {
      Serial.println("[APRS-IS] Timeout, koneksi diputus");
      aprs.stop();
    }
    return;
  }
  if (millis() - lastAprsTry > 10000UL) {
    lastAprsTry = millis();
    connectAPRSIS();
  }
}

void handleServerLine(const char *line) {
  Serial.printf("[APRS-IS] < %s\n", line);
  if (strstr(line, "logresp")) {
    if (strstr(line, "unverified")) {
      Serial.println("[APRS-IS] PERINGATAN: login UNVERIFIED (cek callsign/passcode)");
    } else if (strstr(line, "verified")) {
      Serial.println("[APRS-IS] Login verified");
    }
  }
}

void pollAPRSIS() {
  while (aprs.connected() && aprs.available()) {
    char c = aprs.read();
    lastServerRx = millis();
    if (c == '\n') {
      srvBuf[srvLen] = 0;
      if (srvLen) handleServerLine(srvBuf);
      srvLen = 0;
    } else if (c != '\r' && srvLen < sizeof(srvBuf) - 1) {
      srvBuf[srvLen++] = c;
    }
  }
}

void sendKeepalive() {
  if (aprs.connected() && millis() - lastKeepalive > 60000UL) {
    aprs.print("#keepalive\r\n");
    lastKeepalive = millis();
  }
}

void sendBeacon() {
  char lat[12], lon[12], line[200];
  fmtLat(IGATE_LAT, lat, sizeof(lat));
  fmtLon(IGATE_LON, lon, sizeof(lon));
  snprintf(line, sizeof(line), "%s>%s,%s:!%s%c%s%c%s",
           IGATE_CALL, BEACON_TOCALL, BEACON_PATH,
           lat, SYMBOL_TABLE, lon, SYMBOL_CODE, BEACON_COMMENT);
  aprs.print(line);
  aprs.print("\r\n");
  Serial.printf("[BEACON] > %s\n", line);
}

void beaconService() {
  if (aprs.connected() && (int32_t)(millis() - nextBeaconAt) >= 0) {
    sendBeacon();
    nextBeaconAt = millis() + BEACON_INTERVAL_MS;
  }
}

// ======================= LORA RX -> APRS-IS =========================
void handleRF(const uint8_t *b, int n, float rssi, float snr) {
  cntRx++;

  // buang prefix LoRa APRS 3C FF 01
  int s = 0;
  if (n >= 3 && b[0] == 0x3C && b[1] == 0xFF && b[2] == 0x01) s = 3;

  int len = n - s;
  while (len > 0 && (b[s + len - 1] == '\r' || b[s + len - 1] == '\n' || b[s + len - 1] == 0)) len--;
  if (len <= 0 || len > 250) { cntDropped++; return; }

  char pkt[256];
  memcpy(pkt, b + s, len);
  pkt[len] = 0;

  // CR/LF/NUL di tengah paket tidak boleh masuk APRS-IS
  for (int i = 0; i < len; i++) {
    if (pkt[i] == '\r' || pkt[i] == '\n') {
      Serial.println("[RF] Dibuang: berisi CR/LF");
      cntDropped++;
      return;
    }
  }

  Serial.printf("[RF] RSSI=%.0f SNR=%.1f  %s\n", rssi, snr, pkt);

  char *colon = strchr(pkt, ':');
  char *gt    = strchr(pkt, '>');
  if (!colon || !gt || gt > colon || colon == pkt || *(colon + 1) == 0 || (gt - pkt) > 9) {
    Serial.println("[RF] Dibuang: format TNC2 tidak valid");
    cntDropped++;
    return;
  }

  // aturan gating: jangan teruskan paket yang sudah dari internet / NOGATE
  int hdrLen = colon - pkt;
  char hdr[200];
  if (hdrLen >= (int)sizeof(hdr)) { cntDropped++; return; }
  for (int i = 0; i < hdrLen; i++) hdr[i] = toupper((unsigned char)pkt[i]);
  hdr[hdrLen] = 0;
  if (strstr(hdr, "TCPIP") || strstr(hdr, "TCPXX") ||
      strstr(hdr, "NOGATE") || strstr(hdr, "RFONLY") || strstr(hdr, ",QA")) {
    Serial.println("[RF] Dibuang: path melarang gating");
    cntDropped++;
    return;
  }

  const char *payload = colon + 1;
  if (payload[0] == '}') {            // third-party: tidak digate
    Serial.println("[RF] Dibuang: third-party");
    cntDropped++;
    return;
  }

  if (!aprs.connected()) {
    Serial.println("[RF] APRS-IS belum terhubung, paket tidak diteruskan");
    cntDropped++;
    return;
  }

  char out[420];
  snprintf(out, sizeof(out), "%.*s,qAO,%s:%s", hdrLen, pkt, IGATE_CALL, payload);
  aprs.print(out);
  aprs.print("\r\n");
  cntGated++;
  ledBlink();
  Serial.printf("[GATE] > %s\n", out);
}

void pollLoRa() {
  int size = LoRa.parsePacket();
  if (size <= 0) return;

  uint8_t buf[256];
  int n = 0;
  while (LoRa.available() && n < (int)sizeof(buf) - 1) buf[n++] = (uint8_t)LoRa.read();
  buf[n] = 0;
  handleRF(buf, n, LoRa.packetRssi(), LoRa.packetSnr());
}

// ======================= SETUP / LOOP ===============================
void statsService() {
  if (millis() - lastStats < 60000UL) return;
  lastStats = millis();
  Serial.printf("[STAT] WiFi=%s APRS-IS=%s RX=%lu Gated=%lu Drop=%lu\n",
                WiFi.status() == WL_CONNECTED ? "OK" : "DOWN",
                aprs.connected() ? "OK" : "DOWN",
                (unsigned long)cntRx, (unsigned long)cntGated, (unsigned long)cntDropped);
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);
  Serial.println();
  Serial.println("=== YG3FSL LoRa APRS iGate v1.2 ===");

  pinMode(STATUS_LED, OUTPUT);
  digitalWrite(STATUS_LED, LED_ACTIVE_LOW ? HIGH : LOW);

  passcode = (APRS_PASSCODE < 0) ? computePasscode(IGATE_CALL) : APRS_PASSCODE;
  Serial.printf("Callsign : %s\n", IGATE_CALL);
  Serial.printf("Server   : %s:%d\n", APRSIS_HOST, APRSIS_PORT);

  // --- Radio ---
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);
  LoRa.setSPI(SPI);
  LoRa.setPins(LORA_NSS, LORA_RESET, LORA_DIO0);
  LoRa.setSPIFrequency(1000000);

  if (!LoRa.begin(LORA_FREQ)) {
    Serial.println("LoRa init GAGAL - cek wiring RA-02. Restart dalam 5 detik...");
    delay(5000);
    ESP.restart();
  }
  LoRa.setSignalBandwidth(LORA_BW);
  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setCodingRate4(LORA_CR);
  LoRa.setSyncWord(LORA_SYNC_WORD);
  LoRa.setPreambleLength(LORA_PREAMBLE);
  LoRa.enableCrc();
  Serial.println("LoRa OK (432.150 MHz, SF12, BW125, CR4/5, sync 0x12)");

  // --- WiFi ---
  startWiFi();
  Serial.printf("Menghubungkan WiFi '%s' ", WIFI_SSID);
  t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000UL) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi OK, IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.printf("WiFi belum terhubung, status=%d : %s\n", (int)WiFi.status(), wifiStatusText(WiFi.status()));
    Serial.println("Akan dicoba ulang otomatis");
  }
}

void loop() {
  pollLoRa();            // prioritas utama: jangan lewatkan paket RF
  maintainWiFi();
  maintainAPRSIS();
  pollAPRSIS();
  sendKeepalive();
  beaconService();
  statsService();
  ledService();
}
