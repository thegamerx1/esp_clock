#include "secrets.h"
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <ArduinoOTA.h>
#include <WiFiClientSecure.h>
#include <Fonts/TomThumb.h>
#include <Fonts/FreeSerifBold12pt7b.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <AnimatedGIF.h>
#include <ESP32Ping.h>
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

#define MAX_TASKS 6

// ---- PANEL CONFIG ----
#define ENABLE_OTA 1
#define ENABLE_GIFS 1
#define ENABLE_MQTT 1
#define ENABLE_NTP 1
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
uint8_t LAST_PANEL_BRIGHTNESS;
volatile bool WIFI_CONNECTED = false;
volatile bool POWER_MODE = true;
volatile bool POWER_SAVING = false;
volatile bool OTA_UPDATING = false;
bool activate_power_save_fn = false;

AnimatedGIF gif;

const char *mqtt_topic_brightness = "home/esp1/brightness";
const char *mqtt_topic_animation = "home/esp1/animation";
const char *mqtt_topic_power = "home/esp1/power";
const char *mqtt_topic_sleep_mode = "home/esp1/sleep_mode";
const char *mqtt_topic_animonly = "home/esp1/animonly";
const char *mqtt_topic_ledmode = "home/esp1/ledmode";
const char *mqtt_topic_ledcolor = "home/esp1/ledcolor";
const char *mqtt_topic_disable_anims = "home/esp1/animdisable";
const char *mqtt_topic_calendar = "home/esp1/calendar";
const char *mqtt_topic_dht = "home/esp1/dht22";
const char *mqtt_topic_dht_2 = "home/rpi/dht22";
const char *mqtt_topic_log = "home/esp1/log";

std::map<String, uint8_t> date_colors;

volatile bool ANIM_DISABLE = false;
volatile bool ANIM_ONLY_MODE = false;
volatile bool LED_ONLY_MODE = false;
volatile bool SLEEP_CLOCK = false;
uint16_t LED_ONLY_COLOR;

SemaphoreHandle_t dht_mutex;
SemaphoreHandle_t calendar_mutex;
SemaphoreHandle_t frame_mutex;

float dht_temperature = -99;
float dht_humidity = -99;
float dht_2_temperature = -99;
float dht_2_humidity = -99;

WiFiClientSecure espClient;
PubSubClient mqttclient(espClient);
TaskHandle_t task_handles[MAX_TASKS] = {NULL};

uint16_t myBLACK, myWHITE, myRED, myGREEN, myBLUE, myGRAY, myLightGRAY, myDarkRED, myDarkBLUE, myOrange;

volatile bool mqtt_ready = false;
volatile bool mqtt_connected_state = false;

enum MqttCmdType : uint8_t
{
  MQTT_CMD_PUBLISH = 0
};

struct MqttCmd
{
  MqttCmdType type;
  char topic[128];
  char payload[512];
  bool retained;
};

QueueHandle_t mqtt_queue = nullptr;

static bool mqtt_enqueue_publish(const char *topic, const char *payload, bool retained = false, TickType_t wait_ticks = 0)
{
  if (!mqtt_queue || !topic || !payload)
  {
    return false;
  }

  MqttCmd cmd{};
  cmd.type = MQTT_CMD_PUBLISH;
  snprintf(cmd.topic, sizeof(cmd.topic), "%s", topic);
  snprintf(cmd.payload, sizeof(cmd.payload), "%s", payload);
  cmd.retained = retained;

  return xQueueSend(mqtt_queue, &cmd, wait_ticks) == pdPASS;
}

static void set_current_frame_safe(const String &frame)
{
  xSemaphoreTake(frame_mutex, portMAX_DELAY);
  currentFrame = frame;
  xSemaphoreGive(frame_mutex);
}

String get_current_frame_safe()
{
  xSemaphoreTake(frame_mutex, portMAX_DELAY);
  String frame = currentFrame;
  xSemaphoreGive(frame_mutex);
  return frame;
}

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
      if (!mqtt_enqueue_publish(mqtt_topic_log, message.c_str(), false, pdMS_TO_TICKS(20)))
      {
        vTaskDelay(pdMS_TO_TICKS(200));
      }
      else
      {
      vTaskDelay(pdMS_TO_TICKS(100));
      }
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
  char buffer[512];
  int len = snprintf(buffer, sizeof(buffer), "%s: ", tag);
  if (len < 0 || len >= (int)sizeof(buffer))
    return;

  va_list args;
  va_start(args, format);
  vsnprintf(buffer + len, sizeof(buffer) - len, format, args);
  va_end(args);

  printf("%s\n", buffer);

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

bool status_pingprimary = false;
bool status_pingsecondary = false;
bool status_dns = false;

static void wifi_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
  log_boot_message("WIFI", "Test");
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
  {
    WIFI_CONNECTED = false;
    log_boot_message("WIFI", "disconnected to wifi");
  }

  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED)
  {
    WIFI_CONNECTED = true;
    log_boot_message("WIFI", "Connected to wifi");
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

void wifi_event_init()
{
  esp_event_handler_instance_register(
      WIFI_EVENT,
      ESP_EVENT_ANY_ID,
      &wifi_event_handler,
      NULL,
      NULL);

  esp_event_handler_instance_register(
      IP_EVENT,
      IP_EVENT_STA_GOT_IP,
      &wifi_event_handler,
      NULL,
      NULL);
}

void wifi_task(void *pvParameters)
{
  vTaskDelay(pdMS_TO_TICKS(1000));
  while (1)
  {
    if (!WIFI_CONNECTED)
    {
      log_boot_message("ESP", "Reconnect wifi");
      delay(500);
      esp_wifi_connect();
    }
    else
    {
      IPAddress temp;
      status_pingprimary = Ping.ping(primaryDNS);
      status_pingsecondary = Ping.ping(secondaryDNS);
      status_dns = WiFi.hostByName("google.com", temp);
      // status_dnssecondary = WiFi.hostByName("google.com", secondaryDNS);
    }
    vTaskDelay(pdMS_TO_TICKS(5000));
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

void enter_powersave()
{
  log_boot_message("ESP", "Entering power save mode");

  PANEL_BRIGHTNESS = 0;
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

  POWER_SAVING = true;
  activate_power_save_fn = true;

  esp_pm_config_esp32s3_t pm_config = {
      .max_freq_mhz = 80,
      .min_freq_mhz = 80,
      .light_sleep_enable = true};

  esp_pm_configure(&pm_config);
}

void exit_powersave()
{
  log_boot_message("ESP", "Exiting power save mode");

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
  PANEL_BRIGHTNESS = LAST_PANEL_BRIGHTNESS;
}

void mqtt_callback(char *topic, byte *payload, unsigned int length)
{
  log_boot_message("MQTT", "Received topic: %s", topic);
  String val((const char *)payload, length);
  if (strcmp(topic, mqtt_topic_brightness) == 0)
  {
    int brightness = val.toInt();
    PANEL_BRIGHTNESS = brightness;
  }
  else if (strcmp(topic, mqtt_topic_dht_2) == 0)
  {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, val);
    if (err)
    {
      log_boot_message("DHT2", "Invalid JSON");
      return;
    }

    xSemaphoreTake(dht_mutex, portMAX_DELAY);
    dht_2_temperature = doc["temperature"] | dht_2_temperature;
    dht_2_humidity = doc["humidity"] | dht_2_humidity;
    xSemaphoreGive(dht_mutex);
  }
  else if (strcmp(topic, mqtt_topic_power) == 0)
  {
    POWER_MODE = (val == "on");
    if (POWER_MODE && !SLEEP_CLOCK)
    {
      exit_powersave();
    }
    else
    {
      enter_powersave();
    }
    activate_power_save_fn = true;
  }
  else if (strcmp(topic, mqtt_topic_sleep_mode) == 0)
  {
    SLEEP_CLOCK = (val == "on");
    set_palette(SLEEP_CLOCK);
    if (SLEEP_CLOCK)
    {
      enter_powersave();
    }
    else if (POWER_MODE)
    {
      exit_powersave();
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
    DeserializationError err = deserializeJson(doc, val);
    if (err)
    {
      log_boot_message("CAL", "Error parsing calendar JSON");
      return;
    }

    xSemaphoreTake(calendar_mutex, portMAX_DELAY);
      date_colors.clear();
      JsonObject colors = doc.as<JsonObject>();
      for (JsonPair p : colors)
      {
        String date = p.key().c_str();
        date_colors[date] = p.value().as<uint8_t>();
      }

    xSemaphoreGive(calendar_mutex);
      log_boot_message("CAL", "Updated");
  }
  else if (strcmp(topic, mqtt_topic_ledcolor) == 0)
  {

    // LED_ONLY_COLOR = dma_display->color565(r,g,b);
  }
  else if (strcmp(topic, mqtt_topic_ledmode) == 0)
  {
    LED_ONLY_MODE = (val == "on");
  }
  else if (strcmp(topic, mqtt_topic_disable_anims) == 0)
  {
    ANIM_DISABLE = (val == "on");
  }
  else if (strcmp(topic, mqtt_topic_animation) == 0)
  {
    log_boot_message("GIF", "Setting animation category to: %s", val.c_str());
    if (!PANEL_FRAMES.count(val))
    {
      log_boot_message("GIF", "Received invalid animation: %s", val.c_str());
      return;
    }
    set_current_frame_safe(val);
  }
}

void mqtt_task(void *pvParameters)
{
  mqttclient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttclient.setCallback(mqtt_callback);
  bool first_boot = true;
  MqttCmd cmd{};

  while (1)
  {
    while (!mqttclient.connected())
    {
      mqtt_ready = false;
      mqtt_connected_state = false;
      if (!WIFI_CONNECTED)
      {
        vTaskDelay(pdMS_TO_TICKS(first_boot ? 100 : 1000));
        continue;
      }
      log_boot_message("MQTT", "Reconnecting to mqtt.");
      if (mqttclient.connect("ESP32Client", MQTT_USER, MQTT_PASS))
      {
        mqttclient.subscribe(mqtt_topic_dht_2);
        if (!mqttclient.subscribe(mqtt_topic_power))
        {
          mqttclient.publish(mqtt_topic_power, "on", true);
        }
        if (!mqttclient.subscribe(mqtt_topic_animonly))
        {
          mqttclient.publish(mqtt_topic_animonly, "off", true);
        }
        mqttclient.subscribe(mqtt_topic_calendar);
        if (!mqttclient.subscribe(mqtt_topic_disable_anims))
        {
          mqttclient.publish(mqtt_topic_disable_anims, "off", true);
        }
        if (!mqttclient.subscribe(mqtt_topic_sleep_mode))
        {
          mqttclient.publish(mqtt_topic_sleep_mode, "off", true);
        }
        if (!mqttclient.subscribe(mqtt_topic_animation))
        {
          String frame = get_current_frame_safe();
          mqttclient.publish(mqtt_topic_animation, frame.c_str(), true);
        }
        if (!mqttclient.subscribe(mqtt_topic_ledmode))
        {
          mqttclient.publish(mqtt_topic_ledmode, "off", true);
        }
        if (!mqttclient.subscribe(mqtt_topic_brightness))
        {
          char brightness_buf[16];
          snprintf(brightness_buf, sizeof(brightness_buf), "%d", DEFAULT_BRIGHTNESS);
          mqttclient.publish(mqtt_topic_brightness, brightness_buf, true);
        }

        log_boot_message("MQTT", "Connected to mqtt.");
        break;
      }

      vTaskDelay(pdMS_TO_TICKS(first_boot ? 100 : 1000));
    }

    first_boot = false;

    mqttclient.loop();
    mqtt_ready = true;
    mqtt_connected_state = mqttclient.connected();

    while (mqttclient.connected() && xQueueReceive(mqtt_queue, &cmd, 0) == pdPASS)
    {
      if (cmd.type == MQTT_CMD_PUBLISH)
      {
        bool ok = mqttclient.publish(cmd.topic, cmd.payload, cmd.retained);
        if (!ok)
        {
          log_boot_message("MQTT", "Publish failed: %s", cmd.topic);
        }
      }
    }

    if (POWER_SAVING)
    {
      vTaskDelay(pdMS_TO_TICKS(200));
    }
    else
    {
      vTaskDelay(pdMS_TO_TICKS(30));
    }
  }
}

void mqtt_publish(void *pvParameters)
{
  vTaskDelay(pdMS_TO_TICKS(2500));
  while (1)
  {
    if (dht_temperature > -99 && dht_humidity > -99)
    {
      char payload[128];

      xSemaphoreTake(dht_mutex, portMAX_DELAY);
      snprintf(payload,
               sizeof(payload),
               "{\"temperature\":%.2f,\"humidity\":%.2f}",
               dht_temperature,
               dht_humidity);
      xSemaphoreGive(dht_mutex);

      if (!mqtt_enqueue_publish(mqtt_topic_dht, payload, false, pdMS_TO_TICKS(10)))
      {
        log_boot_message("MQTT", "Publish queue full for dht");
    }
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
  while (!GIF_BUFFER || !dma_display)
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
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }

    gif_index++;

    String selectedFrame = get_current_frame_safe();
    auto &myframes = PANEL_FRAMES[selectedFrame];

    if (myframes.empty())
    {
      vTaskDelay(pdMS_TO_TICKS(2500));
      continue;
    }
    if (played_gif >= myframes.size())
    {
      played_gif = 0;
    }

    log_boot_message("GIF", "Playing gif: %s, ID: %d", selectedFrame.c_str(), played_gif);
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
  log_boot_message("ESP", "BOOT: %s", message.c_str());

  static String lines[MAX_BOOT_LINES];
  static int index = 0;
  static int count = 0;

  lines[index] = message;
  index = (index + 1) % MAX_BOOT_LINES;
  if (count < MAX_BOOT_LINES)
    count++;
  int start = (count == MAX_BOOT_LINES) ? index : 0;

  if (!dma_display)
  {
    return;
  }

  dma_display->clearScreen();
  dma_display->setCursor(0, 5);

  for (int i = 0; i < count; i++)
  {
    int lineIndex = (start + i) % MAX_BOOT_LINES;
    dma_display->println(lines[lineIndex]);
  }
  dma_display->flipDMABuffer();
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
  // mxconfig.latch_blanking = 2;
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
  dma_display->setFont(&TomThumb);
}

void setup()
{
  Serial.begin(115200);
  log_boot_message("ESP", "Starting!");
  log_boot_message("ESP", "Firmware compiled on %s at %s", __DATE__, __TIME__);

  boot_message("Firmware:");
  boot_message(String(" ") + __DATE__);
  boot_message(String(" ") + __TIME__);

  boot_message("WIFI!");
  if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS))
  {
    log_boot_message("ESP", "STA Failed to configure");
  }
  WiFi.setAutoReconnect(false);
  wifi_event_init();
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  esp_wifi_connect();
  // xTaskCreate(wifi_task, "wifi_task", 4096, NULL, 2, NULL);
  boot_message("LittleFS!");
  if (!LittleFS.begin(false))
  {
    log_boot_message("ESP", "An Error has occurred while mounting SPIFFS");
  }

  while (!WIFI_CONNECTED)
  {
    esp_wifi_connect();
    delay(500);
    log_boot_message("ESP", "Connecting to WIFI");
  }
  boot_message("WIFI OK!");
  espClient.setCACert(CA_CERT);

  boot_message("PANEL!");
  configure_panel(true);
  LED_ONLY_COLOR = dma_display->color565(255, 0, 0);

#if ENABLE_GIFS
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
#endif

  boot_message("TASKS!");
  dht_mutex = xSemaphoreCreateMutex();
  calendar_mutex = xSemaphoreCreateMutex();
  frame_mutex = xSemaphoreCreateMutex();
  mqtt_queue = xQueueCreate(16, sizeof(MqttCmd));

  if (!dht_mutex || !calendar_mutex || !frame_mutex || !mqtt_queue)
  {
    log_boot_message("SYS", "Failed to create mutexes or mqtt queue");
    abort();
  }

  task_handles[0] = NULL;

#if ENABLE_MQTT
  xTaskCreate(mqtt_task, "mqtt_task", 8192, NULL, 0, &task_handles[1]);
  xTaskCreate(mqtt_publish, "mqtt_publish", 8192, NULL, 0, &task_handles[2]);
  xTaskCreate(log_task, "log_task", 5012, NULL, 0, &task_handles[4]);
#endif
#if ENABLE_GIFS
  xTaskCreate(gif_task, "gif_task", 4096, NULL, 0, &task_handles[3]);
#endif

#if ENABLE_NTP
  configTzTime(MY_TIMEZONE, NTP_SERVER, NTP_SERVER_FALLBACK);

  boot_message("WAIT CLOCK!");
  while (true)
  {

    log_boot_message("ESP", "Wait clock...");
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 100))
      break;
    delay(300);
  }
#endif

#if ENABLE_OTA
  ArduinoOTA.setPasswordHash(OTA_UPDATE_PASS);
  ArduinoOTA.onStart([]()
                     {
                       OTA_UPDATING = 1;
                       dma_display->clearScreen();
                       dma_display->stopDMAoutput();
                       String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
                       log_boot_message("OTA", "Start updating %s", type.c_str());
                       pause_tasks();
                       // TaskHandle_t loopHandle = xTaskGetHandle("loopTask");

                       // if (loopHandle != NULL) {
                       //   vTaskSuspend(loopHandle);
                       // }
                     });
  ArduinoOTA.onEnd([]()
                   { log_boot_message("OTA", "End"); });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total)
                        { log_boot_message("OTA", "Progress: %u%%", (progress / (total / 100))); });
  ArduinoOTA.onError([](ota_error_t error)
                     { log_boot_message("OTA", "Error[%u]: ", error); esp_restart(); });
  ArduinoOTA.begin();
#endif

#if ENABLE_MQTT
  boot_message("WAIT MQTT!");
  while (!mqtt_ready)
  {
    log_boot_message("ESP", "Wait mqtt...");
    delay(100);
  }
  log_boot_message("ESP", "MQTT connected.");
#endif
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

void draw_guide()
{
  // uint16_t rgb_color = rainbow565(t % 256);
  // int x = t % (dma_display->width() + 10);
  // int x2 = (t + 16) % (dma_display->width() + 10);

  // CENTER LINE
  // dma_display->fillRect(31, 0, 2, 64, myWHITE);

  // dma_display->drawRect(1, 1, 62, 62, rgb_color_rect);
  // dma_display->fillCircle(x - 5, 55, 5, rgb_color);
  // dma_display->fillCircle(x2 - 5, 55, 5, rgb_color);
}

void draw_dht_avg()
{
  if (xSemaphoreTake(dht_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
  {
    float temp_sum = 0;
    float hum_sum = 0;
    int count = 0;
    struct SensorVals
    {
      float t;
      float h;
    };
    SensorVals sensors[] = {
        {dht_2_temperature, dht_2_humidity},
        {dht_temperature, dht_humidity}};

    for (int i = 0; i < 2; i++)
    {
      if (sensor_valid(sensors[i].t, sensors[i].h))
      {
        temp_sum += sensors[i].t;
        hum_sum += sensors[i].h;
        count++;
      }
    }

    if (count > 0)
    {
      dma_display->setCursor(1, 6);
      draw_dht(round_float(temp_sum / count), round_float(hum_sum / count));
    }
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

#define YEAR_PROGRESS_OFFSET_X 65
#define YEAR_PROGRESS_OFFSET_Y 56
#define YEAR_PROGRESS_L2_OFFSET 32
void draw_year_progress()
{
  struct tm timeinfo;
  getLocalTime(&timeinfo, 100);
  int year = timeinfo.tm_year + 1900;
  int month = timeinfo.tm_mon + 1;
  int day = timeinfo.tm_mday;

  // days per month
  int mdays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

  // leap year fix
  int leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
  if (leap)
    mdays[1] = 29;

  // day-of-year (1 Jan = 0%)
  int doy = 0;
  for (int i = 0; i < month - 1; i++)
    doy += mdays[i];
  doy += day - 1; // make Jan 1 = 0

  int total_days = leap ? 366 : 365;

  double percent = (double)doy / (double)(total_days - 1) * 100.0;

  dma_display->setTextColor(myWHITE);
  dma_display->setCursor(YEAR_PROGRESS_OFFSET_X, YEAR_PROGRESS_OFFSET_Y + 5);
  dma_display->printf("%d%% %d", (int)percent, year);

  // dma_display->drawRect(YEAR_PROGRESS_OFFSET_X, YEAR_PROGRESS_OFFSET_Y, 62, 9, myWHITE);
  dma_display->fillRect(YEAR_PROGRESS_OFFSET_X, YEAR_PROGRESS_OFFSET_Y + 6, (int)((percent / 100.0) * 61), 2, myWHITE);
  dma_display->drawLine(YEAR_PROGRESS_OFFSET_X - 1, YEAR_PROGRESS_OFFSET_Y + 6, YEAR_PROGRESS_OFFSET_X - 1, YEAR_PROGRESS_OFFSET_Y + 7, myWHITE);
  dma_display->drawLine(YEAR_PROGRESS_OFFSET_X + 62, YEAR_PROGRESS_OFFSET_Y, YEAR_PROGRESS_OFFSET_X + 62, YEAR_PROGRESS_OFFSET_Y + 8, myWHITE);
  dma_display->drawLine(YEAR_PROGRESS_OFFSET_X + YEAR_PROGRESS_L2_OFFSET, YEAR_PROGRESS_OFFSET_Y, YEAR_PROGRESS_OFFSET_X + 61, YEAR_PROGRESS_OFFSET_Y, myWHITE);
  if (percent > YEAR_PROGRESS_L2_OFFSET)
  {
    dma_display->fillRect(YEAR_PROGRESS_OFFSET_X + YEAR_PROGRESS_L2_OFFSET, YEAR_PROGRESS_OFFSET_Y + 1, (int)((percent / 100.0) * 61) - YEAR_PROGRESS_L2_OFFSET, 7, myWHITE);
  }
}

#define CALENDAR_OFFSET_X 64
#define CALENDAR_OFFSET_Y 0
#define CALENDAR_CELL_W 9
#define CALENDAR_CELL_H 8
void draw_calendar(bool night)
{
  static bool blinkState = false;
  static unsigned long lastBlink = 0;
  const unsigned long blinkInterval = 1000;

  struct tm timeinfo;
  getLocalTime(&timeinfo, 100);

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

  if (!night)
  {
    // Draw weekdays at the top
    for (int i = 0; i < 7; i++)
    {
      int x = CALENDAR_OFFSET_X + i * CALENDAR_CELL_W;
      dma_display->setTextColor(myWHITE);
      dma_display->setCursor(x + 1, CALENDAR_OFFSET_Y + 6);
      char c[3] = {DAYS[i][0], DAYS[i][1], '\0'};
      dma_display->print(c);
    }
    dma_display->drawFastHLine(CALENDAR_OFFSET_X, CALENDAR_OFFSET_Y + 6, 64 - 1, myWHITE);
  }
  int total_cells = 42;

  xSemaphoreTake(calendar_mutex, portMAX_DELAY);

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
    bool is_passed = false;
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
      is_passed = current_day > display_day;
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
    auto it = date_colors.find(date_str);
    bool hasCalendarColor = (it != date_colors.end());

    if (hasCalendarColor)
    {
      uint16_t bg_color = calendar_color(it->second);
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

    bool is_today = false;

    uint crosshair_y0 = y - 1;
    uint crosshair_y1 = y + CALENDAR_CELL_H - 1;
    uint crosshair_x1 = x + CALENDAR_CELL_W - 1;
    if (is_current_month && display_day == current_day)
    {
      is_today = true;
      unsigned long now = millis();
      if (now - lastBlink >= blinkInterval)
      {
        blinkState = !blinkState;
        lastBlink = now;
      }

      uint16_t color = blinkState ? myLightGRAY : myGRAY;

      dma_display->drawRoundRect(x, crosshair_y0, CALENDAR_CELL_W, CALENDAR_CELL_H + 1, 0, myWHITE);
      // Top
      dma_display->drawFastHLine(x + 1, crosshair_y0, 3, color);
      dma_display->drawFastHLine(crosshair_x1 - 3, crosshair_y0, 3, color);
      dma_display->drawFastVLine(x, crosshair_y0 + 1, 3, color);
      dma_display->drawFastVLine(crosshair_x1, crosshair_y0 + 1, 3, color);

      // Bottom
      dma_display->drawFastHLine(x + 1, crosshair_y1, 3, color);
      dma_display->drawFastHLine(crosshair_x1 - 3, crosshair_y1, 3, color);
      dma_display->drawFastVLine(x, crosshair_y1 - 3, 3, color);
      dma_display->drawFastVLine(crosshair_x1, crosshair_y1 - 3, 3, color);
      // dma_display->drawRoundRect(x, y, CALENDAR_CELL_W, CALENDAR_CELL_H - 1, 2, color);
    }

    if (is_prev_month || is_next_month)
    {
      if (hasCalendarColor)
      {
        dma_display->setTextColor(textColor);
      }
      else
      {
        dma_display->setTextColor(myGRAY);
      }
    }
    else
    {
      dma_display->setTextColor(textColor);
    }

    if (is_prev_month || is_passed)
    {
      dma_display->drawLine(x, crosshair_y0, crosshair_x1, crosshair_y1 - 1, myRED);
    }
    dma_display->printf("%2d", display_day);
  }

  xSemaphoreGive(calendar_mutex);
}

#define STATUS_OFFSET_X 58
#define STATUS_OFFSET_Y 1
void draw_status()
{
  uint16_t wifistatus = mqtt_connected_state ? myGREEN : (WIFI_CONNECTED ? myOrange : myRED);
  dma_display->fillRect(STATUS_OFFSET_X, STATUS_OFFSET_Y, 5, 5, wifistatus);

  // DNS
  uint16_t dnsstatus = myRED;
  if (status_pingprimary || status_pingsecondary)
    dnsstatus = myOrange;
  if (status_pingprimary && status_pingsecondary)
    dnsstatus = myGREEN;

  dma_display->fillTriangle(
      STATUS_OFFSET_X - 6, STATUS_OFFSET_Y + 4,
      STATUS_OFFSET_X - 6 + 4, STATUS_OFFSET_Y + 4,
      STATUS_OFFSET_X - 6 + 2, STATUS_OFFSET_Y,
      dnsstatus);

  uint16_t dnsstatus2 = status_dns ? myGREEN : myRED;
  dma_display->fillCircle(STATUS_OFFSET_X - 6 - 4, STATUS_OFFSET_Y + 2, 2, dnsstatus2);
}

#define CLOCK_OFFSET_Y 29
void draw_clock(bool night)
{
  struct tm timeinfo;
  getLocalTime(&timeinfo, 100);
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
  dma_display->setCursor(2, CLOCK_OFFSET_Y - 16);
  dma_display->printf("%s, %d %s\n", DAYS[(timeinfo.tm_wday + 6) % 7], timeinfo.tm_mday, MONTHS[timeinfo.tm_mon]);
}

// ---- LOOP ----
void loop()
{
  static const uint32_t frameDelayMs = 1000 / 100;
  static uint32_t lastFrameTime = 0;
  static int someVariableHoldingFPS = 0;
  static unsigned long lastMillis = 0;
  static int frames = 0;

#if ENABLE_OTA
  ArduinoOTA.handle();
  if (OTA_UPDATING)
  {
    return;
  }
#endif

  if (!WIFI_CONNECTED)
  {
    dma_display->setBrightness8(PANEL_BRIGHTNESS);
    LAST_PANEL_BRIGHTNESS = PANEL_BRIGHTNESS;
  }
  if (POWER_SAVING && activate_power_save_fn)
  {
    dma_display->clearScreen();
    dma_display->setBrightness8(0);
    activate_power_save_fn = false;
  }
  else if (PANEL_BRIGHTNESS != LAST_PANEL_BRIGHTNESS && !POWER_SAVING)
  {
    dma_display->setBrightness8(PANEL_BRIGHTNESS);
    LAST_PANEL_BRIGHTNESS = PANEL_BRIGHTNESS;
  }
  if (!POWER_MODE)
  {
    delay(10000);
    return;
  }

  if (SLEEP_CLOCK)
  {
    dma_display->setBrightness8(5);
    dma_display->clearScreen();
    draw_clock(true);
    draw_dht_avg();
#if PANEL_DUAL
    draw_calendar(true);
#endif
    dma_display->flipDMABuffer();
    delay(5000);
    return;
  }

  if (LED_ONLY_MODE)
  {
    dma_display->fillRect(0, 0, PANEL_RES_X * PANEL_CHAIN, PANEL_RES_Y * PANEL_CHAIN, LED_ONLY_COLOR);
    dma_display->flipDMABuffer();
    delay(500);
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

  uint32_t t = now / 8;

  dma_display->clearScreen();

  dma_display->setTextSize(1);

  // draw_guide();

  draw_dht_avg();

  dma_display->setCursor(33, 63);
  draw_ram();

  dma_display->setCursor(1, 63);
  dma_display->setTextColor(myWHITE);
  dma_display->print("FPS");
  dma_display->setTextColor(myGRAY);
  dma_display->printf("%d", someVariableHoldingFPS);

  draw_clock(false);
  draw_status();

#if PANEL_DUAL
  draw_calendar(false);
  draw_year_progress();
#endif

#if PANEL_TRIPLE && ENABLE_GIFS
  if (!ANIM_DISABLE)
  {
    dma_display->drawRGBBitmap(64 * 2, 0, GIF_BUFFER, 64, 64);
  }
#endif

  dma_display->flipDMABuffer();
}