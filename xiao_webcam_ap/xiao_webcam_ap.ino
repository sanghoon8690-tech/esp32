/*
 * XIAO ESP32S3 Sense - camera stream + Teachable-Machine-style dataset
 * capture, over its own Wi-Fi AP
 *
 * Board:  Seeed XIAO ESP32S3 Sense  (fqbn esp32:esp32:XIAO_ESP32S3)
 * Core:   ESP32 Arduino core 3.x
 *
 * The board starts its own Wi-Fi access point (no router needed).
 * Connect a phone/laptop to that AP, then open http://192.168.4.1/
 *
 * - Live stream at /
 * - Type a label, tap "캡처" to save the current frame to onboard flash
 *   (LittleFS) under that label, Teachable-Machine style.
 * - /files lists everything saved, with per-file download/delete links,
 *   so captured sets can be pulled onto a PC and fed to
 *   `edge-impulse-uploader --label <label> <files>`.
 *
 * Onboard flash capacity note: the default partition scheme gives ~1.5MB
 * for LittleFS, which is roughly 50-90 QVGA JPEGs at this quality setting.
 * Download and clear out old captures via /files once it starts filling up.
 */

#include "esp_camera.h"
#include <WiFi.h>
#include <LittleFS.h>
#include "esp_http_server.h"
#include <vector>

// ---- Wi-Fi AP settings: edit these ----
const char *AP_SSID     = "XIAO-CAM";
const char *AP_PASSWORD = "12345678";   // must be 8+ chars, or "" for an open network
// ----------------------------------------

// Camera pin map for XIAO ESP32S3 Sense (OV2640)
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     10
#define SIOD_GPIO_NUM     40
#define SIOC_GPIO_NUM     39
#define Y9_GPIO_NUM       48
#define Y8_GPIO_NUM       11
#define Y7_GPIO_NUM       12
#define Y6_GPIO_NUM       14
#define Y5_GPIO_NUM       16
#define Y4_GPIO_NUM       18
#define Y3_GPIO_NUM       17
#define Y2_GPIO_NUM       15
#define VSYNC_GPIO_NUM    38
#define HREF_GPIO_NUM     47
#define PCLK_GPIO_NUM     13

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *_STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *_STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *_STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static httpd_handle_t stream_httpd = NULL;

// ---------------- small string helpers ----------------

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

// Keep it filesystem-safe; UTF-8 (e.g. Korean) labels pass through as-is.
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
  if (out.length() == 0) {
    out = "unlabeled";
  }
  return out;
}

static int countFilesWithPrefix(const String &label) {
  int count = 0;
  String prefix = "/" + label + "_";
  File root = LittleFS.open("/");
  File file = root.openNextFile();
  while (file) {
    String name = String(file.name());
    if (!name.startsWith("/")) {
      name = "/" + name;
    }
    if (name.startsWith(prefix)) {
      count++;
    }
    file = root.openNextFile();
  }
  return count;
}

// ---------------- minimal ZIP writer (stored/no compression) ----------------

static uint32_t crc32_table[256];
static bool crc32_table_ready = false;

static void crc32_init() {
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++) {
      c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
    }
    crc32_table[i] = c;
  }
  crc32_table_ready = true;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *buf, size_t len) {
  if (!crc32_table_ready) {
    crc32_init();
  }
  crc ^= 0xFFFFFFFF;
  for (size_t i = 0; i < len; i++) {
    crc = crc32_table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFF;
}

static void put_u16(uint8_t *p, uint16_t v) {
  p[0] = v & 0xFF;
  p[1] = (v >> 8) & 0xFF;
}

static void put_u32(uint8_t *p, uint32_t v) {
  p[0] = v & 0xFF;
  p[1] = (v >> 8) & 0xFF;
  p[2] = (v >> 16) & 0xFF;
  p[3] = (v >> 24) & 0xFF;
}

// ---------------- page templates ----------------

static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<title>XIAO ESP32S3 Cam</title>
<style>
  body{background:#111;color:#eee;margin:0;display:flex;flex-direction:column;align-items:center;padding:16px;gap:10px;font-family:sans-serif}
  img{max-width:100%;height:auto;border-radius:8px}
  input{font-size:16px;padding:8px;border-radius:6px;border:none;width:200px;text-align:center}
  button{font-size:18px;padding:10px 20px;border:none;border-radius:8px;background:#2d7;color:#000}
  button:active{background:#1a5}
  .secondary{background:#444;color:#eee}
  .row{display:flex;gap:8px;align-items:center;flex-wrap:wrap;justify-content:center}
  #status{color:#8f8;font-size:14px;min-height:18px}
  a{color:#5cf}
</style></head>
<body>
<img id="cam" src="/stream">
<div class="row">
  <input id="label" placeholder="라벨 (예: cat)" value="unlabeled">
  <button onclick="save()">캡처</button>
</div>
<div id="status"></div>
<div class="row">
  <button class="secondary" onclick="window.open('/capture', '_blank')">사진 미리보기</button>
  <a href="/files">저장된 사진 보기 →</a>
</div>
<script>
function save() {
  const label = document.getElementById('label').value || 'unlabeled';
  const status = document.getElementById('status');
  status.textContent = '저장 중...';
  fetch('/save?label=' + encodeURIComponent(label))
    .then(r => r.json())
    .then(d => { status.textContent = d.file + ' 저장됨 (총 ' + d.count + '장)'; })
    .catch(() => { status.textContent = '저장 실패'; });
}
</script>
</body></html>
)rawliteral";

// ---------------- handlers ----------------

static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, INDEX_HTML, strlen(INDEX_HTML));
}

static esp_err_t capture_handler(httpd_req_t *req) {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return res;
}

static esp_err_t save_handler(httpd_req_t *req) {
  String label = "unlabeled";
  char query[128];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    char val[64];
    if (httpd_query_key_value(query, "label", val, sizeof(val)) == ESP_OK) {
      label = sanitizeLabel(urlDecode(val));
    }
  }

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  int idx = countFilesWithPrefix(label) + 1;
  char path[96];
  snprintf(path, sizeof(path), "/%s_%03d.jpg", label.c_str(), idx);

  File f = LittleFS.open(path, FILE_WRITE);
  if (!f) {
    esp_camera_fb_return(fb);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  f.write(fb->buf, fb->len);
  f.close();
  esp_camera_fb_return(fb);

  char json[160];
  snprintf(json, sizeof(json), "{\"ok\":true,\"file\":\"%s\",\"count\":%d}", path + 1, idx);
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
    if (!name.startsWith("/")) {
      name = "/" + name;
    }
    if (!first) {
      json += ",";
    }
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

  httpd_resp_set_type(req, "image/jpeg");
  String cd = "attachment; filename=\"" + name + "\"";
  httpd_resp_set_hdr(req, "Content-Disposition", cd.c_str());

  static uint8_t buf[2048];
  esp_err_t res = ESP_OK;
  size_t n;
  while ((n = f.read(buf, sizeof(buf))) > 0) {
    res = httpd_resp_send_chunk(req, (const char *)buf, n);
    if (res != ESP_OK) {
      break;
    }
  }
  f.close();
  httpd_resp_send_chunk(req, NULL, 0);
  return res;
}

static esp_err_t download_all_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "application/zip");
  httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"dataset.zip\"");

  struct Entry {
    String name;
    uint32_t crc;
    uint32_t size;
    uint32_t offset;
  };
  std::vector<Entry> entries;

  static uint8_t filebuf[40960]; // max single-file size we'll bundle
  uint8_t hdr[32];
  esp_err_t res = ESP_OK;
  uint32_t offset = 0;

  File root = LittleFS.open("/");
  File file = root.openNextFile();
  while (file && res == ESP_OK) {
    String name = String(file.name());
    if (!name.startsWith("/")) {
      name = "/" + name;
    }
    name = name.substring(1);
    size_t fsize = file.size();

    if (fsize == 0 || fsize > sizeof(filebuf)) {
      file.close();
      file = root.openNextFile();
      continue; // skip anything that won't fit the buffer (shouldn't happen for our captures)
    }

    size_t n = file.read(filebuf, fsize);
    file.close();
    uint32_t crc = crc32_update(0, filebuf, n);

    memset(hdr, 0, sizeof(hdr));
    put_u32(hdr + 0, 0x04034b50);
    put_u16(hdr + 4, 20);
    put_u16(hdr + 6, 0);
    put_u16(hdr + 8, 0);
    put_u16(hdr + 10, 0);
    put_u16(hdr + 12, 0x21);
    put_u32(hdr + 14, crc);
    put_u32(hdr + 18, n);
    put_u32(hdr + 22, n);
    put_u16(hdr + 26, name.length());
    put_u16(hdr + 28, 0);

    res = httpd_resp_send_chunk(req, (const char *)hdr, 30);
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, name.c_str(), name.length());
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, (const char *)filebuf, n);
    }

    Entry e{ name, crc, (uint32_t)n, offset };
    entries.push_back(e);
    offset += 30 + name.length() + n;

    file = root.openNextFile();
  }

  if (res == ESP_OK) {
    uint32_t cd_start = offset;
    for (auto &e : entries) {
      uint8_t chdr[48];
      memset(chdr, 0, sizeof(chdr));
      put_u32(chdr + 0, 0x02014b50);
      put_u16(chdr + 4, 20);
      put_u16(chdr + 6, 20);
      put_u16(chdr + 8, 0);
      put_u16(chdr + 10, 0);
      put_u16(chdr + 12, 0);
      put_u16(chdr + 14, 0x21);
      put_u32(chdr + 16, e.crc);
      put_u32(chdr + 20, e.size);
      put_u32(chdr + 24, e.size);
      put_u16(chdr + 28, e.name.length());
      put_u16(chdr + 30, 0);
      put_u16(chdr + 32, 0);
      put_u16(chdr + 34, 0);
      put_u16(chdr + 36, 0);
      put_u32(chdr + 38, 0);
      put_u32(chdr + 42, e.offset);

      res = httpd_resp_send_chunk(req, (const char *)chdr, 46);
      if (res == ESP_OK) {
        res = httpd_resp_send_chunk(req, e.name.c_str(), e.name.length());
      }
      if (res != ESP_OK) {
        break;
      }
      offset += 46 + e.name.length();
    }

    if (res == ESP_OK) {
      uint32_t cd_size = offset - cd_start;
      uint8_t eocd[22];
      memset(eocd, 0, sizeof(eocd));
      put_u32(eocd + 0, 0x06054b50);
      put_u16(eocd + 4, 0);
      put_u16(eocd + 6, 0);
      put_u16(eocd + 8, entries.size());
      put_u16(eocd + 10, entries.size());
      put_u32(eocd + 12, cd_size);
      put_u32(eocd + 16, cd_start);
      put_u16(eocd + 20, 0);
      res = httpd_resp_send_chunk(req, (const char *)eocd, 22);
    }
  }

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
  html += "<p><a href='/'>&larr; 카메라로 돌아가기</a></p><h2>저장된 사진</h2>";
  html += "<p>" + String(LittleFS.usedBytes() / 1024) + "KB / " + String(LittleFS.totalBytes() / 1024) + "KB 사용 중</p>";
  html += "<p><a href='/download_all' style='display:inline-block;background:#2d7;color:#000;padding:8px 14px;border-radius:6px;text-decoration:none;font-weight:bold'>전체 다운로드 (ZIP)</a></p>";

  File root = LittleFS.open("/");
  File file = root.openNextFile();
  int n = 0;
  while (file) {
    String name = String(file.name());
    if (!name.startsWith("/")) {
      name = "/" + name;
    }
    name = name.substring(1);
    html += "<div class='row'><span>" + name + " (" + String(file.size() / 1024) + "KB)</span>";
    html += "<span><a href='/download?file=" + name + "'>다운로드</a>";
    html += " <a class='del' href='#' onclick=\"if(confirm('삭제할까요?')){fetch('/delete?file=" + name + "').then(()=>location.reload())}return false;\">삭제</a></span></div>";
    file = root.openNextFile();
    n++;
  }
  if (n == 0) {
    html += "<p>저장된 사진이 없습니다.</p>";
  }
  html += "</body></html>";

  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, html.c_str(), html.length());
}

static esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t *fb = NULL;
  esp_err_t res = ESP_OK;
  char part_buf[64];

  res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
  if (res != ESP_OK) {
    return res;
  }
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) {
      res = ESP_FAIL;
    } else if (fb->format != PIXFORMAT_JPEG) {
      res = ESP_FAIL;
    } else {
      size_t hlen = snprintf(part_buf, sizeof(part_buf), _STREAM_PART, fb->len);
      res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
      if (res == ESP_OK) {
        res = httpd_resp_send_chunk(req, part_buf, hlen);
      }
      if (res == ESP_OK) {
        res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
      }
    }
    if (fb) {
      esp_camera_fb_return(fb);
      fb = NULL;
    }
    if (res != ESP_OK) {
      break;
    }
    // Cap the stream to ~15fps instead of running flat-out; the camera/WiFi
    // radio otherwise stay saturated 100% of the time, which is the main
    // driver of heat during continuous streaming.
    vTaskDelay(pdMS_TO_TICKS(66));
  }
  return res;
}

static void startCameraServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.max_uri_handlers = 9;
  config.stack_size = 8192; // string-heavy handlers (files/list) need more than the 4K default

  httpd_uri_t index_uri        = { .uri = "/",             .method = HTTP_GET, .handler = index_handler,        .user_ctx = NULL };
  httpd_uri_t stream_uri       = { .uri = "/stream",       .method = HTTP_GET, .handler = stream_handler,       .user_ctx = NULL };
  httpd_uri_t capture_uri      = { .uri = "/capture",      .method = HTTP_GET, .handler = capture_handler,      .user_ctx = NULL };
  httpd_uri_t save_uri         = { .uri = "/save",         .method = HTTP_GET, .handler = save_handler,         .user_ctx = NULL };
  httpd_uri_t list_uri         = { .uri = "/list",         .method = HTTP_GET, .handler = list_handler,         .user_ctx = NULL };
  httpd_uri_t files_uri        = { .uri = "/files",        .method = HTTP_GET, .handler = files_handler,        .user_ctx = NULL };
  httpd_uri_t download_uri     = { .uri = "/download",     .method = HTTP_GET, .handler = download_handler,     .user_ctx = NULL };
  httpd_uri_t download_all_uri = { .uri = "/download_all", .method = HTTP_GET, .handler = download_all_handler, .user_ctx = NULL };
  httpd_uri_t delete_uri       = { .uri = "/delete",       .method = HTTP_GET, .handler = delete_handler,       .user_ctx = NULL };

  if (httpd_start(&stream_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &index_uri);
    httpd_register_uri_handler(stream_httpd, &stream_uri);
    httpd_register_uri_handler(stream_httpd, &capture_uri);
    httpd_register_uri_handler(stream_httpd, &save_uri);
    httpd_register_uri_handler(stream_httpd, &list_uri);
    httpd_register_uri_handler(stream_httpd, &files_uri);
    httpd_register_uri_handler(stream_httpd, &download_uri);
    httpd_register_uri_handler(stream_httpd, &download_all_uri);
    httpd_register_uri_handler(stream_httpd, &delete_uri);
  }
}

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
  }

  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn  = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  if (psramFound()) {
    // Smaller/more-compressed frames survive a weak Wi-Fi link much better than VGA.
    config.frame_size = FRAMESIZE_QVGA;
    config.jpeg_quality = 14;
    config.fb_count = 2;
    config.grab_mode = CAMERA_GRAB_LATEST;
    config.fb_location = CAMERA_FB_IN_PSRAM;
  } else {
    config.frame_size = FRAMESIZE_QVGA;
    config.jpeg_quality = 12;
    config.fb_count = 1;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    config.fb_location = CAMERA_FB_IN_DRAM;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x\n", err);
    return;
  }

  sensor_t *s = esp_camera_sensor_get();
  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);
    s->set_brightness(s, 1);
    s->set_saturation(s, -2);
  }

  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);               // AP-mode modem sleep causes drops/stutter mid-stream
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  WiFi.setTxPower(WIFI_POWER_15dBm); // boosted for range, but not flat-out max (runs hot at 19.5dBm)
  IPAddress apIP = WiFi.softAPIP();

  Serial.printf("AP started. SSID: %s\n", AP_SSID);
  Serial.print("Camera stream ready at: http://");
  Serial.println(apIP);

  startCameraServer();
}

void loop() {
  delay(10000);
}
