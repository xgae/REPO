/* IoT Attendance System v3.0 - ESP8266 Fixed Compilation
   (c) Fajer Abednabie - Basrah, Iraq
   Hardware: Wemos D1 + AS608 + LCD I2C 0x27 */

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <SoftwareSerial.h>
#include <Adafruit_Fingerprint.h>
#include <LiquidCrystal_I2C.h>
#include <LittleFS.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>

// ============================================================
// CONFIGURATION
// ============================================================
const char* WIFI_SSID     = "Abd";
const char* WIFI_PASS     = "fajer67ii";
const char* ADMIN_PASS    = "fajer67student";  // [NEW-05]
const long  TZ_OFFSET_S   = 3L * 3600L;       // UTC +3
const char* NTP_SERVER    = "pool.ntp.org";

#define FP_CONFIDENCE_THRESHOLD 50
#define SCAN_DEBOUNCE_SEC       3
#define SENSOR_HEALTH_INTERVAL_MS 300000UL
#define SENSOR_INIT_RETRIES     3
#define HEAP_CRITICAL_THRESHOLD 4096
#define AUTO_RESTART_HOUR       3

// ============================================================
// HARDWARE INSTANCES
// ============================================================
LiquidCrystal_I2C lcd(0x27, 20, 4);
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
// STUDENT DATABASE - NO class field [NEW-09]
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
// ATTENDANCE LOG - persistent [NEW-04]
// ============================================================
struct LogEntry {
  char name    [33];
  char time_str[24];
  char action  [ 4];
  char lesson  [28];  // [NEW-16]
};

#define MAX_LOG_ENTRIES 40
LogEntry logs[MAX_LOG_ENTRIES];
int  logCount    = 0;
int  today_scans = 0;

// ============================================================
// SCHEDULE DEFINITIONS [NEW-16/17]
// Sunday (wday=0): Computer Maintenance - 3 sessions
// Monday (wday=1): Circuit Design + Networking
// ============================================================
struct ScheduleSlot {
  uint8_t     day_of_week;  // 0=Sun, 1=Mon
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

// ============================================================
// DEBUG - [FIX-13] char[] not String concat
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

// [NEW-16] Get current lesson based on schedule
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

// ── NTP persistence ─────────────────────────────────────
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

// ── IN/OUT state persistence ────────────────────────────
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

// ── [NEW-04] Persistent log save/load ───────────────────
void saveRecentLogs() {
  File f = LittleFS.open("/recent_log.dat", "w");
  if (!f) return;
  f.write((uint8_t*)&logCount, sizeof(logCount));
  f.write((uint8_t*)&today_scans, sizeof(today_scans));
  f.write((uint8_t*)current_day_stamp, 10);
  for (int i = 0; i < logCount && i < MAX_LOG_ENTRIES; i++) {
    f.write((uint8_t*)&logs[i], sizeof(LogEntry));
  }
  f.close();
}

void loadRecentLogs() {
  if (!LittleFS.exists("/recent_log.dat")) return;
  File f = LittleFS.open("/recent_log.dat", "r");
  if (!f) return;
  int saved_count = 0, saved_scans = 0;
  char saved_day[10] = {};
  if (f.read((uint8_t*)&saved_count, sizeof(saved_count)) != sizeof(saved_count)) { f.close(); return; }
  if (f.read((uint8_t*)&saved_scans, sizeof(saved_scans)) != sizeof(saved_scans)) { f.close(); return; }
  f.read((uint8_t*)saved_day, 10);

  // Only load if same day
  if (strcmp(saved_day, current_day_stamp) == 0) {
    if (saved_count < 0 || saved_count > MAX_LOG_ENTRIES) { f.close(); return; }
    logCount = saved_count;
    today_scans = saved_scans;
    for (int i = 0; i < logCount; i++) {
      if (f.read((uint8_t*)&logs[i], sizeof(LogEntry)) != sizeof(LogEntry)) {
        logCount = i;
        break;
      }
    }
    debugPrintF("[LOG] Restored %d entries from flash", logCount);
  } else {
    debugPrint("[LOG] Saved logs from different day - ignoring");
  }
  f.close();
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
  f.readStringUntil('\n'); // skip header

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

// [NEW-01] Delete student from CSV array and rewrite
bool deleteStudentFromCSV(uint16_t tid) {
  int idx = findStudent(tid);
  if (idx < 0) return false;
  for (int i = idx; i < student_count - 1; i++)
    students[i] = students[i+1];
  student_count--;
  rewriteStudentsCSV();
  return true;
}

// [NEW-09] Simplified attendance CSV - no device_id, class, template_id, timezone
void appendAttendance(const Student& s, const char* action, time_t utc_t,
                      const char* lesson) {
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
  bool exists = LittleFS.exists("/unknowns.csv");
  File f = LittleFS.open("/unknowns.csv", "a");
  if (!f) return;
  if (!exists) f.println(F("timestamp_local"));
  char local_str[30];
  formatLocal_buf(utc_t, local_str, sizeof(local_str));
  f.println(local_str);
  f.close();
}

// [NEW-02] Clear all attendance data
void clearAllAttendanceData() {
  Dir dir = LittleFS.openDir("/");
  while (dir.next()) {
    yield();
    String fn = dir.fileName();
    if (fn.startsWith("att_") || fn == "unknowns.csv") {
      LittleFS.remove("/" + fn);
    }
  }
  logCount = 0;
  today_scans = 0;
  memset(logs, 0, sizeof(logs));
  if (LittleFS.exists("/recent_log.dat")) LittleFS.remove("/recent_log.dat");
  clearLastActionForNewDay();
  last_scanned_name[0] = '\0';
  debugPrint("[CLEAR] All attendance data cleared");
}

// ============================================================
// LCD UTILITIES & ANIMATIONS
// ============================================================
void initCustomChars() {
  lcd.createChar(0, CH_FINGER);
  lcd.createChar(1, CH_CHECK);
  lcd.createChar(2, CH_CROSS);
  lcd.createChar(3, CH_SP1);
  lcd.createChar(4, CH_SP2);
  lcd.createChar(5, CH_BLOCK);
  lcd.createChar(6, CH_HALF);
  lcd.createChar(7, CH_DOT);
}

void lcdCenter(int row, const char* text) {
  int len = strlen(text);
  if (len > 20) len = 20;
  int col = (20 - len) / 2;
  lcd.setCursor(col, row);
  char buf[21];
  strncpy(buf, text, 20);
  buf[20] = '\0';
  lcd.print(buf);
}

void lcdPad(int row, int col, const char* text) {
  lcd.setCursor(col, row);
  int maxLen = 20 - col;
  char buf[21];
  int tlen = strlen(text);
  int i;
  for (i = 0; i < maxLen && i < tlen; i++) buf[i] = text[i];
  for (; i < maxLen; i++) buf[i] = ' ';
  buf[maxLen] = '\0';
  lcd.print(buf);
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
  if (wifi_connected) {
    char ip_buf[20];
    WiFi.localIP().toString().toCharArray(ip_buf, sizeof(ip_buf));
    lcdCenter(3, ip_buf);
  } else {
    lcdCenter(3, "No WiFi");
  }
  delay(1500);
  lcd.clear();
}

// [NEW-03] Fade-out animation - name dissolves right-to-left
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

  // Show lesson if available
  if (lesson && strlen(lesson) > 0) {
    snprintf(buf, sizeof(buf), " %s|%s", action, lesson);
  } else {
    snprintf(buf, sizeof(buf), " Action: %s", action);
  }
  lcdPad(2, 0, buf);
  snprintf(buf, sizeof(buf), " %s", timeStr);
  lcdPad(3, 0, buf);

  delay(1200);

  // Fade-out: replace chars with spaces right-to-left
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
  if (wifi_connected) {
    char ip_buf[20];
    WiFi.localIP().toString().toCharArray(ip_buf, sizeof(ip_buf));
    lcdCenter(3, ip_buf);
  } else {
    lcdCenter(3, "No WiFi");
  }
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

// [NEW-08] No device ID on idle display, [NEW-16] shows lesson
void updateIdleDisplay() {
  if (millis() - last_idle_ms < 1000) return;
  last_idle_ms = millis();

  time_t t = getNow();

  // Row 0: Status + lesson
  char hdr[21];
  if (!sensor_available)
    strncpy(hdr, "!SENSOR OFFLINE!", sizeof(hdr));
  else if (wifi_connected)
    snprintf(hdr, sizeof(hdr), "WiFi%c Attendance", (char)1);
  else
    strncpy(hdr, "[NoWiFi] Attend.", sizeof(hdr));
  lcdPad(0, 0, hdr);

  // Row 1: Time
  char time_buf[12];
  formatLCDTime_buf(t, time_buf, sizeof(time_buf));
  char row1[21];
  snprintf(row1, sizeof(row1), "  %s", time_buf);
  lcdPad(1, 0, row1);

  // Row 2: Date or current lesson
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

  // Row 3: Last scan or prompt
  if (last_scanned_name[0] != '\0') {
    char s[21];
    snprintf(s, sizeof(s), "%c %s", (char)7, last_scanned_name);
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
  if (finger.getImage()         != FINGERPRINT_OK) return 0;
  if (finger.image2Tz()         != FINGERPRINT_OK) return 0;
  if (finger.fingerFastSearch() != FINGERPRINT_OK) return 0;
  if (finger.confidence < FP_CONFIDENCE_THRESHOLD) {
    debugPrintF("[AS608] Low confidence: %d", finger.confidence);
    showLowConfidenceAnimation(finger.confidence);
    return 0;
  }
  debugPrintF("[AS608] Match ID=%d conf=%d", finger.fingerID, finger.confidence);
  return finger.fingerID;
}

// [NEW-01] Delete fingerprint from sensor
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
// [NEW-14/FIX-14] HEAP & STABILITY MONITORING
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

// [NEW-15] Scheduled 3AM auto-restart as safety net
void checkScheduledRestart() {
  time_t t = getNow();
  if (t < 1000000000UL) return;
  time_t local = t + TZ_OFFSET_S;
  struct tm* ti = gmtime(&local);
  if (ti->tm_hour == AUTO_RESTART_HOUR && ti->tm_min == 0 && !auto_restarted_today) {
    // Only restart if uptime > 12 hours
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
// WEB UI HTML
// ============================================================
void sendHTML() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html; charset=utf-8", "");

  // Part 1: Head + CSS
  server.sendContent_P(PSTR(
    "<!DOCTYPE html><html><head>"
    "<meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>IoT Attendance</title>"
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
    ".stat{flex:1;min-width:100px;"
          "background:rgba(10,132,255,.08);"
          "border:1px solid rgba(10,132,255,.2);"
          "border-radius:10px;padding:12px;text-align:center}"
    ".val{font-size:22px;font-weight:700;color:var(--acc)}"
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
    "input{background:rgba(255,255,255,.06);"
           "border:1px solid rgba(255,255,255,.12);"
           "border-radius:8px;padding:7px 10px;color:var(--txt);"
           "font-size:13px;width:100%}"
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
    ".ee{color:transparent;cursor:default;transition:color .3s}"
    ".ee:hover,.ee:active{color:var(--acc)}"
    "@keyframes fadeIn{from{opacity:0;transform:translateY(-8px)}to{opacity:1;transform:none}}"
    ".fade-row{animation:fadeIn .4s ease-out}"
    "</style></head><body><div class=w>"
  ));

  // Part 2: Header - [NEW-06] 👨🏻‍🎓, [NEW-08] no device info
  server.sendContent_P(PSTR(
    "<div class=hdr>"
      "<div>"
        "<h1>\xF0\x9F\x91\xA8\xF0\x9F\x8F\xBB\xE2\x80\x8D\xF0\x9F\x8E\x93 IoT Attendance System</h1>"
        "<div class=sub>Iraq/Basrah</div>"
      "</div>"
      "<div style='display:flex;gap:6px;flex-wrap:wrap'>"
        "<button class='btn-s' onclick=dlCSV()>&#8595; CSV</button>"
        "<button class='btn-s btn-g' onclick=showEnroll()>+ Enroll</button>"
        "<button class='btn-s btn-r' onclick=showDelete()>&#128465; Delete</button>"
        "<button class='btn-s btn-w' onclick=showClear()>&#128465; Clear Logs</button>"
      "</div>"
    "</div>"
  ));

  // Part 3: Stats
  server.sendContent_P(PSTR(
    "<div class=card><div class=row>"
      "<div class=stat><div class=val id=sc>-</div><div class=lbl>Today Scans</div></div>"
      "<div class=stat><div class=val id=up>-</div><div class=lbl>Uptime</div></div>"
      "<div class=stat><div class=val id=ts>-</div><div class=lbl>NTP Sync</div></div>"
      "<div class=stat><div class=val id=st>-</div><div class=lbl>Students</div></div>"
      "<div class=stat><div class=val id=sn>-</div><div class=lbl>Sensor</div></div>"
      "<div class=stat><div class=val id=hp>-</div><div class=lbl>Free RAM</div></div>"
    "</div>"
    "<div id=lessonInfo></div>"
    "</div>"
  ));

  // Part 4: Schedule card [NEW-16]
  server.sendContent_P(PSTR(
    "<div class=card>"
      "<h2>\xF0\x9F\x93\x85 Today's Schedule</h2>"
      "<div id=schedBox><div class=sched>Loading schedule...</div></div>"
    "</div>"
  ));

  // Part 5: Enroll card - [NEW-09] no class, [NEW-12] placeholders
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
        "<button class=btn-c onclick=hideEnroll()>Cancel</button>"
      "</div>"
      "<div id=eMsg></div>"
    "</div>"
  ));

  // Part 6: Delete card [NEW-01]
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
        "<button class=btn-c onclick=hideDelete()>Cancel</button>"
      "</div>"
      "<div id=dMsg></div>"
      "<div id=studentList style='margin-top:10px'></div>"
    "</div>"
  ));

  // Part 7: Clear logs card [NEW-02]
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
        "<button class=btn-c onclick=hideClear()>Cancel</button>"
      "</div>"
      "<div id=cMsg></div>"
    "</div>"
  ));

  // Part 8: Recent attendance table
  server.sendContent_P(PSTR(
    "<div class=card>"
      "<div style='display:flex;justify-content:space-between;align-items:center'>"
        "<h2>Recent Attendance <span class=pill id=rCnt>0</span></h2>"
        "<div class=sub id=lastSync></div>"
      "</div>"
      "<div id=rTable>"
        "<div style='color:var(--muted);text-align:center;padding:20px'>Loading&hellip;</div>"
      "</div>"
    "</div>"
  ));

  // Part 9: Footer with easter eggs [NEW-13]
  server.sendContent_P(PSTR(
    "<div class=footer>"
      "&copy; <span class=ee title='You found it! 67 forever &#127881;' "
      "onclick='alert(\"&#127881; Easter Egg #1: 67 is the magic number! Fajer was here. &#128526;\")'"
      ">Fajer Abednabie</span> &bull; Basrah, Iraq &bull; "
      "Data stored in device flash"
      "<!-- Easter Egg #2: View source? Nice! The answer is always 67. -->"
      "<div style='margin-top:4px;font-size:10px;color:rgba(147,171,207,.3)' "
      "onclick='this.style.color=\"var(--warn)\";this.textContent=\"&#127881; Easter Egg #3! Secret code: F67A &#128526;\"'"
      ">v3.0</div>"
    "</div></div>"
  ));

  // Part 10: JavaScript
  server.sendContent_P(PSTR(
    "<script>"
    // Easter egg: type "67" [NEW-13]
    "let ek='';"
    "document.addEventListener('keydown',e=>{"
      "ek+=e.key;if(ek.length>10)ek=ek.slice(-10);"
      "if(ek.includes('67')){"
        "document.title='\\uD83C\\uDF89 67! You found the secret!';"
        "setTimeout(()=>document.title='IoT Attendance',3000);"
        "ek='';"
      "}"
    "});"

    "const SCHED=["
      "{d:0,sh:8,sm:0,eh:9,em:0,n:'Computer Maintenance - Session 1'},"
      "{d:0,sh:9,sm:15,eh:10,em:15,n:'Computer Maintenance - Session 2'},"
      "{d:0,sh:10,sm:30,eh:11,em:30,n:'Computer Maintenance - Session 3'},"
      "{d:1,sh:8,sm:0,eh:9,em:30,n:'Electronic Circuit Design'},"
      "{d:1,sh:9,sm:45,eh:11,em:15,n:'Networking'}"
    "];"
    "const DAYS=['Sunday','Monday','Tuesday','Wednesday','Thursday','Friday','Saturday'];"

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
        // Lesson badge
        "if(j.current_lesson){"
          "document.getElementById('lessonInfo').innerHTML="
            "'<div class=lesson-badge style=\"margin-top:10px\">\\uD83D\\uDCDA Now: '+j.current_lesson+'</div>';"
        "}else{"
          "document.getElementById('lessonInfo').innerHTML="
            "'<div style=\"margin-top:8px;color:var(--muted);font-size:12px\">No active lesson</div>';"
        "}"
        "const rows=j.logs.map((e,i)=>"
          "`<tr class=fade-row style='animation-delay:${i*50}ms'>"
          "<td>${e.time}</td><td>${e.name}</td>"
          "<td><span class='${e.action.toLowerCase()}'>${e.action}</span></td>"
          "<td style='font-size:11px;color:var(--muted)'>${e.lesson||''}</td></tr>`"
        ").join('');"
        "document.getElementById('rTable').innerHTML=rows"
          "?`<table><thead><tr><th>Time</th><th>Name</th><th>Action</th><th>Lesson</th></tr></thead>"
             "<tbody>${rows}</tbody></table>`"
          ":'<div style=\"color:var(--muted);text-align:center;padding:20px\">"
             "No scans yet today</div>';"
        "updateSchedule();"
      "}catch(e){}"
    "}"
    "function fmt(s){"
      "if(s===67)return '1h 7m \\u2728';"
      "return Math.floor(s/3600)+'h '+Math.floor(s%3600/60)+'m';"
    "}"
    "function dlCSV(){location.href='/api/attendance?date=today'}"
  ));

  server.sendContent_P(PSTR(
    "function showEnroll(){hide('dCard');hide('cCard');show('eCard');clr('eMsg')}"
    "function hideEnroll(){hide('eCard')}"
    "function showDelete(){hide('eCard');hide('cCard');show('dCard');clr('dMsg');loadStudents()}"
    "function hideDelete(){hide('dCard')}"
    "function showClear(){hide('eCard');hide('dCard');show('cCard');clr('cMsg')}"
    "function hideClear(){hide('cCard')}"
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
        "if(r.ok){load();setTimeout(hideEnroll,3500);}"
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
        "if(r.ok){load();setTimeout(hideClear,2000);}"
      "}catch(e){setMsg('cMsg','\\u274c Connection error','err');}"
    "}"

    "function setMsg(id,m,c){"
      "const d=document.getElementById(id);"
      "d.innerHTML=m;d.className='msg '+(c==='ok'?'ok-msg':'err-msg');"
    "}"

    "load();setInterval(load,7000);setInterval(updateSchedule,60000);"
    "</script></body></html>"
  ));
}

// ============================================================
// WEB HANDLERS
// ============================================================
void handleRoot() { sendHTML(); }

void handleAPIStatus() {
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

  // Current lesson [NEW-16]
  const char* lesson = getCurrentLesson(getNow());
  server.sendContent("\"current_lesson\":");
  if (lesson) {
    server.sendContent("\"");
    server.sendContent(lesson);
    server.sendContent("\",");
  } else {
    server.sendContent("null,");
  }

  server.sendContent("\"last_sync\":\"");
  server.sendContent(last_sync_str);
  server.sendContent("\",\"logs\":[");

  bool first = true;
  for (int i = logCount - 1; i >= 0; i--) {
    if (!first) server.sendContent(",");
    first = false;
    server.sendContent("{\"time\":\"");
    server.sendContent(logs[i].time_str);
    server.sendContent("\",\"name\":\"");
    server.sendContent(logs[i].name);
    server.sendContent("\",\"action\":\"");
    server.sendContent(logs[i].action);
    server.sendContent("\",\"lesson\":\"");
    server.sendContent(logs[i].lesson);
    server.sendContent("\"}");
    yield();
  }
  server.sendContent("]}");
}

// Student list API for delete UI
void handleAPIStudents() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  server.sendContent("{\"students\":[");
  bool first = true;
  for (int i = 0; i < student_count; i++) {
    if (!first) server.sendContent(",");
    first = false;
    char buf[100];
    snprintf(buf, sizeof(buf), "{\"fid\":%d,\"sid\":\"%s\",\"name\":\"%s %s\"}",
             students[i].template_id, students[i].student_id,
             students[i].first, students[i].last_name);
    server.sendContent(buf);
    yield();
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
  server.streamFile(f, "text/csv");
  f.close();
}

void handleEnroll() {
  debugPrint("=== ENROLL START ===");

  if (!sensor_available) {
    server.send(503, "text/plain", "Sensor offline - check wiring and restart");
    return;
  }
  if (server.arg("pass") != String(ADMIN_PASS)) {
    server.send(403, "text/plain", "Wrong password");
    return;
  }

  uint16_t id = (uint16_t)server.arg("id").toInt();
  if (id == 0 || id > 127) {
    server.send(400, "text/plain", "Invalid ID (must be 1-127)");
    return;
  }

  int existing = findStudent(id);
  const char* sid_c   = server.hasArg("sid")   ? server.arg("sid").c_str()   : "";
  const char* first_c = server.hasArg("first") ? server.arg("first").c_str() : "Student";
  const char* last_c  = server.hasArg("last")  ? server.arg("last").c_str()  : "";

  char fullName[33];
  snprintf(fullName, sizeof(fullName), "%s %s", first_c, last_c);

  // SCAN 1
  lcd.clear();
  char idbuf[21];
  snprintf(idbuf, sizeof(idbuf), "Enrolling ID: %d", id);
  lcdCenter(0, idbuf);
  lcd.setCursor(0, 1); lcd.write(byte(0));
  lcdCenter(1, "  Scan 1 of 2");
  lcdCenter(2, "Place finger now");
  char nameBuf[21];
  strncpy(nameBuf, fullName, 20); nameBuf[20] = '\0';
  lcdCenter(3, nameBuf);

  int p = -1, timeout = 0;
  while (p != FINGERPRINT_OK && timeout < 50) {
    p = finger.getImage();
    delay(200); yield(); timeout++;
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

  // REMOVE
  lcd.clear();
  lcdCenter(0, idbuf);
  lcdCenter(1, "Remove finger");
  lcd.setCursor(9, 2); lcd.write(byte(1));
  timeout = 0;
  while (finger.getImage() != FINGERPRINT_NOFINGER && timeout < 30) {
    delay(200); yield(); timeout++;
  }
  delay(500);

  // SCAN 2
  lcd.clear();
  lcdCenter(0, idbuf);
  lcd.setCursor(0, 1); lcd.write(byte(0));
  lcdCenter(1, "  Scan 2 of 2");
  lcdCenter(2, "Place finger again");

  p = -1; timeout = 0;
  while (p != FINGERPRINT_OK && timeout < 50) {
    p = finger.getImage();
    delay(200); yield(); timeout++;
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

  // CREATE & STORE
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
  lcd.setCursor(0, 0); lcd.write(byte(1)); lcd.write(byte(1));
  lcdCenter(0, "  Enroll OK!  ");
  lcdCenter(1, nameBuf);
  snprintf(idbuf, sizeof(idbuf), "ID %d saved", id);
  lcdCenter(2, idbuf);
  lcd.setCursor(0, 3); lcd.write(byte(1));
  lcdCenter(3, " Stored!");

  char resp[64];
  snprintf(resp, sizeof(resp), "Enrolled: %s (ID %d)", fullName, id);
  server.send(200, "text/plain", resp);
  delay(1500);
  lcd.clear();
}

// [NEW-01] Delete handler
void handleDelete() {
  debugPrint("=== DELETE START ===");

  if (server.arg("pass") != String(ADMIN_PASS)) {
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

// [NEW-02] Clear all attendance handler
void handleClear() {
  if (server.arg("pass") != String(ADMIN_PASS)) {
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

// [NEW-13] Easter egg endpoint
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
    "Easter Egg #5: The magic number lives here forever.(i use Arch Linux btw.)</div>"
    "</body></html>");
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

  // LCD
  lcd.init();
  lcd.backlight();
  initCustomChars();
  lcd.clear();
  lcdCenter(0, "* Attendance Sys *");
  lcdCenter(2, "Initializing...");
  lcdCenter(3, "(c) F.Abednabie");
  delay(800);

  // LittleFS
  lcd.clear();
  lcdCenter(0, "* Attendance Sys *");
  lcdCenter(1, "Mounting storage...");
  if (initFS()) {
    lcdCenter(2, "Storage OK");
    loadStudentsCSV();
    loadLastTime();
    loadLastAction();
  } else {
    lcdCenter(2, "Storage WARN");
  }
  delay(500);

  // Initialize day stamp before loading logs
  datestamp_buf(getNow(), current_day_stamp, sizeof(current_day_stamp));

  // [NEW-04] Load persistent logs
  loadRecentLogs();

  // WiFi - [FIX-16] persistent(false)
  lcd.clear();
  lcdCenter(0, "* Attendance Sys *");
  lcdCenter(1, "Connecting WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
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

    configTime(0, 0, NTP_SERVER);
    delay(100);
    int nTry = 0;
    while (getNow() < 1000000000UL && nTry < 15) {
      delay(400); nTry++; yield();
    }
    if (getNow() > 1000000000UL) {
      time_synced = true;
      formatLocal_buf(getNow(), last_sync_str, sizeof(last_sync_str));
      saveLastTime();
      // Re-calculate day stamp with real time
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

  // Web server routes
  server.on("/",               HTTP_GET, handleRoot);
  server.on("/enroll",         HTTP_GET, handleEnroll);
  server.on("/delete",         HTTP_GET, handleDelete);
  server.on("/clear",          HTTP_GET, handleClear);
  server.on("/api/status",     HTTP_GET, handleAPIStatus);
  server.on("/api/attendance", HTTP_GET, handleAttendanceDownload);
  server.on("/api/students",   HTTP_GET, handleAPIStudents);
  server.on("/67",             HTTP_GET, handleEasterEgg67);

  server.begin();
  debugPrint("Web server started on port 80");

  // Boot animation
  showBootAnimation();

  debugPrintF("[HEAP] Free: %u bytes", ESP.getFreeHeap());
  debugPrint("=== Setup complete v3.0 ===");
  if (wifi_connected)
    debugPrintF("IP: %s", WiFi.localIP().toString().c_str());
}

// ============================================================
// LOOP
// ============================================================
void loop() {
  server.handleClient();
  yield();

  // NTP re-sync every 30 minutes
  if (wifi_connected && (millis() - last_ntp_ms > 1800000UL)) {
    last_ntp_ms = millis();
    configTime(0, 0, NTP_SERVER);
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

  // Periodic time save (every 10 min)
  if (millis() - last_save_time_ms > 600000UL) {
    last_save_time_ms = millis();
    saveLastTime();
  }

  // WiFi reconnect check (every 30s)
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
      auto_restarted_today = false;
    }
  }

  // Sensor health check
  checkSensorHealth();

  // [FIX-14] Heap monitoring
  checkHeapHealth();

  // [NEW-15] Scheduled auto-restart
  checkScheduledRestart();

  // ── FINGERPRINT SCAN ──
  uint16_t id = getFingerprintID();

  if (id > 0) {
    // Per-student debounce
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

    // Get current lesson [NEW-16]
    const char* lesson = getCurrentLesson(utc_t);

    if (idx >= 0) {
      Student& s = students[idx];

      // IN / OUT toggle
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

      // Update RAM log
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

      // [NEW-04] Persist logs
      saveRecentLogs();

      showSuccessWithFade(fullName, action, localStr, lesson);

    } else {
      debugPrintF("SCAN: Unknown template %d", id);
      appendUnknown(utc_t);
      showUnknownAnimation();
    }

    lcd.clear();
  }

  // Idle display (non-blocking)
  updateIdleDisplay();

  delay(50);
}

/* ============================================================
   FULL CHANGELOG  v2.1 -> v3.0

   [NEW-01] Fingerprint deletion via web UI
     - /delete endpoint with admin password protection
     - /api/students endpoint lists all enrolled students
     - deleteFingerprint() calls finger.deleteModel()
     - deleteStudentFromCSV() removes from array and rewrites file
     - LCD shows deletion confirmation

   [NEW-02] Clear attendance/logs button
     - /clear endpoint with password + "CONFIRM" text verification
     - clearAllAttendanceData() removes all att_*.csv files
     - Clears RAM logs, today_scans, IN/OUT state, persistent logs
     - Students remain enrolled (only attendance is deleted)

   [NEW-03] LCD fade-out animation
     - showSuccessWithFade() displays match then dissolves name
       right-to-left, character by character, for visual polish
     - 80ms per character with yield() for WDT safety

   [NEW-04] Persistent attendance log (survives reboot)
     - saveRecentLogs() writes logCount + today_scans + day stamp
       + log entries to /recent_log.dat as binary
     - loadRecentLogs() restores on boot if same day
     - Web UI shows continuous log even after power interruption

   [NEW-05] Admin password changed to "fajer67student"
     - WiFi password and admin password are now separate
     - ADMIN_PASS constant used for enroll, delete, clear

   [NEW-06] Emoji changed from 🎓 to 👨🏻‍🎓
     - UTF-8 sequence \xF0\x9F\x91\xA8\xF0\x9F\x8F\xBB\xE2\x80\x8D\xF0\x9F\x8E\x93

   [NEW-07] Timezone display "Iraq/Basrah" in web UI header

   [NEW-08] Removed device_id from web UI, LCD idle, and CSV
     - LCD idle shows "WiFi✓ Attendance" instead of device name

   [NEW-09] Removed class field entirely
     - Student struct simplified, CSV headers updated
     - Enroll form has no class input

   [NEW-10] Removed template_id from downloadable CSV

   [NEW-11] Removed timezone column from downloadable CSV

   [NEW-12] Placeholder text updates
     - "e.g." -> "ex:" everywhere
     - Name placeholders: "fajer" and "Abednabie"
     - ID placeholder: "ex: 67"

   [NEW-13] Hidden easter eggs (5 total)
     - #1: Click copyright name -> alert with 67 message
     - #2: HTML comment in source code
     - #3: Click "v3.0" text -> reveals secret code F67A
     - #4: Uptime exactly 67s shows sparkle in web UI
     - #5: /67 secret page with celebration animation
     - Keyboard: type "67" -> title changes temporarily

   [NEW-14] Anti-restart measures
     - WiFi.persistent(false) stops flash writes every boot
     - WiFi.setAutoReconnect(true) for robust reconnection
     - Heap monitoring with HEAP_CRITICAL_THRESHOLD (4KB)
     - Emergency state save before any forced restart
     - Extra yield() calls throughout all loops and animations

   [NEW-15] Scheduled auto-restart at 3AM
     - Only triggers if uptime > 12 hours (prevents boot loops)
     - Saves time, IN/OUT state, and logs before restart
     - auto_restarted_today flag prevents multiple restarts

   [NEW-16] Schedule-aware attendance
     - ScheduleSlot struct with full weekly practical schedule
     - Sunday: 3 sessions Computer Maintenance (1hr + 15min breaks)
     - Monday: Electronic Circuit Design + Networking
     - getCurrentLesson() determines active lesson by time
     - Lesson name recorded in attendance CSV and web UI
     - LCD idle screen shows current lesson when active
     - Web UI has live schedule card with "NOW" indicator

   [NEW-17] Session tracking
     - Each scan records which lesson/session it belongs to
     - LogEntry has lesson field
     - Web UI attendance table includes Lesson column

   [FIX-13] Stack overflow prevention
     - All debug output uses char[] buffers via debugPrintF()
     - Fixed char buffers for day stamps instead of String
     - Reduced local buffer sizes throughout

   [FIX-14] Heap fragmentation watchdog
     - checkHeapHealth() monitors every 60s
     - If free heap < 4KB -> emergency save + restart
     - Free heap displayed in web UI stats

   [FIX-15] SoftwareSerial buffer overflow protection
     - 150ms delay after finger.begin() in initSensor()
     - yield() after every sensor operation loop iteration

   [FIX-16] WiFi flash wear prevention
     - WiFi.persistent(false) prevents writing credentials
       to flash on every boot (major cause of flash wear)
     - WiFi.setAutoReconnect(true) for background recovery
   ============================================================ */
