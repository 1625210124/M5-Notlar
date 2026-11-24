// ============================================================
//  M5StickS3  |  Notlar + Gemini asistan
//  - Ilk acilista / sorun olursa AP (captive portal) ile WiFi kurulumu
//  - WiFi'ye otomatik baglanma, baglaninca http://m5notes.local (veya IP)
//  - Web'den not ekle / sil, cihazdan not oku / yaz
//  - Gemini: Sesli (A basili tut) veya Klavye (4x4 keypad ile)
//  - Gemini API anahtari ve model adi kodda YOK: AP kurulum sayfasindan
//    veya http://m5notes.local sayfasindaki "Gemini Ayarlari"ndan girilir
//
//  Kutuphaneler: M5Unified, M5GFX (guncel), ArduinoJson 7.x
//  Kart: M5StickS3 (yoksa ESP32S3 Dev Module) | PSRAM: OPI PSRAM
//        USB CDC On Boot: Enabled | Partition: 8MB (spiffs/LittleFS olan)
// ============================================================
#include <M5Unified.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <mbedtls/base64.h>
#include <vector>
#include <algorithm>
#include <time.h>

// ---------------- AYARLAR ----------------
#define AP_SSID          "M5Notes-Setup"
#define AP_PASS          ""              // bos = acik AP (captive portal icin kolay)
#define HOSTNAME         "m5notes"       // http://m5notes.local

#define SCREEN_ROTATION  0               // ilk acilis yonu (0 dikey, 1 yatay, 2 dikey ters, 3 yatay ters). Sonra Ayarlar'dan degisir
#define KEYPAD_SWAP      0               // S2 yerine S5 (S3 yerine S9...) gibi cikiyorsa 1 yap

#define REC_RATE         16000
#define MAX_REC_SEC      15
#define STA_LOST_AP_MS   45000           // WiFi bu kadar sure kopuk kalirsa AP'ye gec

#define C_HDR  0x18E3
#define C_SEL  0x041F

// ---------------- TIPLER ----------------
// Arduino otomatik prototip uretiyor; bu tipler ilk fonksiyondan ONCE tanimli olmali.
struct Line     { String t; uint16_t c; };
struct NoteMeta { uint32_t id; String date; String title; };
struct Turn     { String role; String text; };
struct Input    { bool sel, aPress, next, back, physBHold; int key; };
struct WifiNet  { String ssid; int rssi; bool enc; };

uint8_t scrRot = 0;                      // ekran yonu (Ayarlar'dan)
uint8_t keyRot = 0;                      // keypad yonu: 0, 90, 180, 270 derece saat yonu (Ayarlar'dan)

// ---------------- KEYPAD ----------------
// Keypad header (soldan saga): R4 R3 R2 R1 C1 C2 C3 C4  ->  G5 G4 G6 G7 G43 G44 G2 G3
// S1..S4 = R1 satiri (C1..C4), S5..S8 = R2, S9..S12 = R3, S13..S16 = R4
const uint8_t ROW_PINS[4] = {7, 6, 4, 5};      // R1, R2, R3, R4
const uint8_t COL_PINS[4] = {43, 44, 2, 3};    // C1, C2, C3, C4

int keyStable = 0;                       // debounce edilmis basili tus (S13 = A icin)

void keypadInit() {
  for (int i = 0; i < 4; i++) { pinMode(ROW_PINS[i], OUTPUT); digitalWrite(ROW_PINS[i], HIGH); }
  for (int i = 0; i < 4; i++) pinMode(COL_PINS[i], INPUT_PULLUP);
}

int mapKey(int r, int c) {                // tarama (satir, sutun) -> mantiksal tus no (1..16)
  int rr = r, cc = c;
#if KEYPAD_SWAP
  rr = c; cc = r;
#endif
  int r2 = rr, c2 = cc;                  // keypad saat yonunde cevrildiyse tuslari yeniden esle
  switch (keyRot) {
    case 1: r2 = cc;     c2 = 3 - rr; break;   //  90
    case 2: r2 = 3 - rr; c2 = 3 - cc; break;   // 180
    case 3: r2 = 3 - cc; c2 = rr;     break;   // 270
  }
  return r2 * 4 + c2 + 1;
}

int keypadScan() {                       // 1..16 (S1..S16), 0 = yok
  for (int r = 0; r < 4; r++) {
    digitalWrite(ROW_PINS[r], LOW);
    delayMicroseconds(40);
    for (int c = 0; c < 4; c++) {
      if (digitalRead(COL_PINS[c]) == LOW) {
        digitalWrite(ROW_PINS[r], HIGH);
        return mapKey(r, c);
      }
    }
    digitalWrite(ROW_PINS[r], HIGH);
  }
  return 0;
}

uint16_t keypadMask() {                  // ayni anda basili TUM tuslar (bit n-1 = Sn)
  uint16_t mk = 0;
  for (int r = 0; r < 4; r++) {
    digitalWrite(ROW_PINS[r], LOW);
    delayMicroseconds(40);
    for (int c = 0; c < 4; c++)
      if (digitalRead(COL_PINS[c]) == LOW) mk |= (uint16_t)1 << (mapKey(r, c) - 1);
    digitalWrite(ROW_PINS[r], HIGH);
  }
  return mk;
}

// Mantiksal tus (islev numarasi, 1..16) keypad dondurulunce hangi YAZILI tusa denk geliyor
int printedOf(int logical) {
  int r2 = (logical - 1) / 4, c2 = (logical - 1) % 4;
  int rr = r2, cc = c2;
  switch (keyRot & 3) {
    case 1: rr = 3 - c2; cc = r2;     break;
    case 2: rr = 3 - r2; cc = 3 - c2; break;
    case 3: rr = c2;     cc = 3 - r2; break;
  }
  return rr * 4 + cc + 1;
}
String keyName(int logical) { return "S" + String(printedOf(logical)); }

// Debounce + tekrar (basili tutunca). Basilma aninda tus no doner, yoksa 0.
int keypadEvent() {
  static int raw = 0, stable = 0;
  static uint32_t rawT = 0, repAt = 0;
  uint32_t now = millis();
  int k = keypadScan();
  if (k != raw) { raw = k; rawT = now; }
  if (now - rawT < 15) return 0;
  keyStable = raw;
  if (raw != stable) {
    stable = raw;
    if (stable) { repAt = now + 380; return stable; }
    return 0;
  }
  if (stable && now >= repAt) {
    bool rep = (stable == 1 || stable == 2 || stable == 3 || stable == 5 || stable == 6 || stable == 7);
    if (rep) { repAt = now + 90; return stable; }
  }
  return 0;
}

bool aHeld() {                           // A butonu VEYA keypad S13 basili mi
  keypadEvent();                         // tus durumunu guncelle
  return M5.BtnA.isPressed() || keyStable == 13;
}

// ---------------- GLOBAL ----------------
WebServer server(80);
DNSServer dns;
Preferences prefs;

bool apMode = false;
enum ConnState { CS_IDLE, CS_CONNECTING, CS_OK, CS_FAIL };
ConnState connState = CS_IDLE;
uint32_t connStart = 0, restartAt = 0, staLostSince = 0;
String pendSsid, pendPass, scanJson = "[]";

std::vector<Line> lines;
int scrollPos = 0;
int SW = 240, SH = 135, WRAPW = 228, VIS = 7;   // ekran olculeri (setup'ta ayarlanir)
std::vector<NoteMeta> notes;
int notesIdx = 0;
bool notesDirty = false;
uint32_t viewNoteId = 0, delArm = 0;
std::vector<Turn> hist;

enum Screen { SC_MENU, SC_NOTES, SC_NOTE_VIEW, SC_GEM_MENU, SC_CHAT, SC_KB, SC_NET, SC_SETTINGS, SC_KEYS, SC_GAME, SC_WIFI };
Screen screen = SC_MENU;
bool dirty = true;
int menuIdx = 0, gemIdx = 0, setIdx = 0;
uint8_t brightness = 120;
M5Canvas canvas(&M5.Display);          // cift tampon: ekran titremesin (blink olmasin)
String curTitle;
uint32_t lastHdr = 0;

enum KbPurpose { KB_GEMINI, KB_NOTE, KB_WIFI };
KbPurpose kbPurpose = KB_GEMINI;
String wifiSel;                          // sifresi girilen ag
std::vector<WifiNet> wifiNets;           // cihazdan taranan aglar
int wifiIdx = 0;
String kbText;
int kbR = 1, kbC = 0, kbLayer = 0;
bool chatVoice = false;
uint8_t* recBuf = nullptr;

const char* KB[3][4] = {
  {"1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm,.?"},
  {"1234567890", "QWERTYUIOP", "ASDFGHJKL\"", "ZXCVBNM;:!"},
  {"!@#$%^&*()", "-_=+[]{}|~", "<>/\\`;:'\"@", "?,.!-_()+*"}
};

const char* MAIN_ITEMS[] = {"Notlar", "Yeni Not", "Gemini", "Piyano", "Ayarlar"};
const uint8_t BR_LEVELS[] = {30, 70, 120, 180, 255};
const char* GEM_ITEMS[]  = {"Sesli", "Klavye", "Sohbeti Sifirla"};
bool gemSearch = true;                   // Gemini internet (Google Arama) kullansin
String gemKey, gemModel;                 // Gemini API anahtari ve model (web'den girilir, NVS'te saklanir)

// ---------------- WEB SAYFALARI ----------------
const char NOTES_HTML[] PROGMEM = R"rawliteral(<!doctype html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>M5 Notlar</title>
<style>body{font-family:system-ui,sans-serif;background:#111;color:#eee;max-width:560px;margin:0 auto;padding:16px}
textarea{width:100%;box-sizing:border-box;height:110px;font-size:16px;padding:10px;border-radius:8px;border:0;background:#222;color:#eee}
input{width:100%;box-sizing:border-box;font-size:16px;padding:10px;border-radius:8px;border:0;background:#222;color:#eee;margin:6px 0}
details{background:#1d1d1d;border-radius:10px;padding:12px;margin:0 0 14px}summary{cursor:pointer;font-weight:600}
button{font-size:16px;padding:10px 16px;border-radius:8px;border:0;background:#2d7ff9;color:#fff;margin:6px 0}
.n{background:#1d1d1d;border-radius:10px;padding:12px;margin:10px 0}.n p{white-space:pre-wrap;margin:0 0 8px;word-break:break-word}
.n small{color:#888;display:block}.n button{background:#c0392b;padding:6px 12px;margin:8px 0 0;font-size:14px}</style></head><body>
<h2>📝 M5 Notlar</h2>
<details id=gd><summary>✨ Gemini Ayarları</summary>
<input id=gm placeholder="Model adı">
<input id=gk type=password placeholder="API anahtarı">
<button onclick=gemSave()>Gemini'yi kaydet</button>
<div id=gmsg style="color:#fc6"></div>
</details>
<textarea id=t placeholder="Yeni not..." maxlength=1500></textarea>
<button onclick=add()>Kaydet</button>
<div id=l></div>
<script>
const $=i=>document.getElementById(i);
async function load(){const r=await fetch('/api/notes');const a=await r.json();const l=$('l');l.innerHTML='';
a.forEach(n=>{const d=document.createElement('div');d.className='n';const p=document.createElement('p');p.textContent=n.t;
const m=document.createElement('small');m.textContent=n.d||'';const b=document.createElement('button');b.textContent='Sil';
b.onclick=()=>del(n.id);d.append(p,m,b);l.append(d)});if(!a.length)l.textContent='Henüz not yok.'}
async function add(){const t=$('t');if(!t.value.trim())return;
const r=await fetch('/api/add',{method:'POST',headers:{'Content-Type':'text/plain'},body:t.value});if(!r.ok){alert('Kaydedilemedi: '+await r.text());return}t.value='';load()}
async function del(id){if(!confirm('Silinsin mi?'))return;await fetch('/api/del?id='+id,{method:'POST'});load()}
async function gemLoad(){try{const r=await fetch('/api/gem');const j=await r.json();$('gm').value=j.m||'';
$('gk').placeholder=j.k?('Kayıtlı: '+j.h+' (değiştirmek için yaz)'):'API anahtarı';if(!j.k||!j.m)$('gd').open=true}catch(e){}}
async function gemSave(){try{const r=await fetch('/api/gem',{method:'POST',body:new URLSearchParams({model:$('gm').value,key:$('gk').value})});
$('gmsg').textContent=r.ok?'✅ Kaydedildi':'❌ Kaydedilemedi';$('gk').value='';gemLoad()}catch(e){$('gmsg').textContent='❌ Hata'}}
load();gemLoad();
</script></body></html>)rawliteral";

const char PORTAL_HTML[] PROGMEM = R"rawliteral(<!doctype html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>M5 WiFi Kurulum</title>
<style>body{font-family:system-ui,sans-serif;background:#111;color:#eee;max-width:480px;margin:0 auto;padding:16px}
button,input{font-size:16px;padding:10px;border-radius:8px;border:0;width:100%;box-sizing:border-box;margin:6px 0}
input{background:#222;color:#eee}button{background:#2d7ff9;color:#fff}
.n{background:#222;padding:10px;border-radius:8px;margin:6px 0;cursor:pointer;display:flex;justify-content:space-between}
#m{margin-top:10px;color:#fc6}</style></head><body>
<h2>📶 M5 WiFi Kurulum</h2>
<button onclick=scan(1)>Ağları yenile</button>
<div id=l>Taranıyor...</div>
<input id=s placeholder="Ağ adı (SSID)">
<input id=p type=password placeholder="Şifre">
<button onclick=go()>Bağlan</button>
<div id=m></div>
<hr style="border:0;border-top:1px solid #333;margin:18px 0">
<h3>✨ Gemini Ayarları</h3>
<input id=gm placeholder="Model adı">
<input id=gk type=password placeholder="API anahtarı">
<button onclick=gemSave()>Gemini'yi kaydet</button>
<div id=gmsg style="color:#fc6"></div>
<script>
const $=i=>document.getElementById(i);
async function scan(re){$('l').textContent='Taranıyor...';
try{const r=await fetch(re?'/rescan':'/scan');const a=await r.json();a.sort((x,y)=>y.r-x.r);$('l').innerHTML='';
a.forEach(n=>{const d=document.createElement('div');d.className='n';const x=document.createElement('span');x.textContent=n.s;
const y=document.createElement('span');y.textContent=(n.e?'🔒 ':'')+n.r+' dBm';d.append(x,y);
d.onclick=()=>{$('s').value=n.s;$('p').focus()};$('l').append(d)});if(!a.length)$('l').textContent='Ağ bulunamadı'}
catch(e){$('l').textContent='Tarama hatası'}}
async function go(){const s=$('s').value;if(!s)return;$('m').textContent='Bağlanıyor...';
try{await fetch('/connect',{method:'POST',body:new URLSearchParams({ssid:s,pass:$('p').value})})}catch(e){}poll()}
async function poll(){try{const r=await fetch('/status');const j=await r.json();
if(j.s=='ok'){$('m').textContent='✅ Bağlandı! IP: '+j.ip+' — cihaz yeniden başlıyor. Telefonu normal Wi-Fi ağına geçirip http://m5notes.local veya http://'+j.ip+' adresini aç.';return}
if(j.s=='fail'){$('m').textContent='❌ Bağlanamadı, şifreyi kontrol et.';return}}catch(e){}setTimeout(poll,1500)}
async function gemLoad(){try{const r=await fetch('/api/gem');const j=await r.json();$('gm').value=j.m||'';
$('gk').placeholder=j.k?('Kayıtlı: '+j.h+' (değiştirmek için yaz)'):'API anahtarı'}catch(e){}}
async function gemSave(){try{const r=await fetch('/api/gem',{method:'POST',body:new URLSearchParams({model:$('gm').value,key:$('gk').value})});
$('gmsg').textContent=r.ok?'✅ Kaydedildi':'❌ Kaydedilemedi';$('gk').value='';gemLoad()}catch(e){$('gmsg').textContent='❌ Hata'}}
scan(0);gemLoad();
</script></body></html>)rawliteral";

// ---------------- YARDIMCI ----------------
String fold(const String& s) {           // Turkce/ozel karakterleri ekran fontu icin ASCII'ye cevir
  String o; o.reserve(s.length());
  size_t n = s.length();
  for (size_t i = 0; i < n;) {
    uint8_t c = s[i];
    if (c < 0x80) { o += (char)c; i++; continue; }
    uint8_t d = (i + 1 < n) ? (uint8_t)s[i + 1] : 0;
    if (c == 0xC3) {
      switch (d) {
        case 0xA7: o += 'c'; break; case 0x87: o += 'C'; break;
        case 0xB6: o += 'o'; break; case 0x96: o += 'O'; break;
        case 0xBC: o += 'u'; break; case 0x9C: o += 'U'; break;
        default: o += '?';
      }
      i += 2;
    } else if (c == 0xC4) {
      switch (d) {
        case 0x9F: o += 'g'; break; case 0x9E: o += 'G'; break;
        case 0xB1: o += 'i'; break; case 0xB0: o += 'I'; break;
        default: o += '?';
      }
      i += 2;
    } else if (c == 0xC5) {
      switch (d) {
        case 0x9F: o += 's'; break; case 0x9E: o += 'S'; break;
        default: o += '?';
      }
      i += 2;
    } else if (c == 0xE2 && i + 2 < n) {
      uint8_t e = (uint8_t)s[i + 2];
      if (d == 0x80) {
        if (e == 0x98 || e == 0x99) o += '\'';
        else if (e == 0x9C || e == 0x9D) o += '"';
        else if (e == 0x93 || e == 0x94) o += '-';
        else if (e == 0xA6) o += "...";
        else if (e == 0xA2) o += '*';
        else o += '?';
      } else o += '?';
      i += 3;
    } else {
      int len = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : 2;
      i += len;                          // emoji vb. atla
    }
  }
  return o;
}

String fitText(String s, int w) {
  auto& d = canvas;
  d.setFont(&fonts::Font2);
  s = fold(s);
  while (s.length() > 1 && d.textWidth(s.c_str()) > w) s.remove(s.length() - 1);
  return s;
}

void wrapText(const String& raw, uint16_t col, int maxW, std::vector<Line>& out) {
  auto& d = canvas;
  d.setFont(&fonts::Font2);
  String s = fold(raw), line, word;
  auto flush = [&]() { out.push_back({line, col}); line = ""; };
  auto addWord = [&](String w) {
    while (d.textWidth(w.c_str()) > maxW && w.length() > 1) {
      int k = w.length() - 1;
      while (k > 1 && d.textWidth(w.substring(0, k).c_str()) > maxW) k--;
      if (line.length()) flush();
      out.push_back({w.substring(0, k), col});
      w = w.substring(k);
    }
    String t = line.length() ? line + " " + w : w;
    if (d.textWidth(t.c_str()) <= maxW) line = t;
    else { flush(); line = w; }
  };
  for (size_t i = 0; i <= s.length(); i++) {
    char ch = (i < s.length()) ? s[i] : '\n';
    if (ch == '\n') { if (word.length()) addWord(word); word = ""; flush(); }
    else if (ch == ' ') { if (word.length()) addWord(word); word = ""; }
    else if (ch == '\r') {}
    else word += ch;
  }
}

bool netOnline() { return !apMode && WiFi.status() == WL_CONNECTED; }

String nowStr() {
  struct tm t;
  if (!getLocalTime(&t, 0)) return "";
  char b[24];
  strftime(b, sizeof b, "%d.%m.%Y %H:%M", &t);
  return String(b);
}

// ---------------- NOTLAR (LittleFS; yoksa NVS yedegi) ----------------
bool fsOk = false;
String fsErr;

String notePath(uint32_t id) { return "/n" + String(id) + ".txt"; }

void splitNote(const String& raw, String& date, String& text) {
  int p = raw.indexOf('\n');
  if (p < 0) { date = ""; text = raw; }
  else { date = raw.substring(0, p); text = raw.substring(p + 1); }
}

void nvsIds(std::vector<uint32_t>& ids) {
  ids.clear();
  prefs.begin("notes", true);
  String s = prefs.getString("ids", "");
  prefs.end();
  int st = 0;
  while (st < (int)s.length()) {
    int c = s.indexOf(',', st);
    if (c < 0) c = s.length();
    uint32_t id = s.substring(st, c).toInt();
    if (id > 0) ids.push_back(id);
    st = c + 1;
  }
}

void nvsSaveIds(const std::vector<uint32_t>& ids) {
  String s;
  for (uint32_t id : ids) { s += String(id); s += ','; }
  prefs.begin("notes", false);
  prefs.putString("ids", s);
  prefs.end();
}

bool readNote(uint32_t id, String& date, String& text) {
  if (fsOk) {
    File f = LittleFS.open(notePath(id), "r");
    if (!f) return false;
    date = f.readStringUntil('\n');
    text = f.readString();
    f.close();
    return true;
  }
  prefs.begin("notes", true);
  String raw = prefs.getString(("n" + String(id)).c_str(), "\x01");
  prefs.end();
  if (raw == "\x01") return false;
  splitNote(raw, date, text);
  return true;
}

void listNotes(std::vector<NoteMeta>& v) {
  v.clear();
  if (!fsOk) {                                     // NVS yedek modu
    std::vector<uint32_t> ids;
    nvsIds(ids);
    std::sort(ids.begin(), ids.end(), [](uint32_t a, uint32_t b) { return a > b; });
    for (uint32_t id : ids) {
      String d, t;
      if (!readNote(id, d, t)) continue;
      NoteMeta m;
      m.id = id;
      m.date = d;
      int nl = t.indexOf('\n');
      m.title = (nl < 0) ? t : t.substring(0, nl);
      v.push_back(m);
    }
    return;
  }
  File dir = LittleFS.open("/");
  if (!dir || !dir.isDirectory()) return;
  File f = dir.openNextFile();
  while (f) {
    String nm = f.name();
    int p = nm.lastIndexOf('/');
    if (p >= 0) nm = nm.substring(p + 1);
    if (!f.isDirectory() && nm.length() > 5 && nm[0] == 'n' && nm.endsWith(".txt")) {
      uint32_t id = nm.substring(1).toInt();
      if (id > 0) {
        NoteMeta m; m.id = id;
        m.date = f.readStringUntil('\n');
        m.title = f.readStringUntil('\n');
        v.push_back(m);
      }
    }
    f.close();
    f = dir.openNextFile();
  }
  std::sort(v.begin(), v.end(), [](const NoteMeta& a, const NoteMeta& b) { return a.id > b.id; });
}

uint32_t addNote(const String& text) {
  fsErr = "";
  String t = text; t.trim();
  if (!t.length()) { fsErr = "Not bos"; return 0; }
  String body = nowStr() + "\n";
  if (fsOk) {
    if (t.length() > 1500) t = t.substring(0, 1500);
    body += t;
    std::vector<NoteMeta> v;
    listNotes(v);
    uint32_t id = v.empty() ? 1 : v[0].id + 1;     // liste buyukten kucuge sirali
    File f = LittleFS.open(notePath(id), "w");
    if (!f) { fsErr = "Dosya acilamadi"; return 0; }
    size_t w = f.print(body);
    f.close();
    if (w != body.length()) { LittleFS.remove(notePath(id)); fsErr = "Yazma hatasi (dolu?)"; return 0; }
    notesDirty = true;
    return id;
  }
  // dosya sistemi yok: kucuk hafizaya (NVS) kaydet
  if (t.length() > 800) t = t.substring(0, 800);
  body += t;
  std::vector<uint32_t> ids;
  nvsIds(ids);
  uint32_t id = 1;
  for (uint32_t x : ids) if (x >= id) id = x + 1;
  prefs.begin("notes", false);
  size_t w = prefs.putString(("n" + String(id)).c_str(), body);
  prefs.end();
  if (!w) { fsErr = "Hafiza dolu"; return 0; }
  ids.push_back(id);
  nvsSaveIds(ids);
  notesDirty = true;
  return id;
}

bool delNote(uint32_t id) {
  bool ok;
  if (fsOk) {
    ok = LittleFS.remove(notePath(id));
  } else {
    prefs.begin("notes", false);
    ok = prefs.remove(("n" + String(id)).c_str());
    prefs.end();
    std::vector<uint32_t> ids;
    nvsIds(ids);
    ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
    nvsSaveIds(ids);
  }
  notesDirty = true;
  return ok;
}

void loadNotes() {
  listNotes(notes);
  if (notesIdx >= (int)notes.size()) notesIdx = notes.size() ? notes.size() - 1 : 0;
}

// ---------------- WIFI / AP / WEB ----------------
void saveCreds(const String& s, const String& p) {
  prefs.begin("net", false);
  prefs.putString("ssid", s);
  prefs.putString("pass", p);
  prefs.end();
}

void doScan() {
  int n = WiFi.scanNetworks();
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  std::vector<String> seen;
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (!s.length()) continue;
    if (std::find(seen.begin(), seen.end(), s) != seen.end()) continue;
    seen.push_back(s);
    JsonObject o = arr.add<JsonObject>();
    o["s"] = s;
    o["r"] = WiFi.RSSI(i);
    o["e"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
  }
  WiFi.scanDelete();
  scanJson = "";
  serializeJson(doc, scanJson);
}

void startAP() {
  apMode = true;
  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, strlen(AP_PASS) ? AP_PASS : nullptr);
  delay(300);
  dns.start(53, "*", WiFi.softAPIP());
  doScan();
  connState = CS_IDLE;
  staLostSince = 0;
  dirty = true;
}

bool connectSTA(const String& s, const String& p, uint32_t timeoutMs) {
  WiFi.setHostname(HOSTNAME);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.begin(s.c_str(), p.c_str());
  uint32_t t = millis();
  int dots = 0;
  auto& d = canvas;
  while (WiFi.status() != WL_CONNECTED && millis() - t < timeoutMs) {
    delay(300);
    M5.update();
    if (M5.BtnB.isPressed() || keypadScan() == 14) return false;   // B / S14 basili tutarak vazgec -> AP
    d.setFont(&fonts::Font2);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.setTextDatum(middle_center);
    d.drawString("Baglaniyor" + String("...").substring(0, (dots++ % 3) + 1) + "   ", d.width() / 2, d.height() / 2);
    canvas.pushSprite(0, 0);
  }
  return WiFi.status() == WL_CONNECTED;
}

void handleRoot() {
  if (apMode) server.send_P(200, "text/html", PORTAL_HTML);
  else server.send_P(200, "text/html", NOTES_HTML);
}

void handleNotFound() {
  if (apMode) {
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
  } else server.send(404, "text/plain", "yok");
}

void apiList() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  std::vector<NoteMeta> v;
  listNotes(v);
  for (auto& n : v) {
    String d, t;
    if (!readNote(n.id, d, t)) continue;
    JsonObject o = arr.add<JsonObject>();
    o["id"] = n.id; o["d"] = d; o["t"] = t;
  }
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void apiAdd() {
  uint32_t id = addNote(server.arg("plain"));
  if (id) server.send(200, "application/json", "{\"ok\":1}");
  else server.send(500, "text/plain", fsErr.length() ? fsErr : String("hata"));
}

void apiDel() {
  uint32_t id = server.arg("id").toInt();
  bool ok = id && delNote(id);
  server.send(ok ? 200 : 404, "application/json", ok ? "{\"ok\":1}" : "{\"ok\":0}");
}

void apiConnect() {
  String s = server.arg("ssid"), p = server.arg("pass");
  if (!s.length()) { server.send(400, "text/plain", "ssid?"); return; }
  pendSsid = s; pendPass = p;
  WiFi.disconnect(false);
  WiFi.begin(s.c_str(), p.c_str());
  connState = CS_CONNECTING;
  connStart = millis();
  server.send(200, "application/json", "{\"ok\":1}");
}

void apiStatus() {
  const char* st = "idle";
  if (connState == CS_CONNECTING) st = "connecting";
  else if (connState == CS_OK) st = "ok";
  else if (connState == CS_FAIL) st = "fail";
  String ip = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : "";
  server.send(200, "application/json", String("{\"s\":\"") + st + "\",\"ip\":\"" + ip + "\"}");
}

void apiGemGet() {
  JsonDocument doc;
  doc["m"] = gemModel;
  doc["k"] = gemKey.length() > 0;                       // anahtar var mi (kendisi gonderilmez)
  doc["h"] = gemKey.length() > 4 ? String("...") + gemKey.substring(gemKey.length() - 4) : String("");
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void apiGemSet() {
  if (server.hasArg("model")) {
    String m = server.arg("model");
    m.trim();
    if (m.startsWith("models/")) m = m.substring(7);
    gemModel = m;
  }
  String k = server.arg("key");
  k.trim();
  if (k.length()) gemKey = k;                           // bos birakilirsa eski anahtar korunur
  prefs.begin("cfg", false);
  prefs.putString("gmodel", gemModel);
  prefs.putString("gkey", gemKey);
  prefs.end();
  server.send(200, "application/json", "{\"ok\":1}");
}

void startWeb() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/notes", HTTP_GET, []() { server.send_P(200, "text/html", NOTES_HTML); });
  server.on("/api/notes", HTTP_GET, apiList);
  server.on("/api/add", HTTP_POST, apiAdd);
  server.on("/api/del", HTTP_POST, apiDel);
  server.on("/scan", HTTP_GET, []() { server.send(200, "application/json", scanJson); });
  server.on("/rescan", HTTP_GET, []() { doScan(); server.send(200, "application/json", scanJson); });
  server.on("/connect", HTTP_POST, apiConnect);
  server.on("/status", HTTP_GET, apiStatus);
  server.on("/api/gem", HTTP_GET, apiGemGet);
  server.on("/api/gem", HTTP_POST, apiGemSet);
  server.onNotFound(handleNotFound);
  server.begin();
}

void netLoop() {
  if (apMode) dns.processNextRequest();
  server.handleClient();
  if (connState == CS_CONNECTING) {
    if (WiFi.status() == WL_CONNECTED) {
      saveCreds(pendSsid, pendPass);
      connState = CS_OK;
      restartAt = millis() + 5000;
      dirty = true;
    } else if (millis() - connStart > 15000) {
      connState = CS_FAIL;
      WiFi.disconnect(false);
    }
  }
  if (connState == CS_OK && millis() > restartAt) ESP.restart();
  if (!apMode) {
    if (WiFi.status() != WL_CONNECTED) {
      if (!staLostSince) staLostSince = millis();
      else if (millis() - staLostSince > STA_LOST_AP_MS) { startAP(); }
    } else staLostSince = 0;
  }
}

// ---------------- GEMINI ----------------
String jsonQuote(const String& s) {
  JsonDocument d;
  d.set(s);
  String o;
  serializeJson(d, o);
  return o;
}

String histJson() {
  String s;
  for (auto& t : hist) {
    JsonDocument d;
    d["role"] = t.role;
    d["parts"][0]["text"] = t.text;
    String o;
    serializeJson(d, o);
    s += o; s += ',';
  }
  return s;
}

String bodyPrefix(bool voice) {
  String sys = "Sen M5StickS3 adli kucuk bir cihazda calisan yardimci asistansin. "
               "Kullanicinin dilinde cevap ver (genelde Turkce). Ekran cok kucuk: cevaplar kisa, net ve duz metin olsun; "
               "markdown, emoji ve ozel sembol kullanma.";
  String now = nowStr();
  if (now.length()) sys += " Simdiki tarih ve saat (Turkiye): " + now + ".";
  if (gemSearch) sys += " Guncel veya bilmedigin bir bilgi gerekirse internette ara ve sonucu kisaca ozetle.";
  if (voice) {
    if (gemSearch)
      sys += " Kullanici sesli mesaj gonderiyor. Cevabi tam su bicimde ver: ilk satir 'S: ' ile baslayip kullanicinin soyledigini yaz, "
             "sonraki satirlar 'C: ' ile baslayip cevabi yaz.";
    else
      sys += " Kullanici sesli mesaj gonderiyor: once ne dedigini 'transcript' alanina yaz, cevabini 'reply' alanina yaz.";
  }
  return "{\"systemInstruction\":{\"parts\":[{\"text\":" + jsonQuote(sys) + "}]},\"contents\":[" + histJson();
}

String bodySuffix(bool voice) {
  // Google Arama acikken yapilandirilmis (JSON) cikti kullanilmaz; sesli modda S:/C: bicimi istenir
  if (gemSearch) return "],\"tools\":[{\"google_search\":{}}]}";
  if (!voice) return "]}";
  return "],\"generationConfig\":{\"responseMimeType\":\"application/json\",\"responseSchema\":{\"type\":\"OBJECT\","
         "\"properties\":{\"transcript\":{\"type\":\"STRING\"},\"reply\":{\"type\":\"STRING\"}},"
         "\"required\":[\"transcript\",\"reply\"]}}}";
}

int geminiPost(const uint8_t* payload, size_t len, String& out) {
  if (!gemKey.length() || !gemModel.length()) {
    out = "API anahtari/model girilmemis (web sayfasindan gir)";
    return -1;
  }
  WiFiClientSecure client;
  client.setInsecure();                  // sertifika dogrulama yok (basit tutmak icin)
  HTTPClient http;
  String url = String("https://generativelanguage.googleapis.com/v1beta/models/") + gemModel + ":generateContent";
  if (!http.begin(client, url)) return -1;
  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-goog-api-key", gemKey);
  http.setConnectTimeout(15000);
  http.setTimeout(60000);
  int code = http.POST((uint8_t*)payload, len);
  if (code > 0) out = http.getString();
  else out = http.errorToString(code);
  http.end();
  return code;
}

bool parseGemini(int code, const String& resp, bool wantJson, String& tr, String& rp, String& err) {
  if (code <= 0) { err = "Baglanti hatasi: " + resp; return false; }
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, resp);
  if (code != 200) {
    const char* m = e ? nullptr : (const char*)doc["error"]["message"];
    err = String(code) + (m ? String(": ") + m : String(" HTTP hatasi"));
    return false;
  }
  if (e) { err = "JSON hatasi"; return false; }
  String text;
  JsonArray parts = doc["candidates"][0]["content"]["parts"].as<JsonArray>();
  for (JsonObject p : parts) {
    if (p["thought"] | false) continue;
    const char* t = p["text"];
    if (t) text += t;
  }
  if (!text.length()) {
    err = "Bos cevap";
    const char* fr = doc["candidates"][0]["finishReason"];
    if (fr) { err += " ("; err += fr; err += ")"; }
    return false;
  }
  if (wantJson) {
    JsonDocument d2;
    if (!deserializeJson(d2, text)) {
      const char* a = d2["transcript"] | "";
      const char* b = d2["reply"] | "";
      tr = a; rp = b;
      if (!rp.length()) rp = text;
    } else rp = text;
  } else rp = text;
  return true;
}

void pushHist(const String& u, const String& m) {
  hist.push_back({"user", u});
  hist.push_back({"model", m});
  while (hist.size() > 12) hist.erase(hist.begin(), hist.begin() + 2);
}

bool geminiText(const String& q, String& reply, String& err) {
  JsonDocument d;
  d["role"] = "user";
  d["parts"][0]["text"] = q;
  String item;
  serializeJson(d, item);
  String body = bodyPrefix(false) + item + bodySuffix(false);
  String resp, tr;
  int code = geminiPost((const uint8_t*)body.c_str(), body.length(), resp);
  if (!parseGemini(code, resp, false, tr, reply, err)) return false;
  pushHist(q, reply);
  return true;
}

bool geminiVoice(const uint8_t* wav, size_t wavLen, String& tr, String& reply, String& err) {
  String pre = bodyPrefix(true) + "{\"role\":\"user\",\"parts\":[{\"inlineData\":{\"mimeType\":\"audio/wav\",\"data\":\"";
  String suf = "\"}}]}" + bodySuffix(true);
  size_t b64cap = 4 * ((wavLen + 2) / 3) + 4;
  size_t total = pre.length() + b64cap + suf.length() + 8;
  uint8_t* buf = (uint8_t*)ps_malloc(total);
  if (!buf) { err = "Bellek yetersiz (PSRAM?)"; return false; }
  memcpy(buf, pre.c_str(), pre.length());
  size_t olen = 0;
  if (mbedtls_base64_encode(buf + pre.length(), b64cap, &olen, wav, wavLen) != 0) {
    free(buf); err = "Base64 hatasi"; return false;
  }
  memcpy(buf + pre.length() + olen, suf.c_str(), suf.length());
  size_t len = pre.length() + olen + suf.length();
  String resp;
  int code = geminiPost(buf, len, resp);
  free(buf);
  if (!parseGemini(code, resp, !gemSearch, tr, reply, err)) return false;
  if (gemSearch) {                       // "S: ...  C: ..." bicimini ayir
    String raw = reply;
    raw.trim();
    int cs = raw.indexOf("\nC:");
    if (cs < 0) cs = raw.indexOf(" C:");
    if (raw.startsWith("S:") && cs > 0) {
      tr = raw.substring(2, cs); tr.trim();
      reply = raw.substring(cs + 3); reply.trim();
    }
  }
  pushHist(tr.length() ? tr : String("(sesli mesaj)"), reply);
  return true;
}

// ---------------- EKRAN CIZIM ----------------
void drawHeader(const String& title) {
  auto& d = canvas;
  curTitle = title;
  d.fillRect(0, 0, d.width(), 18, C_HDR);
  d.setFont(&fonts::Font2);
  String r;
  if (d.width() < 200) r = apMode ? "AP" : (WiFi.status() == WL_CONNECTED ? "W" : "-");
  else r = apMode ? "AP" : (WiFi.status() == WL_CONNECTED ? "WiFi" : "--");
  int b = M5.Power.getBatteryLevel();
  if (b >= 0) r += " " + String(b) + "%";
  int rw = d.textWidth(r.c_str());
  d.setTextColor(TFT_WHITE, C_HDR);
  d.setTextDatum(top_left);
  d.drawString(fitText(title, d.width() - rw - 14), 4, 1);
  d.setTextDatum(top_right);
  d.drawString(r, d.width() - 4, 1);
  d.setTextDatum(top_left);
}

void toast(const String& m, uint16_t col = TFT_YELLOW, int ms = 1200) {
  auto& d = canvas;
  std::vector<Line> tl;
  wrapText(m, col, d.width() - 32, tl);
  if (tl.size() > 4) tl.resize(4);
  int h = tl.size() * 16 + 16;
  int y0 = (d.height() - h) / 2;
  d.fillRoundRect(8, y0, d.width() - 16, h, 6, C_HDR);
  d.drawRoundRect(8, y0, d.width() - 16, h, 6, col);
  d.setFont(&fonts::Font2);
  d.setTextColor(col, C_HDR);
  d.setTextDatum(middle_center);
  for (size_t i = 0; i < tl.size(); i++) d.drawString(tl[i].t, d.width() / 2, y0 + 16 + i * 16);
  d.setTextDatum(top_left);
  canvas.pushSprite(0, 0);
  if (ms > 0) { delay(ms); dirty = true; }
}

String ellipsize(String s, int w) {
  auto& d = canvas;
  d.setFont(&fonts::Font2);
  while (s.length() > 0 && d.textWidth((s + "...").c_str()) > w) s.remove(s.length() - 1);
  return s + "...";
}

// Liste ogesini kelime kelime sarar (en fazla 2 satir). Sigmayan kelime bir alt satira bütün olarak gecer.
std::vector<String> itemRows(const String& txt, int maxW) {
  std::vector<Line> tl;
  wrapText(txt, TFT_WHITE, maxW, tl);
  std::vector<String> rows;
  if (tl.size() > 2) {
    rows.push_back(tl[0].t);
    rows.push_back(ellipsize(tl[1].t, maxW));
  } else {
    for (auto& l : tl) rows.push_back(l.t);
  }
  if (rows.empty()) rows.push_back("");
  return rows;
}

void drawList(const String& title, const std::vector<String>& items, int idx) {
  auto& d = canvas;
  drawHeader(title);
  d.setFont(&fonts::Font2);
  d.setTextDatum(top_left);
  int n = items.size();
  if (!n) { d.setTextColor(TFT_DARKGREY, TFT_BLACK); d.drawString("(bos)", 10, 30); return; }
  int H = d.height(), W = d.width();
  int maxW = W - 24;
  auto hOf = [&](int k) { return (int)itemRows(items[k], maxW).size() * 16 + 6; };
  static String lastTitle;
  static int first = 0;
  if (title != lastTitle) { lastTitle = title; first = 0; }
  if (first > idx) first = idx;
  if (first < idx - 11) first = idx - 11;
  if (first < 0) first = 0;
  for (;;) {                                           // secili oge tamami gorunsun
    int sum = 0;
    for (int k = first; k <= idx; k++) sum += hOf(k);
    if (sum <= H - 22 || first >= idx) break;
    first++;
  }
  int y = 22;
  for (int k = first; k < n; k++) {
    std::vector<String> rows = itemRows(items[k], maxW);
    int h = (int)rows.size() * 16 + 6;
    if (k > first && y + h > H) break;
    bool sel = (k == idx);
    if (sel) d.fillRoundRect(2, y - 2, W - 8, h - 2, 4, C_SEL);
    d.setTextColor(TFT_WHITE, sel ? C_SEL : TFT_BLACK);
    for (size_t i = 0; i < rows.size(); i++) d.drawString(rows[i], 10, y + (int)i * 16);
    y += h;
  }
}

void drawLines() {
  auto& d = canvas;
  d.setFont(&fonts::Font2);
  d.setTextDatum(top_left);
  for (int i = 0; i < VIS; i++) {
    int idx = scrollPos + i;
    if (idx >= (int)lines.size()) break;
    d.setTextColor(lines[idx].c, TFT_BLACK);
    d.drawString(lines[idx].t, 4, 20 + i * 16);
  }
  int n = lines.size();
  if (n > VIS) {
    int h = VIS * 16;
    int bh = h * VIS / n; if (bh < 8) bh = 8;
    int rng = n - VIS;
    int by = 20 + (h - bh) * scrollPos / (rng > 0 ? rng : 1);
    d.fillRect(d.width() - 4, by, 3, bh, TFT_DARKGREY);
  }
}

void scrollBy(int dlt) {
  int mx = (int)lines.size() - VIS; if (mx < 0) mx = 0;
  scrollPos += dlt;
  if (scrollPos < 0) scrollPos = 0;
  if (scrollPos > mx) scrollPos = mx;
  dirty = true;
}

void pageScroll() {                      // B: bir sayfa asagi, sonda basa don
  int mx = (int)lines.size() - VIS;
  if (mx <= 0) return;
  if (scrollPos >= mx) scrollPos = 0;
  else { scrollPos += VIS - 1; if (scrollPos > mx) scrollPos = mx; }
  dirty = true;
}

void drawKeyboard() {
  auto& d = canvas;
  const char* lay[3] = {"abc", "ABC", "#+="};
  const char* kbName = (kbPurpose == KB_NOTE) ? "Yeni Not " : (kbPurpose == KB_WIFI ? "Sifre " : "Gemini ");
  drawHeader(String(kbName) + "[" + lay[kbLayer] + "]");
  int W = d.width(), H = d.height();
  int cw = W / 10;
  int ch = (H > W) ? 24 : 19;
  int kbTop = H - 4 * ch - 2;
  int topY = 20;
  if (kbPurpose == KB_WIFI) {            // hangi aga sifre yazildigini goster
    d.setFont(&fonts::Font2);
    d.setTextColor(TFT_CYAN, TFT_BLACK);
    d.setTextDatum(top_left);
    d.drawString(fitText(wifiSel, W - 8), 4, 20);
    topY = 36;
  }
  int maxLines = (kbTop - topY - 2) / 16;
  if (maxLines < 1) maxLines = 1;
  std::vector<Line> tl;
  wrapText(kbText + "_", TFT_WHITE, W - 8, tl);
  d.setFont(&fonts::Font2);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.setTextDatum(top_left);
  int start = (int)tl.size() > maxLines ? (int)tl.size() - maxLines : 0;
  for (int i = start; i < (int)tl.size(); i++) d.drawString(tl[i].t, 4, topY + (i - start) * 16);
  d.setTextDatum(middle_center);
  int x0 = (W - cw * 10) / 2;
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 10; c++) {
      int x = x0 + c * cw, y = kbTop + r * ch;
      bool sel = (r == kbR && c == kbC);
      if (sel) d.fillRoundRect(x + 1, y, cw - 2, ch - 1, 3, C_SEL);
      else d.drawRoundRect(x + 1, y, cw - 2, ch - 1, 3, TFT_DARKGREY);
      d.setTextColor(TFT_WHITE, sel ? C_SEL : TFT_BLACK);
      d.drawString(String(KB[kbLayer][r][c]), x + cw / 2, y + ch / 2);
    }
  }
  d.setTextDatum(top_left);
}

void drawNet() {
  auto& d = canvas;
  drawHeader("Ag Bilgisi");
  std::vector<Line> v;
  if (apMode) {
    wrapText("AP modu (kurulum)", TFT_ORANGE, WRAPW, v);
    wrapText(String("Ag: ") + AP_SSID, TFT_WHITE, WRAPW, v);
    wrapText("Telefonla baglan, oturum ac sayfasi acilir veya 192.168.4.1", TFT_CYAN, WRAPW, v);
    if (connState == CS_CONNECTING) wrapText("Baglaniyor...", TFT_YELLOW, WRAPW, v);
    if (connState == CS_OK) wrapText("Baglandi! Yeniden basliyor", TFT_GREEN, WRAPW, v);
    if (connState == CS_FAIL) wrapText("Baglanamadi", TFT_RED, WRAPW, v);
  } else {
    bool ok = WiFi.status() == WL_CONNECTED;
    wrapText(ok ? "WiFi bagli" : "WiFi baglantisi yok", ok ? TFT_GREEN : TFT_RED, WRAPW, v);
    wrapText("SSID: " + WiFi.SSID(), TFT_WHITE, WRAPW, v);
    wrapText("IP: " + WiFi.localIP().toString(), TFT_CYAN, WRAPW, v);
    wrapText("http://" HOSTNAME ".local", TFT_CYAN, WRAPW, v);
    wrapText("Notlar: / veya /notes", TFT_DARKGREY, WRAPW, v);
    bool gOk = gemKey.length() && gemModel.length();
    wrapText(gOk ? "Gemini: hazir" : "Gemini: web'den ayarla", gOk ? TFT_GREEN : TFT_ORANGE, WRAPW, v);
  }
  d.setFont(&fonts::Font2);
  d.setTextDatum(top_left);
  for (int i = 0; i < (int)v.size() && i < VIS; i++) {
    d.setTextColor(v[i].c, TFT_BLACK);
    d.drawString(v[i].t, 4, 22 + i * 16);
  }
}

void drawRecording(float sec, bool full) {
  auto& d = canvas;
  int cy = d.height() / 2 - 22;
  if (full) {
    d.fillScreen(TFT_BLACK);
    drawHeader("Gemini Sesli");
    d.fillCircle(22, cy, 11, TFT_RED);
    d.setFont(&fonts::Font4);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.setTextDatum(middle_left);
    d.drawString("KAYIT", 42, cy);
    d.setFont(&fonts::Font2);
    d.setTextDatum(top_left);
    d.setTextColor(TFT_DARKGREY, TFT_BLACK);
    d.drawString("birakinca gider", 10, cy + 50);
  }
  char b[24];
  snprintf(b, sizeof b, "%4.1f / %d sn  ", sec, MAX_REC_SEC);
  d.setFont(&fonts::Font2);
  d.setTextColor(TFT_YELLOW, TFT_BLACK);
  d.setTextDatum(top_left);
  d.drawString(b, 10, cy + 26);
  canvas.pushSprite(0, 0);
}

void render() {
  auto& d = canvas;
  d.startWrite();
  d.fillScreen(TFT_BLACK);
  switch (screen) {
    case SC_MENU: {
      std::vector<String> it;
      for (auto s : MAIN_ITEMS) it.push_back(s);
      drawList("Menu", it, menuIdx);
      break;
    }
    case SC_NOTES: {
      std::vector<String> it;
      for (auto& n : notes) it.push_back(n.title.length() ? n.title : String("(bos not)"));
      drawList("Notlar (" + String(notes.size()) + ")", it, notesIdx);
      break;
    }
    case SC_NOTE_VIEW:
      drawHeader("Not");
      drawLines();
      break;
    case SC_GEM_MENU: {
      std::vector<String> it;
      for (auto s : GEM_ITEMS) it.push_back(s);
      it.push_back(String("Internet: ") + (gemSearch ? "acik" : "kapali"));
      drawList("Gemini", it, gemIdx);
      break;
    }
    case SC_CHAT:
      drawHeader(chatVoice ? "Gemini Sesli" : "Gemini");
      drawLines();
      break;
    case SC_KB:
      drawKeyboard();
      break;
    case SC_NET:
      drawNet();
      break;
    case SC_SETTINGS: {
      std::vector<String> it;
      it.push_back("Ag Bilgisi");
      it.push_back("WiFi Ekle");
      it.push_back("WiFi Kurulum (AP)");
      it.push_back("Tuslar");
      it.push_back("Parlaklik: " + String(brightness));
      {
        const char* rn[4] = {"dikey", "yatay", "dikey ters", "yatay ters"};
        it.push_back(String("Ekran: ") + rn[scrRot & 3]);
        it.push_back("Keypad: " + String((keyRot & 3) * 90) + " derece");
      }
      drawList("Ayarlar", it, setIdx);
      break;
    }
    case SC_WIFI: {
      std::vector<String> it;
      for (auto& n : wifiNets) it.push_back(n.ssid + (n.enc ? " *" : ""));
      it.push_back("[Yeniden tara]");
      drawList("WiFi Sec", it, wifiIdx);
      break;
    }
    case SC_KEYS:
      drawHeader("Tuslar");
      drawLines();
      break;
    case SC_GAME:
      drawGame();
      break;
  }
  d.endWrite();
  canvas.pushSprite(0, 0);
}

// ---------------- SOHBET / SES ----------------
void setChat(const String& user, const String& reply, bool ok) {
  lines.clear();
  if (user.length()) {
    wrapText("Sen: " + user, TFT_CYAN, WRAPW, lines);
    lines.push_back({"", TFT_BLACK});
  }
  wrapText((ok ? "Gemini: " : "") + reply, ok ? TFT_WHITE : TFT_RED, WRAPW, lines);
  scrollPos = 0;
  dirty = true;
}

void voiceHint() {
  lines.clear();
  wrapText("Gemini Sesli", TFT_CYAN, WRAPW, lines);
  wrapText("", TFT_BLACK, WRAPW, lines);
  wrapText("A tusuna (veya " + keyName(13) + ") BASILI TUT ve konus, birakinca gider.", TFT_WHITE, WRAPW, lines);
  wrapText("", TFT_BLACK, WRAPW, lines);
  wrapText("B / " + keyName(14) + ": kaydir", TFT_DARKGREY, WRAPW, lines);
  wrapText("B / " + keyName(14) + " basili: geri", TFT_DARKGREY, WRAPW, lines);
  scrollPos = 0;
}

void keysHelp() {
  lines.clear();
  auto add = [&](const String& t, uint16_t c) { wrapText(t, c, WRAPW, lines); };
  auto kn = [&](int n) { return keyName(n); };      // keypad yonune gore yazili tus adi
  add("KLAVYE", TFT_YELLOW);
  add(kn(1) + " sil, " + kn(2) + " yukari", TFT_WHITE);
  add(kn(3) + " bosluk, " + kn(4) + " gonder", TFT_WHITE);
  add(kn(5) + " sol, " + kn(6) + " asagi, " + kn(7) + " sag", TFT_WHITE);
  add(kn(8) + " shift, " + kn(9) + " sembol", TFT_WHITE);
  add(kn(10) + " hepsini sil", TFT_WHITE);
  add("A / " + kn(13) + ": harfi yaz", TFT_WHITE);
  add("B / " + kn(14) + ": sonraki", TFT_WHITE);
  add("", TFT_BLACK);
  add("MENULER", TFT_YELLOW);
  add(kn(2) + "/" + kn(6) + " gez, " + kn(4) + " sec", TFT_WHITE);
  add(kn(13) + " = A (sec)", TFT_WHITE);
  add(kn(14) + " = B (sonraki)", TFT_WHITE);
  add(kn(5) + " veya B / " + kn(14) + " basili: geri", TFT_WHITE);
  add("", TFT_BLACK);
  add("NOT / SOHBET", TFT_YELLOW);
  add(kn(2) + "/" + kn(6) + " kaydir", TFT_WHITE);
  add(kn(1) + " notu sil (2 kez bas)", TFT_WHITE);
  add("", TFT_BLACK);
  add("SESLI", TFT_YELLOW);
  add("A / " + kn(13) + " basili tut: konus", TFT_WHITE);
  add("", TFT_BLACK);
  add("PIYANO", TFT_YELLOW);
  add("Sutundaki herhangi bir tus o sutunu vurur", TFT_WHITE);
  add("B basili: cik", TFT_WHITE);
  add("", TFT_BLACK);
  add("GENEL", TFT_YELLOW);
  add("A sec, B sonraki", TFT_WHITE);
  add("B basili tut: geri", TFT_WHITE);
  scrollPos = 0;
}

void writeWavHeader(uint8_t* b, uint32_t dataLen) {
  uint32_t v; uint16_t s;
  memcpy(b, "RIFF", 4); v = 36 + dataLen; memcpy(b + 4, &v, 4);
  memcpy(b + 8, "WAVEfmt ", 8);
  v = 16; memcpy(b + 16, &v, 4);
  s = 1; memcpy(b + 20, &s, 2);
  s = 1; memcpy(b + 22, &s, 2);
  v = REC_RATE; memcpy(b + 24, &v, 4);
  v = REC_RATE * 2; memcpy(b + 28, &v, 4);
  s = 2; memcpy(b + 32, &s, 2);
  s = 16; memcpy(b + 34, &s, 2);
  memcpy(b + 36, "data", 4);
  memcpy(b + 40, &dataLen, 4);
}

size_t recordVoice() {                   // A basili oldugu surece kaydeder, ornek sayisi doner
  const size_t maxSamples = (size_t)REC_RATE * MAX_REC_SEC;
  const size_t CH = 1024;
  int16_t* pcm = (int16_t*)(recBuf + 44);
  M5.Mic.begin();
  size_t pos = 0;
  uint32_t lastDraw = 0;
  drawRecording(0, true);
  while (aHeld() && pos + CH <= maxSamples) {
    M5.update();
    if (M5.Mic.record(pcm + pos, CH, REC_RATE)) pos += CH;
    if (millis() - lastDraw > 250) {
      lastDraw = millis();
      drawRecording((float)pos / REC_RATE, false);
    }
  }
  while (M5.Mic.isRecording()) delay(1);
  M5.Mic.end();
  return pos;
}

void doVoice() {
  if (!netOnline()) { toast("Internet yok", TFT_RED); return; }
  if (!psramFound()) { toast("PSRAM kapali!", TFT_RED, 1800); return; }
  if (!recBuf) {
    recBuf = (uint8_t*)ps_malloc(44 + (size_t)REC_RATE * MAX_REC_SEC * 2);
    if (!recBuf) { toast("Bellek yok", TFT_RED); return; }
  }
  size_t n = recordVoice();
  if (n < (size_t)REC_RATE / 2) { toast("Basili tutarak konus"); return; }
  writeWavHeader(recBuf, n * 2);
  canvas.fillScreen(TFT_BLACK);
  drawHeader("Gemini Sesli");
  toast("Gemini dusunuyor...", TFT_CYAN, 0);
  String tr, rp, err;
  bool ok = geminiVoice(recBuf, 44 + n * 2, tr, rp, err);
  setChat(tr, ok ? rp : err, ok);
  dirty = true;
}

void sendKb() {
  if (kbPurpose == KB_WIFI) {            // sifre girildi: aga baglan
    if (!kbText.length()) return;
    wifiConnectFlow(wifiSel, kbText);
    return;
  }
  String q = kbText;
  q.trim();
  if (!q.length()) return;
  if (kbPurpose == KB_NOTE) {
    if (addNote(q)) {
      toast("Not kaydedildi", TFT_GREEN, 900);
      kbText = "";
      screen = SC_MENU;
    } else {
      toast("Hata: " + fsErr, TFT_RED, 2200);   // yazi silinmez
    }
    dirty = true;
    return;
  }
  if (!netOnline()) { toast("Internet yok", TFT_RED); return; }
  toast("Gemini dusunuyor...", TFT_CYAN, 0);
  String rp, err;
  bool ok = geminiText(q, rp, err);
  kbText = "";
  chatVoice = false;
  setChat(q, ok ? rp : err, ok);
  screen = SC_CHAT;
  dirty = true;
}

void openNote(uint32_t id) {
  String dt, tx;
  if (!readNote(id, dt, tx)) return;
  viewNoteId = id;
  lines.clear();
  if (dt.length()) wrapText(dt, TFT_YELLOW, WRAPW, lines);
  wrapText(tx, TFT_WHITE, WRAPW, lines);
  scrollPos = 0;
  delArm = 0;
  screen = SC_NOTE_VIEW;
  dirty = true;
}

// ---------------- PIYANO (magic tiles) ----------------
struct Tile { int col; float y; bool hit; uint32_t hitT; };
enum GState { GS_READY, GS_PLAY, GS_OVER };
std::vector<Tile> tiles;
GState gState = GS_READY;
uint8_t gDiff = 1;                       // 0 kolay (1 blok), 1 normal (2 blok), 2 zor (3 blok)
uint16_t gBest[3] = {0, 0, 0};
int gScore = 0, gTh = 40, gLastCol = -1, gFailCol = -1;
uint32_t gLast = 0, gOverT = 0, gColUp[4] = {0, 0, 0, 0};
float gSince = 0, gNextGap = 0;
uint8_t gColsPrev = 0, gReadyCols = 0;
const char* GM_NAMES[3] = {"Kolay", "Normal", "Zor"};

uint8_t colsDown() {                     // hangi sutunlarda tus basili (bit 0..3)
  uint16_t mk = keypadMask();
  uint8_t c = 0;
  for (int L = 0; L < 16; L++) if (mk & (1u << L)) c |= (1 << (L % 4));
  return c;
}

float gSpeed() {                         // px/sn, skorla yavas yavas hizlanir
  float H = canvas.height();
  float v = H * 0.50f + gScore * H * 0.008f;
  float cap = H * 1.4f;
  return v > cap ? cap : v;
}

void saveGameDiff() {
  prefs.begin("cfg", false);
  prefs.putUChar("gd", gDiff);
  prefs.end();
}

void startGame() {
  tiles.clear();
  gScore = 0;
  gTh = canvas.height() / 4;
  gSince = 0; gNextGap = 0;
  gLastCol = -1; gFailCol = -1;
  gColsPrev = colsDown();                // basili tus varsa birakilana kadar sayma
  for (int i = 0; i < 4; i++) gColUp[i] = millis();
  gLast = millis();
  gState = GS_PLAY;
}

void failGame(int col) {
  gFailCol = col;
  gState = GS_OVER;
  gOverT = millis();
  if (gScore > (int)gBest[gDiff]) {
    gBest[gDiff] = gScore;
    prefs.begin("cfg", false);
    prefs.putUShort(("hs" + String(gDiff)).c_str(), gBest[gDiff]);
    prefs.end();
  }
  dirty = true;
}

void spawnTile() {
  int col = random(4);
  if (col == gLastCol && random(100) < 70) col = (col + 1 + random(3)) % 4;
  Tile t;
  t.col = col; t.y = -gTh; t.hit = false; t.hitT = 0;
  tiles.push_back(t);
  gLastCol = col;
  gSince = 0;
  gNextGap = gTh * (1.05f + random(0, 131) / 100.0f);   // karolar arasi rastgele bosluk
}

void tapCol(int c) {
  int best = -1;
  float by = -1e9f;
  for (size_t i = 0; i < tiles.size(); i++) {
    Tile& t = tiles[i];
    if (!t.hit && t.col == c && t.y + gTh > 6 && t.y > by) { best = (int)i; by = t.y; }
  }
  if (best < 0) { failGame(c); return; }  // bos sutuna basti
  tiles[best].hit = true;
  tiles[best].hitT = millis();
  gScore++;
}

void stepGame(float dt) {
  int H = canvas.height();
  float dy = gSpeed() * dt;
  uint32_t now = millis();
  for (size_t i = 0; i < tiles.size(); i++) tiles[i].y += dy;
  for (int i = (int)tiles.size() - 1; i >= 0; i--)
    if (tiles[i].hit && now - tiles[i].hitT > 150) tiles.erase(tiles.begin() + i);
  for (size_t i = 0; i < tiles.size(); i++)
    if (!tiles[i].hit && tiles[i].y >= H) { failGame(tiles[i].col); return; }   // karo kacti
  int active = 0;
  for (size_t i = 0; i < tiles.size(); i++) if (!tiles[i].hit) active++;
  gSince += dy;
  if (gSince >= gNextGap) {
    if (active < (int)gDiff + 1) spawnTile();   // ayni anda en fazla 1 / 2 / 3 blok
    else gSince = gNextGap;
  }
}

void drawGame() {
  auto& d = canvas;
  int W = d.width(), H = d.height();
  int cw = W / 4, x0 = (W - cw * 4) / 2;
  d.fillScreen(0x0841);
  if (gState == GS_READY) {              // basili sutunlari goster (hizalamayi denemek icin)
    for (int c = 0; c < 4; c++)
      if (gReadyCols & (1 << c)) d.fillRect(x0 + c * cw, 0, cw, H, 0x2965);
  }
  for (int i = 0; i <= 4; i++) d.drawFastVLine(x0 + i * cw, 0, H, 0x3186);

  if (gState != GS_READY) {
    for (size_t i = 0; i < tiles.size(); i++) {
      Tile& t = tiles[i];
      uint16_t col = 0xFFFF;
      if (t.hit) col = 0x4A69;
      else if (gState == GS_OVER && t.col == gFailCol && t.y >= H) col = TFT_RED;
      d.fillRoundRect(x0 + t.col * cw + 2, (int)t.y, cw - 4, gTh - 3, 3, col);
    }
    if (gState == GS_OVER && gFailCol >= 0) {
      d.drawRect(x0 + gFailCol * cw, 0, cw + 1, H, TFT_RED);
      d.fillRect(x0 + gFailCol * cw + 1, H - 6, cw - 1, 6, TFT_RED);
    }
    d.setFont(&fonts::Font4);
    d.setTextDatum(top_center);
    d.setTextColor(TFT_YELLOW);
    d.drawString(String(gScore), W / 2, 2);
  }

  if (gState == GS_READY) {
    int y = 6;
    d.setTextDatum(top_center);
    d.setFont(&fonts::Font4);
    d.setTextColor(TFT_YELLOW);
    d.drawString("PIYANO", W / 2, y); y += 34;
    d.setFont(&fonts::Font2);
    d.setTextColor(TFT_WHITE);
    d.drawString("Mod (B)", W / 2, y); y += 18;
    d.setFont(&fonts::Font4);
    d.setTextColor(TFT_CYAN);
    d.drawString(GM_NAMES[gDiff], W / 2, y); y += 30;
    d.setFont(&fonts::Font2);
    d.setTextColor(TFT_WHITE);
    d.drawString(String(gDiff + 1) + " blok ayni anda", W / 2, y); y += 18;
    d.setTextColor(TFT_GREEN);
    d.drawString("En iyi: " + String(gBest[gDiff]), W / 2, y); y += 22;
    std::vector<Line> hl;
    wrapText("A / " + keyName(13) + " basla, B / " + keyName(14) + " mod sec.", TFT_WHITE, W - 12, hl);
    wrapText("Her sutunun herhangi bir tusu o sutunu vurur.", TFT_DARKGREY, W - 12, hl);
    wrapText("B basili: geri", TFT_DARKGREY, W - 12, hl);
    for (size_t i = 0; i < hl.size() && y + 16 <= H; i++) {
      d.setTextColor(hl[i].c);
      d.drawString(hl[i].t, W / 2, y);
      y += 16;
    }
  }

  if (gState == GS_OVER) {
    int ph = 108; if (ph > H - 8) ph = H - 8;
    int py = (H - ph) / 2, pw = W - 12;
    d.fillRoundRect(6, py, pw, ph, 8, 0x0000);
    d.drawRoundRect(6, py, pw, ph, 8, TFT_RED);
    int y = py + 6;
    d.setTextDatum(top_center);
    d.setFont(&fonts::Font2);
    d.setTextColor(TFT_RED);
    d.drawString("OYUN BITTI", W / 2, y); y += 18;
    d.setFont(&fonts::Font4);
    d.setTextColor(TFT_YELLOW);
    d.drawString(String(gScore), W / 2, y); y += 30;
    d.setFont(&fonts::Font2);
    d.setTextColor(TFT_WHITE);
    d.drawString("En iyi: " + String(gBest[gDiff]), W / 2, y); y += 18;
    d.setTextColor(TFT_GREEN);
    d.drawString("A: tekrar", W / 2, y); y += 16;
    d.setTextColor(TFT_CYAN);
    d.drawString("B: mod sec", W / 2, y);
  }
  d.setTextDatum(top_left);
}

void gameTick() {                        // oyun sirasinda her dongude cagrilir
  uint32_t now = millis();
  uint8_t cur = colsDown();
  for (int c = 0; c < 4 && gState == GS_PLAY; c++) {
    bool down = cur & (1 << c), was = gColsPrev & (1 << c);
    if (down && !was && now - gColUp[c] > 25) tapCol(c);
    if (!down && was) gColUp[c] = now;
  }
  gColsPrev = cur;
  if (gState != GS_PLAY) { dirty = true; return; }
  if (now - gLast >= 28) {
    float dt = (now - gLast) / 1000.0f;
    if (dt > 0.1f) dt = 0.1f;
    gLast = now;
    stepGame(dt);
    if (gState == GS_PLAY) { drawGame(); canvas.pushSprite(0, 0); }
    else dirty = true;
  }
}

void gameIdle() {                        // hazir ekraninda basili sutunu canli goster
  uint8_t cur = colsDown();
  if (cur != gReadyCols) { gReadyCols = cur; dirty = true; }
}

// ---------------- CIHAZDAN WIFI EKLE ----------------
void wifiScanList() {
  int n = WiFi.scanNetworks();
  wifiNets.clear();
  for (int i = 0; i < n; i++) {
    String sd = WiFi.SSID(i);
    if (!sd.length()) continue;
    int rssi = WiFi.RSSI(i);
    bool enc = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    bool found = false;
    for (auto& w : wifiNets) {
      if (w.ssid == sd) { found = true; if (rssi > w.rssi) { w.rssi = rssi; w.enc = enc; } break; }
    }
    if (!found) { WifiNet w; w.ssid = sd; w.rssi = rssi; w.enc = enc; wifiNets.push_back(w); }
  }
  WiFi.scanDelete();
  std::sort(wifiNets.begin(), wifiNets.end(), [](const WifiNet& a, const WifiNet& b) { return a.rssi > b.rssi; });
}

bool wifiTryConnect(const String& sd, const String& pw) {
  WiFi.setHostname(HOSTNAME);
  WiFi.disconnect(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.begin(sd.c_str(), pw.c_str());
  uint32_t t = millis();
  int dots = 0;
  while (WiFi.status() != WL_CONNECTED && millis() - t < 15000) {
    delay(300);
    M5.update();
    if (M5.BtnB.isPressed()) break;                       // B'ye basarak vazgec
    if (WiFi.status() == WL_CONNECT_FAILED) break;        // sifre yanlis
    toast("Baglaniyor" + String("...").substring(0, (dots++ % 3) + 1), TFT_CYAN, 0);
  }
  return WiFi.status() == WL_CONNECTED;
}

void wifiRestoreOld() {                  // basarisiz olursa kayitli aga geri don
  prefs.begin("net", true);
  String os = prefs.getString("ssid", "");
  String op = prefs.getString("pass", "");
  prefs.end();
  staLostSince = 0;
  if (os.length()) WiFi.begin(os.c_str(), op.c_str());
}

void wifiConnectFlow(const String& sd, const String& pw) {
  bool wasAp = apMode;
  if (wifiTryConnect(sd, pw)) {
    saveCreds(sd, pw);                   // sonraki acilislarda otomatik baglanir
    apMode = false;
    dns.stop();
    connState = CS_IDLE;
    staLostSince = 0;
    configTzTime("<+03>-3", "pool.ntp.org", "time.google.com");
    MDNS.end();
    if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);
    kbText = "";
    toast("Baglandi!", TFT_GREEN, 900);
    screen = SC_NET;
  } else {
    if (wasAp) startAP(); else wifiRestoreOld();
    toast("Baglanamadi. Sifreyi kontrol et", TFT_RED, 1800);
    screen = pw.length() ? SC_KB : SC_WIFI;               // sifre ekranina don, yazilan korunur
  }
  dirty = true;
}

// ---------------- GIRIS ISLEME ----------------
void applyScreenRotation() {             // yonu uygular, tamponu yeni olcuyle yeniden kurar
  M5.Display.setRotation(scrRot & 3);
  M5.Display.fillScreen(TFT_BLACK);
  canvas.deleteSprite();
  canvas.setColorDepth(16);
  if (!canvas.createSprite(M5.Display.width(), M5.Display.height())) {
    canvas.setPsram(true);
    canvas.createSprite(M5.Display.width(), M5.Display.height());
  }
  canvas.fillScreen(TFT_BLACK);
  SW = canvas.width(); SH = canvas.height();
  WRAPW = SW - 12;
  VIS = (SH - 20) / 16;
  lines.clear();                         // eski genislige gore sarilmis satirlari at
  scrollPos = 0;
}

void typeChar() {
  if (kbText.length() >= 400) return;
  kbText += KB[kbLayer][kbR][kbC];
  if (kbLayer == 1) kbLayer = 0;         // shift tek seferlik
  dirty = true;
}

void handleInput(const Input& in) {
  bool nx = in.next || in.key == 6;
  bool pv = in.key == 2;
  bool ok = in.sel || in.key == 4;
  bool bk = in.back || in.key == 5;

  switch (screen) {
    case SC_MENU: {
      const int N = 5;
      if (nx) { menuIdx = (menuIdx + 1) % N; dirty = true; }
      if (pv) { menuIdx = (menuIdx + N - 1) % N; dirty = true; }
      if (ok) {
        switch (menuIdx) {
          case 0: loadNotes(); notesIdx = 0; screen = SC_NOTES; break;
          case 1: kbPurpose = KB_NOTE; kbText = ""; kbLayer = 0; screen = SC_KB; break;
          case 2: gemIdx = 0; screen = SC_GEM_MENU; break;
          case 3: gState = GS_READY; gReadyCols = 0; screen = SC_GAME; break;
          case 4: setIdx = 0; screen = SC_SETTINGS; break;
        }
        dirty = true;
      }
      break;
    }
    case SC_NOTES: {
      int N = notes.size();
      if (nx && N) { notesIdx = (notesIdx + 1) % N; dirty = true; }
      if (pv && N) { notesIdx = (notesIdx + N - 1) % N; dirty = true; }
      if (ok && N) openNote(notes[notesIdx].id);
      if (bk) { screen = SC_MENU; dirty = true; }
      break;
    }
    case SC_NOTE_VIEW: {
      if (in.next) pageScroll();
      if (in.key == 6) scrollBy(1);
      if (in.key == 2) scrollBy(-1);
      if (in.key == 1) {
        if (millis() < delArm) {
          delNote(viewNoteId);
          loadNotes();
          screen = SC_NOTES;
          toast("Silindi", TFT_GREEN, 700);
        } else {
          delArm = millis() + 3000;
          toast("Silmek icin tekrar bas: " + keyName(1), TFT_ORANGE, 900);
        }
        dirty = true;
      }
      if (in.back || in.key == 5) { loadNotes(); screen = SC_NOTES; dirty = true; }
      break;
    }
    case SC_GEM_MENU: {
      const int N = 4;
      if (nx) { gemIdx = (gemIdx + 1) % N; dirty = true; }
      if (pv) { gemIdx = (gemIdx + N - 1) % N; dirty = true; }
      if (ok) {
        if (gemIdx == 0) {
          if (!netOnline()) { toast("Internet yok", TFT_RED); }
          else { chatVoice = true; voiceHint(); screen = SC_CHAT; }
        } else if (gemIdx == 1) {
          if (!netOnline()) { toast("Internet yok", TFT_RED); }
          else { kbPurpose = KB_GEMINI; kbText = ""; kbLayer = 0; screen = SC_KB; }
        } else if (gemIdx == 2) {
          hist.clear();
          toast("Sohbet sifirlandi", TFT_GREEN, 800);
        } else {                                     // internet aramasi ac / kapat
          gemSearch = !gemSearch;
          prefs.begin("cfg", false);
          prefs.putUChar("gs", gemSearch ? 1 : 0);
          prefs.end();
        }
        dirty = true;
      }
      if (bk) { screen = SC_MENU; dirty = true; }
      break;
    }
    case SC_CHAT: {
      if (chatVoice && in.aPress) { doVoice(); break; }
      if (in.next) pageScroll();
      if (in.key == 6) scrollBy(1);
      if (in.key == 2) scrollBy(-1);
      if (!chatVoice && ok && in.key != 6) { screen = SC_KB; dirty = true; }   // A / S4: yeni soru
      if (in.back || in.key == 5) {
        if (recBuf) { free(recBuf); recBuf = nullptr; }
        screen = SC_GEM_MENU; dirty = true;
      }
      break;
    }
    case SC_KB: {
      int k = in.key;
      if (k == 2) { kbR = (kbR + 3) % 4; dirty = true; }          // yukari
      if (k == 6) { kbR = (kbR + 1) % 4; dirty = true; }          // asagi
      if (k == 5) { kbC = (kbC + 9) % 10; dirty = true; }         // sol
      if (k == 7) { kbC = (kbC + 1) % 10; dirty = true; }         // sag
      if (in.next) { kbC++; if (kbC >= 10) { kbC = 0; kbR = (kbR + 1) % 4; } dirty = true; }
      if (in.sel) typeChar();                                     // A: harfi yaz
      if (k == 3 && kbText.length() < 400) { kbText += ' '; dirty = true; }
      if (k == 1 && kbText.length()) { kbText.remove(kbText.length() - 1); dirty = true; }
      if (k == 8) { kbLayer = (kbLayer == 1) ? 0 : 1; dirty = true; }   // shift
      if (k == 9) { kbLayer = (kbLayer == 2) ? 0 : 2; dirty = true; }   // semboller
      if (k == 10) { kbText = ""; dirty = true; }                       // hepsini sil
      if (k == 4) sendKb();                                            // gonder / kaydet
      if (in.back) { screen = (kbPurpose == KB_NOTE) ? SC_MENU : (kbPurpose == KB_WIFI ? SC_WIFI : SC_GEM_MENU); dirty = true; }
      break;
    }
    case SC_SETTINGS: {
      const int N = 7;
      if (nx) { setIdx = (setIdx + 1) % N; dirty = true; }
      if (pv) { setIdx = (setIdx + N - 1) % N; dirty = true; }
      if (ok) {
        switch (setIdx) {
          case 0: screen = SC_NET; break;
          case 1: toast("Taraniyor...", TFT_CYAN, 0); wifiScanList(); wifiIdx = 0; screen = SC_WIFI; break;
          case 2: toast("AP aciliyor...", TFT_CYAN, 0); startAP(); screen = SC_NET; break;
          case 3: keysHelp(); screen = SC_KEYS; break;
          case 4: {
            int cur = 0;
            for (int i = 0; i < 5; i++) if (BR_LEVELS[i] == brightness) cur = i;
            cur = (cur + 1) % 5;
            brightness = BR_LEVELS[cur];
            M5.Display.setBrightness(brightness);
            prefs.begin("cfg", false);
            prefs.putUChar("br", brightness);
            prefs.end();
            break;
          }
          case 5:                            // ekran yonu: 0 -> 1 -> 2 -> 3
            scrRot = (scrRot + 1) & 3;
            applyScreenRotation();
            prefs.begin("cfg", false);
            prefs.putUChar("rot", scrRot);
            prefs.end();
            break;
          case 6:                            // keypad yonu: 0 -> 90 -> 180 -> 270
            keyRot = (keyRot + 1) & 3;
            prefs.begin("cfg", false);
            prefs.putUChar("krot", keyRot);
            prefs.end();
            break;
        }
        dirty = true;
      }
      if (bk) { screen = SC_MENU; dirty = true; }
      break;
    }
    case SC_WIFI: {
      int N = (int)wifiNets.size() + 1;      // son oge: yeniden tara
      if (nx) { wifiIdx = (wifiIdx + 1) % N; dirty = true; }
      if (pv) { wifiIdx = (wifiIdx + N - 1) % N; dirty = true; }
      if (ok) {
        if (wifiIdx >= (int)wifiNets.size()) {
          toast("Taraniyor...", TFT_CYAN, 0);
          wifiScanList();
          wifiIdx = 0;
        } else if (wifiNets[wifiIdx].enc) {          // sifreli ag: klavyeyle sifre yaz
          wifiSel = wifiNets[wifiIdx].ssid;
          kbPurpose = KB_WIFI; kbText = ""; kbLayer = 0;
          screen = SC_KB;
        } else {                                     // acik ag: sifresiz baglan
          wifiSel = wifiNets[wifiIdx].ssid;
          wifiConnectFlow(wifiSel, "");
        }
        dirty = true;
      }
      if (bk) { screen = SC_SETTINGS; dirty = true; }
      break;
    }
    case SC_GAME: {
      if (gState == GS_READY) {
        if (in.next) { gDiff = (gDiff + 1) % 3; saveGameDiff(); dirty = true; }
        if (in.sel) startGame();
        if (in.back) { screen = SC_MENU; dirty = true; }
      } else if (gState == GS_OVER) {
        if (in.sel && millis() - gOverT > 700) startGame();
        if (in.next && millis() - gOverT > 400) { gState = GS_READY; gReadyCols = 0; dirty = true; }
        if (in.back && millis() - gOverT > 600) { screen = SC_MENU; dirty = true; }
      } else {
        if (in.physBHold) { gState = GS_READY; gReadyCols = 0; dirty = true; }   // oyun sirasinda sadece B butonu
      }
      break;
    }
    case SC_KEYS: {
      if (in.next) pageScroll();
      if (in.key == 6) scrollBy(1);
      if (in.key == 2) scrollBy(-1);
      if (in.back || in.key == 5) { screen = SC_SETTINGS; dirty = true; }
      break;
    }
    case SC_NET: {
      if (bk) { screen = SC_SETTINGS; dirty = true; }
      break;
    }
  }
}

// ---------------- SETUP / LOOP ----------------
void setup() {
  auto cfg = M5.config();
  cfg.internal_spk = false;              // mikrofon ile cakismasin
  M5.begin(cfg);
  prefs.begin("cfg", true);
  brightness = prefs.getUChar("br", 120);
  scrRot = prefs.getUChar("rot", SCREEN_ROTATION) & 3;
  keyRot = prefs.getUChar("krot", 0) & 3;
  gDiff = prefs.getUChar("gd", 1) % 3;
  gemSearch = prefs.getUChar("gs", 1) != 0;
  gemKey   = prefs.getString("gkey", "");
  gemModel = prefs.getString("gmodel", "");
  for (int i = 0; i < 3; i++) gBest[i] = prefs.getUShort(("hs" + String(i)).c_str(), 0);
  prefs.end();
  M5.Display.setBrightness(brightness);
  applyScreenRotation();
  keypadInit();

  fsOk = LittleFS.begin(true);
  if (!fsOk) { LittleFS.format(); fsOk = LittleFS.begin(false); }

  prefs.begin("net", true);
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  prefs.end();

  M5.update();
  bool forceAP = M5.BtnB.isPressed() || keypadScan() == 14;    // acilista B / S14 basili = AP modu
  bool connected = false;
  if (!forceAP && ssid.length()) {
    drawHeader("M5 Notlar");
    connected = connectSTA(ssid, pass, 15000);
  }
  if (connected) {
    configTzTime("<+03>-3", "pool.ntp.org", "time.google.com");
    if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);
    apMode = false;
  } else {
    startAP();
  }
  startWeb();
  screen = SC_NET;                       // acilista adresi / AP bilgisini goster
  dirty = true;
  if (!fsOk) toast("Dosya sistemi yok: notlar kucuk hafizada tutulacak (az sayida)", TFT_ORANGE, 2500);
  if (!psramFound()) toast("PSRAM kapali: ses calismaz", TFT_ORANGE, 1500);
}

void loop() {
  M5.update();
  netLoop();

  int rawKey = keypadEvent();
  bool k13 = (rawKey == 13);             // S13 = A butonu
  bool k14 = (rawKey == 14);             // S14 = B butonu

  // S14: kisa basis = B tiklama (sonraki), basili tutma = B basili (geri)
  static bool k14Active = false, k14Held = false;
  static uint32_t k14T = 0;
  bool k14Click = false, k14Back = false;
  if (keyStable == 14) {
    if (!k14Active) { k14Active = true; k14Held = false; k14T = millis(); }
    else if (!k14Held && millis() - k14T >= 500) { k14Held = true; k14Back = true; }
  } else if (k14Active) {
    k14Active = false;
    if (!k14Held) k14Click = true;
  }

  Input in;
  in.sel    = M5.BtnA.wasClicked() || k13;
  in.aPress = M5.BtnA.wasPressed() || k13;
  in.next   = M5.BtnB.wasClicked() || k14Click;
  in.back   = M5.BtnB.wasHold() || k14Back;
  in.key    = (k13 || k14) ? 0 : rawKey;
  in.physBHold = M5.BtnB.wasHold();
  handleInput(in);

  if (screen == SC_GAME) {
    if (gState == GS_PLAY) gameTick();
    else if (gState == GS_READY) gameIdle();
  }

  if (notesDirty) {
    notesDirty = false;
    if (screen == SC_NOTES) { loadNotes(); dirty = true; }
  }
  if (dirty) { dirty = false; render(); }

  if (screen != SC_GAME && millis() - lastHdr > 10000) {      // ust cubugu (pil / wifi) tazele
    lastHdr = millis();
    if (curTitle.length()) { drawHeader(curTitle); canvas.pushSprite(0, 0); }
  }
  delay(5);
}
