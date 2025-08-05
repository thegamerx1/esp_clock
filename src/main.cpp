#include "secrets.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <WiFiUdp.h>
#include <LittleFS.h>
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

#define PANEL_RES_X 64
#define PANEL_RES_Y 64
#define PANEL_CHAIN 1
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
struct Frame
{
  uint8_t *data = nullptr;
  size_t size = 0;
};

std::map<String, std::vector<Frame>> PANEL_FRAMES;
String currentFrame = "pharmacy";

MatrixPanel_I2S_DMA *dma_display = nullptr;
uint16_t myBLACK, myWHITE, myRED, myGREEN, myBLUE, myGRAY, myLightGRAY;
uint16_t *GIF_BUFFER;
uint8_t PANEL_BRIGHTNESS;
bool POWER_SAVING = false;

AnimatedGIF gif;
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
      Serial.println("Skipping recursion: " + path);
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
      Serial.println(String("Invalid file (no category): ") + path);
      continue;
    }

    size_t size = file.size();
    if (size == 0)
    {
      Serial.println(String("Skipping empty file: ") + path);
      continue;
    }

    uint8_t *buf = (uint8_t *)ps_malloc(size);
    if (!buf)
    {
      Serial.println("ps_malloc failed");
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

    Serial.println(String("Loaded: ") + path + " (" + size + " bytes)");
  }
}

void loadGifsByCategory()
{
  File root = LittleFS.open("/");
  if (!root || !root.isDirectory())
  {
    Serial.println("Failed to open root or not a directory");
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

const char *mqtt_brightness_topic = "home/esp1/brightness";
const char *mqtt_animation_topic = "home/esp1/animation";
const char *mqtt_power_topic = "home/esp1/power";
const char *mqtt_show_clock_on_sleep_topic = "home/esp1/show_clock_on_sleep";
const char *mqtt_animonly_topic = "home/esp1/animonly";
const char *mqtt_disable_anims_topic = "home/esp1/animdisable";
const char *mqtt_dht_topic = "home/esp1/dht22";
const char *mqtt_dht_2_topic = "home/rpi/dht22";

#define DHTPIN 39
#define DHTTYPE DHT22
bool ANIM_DISABLE = false;
bool ANIM_ONLY_MODE = false;
bool SHOW_CLOCK_ON_SLEEP = false;

DHT dht(DHTPIN, DHTTYPE);

SemaphoreHandle_t dht_mutex;
float dht_temperature = 0;
float dht_humidity = 0;
float dht_2_temperature = 0;
float dht_2_humidity = 0;

void dht_task(void *pvParameters)
{
  vTaskDelay(pdMS_TO_TICKS(500));
  Serial.println("Init DHT22");
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
      vTaskDelay(pdMS_TO_TICKS(10000));
    }
    else
    {
      vTaskDelay(pdMS_TO_TICKS(4000));
    }
  }
}

WiFiClientSecure espClient;
WiFiUDP espUdpClient;

#define NTP_SERVER "212.230.255.2"

// NTPClient timeClient(espUdpClient, NTP_SERVER, 0, 240000);
// void ntp_task(void *pvParameters)
// {
//   timeClient.begin();
//   vTaskDelay(pdMS_TO_TICKS(100));
//   while (1)
//   {
//     timeClient.update();
//     vTaskDelay(pdMS_TO_TICKS(5000));
//   }
// }

PubSubClient mqttclient(espClient);

#define MAX_TASKS 6

TaskHandle_t task_handles[MAX_TASKS] = {NULL};

void pause_tasks_and_reduce_clock()
{
  for (int i = 0; i < MAX_TASKS; i++)
  {
    if (task_handles[i] != NULL)
    {
      vTaskSuspend(task_handles[i]);
    }
  }

  // Lower CPU frequency to 80MHz (from default 240MHz)
  esp_pm_config_esp32s3_t pm_config = {
      .max_freq_mhz = 80,
      .min_freq_mhz = 80,
      .light_sleep_enable = true // enable light sleep
  };
  esp_pm_configure(&pm_config);

  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
  POWER_SAVING = true;
  dma_display->setBrightness(0);
}

void restore_clock_and_resume_tasks()
{
  // Restore CPU frequency to 240MHz
  esp_pm_config_esp32s3_t pm_config = {
      .max_freq_mhz = 240,
      .min_freq_mhz = 240,
      .light_sleep_enable = false};

  esp_pm_configure(&pm_config);

  esp_wifi_set_ps(WIFI_PS_NONE);
  POWER_SAVING = false;

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
  Serial.println("topic: " + String(topic));
  payload[length] = '\0'; // null-terminate
  String val = String((char *)payload);
  if (strcmp(topic, mqtt_brightness_topic) == 0)
  {
    int brightness = val.toInt();
    if (!POWER_SAVING)
    {
      dma_display->setBrightness8(brightness);
    }
    PANEL_BRIGHTNESS = brightness;
  }
  else if (strcmp(topic, mqtt_dht_2_topic) == 0)
  {
    // Serial.println("Updated 2: " + String((char *)payload));
    JsonDocument doc;
    deserializeJson(doc, val);
    xSemaphoreTake(dht_mutex, portMAX_DELAY);
    dht_2_temperature = doc["temperature"];
    dht_2_humidity = doc["humidity"];
    xSemaphoreGive(dht_mutex);
  }
  else if (strcmp(topic, mqtt_power_topic) == 0)
  {
    if (val == "off")
    {
      Serial.println("Entering power save mode");
      pause_tasks_and_reduce_clock();
    }
    else
    {
      Serial.println("Exiting power save mode");
      restore_clock_and_resume_tasks();
    }
  }
  else if (strcmp(topic, mqtt_animonly_topic) == 0)
  {
    ANIM_ONLY_MODE = (val == "on");
  }
  else if (strcmp(topic, mqtt_disable_anims_topic) == 0)
  {
    ANIM_DISABLE = (val == "on");
  }
  else if (strcmp(topic, mqtt_show_clock_on_sleep_topic) == 0)
  {
    SHOW_CLOCK_ON_SLEEP = (val == "on");
  }
  else if (strcmp(topic, mqtt_animation_topic) == 0)
  {
    Serial.println("Received animation: " + val);
    if (!PANEL_FRAMES.count(val))
    {
      Serial.println("Received Invalid animation: " + val);
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
      Serial.println("Reconnecting to mqtt.");
      if (mqttclient.connect("ESP32Client", mqtt_user, mqtt_pass))
      {
        assert(mqttclient.subscribe("home/esp1/#"));
        assert(mqttclient.subscribe(mqtt_dht_2_topic));
        if (!mqttclient.subscribe(mqtt_brightness_topic))
        {
          mqttclient.publish(mqtt_brightness_topic, String(DEFAULT_BRIGHTNESS).c_str(), true);
          assert(mqttclient.subscribe(mqtt_brightness_topic));
        }
        Serial.println("Connected to MQTT.");
        break;
      };
      vTaskDelay(pdMS_TO_TICKS(1000));
    }

    mqttclient.loop();
    if (POWER_SAVING)
    {
      vTaskDelay(pdMS_TO_TICKS(500));
    }
    else
    {
      vTaskDelay(pdMS_TO_TICKS(20));
    }
  }
}

void mqtt_publish(void *pvParameters)
{
  vTaskDelay(pdMS_TO_TICKS(2500));
  while (1)
  {
    if (mqttclient.connected())
    {
      String payload;
      xSemaphoreTake(dht_mutex, portMAX_DELAY);
      payload = "{\"temperature\":" + String(dht_temperature) + ",\"humidity\":" + String(dht_humidity) + "}";
      xSemaphoreGive(dht_mutex);
      mqttclient.publish(mqtt_dht_topic, payload.c_str());
    }
    if (POWER_SAVING)
    {
      vTaskDelay(pdMS_TO_TICKS(15000));
    }
    else
    {
      vTaskDelay(pdMS_TO_TICKS(5000));
    }
  }
}

#define MAX_LINES 7

void boot_message(String message)
{
  Serial.println("BOOT: " + message);
  static String lines[MAX_LINES];
  static int index = 0;
  static int count = 0;

  lines[index] = message;          // Add new message at current index
  index = (index + 1) % MAX_LINES; // Move index, wrap around
  if (count < MAX_LINES)
    count++; // Keep track of how many lines stored

  dma_display->clearScreen();
  dma_display->setCursor(0, 0);

  int start = (count == MAX_LINES) ? index : 0; // Start printing from oldest line
  for (int i = 0; i < count; i++)
  {
    int lineIndex = (start + i) % MAX_LINES;
    dma_display->println(lines[lineIndex]);
  }
  dma_display->flipDMABuffer();
}

uint16_t
rainbow565(uint8_t pos)
{
  uint8_t r, g, b;
  if (pos < 85)
  {
    r = 255 - pos * 3;
    g = pos * 3;
    b = 0;
  }
  else if (pos < 170)
  {
    pos -= 85;
    r = 0;
    g = 255 - pos * 3;
    b = pos * 3;
  }
  else
  {
    pos -= 170;
    r = pos * 3;
    g = 0;
    b = 255 - pos * 3;
  }
  return dma_display->color565(r, g, b); // <-- return RGB565 color
}

void test_screen()
{
  // fix the screen with green
  dma_display->clearScreen();
  dma_display->fillRect(0, 0, dma_display->width(), dma_display->height(), dma_display->color444(0, 15, 0));
  // dma_display->flipDMABuffer();
  delay(250);

  // draw a box in yellow
  dma_display->clearScreen();
  dma_display->drawRect(0, 0, dma_display->width(), dma_display->height(), dma_display->color444(15, 15, 0));
  // dma_display->flipDMABuffer();
  delay(250);

  // draw an 'X' in red
  dma_display->drawLine(0, 0, dma_display->width() - 1, dma_display->height() - 1, dma_display->color444(15, 0, 0));
  dma_display->drawLine(dma_display->width() - 1, 0, 0, dma_display->height() - 1, dma_display->color444(15, 0, 0));
  // dma_display->flipDMABuffer();
  delay(250);

  // draw a blue circle
  dma_display->drawCircle(10, 10, 10, dma_display->color444(0, 0, 15));
  // dma_display->flipDMABuffer();
  delay(250);

  // fill a violet circle
  dma_display->fillCircle(40, 21, 10, dma_display->color444(15, 0, 15));
  // dma_display->flipDMABuffer();
  delay(250);
  delay(1000);
}

void configure_panel(bool double_buff)
{
  // if (dma_display)
  // {
  //   delete dma_display;
  //   dma_display = nullptr;
  // }

  HUB75_I2S_CFG::i2s_pins _pins = {G1_PIN, B1_PIN, R1_PIN, G2_PIN, B2_PIN, R2_PIN, A_PIN, B_PIN, C_PIN, D_PIN, E_PIN, LAT_PIN, OE_PIN, CLK_PIN};
  HUB75_I2S_CFG mxconfig(
      PANEL_RES_X, // module width
      PANEL_RES_Y, // module height
      PANEL_CHAIN, // Chain length
      _pins);

  mxconfig.double_buff = double_buff;
  // mxconfig.latch_blanking = 2;
  mxconfig.clkphase = false;
  // mxconfig.i2sspeed = HUB75_I2S_CFG::HZ_10M;

  // Display Setup
  dma_display = new MatrixPanel_I2S_DMA(mxconfig);
  myBLACK = dma_display->color565(0, 0, 0);
  myWHITE = dma_display->color565(255, 255, 255);
  myGRAY = dma_display->color565(128, 128, 128);
  myLightGRAY = dma_display->color565(50, 50, 50);
  myRED = dma_display->color565(242, 20, 20);
  myGREEN = dma_display->color565(0, 255, 0);
  myBLUE = dma_display->color565(0, 128, 255);

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
  Serial.println("Restart reason: " + String(esp_reset_reason()));
  configure_panel(true);
  boot_message("WIFI!");
  dma_display->setFont(&TomThumb);

  if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS))
  {
    Serial.println("STA Failed to configure");
  }
  WiFi.begin(ssid, password);
  WiFi.setAutoReconnect(true);
  boot_message("LittleFS!");
  if (!LittleFS.begin(false))
  {
    Serial.println("An Error has occurred while mounting SPIFFS");
  }

  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print("Wifi.");
  }
  boot_message("WIFI OK!");
  // Serial.println("Connected!");
  // TODO: Fix the cert
  // espClient.setCACert(CA_CERT);
  espClient.setInsecure();

  boot_message("TASKS!");
  dht_mutex = xSemaphoreCreateMutex();
  task_handles[0] = NULL;
  xTaskCreate(dht_task, "dht_task", 8192, NULL, 5, NULL);
  xTaskCreate(mqtt_task, "mqtt_task", 16384, NULL, 5, NULL);
  xTaskCreate(mqtt_publish, "mqtt_publish", 8192, NULL, 5, NULL);
  // xTaskCreate(ntp_task, "ntp_task", 4096, NULL, 5, &task_handles[0]);
  configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", NTP_SERVER);
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
    Serial.println("ps_malloc failed");
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

void GIFDraw(GIFDRAW *pDraw)
{
  uint8_t *s;
  uint16_t *d, *usPalette, usTemp[320];
  int x, y, iWidth;

  usPalette = pDraw->pPalette;
  y = pDraw->iY + pDraw->y; // current line

  s = pDraw->pPixels;
  if (pDraw->ucDisposalMethod == 2) // restore to background color
  {
    for (x = 0; x < iWidth; x++)
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
} /* GIFDraw() */

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

#define CLOCK_OFFSET_Y 32
void draw_clock(bool night)
{
  const char *days[] = {
      "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
  const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
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
    dma_display->setCursor(20, CLOCK_OFFSET_Y + 17);
    dma_display->print(time.substring(6, 9));
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
  dma_display->printf("%s, %d %s\n", days[timeinfo.tm_wday], timeinfo.tm_mday, months[timeinfo.tm_mon]);
}

int round_float(float v)
{
  return (int)(v + 0.5f);
}

// ---- LOOP ----
void loop()
{

  static const uint32_t frameDelayMs = 1000 / 100;
  static uint32_t lastFrameTime = 0;
  static int someVariableHoldingFPS = 0;
  static unsigned long lastMillis = 0;
  static int frames = 0;
  if (POWER_SAVING)
  {
    if (SHOW_CLOCK_ON_SLEEP)
    {
      dma_display->setBrightness8(5);
      unsigned long t_start = millis();
      dma_display->clearScreen();
      draw_clock(true);
      dma_display->flipDMABuffer();
      unsigned long t_end = millis();
      unsigned long elapsed = t_end - t_start;
      delay(1000 - min(elapsed, 1000UL));
    }
    else
    {
      delay(5000);
    }
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
  else
  {
    delay(frameDelayMs - (now - lastFrameTime));
    return;
  }

  uint32_t t = now / 8;
  uint32_t mode = t % 4096;
  if (mode > 1024 && !ANIM_ONLY_MODE || ANIM_DISABLE)
  {
    if (xSemaphoreTake(dht_mutex, pdMS_TO_TICKS(0)) == pdTRUE)
    {
      // uint16_t rgb_color = rainbow565(t % 256);
      uint16_t rgb_color_rect = rainbow565((t + 64) % 256);
      // int x = t % (dma_display->width() + 10);
      // int x2 = (t + 16) % (dma_display->width() + 10);

      dma_display->clearScreen();

      dma_display->setTextSize(1);
      // CENTER LINE
      // dma_display->fillRect(31, 0, 2, 64, myWHITE);

      // RGB BORDER
      dma_display->drawRect(0, 0, 64, 64, rgb_color_rect);
      // dma_display->drawRect(1, 1, 62, 62, rgb_color_rect);
      // dma_display->fillCircle(x - 5, 55, 5, rgb_color);
      // dma_display->fillCircle(x2 - 5, 55, 5, rgb_color);

      float temp_sum = dht_temperature;
      float hum_sum = dht_humidity;
      int count = 1;
      if (dht_2_temperature >= 0 && dht_2_humidity >= 0)
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

      dma_display->setCursor(31, 62);
      draw_ram();

      dma_display->setCursor(3, 62);
      dma_display->setTextColor(myWHITE);
      dma_display->print("FPS");
      dma_display->setTextColor(myGRAY);
      dma_display->printf("%d", someVariableHoldingFPS);

      draw_clock(false);
      dma_display->flipDMABuffer();
    }
  }
  else
  {
    dma_display->clearScreen();
    dma_display->flipDMABuffer();

    memset(GIF_BUFFER, 0, 64 * 64 * 2);

    auto &myframes = PANEL_FRAMES[currentFrame];
    if (myframes.empty())
    {
      return;
    }
    if (played_gif >= myframes.size())
    {
      played_gif = 0;
    }
    Serial.println("Playing gif: " + currentFrame + ", ID: " + played_gif);
    gif.open(myframes[played_gif].data, myframes[played_gif].size, GIFDraw);

    int frameDelay = 0; // store delay for the last frame
    int then = 0;       // store overall delay

    while (gif.playFrame(true, &frameDelay))
    {
      // for (int y = 0; y < 64; y++)
      // {
      //   for (int x = 0; x < 64; x++)
      //   {
      //     int index = y * 64 + x;
      //     dma_display->drawPixel(x, y, GIF_BUFFER[index]);
      //   }
      // }
      dma_display->drawRGBBitmap(0, 0, GIF_BUFFER, 64, 64);

      dma_display->setTextSize(1);
      dma_display->setTextColor(myWHITE);
      dma_display->setCursor(0, 53);
      // dma_display->println("Ander");
      // dma_display->println("Darkity.top");
      dma_display->flipDMABuffer();

      then += frameDelay;
      if (then > maxGifDuration)
      { // avoid being trapped in infinite GIF's
        // log_w("Broke the GIF loop, max duration exceeded");
        break;
      }
    }

    gif.close();
    played_gif++;
  }
  // }
}