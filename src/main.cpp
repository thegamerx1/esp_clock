#include "secrets.h"
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <ArduinoOTA.h>
#include <WiFiClientSecure.h>
#include <Fonts/TomThumb.h>
#include <Fonts/FreeSerifBold12pt7b.h>
#include <DHT.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <AnimatedGIF.h>
#include "Arduino.h"
#include "time.h"
#include <esp_system.h>
#include <map>
#include <vector>
#include <String.h>
#include "esp_pm.h"
#include "esp_wifi.h"
#include "utils.h"
#include <mutex>
#include <queue>

#define NTP_SERVER "192.168.25.72"
#define NTP_SERVER_FALLBACK "212.230.255.2"
#define MY_TIMEZONE "CET-1CEST,M3.5.0,M10.5.0/3"
#define MAX_TASKS 6

// ---- PANEL CONFIG ----
#define PANEL_RES_X 64
#define PANEL_RES_Y 64
#define PANEL_CHAIN 3

#define PANEL_DUAL PANEL_CHAIN >= 2
#define PANEL_TRIPLE PANEL_CHAIN >= 3

#define DEFAULT_BRIGHTNESS 5
#define R1_PIN 4
#define G1_PIN 5
#define B1_PIN 6
#define R2_PIN 7
#define G2_PIN 15
#define B2_PIN 16
#define A_PIN 18
#define B_PIN 8
#define C_PIN 3
#define D_PIN 42
#define E_PIN 38
#define LAT_PIN 40
#define OE_PIN 2
#define CLK_PIN 41
#define FRAME_COUNT 32

#define DHTPIN 9
#define DHTTYPE DHT22

// #define FRAME_SIZE (PANEL_RES_X * PANEL_RES_Y)
// #define FRAME_BYTES (FRAME_SIZE * 2)
const int maxGifDuration = 30000; // ms, max GIF duration
int LOADED_ANIMATIONS = 0;
int played_gif = 0;
std::map<String, std::vector<Frame>> PANEL_FRAMES;
String currentFrame = "pharmacy";

MatrixPanel_I2S_DMA *dma_display = nullptr;
uint16_t *GIF_BUFFER;
uint16_t gif_index;
uint8_t PANEL_BRIGHTNESS;
bool POWER_MODE = true;
bool POWER_SAVING = false;
bool activate_power_save_fn = false;

AnimatedGIF gif;

// ---- CONFIG ----
const char *ssid = WIFI_SSID;
const char *password = WIFI_PASS;

IPAddress local_IP(192, 168, 25, 55);
IPAddress gateway(192, 168, 25, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(192, 168, 25, 71);
IPAddress secondaryDNS(8, 8, 4, 4);

const char *mqtt_server = MQTT_SERVER;
const int mqtt_port = 8883;
const char *mqtt_user = MQTT_USER;
const char *mqtt_pass = MQTT_PASS;

const char *mqtt_topic_brightness = "home/esp1/brightness";
const char *mqtt_topic_animation = "home/esp1/animation";
const char *mqtt_topic_power = "home/esp1/power";
const char *mqtt_topic_sleep_mode = "home/esp1/sleep_mode";
const char *mqtt_topic_animonly = "home/esp1/animonly";
const char *mqtt_topic_rgbborder = "home/esp1/rgbborder";
const char *mqtt_topic_disable_anims = "home/esp1/animdisable";
const char *mqtt_topic_calendar = "home/esp1/calendar";
const char *mqtt_topic_dht = "home/esp1/dht22";
const char *mqtt_topic_dht_2 = "home/rpi/dht22";
const char *mqtt_topic_log = "home/esp1/log";

std::map<String, uint8_t> date_colors;

bool ANIM_DISABLE = true;
bool ANIM_RGBBORDER = false;
bool ANIM_ONLY_MODE = false;
bool SLEEP_CLOCK = false;

DHT dht(DHTPIN, DHTTYPE);

SemaphoreHandle_t dht_mutex;
float dht_temperature = -99;
float dht_humidity = -99;
float dht_2_temperature = -99;
float dht_2_humidity = -99;

WiFiClientSecure espClient;
PubSubClient mqttclient(espClient);
TaskHandle_t task_handles[MAX_TASKS] = {NULL};

uint16_t myBLACK, myWHITE, myRED, myGREEN, myBLUE, myGRAY, myLightGRAY, myDarkRED, myDarkBLUE, myOrange;
const uint16_t calendar_color(int index)
{
  switch (index)
  {

  case 1:
    return myDarkBLUE;
  case 2:
    return myDarkRED;
  case 3:
    return myOrange;
  default:
    return myRED;
  }
}

void set_palette(bool night)
{
  if (night)
  {
    myBLACK = dma_display->color565(0, 0, 0);
    myWHITE = dma_display->color565(40, 40, 40);
    myGRAY = myWHITE;
    myLightGRAY = myWHITE;
    myRED = dma_display->color565(40, 5, 0);
    myGREEN = dma_display->color565(0, 40, 0);
    myBLUE = dma_display->color565(5, 40, 40);
    // Calendar colors
    myDarkRED = dma_display->color565(50, 0, 0);
    myOrange = dma_display->color565(80, 50, 10);
    myDarkBLUE = dma_display->color565(10, 15, 40);
  }
  else
  {
    myBLACK = dma_display->color565(0, 0, 0);
    myWHITE = dma_display->color565(255, 255, 255);
    myGRAY = dma_display->color565(128, 128, 128);
    myLightGRAY = dma_display->color565(50, 50, 50);
    myRED = dma_display->color565(242, 0, 0);
    myGREEN = dma_display->color565(0, 255, 0);
    myBLUE = dma_display->color565(60, 150, 255);
    // Calendar colors
    myDarkRED = dma_display->color565(181, 0, 0);
    myOrange = dma_display->color565(247, 165, 42);
    myDarkBLUE = dma_display->color565(24, 57, 154);
  }
}

std::queue<std::string> message_queue;
std::mutex queue_mutex;
bool publish_task_running = false;
bool mqtt_ready = false;
void log_task(void *pvParameters)
{
  publish_task_running = true;
  while (publish_task_running)
  {
    while (!mqtt_ready)
    {
      vTaskDelay(pdMS_TO_TICKS(500));
    }
    // Check if there are messages to publish
    std::string message;
    bool has_message = false;

    {
      std::lock_guard<std::mutex> lock(queue_mutex);
      if (!message_queue.empty())
      {
        message = message_queue.front();
        message_queue.pop();
        has_message = true;
      }
    }

    if (has_message)
    {
      if (mqttclient.connected())
      {
        mqttclient.publish(mqtt_topic_log, message.c_str());
      }
      // Small delay to prevent flooding the MQTT connection
      vTaskDelay(pdMS_TO_TICKS(100));
    }
    else
    {
      // No messages, wait a bit before checking again
      vTaskDelay(pdMS_TO_TICKS(200));
    }
  }
}

void log_boot_message(const char *tag, const char *format, ...)
{
  va_list args;
  char buffer[512];
  printf("%s: ", tag);
  va_start(args, format);
  vprintf(format, args);
  printf("\n");

  int len = snprintf(buffer, sizeof(buffer), "%s: ", tag);
  if (len > 0 && len < (int)sizeof(buffer))
  {
    vsnprintf(buffer + len, sizeof(buffer) - len, format, args);
  }
  va_end(args);

  std::lock_guard<std::mutex> lock(queue_mutex);
  if (message_queue.size() < 50)
  {
    std::string str(buffer);
    if (str.length() > 100)
      str = str.substr(0, 100);
    message_queue.push(str);
  }
}

void loadGifsFromDir(File dir)
{
  while (true)
  {
    File file = dir.openNextFile();
    if (!file)
      break;

    String path = String(file.path()); // e.g. "/cat/image1.gif"
    if (file.isDirectory())
    {
      log_boot_message("GIF", "Skipping recursion: %s", path.c_str());
      // loadGifsFromDir(file);
      continue;
    }

    // Extract category from path: /category/filename.gif
    int firstSlash = path.indexOf('/', 0); // always 0
    int secondSlash = path.indexOf('/', firstSlash + 1);
    String category;
    if (secondSlash != -1)
    {
      category = path.substring(firstSlash + 1, secondSlash);
    }
    else
    {
      log_boot_message("GIF", "Invalid file (no category): %s", path.c_str());
      continue;
    }

    size_t size = file.size();
    if (size == 0)
    {
      log_boot_message("GIF", "Skipping empty file: %s", path.c_str());
      continue;
    }

    uint8_t *buf = (uint8_t *)ps_malloc(size);
    if (!buf)
    {
      log_boot_message("GIF", "ps_malloc failed for file: %s", path.c_str());
      file.close();
      return;
    }

    file.read(buf, size);
    file.close();

    Frame frame;
    frame.data = buf;
    frame.size = size;

    PANEL_FRAMES[category].push_back(frame);
    LOADED_ANIMATIONS++;

    // log_boot_message("GIF", "Loaded: %s (%d bytes)", path.c_str(), size);
  }
}

void loadGifsByCategory()
{
  File root = LittleFS.open("/");
  if (!root || !root.isDirectory())
  {
    log_boot_message("GIF", "Failed to open root or not a directory");
    return;
  }

  while (true)
  {
    File folder = root.openNextFile();
    if (!folder)
      break;
    loadGifsFromDir(folder);
  }
}

void dht_task(void *pvParameters)
{
  vTaskDelay(pdMS_TO_TICKS(500));
  log_boot_message("DHT22", "Init DHT22");
  dht.begin();
  vTaskDelay(pdMS_TO_TICKS(500));
  while (1)
  {
    float humidity = dht.readHumidity();
    float temperature = dht.readTemperature();
    if (!isnan(humidity) && !isnan(temperature) &&
        humidity >= 0 && humidity <= 100 &&
        temperature >= -40 && temperature <= 100)
    {
      xSemaphoreTake(dht_mutex, portMAX_DELAY);
      dht_humidity = humidity;
      dht_temperature = temperature;
      xSemaphoreGive(dht_mutex);
    }
    if (POWER_SAVING)
    {
      vTaskDelay(pdMS_TO_TICKS(60000));
    }
    else
    {
      vTaskDelay(pdMS_TO_TICKS(15000));
    }
  }
}

void pause_tasks()
{
  log_boot_message("ESP", "Pausing tasks");
  for (int i = 0; i < MAX_TASKS; i++)
  {
    if (task_handles[i] != NULL)
    {
      vTaskSuspend(task_handles[i]);
    }
  }
}

void pause_tasks_and_reduce_clock()
{
  log_boot_message("ESP", "Entering power save mode");

  // pause_tasks();

  // Lower CPU frequency to 80MHz (from default 240MHz)
  dma_display->setBrightness(0);
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

  POWER_SAVING = true;
  activate_power_save_fn = true;
  esp_pm_config_esp32s3_t pm_config = {
      .max_freq_mhz = 80,
      .min_freq_mhz = 80,
      .light_sleep_enable = true};
  esp_pm_configure(&pm_config);
}

void restore_clock_and_resume_tasks()
{
  log_boot_message("ESP", "Exiting power save mode");
  // Restore CPU frequency to 240MHz
  esp_pm_config_esp32s3_t pm_config = {
      .max_freq_mhz = 240,
      .min_freq_mhz = 240,
      .light_sleep_enable = false};

  esp_pm_configure(&pm_config);

  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
  POWER_SAVING = false;
  activate_power_save_fn = true;

  for (int i = 0; i < MAX_TASKS; i++)
  {
    if (task_handles[i] != NULL)
    {
      vTaskResume(task_handles[i]);
    }
  }
  dma_display->setBrightness(PANEL_BRIGHTNESS);
}

void mqtt_callback(char *topic, byte *payload, unsigned int length)
{
  log_boot_message("MQTT", "Received topic: %s", topic);
  payload[length] = '\0';
  String val = String((char *)payload);
  if (strcmp(topic, mqtt_topic_brightness) == 0)
  {
    int brightness = val.toInt();
    if (!POWER_SAVING)
    {
      dma_display->setBrightness8(brightness);
    }
    PANEL_BRIGHTNESS = brightness;
  }
  else if (strcmp(topic, mqtt_topic_dht_2) == 0)
  {
    JsonDocument doc;
    deserializeJson(doc, val);
    xSemaphoreTake(dht_mutex, portMAX_DELAY);
    dht_2_temperature = doc["temperature"];
    dht_2_humidity = doc["humidity"];
    xSemaphoreGive(dht_mutex);
  }
  else if (strcmp(topic, mqtt_topic_power) == 0)
  {
    POWER_MODE = (val == "on");
    if (POWER_MODE && !SLEEP_CLOCK)
    {
      restore_clock_and_resume_tasks();
    }
    else
    {
      pause_tasks_and_reduce_clock();
    }
    activate_power_save_fn = true;
  }
  else if (strcmp(topic, mqtt_topic_animonly) == 0)
  {
    ANIM_ONLY_MODE = (val == "on");
  }
  else if (strcmp(topic, mqtt_topic_calendar) == 0)
  {
    log_boot_message("CAL", "Got calendar");
    log_boot_message("CAL", "%s", val.c_str());
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload, length);
    if (err)
    {
      log_boot_message("CAL", "Error calendar");
    }
    else
    {
      date_colors.clear();
      JsonObject colors = doc.as<JsonObject>();
      for (JsonPair p : colors)
      {
        String date = p.key().c_str();
        date_colors[date] = p.value().as<uint8_t>();
      }
      log_boot_message("CAL", "Updated");
    }
  }
  else if (strcmp(topic, mqtt_topic_rgbborder) == 0)
  {
    ANIM_RGBBORDER = (val == "on");
  }
  else if (strcmp(topic, mqtt_topic_disable_anims) == 0)
  {
    ANIM_DISABLE = (val == "on");
  }
  else if (strcmp(topic, mqtt_topic_sleep_mode) == 0)
  {
    SLEEP_CLOCK = (val == "on");
    set_palette(SLEEP_CLOCK);
    if (SLEEP_CLOCK)
    {
      pause_tasks_and_reduce_clock();
    }
    else if (POWER_MODE)
    {
      restore_clock_and_resume_tasks();
    }
    activate_power_save_fn = true;
  }
  else if (strcmp(topic, mqtt_topic_animation) == 0)
  {
    log_boot_message("GIF", "Setting animation category to: %s", val);
    if (!PANEL_FRAMES.count(val))
    {
      log_boot_message("GIF", "Received Invalid animation: %s", val);
    }
    currentFrame = val;
  }
}

void mqtt_task(void *pvParameters)
{
  mqttclient.setServer(mqtt_server, mqtt_port);
  mqttclient.setCallback(mqtt_callback);
  while (1)
  {
    while (!mqttclient.connected())
    {
      mqtt_ready = false;
      log_boot_message("MQTT", "Reconnecting to mqtt.");
      if (mqttclient.connect("ESP32Client", mqtt_user, mqtt_pass))
      {
        mqttclient.subscribe(mqtt_topic_dht_2);
        if (!mqttclient.subscribe(mqtt_topic_power))
        {
          mqttclient.publish(mqtt_topic_power, "on", true);
        }
        if (!mqttclient.subscribe(mqtt_topic_animonly))
        {
          mqttclient.publish(mqtt_topic_power, "off", true);
        }
        mqttclient.subscribe(mqtt_topic_calendar);
        if (!mqttclient.subscribe(mqtt_topic_rgbborder))
        {
          mqttclient.publish(mqtt_topic_rgbborder, "off", true);
        }
        if (!mqttclient.subscribe(mqtt_topic_disable_anims))
        {
          mqttclient.publish(mqtt_topic_rgbborder, "off", true);
        }
        if (!mqttclient.subscribe(mqtt_topic_sleep_mode))
        {
          mqttclient.publish(mqtt_topic_rgbborder, "off", true);
        }
        if (!mqttclient.subscribe(mqtt_topic_animation))
        {
          mqttclient.publish(mqtt_topic_rgbborder, currentFrame.c_str(), true);
        }
        if (!mqttclient.subscribe(mqtt_topic_brightness))
        {
          mqttclient.publish(mqtt_topic_brightness, String(DEFAULT_BRIGHTNESS).c_str(), true);
        }
        log_boot_message("MQTT", "Connected to mqtt.");
        break;
      };
      vTaskDelay(pdMS_TO_TICKS(1000));
    }

    mqttclient.loop();
    mqtt_ready = true;
    if (POWER_SAVING)
    {
      vTaskDelay(pdMS_TO_TICKS(400));
    }
    else
    {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
}

void mqtt_publish(void *pvParameters)
{
  vTaskDelay(pdMS_TO_TICKS(2500));
  while (1)
  {
    if (mqttclient.connected() && dht_temperature > -99 && dht_humidity > -99)
    {
      String payload;
      xSemaphoreTake(dht_mutex, portMAX_DELAY);
      payload = "{\"temperature\":" + String(dht_temperature) + ",\"humidity\":" + String(dht_humidity) + "}";
      xSemaphoreGive(dht_mutex);
      mqttclient.publish(mqtt_topic_dht, payload.c_str());
    }
    if (POWER_SAVING)
    {
      vTaskDelay(pdMS_TO_TICKS(61000));
    }
    else
    {
      vTaskDelay(pdMS_TO_TICKS(16000));
    }
  }
}

void GIFDraw(GIFDRAW *pDraw)
{
  uint8_t *s;
  uint16_t *d, *usPalette, usTemp[320];
  int x, y;

  noInterrupts();
  usPalette = pDraw->pPalette;
  y = pDraw->iY + pDraw->y; // current line

  s = pDraw->pPixels;
  if (pDraw->ucDisposalMethod == 2) // restore to background color
  {
    for (x = 0; x < pDraw->iWidth; x++)
    {
      if (s[x] == pDraw->ucTransparent)
        s[x] = pDraw->ucBackground;
    }
    pDraw->ucHasTransparency = 0;
  }
  // Apply the new pixels to the main image
  if (pDraw->ucHasTransparency) // if transparency used
  {
    uint8_t *pEnd, c, ucTransparent = pDraw->ucTransparent;
    int x, iCount;
    pEnd = s + pDraw->iWidth;
    x = 0;
    iCount = 0; // count non-transparent pixels
    while (x < pDraw->iWidth)
    {
      c = ucTransparent - 1;
      d = usTemp;
      while (c != ucTransparent && s < pEnd)
      {
        c = *s++;
        if (c == ucTransparent) // done, stop
        {
          s--; // back up to treat it like transparent
        }
        else // opaque
        {
          *d++ = usPalette[c];
          iCount++;
        }
      } // while looking for opaque pixels
      if (iCount) // any opaque pixels?
      {
        for (int xOffset = 0; xOffset < iCount; xOffset++)
        {
          // dma_display->drawPixel(x + xOffset + pDraw->iX, y, usTemp[xOffset]);
          GIF_BUFFER[y * 64 + (x + xOffset + pDraw->iX)] = usTemp[xOffset];
        }
        x += iCount;
        iCount = 0;
      }
      // no, look for a run of transparent pixels
      c = ucTransparent;
      while (c == ucTransparent && s < pEnd)
      {
        c = *s++;
        if (c == ucTransparent)
          iCount++;
        else
          s--;
      }
      if (iCount)
      {
        x += iCount; // skip these
        iCount = 0;
      }
    }
  }
  else
  {
    s = pDraw->pPixels;
    // Translate the 8-bit pixels through the RGB565 palette (already byte reversed)
    for (x = 0; x < pDraw->iWidth; x++)
    {
      // dma_display->drawPixel(x + pDraw->iX, y, usPalette[*s++]);
      GIF_BUFFER[y * 64 + (x + pDraw->iX)] = usPalette[*s++];
    }
  }
  interrupts();
} /* GIFDraw() */

void gif_task(void *pvParameters)
{
  while (!GIF_BUFFER)
  {
    vTaskDelay(pdMS_TO_TICKS(2500));
  }
  while (1)
  {
    if (POWER_SAVING)
    {
      vTaskDelay(pdMS_TO_TICKS(6000));
      continue;
    }
    if (ANIM_DISABLE)
    {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    gif_index++;
    // memset(GIF_BUFFER, 0, 64 * 64 * 2);

    auto &myframes = PANEL_FRAMES[currentFrame];
    if (myframes.empty())
    {
      vTaskDelay(pdMS_TO_TICKS(2500));
      continue;
    }
    if (played_gif >= myframes.size())
    {
      played_gif = 0;
    }
    log_boot_message("GIF", "Playing gif: %s, ID: %d", currentFrame.c_str(), played_gif);
    gif.open(myframes[played_gif].data, myframes[played_gif].size, GIFDraw);

    int frameDelay = 0;
    int then = 0;

    while (gif.playFrame(true, &frameDelay))
    {
      if (POWER_SAVING)
      {
        break;
      }
      gif_index++;
      vTaskDelay(pdMS_TO_TICKS(frameDelay));

      then += frameDelay;
      if (then > maxGifDuration)
      {
        break;
      }
    }

    gif.close();
    played_gif++;
  }
}

#define MAX_BOOT_LINES 10
void boot_message(String message)
{
  log_boot_message("ESP", "BOOT: %s", message);
  static String lines[MAX_BOOT_LINES];
  static int index = 0;
  static int count = 0;

  lines[index] = message;
  index = (index + 1) % MAX_BOOT_LINES;
  if (count < MAX_BOOT_LINES)
    count++;

  dma_display->clearScreen();
  dma_display->setCursor(0, 5);

  int start = (count == MAX_BOOT_LINES) ? index : 0;
  for (int i = 0; i < count; i++)
  {
    int lineIndex = (start + i) % MAX_BOOT_LINES;
    dma_display->println(lines[lineIndex]);
  }
  dma_display->flipDMABuffer();
}

void ota_task(void *pvParameters)
{
  ArduinoOTA.setPasswordHash(OTA_UPDATE_PASS);
  ArduinoOTA.onStart([]()
                     {
                      String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
                      log_boot_message("OTA", "Start updating %s", type);
                      pause_tasks();
                      TaskHandle_t loopHandle = xTaskGetHandle("loopTask");

                      if (loopHandle != NULL) {
                        vTaskSuspend(loopHandle);
                      }
                      dma_display->clearScreen();
                      dma_display->stopDMAoutput(); });
  ArduinoOTA.onEnd([]()
                   { log_boot_message("OTA", "End"); });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total)
                        { log_boot_message("OTA", "Progress: %u%%\r", (progress / (total / 100))); });
  ArduinoOTA.onError([](ota_error_t error)
                     { log_boot_message("OTA", "Error[%u]: ", error); esp_restart(); });
  ArduinoOTA.begin();
  while (1)
  {
    ArduinoOTA.handle();

    if (POWER_SAVING)
    {
      vTaskDelay(pdMS_TO_TICKS(5000));
    }
    else
    {
      vTaskDelay(pdMS_TO_TICKS(200));
    }
  }
}

void configure_panel(bool double_buff)
{
  HUB75_I2S_CFG::i2s_pins _pins = {G1_PIN, B1_PIN, R1_PIN, G2_PIN, B2_PIN, R2_PIN, A_PIN, B_PIN, C_PIN, D_PIN, E_PIN, LAT_PIN, OE_PIN, CLK_PIN};
  HUB75_I2S_CFG mxconfig(
      PANEL_RES_X, // module width
      PANEL_RES_Y, // module height
      PANEL_CHAIN, // Chain length
      _pins);

  mxconfig.double_buff = double_buff;
  mxconfig.clkphase = false;
  mxconfig.latch_blanking = 35;
  mxconfig.min_refresh_rate = 90;
  // mxconfig.i2sspeed = HUB75_I2S_CFG::HZ_10M;

  // Display Setup
  dma_display = new MatrixPanel_I2S_DMA(mxconfig);
  set_palette(SLEEP_CLOCK);
  dma_display->begin();
  dma_display->clearScreen();
  dma_display->setBrightness8(DEFAULT_BRIGHTNESS); // 0-255
  PANEL_BRIGHTNESS = DEFAULT_BRIGHTNESS;

  dma_display->setTextSize(1);     // size 1 == 8 pixels high
  dma_display->setTextWrap(false); // Don't wrap at end of line - will do ourselves
  dma_display->setTextColor(dma_display->color444(15, 15, 15));
}

void setup()
{
  Serial.begin(115200);
  log_boot_message("ESP", "Starting!");
  log_boot_message("ESP", "Firmware compiled on %s at %s\n", __DATE__, __TIME__);
  configure_panel(true);
  dma_display->setFont(&TomThumb);

  boot_message("Firmware:");
  boot_message(String(" ") + __DATE__);
  boot_message(String(" ") + __TIME__);

  boot_message("WIFI!");
  if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS))
  {
    log_boot_message("ESP", "STA Failed to configure");
  }
  WiFi.begin(ssid, password);
  WiFi.setAutoReconnect(true);
  boot_message("LittleFS!");
  if (!LittleFS.begin(false))
  {
    log_boot_message("ESP", "An Error has occurred while mounting SPIFFS");
  }

  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    log_boot_message("ESP", "Connecting to WIFI");
  }
  boot_message("WIFI OK!");
  // TODO: Fix the cert
  // espClient.setCACert(CA_CERT);
  espClient.setInsecure();

  boot_message("TASKS!");
  dht_mutex = xSemaphoreCreateMutex();
  task_handles[0] = NULL;

  xTaskCreate(dht_task, "dht_task", 8192, NULL, 1, &task_handles[0]);
  xTaskCreate(mqtt_task, "mqtt_task", 8192, NULL, 5, &task_handles[1]);
  xTaskCreate(mqtt_publish, "mqtt_publish", 8192, NULL, 3, &task_handles[2]);
  xTaskCreate(gif_task, "gif_task", 4096, NULL, 2, &task_handles[3]);
  xTaskCreate(ota_task, "ota_task", 8192, NULL, 5, NULL);
  xTaskCreate(log_task, "log_task", 5012, NULL, 1, &task_handles[4]);

  configTzTime(MY_TIMEZONE, NTP_SERVER, NTP_SERVER_FALLBACK);
  // boot_message("TEST SCREEN!");
  // test_screen();
  boot_message("OK!");

  boot_message("GIFS LOAD!");
  loadGifsByCategory();
  boot_message("GIFS: " + String(LOADED_ANIMATIONS));

  boot_message("BUFFER!");
  gif.begin(GIF_PALETTE_RGB565_LE);
  GIF_BUFFER = (uint16_t *)ps_malloc(64 * 64 * 2);
  if (!GIF_BUFFER)
  {
    log_boot_message("GIF", "ps_malloc failed for GIF BUFFER");
    return;
  }
  memset(GIF_BUFFER, 0, 64 * 64 * 2);
}

void draw_dht(int temp, int hum)
{
  dma_display->setTextColor(myWHITE);
  dma_display->print("T");
  dma_display->setTextColor(myRED);
  dma_display->printf("%02dC", temp);

  dma_display->setTextColor(myWHITE);
  dma_display->print("H");
  dma_display->setTextColor(myBLUE);
  dma_display->printf("%02d%%\n", hum);
}

void draw_dht_avg()
{
  if (xSemaphoreTake(dht_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
  {

    float temp_sum = dht_temperature;
    float hum_sum = dht_humidity;
    int count = 1;
    if (dht_2_temperature > -99 && dht_2_humidity > -99)
    {
      temp_sum += dht_2_temperature;
      hum_sum += dht_2_humidity;
      count = 2;
    }
    dma_display->setCursor(3, 7);
    draw_dht(round_float(temp_sum / count), round_float(hum_sum / count));
    // dma_display->setCursor(5, 12);
    // draw_dht((int)dht_2_temperature, (int)dht_2_humidity);
    xSemaphoreGive(dht_mutex);
  }
  else
  {
    log_boot_message("DHT22", "Failed to get dht22 mutex");
  }
}

void draw_ram()
{
  dma_display->setTextSize(1);
  dma_display->setTextColor(myGRAY);
  uint32_t freeHeap = ESP.getFreeHeap();
  uint32_t totalHeap = ESP.getHeapSize();
  float freePercent = 100.0 - ((freeHeap * 100.0) / totalHeap);
  freeHeap = ESP.getFreePsram();
  totalHeap = ESP.getPsramSize();
  float psfreePercent = 100.0 - ((freeHeap * 100.0) / totalHeap);
  dma_display->setTextColor(myWHITE);
  dma_display->print("R");
  dma_display->setTextColor(myGRAY);
  dma_display->printf("%2.f%%", freePercent);
  dma_display->setTextColor(myWHITE);
  dma_display->print("P");
  dma_display->setTextColor(myGRAY);
  dma_display->printf("%2.f%%\n", psfreePercent);
}

#define CALENDAR_OFFSET_X 64
#define CALENDAR_OFFSET_Y 1
#define CALENDAR_CELL_W 9
#define CALENDAR_CELL_H 8
void draw_calendar()
{
  static bool blinkState = false;
  static unsigned long lastBlink = 0;
  const unsigned long blinkInterval = 1000;

  struct tm timeinfo;
  getLocalTime(&timeinfo);

  dma_display->setCursor(CALENDAR_OFFSET_X, CALENDAR_OFFSET_Y);

  int current_year = timeinfo.tm_year + 1900;
  int current_month = timeinfo.tm_mon + 1;
  int current_day = timeinfo.tm_mday;

  // Calculate previous and next month
  int prev_month = current_month - 1;
  int prev_year = current_year;
  if (prev_month < 1)
  {
    prev_month = 12;
    prev_year = current_year - 1;
  }

  int next_month = current_month + 1;
  int next_year = current_year;
  if (next_month > 12)
  {
    next_month = 1;
    next_year = current_year + 1;
  }

  int current_days = days_in_month(current_year, current_month);
  int prev_days = days_in_month(prev_year, prev_month);
  int start_weekday = first_weekday_of_month(current_year, current_month);

  dma_display->setTextSize(1);

  // Draw weekdays at the top
  for (int i = 0; i < 7; i++)
  {
    int x = CALENDAR_OFFSET_X + i * CALENDAR_CELL_W;
    dma_display->setTextColor(myWHITE);
    dma_display->setCursor(x + 1, CALENDAR_OFFSET_Y + 6);
    char c[3] = {DAYS[i][0], DAYS[i][1], '\0'};
    dma_display->printf(c);
  }
  dma_display->drawFastHLine(CALENDAR_OFFSET_X, CALENDAR_OFFSET_Y + 6, 64 - 1, myWHITE);

  int total_cells = 42;

  for (int cell = 0; cell < total_cells; cell++)
  {
    int row = cell / 7 + 1;
    int col = cell % 7;

    int x = CALENDAR_OFFSET_X + col * CALENDAR_CELL_W;
    int y = CALENDAR_OFFSET_Y + row * CALENDAR_CELL_H;

    int display_day;
    int display_year;
    int display_month;
    bool is_current_month = false;
    bool is_prev_month = false;
    bool is_next_month = false;

    if (cell < start_weekday)
    {
      // Previous month days
      display_day = prev_days - start_weekday + cell + 1;
      display_year = prev_year;
      display_month = prev_month;
      is_prev_month = true;
    }
    else if (cell < start_weekday + current_days)
    {
      // Current month days
      display_day = cell - start_weekday + 1;
      display_year = current_year;
      display_month = current_month;
      is_current_month = true;
    }
    else
    {
      // Next month days
      display_day = cell - start_weekday - current_days + 1;
      display_year = next_year;
      display_month = next_month;
      is_next_month = true;
    }

    String date_str = String(display_year) + "-" +
                      String(display_month < 10 ? "0" : "") + String(display_month) + "-" +
                      String(display_day < 10 ? "0" : "") + String(display_day);

    dma_display->setCursor(x + 1, y + 6);

    uint16_t textColor = myWHITE;
    bool is_today = false;

    if (date_colors.find(date_str) != date_colors.end())
    {
      uint16_t bg_color = calendar_color(date_colors[date_str]);
      if (SLEEP_CLOCK)
      {
        bg_color = brightenDown(bg_color);
        dma_display->drawRect(x, y, CALENDAR_CELL_W, CALENDAR_CELL_H - 1, bg_color);
      }
      else
      {
        dma_display->fillRect(x, y, CALENDAR_CELL_W, CALENDAR_CELL_H - 1, bg_color);
        textColor = useBlackText(bg_color) ? myBLACK : myWHITE;
      }
    }

    if (is_current_month && display_day == current_day)
    {
      is_today = true;
      unsigned long now = millis();
      if (now - lastBlink >= blinkInterval)
      {
        blinkState = !blinkState;
        lastBlink = now;
      }

      uint16_t color = blinkState ? myRED : myGRAY;
      uint crosshair_y0 = y - 1;
      uint crosshair_y1 = y + CALENDAR_CELL_H - 1;
      uint crosshair_x1 = x + CALENDAR_CELL_W - 1;
      // Top
      dma_display->drawFastHLine(x, crosshair_y0, CALENDAR_CELL_W, color);
      // dma_display->drawFastHLine(crosshair_x1 - 3, crosshair_y0, 3, color);
      dma_display->drawFastVLine(x, crosshair_y0, 4, color);
      dma_display->drawFastVLine(crosshair_x1, crosshair_y0, 4, color);

      // Bottom
      dma_display->drawFastHLine(x, crosshair_y1, CALENDAR_CELL_W, color);
      // dma_display->drawFastHLine(crosshair_x1 - 3, crosshair_y1, 3, color);
      dma_display->drawFastVLine(x, crosshair_y1 - 3, 3, color);
      dma_display->drawFastVLine(crosshair_x1, crosshair_y1 - 3, 3, color);
      // dma_display->drawRoundRect(x, y, CALENDAR_CELL_W, CALENDAR_CELL_H - 1, 2, color);
    }

    if (is_prev_month || is_next_month)
    {
      if (date_colors.find(date_str) != date_colors.end())
      {
        dma_display->setTextColor(textColor);
      }
      else
      {
        dma_display->setTextColor(myGRAY);
      }
    }
    else if (col >= 5)
    {
      dma_display->setTextColor(textColor);
    }
    else
    {
      dma_display->setTextColor(textColor);
    }

    dma_display->printf("%2d", display_day);
  }
}

#define CLOCK_OFFSET_Y 30
void draw_clock(bool night)
{
  struct tm timeinfo;
  getLocalTime(&timeinfo);
  char buf[9];
  strftime(buf, sizeof(buf), "%H:%M:%S", &timeinfo);
  String time = String(buf);

  dma_display->setFont(&FreeSerifBold12pt7b);
  dma_display->setTextSize(1);
  dma_display->setCursor(4, CLOCK_OFFSET_Y);

  if (night)
  {

    dma_display->setTextColor(myLightGRAY);
    dma_display->print(time.substring(0, 5));
    // dma_display->setCursor(20, CLOCK_OFFSET_Y + 17);
    // dma_display->print(time.substring(6, 9));
  }
  else
  {
    dma_display->setTextColor(myWHITE);
    dma_display->print(time.substring(0, 5));
    dma_display->setTextColor(myGRAY);
    dma_display->setCursor(20, CLOCK_OFFSET_Y + 17);
    dma_display->print(time.substring(6, 9));
    dma_display->setTextColor(myWHITE);
  }

  dma_display->setFont(&TomThumb);
  dma_display->setCursor(3, CLOCK_OFFSET_Y - 16);
  dma_display->printf("%s, %d %s\n", DAYS[(timeinfo.tm_wday + 6) % 7], timeinfo.tm_mday, MONTHS[timeinfo.tm_mon]);
}

// ---- LOOP ----
void loop()
{
  static const uint32_t frameDelayMs = 1000 / 100;
  static uint32_t lastFrameTime = 0;
  static int someVariableHoldingFPS = 0;
  static uint16_t last_gif = 0;
  static unsigned long lastMillis = 0;
  static int frames = 0;

  if (POWER_SAVING && activate_power_save_fn)
  {
    dma_display->clearScreen();
    dma_display->setBrightness8(0);
    activate_power_save_fn = false;
  }
  if (!POWER_MODE)
  {
    delay(10000);
    return;
  }

  if (SLEEP_CLOCK)
  {
    dma_display->setBrightness8(5);
    unsigned long t_start = millis();
    dma_display->clearScreen();
    draw_clock(true);
    draw_dht_avg();
    if (PANEL_DUAL)
    {
      draw_calendar();
    }

    dma_display->flipDMABuffer();
    unsigned long t_end = millis();
    unsigned long elapsed = t_end - t_start;
    // delay(1000 - min(elapsed, 1000UL));
    delay(5000);
    return;
  }

  uint32_t now = millis();
  if (now - lastFrameTime >= frameDelayMs)
  {
    frames++;
    if (now - lastMillis >= 1000)
    {
      someVariableHoldingFPS = frames;
      frames = 0;
      lastMillis = now;
    }
    lastFrameTime = now;
  }
  // else
  // {
  //   delay(frameDelayMs - (now - lastFrameTime));
  //   return;
  // }

  uint32_t t = now / 8;

  dma_display->clearScreen();
  // dma_display->writeFillRect(0, 0, 64 * 2, 64, myBLACK);

  dma_display->setTextSize(1);
  // uint16_t rgb_color = rainbow565(t % 256);
  // int x = t % (dma_display->width() + 10);
  // int x2 = (t + 16) % (dma_display->width() + 10);

  // CENTER LINE
  // dma_display->fillRect(31, 0, 2, 64, myWHITE);

  // dma_display->drawRect(1, 1, 62, 62, rgb_color_rect);
  // dma_display->fillCircle(x - 5, 55, 5, rgb_color);
  // dma_display->fillCircle(x2 - 5, 55, 5, rgb_color);

  draw_dht_avg();

  dma_display->setCursor(31, 62);
  draw_ram();

  dma_display->setCursor(3, 62);
  dma_display->setTextColor(myWHITE);
  dma_display->print("FPS");
  dma_display->setTextColor(myGRAY);
  dma_display->printf("%d", someVariableHoldingFPS);

  draw_clock(false);

#if PANEL_DUAL
  draw_calendar();
#endif

#if PANEL_TRIPLE
  if (!ANIM_DISABLE && !SLEEP_CLOCK)
  {
    dma_display->drawRGBBitmap(64 * 2, 0, GIF_BUFFER, 64, 64);
  }
#endif

  // RGB BORDER
  if (ANIM_RGBBORDER)
  {
    uint16_t rgb_color_rect = rainbow565((t + 64) % 256);
    dma_display->drawRect(0, 0, PANEL_RES_X * PANEL_CHAIN, PANEL_RES_Y, rgb_color_rect);
  }

  dma_display->flipDMABuffer();
}