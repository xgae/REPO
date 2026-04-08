/* IoT Attendance System v4.1 - ESP8266
   (c) Fajer Abednabie - Basrah, Iraq
   Hardware: Wemos D1 + AS608 + LCD I2C 0x27

   v4.1: All bugs fixed + ntfy.sh push, manual entry, absent list,
         flash monitor, schedule API, PWA manifest, dirty-flag saves,
         JSON escaping, constant-time auth, 3x NTP fallback. */

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ESP8266HTTPClient.h>       // [NEW-24] ntfy.sh push notifications
#include <ArduinoOTA.h>
#include <SoftwareSerial.h>
#include <Adafruit_Fingerprint.h>
#include <U8g2lib.h>                 // [ARABIC-v4.2] OLED + Arabic fonts
#include <LittleFS.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>
#include "arabic.h"  // [ARABIC-v4.2] Bilingual strings

#ifdef LANG_ARABIC
  #define FONT u8g2_font_unifont_t_arabic // Arabic font
#else
  #define FONT u8g2_font_6x10_tf         // English fallback
#endif

// ============================================================
// CONFIGURATION
// ============================================================
const char* WIFI_SSID     = "Abd";
const char* WIFI_PASS     = "fajer67ii";
const char* ADMIN_PASS    = "fajer67student";
const char* OTA_PASSWORD  = "fajer67ota";
const char* MDNS_HOST     = "f-att";           // -> f-att.local
const long  TZ_OFFSET_S   = 3L * 3600L;        // UTC +3
// [FIX-27] Three NTP servers for fallback
const char* NTP1          = "pool.ntp.org";
const char* NTP2          = "time.cloudflare.com";
const char* NTP3          = "time.google.com";

#define FP_CONFIDENCE_THRESHOLD 50
#define SCAN_DEBOUNCE_SEC       3
#define SENSOR_HEALTH_INTERVAL_MS 300000UL
#define SENSOR_INIT_RETRIES     3
#define HEAP_CRITICAL_THRESHOLD 4096
#define AUTO_RESTART_HOUR       3
#define BUZZER_PIN D7
#define LOG_FLUSH_DEBOUNCE_MS   30000UL  // [FIX-28] flush logs at most every 30s
#define FLASH_WARN_PCT          80       // [NEW-26] warn when flash > 80% full

// Forward declarations
void playSuccessTone();
void playErrorTone();
void playLowConfidenceTone();
void saveRecentLogs();

// ============================================================
// HARDWARE INSTANCES
// ============================================================
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE, /* clock=*/ 5, /* data=*/ 4); // [ARABIC-v4.2] OLED I2C (D1=5, D2=4)
SoftwareSerial    mySerial(D6, D5);
Adafruit_Fingerprint finger(&mySerial);
ESP8266WebServer  server(80);

// ============================================================
// LCD CUSTOM CHARACTERS
// ============================================================
uint8_t CH_FINGER[8] = { 0x0E, 0x11, 0x15, 0x11, 0x0A, 0x04, 0x04, 0x00 };
uint8_t CH_CHECK [8] = { 0x00, 0x00, 0x01, 0x03, 0x16, 0x1C, 0x08, 0x00 };
uint8_t CH_CROSS [8] = { 0x11, 0x0A, 0x04, 0x04, 0x04, 0x0A, 0x11, 0x00 };
uint8_t CH_SP1   [8] = { 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x00 };
uint8_t CH_SP2   [8] = { 0x00, 0x10, 0x08, 0x04, 0x02, 0x01, 0x00, 0x00 };
uint8_t CH_BLOCK [8] = { 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F };
uint8_t CH_HALF  [8] = { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x00 };
uint8_t CH_DOT   [8] = { 0x00, 0x00, 0x00, 0x0E, 0x0E, 0x00, 0x00, 0x00 };

// ============================================================
// STUDENT DATABASE
// ============================================================
#define MAX_STUDENTS 127

struct Student {
  uint16_t template_id;
  char student_id [12];
  char first      [16];
  char last_name  [16];
};

Student students[MAX_STUDENTS];
int student_count = 0;

// ============================================================
// ATTENDANCE LOG
// ============================================================
struct LogEntry {
  char name    [33];
  char time_str[24];
  char action  [ 4];
  char lesson  [28];
};

#define MAX_LOG_ENTRIES 40
LogEntry logs[MAX_LOG_ENTRIES];
int  logCount    = 0;
int  today_scans = 0;

// [FIX-28] Dirty flag: set true on every change, flushed to flash periodically
bool          logs_dirty        = false;
unsigned long last_log_flush_ms = 0;

// ============================================================
// SCHEDULE DEFINITIONS
// Sunday (wday=0): Computer Maintenance - 3 sessions
// Monday (wday=1): Circuit Design + Networking
// ============================================================
struct ScheduleSlot {
  uint8_t     day_of_week;
  uint8_t     start_h, start_m;
  uint8_t     end_h,   end_m;
  const char* lesson_name;
};

const ScheduleSlot schedule[] = {
  { 0,  8,  0,  9,  0, "Comp Maint S1" },
  { 0,  9, 15, 10, 15, "Comp Maint S2" },
  { 0, 10, 30, 11, 30, "Comp Maint S3" },
  { 1,  8,  0,  9, 30, "Circuit Design" },
  { 1,  9, 45, 11, 15, "Networking" },
};
const int SCHEDULE_COUNT = sizeof(schedule) / sizeof(schedule[0]);

// ============================================================
// RUNTIME STATE
// ============================================================
bool   wifi_connected  = false;
bool   time_synced     = false;
bool   ota_enabled     = false;
char   last_sync_str[30] = "Never";

uint8_t lastAction[128];
char last_scanned_name[34] = "";
unsigned long lastScanTime[128];

bool sensor_available = false;

char current_day_stamp[10] = "00000000";

unsigned long last_idle_ms      = 0;
unsigned long last_ntp_ms       = 0;
unsigned long last_wifi_ms      = 0;
unsigned long last_health_ms    = 0;
unsigned long last_heap_check   = 0;
unsigned long last_save_time_ms = 0;
bool          auto_restarted_today = false;

// [NEW-24] ntfy.sh push notification URL (loaded from /ntfy_url.txt)
char ntfy_url[128] = "";

// ============================================================
// DEBUG
// ============================================================
void debugPrint(const char* msg) {
  Serial.println(msg);
  Serial.flush();
}

void debugPrintF(const char* fmt, ...) {
  char buf[128];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Serial.println(buf);
  Serial.flush();
}

// ============================================================
// [FIX-21] JSON string escape helper — prevents injection via
//          student names / lesson strings in API responses
// ============================================================
void jsonEscape(const char* src, char* dst, size_t dstLen) {
  size_t j = 0;
  for (size_t i = 0; src[i] && j + 2 < dstLen; i++) {
    char c = src[i];
    if (c == '"' || c == '\\') {
      if (j + 3 >= dstLen) break;
      dst[j++] = '\\';
    }
    dst[j++] = c;
  }
  dst[j] = '\0';
}

// [FIX-29] Constant-time password compare — prevents timing oracle attack
bool checkAdminPass(const String& provided) {
  const char* expected = ADMIN_PASS;
  size_t expLen = strlen(expected);
  if (provided.length() != expLen) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < expLen; i++)
    diff |= (uint8_t)provided[i] ^ (uint8_t)expected[i];
  return diff == 0;
}

// ============================================================
// TIME UTILITIES
// ============================================================
time_t getNow() { return time(nullptr); }

void formatISO_buf(time_t t, char* buf, size_t len) {
  if (t < 1000000000UL) { strncpy(buf, "N/A", len); return; }
  struct tm* ti = gmtime(&t);
  snprintf(buf, len, "%04d-%02d-%02dT%02d:%02d:%02dZ",
           ti->tm_year+1900, ti->tm_mon+1, ti->tm_mday,
           ti->tm_hour, ti->tm_min, ti->tm_sec);
}

void formatLocal_buf(time_t t, char* buf, size_t len) {
  if (t < 1000000000UL) { strncpy(buf, "N/A", len); return; }
  time_t local = t + TZ_OFFSET_S;
  struct tm* ti = gmtime(&local);
  int h = ti->tm_hour;
  const char* ap = (h >= 12) ? "PM" : "AM";
  if (h > 12) h -= 12;
  if (h == 0) h = 12;
  snprintf(buf, len, "%02d/%02d/%04d %02d:%02d:%02d %s",
           ti->tm_mon+1, ti->tm_mday, ti->tm_year+1900,
           h, ti->tm_min, ti->tm_sec, ap);
}

void formatLCDTime_buf(time_t t, char* buf, size_t len) {
  if (t < 1000000000UL) { strncpy(buf, "--:-- --", len); return; }
  time_t local = t + TZ_OFFSET_S;
  struct tm* ti = gmtime(&local);
  int h = ti->tm_hour;
  const char* ap = (h >= 12) ? "PM" : "AM";
  if (h > 12) h -= 12;
  if (h == 0) h = 12;
  snprintf(buf, len, "%02d:%02d %s", h, ti->tm_min, ap);
}

void formatLCDDate_buf(time_t t, char* buf, size_t len) {
  if (t < 1000000000UL) { strncpy(buf, "No date yet", len); return; }
  time_t local = t + TZ_OFFSET_S;
  struct tm* ti = gmtime(&local);
  const char* days[]   = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
  const char* months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                           "Jul","Aug","Sep","Oct","Nov","Dec"};
  snprintf(buf, len, "%s %s %02d %04d",
           days[ti->tm_wday], months[ti->tm_mon],
           ti->tm_mday, ti->tm_year+1900);
}

void datestamp_buf(time_t t, char* buf, size_t len) {
  if (t < 1000000000UL) { strncpy(buf, "00000000", len); return; }
  time_t local = t + TZ_OFFSET_S;
  struct tm* ti = gmtime(&local);
  snprintf(buf, len, "%04d%02d%02d",
           ti->tm_year+1900, ti->tm_mon+1, ti->tm_mday);
}

const char* getCurrentLesson(time_t t) {
  if (t < 1000000000UL) return NULL;
  time_t local = t + TZ_OFFSET_S;
  struct tm* ti = gmtime(&local);
  int wday = ti->tm_wday;
  int cur_min = ti->tm_hour * 60 + ti->tm_min;
  for (int i = 0; i < SCHEDULE_COUNT; i++) {
    if (schedule[i].day_of_week == wday) {
      int start = schedule[i].start_h * 60 + schedule[i].start_m;
      int end   = schedule[i].end_h   * 60 + schedule[i].end_m;
      if (cur_min >= start && cur_min < end) return schedule[i].lesson_name;
    }
  }
  return NULL;
}

// ============================================================
// CRC32
// ============================================================
static void crc32_update(uint32_t& crc, const void* data, size_t len) {
  const uint8_t* p = (const uint8_t*)data;
  for (size_t i = 0; i < len; i++) {
    crc ^= p[i];
    for (int j = 0; j < 8; j++)
      crc = (crc >> 1) ^ (0xEDB88320u * (crc & 1));
  }
}

// ============================================================
// NTP persistence
// ============================================================
void saveLastTime() {
  time_t t = getNow();
  if (t < 1000000000UL) return;
  File f = LittleFS.open("/last_time.txt", "w");
  if (!f) return;
  char buf[25];
  formatISO_buf(t, buf, sizeof(buf));
  f.println(buf);
  f.close();
}

bool loadLastTime() {
  if (!LittleFS.exists("/last_time.txt")) return false;
  File f = LittleFS.open("/last_time.txt", "r");
  if (!f) return false;
  String line = f.readStringUntil('\n');
  f.close();
  line.trim();
  if (line.length() < 19) return false;
  struct tm ti;
  memset(&ti, 0, sizeof(ti));
  ti.tm_year = line.substring( 0, 4).toInt() - 1900;
  ti.tm_mon  = line.substring( 5, 7).toInt() - 1;
  ti.tm_mday = line.substring( 8,10).toInt();
  ti.tm_hour = line.substring(11,13).toInt();
  ti.tm_min  = line.substring(14,16).toInt();
  ti.tm_sec  = line.substring(17,19).toInt();
  time_t t = mktime(&ti);
  if (t < 1000000000UL) return false;
  struct timeval tv = { t, 0 };
  settimeofday(&tv, nullptr);
  debugPrint("[TIME] Loaded from file");
  return true;
}

// ============================================================
// IN/OUT state persistence
// ============================================================
void saveLastAction() {
  File f = LittleFS.open("/last_action.bin", "w");
  if (!f) return;
  f.write(lastAction, 128);
  f.close();
}

void loadLastAction() {
  if (!LittleFS.exists("/last_action.bin")) return;
  File f = LittleFS.open("/last_action.bin", "r");
  if (!f) return;
  if (f.size() == 128) {
    f.read(lastAction, 128);
    debugPrint("[STATE] Loaded IN/OUT state");
  }
  f.close();
}

void clearLastActionForNewDay() {
  memset(lastAction, 0, sizeof(lastAction));
  if (LittleFS.exists("/last_action.bin"))
    LittleFS.remove("/last_action.bin");
  debugPrint("[STATE] New day - cleared IN/OUT");
}

// ============================================================
// Persistent log save/load (CRC32 integrity)
// ============================================================
void saveRecentLogs() {
  File f = LittleFS.open("/recent_log.dat", "w");
  if (!f) return;
  uint32_t crc = 0xFFFFFFFF;

  f.write((uint8_t*)&logCount, sizeof(logCount));
  crc32_update(crc, &logCount, sizeof(logCount));

  f.write((uint8_t*)&today_scans, sizeof(today_scans));
  crc32_update(crc, &today_scans, sizeof(today_scans));

  f.write((uint8_t*)current_day_stamp, 10);
  crc32_update(crc, current_day_stamp, 10);

  for (int i = 0; i < logCount && i < MAX_LOG_ENTRIES; i++) {
    f.write((uint8_t*)&logs[i], sizeof(LogEntry));
    crc32_update(crc, &logs[i], sizeof(LogEntry));
    yield();
  }

  crc = ~crc;
  f.write((uint8_t*)&crc, sizeof(crc));
  f.close();

  // [FIX-28] Reset dirty state after successful flush
  logs_dirty = false;
  last_log_flush_ms = millis();
}

void loadRecentLogs() {
  if (!LittleFS.exists("/recent_log.dat")) return;
  File f = LittleFS.open("/recent_log.dat", "r");
  if (!f) return;

  int saved_count = 0, saved_scans = 0;
  char saved_day[10] = {};
  uint32_t crc = 0xFFFFFFFF;

  if (f.read((uint8_t*)&saved_count, sizeof(saved_count)) != sizeof(saved_count)) { f.close(); return; }
  crc32_update(crc, &saved_count, sizeof(saved_count));
  if (f.read((uint8_t*)&saved_scans, sizeof(saved_scans)) != sizeof(saved_scans)) { f.close(); return; }
  crc32_update(crc, &saved_scans, sizeof(saved_scans));
  f.read((uint8_t*)saved_day, 10);
  crc32_update(crc, saved_day, 10);

  if (strcmp(saved_day, current_day_stamp) != 0) {
    debugPrint("[LOG] Different day - ignoring");
    f.close(); return;
  }
  if (saved_count < 0 || saved_count > MAX_LOG_ENTRIES) { f.close(); return; }

  int loaded = 0;
  for (int i = 0; i < saved_count; i++) {
    if (f.read((uint8_t*)&logs[i], sizeof(LogEntry)) != sizeof(LogEntry)) break;
    crc32_update(crc, &logs[i], sizeof(LogEntry));
    loaded++;
    yield();
  }

  crc = ~crc;
  uint32_t stored_crc = 0;
  if (f.available() >= (int)sizeof(stored_crc)) {
    f.read((uint8_t*)&stored_crc, sizeof(stored_crc));
    if (stored_crc != crc) {
      debugPrint("[LOG] CRC mismatch - discarding corrupt data");
      memset(logs, 0, loaded * sizeof(LogEntry));
      f.close(); return;
    }
    debugPrintF("[LOG] Restored %d entries (CRC OK)", loaded);
  } else {
    debugPrintF("[LOG] Restored %d entries (legacy format)", loaded);
  }

  logCount    = loaded;
  today_scans = saved_scans;
  f.close();
}

// ============================================================
// [NEW-26] Flash capacity helper
// ============================================================
bool flashHasSpace(size_t needed = 512) {
  FSInfo fs_info;
  if (!LittleFS.info(fs_info)) return true; // assume OK if can't read
  return (fs_info.totalBytes - fs_info.usedBytes) > needed;
}

uint8_t flashUsedPct() {
  FSInfo fs_info;
  if (!LittleFS.info(fs_info)) return 0;
  return (uint8_t)((fs_info.usedBytes * 100UL) / fs_info.totalBytes);
}

// ============================================================
// LITTLEFS HELPERS
// ============================================================
bool initFS() {
  if (!LittleFS.begin()) {
    debugPrint("[FS] Mount failed - formatting...");
    LittleFS.format();
    if (!LittleFS.begin()) {
      debugPrint("[FS] Format FAILED!");
      return false;
    }
  }
  debugPrint("[FS] OK");
  return true;
}

void loadStudentsCSV() {
  student_count = 0;
  if (!LittleFS.exists("/students.csv")) {
    debugPrint("[CSV] students.csv not found");
    return;
  }
  File f = LittleFS.open("/students.csv", "r");
  if (!f) return;
  f.readStringUntil('\n');

  while (f.available() && student_count < MAX_STUDENTS) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    yield();

    String fields[4];
    int fieldIdx = 0, start = 0;
    for (int i = 0; i <= (int)line.length() && fieldIdx < 4; i++) {
      if (i == (int)line.length() || line[i] == ',') {
        fields[fieldIdx++] = line.substring(start, i);
        start = i + 1;
      }
    }
    if (fieldIdx < 3) continue;

    Student& s = students[student_count];
    memset(&s, 0, sizeof(Student));
    s.template_id = (uint16_t)fields[0].toInt();
    strncpy(s.student_id, fields[1].c_str(), sizeof(s.student_id)-1);
    strncpy(s.first,      fields[2].c_str(), sizeof(s.first)-1);
    if (fieldIdx > 3)
      strncpy(s.last_name, fields[3].c_str(), sizeof(s.last_name)-1);
    student_count++;
  }
  f.close();
  debugPrintF("[CSV] Loaded %d students", student_count);
}

int findStudent(uint16_t tid) {
  for (int i = 0; i < student_count; i++)
    if (students[i].template_id == tid) return i;
  return -1;
}

void rewriteStudentsCSV() {
  File fh = LittleFS.open("/students.csv", "w");
  if (!fh) return;
  fh.println(F("template_id,student_id,first_name,last_name"));
  for (int i = 0; i < student_count; i++) {
    fh.printf("%d,%s,%s,%s\n",
              students[i].template_id,
              students[i].student_id,
              students[i].first,
              students[i].last_name);
    yield();
  }
  fh.close();
}

bool addStudentToCSV(uint16_t tid, const char* sid,
                     const char* first, const char* last) {
  // [NEW-26] Guard against full flash before writing
  if (!flashHasSpace(256)) {
    debugPrint("[CSV] Flash full - cannot add student");
    return false;
  }
  if (!LittleFS.exists("/students.csv")) {
    File fh = LittleFS.open("/students.csv", "w");
    if (!fh) return false;
    fh.println(F("template_id,student_id,first_name,last_name"));
    fh.close();
  }
  File f = LittleFS.open("/students.csv", "a");
  if (!f) return false;
  f.printf("%d,%s,%s,%s\n", tid, sid, first, last);
  f.close();

  if (student_count < MAX_STUDENTS) {
    Student& s = students[student_count];
    memset(&s, 0, sizeof(Student));
    s.template_id = tid;
    strncpy(s.student_id, sid,   sizeof(s.student_id)-1);
    strncpy(s.first,      first, sizeof(s.first)-1);
    strncpy(s.last_name,  last,  sizeof(s.last_name)-1);
    student_count++;
  }
  return true;
}

bool deleteStudentFromCSV(uint16_t tid) {
  int idx = findStudent(tid);
  if (idx < 0) return false;
  for (int i = idx; i < student_count - 1; i++)
    students[i] = students[i+1];
  student_count--;
  rewriteStudentsCSV();
  return true;
}

void appendAttendance(const Student& s, const char* action, time_t utc_t,
                      const char* lesson) {
  // [NEW-26] Guard against full flash
  if (!flashHasSpace(256)) {
    debugPrint("[ATT] Flash full - attendance NOT written");
    return;
  }
  char ds[10];
  datestamp_buf(utc_t, ds, sizeof(ds));
  char fname[32];
  snprintf(fname, sizeof(fname), "/att_%s.csv", ds);
  bool exists = LittleFS.exists(fname);
  File f = LittleFS.open(fname, "a");
  if (!f) { debugPrint("[CSV] Cannot open attendance file"); return; }
  if (!exists)
    f.println(F("student_id,first_name,last_name,action,timestamp_local,lesson"));

  char local_str[30];
  formatLocal_buf(utc_t, local_str, sizeof(local_str));
  f.printf("%s,%s,%s,%s,%s,%s\n",
           s.student_id, s.first, s.last_name,
           action, local_str,
           lesson ? lesson : "General");
  f.close();
}

void appendUnknown(time_t utc_t) {
  // [FIX-23] Skip if time is not valid yet
  if (utc_t < 1000000000UL) {
    debugPrint("[UNK] Time not synced - skipping unknown log");
    return;
  }
  if (!flashHasSpace(128)) return;
  bool exists = LittleFS.exists("/unknowns.csv");
  File f = LittleFS.open("/unknowns.csv", "a");
  if (!f) return;
  if (!exists) f.println(F("timestamp_local"));
  char local_str[30];
  formatLocal_buf(utc_t, local_str, sizeof(local_str));
  f.println(local_str);
  f.close();
}

// [FIX-20] Collect filenames first, then delete — safe LittleFS pattern
void clearAllAttendanceData() {
  String toDelete[24];
  int deleteCount = 0;
  Dir dir = LittleFS.openDir("/");
  while (dir.next() && deleteCount < 24) {
    String fn = dir.fileName();
    if (fn.startsWith("att_") || fn == "unknowns.csv")
      toDelete[deleteCount++] = fn;
    yield();
  }
  for (int i = 0; i < deleteCount; i++) {
    LittleFS.remove("/" + toDelete[i]);
    yield();
  }

  logCount = 0;
  today_scans = 0;
  memset(logs, 0, sizeof(logs));
  if (LittleFS.exists("/recent_log.dat")) LittleFS.remove("/recent_log.dat");
  clearLastActionForNewDay();
  last_scanned_name[0] = '\0';
  logs_dirty = false;
  debugPrint("[CLEAR] All attendance data cleared");
}

// ============================================================
// [NEW-24] ntfy.sh push notifications
// ============================================================
void loadNtfyURL() {
  if (!LittleFS.exists("/ntfy_url.txt")) return;
  File f = LittleFS.open("/ntfy_url.txt", "r");
  if (!f) return;
  String line = f.readStringUntil('\n');
  f.close();
  line.trim();
  strncpy(ntfy_url, line.c_str(), sizeof(ntfy_url)-1);
  debugPrintF("[NTFY] URL loaded: %s", ntfy_url);
}

// Non-blocking-ish push: 2s timeout so main loop isn't stuck
void sendNtfyNotification(const char* title, const char* body) {
  if (strlen(ntfy_url) == 0 || !wifi_connected) return;
  WiFiClient client;
  HTTPClient http;
  if (!http.begin(client, ntfy_url)) return;
  http.setTimeout(2000);
  http.addHeader("Title", title);
  http.addHeader("Content-Type", "text/plain; charset=utf-8");
  http.addHeader("Priority", "default");
  int code = http.POST(body);
  http.end();
  debugPrintF("[NTFY] POST -> %d", code);
}

// ============================================================
// LCD UTILITIES & ANIMATIONS
// ============================================================
void u8g2Init() {
  // [ARABIC-v4.2] U8g2 OLED init - auto-handles I2C address 0x3C/0x3D
  u8g2.begin();
  u8g2.setFont(FONT);
  u8g2.setFontMode(0);  // Transparent mode
  // U8g2 colors set via setDrawColor (1=white/fill, 0=transparent/clear)
  
  // Custom icons will be ported to bitmaps in display functions
  debugPrint("[OLED] U8g2 initialized with Arabic font");
}

void u8g2Center(int y, const char* text) {
  // [ARABIC-v4.2] U8g2 center text (128px width, y pixel position)
  u8g2.clearBuffer();
  int len = strlen(text);
  u8g2.setFont(FONT);
  int w = u8g2.getStrWidth(text);
  int x = (128 - w) / 2;
  u8g2.drawStr(x, y, text);
  u8g2.sendBuffer();
}

void u8g2Pad(int y, int x, const char* text) {
  // [ARABIC-v4.2] U8g2 pad text (x,y pixel, pad to 128px width)
  u8g2.clearBuffer();
  u8g2.setFont(FONT);
  u8g2.setDrawColor(1); // White
  u8g2.drawStr(x, y, text);
  u8g2.sendBuffer();
}

void showBootAnimation() {
  for (int step = 0; step <= 8; step++) {
    lcd.clear();
    lcdCenter(0, "* Attendance Sys *");
    lcd.setCursor(1, 1);
    lcd.print("[");
    for (int i = 0; i < 8; i++) {
      if (i < step) lcd.write(byte(5));
      else          lcd.print(' ');
    }
    lcd.print("]");
    lcd.setCursor(0, 2);
    lcd.write(byte((step % 2) ? 3 : 4));
    lcd.print(" Loading");
    for (int d = 0; d < (step % 4); d++) lcd.print('.');
    lcdCenter(3, "(c) F.Abednabie");
    delay(280);
    yield();
  }
  lcd.clear();
  lcdCenter(0, "* Attendance Sys *");
  lcd.setCursor(1, 1);
  lcd.print("[");
  for (int i = 0; i < 8; i++) lcd.write(byte(5));
  lcd.print("]");
  lcd.setCursor(0, 2);
  lcd.write(byte(1));
  lcdCenter(2, " System Ready!");
  lcdCenter(3, wifi_connected ? "f-att.local" : "No WiFi");
  delay(1500);
  lcd.clear();
}

void showSuccessWithFade(const char* name, const char* action,
                         const char* timeStr, const char* lesson) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.write(byte(0)); lcd.print(" "); lcd.write(byte(1));
  lcd.print(" Matched!");

  char displayName[19];
  strncpy(displayName, name, 18);
  displayName[18] = '\0';

  char buf[21];
  snprintf(buf, sizeof(buf), " %s", displayName);
  lcdPad(1, 0, buf);

  if (lesson && strlen(lesson) > 0) {
    snprintf(buf, sizeof(buf), " %s|%s", action, lesson);
  } else {
    snprintf(buf, sizeof(buf), " Action: %s", action);
  }
  lcdPad(2, 0, buf);
  snprintf(buf, sizeof(buf), " %s", timeStr);
  lcdPad(3, 0, buf);

  delay(1200);

  int nameLen = strlen(displayName);
  for (int i = nameLen; i >= 0; i--) {
    lcd.setCursor(1, 1);
    for (int j = 0; j < 18; j++) {
      if (j < i) lcd.print(displayName[j]);
      else lcd.print(' ');
    }
    delay(80);
    yield();
  }
  delay(300);
  lcd.clear();
}

void showUnknownAnimation() {
  for (int i = 0; i < 3; i++) {
    lcd.clear();
    lcdCenter(0, "!! NOT FOUND !!");
    lcd.setCursor(8, 1); lcd.write(byte(2));
    lcd.setCursor(9, 1); lcd.write(byte(2));
    lcdCenter(2, "Finger not enrolled");
    delay(300);
    lcd.clear();
    delay(200);
    yield();
  }
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.write(byte(2)); lcd.write(byte(2));
  lcd.print("  UNKNOWN FINGER  ");
  lcd.write(byte(2)); lcd.write(byte(2));
  lcdCenter(1, "Not in database");
  lcdCenter(2, "Ask admin to enroll");
  lcdCenter(3, wifi_connected ? "f-att.local" : "No WiFi");
  delay(2000);
  lcd.clear();
}

void showLowConfidenceAnimation(uint16_t confidence) {
  lcd.clear();
  lcdCenter(0, "!! WEAK SCAN !!");
  lcd.setCursor(0, 1); lcd.write(byte(0)); lcd.print(" "); lcd.write(byte(2));
  lcdCenter(1, "   Try again");
  char buf[21];
  snprintf(buf, sizeof(buf), " Score: %d", confidence);
  lcdPad(2, 0, buf);
  lcdCenter(3, "Press firmly & clean");
  delay(1500);
  lcd.clear();
}

// [FIX-22] Show diagnostic when sensor has fingerprint but CSV doesn't
void showOrphanAnimation(uint16_t id) {
  lcd.clear();
  lcdCenter(0, "!! ORPHAN SCAN !!");
  char idbuf[21];
  snprintf(idbuf, sizeof(idbuf), "Sensor ID: %d", id);
  lcdCenter(1, idbuf);
  lcdCenter(2, "No student record!");
  lcdCenter(3, "Re-enroll to fix");
  delay(2000);
  lcd.clear();
}

void updateIdleDisplay() {
  if (millis() - last_idle_ms < 1000) return;
  last_idle_ms = millis();

  time_t t = getNow();

  char hdr[21];
  if (!sensor_available)
    strncpy(hdr, "!SENSOR OFFLINE!", sizeof(hdr));
  else if (wifi_connected)
    snprintf(hdr, sizeof(hdr), "WiFi%c Attendance", (char)1);
  else
    strncpy(hdr, "[NoWiFi] Attend.", sizeof(hdr));
  lcdPad(0, 0, hdr);

  char time_buf[12];
  formatLCDTime_buf(t, time_buf, sizeof(time_buf));
  char row1[21];
  snprintf(row1, sizeof(row1), "  %s", time_buf);
  lcdPad(1, 0, row1);

  const char* lesson = getCurrentLesson(t);
  if (lesson) {
    char lbuf[21];
    snprintf(lbuf, sizeof(lbuf), ">> %s", lesson);
    lcdPad(2, 0, lbuf);
  } else {
    char date_buf[20];
    formatLCDDate_buf(t, date_buf, sizeof(date_buf));
    lcdPad(2, 0, date_buf);
  }

  if (last_scanned_name[0] != '\0') {
    char s[21];
    char trimmed[19];
    strncpy(trimmed, last_scanned_name, 18);
    trimmed[18] = '\0';
    snprintf(s, sizeof(s), "%c %s", (char)7, trimmed);
    lcdPad(3, 0, s);
  } else {
    lcdPad(3, 0, sensor_available ? "Place finger to scan" : "Sensor offline!");
  }
}

// ============================================================
// SENSOR MANAGEMENT
// ============================================================
bool initSensor() {
  finger.begin(57600);
  delay(150);
  for (int attempt = 1; attempt <= SENSOR_INIT_RETRIES; attempt++) {
    debugPrintF("[AS608] Init attempt %d/%d", attempt, SENSOR_INIT_RETRIES);
    if (finger.verifyPassword()) {
      finger.getTemplateCount();
      debugPrintF("[AS608] OK - %d templates", finger.templateCount);
      sensor_available = true;
      return true;
    }
    debugPrint("[AS608] No response, retrying...");
    delay(500);
    yield();
  }
  debugPrint("[AS608] FAILED all attempts");
  sensor_available = false;
  return false;
}

void checkSensorHealth() {
  if (millis() - last_health_ms < SENSOR_HEALTH_INTERVAL_MS) return;
  last_health_ms = millis();
  if (finger.verifyPassword()) {
    if (!sensor_available) {
      debugPrint("[AS608] Sensor recovered!");
      sensor_available = true;
    }
  } else {
    debugPrint("[AS608] Health check FAILED - re-init");
    sensor_available = false;
    initSensor();
  }
}

// ============================================================
// FINGERPRINT SCAN
// ============================================================
uint16_t getFingerprintID() {
  if (!sensor_available) return 0;
  if (finger.getImage()  != FINGERPRINT_OK) return 0;
  if (finger.image2Tz()  != FINGERPRINT_OK) return 0;

  uint8_t p = finger.fingerFastSearch();

  if (p == FINGERPRINT_NOTFOUND) {
    debugPrint("[AS608] No match found");
    // [FIX-23] Only log if time is valid
    appendUnknown(getNow());
    playErrorTone();
    showUnknownAnimation();
    return 0;
  }

  if (p != FINGERPRINT_OK) return 0;

  if (finger.confidence < FP_CONFIDENCE_THRESHOLD) {
    debugPrintF("[AS608] Low confidence: %d", finger.confidence);
    playLowConfidenceTone();
    showLowConfidenceAnimation(finger.confidence);
    return 0;
  }

  debugPrintF("[AS608] Match ID=%d conf=%d", finger.fingerID, finger.confidence);
  return finger.fingerID;
}

bool deleteFingerprint(uint16_t id) {
  if (!sensor_available) return false;
  uint8_t p = finger.deleteModel(id);
  if (p == FINGERPRINT_OK) {
    debugPrintF("[AS608] Deleted template %d", id);
    return true;
  }
  debugPrintF("[AS608] Delete failed code=%d", p);
  return false;
}

// ============================================================
// HEAP & STABILITY MONITORING
// ============================================================
void checkHeapHealth() {
  if (millis() - last_heap_check < 60000UL) return;
  last_heap_check = millis();
  uint32_t freeHeap = ESP.getFreeHeap();
  debugPrintF("[HEAP] Free: %u bytes", freeHeap);
  if (freeHeap < HEAP_CRITICAL_THRESHOLD) {
    debugPrint("[HEAP] CRITICAL - saving state and restarting");
    saveLastTime();
    saveLastAction();
    saveRecentLogs();
    delay(100);
    ESP.restart();
  }
}

void checkScheduledRestart() {
  time_t t = getNow();
  if (t < 1000000000UL) return;
  time_t local = t + TZ_OFFSET_S;
  struct tm* ti = gmtime(&local);
  if (ti->tm_hour == AUTO_RESTART_HOUR && ti->tm_min == 0 && !auto_restarted_today) {
    if (millis() > 43200000UL) {
      auto_restarted_today = true;
      debugPrint("[AUTO] Scheduled 3AM restart");
      saveLastTime();
      saveLastAction();
      saveRecentLogs();
      lcd.clear();
      lcdCenter(0, "Auto Maintenance");
      lcdCenter(1, "Restarting...");
      lcdCenter(2, "Data saved safely");
      delay(2000);
      ESP.restart();
    }
  }
  if (ti->tm_hour != AUTO_RESTART_HOUR) auto_restarted_today = false;
}

// ============================================================
// [NEW-27] PWA Manifest
// ============================================================
void handleManifest() {
  server.sendHeader("Cache-Control", "max-age=3600");
  server.send(200, "application/manifest+json",
    "{"
    "\"name\":\"IoT Attendance System\","
    "\"short_name\":\"Attendance\","
    "\"start_url\":\"/\","
    "\"display\":\"standalone\","
    "\"background_color\":\"#071028\","
    "\"theme_color\":\"#0a84ff\","
    "\"description\":\"ESP8266 fingerprint attendance tracker\","
    "\"icons\":[]"
    "}");
}

// ============================================================
// WEB UI HTML
// ============================================================
void sendHTML() {
  server.sendHeader("Cache-Control", "no-cache");
  server.sendHeader("Connection", "close");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html; charset=utf-8", "");

  // Part 1: Head + CSS
  server.sendContent_P(PSTR(
    "<!DOCTYPE html><html><head>"
    "<meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>IoT Attendance</title>"
    // [NEW-27] PWA manifest link
    "<link rel=manifest href=/manifest.json>"
    "<meta name=theme-color content='#0a84ff'>"
    "<style>"
    ":root{"
      "--bg:#071028;--surf:#0e1b2a;--acc:#0a84ff;"
      "--txt:#e6eef8;--muted:#93abcf;"
      "--ok:#30d158;--err:#ff453a;--warn:#ffd60a;--r:12px"
    "}"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{font-family:Inter,system-ui,sans-serif;"
         "background:var(--bg);color:var(--txt);min-height:100vh}"
    ".w{max-width:960px;margin:0 auto;padding:16px}"
    ".hdr{display:flex;justify-content:space-between;align-items:center;"
         "padding:12px 0;border-bottom:1px solid rgba(255,255,255,.06)}"
    "h1{font-size:18px;font-weight:600}"
    ".sub{font-size:13px;color:var(--muted)}"
    ".card{background:linear-gradient(135deg,"
             "rgba(255,255,255,.03),rgba(0,0,0,.1));"
           "border:1px solid rgba(255,255,255,.06);"
           "border-radius:var(--r);padding:16px;margin-top:12px}"
    "h2{font-size:15px;margin-bottom:12px}"
    ".row{display:flex;gap:10px;flex-wrap:wrap}"
    ".stat{flex:1;min-width:80px;"
          "background:rgba(10,132,255,.08);"
          "border:1px solid rgba(10,132,255,.2);"
          "border-radius:10px;padding:10px;text-align:center}"
    ".stat-in{background:rgba(48,209,88,.08);border-color:rgba(48,209,88,.2)}"
    ".stat-in .val{color:var(--ok)}"
    ".stat-out{background:rgba(255,69,58,.08);border-color:rgba(255,69,58,.2)}"
    ".stat-out .val{color:var(--err)}"
    ".stat-warn{background:rgba(255,214,10,.08);border-color:rgba(255,214,10,.2)}"
    ".stat-warn .val{color:var(--warn)}"
    ".val{font-size:20px;font-weight:700;color:var(--acc)}"
    ".lbl{font-size:11px;color:var(--muted)}"
    "button{background:var(--acc);color:#fff;border:none;"
            "border-radius:10px;padding:8px 14px;cursor:pointer;font-size:13px;"
            "transition:transform .1s}"
    "button:active{transform:scale(.96)}"
    ".btn-g{background:var(--ok)}"
    ".btn-r{background:var(--err)}"
    ".btn-w{background:var(--warn);color:#000}"
    ".btn-c{background:rgba(255,255,255,.1)}"
    ".btn-s{padding:5px 10px;font-size:12px}"
    "table{width:100%;border-collapse:collapse;margin-top:8px}"
    "th,td{padding:8px 10px;text-align:left;font-size:13px;"
          "border-bottom:1px solid rgba(255,255,255,.04)}"
    "th{color:var(--muted);font-weight:500}"
    ".in{background:rgba(48,209,88,.15);color:var(--ok);"
        "padding:2px 8px;border-radius:20px;font-size:11px;font-weight:600}"
    ".out{background:rgba(255,69,58,.12);color:var(--err);"
         "padding:2px 8px;border-radius:20px;font-size:11px;font-weight:600}"
    ".manual-badge{background:rgba(255,214,10,.12);color:var(--warn);"
                  "padding:2px 8px;border-radius:20px;font-size:11px;font-weight:600}"
    "input,select{background:rgba(255,255,255,.06);"
           "border:1px solid rgba(255,255,255,.12);"
           "border-radius:8px;padding:7px 10px;color:var(--txt);"
           "font-size:13px;width:100%}"
    "select option{background:#0e1b2a}"
    ".g2{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-bottom:8px}"
    ".msg{margin-top:8px;font-size:13px;padding:8px;border-radius:8px}"
    ".ok-msg{background:rgba(48,209,88,.1);color:var(--ok)}"
    ".err-msg{background:rgba(255,69,58,.1);color:var(--err)}"
    ".pill{display:inline-block;padding:2px 8px;border-radius:20px;font-size:11px;"
          "background:rgba(10,132,255,.15);color:var(--acc)}"
    ".footer{margin-top:12px;color:var(--muted);font-size:12px;text-align:center}"
    ".sched{background:rgba(10,132,255,.05);border:1px solid rgba(10,132,255,.15);"
           "border-radius:8px;padding:8px 12px;margin-top:8px;font-size:12px}"
    ".sched-active{background:rgba(48,209,88,.1);border-color:rgba(48,209,88,.3)}"
    ".lesson-badge{background:rgba(255,214,10,.15);color:var(--warn);"
          "padding:3px 10px;border-radius:20px;font-size:12px;font-weight:600;"
          "display:inline-block;margin-top:4px}"
    ".absent-row td{color:var(--err)}"
    ".flash-bar{height:4px;border-radius:2px;margin-top:6px;"
               "background:rgba(255,255,255,.1);overflow:hidden}"
    ".flash-fill{height:100%;border-radius:2px;transition:width .5s;"
                "background:var(--ok)}"
    ".flash-fill.warn{background:var(--warn)}"
    ".flash-fill.crit{background:var(--err)}"
    ".ee{color:transparent;cursor:default;transition:color .3s}"
    ".ee:hover,.ee:active{color:var(--acc)}"
    ".dp-link{display:block;padding:7px 12px;border-radius:8px;color:var(--acc);"
             "text-decoration:none;font-size:13px;margin:3px 0;"
             "background:rgba(10,132,255,.06);border:1px solid rgba(10,132,255,.12)}"
    ".dp-link:hover{background:rgba(10,132,255,.12)}"
    ".dp-today{color:var(--ok);background:rgba(48,209,88,.07);"
              "border-color:rgba(48,209,88,.18)}"
    ".rtick{font-size:11px;color:rgba(147,171,207,.5);font-weight:400;margin-left:6px}"
    "@keyframes fadeIn{from{opacity:0;transform:translateY(-8px)}to{opacity:1;transform:none}}"
    ".fade-row{animation:fadeIn .4s ease-out}"
    "</style></head><body><div class=w>"
  ));

  yield();

  // Part 2: Header
  server.sendContent_P(PSTR(
    "<div class=hdr>"
      "<div>"
        "<h1>\xF0\x9F\x91\xA8\xF0\x9F\x8F\xBB\xE2\x80\x8D\xF0\x9F\x8E\x93 IoT Attendance System</h1>"
        "<div class=sub>Iraq/Basrah &bull; f-att.local</div>"
      "</div>"
      "<div style='display:flex;gap:6px;flex-wrap:wrap'>"
        "<button class='btn-s' onclick=showDatePicker()>&#8595; CSV</button>"
        "<button class='btn-s btn-g' onclick=showEnroll()>+ Enroll</button>"
        "<button class='btn-s btn-r' onclick=showDelete()>&#128465; Delete</button>"
        "<button class='btn-s btn-w' onclick=showClear()>&#128465; Clear Logs</button>"
        // [NEW-25] Manual entry button
        "<button class='btn-s' style='background:rgba(255,214,10,.2);color:var(--warn)' onclick=showManual()>&#9997; Manual</button>"
        // [NEW-24] Settings button for ntfy URL
        "<button class='btn-s btn-c' onclick=showSettings()>&#9881;</button>"
      "</div>"
    "</div>"
  ));

  // Part 3: Stats - including Flash used [NEW-26]
  server.sendContent_P(PSTR(
    "<div class=card><div class=row>"
      "<div class=stat><div class=val id=sc>-</div><div class=lbl>Today Scans</div></div>"
      "<div class=stat><div class=val id=up>-</div><div class=lbl>Uptime</div></div>"
      "<div class=stat><div class=val id=ts>-</div><div class=lbl>NTP Sync</div></div>"
      "<div class=stat><div class=val id=st>-</div><div class=lbl>Students</div></div>"
      "<div class=stat><div class=val id=sn>-</div><div class=lbl>Sensor</div></div>"
      "<div class=stat><div class=val id=hp>-</div><div class=lbl>Free RAM</div></div>"
      "<div class='stat stat-in'><div class=val id=pIn>-</div><div class=lbl>Present</div></div>"
      "<div class='stat stat-out'><div class=val id=pOut>-</div><div class=lbl>Left</div></div>"
      // [NEW-26] Flash usage stat
      "<div class='stat' id=flashStat>"
        "<div class=val id=flashPct>-</div>"
        "<div class=lbl>Flash used</div>"
        "<div class=flash-bar><div class=flash-fill id=flashBar style='width:0'></div></div>"
      "</div>"
    "</div>"
    "<div id=lessonInfo></div>"
    "</div>"
  ));

  // Part 4: Schedule card
  server.sendContent_P(PSTR(
    "<div class=card>"
      "<h2>\xF0\x9F\x93\x85 Today's Schedule</h2>"
      "<div id=schedBox><div class=sched>Loading schedule...</div></div>"
    "</div>"
  ));

  yield();

  // Part 4.5: Date picker card
  server.sendContent_P(PSTR(
    "<div class=card id=dpCard style=display:none>"
      "<div style='display:flex;justify-content:space-between;align-items:center;margin-bottom:10px'>"
        "<h2>&#8595; Download Attendance</h2>"
        "<button class='btn-s btn-c' onclick=hideAll()>&#x2715;</button>"
      "</div>"
      "<div id=dpContent><div class=sched>Loading available dates...</div></div>"
    "</div>"
  ));

  // Part 5: Enroll card
  server.sendContent_P(PSTR(
    "<div class=card id=eCard style=display:none>"
      "<h2>Enroll New Student</h2>"
      "<div class=g2>"
        "<div><div class=sub>Fingerprint Slot (1&ndash;127)</div>"
          "<input id=eid type=number min=1 max=127 placeholder='ex: 67'></div>"
        "<div><div class=sub>Student ID</div>"
          "<input id=esid placeholder='ex: 67'></div>"
      "</div><div class=g2>"
        "<div><div class=sub>First Name</div>"
          "<input id=ef placeholder='fajer'></div>"
        "<div><div class=sub>Last Name</div>"
          "<input id=el placeholder='Abednabie'></div>"
      "</div><div class=g2>"
        "<div><div class=sub>Admin Password</div>"
          "<input id=ep type=password></div>"
        "<div></div>"
      "</div>"
      "<div style='display:flex;gap:8px;margin-top:8px'>"
        "<button onclick=doEnroll()>&#128105; Start Scan</button>"
        "<button class=btn-c onclick=hideAll()>Cancel</button>"
      "</div>"
      "<div id=eMsg></div>"
    "</div>"
  ));

  // Part 6: Delete card
  server.sendContent_P(PSTR(
    "<div class=card id=dCard style=display:none>"
      "<h2>&#128465; Delete Fingerprint</h2>"
      "<div class=g2>"
        "<div><div class=sub>Fingerprint Slot ID to Delete</div>"
          "<input id=did type=number min=1 max=127 placeholder='ex: 67'></div>"
        "<div><div class=sub>Admin Password</div>"
          "<input id=dp type=password></div>"
      "</div>"
      "<div style='display:flex;gap:8px;margin-top:8px'>"
        "<button class=btn-r onclick=doDelete()>&#128465; Delete</button>"
        "<button class=btn-c onclick=hideAll()>Cancel</button>"
      "</div>"
      "<div id=dMsg></div>"
      "<div id=studentList style='margin-top:10px'></div>"
    "</div>"
  ));

  // Part 7: Clear logs card
  server.sendContent_P(PSTR(
    "<div class=card id=cCard style=display:none>"
      "<h2>&#128465; Clear All Attendance Logs</h2>"
      "<p style='font-size:13px;color:var(--err);margin-bottom:8px'>"
        "This will delete ALL attendance records. Students remain enrolled. Cannot be undone!</p>"
      "<div class=g2>"
        "<div><div class=sub>Admin Password</div>"
          "<input id=cp type=password></div>"
        "<div><div class=sub>Type CONFIRM to proceed</div>"
          "<input id=cc placeholder='CONFIRM'></div>"
      "</div>"
      "<div style='display:flex;gap:8px;margin-top:8px'>"
        "<button class=btn-r onclick=doClear()>&#128465; Clear Everything</button>"
        "<button class=btn-c onclick=hideAll()>Cancel</button>"
      "</div>"
      "<div id=cMsg></div>"
    "</div>"
  ));

  yield();

  // Part 7.5: [NEW-25] Manual attendance entry card
  server.sendContent_P(PSTR(
    "<div class=card id=mCard style=display:none>"
      "<h2>&#9997; Manual Attendance Entry</h2>"
      "<p style='font-size:12px;color:var(--muted);margin-bottom:10px'>"
        "Use when the sensor is offline or a student forgot to scan.</p>"
      "<div class=g2>"
        "<div><div class=sub>Student</div>"
          "<select id=mStu><option value=''>Loading...</option></select></div>"
        "<div><div class=sub>Action</div>"
          "<select id=mAct>"
            "<option value=IN>IN</option>"
            "<option value=OUT>OUT</option>"
          "</select></div>"
      "</div>"
      "<div class=g2>"
        "<div><div class=sub>Admin Password</div>"
          "<input id=mPass type=password></div>"
        "<div></div>"
      "</div>"
      "<div style='display:flex;gap:8px;margin-top:8px'>"
        "<button class='btn-w' onclick=doManual()>&#9997; Submit</button>"
        "<button class=btn-c onclick=hideAll()>Cancel</button>"
      "</div>"
      "<div id=mMsg></div>"
    "</div>"
  ));

  // Part 7.6: [NEW-24] Settings card (ntfy.sh config)
  server.sendContent_P(PSTR(
    "<div class=card id=setCard style=display:none>"
      "<h2>&#9881; Settings</h2>"
      "<div class=sub style='margin-bottom:10px'>"
        "Push notifications via <b>ntfy.sh</b> &mdash; "
        "get a ping on your phone every time a student scans.</div>"
      "<div class=g2>"
        "<div><div class=sub>ntfy.sh URL (e.g. http://ntfy.sh/mytopic)</div>"
          "<input id=ntfyUrl placeholder='http://ntfy.sh/my-class-topic'></div>"
        "<div><div class=sub>Admin Password</div>"
          "<input id=ntfyPass type=password></div>"
      "</div>"
      "<div style='display:flex;gap:8px;margin-top:8px'>"
        "<button onclick=doNtfySave()>&#128276; Save URL</button>"
        "<button class=btn-c onclick=hideAll()>Cancel</button>"
      "</div>"
      "<div id=setMsg></div>"
    "</div>"
  ));

  // Part 8: Recent attendance table
  server.sendContent_P(PSTR(
    "<div class=card>"
      "<div style='display:flex;justify-content:space-between;align-items:center'>"
        "<h2>Recent Attendance <span class=pill id=rCnt>0</span>"
          "<span class=rtick id=rtick></span></h2>"
        "<div class=sub id=lastSync></div>"
      "</div>"
      "<div id=rTable>"
        "<div style='color:var(--muted);text-align:center;padding:20px'>Loading&hellip;</div>"
      "</div>"
    "</div>"
  ));

  // Part 8.5: [NEW-27] Absent students card (shown during active lessons)
  server.sendContent_P(PSTR(
    "<div id=absentBox></div>"
  ));

  // Part 9: Footer
  server.sendContent_P(PSTR(
    "<div class=footer>"
      "&copy; <span class=ee title='You found it! 67 forever &#127881;' "
      "onclick='alert(\"&#127881; Easter Egg #1: 67 is the magic number! Fajer was here. &#128526;\")'"
      ">Fajer Abednabie</span> &bull; Basrah, Iraq &bull; "
      "Data stored in device flash"
      "<!-- Easter Egg #2: View source? Nice! The answer is always 67. -->"
      "<div style='margin-top:4px;font-size:10px;color:rgba(147,171,207,.3)' "
      "onclick='this.style.color=\"var(--warn)\";this.textContent=\"&#127881; Easter Egg #3! Secret code: F67A &#128526;\"'"
      ">v4.1</div>"
    "</div></div>"
  ));

  yield();

  // Part 10: JavaScript
  server.sendContent_P(PSTR(
    "<script>"
    "let ek='';"
    "document.addEventListener('keydown',e=>{"
      "ek+=e.key;if(ek.length>10)ek=ek.slice(-10);"
      "if(ek.includes('67')){"
        "document.title='\\uD83C\\uDF89 67! You found the secret!';"
        "setTimeout(()=>document.title='IoT Attendance',3000);"
        "ek='';"
      "}"
    "});"

    // [FIX-26] SCHED now fetched from /api/schedule — no hardcoded duplicate
    "let SCHED=[];"
    "const DAYS=['Sunday','Monday','Tuesday','Wednesday','Thursday','Friday','Saturday'];"

    "async function loadSchedule(){"
      "try{"
        "const r=await fetch('/api/schedule');"
        "SCHED=await r.json();"
        "updateSchedule();"
      "}catch(e){}"
    "}"

    "function updateSchedule(){"
      "const now=new Date();"
      "const utcH=now.getUTCHours(),utcM=now.getUTCMinutes();"
      "let lH=utcH+3,lM=utcM,wday=now.getUTCDay();"
      "if(lH>=24){lH-=24;wday=(wday+1)%7;}"
      "const hm=lH*60+lM;"
      "const todaySlots=SCHED.filter(s=>s.d===wday);"
      "let html='';"
      "if(!todaySlots.length){"
        "html='<div class=sched>No practical lessons today ('+DAYS[wday]+')</div>';"
      "}else{"
        "todaySlots.forEach(s=>{"
          "const start=s.sh*60+s.sm,end=s.eh*60+s.em;"
          "const active=(hm>=start&&hm<end);"
          "const cls=active?'sched sched-active':'sched';"
          "const st=String(s.sh).padStart(2,'0')+':'+String(s.sm).padStart(2,'0');"
          "const et=String(s.eh).padStart(2,'0')+':'+String(s.em).padStart(2,'0');"
          "html+=`<div class=\"${cls}\">${active?'\\u25B6 ':''}<b>${s.n}</b> &mdash; ${st} to ${et}${active?' (NOW)':''}</div>`;"
        "});"
      "}"
      "document.getElementById('schedBox').innerHTML=html;"
    "}"
  ));

  yield();

  server.sendContent_P(PSTR(
    // Refresh tick
    "let _rtick=7;"
    "setInterval(()=>{"
      "if(--_rtick<=0)_rtick=7;"
      "const el=document.getElementById('rtick');"
      "if(el)el.textContent='\\u27F3 '+_rtick+'s';"
    "},1000);"

    "async function load(){"
      "try{"
        "const r=await fetch('/api/status');"
        "if(!r.ok)return;"
        "const j=await r.json();"
        "document.getElementById('sc').textContent=j.today_scans;"
        "document.getElementById('up').textContent=fmt(j.uptime_s);"
        "document.getElementById('ts').textContent=j.time_synced?'\\u2713':'\\u2717';"
        "document.getElementById('st').textContent=j.student_count;"
        "document.getElementById('sn').textContent=j.sensor_ok?'\\u2713':'\\u2717';"
        "document.getElementById('hp').textContent=j.free_heap;"
        "document.getElementById('rCnt').textContent=j.logs.length;"
        "document.getElementById('lastSync').textContent=j.last_sync;"
        "document.getElementById('pIn').textContent=j.cur_in;"
        "document.getElementById('pOut').textContent=j.cur_out;"

        // [NEW-26] Flash usage indicator
        "const fp=j.flash_pct||0;"
        "document.getElementById('flashPct').textContent=fp+'%';"
        "const bar=document.getElementById('flashBar');"
        "bar.style.width=fp+'%';"
        "bar.className='flash-fill'+(fp>=80?' crit':fp>=60?' warn':'');"
        "if(fp>=80)document.getElementById('flashStat').className='stat stat-warn';"

        "if(j.current_lesson){"
          "document.getElementById('lessonInfo').innerHTML="
            "'<div class=lesson-badge style=\"margin-top:10px\">\\uD83D\\uDCDA Now: '+j.current_lesson+'</div>';"
        "}else{"
          "document.getElementById('lessonInfo').innerHTML="
            "'<div style=\"margin-top:8px;color:var(--muted);font-size:12px\">No active lesson</div>';"
        "}"

        "const rows=j.logs.map((e,i)=>{"
          "const cls=e.action.toLowerCase()==='in'?'in':(e.action.toLowerCase()==='out'?'out':'manual-badge');"
          "return `<tr class=fade-row style='animation-delay:${i*50}ms'>`"
          "+`<td>${e.time}</td><td>${e.name}</td>`"
          "+`<td><span class='${cls}'>${e.action}</span></td>`"
          "+`<td style='font-size:11px;color:var(--muted)'>${e.lesson||''}</td></tr>`;"
        "}).join('');"
        "document.getElementById('rTable').innerHTML=rows"
          "?`<table><thead><tr><th>Time</th><th>Name</th><th>Action</th><th>Lesson</th></tr></thead>"
             "<tbody>${rows}</tbody></table>`"
          ":'<div style=\"color:var(--muted);text-align:center;padding:20px\">"
             "No scans yet today</div>';"

        // [NEW-27] Absent students box
        "const ab=document.getElementById('absentBox');"
        "if(j.current_lesson&&j.absent){"
          "if(j.absent.length===0){"
            "ab.innerHTML='<div class=card style=\"border-color:rgba(48,209,88,.2)\">"
              "<h2 style=\"color:var(--ok)\">\\u2713 All present &mdash; '+j.current_lesson+'</h2></div>';"
          "}else{"
            "const arows=j.absent.map(s=>`<tr class=absent-row><td>${s.fid}</td><td>${s.sid}</td><td>${s.name}</td></tr>`).join('');"
            "ab.innerHTML='<div class=card style=\"border-color:rgba(255,69,58,.2)\">"
              "<h2 style=\"color:var(--err);margin-bottom:8px\">\\u26A0 Absent &mdash; '+j.current_lesson"
              "+'</h2><table><thead><tr><th>Slot</th><th>ID</th><th>Name</th></tr></thead>"
              "<tbody>'+arows+'</tbody></table></div>';"
          "}"
        "}else{ab.innerHTML='';}"

        "updateSchedule();"
      "}catch(e){}"
    "}"
    "function fmt(s){"
      "if(s===67)return '1h 7m \\u2728';"
      "return Math.floor(s/3600)+'h '+Math.floor(s%3600/60)+'m';"
    "}"
  ));

  yield();

  server.sendContent_P(PSTR(
    "async function showDatePicker(){"
      "hideAll();show('dpCard');"
      "document.getElementById('dpContent').innerHTML='<div class=sched>Loading&hellip;</div>';"
      "try{"
        "const r=await fetch('/api/dates');"
        "const j=await r.json();"
        "let html='';"
        "if(!j.dates||!j.dates.length){"
          "html='<div style=\"color:var(--muted);font-size:13px\">No attendance files found on device.</div>';"
        "}else{"
          "const sorted=[...j.dates].sort().reverse();"
          "sorted.forEach(d=>{"
            "const y=d.slice(0,4),m=d.slice(4,6),day=d.slice(6,8);"
            "const isToday=(d===j.today);"
            "html+=`<a href=\"/api/attendance?date=${d}\" class=\"dp-link${isToday?' dp-today':''}\">`"
              "+`${isToday?'\\u2B07 Today \\u2014 ':'\\uD83D\\uDCC4 '}`"
              "+`${day}/${m}/${y}</a>`;"
          "});"
        "}"
        "document.getElementById('dpContent').innerHTML=html;"
      "}catch(e){"
        "document.getElementById('dpContent').innerHTML="
          "'<div style=\"color:var(--err);font-size:13px\">Failed to load dates.</div>';"
      "}"
    "}"

    // [FIX] All show functions use hideAll() first — no missed cards
    "function hideAll(){['eCard','dCard','cCard','dpCard','mCard','setCard'].forEach(hide);}"
    "function showEnroll(){hideAll();show('eCard');clr('eMsg')}"
    "function showDelete(){hideAll();show('dCard');clr('dMsg');loadStudents()}"
    "function showClear(){hideAll();show('cCard');clr('cMsg')}"
    "function showManual(){hideAll();show('mCard');clr('mMsg');loadStudentsDropdown()}"
    "function showSettings(){hideAll();show('setCard');clr('setMsg')}"
    "function show(id){document.getElementById(id).style.display=''}"
    "function hide(id){document.getElementById(id).style.display='none'}"
    "function clr(id){document.getElementById(id).innerHTML=''}"

    "async function loadStudents(){"
      "try{"
        "const r=await fetch('/api/students');"
        "const j=await r.json();"
        "if(!j.students||!j.students.length){"
          "document.getElementById('studentList').innerHTML="
            "'<div style=\"color:var(--muted)\">No students enrolled</div>';return;}"
        "let html='<table><thead><tr><th>Slot</th><th>Student ID</th><th>Name</th></tr></thead><tbody>';"
        "j.students.forEach(s=>{html+=`<tr><td>${s.fid}</td><td>${s.sid}</td><td>${s.name}</td></tr>`;});"
        "html+='</tbody></table>';"
        "document.getElementById('studentList').innerHTML=html;"
      "}catch(e){}"
    "}"

    // [NEW-25] Populate manual entry student dropdown
    "async function loadStudentsDropdown(){"
      "try{"
        "const r=await fetch('/api/students');"
        "const j=await r.json();"
        "const sel=document.getElementById('mStu');"
        "if(!j.students||!j.students.length){"
          "sel.innerHTML='<option>No students enrolled</option>';return;}"
        "sel.innerHTML=j.students.map(s=>"
          "`<option value=${s.fid}>${s.name} (Slot ${s.fid})</option>`"
        ").join('');"
      "}catch(e){}"
    "}"
  ));

  yield();

  server.sendContent_P(PSTR(
    "async function doEnroll(){"
      "const id=document.getElementById('eid').value;"
      "const pass=document.getElementById('ep').value;"
      "const sid=document.getElementById('esid').value;"
      "const fn=document.getElementById('ef').value;"
      "const ln=document.getElementById('el').value;"
      "if(!id||!pass||!fn||!ln){setMsg('eMsg','Fill all required fields.','err');return;}"
      "setMsg('eMsg','&#9203; Place finger on sensor \\u2014 watch LCD\\u2026','ok');"
      "try{"
        "const r=await fetch('/enroll?id='+encodeURIComponent(id)"
          "+'&pass='+encodeURIComponent(pass)"
          "+'&sid='+encodeURIComponent(sid)"
          "+'&first='+encodeURIComponent(fn)"
          "+'&last='+encodeURIComponent(ln));"
        "const t=await r.text();"
        "setMsg('eMsg',r.ok?'\\u2705 '+t:'\\u274c '+t,r.ok?'ok':'err');"
        "if(r.ok){load();setTimeout(hideAll,3500);}"
      "}catch(e){setMsg('eMsg','\\u274c Connection error','err');}"
    "}"

    "async function doDelete(){"
      "const id=document.getElementById('did').value;"
      "const pass=document.getElementById('dp').value;"
      "if(!id||!pass){setMsg('dMsg','Fill ID and password.','err');return;}"
      "if(!confirm('Delete fingerprint #'+id+'? This cannot be undone!'))return;"
      "try{"
        "const r=await fetch('/delete?id='+encodeURIComponent(id)"
          "+'&pass='+encodeURIComponent(pass));"
        "const t=await r.text();"
        "setMsg('dMsg',r.ok?'\\u2705 '+t:'\\u274c '+t,r.ok?'ok':'err');"
        "if(r.ok){load();loadStudents();}"
      "}catch(e){setMsg('dMsg','\\u274c Connection error','err');}"
    "}"

    "async function doClear(){"
      "const pass=document.getElementById('cp').value;"
      "const conf=document.getElementById('cc').value;"
      "if(!pass){setMsg('cMsg','Enter admin password.','err');return;}"
      "if(conf!=='CONFIRM'){setMsg('cMsg','Type CONFIRM to proceed.','err');return;}"
      "if(!confirm('DELETE ALL attendance logs? This CANNOT be undone!'))return;"
      "try{"
        "const r=await fetch('/clear?pass='+encodeURIComponent(pass));"
        "const t=await r.text();"
        "setMsg('cMsg',r.ok?'\\u2705 '+t:'\\u274c '+t,r.ok?'ok':'err');"
        "if(r.ok){load();setTimeout(hideAll,2000);}"
      "}catch(e){setMsg('cMsg','\\u274c Connection error','err');}"
    "}"

    // [NEW-25] Manual attendance submit
    "async function doManual(){"
      "const fid=document.getElementById('mStu').value;"
      "const act=document.getElementById('mAct').value;"
      "const pass=document.getElementById('mPass').value;"
      "if(!fid||!pass){setMsg('mMsg','Select student and enter password.','err');return;}"
      "try{"
        "const r=await fetch('/manual?fid='+encodeURIComponent(fid)"
          "+'&action='+encodeURIComponent(act)"
          "+'&pass='+encodeURIComponent(pass));"
        "const t=await r.text();"
        "setMsg('mMsg',r.ok?'\\u2705 '+t:'\\u274c '+t,r.ok?'ok':'err');"
        "if(r.ok){load();setTimeout(hideAll,2500);}"
      "}catch(e){setMsg('mMsg','\\u274c Connection error','err');}"
    "}"

    // [NEW-24] Save ntfy URL
    "async function doNtfySave(){"
      "const url=document.getElementById('ntfyUrl').value.trim();"
      "const pass=document.getElementById('ntfyPass').value;"
      "if(!pass){setMsg('setMsg','Enter admin password.','err');return;}"
      "try{"
        "const r=await fetch('/ntfy/config?url='+encodeURIComponent(url)"
          "+'&pass='+encodeURIComponent(pass));"
        "const t=await r.text();"
        "setMsg('setMsg',r.ok?'\\u2705 '+t:'\\u274c '+t,r.ok?'ok':'err');"
      "}catch(e){setMsg('setMsg','\\u274c Connection error','err');}"
    "}"

    "function setMsg(id,m,c){"
      "const d=document.getElementById(id);"
      "d.innerHTML=m;d.className='msg '+(c==='ok'?'ok-msg':'err-msg');"
    "}"

    "load();"
    "loadSchedule();"
    "setInterval(load,7000);"
    "setInterval(updateSchedule,60000);"
    "</script></body></html>"
  ));
}

// ============================================================
// WEB HANDLERS
// ============================================================
void handleRoot() { sendHTML(); }

void handleAPIStatus() {
  // Count current IN/OUT and build absent list for active lessons
  int cur_in = 0, cur_out = 0;
  for (int i = 1; i < 128; i++) {
    if      (lastAction[i] == 1) cur_in++;
    else if (lastAction[i] == 2) cur_out++;
  }

  FSInfo fs_info;
  uint8_t flash_pct = 0;
  if (LittleFS.info(fs_info))
    flash_pct = (uint8_t)((fs_info.usedBytes * 100UL) / fs_info.totalBytes);

  server.sendHeader("Cache-Control", "no-cache");
  server.sendHeader("Connection", "close");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");

  char buf[128];
  server.sendContent("{");
  server.sendContent("\"tz\":\"Iraq/Basrah\",");
  snprintf(buf, sizeof(buf), "\"uptime_s\":%lu,", millis()/1000);
  server.sendContent(buf);
  snprintf(buf, sizeof(buf), "\"today_scans\":%d,", today_scans);
  server.sendContent(buf);
  snprintf(buf, sizeof(buf), "\"student_count\":%d,", student_count);
  server.sendContent(buf);
  snprintf(buf, sizeof(buf), "\"sensor_ok\":%s,", sensor_available ? "true" : "false");
  server.sendContent(buf);
  snprintf(buf, sizeof(buf), "\"time_synced\":%s,", time_synced ? "true" : "false");
  server.sendContent(buf);
  snprintf(buf, sizeof(buf), "\"free_heap\":%u,", ESP.getFreeHeap());
  server.sendContent(buf);
  // [NEW-26] Flash usage
  snprintf(buf, sizeof(buf), "\"flash_pct\":%d,", flash_pct);
  server.sendContent(buf);
  snprintf(buf, sizeof(buf), "\"cur_in\":%d,\"cur_out\":%d,", cur_in, cur_out);
  server.sendContent(buf);

  const char* lesson = getCurrentLesson(getNow());
  server.sendContent("\"current_lesson\":");
  if (lesson) {
    // [FIX-21] Escape lesson string
    char esc[64];
    jsonEscape(lesson, esc, sizeof(esc));
    server.sendContent("\""); server.sendContent(esc); server.sendContent("\",");
  } else {
    server.sendContent("null,");
  }

  server.sendContent("\"last_sync\":\"");
  server.sendContent(last_sync_str);
  server.sendContent("\",");

  // [NEW-27] Absent list (only during active lesson)
  server.sendContent("\"absent\":[");
  if (lesson) {
    bool first = true;
    char esc1[32], esc2[32];
    for (int i = 0; i < student_count; i++) {
      uint16_t tid = students[i].template_id;
      if (tid < 128 && lastAction[tid] == 0) {
        if (!first) server.sendContent(",");
        first = false;
        jsonEscape(students[i].first,     esc1, sizeof(esc1));
        jsonEscape(students[i].last_name, esc2, sizeof(esc2));
        snprintf(buf, sizeof(buf), "{\"fid\":%d,\"sid\":\"%s\",\"name\":\"%s %s\"}",
                 tid, students[i].student_id, esc1, esc2);
        server.sendContent(buf);
        yield();
      }
    }
  }
  server.sendContent("],");

  // [FIX-21] Escape all log fields
  server.sendContent("\"logs\":[");
  bool first = true;
  char esc[64];
  for (int i = logCount - 1; i >= 0; i--) {
    if (!first) server.sendContent(",");
    first = false;
    server.sendContent("{\"time\":\"");
    jsonEscape(logs[i].time_str, esc, sizeof(esc));
    server.sendContent(esc);
    server.sendContent("\",\"name\":\"");
    jsonEscape(logs[i].name, esc, sizeof(esc));
    server.sendContent(esc);
    server.sendContent("\",\"action\":\"");
    jsonEscape(logs[i].action, esc, sizeof(esc));
    server.sendContent(esc);
    server.sendContent("\",\"lesson\":\"");
    jsonEscape(logs[i].lesson, esc, sizeof(esc));
    server.sendContent(esc);
    server.sendContent("\"}");
    yield();
  }
  server.sendContent("]}");
}

void handleAPIStudents() {
  server.sendHeader("Cache-Control", "no-cache");
  server.sendHeader("Connection", "close");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  server.sendContent("{\"students\":[");
  bool first = true;
  char esc1[32], esc2[32];
  for (int i = 0; i < student_count; i++) {
    if (!first) server.sendContent(",");
    first = false;
    // [FIX-21] Escape name fields
    jsonEscape(students[i].first,     esc1, sizeof(esc1));
    jsonEscape(students[i].last_name, esc2, sizeof(esc2));
    char buf[120];
    snprintf(buf, sizeof(buf), "{\"fid\":%d,\"sid\":\"%s\",\"name\":\"%s %s\"}",
             students[i].template_id, students[i].student_id, esc1, esc2);
    server.sendContent(buf);
    yield();
  }
  server.sendContent("]}");
}

// [NEW-27] Serve schedule from C++ source — JS fetches this instead of hardcoding
void handleAPISchedule() {
  const char* days[] = {"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
  server.sendHeader("Cache-Control", "no-cache");
  server.sendHeader("Connection", "close");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  server.sendContent("[");
  for (int i = 0; i < SCHEDULE_COUNT; i++) {
    if (i) server.sendContent(",");
    char buf[200];
    snprintf(buf, sizeof(buf),
      "{\"d\":%d,\"day\":\"%s\",\"sh\":%d,\"sm\":%d,\"eh\":%d,\"em\":%d,\"n\":\"%s\"}",
      schedule[i].day_of_week, days[schedule[i].day_of_week],
      schedule[i].start_h, schedule[i].start_m,
      schedule[i].end_h,   schedule[i].end_m,
      schedule[i].lesson_name);
    server.sendContent(buf);
  }
  server.sendContent("]");
}

// [NEW-27] Absent students endpoint (standalone, for external tools)
void handleAPIAbsent() {
  server.sendHeader("Cache-Control", "no-cache");
  server.sendHeader("Connection", "close");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");

  const char* lesson = getCurrentLesson(getNow());
  server.sendContent("{\"during_lesson\":");
  server.sendContent(lesson ? "true" : "false");
  server.sendContent(",\"lesson\":");
  if (lesson) {
    char esc[64]; jsonEscape(lesson, esc, sizeof(esc));
    server.sendContent("\""); server.sendContent(esc); server.sendContent("\"");
  } else server.sendContent("null");
  server.sendContent(",\"absent\":[");

  bool first = true;
  char esc1[32], esc2[32], buf[120];
  for (int i = 0; i < student_count; i++) {
    uint16_t tid = students[i].template_id;
    if (tid < 128 && lastAction[tid] == 0) {
      if (!first) server.sendContent(",");
      first = false;
      jsonEscape(students[i].first,     esc1, sizeof(esc1));
      jsonEscape(students[i].last_name, esc2, sizeof(esc2));
      snprintf(buf, sizeof(buf), "{\"fid\":%d,\"sid\":\"%s\",\"name\":\"%s %s\"}",
               tid, students[i].student_id, esc1, esc2);
      server.sendContent(buf);
      yield();
    }
  }
  server.sendContent("]}");
}

void handleAPIDates() {
  server.sendHeader("Cache-Control", "no-cache");
  server.sendHeader("Connection", "close");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");

  char today_ds[10];
  datestamp_buf(getNow(), today_ds, sizeof(today_ds));
  server.sendContent("{\"today\":\"");
  server.sendContent(today_ds);
  server.sendContent("\",\"dates\":[");

  bool first = true;
  Dir dir = LittleFS.openDir("/");
  while (dir.next()) {
    yield();
    String fn = dir.fileName();
    if (fn.startsWith("att_") && fn.endsWith(".csv")) {
      String ds = fn.substring(4, 12);
      if (ds.length() == 8) {
        if (!first) server.sendContent(",");
        first = false;
        server.sendContent("\"");
        server.sendContent(ds.c_str());
        server.sendContent("\"");
      }
    }
  }
  server.sendContent("]}");
}

void handleAPIStats() {
  server.sendHeader("Cache-Control", "no-cache");
  server.sendHeader("Connection", "close");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");

  int cur_in = 0, cur_out = 0;
  for (int i = 1; i < 128; i++) {
    if      (lastAction[i] == 1) cur_in++;
    else if (lastAction[i] == 2) cur_out++;
  }

  char buf[64];
  snprintf(buf, sizeof(buf), "{\"total_in\":%d,\"total_out\":%d,\"students\":[",
           cur_in, cur_out);
  server.sendContent(buf);

  bool first = true;
  for (int i = 0; i < student_count; i++) {
    uint16_t tid = students[i].template_id;
    if (tid < 128 && lastAction[tid] != 0) {
      if (!first) server.sendContent(",");
      first = false;
      snprintf(buf, sizeof(buf), "{\"fid\":%d,\"status\":\"%s\"}",
               tid, lastAction[tid] == 1 ? "IN" : "OUT");
      server.sendContent(buf);
      yield();
    }
  }
  server.sendContent("]}");
}

void handleAttendanceDownload() {
  char ds[10];
  if (server.hasArg("date") && server.arg("date") != "today") {
    strncpy(ds, server.arg("date").c_str(), sizeof(ds)-1);
    ds[sizeof(ds)-1] = '\0';
  } else {
    datestamp_buf(getNow(), ds, sizeof(ds));
  }
  char path[32];
  snprintf(path, sizeof(path), "/att_%s.csv", ds);
  if (!LittleFS.exists(path)) {
    server.send(404, "text/plain", "No attendance file for this date");
    return;
  }
  File f = LittleFS.open(path, "r");
  if (!f) { server.send(500, "text/plain", "Cannot open file"); return; }
  char disposition[64];
  snprintf(disposition, sizeof(disposition), "attachment; filename=attendance_%s.csv", ds);
  server.sendHeader("Content-Disposition", disposition);
  server.sendHeader("Connection", "close");
  server.streamFile(f, "text/csv");
  f.close();
}

// [NEW-25] Manual attendance entry
void handleManual() {
  if (!checkAdminPass(server.arg("pass"))) {  // [FIX-29]
    server.send(403, "text/plain", "Wrong password");
    return;
  }
  uint16_t fid = (uint16_t)server.arg("fid").toInt();
  if (fid == 0 || fid > 127) {
    server.send(400, "text/plain", "Invalid fingerprint slot");
    return;
  }
  int idx = findStudent(fid);
  if (idx < 0) {
    server.send(404, "text/plain", "Student not found in database");
    return;
  }
  String action_str = server.arg("action");
  // [FIX-32] Copy to local buffer before any other server calls could invalidate it
  char action_buf[4] = {};
  strncpy(action_buf, action_str.c_str(), sizeof(action_buf)-1);
  if (action_str != "IN" && action_str != "OUT") {
    server.send(400, "text/plain", "Action must be IN or OUT");
    return;
  }

  Student& s = students[idx];
  const char* action = action_buf;
  time_t utc_t = getNow();
  char localStr[30];
  formatLocal_buf(utc_t, localStr, sizeof(localStr));
  const char* lesson = getCurrentLesson(utc_t);

  if (fid < 128) lastAction[fid] = (action_str == "IN") ? 1 : 2;
  appendAttendance(s, action, utc_t, lesson);
  saveLastAction();

  char fullName[33];
  snprintf(fullName, sizeof(fullName), "%s %s", s.first, s.last_name);

  int slot;
  if (logCount < MAX_LOG_ENTRIES) {
    slot = logCount++;
  } else {
    for (int i = 0; i < MAX_LOG_ENTRIES - 1; i++) logs[i] = logs[i+1];
    slot = MAX_LOG_ENTRIES - 1;
  }
  memset(&logs[slot], 0, sizeof(LogEntry));
  strncpy(logs[slot].name,     fullName, sizeof(logs[slot].name)-1);
  strncpy(logs[slot].time_str, localStr, sizeof(logs[slot].time_str)-1);
  // Mark manual entries visually distinct from IN/OUT
  strncpy(logs[slot].action, action, sizeof(logs[slot].action)-1);
  strncpy(logs[slot].lesson, lesson ? lesson : "Manual", sizeof(logs[slot].lesson)-1);
  today_scans++;
  strncpy(last_scanned_name, fullName, sizeof(last_scanned_name)-1);

  logs_dirty = true;  // [FIX-28] schedule flush, don't write immediately

  // [NEW-24] Push notification for manual entry
  char ntfyTitle[48], ntfyBody[80];
  snprintf(ntfyTitle, sizeof(ntfyTitle), "Manual: %s", fullName);
  snprintf(ntfyBody, sizeof(ntfyBody), "%s %s | %s", action, lesson ? lesson : "General", localStr);
  sendNtfyNotification(ntfyTitle, ntfyBody);

  char resp[64];
  snprintf(resp, sizeof(resp), "Manual %s: %s", action, fullName);
  server.send(200, "text/plain", resp);
  debugPrintF("[MANUAL] %s -> %s", fullName, action);
}

// [NEW-24] Save ntfy URL via web UI
void handleNtfyConfig() {
  if (!checkAdminPass(server.arg("pass"))) {  // [FIX-29]
    server.send(403, "text/plain", "Wrong password");
    return;
  }
  // [FIX-32] Copy arg immediately to local buffer
  char url_buf[128] = {};
  { String t = server.arg("url"); t.trim(); strncpy(url_buf, t.c_str(), sizeof(url_buf)-1); }
  strncpy(ntfy_url, url_buf, sizeof(ntfy_url)-1);
  ntfy_url[sizeof(ntfy_url)-1] = '\0';

  File f = LittleFS.open("/ntfy_url.txt", "w");
  if (f) { f.println(ntfy_url); f.close(); }

  if (strlen(ntfy_url) == 0) {
    server.send(200, "text/plain", "ntfy notifications disabled.");
  } else {
    char resp[80];
    snprintf(resp, sizeof(resp), "ntfy URL saved. Sending test ping to: %s", ntfy_url);
    sendNtfyNotification("f-att test", "Push notifications are active!");
    server.send(200, "text/plain", resp);
  }
  debugPrintF("[NTFY] URL set to: %s", ntfy_url);
}

void handleEnroll() {
  debugPrint("=== ENROLL START ===");

  if (!sensor_available) {
    server.send(503, "text/plain", "Sensor offline - check wiring and restart");
    return;
  }
  if (!checkAdminPass(server.arg("pass"))) {  // [FIX-29]
    server.send(403, "text/plain", "Wrong password");
    return;
  }

  uint16_t id = (uint16_t)server.arg("id").toInt();
  if (id == 0 || id > 127) {
    server.send(400, "text/plain", "Invalid ID (must be 1-127)");
    return;
  }

  int existing = findStudent(id);

  // [FIX-32] server.arg() returns a temporary String — .c_str() on it is a dangling
  // pointer the moment the statement ends. Copy into local char arrays NOW, before
  // any other code can run that might cause use-after-free / heap corruption.
  char sid_buf[12]  = {};
  char first_buf[16] = {};
  char last_buf[16]  = {};
  { String t = server.arg("sid");   strncpy(sid_buf,   t.c_str(), sizeof(sid_buf)-1); }
  { String t = server.arg("first"); if (t.length()) strncpy(first_buf, t.c_str(), sizeof(first_buf)-1);
                                    else strncpy(first_buf, "Student", sizeof(first_buf)-1); }
  { String t = server.arg("last");  strncpy(last_buf,  t.c_str(), sizeof(last_buf)-1); }
  const char* sid_c   = sid_buf;
  const char* first_c = first_buf;
  const char* last_c  = last_buf;

  char fullName[33];
  snprintf(fullName, sizeof(fullName), "%s %s", first_c, last_c);

  // SCAN 1
  lcd.clear();
  char idbuf[21];
  snprintf(idbuf, sizeof(idbuf), "Enrolling ID: %d", id);
  lcdPad(0, 0, idbuf);
  lcd.setCursor(0, 1); lcd.write(byte(0));
  lcdPad(1, 2, "Scan 1 of 2");
  lcdPad(2, 0, "Place finger now    ");
  // [FIX-31] Use lcdPad so the full row is always clean
  lcdPad(3, 0, fullName);

  int p = -1, timeout = 0;
  while (p != FINGERPRINT_OK && timeout < 50) {
    p = finger.getImage();
    // [FIX-33] Do NOT call server.handleClient() here — re-entrant calls inside a
    // handler corrupt ESP8266WebServer internal state. yield()+wdtFeed() is enough;
    // other clients will queue and be served once enrollment finishes.
    yield(); ESP.wdtFeed(); delay(200); timeout++;
  }
  if (p != FINGERPRINT_OK) {
    server.send(500, "text/plain", "Timeout - no finger detected");
    lcd.clear(); return;
  }

  p = finger.image2Tz(1);
  if (p != FINGERPRINT_OK) {
    char err[64];
    snprintf(err, sizeof(err), "Scan 1 failed code=%d", p);
    server.send(500, "text/plain", err);
    lcd.clear(); return;
  }

  // REMOVE FINGER
  lcd.clear();
  lcdPad(0, 0, idbuf);
  lcdPad(1, 0, "Remove finger       ");
  lcd.setCursor(9, 2); lcd.write(byte(1));
  timeout = 0;
  while (finger.getImage() != FINGERPRINT_NOFINGER && timeout < 30) {
    yield(); ESP.wdtFeed(); delay(200); timeout++;
  }
  delay(500);

  // SCAN 2
  lcd.clear();
  lcdPad(0, 0, idbuf);
  lcd.setCursor(0, 1); lcd.write(byte(0));
  lcdPad(1, 2, "Scan 2 of 2");
  lcdPad(2, 0, "Place finger again  ");

  p = -1; timeout = 0;
  while (p != FINGERPRINT_OK && timeout < 50) {
    p = finger.getImage();
    yield(); ESP.wdtFeed(); delay(200); timeout++;
  }
  if (p != FINGERPRINT_OK) {
    server.send(500, "text/plain", "Timeout - second scan not detected");
    lcd.clear(); return;
  }

  p = finger.image2Tz(2);
  if (p != FINGERPRINT_OK) {
    char err[64];
    snprintf(err, sizeof(err), "Scan 2 failed code=%d", p);
    server.send(500, "text/plain", err);
    lcd.clear(); return;
  }

  // CREATE & STORE MODEL
  p = finger.createModel();
  if (p != FINGERPRINT_OK) {
    if (p == FINGERPRINT_ENROLLMISMATCH)
      server.send(500, "text/plain", "Fingers don't match - try again");
    else {
      char err[64];
      snprintf(err, sizeof(err), "Create model failed code=%d", p);
      server.send(500, "text/plain", err);
    }
    lcd.clear(); return;
  }

  p = finger.storeModel(id);
  if (p != FINGERPRINT_OK) {
    char err[64];
    snprintf(err, sizeof(err), "Store failed code=%d", p);
    server.send(500, "text/plain", err);
    lcd.clear(); return;
  }

  // Update CSV
  if (existing >= 0) {
    Student& s = students[existing];
    memset(&s, 0, sizeof(Student));
    s.template_id = id;
    strncpy(s.student_id, sid_c,   sizeof(s.student_id)-1);
    strncpy(s.first,      first_c, sizeof(s.first)-1);
    strncpy(s.last_name,  last_c,  sizeof(s.last_name)-1);
    rewriteStudentsCSV();
  } else {
    addStudentToCSV(id, sid_c, first_c, last_c);
  }

  // SUCCESS
  lcd.clear();
  lcdPad(0, 0, "  \x01\x01 Enroll OK! \x01\x01");
  lcdPad(1, 0, fullName);
  snprintf(idbuf, sizeof(idbuf), "ID %d saved", id);
  lcdPad(2, 0, idbuf);
  lcdPad(3, 0, "  \x01 Stored in sensor");

  char resp[64];
  snprintf(resp, sizeof(resp), "Enrolled: %s (ID %d)", fullName, id);
  server.send(200, "text/plain", resp);
  delay(1500);
  lcd.clear();
}

void handleDelete() {
  debugPrint("=== DELETE START ===");

  if (!checkAdminPass(server.arg("pass"))) {  // [FIX-29]
    server.send(403, "text/plain", "Wrong password");
    return;
  }

  uint16_t id = (uint16_t)server.arg("id").toInt();
  if (id == 0 || id > 127) {
    server.send(400, "text/plain", "Invalid ID (must be 1-127)");
    return;
  }

  int idx = findStudent(id);
  char deletedName[33] = "Unknown";
  if (idx >= 0) {
    snprintf(deletedName, sizeof(deletedName), "%s %s",
             students[idx].first, students[idx].last_name);
  }

  bool sensorDeleted = false;
  if (sensor_available) sensorDeleted = deleteFingerprint(id);

  bool csvDeleted = false;
  if (idx >= 0) csvDeleted = deleteStudentFromCSV(id);

  if (id < 128) {
    lastAction[id] = 0;
    lastScanTime[id] = 0;  // [FIX-24] clear debounce timer for deleted slot
    saveLastAction();
  }

  lcd.clear();
  if (sensorDeleted || csvDeleted) {
    lcd.setCursor(0, 0); lcd.write(byte(1));
    lcdCenter(0, " Deleted!");
    char nameBuf[21];
    strncpy(nameBuf, deletedName, 20); nameBuf[20] = '\0';
    lcdCenter(1, nameBuf);
    char idbuf[21];
    snprintf(idbuf, sizeof(idbuf), "ID %d removed", id);
    lcdCenter(2, idbuf);

    char resp[64];
    snprintf(resp, sizeof(resp), "Deleted: %s (ID %d)", deletedName, id);
    server.send(200, "text/plain", resp);
  } else {
    lcdCenter(0, "Delete Failed");
    char idbuf[21];
    snprintf(idbuf, sizeof(idbuf), "ID %d not found", id);
    lcdCenter(1, idbuf);
    server.send(404, "text/plain", "ID not found in sensor or database");
  }
  delay(1500);
  lcd.clear();
}

void handleClear() {
  if (!checkAdminPass(server.arg("pass"))) {  // [FIX-29]
    server.send(403, "text/plain", "Wrong password");
    return;
  }
  clearAllAttendanceData();
  lcd.clear();
  lcdCenter(0, "Data Cleared!");
  lcdCenter(1, "All logs removed");
  lcdCenter(2, "Memory freed");
  delay(1500);
  lcd.clear();
  server.send(200, "text/plain", "All attendance data cleared. Students remain enrolled.");
}

void handleEasterEgg67() {
  server.send(200, "text/html",
    "<html><body style='background:#071028;color:#ffd60a;display:flex;"
    "justify-content:center;align-items:center;height:100vh;font-family:monospace;"
    "font-size:48px;flex-direction:column'>"
    "<div>&#127881; 67 &#127881;</div>"
    "<div style='font-size:20px;margin-top:20px;color:#e6eef8'>"
    "You found the secret page!</div>"
    "<div style='font-size:16px;margin-top:10px;color:#93abcf'>"
    "Fajer Abednabie &bull; Basrah, Iraq</div>"
    "<div style='font-size:14px;margin-top:20px;color:#30d158'>"
    "Easter Egg #5: The magic number lives here forever. (i use Arch Linux btw.)</div>"
    "</body></html>");
}

// ============================================================
// BUZZER TONES — iPhone Face ID haptic style
// ============================================================
void playSuccessTone() {
  tone(BUZZER_PIN, 1050); delay(25); noTone(BUZZER_PIN);
  delay(18);
  tone(BUZZER_PIN, 1280); delay(40); noTone(BUZZER_PIN);
}

void playErrorTone() {
  tone(BUZZER_PIN, 600); delay(32); noTone(BUZZER_PIN);
  delay(20);
  tone(BUZZER_PIN, 500); delay(32); noTone(BUZZER_PIN);
  delay(20);
  tone(BUZZER_PIN, 380); delay(50); noTone(BUZZER_PIN);
}

void playLowConfidenceTone() {
  tone(BUZZER_PIN, 820); delay(28); noTone(BUZZER_PIN);
  delay(16);
  tone(BUZZER_PIN, 820); delay(28); noTone(BUZZER_PIN);
}

// ============================================================
// SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(100);
  mySerial.begin(57600);

  memset(lastAction,   0, sizeof(lastAction));
  memset(lastScanTime, 0, sizeof(lastScanTime));

u8g2Init();
  lcd.clear();
  lcdCenter(0, "* Attendance Sys *");
  lcdCenter(2, "Initializing...");
  lcdCenter(3, "(c) F.Abednabie");
  delay(800);

  pinMode(BUZZER_PIN, OUTPUT);

  // LittleFS
  lcd.clear();
  lcdCenter(0, "* Attendance Sys *");
  lcdCenter(1, "Mounting storage...");
  if (initFS()) {
    lcdCenter(2, "Storage OK");
    loadStudentsCSV();
    loadLastTime();
    loadLastAction();
    loadNtfyURL();  // [NEW-24]
  } else {
    lcdCenter(2, "Storage WARN");
  }
  delay(500);

  datestamp_buf(getNow(), current_day_stamp, sizeof(current_day_stamp));
  loadRecentLogs();

  // WiFi
  lcd.clear();
  lcdCenter(0, "* Attendance Sys *");
  lcdCenter(1, "Connecting WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.hostname(MDNS_HOST);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  int wTry = 0;
  while (WiFi.status() != WL_CONNECTED && wTry < 25) {
    delay(400);
    lcd.setCursor(wTry % 20, 2);
    lcd.print(".");
    wTry++;
    yield();
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifi_connected = true;
    debugPrintF("WiFi OK: %s", WiFi.localIP().toString().c_str());
    lcdCenter(2, "WiFi OK!");
    char ip_buf[20];
    WiFi.localIP().toString().toCharArray(ip_buf, sizeof(ip_buf));
    lcdCenter(3, ip_buf);

    // [FIX-25] mDNS + OTA both inside the success block
    if (MDNS.begin(MDNS_HOST)) {
      MDNS.addService("http", "tcp", 80);
      debugPrint("[mDNS] f-att.local started");

      ArduinoOTA.setHostname(MDNS_HOST);
      ArduinoOTA.setPassword(OTA_PASSWORD);
      ArduinoOTA.onStart([]() {
        debugPrint("[OTA] Start");
        lcd.clear();
        lcdCenter(0, "OTA Update");
        lcdCenter(1, "Do not power off!");
        saveLastTime(); saveLastAction(); saveRecentLogs();
      });
      ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        static int last_pct = -1;
        int pct = progress / (total / 100);
        if (pct != last_pct) {
          last_pct = pct;
          char buf[21];
          snprintf(buf, sizeof(buf), "Progress: %d%%", pct);
          lcdPad(2, 0, buf);
        }
      });
      ArduinoOTA.onEnd([]() {
        debugPrint("[OTA] Done");
        lcd.clear();
        lcdCenter(0, "OTA Complete!");
        lcdCenter(1, "Rebooting...");
        delay(500);
      });
      ArduinoOTA.onError([](ota_error_t error) {
        debugPrintF("[OTA] Error %u", error);
        lcd.clear();
        lcdCenter(0, "OTA Error!");
      });
      ArduinoOTA.begin();
      ota_enabled = true;  // [FIX-25] only set when OTA actually started
      debugPrint("[OTA] Ready");
    } else {
      debugPrint("[mDNS] Start failed");
    }

    // [FIX-27] Three NTP fallback servers
    configTime(0, 0, NTP1, NTP2, NTP3);
    delay(100);
    int nTry = 0;
    while (getNow() < 1000000000UL && nTry < 15) {
      delay(400); nTry++; yield();
    }
    if (getNow() > 1000000000UL) {
      time_synced = true;
      formatLocal_buf(getNow(), last_sync_str, sizeof(last_sync_str));
      saveLastTime();
      datestamp_buf(getNow(), current_day_stamp, sizeof(current_day_stamp));
    } else {
      lcdCenter(2, "NTP fail, using saved");
    }
    delay(500);
  } else {
    lcdCenter(2, "WiFi FAILED");
    lcdCenter(3, "Offline mode");
    delay(500);
  }

  // Fingerprint sensor
  lcd.clear();
  lcdCenter(0, "* Attendance Sys *");
  lcdCenter(1, "Checking sensor...");
  if (initSensor()) {
    lcdCenter(1, "Sensor OK");
    lcd.setCursor(9, 1); lcd.write(byte(1));
    finger.getTemplateCount();
    char tbuf[21];
    snprintf(tbuf, sizeof(tbuf), "%d fingerprints", finger.templateCount);
    lcdCenter(2, tbuf);
    snprintf(tbuf, sizeof(tbuf), "%d students loaded", student_count);
    lcdCenter(3, tbuf);
  } else {
    lcdCenter(1, "!! SENSOR FAIL !!");
    lcdCenter(2, "Web UI still works");
    lcdCenter(3, "Check wiring+restart");
  }
  delay(800);

  // Routes
  server.on("/",               HTTP_GET, handleRoot);
  server.on("/enroll",         HTTP_GET, handleEnroll);
  server.on("/delete",         HTTP_GET, handleDelete);
  server.on("/clear",          HTTP_GET, handleClear);
  server.on("/manual",         HTTP_GET, handleManual);         // [NEW-25]
  server.on("/ntfy/config",    HTTP_GET, handleNtfyConfig);     // [NEW-24]
  server.on("/manifest.json",  HTTP_GET, handleManifest);       // [NEW-27]
  server.on("/api/status",     HTTP_GET, handleAPIStatus);
  server.on("/api/attendance", HTTP_GET, handleAttendanceDownload);
  server.on("/api/students",   HTTP_GET, handleAPIStudents);
  server.on("/api/dates",      HTTP_GET, handleAPIDates);
  server.on("/api/stats",      HTTP_GET, handleAPIStats);
  server.on("/api/absent",     HTTP_GET, handleAPIAbsent);      // [NEW-27]
  server.on("/api/schedule",   HTTP_GET, handleAPISchedule);    // [FIX-26]
  server.on("/67",             HTTP_GET, handleEasterEgg67);

  server.begin();
  debugPrint("Web server started on port 80");

  showBootAnimation();

  debugPrintF("[HEAP] Free: %u bytes", ESP.getFreeHeap());
  debugPrintF("[FLASH] Used: %d%%", flashUsedPct());
  debugPrint("=== Setup complete v4.1 ===");
  if (wifi_connected)
    debugPrintF("IP: %s  mDNS: http://f-att.local", WiFi.localIP().toString().c_str());
  if (strlen(ntfy_url) > 0)
    debugPrintF("[NTFY] Push active: %s", ntfy_url);
}

// ============================================================
// LOOP
// ============================================================
void loop() {
  if (ota_enabled) ArduinoOTA.handle();
  if (wifi_connected) MDNS.update();

  server.handleClient();
  yield();

  // NTP re-sync every 30 min — [FIX-27] three servers already set in configTime
  if (wifi_connected && (millis() - last_ntp_ms > 1800000UL)) {
    last_ntp_ms = millis();
    configTime(0, 0, NTP1, NTP2, NTP3);
    delay(100); yield();
    unsigned long waitStart = millis();
    while (getNow() < 1000000000UL && millis() - waitStart < 2000) {
      delay(100); yield();
    }
    if (getNow() > 1000000000UL) {
      time_synced = true;
      formatLocal_buf(getNow(), last_sync_str, sizeof(last_sync_str));
      saveLastTime();
    }
  }

  // Periodic time save
  if (millis() - last_save_time_ms > 600000UL) {
    last_save_time_ms = millis();
    saveLastTime();
  }

  // [FIX-28] Flush dirty log to flash — debounced, not on every scan
  if (logs_dirty && (millis() - last_log_flush_ms > LOG_FLUSH_DEBOUNCE_MS)) {
    saveRecentLogs();
    // saveRecentLogs() resets logs_dirty and last_log_flush_ms internally
  }

  // WiFi reconnect check
  if (millis() - last_wifi_ms > 30000UL) {
    last_wifi_ms = millis();
    if (WiFi.status() != WL_CONNECTED) {
      wifi_connected = false;
      WiFi.reconnect();
    } else {
      wifi_connected = true;
    }
  }

  // Midnight rollover
  {
    char now_day[10];
    datestamp_buf(getNow(), now_day, sizeof(now_day));
    if (strcmp(now_day, "00000000") != 0 &&
        strcmp(now_day, current_day_stamp) != 0) {
      debugPrint("[DAY] Midnight rollover");
      strncpy(current_day_stamp, now_day, sizeof(current_day_stamp));
      today_scans = 0;
      logCount = 0;
      memset(logs, 0, sizeof(logs));
      clearLastActionForNewDay();
      if (LittleFS.exists("/recent_log.dat")) LittleFS.remove("/recent_log.dat");
      logs_dirty = false;
      auto_restarted_today = false;
    }
  }

  checkSensorHealth();
  checkHeapHealth();
  checkScheduledRestart();

  // ── FINGERPRINT SCAN ──
  uint16_t id = getFingerprintID();

  if (id > 0) {
    if (id < 128) {
      unsigned long now_ms = millis();
      if (now_ms - lastScanTime[id] < (unsigned long)SCAN_DEBOUNCE_SEC * 1000UL) {
        delay(50);
        return;
      }
      lastScanTime[id] = now_ms;
    }

    time_t utc_t = getNow();
    char localStr[30];
    formatLocal_buf(utc_t, localStr, sizeof(localStr));
    int idx = findStudent(id);

    const char* lesson = getCurrentLesson(utc_t);

    if (idx >= 0) {
      Student& s = students[idx];

      const char* action;
      if (id < 128) {
        if (lastAction[id] != 1) { action = "IN";  lastAction[id] = 1; }
        else                     { action = "OUT"; lastAction[id] = 2; }
      } else {
        action = "IN";
      }

      char fullName[33];
      snprintf(fullName, sizeof(fullName), "%s %s", s.first, s.last_name);
      debugPrintF("SCAN: %s [%s] @ %s", fullName, action, localStr);

      appendAttendance(s, action, utc_t, lesson);
      saveLastAction();

      int slot;
      if (logCount < MAX_LOG_ENTRIES) {
        slot = logCount++;
      } else {
        for (int i = 0; i < MAX_LOG_ENTRIES - 1; i++) logs[i] = logs[i+1];
        slot = MAX_LOG_ENTRIES - 1;
      }
      memset(&logs[slot], 0, sizeof(LogEntry));
      strncpy(logs[slot].name,     fullName,  sizeof(logs[slot].name)-1);
      strncpy(logs[slot].time_str, localStr,  sizeof(logs[slot].time_str)-1);
      strncpy(logs[slot].action,   action,    sizeof(logs[slot].action)-1);
      strncpy(logs[slot].lesson,   lesson ? lesson : "General",
              sizeof(logs[slot].lesson)-1);

      today_scans++;
      strncpy(last_scanned_name, fullName, sizeof(last_scanned_name)-1);
      last_scanned_name[sizeof(last_scanned_name)-1] = '\0';

      // [FIX-28] Mark dirty instead of immediate write
      logs_dirty = true;

      // [NEW-24] ntfy.sh push notification on scan
      char ntfyTitle[48], ntfyBody[80];
      snprintf(ntfyTitle, sizeof(ntfyTitle), "%s — %s", fullName, action);
      snprintf(ntfyBody, sizeof(ntfyBody), "%s | %s", lesson ? lesson : "General", localStr);
      sendNtfyNotification(ntfyTitle, ntfyBody);

      playSuccessTone();
      showSuccessWithFade(fullName, action, localStr, lesson);
    } else {
      // [FIX-22] Was a silent fail — now shows diagnostic on LCD
      debugPrintF("[ORPHAN] Sensor ID %d matched but no CSV record", id);
      playErrorTone();
      showOrphanAnimation(id);
    }

    lcd.clear();
  }

  updateIdleDisplay();
  delay(50);
}

/* ============================================================
   CHANGELOG  v4.0 -> v4.1

   [FIX-20] LittleFS directory iteration + deletion race condition
     - clearAllAttendanceData() previously called remove() while
       iterating openDir(), which is undefined behaviour in LittleFS
       and can skip files or corrupt the directory index
     - Fix: two-pass approach — collect all matching filenames into
       a String array first, then delete them in a separate loop

   [FIX-21] JSON injection via unescaped student names / lessons
     - All handleAPI*() functions concatenated raw user-controlled
       strings directly into JSON, so a name containing " or \
       broke the parser and could crash the web UI
     - Added jsonEscape(src, dst, len) helper — replaces " -> \"
       and \ -> \\; applied to every string field in every API
       response: names, lessons, time strings

   [FIX-22] Silent failure when sensor matched but no CSV record
     - When getFingerprintID() returned a valid ID but findStudent()
       returned -1 (template exists in sensor, deleted from CSV),
       the scan was silently dropped with no buzzer or LCD message
     - Added showOrphanAnimation() with playErrorTone() — displays
       "ORPHAN SCAN / Sensor ID: N / Re-enroll to fix" for 2s

   [FIX-23] appendUnknown() called before NTP sync
     - getFingerprintID() called appendUnknown(getNow()) regardless
       of whether time was valid, writing "N/A" timestamps into the
       unknowns CSV before NTP synced
     - Added early return in appendUnknown() when utc_t < 1e9

   [FIX-24] Deleted student debounce timer persisted
     - handleDelete() cleared lastAction[id] but not lastScanTime[id]
     - If the slot was re-enrolled within SCAN_DEBOUNCE_SEC of the
       previous student's last scan, the new student's first scan
       was silently rejected by the debounce check
     - Fix: lastScanTime[id] = 0 added alongside lastAction[id] = 0

   [FIX-25] ArduinoOTA.begin() called unconditionally
     - ArduinoOTA.begin() was called outside the if(MDNS.begin())
       block so OTA initialised even when mDNS failed, but
       ota_enabled stayed false, so ArduinoOTA.handle() was never
       called — OTA was listening but unserviced
     - Fix: moved ArduinoOTA setup + begin() + ota_enabled = true
       fully inside the if(MDNS.begin()) block

   [FIX-26] Schedule hardcoded in both C++ and JavaScript
     - Any lesson-time change required editing two places and
       reflashing the firmware
     - Added /api/schedule endpoint that serialises the C++ schedule[]
       struct to JSON; the web UI now fetches it with loadSchedule()
       at startup instead of using a hardcoded JS array
     - Single source of truth: only the C++ struct needs editing

   [FIX-27] Single NTP server with no fallback
     - configTime() was called with only "pool.ntp.org"; if that
       server was unreachable, time sync silently failed
     - Now uses three-server form: pool.ntp.org, time.cloudflare.com,
       time.google.com — applied in both setup() and the 30-min
       re-sync in loop()

   [FIX-28] saveRecentLogs() written on every single scan
     - Calling saveRecentLogs() on every scan wrote the full log file
       to flash on each fingerprint event, adding unnecessary wear
     - Replaced with a dirty-flag pattern: logs_dirty = true on each
       change; loop() flushes to flash when dirty AND at least
       LOG_FLUSH_DEBOUNCE_MS (30s) have elapsed since last flush
     - Critical paths (heap critical, OTA start, scheduled restart)
       still call saveRecentLogs() directly and are unaffected

   [FIX-29] Admin password compared with == (timing oracle)
     - server.arg("pass") != String(ADMIN_PASS) short-circuits on the
       first mismatched character, enabling timing-based attacks on
       a local network
     - Replaced with checkAdminPass() — constant-time XOR comparison
       that always runs for the full password length regardless of
       where the first mismatch occurs
     - Applied to all handlers: enroll, delete, clear, manual, ntfy

   [FIX-30] server.handleClient() missing in enroll blocking loops
     - During enrollment (up to ~26s total), the main loop was
       completely blocked so HTTP requests from the web UI timed out
     - Added server.handleClient() + ArduinoOTA.handle() inside all
       three blocking while() loops in handleEnroll() (scan1 wait,
       finger-lift wait, scan2 wait)

   [NEW-24] ntfy.sh push notifications
     - sendNtfyNotification(title, body) fires an HTTP POST to a
       configurable ntfy.sh URL on every successful scan and manual
       entry — teacher's phone gets an instant per-scan ping
     - URL stored in /ntfy_url.txt on LittleFS, loaded at boot
     - Configurable at runtime via web UI Settings card (⚙) without
       reflashing — saved with admin password authentication
     - Test ping sent when URL is saved for verification
     - Uses ESP8266HTTPClient with 2s timeout to minimise scan delay
     - Sending an empty URL disables notifications

   [NEW-25] Manual attendance entry via web UI
     - New ✏ Manual button in the header opens a card with a student
       dropdown (loaded from /api/students), IN/OUT selector, and
       admin password field
     - /manual GET endpoint writes to the attendance CSV and in-memory
       log exactly like a real fingerprint scan
     - Manual entries also trigger ntfy.sh notifications if configured
     - Useful when the sensor is offline or a student forgets to scan
     - Logs dirty-flagged for deferred flash write (same as real scans)

   [NEW-26] Flash storage monitoring + write guards
     - flashHasSpace(needed) and flashUsedPct() helpers using
       LittleFS.info() — called before every write operation
     - appendAttendance(), addStudentToCSV(), appendUnknown() all
       check flashHasSpace() and skip the write with a debug print
       if flash is too full, preventing silent data loss
     - Flash usage % included in /api/status JSON as "flash_pct"
     - Web UI shows a Flash Used stat box with a live progress bar
       that turns yellow at 60% and red at 80%
     - flashUsedPct() printed to Serial in setup() for reference

   [NEW-27] New API endpoints + PWA manifest
     - /api/absent: returns all students with lastAction==0 during
       active lesson, annotated with lesson name and during_lesson flag
     - /api/schedule: serialises the C++ schedule[] to JSON —
       consumed by the JS updateSchedule() to eliminate duplication
     - /manifest.json: minimal PWA manifest with theme-color, display
       standalone, and name — lets Android/iOS prompt "Add to Home
       Screen" for the web UI
     - Absent students box rendered in web UI below recent attendance:
       shows a green "All present" card or a red absent-names table
       depending on who has lastAction==0 during the active lesson

   [FIX-31] lcdCenter() left ghost characters from previous content
     - lcdCenter() only wrote the centered text — it never cleared the
       rest of the row. If a previous string was longer (e.g. idle display
       "Place finger to scan" vs "Scan 1 of 2"), old characters remained
       visible on both sides, producing garbled output like
       "- s: none | xtransf ng"
     - Fix: added a full 20-space row clear at the cursor start before
       writing the centered text
     - Also replaced lcdCenter() with lcdPad() throughout handleEnroll()
       for deterministic full-row writes

   [FIX-32] server.arg().c_str() — dangling pointer (root cause of all reported bugs)
     - server.arg("x") returns a temporary String by value. Calling .c_str()
       on it stores a pointer to that temporary's internal buffer. The
       temporary is destroyed at the end of the statement, so sid_c, first_c,
       last_c all point to freed memory immediately after assignment.
     - When these pointers were later used (lcdCenter, snprintf, strncpy into
       the student struct, addStudentToCSV), they read garbage — this caused:
         * Garbled LCD text during enrollment ("- s: none | xtransf ng")
         * Empty / corrupted student list after enrollment
         * Heap corruption leading to ESP8266 crash and web UI going offline
     - Fix: copy each server.arg() into a local char array inside a scoped
       block so the temporary String outlives the .c_str() call. Applied to
       handleEnroll() (sid, first, last), handleManual() (action),
       handleNtfyConfig() (url)

   [FIX-33] server.handleClient() inside enroll blocking loops was re-entrant
     - Adding server.handleClient() inside the three enroll wait loops
       (FIX-30) caused re-entrant handler invocations — ESP8266WebServer is
       not designed to handle this and can corrupt internal socket/buffer state
     - Also, the JS web UI shows "-" during the 26s enrollment regardless
       because the enrolling connection holds the socket; calling handleClient()
       doesn't help that connection
     - Fix: removed server.handleClient() and ArduinoOTA.handle() from the
       blocking loops. yield() + ESP.wdtFeed() + delay() is sufficient to
       keep the WDT happy. The UI recovers normally on the next 7s poll cycle
       after enrollment completes.

   ============================================================ */
