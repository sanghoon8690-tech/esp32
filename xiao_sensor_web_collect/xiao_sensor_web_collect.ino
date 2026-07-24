// XIAO ESP32S3 - BNO055 gesture/sensor dataset collector, over Wi-Fi STA,
// with a web UI (label + record button), for Edge Impulse.
//
// Board joins an existing Wi-Fi network (STA mode) so it's reachable from
// any device on that network - no AP-roaming issues (see esp32/SKILL.md).
//
// - Live accel/gyro readout at /
// - Type a label, pick a duration, hit "캡처" to record a timed sample at
//   50Hz to onboard flash (LittleFS) as CSV:
//     timestamp_ms,accX,accY,accZ,gyrX,gyrY,gyrZ
//   (same axis order/units as XIAO_PROJECT/xiao_gesture_data_collect.ino)
// - /files lists saved captures with per-file download/delete, so they can
//   be pulled onto a PC and fed to Edge Impulse (Data acquisition -> CSV
//   Wizard, or `edge-impulse-uploader --label <label> <file>.csv`).

#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BNO055.h>
#include <WiFi.h>
#include <LittleFS.h>
#include "esp_http_server.h"
#include <map>

// ---- Wi-Fi STA settings: edit these ----
const char *STA_SSID     = "your-wifi-ssid";
const char *STA_PASSWORD = "your-wifi-password";
// -----------------------------------------

#define I2C_SDA 5
#define I2C_SCL 6
const unsigned long SAMPLE_INTERVAL_MS = 20; // 50Hz

Adafruit_BNO055 bno = Adafruit_BNO055(55, 0x29, &Wire);
bool bnoReady = false;

static httpd_handle_t server = NULL;

// ---------------- string / label helpers (same convention as xiao_webcam_ap.ino) ----------------

static String urlDecode(const char *src) {
  String out;
  while (*src) {
    if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
      auto hexval = [](char c) -> int {
        c = tolower(c);
        return (c >= '0' && c <= '9') ? c - '0' : c - 'a' + 10;
      };
      out += (char)(hexval(src[1]) * 16 + hexval(src[2]));
      src += 3;
    } else if (*src == '+') {
      out += ' ';
      src++;
    } else {
      out += *src++;
    }
  }
  return out;
}

static String sanitizeLabel(const String &raw) {
  String out;
  for (size_t i = 0; i < raw.length() && out.length() < 40; i++) {
    char c = raw[i];
    if (c == '/' || c == '\\' || c == '\0' || c == '"' || c == '?' || c == '*' || c == ':' || (unsigned char)c < 0x20) {
      out += '_';
    } else {
      out += c;
    }
  }
  if (out.length() == 0) out = "idle";
  return out;
}

static int countFilesWithPrefix(const String &label) {
  int count = 0;
  String prefix = "/" + label + "_";
  File root = LittleFS.open("/");
  File file = root.openNextFile();
  while (file) {
    String name = String(file.name());
    if (!name.startsWith("/")) name = "/" + name;
    if (name.startsWith(prefix)) count++;
    file = root.openNextFile();
  }
  return count;
}

static std::map<String, int> labelCounters;

static int nextIndexForLabel(const String &label) {
  auto it = labelCounters.find(label);
  int next;
  if (it == labelCounters.end()) {
    next = countFilesWithPrefix(label) + 1;
  } else {
    next = it->second;
  }
  labelCounters[label] = next + 1;
  return next;
}

// ---------------- page ----------------

static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<title>XIAO Sensor Collect</title>
<style>
  body{background:#111;color:#eee;margin:0;display:flex;flex-direction:column;align-items:center;padding:16px;gap:10px;font-family:sans-serif}
  input{font-size:16px;padding:8px;border-radius:6px;border:none;text-align:center}
  #label{width:160px} #duration{width:90px}
  button{font-size:18px;padding:10px 20px;border:none;border-radius:8px;background:#2d7;color:#000}
  button:disabled{background:#555;color:#aaa}
  .row{display:flex;gap:8px;align-items:center;flex-wrap:wrap;justify-content:center}
  #status{color:#8f8;font-size:14px;min-height:18px}
  #live{font-family:monospace;font-size:13px;color:#aaf;white-space:pre;background:#000;padding:8px;border-radius:6px}
  a{color:#5cf}
</style></head>
<body>
<h2>XIAO BNO055 - 센서 데이터 수집 (STA)</h2>
<div id="live">accX=0 accY=0 accZ=0
gyrX=0 gyrY=0 gyrZ=0</div>
<div class="row">
  <input id="label" placeholder="라벨 (예: swipe_left)" value="idle">
  <input id="duration" type="number" value="2000" min="200" step="100"> ms
  <button id="btn" onclick="record()">캡처</button>
</div>
<div id="status"></div>
<div class="row"><a href="/files">저장된 파일 보기 →</a></div>
<script>
function poll() {
  fetch('/sample').then(r => r.json()).then(d => {
    document.getElementById('live').textContent =
      'accX=' + d.accX.toFixed(2) + ' accY=' + d.accY.toFixed(2) + ' accZ=' + d.accZ.toFixed(2) + '\n' +
      'gyrX=' + d.gyrX.toFixed(2) + ' gyrY=' + d.gyrY.toFixed(2) + ' gyrZ=' + d.gyrZ.toFixed(2) +
      '\ncalib sys=' + d.sys + ' gyro=' + d.gyro + ' accel=' + d.accel + ' mag=' + d.mag;
  }).catch(()=>{});
}
setInterval(poll, 300);
poll();

function record() {
  const label = document.getElementById('label').value || 'idle';
  const duration = document.getElementById('duration').value || 2000;
  const btn = document.getElementById('btn');
  const status = document.getElementById('status');
  btn.disabled = true;
  status.textContent = '녹화 중... (' + duration + 'ms)';
  fetch('/start?label=' + encodeURIComponent(label) + '&duration=' + duration)
    .then(r => r.json())
    .then(d => { status.textContent = d.file + ' 저장됨 (' + d.samples + '개 샘플, 라벨 내 ' + d.count + '번째)'; })
    .catch(() => { status.textContent = '저장 실패'; })
    .finally(() => { btn.disabled = false; });
}
</script>
</body></html>
)rawliteral";

// ---------------- handlers ----------------

static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, INDEX_HTML, strlen(INDEX_HTML));
}

static esp_err_t sample_handler(httpd_req_t *req) {
  char json[220] = "{\"accX\":0,\"accY\":0,\"accZ\":0,\"gyrX\":0,\"gyrY\":0,\"gyrZ\":0,\"sys\":0,\"gyro\":0,\"accel\":0,\"mag\":0}";
  if (bnoReady) {
    sensors_event_t accelData, gyroData;
    bno.getEvent(&accelData, Adafruit_BNO055::VECTOR_ACCELEROMETER);
    bno.getEvent(&gyroData, Adafruit_BNO055::VECTOR_GYROSCOPE);
    uint8_t sys, gyro, accel, mag;
    bno.getCalibration(&sys, &gyro, &accel, &mag);
    snprintf(json, sizeof(json),
        "{\"accX\":%.4f,\"accY\":%.4f,\"accZ\":%.4f,\"gyrX\":%.4f,\"gyrY\":%.4f,\"gyrZ\":%.4f,"
        "\"sys\":%d,\"gyro\":%d,\"accel\":%d,\"mag\":%d}",
        accelData.acceleration.x, accelData.acceleration.y, accelData.acceleration.z,
        gyroData.gyro.x, gyroData.gyro.y, gyroData.gyro.z, sys, gyro, accel, mag);
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, json, strlen(json));
}

static esp_err_t start_handler(httpd_req_t *req) {
  if (!bnoReady) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  String label = "idle";
  unsigned long duration = 2000;
  char query[128];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    char val[64];
    if (httpd_query_key_value(query, "label", val, sizeof(val)) == ESP_OK) {
      label = sanitizeLabel(urlDecode(val));
    }
    if (httpd_query_key_value(query, "duration", val, sizeof(val)) == ESP_OK) {
      duration = strtoul(val, NULL, 10);
      if (duration < 200) duration = 200;
      if (duration > 20000) duration = 20000;
    }
  }

  int idx = nextIndexForLabel(label);
  char path[96];
  snprintf(path, sizeof(path), "/%s_%03d.csv", label.c_str(), idx);

  File f = LittleFS.open(path, FILE_WRITE);
  if (!f) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  f.println("timestamp_ms,accX,accY,accZ,gyrX,gyrY,gyrZ");

  unsigned long start = millis();
  unsigned long lastSample = 0;
  int samples = 0;
  while (millis() - start < duration) {
    unsigned long now = millis();
    if (now - lastSample >= SAMPLE_INTERVAL_MS) {
      lastSample = now;

      sensors_event_t accelData, gyroData;
      bno.getEvent(&accelData, Adafruit_BNO055::VECTOR_ACCELEROMETER);
      bno.getEvent(&gyroData, Adafruit_BNO055::VECTOR_GYROSCOPE);

      f.printf("%lu,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n",
          now - start,
          accelData.acceleration.x, accelData.acceleration.y, accelData.acceleration.z,
          gyroData.gyro.x, gyroData.gyro.y, gyroData.gyro.z);
      samples++;
    }
    vTaskDelay(1); // yield so this busy-wait doesn't trip the watchdog
  }
  f.close();

  char json[160];
  snprintf(json, sizeof(json), "{\"ok\":true,\"file\":\"%s\",\"count\":%d,\"samples\":%d}", path + 1, idx, samples);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, json, strlen(json));
}

static esp_err_t list_handler(httpd_req_t *req) {
  String json = "[";
  bool first = true;
  File root = LittleFS.open("/");
  File file = root.openNextFile();
  while (file) {
    String name = String(file.name());
    if (!name.startsWith("/")) name = "/" + name;
    if (!first) json += ",";
    first = false;
    json += "{\"name\":\"" + name.substring(1) + "\",\"size\":" + String(file.size()) + "}";
    file = root.openNextFile();
  }
  json += "]";
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, json.c_str(), json.length());
}

static esp_err_t download_handler(httpd_req_t *req) {
  char query[96], val[64];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "file", val, sizeof(val)) != ESP_OK) {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }
  String name = urlDecode(val);
  String path = "/" + name;
  File f = LittleFS.open(path, FILE_READ);
  if (!f) {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "text/csv");
  String cd = "attachment; filename=\"" + name + "\"";
  httpd_resp_set_hdr(req, "Content-Disposition", cd.c_str());

  static uint8_t buf[1024];
  esp_err_t res = ESP_OK;
  size_t n;
  while ((n = f.read(buf, sizeof(buf))) > 0) {
    res = httpd_resp_send_chunk(req, (const char *)buf, n);
    if (res != ESP_OK) break;
  }
  f.close();
  httpd_resp_send_chunk(req, NULL, 0);
  return res;
}

static esp_err_t delete_handler(httpd_req_t *req) {
  char query[96], val[64];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "file", val, sizeof(val)) != ESP_OK) {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }
  String path = "/" + urlDecode(val);
  LittleFS.remove(path);
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, "ok", 2);
}

static esp_err_t files_handler(httpd_req_t *req) {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><title>Captures</title>";
  html += "<style>body{background:#111;color:#eee;font-family:sans-serif;padding:16px}";
  html += "a{color:#5cf} .row{display:flex;justify-content:space-between;border-bottom:1px solid #333;padding:6px 0}";
  html += ".del{color:#f66;margin-left:12px}</style></head><body>";
  html += "<p><a href='/'>&larr; 돌아가기</a></p><h2>저장된 CSV</h2>";
  html += "<p>" + String(LittleFS.usedBytes() / 1024) + "KB / " + String(LittleFS.totalBytes() / 1024) + "KB 사용 중</p>";

  File root = LittleFS.open("/");
  File file = root.openNextFile();
  int n = 0;
  while (file) {
    String name = String(file.name());
    if (!name.startsWith("/")) name = "/" + name;
    name = name.substring(1);
    html += "<div class='row'><span>" + name + " (" + String(file.size()) + "B)</span>";
    html += "<span><a href='/download?file=" + name + "'>다운로드</a>";
    html += " <a class='del' href='#' onclick=\"if(confirm('삭제할까요?')){fetch('/delete?file=" + name + "').then(()=>location.reload())}return false;\">삭제</a></span></div>";
    file = root.openNextFile();
    n++;
  }
  if (n == 0) html += "<p>저장된 파일이 없습니다.</p>";
  html += "</body></html>";

  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, html.c_str(), html.length());
}

static void startServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.max_uri_handlers = 8;
  config.stack_size = 8192;

  httpd_uri_t index_uri    = { .uri = "/",         .method = HTTP_GET, .handler = index_handler,    .user_ctx = NULL };
  httpd_uri_t sample_uri   = { .uri = "/sample",   .method = HTTP_GET, .handler = sample_handler,   .user_ctx = NULL };
  httpd_uri_t start_uri    = { .uri = "/start",    .method = HTTP_GET, .handler = start_handler,    .user_ctx = NULL };
  httpd_uri_t list_uri     = { .uri = "/list",     .method = HTTP_GET, .handler = list_handler,     .user_ctx = NULL };
  httpd_uri_t files_uri    = { .uri = "/files",    .method = HTTP_GET, .handler = files_handler,    .user_ctx = NULL };
  httpd_uri_t download_uri = { .uri = "/download", .method = HTTP_GET, .handler = download_handler, .user_ctx = NULL };
  httpd_uri_t delete_uri   = { .uri = "/delete",   .method = HTTP_GET, .handler = delete_handler,   .user_ctx = NULL };

  if (httpd_start(&server, &config) == ESP_OK) {
    httpd_register_uri_handler(server, &index_uri);
    httpd_register_uri_handler(server, &sample_uri);
    httpd_register_uri_handler(server, &start_uri);
    httpd_register_uri_handler(server, &list_uri);
    httpd_register_uri_handler(server, &files_uri);
    httpd_register_uri_handler(server, &download_uri);
    httpd_register_uri_handler(server, &delete_uri);
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
  }

  Wire.begin(I2C_SDA, I2C_SCL);
  bnoReady = bno.begin();
  if (bnoReady) {
    delay(200);
    bno.setExtCrystalUse(true);
    Serial.println("BNO055 detected OK.");
  } else {
    Serial.println("ERROR: BNO055 not detected - check wiring/address (0x29).");
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(STA_SSID, STA_PASSWORD);
  Serial.print("Connecting to Wi-Fi");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Connected. Open: http://");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("Wi-Fi connect FAILED - check SSID/password.");
  }

  startServer();
}

void loop() {
  delay(10000);
}
