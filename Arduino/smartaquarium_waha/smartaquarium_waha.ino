#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ESP32Servo.h>
#include <Adafruit_NeoPixel.h>
#include <WiFi.h>
#include <WebServer.h>  // API HTTP lokal untuk controller WAHA
#include <time.h>
#include <RTClib.h>  // Library untuk RTC DS3231

// ==================== PIN ASSIGNMENTS & CONFIG (ESP32-S3) ====================
#define ONE_WIRE_BUS       8    
#define TRIG_PIN           48   
#define ECHO_PIN           47   
#define SERVO_PIN          7    
#define LED_RGB_PIN        2    
#define ULTRASONIC_FEED_THRESHOLD   10.0f    //ultradsonik 1 pakan (cm)
#define QUICK_FEED_COOLDOWN        3000UL    
#define SDA_PIN           38     
#define SCL_PIN           39     

// I2C terpisah khusus RTC DS3231
#define RTC_SDA_PIN       17
#define RTC_SCL_PIN       16

// ======== TAMBAHAN: Ultrasonik #2 & Buzzer ========
#define TRIG2_PIN          5  // Ultrasonik 2 (level air) - TRIG
#define ECHO2_PIN          4  // Ultrasonik 2 (level air) - ECHO
#define BUZZER_PIN         14   // Buzzer aktif

// Threshold level air (jarak sensor ke permukaan air, cm)
#define WATER_HIGH_MAX_CM  3.0f   // terlalu tinggi bila jarak < 3 cm (bunyi)
#define WATER_LOW_MIN_CM   5.0f   // terlalu rendah bila jarak > 5 cm (bunyi)

// Batas suhu normal ikan hias tropis (°C) — ubah jika perlu
#define TEMP_MIN_C         24.0f
#define TEMP_MAX_C         30.0f

// Konfigurasi buzzer pakai tone() / nada buzzer
#define BUZZ_FREQ_HZ       2000   // nada ~2 kHz

// --- Tambahan proteksi & histeresis ---
#define ULTRA_MAX_CM       300.0f      // bacaan di atas ini dianggap tidak wajar (timeout/jauh)
#define TEMP_HYST          1.0f        // histeresis suhu ±1°C untuk mencegah flip-flop

bool isTempValid(float t) {
  // DS18B20: 85.0 = belum selesai konversi, -127 = error
  if (t == 85.0f || t == -127.0f) return false;
  // batas aman umum sensor
  return (t > -40.0f && t < 85.0f);
}

const char* ssid     = "";  // Wi-Fi lokal yang sama dengan laptop
const char* password = ""; 
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 7 * 3600;  
const int   daylightOffset_sec = 0 ;  
const char* localApiKey = "";  // samakan dengan ESP32_API_KEY di .env

// ==================== OBJEK GLOBAL ====================
WebServer server(80);  // API hanya digunakan di jaringan lokal
Adafruit_NeoPixel led(1, LED_RGB_PIN, NEO_GRB + NEO_KHZ800);
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
LiquidCrystal_I2C lcd(0x27, 16, 2);
Servo myServo;

// RTC
RTC_DS3231 rtc;  // Menambahkan objek RTC DS3231
TwoWire I2C_RTC = TwoWire(1);  // I2C terpisah khusus RTC

time_t nextFeedTime1 = 0;
time_t nextFeedTime2 = 0;
String lcdLine1 = "";
String lcdLine2 = "";
unsigned long lastQuickFeedMillis = 0;
String pendingFeedSet = "";

bool alreadyFed1 = false;
bool alreadyFed2 = false;

// ==================== DEKLARASI FUNGSI ====================
void feedFish();
String formatTime(struct tm timeinfo);
String formatShortTime(time_t t);
bool setFeedTime(int feedNumber, String timeStr);
bool isValidHHMM(const String& value);
String buildInfoMessage();
bool apiAuthorized();
void setupLocalApi();
void handleApiInfo();
void handleApiFeed();
void handleApiSetFeed(int feedNumber);

// ======== TAMBAHAN: Deklarasi ========
void TaskWaterAndBuzzer(void* parameter);
float readUltrasonicCM(int trigPin, int echoPin);
void buzzerOn();
void buzzerOff();

// ==================== TASK SENSORS & LCD ====================
void TaskSensors(void* parameter) {
  unsigned long lastLCDTime = 0;
  static bool showFeed1 = true;  // Untuk bergantian feed1/feed2

  for (;;) {
    unsigned long currentMillis = millis();
    if (currentMillis - lastLCDTime >= 1000) {
      sensors.requestTemperatures();
      float suhu = sensors.getTempCByIndex(0);
      struct tm timeinfo;

      if (WiFi.status() == WL_CONNECTED) {
        // Jika online, ambil waktu dari NTP
        if (!getLocalTime(&timeinfo)) {
        Serial.println("Waktu NTP belum tersedia");
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        continue;
      }
      } else {
        // Jika offline, ambil waktu dari RTC
        if (!rtc.begin()) {
          Serial.println("ERROR: RTC not detected!");
          return;
        }
        DateTime now = rtc.now();
        timeinfo.tm_min = now.minute();
        timeinfo.tm_sec = now.second();
        Serial.println("Using RTC time...");
      }

      // Baris atas: suhu (kiri) + waktu (kanan)
      char jamStr[6];
      strftime(jamStr, sizeof(jamStr), "%H:%M", &timeinfo);
      lcdLine1 = String(suhu, 1) + (char)223 + "C"; 
      while (lcdLine1.length() < 11) lcdLine1 += " ";
      lcdLine1 += String(jamStr);

      // Baris bawah: Feed bergantian
      if (showFeed1 && nextFeedTime1 > 0) {
        lcdLine2 = "Feed1 " + formatShortTime(nextFeedTime1);
      } else if (!showFeed1 && nextFeedTime2 > 0) {
        lcdLine2 = "Feed2 " + formatShortTime(nextFeedTime2);
      } else {
        lcdLine2 = "Next --:--";
      }
      showFeed1 = !showFeed1;  // Toggle tiap detik

      lcd.clear();
      lcd.setCursor(0, 0); lcd.print(lcdLine1);
      lcd.setCursor(0, 1); lcd.print(lcdLine2);
      lastLCDTime = currentMillis;

      // Auto Feed
      time_t now = time(NULL);
      if (nextFeedTime1 > 0 && abs(difftime(now, nextFeedTime1)) < 2 && !alreadyFed1) {
        feedFish();
        alreadyFed1 = true;
      }
      if (nextFeedTime2 > 0 && abs(difftime(now, nextFeedTime2)) < 2 && !alreadyFed2) {
        feedFish();
        alreadyFed2 = true;
      }

      // Reset flag feed
      if (difftime(now, nextFeedTime1) > 60) alreadyFed1 = false;
      if (difftime(now, nextFeedTime2) > 60) alreadyFed2 = false;
    }

    vTaskDelay(10 / portTICK_PERIOD_MS); 
  }
}

// ==================== FORMAT WAKTU ====================
String formatTime(struct tm timeinfo) {
  char buffer[20];
  strftime(buffer, sizeof(buffer), "%H:%M:%S", &timeinfo);
  return String(buffer);
}

String formatShortTime(time_t t) {
  struct tm* timeinfo = localtime(&t);
  char buffer[6];
  strftime(buffer, sizeof(buffer), "%H:%M", timeinfo);
  return String(buffer);
}

// ==================== TASK ULTRASONIK ====================
void TaskUltrasonic(void* parameter) {
  unsigned long lastUltraTime = 0;
  for (;;) {
    unsigned long currentMillis = millis();
    if (currentMillis - lastUltraTime >= 1000) {
      digitalWrite(TRIG_PIN, LOW);
      delayMicroseconds(2);
      digitalWrite(TRIG_PIN, HIGH);
      delayMicroseconds(10);
      digitalWrite(TRIG_PIN, LOW);
      long duration = pulseIn(ECHO_PIN, HIGH, 3000UL); 
      if (duration > 0) {
        float jarak = (duration * 0.034f) / 2.0f; 
        if (jarak > 0 && jarak <= ULTRASONIC_FEED_THRESHOLD &&
            (currentMillis - lastQuickFeedMillis > QUICK_FEED_COOLDOWN)) {
          feedFish();
          lastQuickFeedMillis = currentMillis;
        }
      }
      lastUltraTime = currentMillis;
    }
    vTaskDelay(10 / portTICK_PERIOD_MS); 
  }
}

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  delay(1000);

 
  Serial.println();
  Serial.println("===== DEBUG BOOT =====");
  Serial.printf("Free heap: %u bytes\n", ESP.getFreeHeap());
  Serial.println("Masuk setup()");
  Serial.println("\n=== Memulai Sistem FishFeeder (ESP32-S3) ===");
  Wire.begin(SDA_PIN, SCL_PIN);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Inisialisasi...");

  sensors.begin();
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  myServo.setPeriodHertz(50);
  myServo.attach(SERVO_PIN, 500, 2400);
  myServo.write(0);

  led.begin();
  led.clear();
  led.show();

  Serial.println();
  Serial.println("========================================");
  Serial.println("     SMART AQUARIUM ESP32-S3");
  Serial.println("========================================");
  Serial.print("Menghubungkan ke WiFi: ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  int wifiAttempts = 0;
  while (WiFi.status() != WL_CONNECTED && wifiAttempts < 20) {
    delay(500);
    Serial.print(".");
    wifiAttempts++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Status WiFi : TERHUBUNG");
    Serial.print("Nama WiFi   : ");
    Serial.println(WiFi.SSID());
    Serial.print("IP ESP32-S3 : ");
    Serial.println(WiFi.localIP());
    Serial.print("API Lokal   : http://");
    Serial.println(WiFi.localIP());
    Serial.println("Port API    : 80");
    Serial.println("========================================");

    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    struct tm timeinfo;
    int retry = 0;
    while (!getLocalTime(&timeinfo) && retry < 10) {
      delay(1000);
      retry++;
    }
    if (retry == 10) {
      Serial.println("ERROR: Gagal sinkronisasi waktu NTP.");
    } else {
      Serial.println("NTP OK: Waktu tersinkron.");
    }
  } else {
    Serial.println("Status WiFi : GAGAL TERHUBUNG");
    Serial.println("IP ESP32-S3 : Belum tersedia");
    Serial.println("Periksa SSID/password dan jaringan WiFi.");
    Serial.println("========================================");
  }

  // ==================== Cek RTC DS3231 ====================
  // RTC menggunakan bus I2C terpisah: SDA GPIO 16, SCL GPIO 17
  I2C_RTC.begin(RTC_SDA_PIN, RTC_SCL_PIN);
  if (!rtc.begin(&I2C_RTC)) {
    Serial.println("ERROR: RTC not detected!");  // Jika RTC tidak terdeteksi
  } else {
    Serial.println("RTC OK: RTC terhubung!");   // Jika RTC berhasil terdeteksi
    DateTime now = rtc.now();
    (void)now;
  }

  // Kontrol jarak jauh melalui bridge WAHA lokal di laptop
  setupLocalApi();

  xTaskCreate(TaskSensors, "TaskSensors", 8192, NULL, 1, NULL);  
  xTaskCreate(TaskUltrasonic, "TaskUltrasonic", 8192, NULL, 2, NULL);  

  // ======== TAMBAHAN: Inisialisasi Ultrasonik #2 & Buzzer ========
  pinMode(TRIG2_PIN, OUTPUT);
  pinMode(ECHO2_PIN, INPUT);

  pinMode(BUZZER_PIN, OUTPUT);
  buzzerOff(); // pastikan diam

  // Task pemantau level air & suhu -> kontrol buzzer
  xTaskCreate(TaskWaterAndBuzzer, "TaskWaterAndBuzzer", 8192, NULL, 2, NULL);

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("FishFeeder Siap!");
  Serial.println("=== Setup Selesai ===");
}

// ==================== LOOP UTAMA ====================
void loop() {
  // Jalankan HTTP server lokal; tugas sensor tetap berjalan di FreeRTOS.
  server.handleClient();
  delay(2);
}

// ==================== FUNGSI PEMBERIAN MAKAN ====================
void feedFish() {
  myServo.write(160); //servo derajat
  delay(1000); //delaynya
  myServo.write(0);
}

// ==================== API LOKAL UNTUK WAHA ====================
// Python controller menerima pesan dari WAHA lalu memanggil endpoint ini.
// API dilindungi header X-API-Key; jangan expose port 80 ESP32 ke internet.

bool apiAuthorized() {
  String suppliedKey = server.header("X-API-Key");
  if (suppliedKey != String(localApiKey) ||
      String(localApiKey) == "REPLACE_WITH_RANDOM_32_CHAR_ESP32_API_KEY") {
    server.send(401, "text/plain; charset=utf-8", "Unauthorized: API key salah/belum diganti");
    return false;
  }
  return true;
}

bool isValidHHMM(const String& value) {
  if (value.length() != 5 || value.charAt(2) != ':') return false;
  if (!isDigit(value.charAt(0)) || !isDigit(value.charAt(1)) ||
      !isDigit(value.charAt(3)) || !isDigit(value.charAt(4))) return false;
  int hour = value.substring(0, 2).toInt();
  int minute = value.substring(3, 5).toInt();
  return hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59;
}

bool setFeedTime(int feedNumber, String timeStr) {
  if (!isValidHHMM(timeStr)) return false;

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return false;

  int hour = timeStr.substring(0, 2).toInt();
  int minute = timeStr.substring(3, 5).toInt();
  timeinfo.tm_hour = hour;
  timeinfo.tm_min = minute;
  timeinfo.tm_sec = 0;
  time_t scheduledTime = mktime(&timeinfo);
  if (scheduledTime <= 0) return false;

  if (feedNumber == 1) {
    nextFeedTime1 = scheduledTime;
  } else if (feedNumber == 2) {
    nextFeedTime2 = scheduledTime;
  } else {
    return false;
  }
  return true;
}

String buildInfoMessage() {
  // Pertahankan sumber data suhu yang sebelumnya digunakan perintah /info.
  float suhuNow = NAN;
  int degIdx = lcdLine1.indexOf((char)223);
  if (degIdx > 0) {
    String tstr = lcdLine1.substring(0, degIdx);
    suhuNow = tstr.toFloat();
  }

  float jarakNow = readUltrasonicCM(TRIG2_PIN, ECHO2_PIN);
  String feed1Str = (nextFeedTime1 > 0) ? formatShortTime(nextFeedTime1) : String("--:--");
  String feed2Str = (nextFeedTime2 > 0) ? formatShortTime(nextFeedTime2) : String("--:--");

  struct tm tnow;
  String nowStr = (getLocalTime(&tnow)) ? formatTime(tnow) : String("--:--:--");
  String ipStr = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : String("offline");

  String info = "*FishFeeder Info*\n";
  info += "Waktu: *" + nowStr + "*\n";
  info += "WiFi IP: `" + ipStr + "`\n\n";
  info += "*Suhu*: ";
  if (!isnan(suhuNow)) info += String(suhuNow, 1) + "°C\n";
  else info += "_invalid_\n";

  info += "*Level Air* (cm dari sensor): ";
  if (jarakNow >= 0) info += String(jarakNow, 1) + " cm\n";
  else info += "_invalid/timeout_\n";

  info += "• Aman: sekitar 4 cm (diam)\n";
  info += "• Terlalu tinggi (dekat sensor) jika < " + String(WATER_HIGH_MAX_CM, 1) + " cm (bunyi)\n";
  info += "• Terlalu rendah jika > " + String(WATER_LOW_MIN_CM, 1) + " cm (bunyi)\n\n";
  info += "*Jadwal Feed*\n";
  info += "• Feed 1: " + feed1Str + "\n";
  info += "• Feed 2: " + feed2Str + "\n";
  return info;
}

void handleApiInfo() {
  if (!apiAuthorized()) return;
  server.send(200, "text/plain; charset=utf-8", buildInfoMessage());
}

void handleApiFeed() {
  if (!apiAuthorized()) return;
  feedFish();
  server.send(200, "text/plain; charset=utf-8", "OK: Ikan telah diberi makan.");
}

void handleApiSetFeed(int feedNumber) {
  if (!apiAuthorized()) return;
  String timeStr = server.arg("time");
  if (!isValidHHMM(timeStr)) {
    server.send(400, "text/plain; charset=utf-8", "Format waktu salah. Gunakan HH:MM (00:00-23:59).");
    return;
  }
  if (!setFeedTime(feedNumber, timeStr)) {
    server.send(503, "text/plain; charset=utf-8", "Gagal mengatur jadwal: waktu belum tersedia atau input tidak valid.");
    return;
  }
  server.send(200, "text/plain; charset=utf-8",
              "OK: Jadwal Feed " + String(feedNumber) + " diatur ke " + timeStr + ".");
}

void setupLocalApi() {
  const char* headerKeys[] = {"X-API-Key"};
  server.collectHeaders(headerKeys, 1);

  server.on("/", HTTP_GET, []() {
    server.send(200, "text/plain; charset=utf-8",
                "SmartAquarium local API. Gunakan /api/health dengan header X-API-Key.");
  });

  server.on("/api/health", HTTP_GET, []() {
    if (!apiAuthorized()) return;
    server.send(200, "text/plain; charset=utf-8", "OK: SmartAquarium API siap.");
  });

  server.on("/api/info", HTTP_GET, handleApiInfo);
  server.on("/api/feed", HTTP_POST, handleApiFeed);
  server.on("/api/setfeed1", HTTP_POST, []() { handleApiSetFeed(1); });
  server.on("/api/setfeed2", HTTP_POST, []() { handleApiSetFeed(2); });

  server.onNotFound([]() {
    server.send(404, "text/plain; charset=utf-8", "Endpoint tidak ditemukan.");
  });

  server.begin();
  Serial.println("Local WAHA API started on port 80");
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("ESP32 API URL: http://");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("ESP32 API aktif, tetapi WiFi belum terhubung; IP belum tersedia.");
  }
}

// ======== TAMBAHAN: Helper buzzer ========
void buzzerOn()  { digitalWrite(BUZZER_PIN, HIGH); }
void buzzerOff() { digitalWrite(BUZZER_PIN, LOW); }

// ======== TAMBAHAN: Baca jarak Ultrasonik (dalam cm) ========
float readUltrasonicCM(int trigPin, int echoPin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);
  long duration = pulseIn(echoPin, HIGH, 30000UL); // timeout 30ms (~5m)
  if (duration <= 0) return -1.0f;  // invalid (timeout)
  float cm = (duration * 0.034f) / 2.0f;
  if (cm <= 0 || cm > ULTRA_MAX_CM) return -1.0f; // anggap invalid
  return cm;
}

// ======== TAMBAHAN: Task monitor level air & suhu, kontrol buzzer ========
void TaskWaterAndBuzzer(void* parameter) {
  const uint32_t intervalMs = 1000;   // cek tiap 1 detik
  uint32_t lastCheck = 0;

  // status untuk histeresis suhu
  static bool tempAlert = false;

  for (;;) {
    uint32_t nowMs = millis();
    if (nowMs - lastCheck >= intervalMs) {
      lastCheck = nowMs;

      // --- Baca suhu ---
      sensors.requestTemperatures();
      float suhu = sensors.getTempCByIndex(0);

      bool tempValid = isTempValid(suhu);
      bool tempTooCold = false, tempTooHot = false;

      if (tempValid) {
        // HISTERESIS: hanya clear alert bila sudah masuk "zona aman" + margin
        if (!tempAlert) {
          // masuk alert jika keluar rentang + histeresis
          tempTooCold = (suhu < (TEMP_MIN_C - TEMP_HYST));
          tempTooHot  = (suhu > (TEMP_MAX_C + TEMP_HYST));
          tempAlert   = tempTooCold || tempTooHot;
        } else {
          // saat alert, baru clear bila sudah cukup aman (di tengah dengan margin)
          bool inComfort = (suhu > (TEMP_MIN_C + TEMP_HYST)) && (suhu < (TEMP_MAX_C - TEMP_HYST));
          if (inComfort) tempAlert = false;
          // tandai arah alert agar pola tetap konsisten
          tempTooCold = (suhu < TEMP_MIN_C);
          tempTooHot  = (suhu > TEMP_MAX_C);
        }
      } else {
        // Bacaan sensor suhu invalid → tidak memicu alarm suhu
        tempTooCold = false; tempTooHot = false; tempAlert = false;
        // (Jika ingin justru memicu saat invalid, ubah: tempTooCold = true; tempAlert = true;)
      }

      // --- Baca level air dari Ultrasonik #2 ---
      float jarakAir = readUltrasonicCM(TRIG2_PIN, ECHO2_PIN);

      bool waterTooHigh = false;
      bool waterTooLow  = false;

      if (jarakAir < 0) {
        // INVALID/TIMEOUT → anggap AQUARIUM KOSONG (air terlalu rendah)
        waterTooLow = true;
      } else {
        // valid → pakai threshold
        waterTooHigh = (jarakAir < WATER_HIGH_MAX_CM);
        waterTooLow  = (jarakAir > WATER_LOW_MIN_CM);
      }

      bool anyAlert = waterTooHigh || waterTooLow || tempAlert;

      // Pola bunyi sederhana: jika alert, nyala-mati tiap loop ~1 detik
      static bool phase = false;
      if (anyAlert) {
        phase = !phase;
        if (phase) buzzerOn(); else buzzerOff();
      } else {
        buzzerOff();
      }
    }

    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

