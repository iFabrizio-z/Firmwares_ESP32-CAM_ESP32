
#include "esp_camera.h"
#include <WiFi.h>
#include "esp_http_server.h"
#include <stdio.h>
#include <string.h>

// =====================================================
// AgroSmart - ESP32-CAM AI-Thinker + OV2640
// Pagina web:       http://IP/
// Fotografia:       http://IP/capture
// Video MJPEG:      http://IP:81/stream
// =====================================================

// CAMBIA ESTOS DATOS POR LOS DE TU RED WI-FI
const char* WIFI_SSID = "NAME_WIFI";
const char* WIFI_PASSWORD = "PASSWORD-WIFI";

// Pines ESP32-CAM AI-Thinker
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27

#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5

#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

static httpd_handle_t camera_httpd = NULL;
static httpd_handle_t stream_httpd = NULL;

// -----------------------------------------------------
// Pagina web
// -----------------------------------------------------
static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>AgroSmart - Camara</title>
<style>
body {
  font-family: Arial, sans-serif;
  background: #eef4ef;
  color: #20352b;
  text-align: center;
  margin: 0;
  padding: 16px;
}
main {
  max-width: 760px;
  margin: auto;
  background: white;
  padding: 18px;
  border-radius: 14px;
}
img {
  display: block;
  width: 100%;
  height: auto;
  background: #222;
  border-radius: 10px;
}
a {
  display: inline-block;
  margin: 12px;
  padding: 12px 16px;
  background: #23784b;
  color: white;
  text-decoration: none;
  border-radius: 8px;
}
</style>
</head>
<body>
<main>
<h1>AgroSmart</h1>
<h2>Camara en vivo</h2>
<p>Transmision desde la ESP32-CAM</p>
<img id="cam" alt="Video de la camara">
<p><a href="/capture" target="_blank">Tomar fotografia</a></p>
</main>
<script>
document.getElementById('cam').src =
  'http://' + location.hostname + ':81/stream';
</script>
</body>
</html>
)rawliteral";

// -----------------------------------------------------
// Ruta principal
// -----------------------------------------------------
static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  return httpd_resp_send(
    req, INDEX_HTML, HTTPD_RESP_USE_STRLEN
  );
}

// -----------------------------------------------------
// Captura JPEG: /capture
// -----------------------------------------------------
static esp_err_t capture_handler(httpd_req_t *req) {
  camera_fb_t *fb = esp_camera_fb_get();

  if (fb == NULL) {
    Serial.println("ERROR: no se pudo capturar la imagen.");
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  if (fb->format != PIXFORMAT_JPEG) {
    esp_camera_fb_return(fb);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(
    req, "Content-Disposition",
    "inline; filename=agrosmart.jpg"
  );

  esp_err_t result = httpd_resp_send(
    req, (const char *)fb->buf, fb->len
  );

  esp_camera_fb_return(fb);
  return result;
}

// -----------------------------------------------------
// Video MJPEG: /stream en el puerto 81
// -----------------------------------------------------
static const char *STREAM_CONTENT_TYPE =
  "multipart/x-mixed-replace;boundary=agrosmartframe";

static const char *STREAM_BOUNDARY =
  "\r\n--agrosmartframe\r\n";

static const char *STREAM_PART =
  "Content-Type: image/jpeg\r\n"
  "Content-Length: %u\r\n\r\n";

static esp_err_t stream_handler(httpd_req_t *req) {
  esp_err_t res = httpd_resp_set_type(
    req, STREAM_CONTENT_TYPE
  );

  if (res != ESP_OK) {
    return res;
  }

  httpd_resp_set_hdr(
    req, "Access-Control-Allow-Origin", "*"
  );

  Serial.println("Cliente conectado al video.");

  while (true) {
    camera_fb_t *fb = esp_camera_fb_get();

    if (fb == NULL) {
      Serial.println("ERROR: fallo al obtener fotograma.");
      res = ESP_FAIL;
      break;
    }

    if (fb->format != PIXFORMAT_JPEG) {
      esp_camera_fb_return(fb);
      res = ESP_FAIL;
      break;
    }

    char part_buf[64];

    res = httpd_resp_send_chunk(
      req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY)
    );

    if (res == ESP_OK) {
      size_t hlen = snprintf(
        part_buf,
        sizeof(part_buf),
        STREAM_PART,
        (unsigned int)fb->len
      );

      res = httpd_resp_send_chunk(
        req, part_buf, hlen
      );
    }

    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(
        req, (const char *)fb->buf, fb->len
      );
    }

    esp_camera_fb_return(fb);

    if (res != ESP_OK) {
      // Normal cuando el cliente cierra el video.
      break;
    }

    delay(30);
  }

  Serial.println("Cliente de video desconectado.");
  return res;
}

// -----------------------------------------------------
// Iniciar servidores HTTP
// -----------------------------------------------------
static bool startCameraServer() {
  // Servidor web en puerto 80
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.ctrl_port = 32768;

  httpd_uri_t index_uri = {};
  index_uri.uri = "/";
  index_uri.method = HTTP_GET;
  index_uri.handler = index_handler;

  httpd_uri_t capture_uri = {};
  capture_uri.uri = "/capture";
  capture_uri.method = HTTP_GET;
  capture_uri.handler = capture_handler;

  if (httpd_start(&camera_httpd, &config) != ESP_OK) {
    Serial.println("ERROR: no inicio el servidor del puerto 80.");
    camera_httpd = NULL;
    return false;
  }

  if (httpd_register_uri_handler(
        camera_httpd, &index_uri) != ESP_OK ||
      httpd_register_uri_handler(
        camera_httpd, &capture_uri) != ESP_OK) {
    Serial.println("ERROR: no se pudieron registrar las rutas.");
    httpd_stop(camera_httpd);
    camera_httpd = NULL;
    return false;
  }

  // Servidor de video en puerto 81
  httpd_config_t stream_config = HTTPD_DEFAULT_CONFIG();
  stream_config.server_port = 81;
  stream_config.ctrl_port = 32769;

  httpd_uri_t stream_uri = {};
  stream_uri.uri = "/stream";
  stream_uri.method = HTTP_GET;
  stream_uri.handler = stream_handler;

  if (httpd_start(&stream_httpd, &stream_config) != ESP_OK) {
    Serial.println("ERROR: no inicio el servidor del puerto 81.");
    httpd_stop(camera_httpd);
    camera_httpd = NULL;
    stream_httpd = NULL;
    return false;
  }

  if (httpd_register_uri_handler(
        stream_httpd, &stream_uri) != ESP_OK) {
    Serial.println("ERROR: no se registro /stream.");
    httpd_stop(stream_httpd);
    httpd_stop(camera_httpd);
    stream_httpd = NULL;
    camera_httpd = NULL;
    return false;
  }

  Serial.println("Servidores web y video iniciados.");
  return true;
}

// -----------------------------------------------------
// Configuracion inicial
// -----------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(false);
  delay(1000);

  Serial.println();
  Serial.println("==============================");
  Serial.println(" AgroSmart ESP32-CAM");
  Serial.println("==============================");

  // Configuracion de la camara
  camera_config_t config = {};

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
  config.frame_size = FRAMESIZE_VGA;
  config.jpeg_quality = 12;
  config.fb_count = 1;

  if (psramFound()) {
    Serial.println("PSRAM detectada.");
    config.frame_size = FRAMESIZE_VGA;
    config.jpeg_quality = 10;
    config.fb_count = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_LATEST;
  } else {
    Serial.println("AVISO: no se detecto PSRAM.");
    config.frame_size = FRAMESIZE_QVGA;
    config.jpeg_quality = 14;
    config.fb_count = 1;
    config.fb_location = CAMERA_FB_IN_DRAM;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  }

  esp_err_t err = esp_camera_init(&config);

  if (err != ESP_OK) {
    Serial.printf(
      "ERROR: camara no inicializada. Codigo: 0x%x\n",
      err
    );
    Serial.println(
      "Comprueba el modelo, los pines y la alimentacion."
    );
    return;
  }

  Serial.println("Camara inicializada correctamente.");

  // Conexion Wi-Fi
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Conectando al Wi-Fi");

  unsigned long inicio = millis();

  while (WiFi.status() != WL_CONNECTED &&
         millis() - inicio < 30000) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("ERROR: no se pudo conectar al Wi-Fi.");
    Serial.println(
      "Comprueba el nombre y la clave de la red."
    );
    return;
  }

  Serial.println("Wi-Fi conectado.");
  Serial.print("IP de la camara: ");
  Serial.println(WiFi.localIP());

  if (!startCameraServer()) {
    Serial.println("ERROR: los servidores no iniciaron.");
    return;
  }

  Serial.println();
  Serial.println("AgroSmart listo.");
  Serial.print("Pagina web: http://");
  Serial.println(WiFi.localIP());

  Serial.print("Fotografia: http://");
  Serial.print(WiFi.localIP());
  Serial.println("/capture");

  Serial.print("Video MJPEG: http://");
  Serial.print(WiFi.localIP());
  Serial.println(":81/stream");
}

// -----------------------------------------------------
// Bucle principal
// -----------------------------------------------------
void loop() {
  delay(1000);
}
