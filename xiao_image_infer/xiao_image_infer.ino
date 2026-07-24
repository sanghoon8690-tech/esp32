// XIAO ESP32S3 Sense - Edge Impulse image classification + web viewer.
//
// Serves the live camera stream and the latest classification result over
// HTTP, either as the board's own Wi-Fi AP or joined to an existing network
// (STA) - toggle with USE_STA below.
//
// ----------------------------------------------------------------------
// SETUP REQUIRED BEFORE THIS COMPILES - a trained model is not included:
//   1. Upload esp32/dataset/<label>/*.jpg to your Edge Impulse project
//      (edge-impulse-uploader, one run per label folder).
//   2. In EI Studio: Create impulse (Image processing block + a
//      classification/transfer-learning learning block) -> Generate
//      features -> Train -> check the confusion matrix.
//   3. Deployment tab -> "Arduino library" -> Build -> download the .zip.
//   4. Install it: arduino-cli lib install --zip-path <downloaded.zip>
//      (or Arduino IDE: Sketch -> Include Library -> Add .ZIP Library).
//   5. Replace PROJECT_NAME_inferencing.h below with the real header name
//      (check the .zip's src/ folder for the exact filename).
//   6. The exported library also ships examples/esp32/esp32_camera/ - if
//      fmt2rgb888/crop_and_interpolate_rgb888 below don't match that
//      example's signatures for your SDK version, copy the capture
//      function from there instead; the EI camera DSP API has shifted
//      slightly across SDK releases.
// ----------------------------------------------------------------------

#include <PROJECT_NAME_inferencing.h>   // TODO: replace with your exported library's header
#include "esp_camera.h"
#include "esp_http_server.h"
#include <WiFi.h>
#include <freertos/semphr.h>

// ---------- Wi-Fi mode ----------
// 0 = AP mode: board is its own hotspot at http://192.168.4.1 (no router
//     needed, but a PC already on another Wi-Fi network tends to roam
//     back to it - use a phone to view, see esp32/SKILL.md gotchas).
// 1 = STA mode: board joins your existing Wi-Fi; both PC and phone can
//     reach it normally through the router. Recommended if you have a
//     router available.
#define USE_STA 0

#if USE_STA
const char *STA_SSID = "your-wifi-ssid";
const char *STA_PASSWORD = "your-wifi-password";
#else
const char *AP_SSID = "XIAO-Infer";
const char *AP_PASSWORD = "12345678";
#endif

// ---------- Camera pins (XIAO ESP32S3 Sense, OV2640) ----------
#define PWDN_GPIO_NUM  -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM  10
#define SIOD_GPIO_NUM  40
#define SIOC_GPIO_NUM  39
#define Y9_GPIO_NUM    48
#define Y8_GPIO_NUM    11
#define Y7_GPIO_NUM    12
#define Y6_GPIO_NUM    14
#define Y5_GPIO_NUM    16
#define Y4_GPIO_NUM    18
#define Y3_GPIO_NUM    17
#define Y2_GPIO_NUM    15
#define VSYNC_GPIO_NUM 38
#define HREF_GPIO_NUM  47
#define PCLK_GPIO_NUM  13

// FRAMESIZE_QVGA below; keep these in sync if you change frame_size.
#define CAM_WIDTH  320
#define CAM_HEIGHT 240

static SemaphoreHandle_t camMutex;
static SemaphoreHandle_t resultMutex;
static char latestLabel[32] = "warming up";
static float latestConfidence = 0.0f;
static unsigned long latestResultAt = 0;

// ---------- Edge Impulse camera capture ----------
static uint8_t *fbRgb888 = nullptr;    // full CAM_WIDTH x CAM_HEIGHT RGB888
static uint8_t *snapshotBuf = nullptr; // resized to the model's input size

static int ei_camera_get_data(size_t offset, size_t length, float *out_ptr) {
  size_t pixel_ix = offset * 3;
  for (size_t i = 0; i < length; i++) {
    out_ptr[i] = (snapshotBuf[pixel_ix + 2] << 16) | (snapshotBuf[pixel_ix + 1] << 8) | snapshotBuf[pixel_ix];
    pixel_ix += 3;
  }
  return 0;
}

// Grab one JPEG frame, decode to RGB888, resize/crop to the model's input.
// Caller must hold camMutex.
static bool ei_camera_capture() {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) return false;

  bool converted = fmt2rgb888(fb->buf, fb->len, fb->format, fbRgb888);
  esp_camera_fb_return(fb);
  if (!converted) return false;

  ei::image::processing::crop_and_interpolate_rgb888(
      fbRgb888, CAM_WIDTH, CAM_HEIGHT,
      snapshotBuf, EI_CLASSIFIER_INPUT_WIDTH, EI_CLASSIFIER_INPUT_HEIGHT);
  return true;
}

void inferenceTask(void *arg) {
  fbRgb888 = (uint8_t *)ps_malloc(CAM_WIDTH * CAM_HEIGHT * 3);
  snapshotBuf = (uint8_t *)ps_malloc(EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT * 3);
  if (!fbRgb888 || !snapshotBuf) {
    Serial.println("ERROR: PSRAM buffer alloc failed");
    vTaskDelete(NULL);
    return;
  }

  while (true) {
    xSemaphoreTake(camMutex, portMAX_DELAY);
    bool ok = ei_camera_capture();
    xSemaphoreGive(camMutex);

    if (ok) {
      ei::signal_t signal;
      signal.total_length = EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT;
      signal.get_data = &ei_camera_get_data;

      ei_impulse_result_t result = {0};
      EI_IMPULSE_ERROR err = run_classifier(&signal, &result, false);

      if (err == EI_IMPULSE_OK) {
        size_t best = 0;
        for (size_t i = 1; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
          if (result.classification[i].value > result.classification[best].value) best = i;
        }
        xSemaphoreTake(resultMutex, portMAX_DELAY);
        strncpy(latestLabel, result.classification[best].label, sizeof(latestLabel) - 1);
        latestLabel[sizeof(latestLabel) - 1] = '\0';
        latestConfidence = result.classification[best].value;
        latestResultAt = millis();
        xSemaphoreGive(resultMutex);

        Serial.printf("predict: %s (%.2f)\n", latestLabel, latestConfidence);
      } else {
        Serial.printf("run_classifier failed (%d)\n", err);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(300));  // ~3 inferences/sec, leaves headroom for the MJPEG stream
  }
}

// ---------- HTTP server ----------
httpd_handle_t server = NULL;

static esp_err_t index_handler(httpd_req_t *req) {
  const char *html =
      "<!DOCTYPE html><html><head><meta charset='utf-8'>"
      "<title>XIAO Inference</title>"
      "<style>body{font-family:sans-serif;text-align:center;background:#111;color:#eee}"
      "img{max-width:100%;border-radius:8px}"
      "#result{font-size:1.5em;margin-top:12px}</style></head><body>"
      "<h2>XIAO ESP32S3 - Live Inference</h2>"
      "<img src='/stream'>"
      "<div id='result'>waiting for prediction...</div>"
      "<script>"
      "setInterval(()=>{fetch('/predict').then(r=>r.json()).then(d=>{"
      "document.getElementById('result').innerText = d.label + '  (' + (d.confidence*100).toFixed(1) + '%)';"
      "});}, 400);"
      "</script></body></html>";
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t predict_handler(httpd_req_t *req) {
  char label[32];
  float confidence;
  unsigned long age;

  xSemaphoreTake(resultMutex, portMAX_DELAY);
  strncpy(label, latestLabel, sizeof(label));
  confidence = latestConfidence;
  age = millis() - latestResultAt;
  xSemaphoreGive(resultMutex);

  char json[128];
  snprintf(json, sizeof(json), "{\"label\":\"%s\",\"confidence\":%.3f,\"age_ms\":%lu}", label, confidence, age);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t stream_handler(httpd_req_t *req) {
  esp_err_t res = httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=frame");
  if (res != ESP_OK) return res;

  char part_buf[64];
  while (true) {
    xSemaphoreTake(camMutex, portMAX_DELAY);
    camera_fb_t *fb = esp_camera_fb_get();
    xSemaphoreGive(camMutex);
    if (!fb) { res = ESP_FAIL; break; }

    size_t hlen = snprintf(part_buf, sizeof(part_buf),
        "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", fb->len);
    res = httpd_resp_send_chunk(req, part_buf, hlen);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, "\r\n", 2);
    esp_camera_fb_return(fb);
    if (res != ESP_OK) break;
  }
  return res;
}

void startServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.ctrl_port = 32768;

  if (httpd_start(&server, &config) == ESP_OK) {
    httpd_uri_t index_uri = {.uri = "/", .method = HTTP_GET, .handler = index_handler};
    httpd_uri_t predict_uri = {.uri = "/predict", .method = HTTP_GET, .handler = predict_handler};
    httpd_uri_t stream_uri = {.uri = "/stream", .method = HTTP_GET, .handler = stream_handler};
    httpd_register_uri_handler(server, &index_uri);
    httpd_register_uri_handler(server, &predict_uri);
    httpd_register_uri_handler(server, &stream_uri);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  camMutex = xSemaphoreCreateMutex();
  resultMutex = xSemaphoreCreateMutex();

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
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
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_QVGA;  // keep in sync with CAM_WIDTH/CAM_HEIGHT above
  config.jpeg_quality = 14;
  config.fb_count = 2;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_LATEST;

  if (esp_camera_init(&config) != ESP_OK) {
    Serial.println("ERROR: camera init failed");
    return;
  }

#if USE_STA
  WiFi.mode(WIFI_STA);
  WiFi.begin(STA_SSID, STA_PASSWORD);
  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Connected. Open: http://");
  Serial.println(WiFi.localIP());
#else
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  Serial.print("AP started. SSID: ");
  Serial.println(AP_SSID);
  Serial.print("Open: http://");
  Serial.println(WiFi.softAPIP());
#endif

  startServer();
  xTaskCreatePinnedToCore(inferenceTask, "inference", 8192, NULL, 1, NULL, 1);
}

void loop() {
  delay(1000);
}
