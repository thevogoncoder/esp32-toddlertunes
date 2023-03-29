#include "Arduino.h"
//#include "WiFiMulti.h"
#include "Audio.h"
#include <SPI.h>
#include <SD.h>
#include <FS.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <PN532_HSU.h>
#include <PN532.h>
#include <Dictionary.h>
#include "avdweb_Switch.h"
#include <TaskScheduler.h>
#include "esp_log.h"
#include <AverageFilter.h>

// SD Card
#define SD_CS 22
#define SPI_MOSI 23
#define SPI_MISO 19
#define SPI_SCK 18

// Digital I/O used  //Makerfabs Audio V2.0
#define I2S_DOUT 27
#define I2S_BCLK 26
#define I2S_LRC 25

// SSD1306
#define MAKEPYTHON_ESP32_SDA 4
#define MAKEPYTHON_ESP32_SCL 5
#define SCREEN_WIDTH 128 // OLED display width, in pixels
#define SCREEN_HEIGHT 64 // OLED display height, in pixels
#define OLED_RESET -1    // Reset pin # (or -1 if sharing Arduino reset pin)

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

PN532_HSU pn532hsu(Serial2);
PN532 nfc(pn532hsu);

// Power control
const int Pin_On_Off = 32;
const int Pin_Batt_Sense = 34;

// Button
const int Pin_vol_up = 39;
const int Pin_vol_down = 36;
const int Pin_mute = 35;

const int Pin_previous = 15;
const int Pin_pause = 33;
const int Pin_next = 2;

Switch btn_vol_up(Pin_vol_up);
Switch btn_vol_down(Pin_vol_down);
Switch btn_mute(Pin_mute);
Switch btn_previous(Pin_previous);
Switch btn_pause(Pin_pause);
Switch btn_next(Pin_next);

Audio audio;

/*
WiFiMulti wifiMulti;
String ssid = "Makerfabs";
String password = "20160704";
*/

struct Music_info
{
  String name;
  int length;
  int runtime;
  int volume;
  int status;
  int mute_volume;
} music_info = {"", 0, 0, 0, 0, 0};

struct Battery_info
{
  int level;
  int voltage;
} battery_info = {0, 0};


uint8_t uid[] = { 0, 0, 0, 0, 0, 0, 0 };	// Buffer to store the returned UID
uint8_t uidLength;				// Length of the UID (4 or 7 bytes depending on ISO14443A card type)

struct Tag_info
{
  String uid;
  uint timestamp;
} tag_info = {"", 0};

String file_list[20];
int file_num = 0;
int file_index = 0;

Dictionary &tags = *(new Dictionary());

const int BATTERY_LEVEL_SAMPLES = 64;

averageFilter<int> batteryLevel(64);

// Scheduler
Scheduler ts;

bool programming_mode = false;

// forward declare callback functions
void nfcLoop();
void onTagDetected(String uid);
bool inputEnableCBs();
void inputDisableCBs();
void inputLoop();
void audioLoop();
void update_audio_info();

void updateBatteryLevel();

bool onDisplayEnabled();
void displayUpdate();
void onDisplayDisabled();

void displaySleepTimerUpdate();
void displaySleepTimerElapsed();

void poweroffTimerUpdate();
void poweroffTimerElapsed();

void vol_up_clicked(void* s);
void vol_down_clicked(void* s);
void mute_clicked(void* s);
void mute_long_clicked(void* s);
void previous_clicked(void* s);
void pause_clicked(void* s);
void pause_long_clicked(void* s);
void pause_released(void* s);
void next_clicked(void* s);

void save_tags_dict(fs::FS &fs, const char *filename);
int load_tags_dict(fs::FS &fs, const char *filename);

void display_sleep();
void display_wakeup();

#define POWEROFF_TIMOUT (2 * TASK_MINUTE)
#define DISPLAY_IDLE_TIMOUT (10 * TASK_SECOND)
#define INPUT_POLL_FAST (20 * TASK_MILLISECOND)
#define INPUT_POLL_IDLE (200 * TASK_MILLISECOND)

// Tasks
Task audioTask(3 * TASK_MILLISECOND, TASK_FOREVER, &audioLoop, &ts);  //adding task to the chain on creation
Task audioMetadataTask(1 * TASK_SECOND, TASK_FOREVER, &update_audio_info, &ts);  //adding task to the chain on creation
Task inputTask(20 * TASK_MILLISECOND, TASK_FOREVER, &inputLoop, &ts, false, &inputEnableCBs, &inputDisableCBs);      //adding task to the chain on creation
Task nfcTask(1 * TASK_SECOND, TASK_FOREVER, &nfcLoop, &ts);      //adding task to the chain on creation
Task displayTask(500 * TASK_MILLISECOND, TASK_FOREVER, &displayUpdate, &ts, false, &onDisplayEnabled, &onDisplayDisabled);  //adding task to the chain on creation
Task displaySleepTimerTask(1 * TASK_SECOND, TASK_FOREVER, &displaySleepTimerUpdate, &ts, false, nullptr, &displaySleepTimerElapsed);  //adding task to the chain on creation
Task poweroffTimerTask(1 * TASK_SECOND, TASK_FOREVER, &poweroffTimerUpdate, &ts, false, nullptr, &poweroffTimerElapsed);  //adding task to the chain on creation
Task batteryUpdateTask(1 * TASK_MINUTE, TASK_FOREVER, &updateBatteryLevel, &ts);  //adding task to the chain on creation

void updateBatteryLevel()
{
  for (size_t i = 0; i < 64; i++)
  {
    batteryLevel.value(analogRead(Pin_Batt_Sense));
  }
  
  battery_info.voltage = batteryLevel.currentValue();
  battery_info.level = map(battery_info.voltage, 0, 4095, 0, 100);
}

void audioLoop()
{
  audio.loop();
}

void update_audio_info()
{
  music_info.runtime = audio.getAudioCurrentTime();
  music_info.length = audio.getAudioFileDuration();
}

void poweroffTimerUpdate()
{
  if(audio.isRunning()) {
    poweroffTimerTask.resetTimeout();
  } else {
    log_v("Idle timer update: %d ms left", poweroffTimerTask.untilTimeout());
  }
}

void displaySleepTimerUpdate()
{
  log_v("Display sleep timer update: %d ms left", displaySleepTimerTask.untilTimeout());
}

void displaySleepTimerElapsed()
{
  if(displaySleepTimerTask.timedOut()) {
    log_i("Display sleep timer elapsed");
    display_sleep();
  } else {
    log_v("Display sleep timer disabled");
  }
}

void poweroffTimerElapsed()
{
  if(poweroffTimerTask.timedOut()) {
    log_i("Idle timer elapsed");
    // do sleep stuff###+
    nfc.powerDownMode();
    display.ssd1306_command(SSD1306_DISPLAYOFF);
    esp_sleep_enable_timer_wakeup(1000 * TASK_HOUR);
    delay(1000);
    esp_deep_sleep_start();
  } else {
    log_i("Idle timer disabled");
  }
}

bool inputEnableCBs()
{
  btn_vol_up.setSingleClickCallback(&vol_up_clicked);
  btn_vol_down.setSingleClickCallback(&vol_down_clicked);
  btn_mute.setSingleClickCallback(&mute_clicked);
  btn_mute.setLongPressCallback(&mute_long_clicked);
  btn_previous.setSingleClickCallback(&previous_clicked);
  btn_pause.setSingleClickCallback(&pause_clicked);
  btn_pause.setLongPressCallback(&pause_long_clicked);
  btn_pause.setReleasedCallback(&pause_released);
  btn_next.setSingleClickCallback(&next_clicked);
  return true;
}

void inputDisableCBs()
{
  btn_vol_up.setSingleClickCallback(nullptr);
  btn_vol_down.setSingleClickCallback(nullptr);
  btn_mute.setSingleClickCallback(nullptr);
  btn_previous.setSingleClickCallback(nullptr);
  btn_pause.setSingleClickCallback(nullptr);
  btn_next.setSingleClickCallback(nullptr);
}

void inputLoop() {
  btn_vol_up.poll();
  btn_vol_down.poll();
  btn_mute.poll();
  btn_previous.poll();
  btn_pause.poll();
  btn_next.poll();

  // reset interval to fast polling interval if button is pressed
  if(inputTask.getInterval() != INPUT_POLL_FAST) {
    if(btn_vol_up.pushed()
    || btn_vol_down.pushed()
    || btn_mute.pushed()
    || btn_previous.pushed()
    || btn_pause.pushed()
    || btn_next.pushed()) {
      inputTask.setInterval(INPUT_POLL_FAST);
    }
  }
}

void print_tag_info()
{
  Serial.println("**************TAG*****************");
  Serial.println(tag_info.uid);
  // Serial.println(tag_info.timestamp);
  Serial.println("***********************************");
}

String getUidString()
{
    String uidString = "";
    for (unsigned int i = 0; i < uidLength; i++)
    {
        if (i > 0)
        {
            uidString += " ";
        }

        if (uid[i] < 0xF)
        {
            uidString += "0";
        }

        uidString += String((unsigned int)uid[i], (unsigned char)HEX);
    }
    uidString.toUpperCase();
    return uidString;
}

void nfcLoop()
{
  // log_d("nfcLoop");
  if(nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, &uid[0], &uidLength, 100)) {
    // log_d("tagPresent");
    onTagDetected(getUidString());
  } else {
    // log_d("no tag");
    tag_info.uid = "";
  }
}

bool onDisplayEnabled()
{
  display.ssd1306_command(SSD1306_DISPLAYON);
  return true;
}

void onDisplayDisabled()
{
  display.ssd1306_command(SSD1306_DISPLAYOFF);
}

char buf[20];
void displayUpdate()
{
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0, 0);
  if(programming_mode) {
    display.println("Programming Mode");
  } else {
    display.println("Music Player");
  }
  display.println("--------------");

  display.setTextWrap(true);
  display.println(music_info.name);
  display.setTextWrap(false);

  sprintf(buf, "runtime %3d/%03d", music_info.runtime, music_info.length);
  display.println(buf);

  sprintf(buf, "volume %2d/21", music_info.volume);
  display.println(buf);

  sprintf(buf, "battery %3d%%", battery_info.level);
  display.println(buf);

  display.println("--------------");
  display.println(tag_info.uid);

  display.display();
}

void open_new_song(String filename)
{
  //去掉文件名的根目录"/"和文件后缀".mp3",".wav"
  // music_info.name = filename.substring(1, filename.indexOf("."));
  music_info.name = filename;
  audio.connecttoFS(SD, filename.c_str());
  music_info.runtime = audio.getAudioCurrentTime();
  music_info.length = audio.getAudioFileDuration();
  music_info.volume = audio.getVolume();
  music_info.status = 1;
  Serial.println("**********start a new sound************");
}

void onTagDetected(String uid)
{
  // Return if tag is not new
  if(tag_info.uid == uid) {
    return;
  }

  tag_info.uid = uid;
  log_d("Tag detected with uid: %s", tag_info.uid.c_str());
  String filename = tags.search(tag_info.uid);
  if(!programming_mode) {
    if (!filename.isEmpty()) {
      log_d("Tag found in dictionary: %s", filename.c_str());
      open_new_song(filename);
    }
  } else {
    tags.insert(tag_info.uid, music_info.name);
    log_d("New tag inserted: %s > %s", tag_info.uid.c_str(), music_info.name.c_str());
    
    save_tags_dict(SD, "/tags.json");
  }
  print_tag_info();
  display_wakeup();
}

void vol_up_clicked(void* s)
{
  log_d("vol_up_clicked");
  if (music_info.volume < 21) {
    music_info.volume++;
    audio.setVolume(music_info.volume);
  }
  display_wakeup();
}

void vol_down_clicked(void* s)
{
  log_d("vol_down_clicked");
  if (music_info.volume > 0) {
    music_info.volume--;
    audio.setVolume(music_info.volume);
  }
  display_wakeup();
}

void mute_clicked(void* s)
{
  log_d("mute_clicked");
  if (music_info.volume > 0) {
    music_info.mute_volume = music_info.volume;
    music_info.volume = 0;
    audio.setVolume(music_info.volume);
  } else {
    music_info.volume = music_info.mute_volume;
    audio.setVolume(music_info.volume);
  }
  display_wakeup();
}

void mute_long_clicked(void* s)
{
  log_d("mute_long_clicked");
  // if(nfc.shield->setRFField(0x02, 0x01)) {
  //   log_d("NFC shield rf field is on");
  // } else {
  //   log_d("NFC shield rf field is off");
  // }
  // if(nfc.shield->powerDownMode()) {
  //   log_d("NFC shield is in power down mode");
  // } else {
  //   log_d("NFC shield is in normal mode");
  // }
  if(nfc.startPassiveTargetIDDetection(PN532_MIFARE_ISO14443A)) {
    log_d("NFC shield is in passive target mode");
  } else {
    log_d("NFC shield is not in passive target mode");
  }
  display_wakeup();
}

void previous_clicked(void* s)
{
  log_d("previous_clicked");
  if (file_index > 0) {
    file_index--;
    open_new_song(file_list[file_index]);
  }
  display_wakeup();
}

void pause_clicked(void* s)
{
  log_d("pause_clicked");
  if (music_info.status == 1) {
    audio.pauseResume();
    music_info.status = 0;
  } else {
    audio.pauseResume();
    music_info.status = 1;
  }
  display_wakeup();
}

void pause_long_clicked(void* s)
{
  log_d("pause_long_clicked");
  programming_mode = true;
  display_wakeup();
}

void pause_released(void* s)
{
  log_d("pause_released");
  programming_mode = false;
  display_wakeup();
}

void next_clicked(void* s)
{
  log_d("next_clicked");
  if (file_index < file_num - 1) {
    file_index++;
    open_new_song(file_list[file_index]);
  }
  display_wakeup();
}

void display_sleep() {
  displaySleepTimerTask.disable();
  audioMetadataTask.disable();
  displayTask.disable();
  inputTask.setInterval(INPUT_POLL_IDLE);
}

void display_wakeup() {
  displaySleepTimerTask.enableIfNot();
  displaySleepTimerTask.resetTimeout();
  audioMetadataTask.enableIfNot();
  displayTask.enableIfNot();
  displayTask.forceNextIteration();
}

void logoshow(void)
{
  display.clearDisplay();

  display.setTextSize(2);              // Normal 1:1 pixel scale
  display.setTextColor(SSD1306_WHITE); // Draw white text
  display.setCursor(0, 0);             // Start at top-left corner
  display.println(F("Nahuelito"));
  display.setCursor(0, 20); // Start at top-left corner
  display.println(F("Music"));
  display.setCursor(0, 40); // Start at top-left corner
  display.println(F("Box V2"));
  display.display();
  // delay(2000);
}

void lcd_error_show(String text)
{
  display.clearDisplay();

  display.setTextSize(2);              // Normal 1:1 pixel scale
  display.setTextColor(SSD1306_WHITE); // Draw white text
  display.setCursor(0, 0);             // Start at top-left corner
  display.println(text);
  display.display();
  delay(1000);
}

void print_song_info()
{
  Serial.println("***********************************");
  Serial.println(audio.getFileSize());
  Serial.println(audio.getFilePos());
  Serial.println(audio.getSampleRate());
  Serial.println(audio.getBitsPerSample());
  Serial.println(audio.getChannels());
  Serial.println(audio.getVolume());
  Serial.println("***********************************");
}

int load_tags_dict(fs::FS &fs, const char *filename)
{
  Serial.printf("Loading tags info from file '%s'\n", filename);

  File tags_file = fs.open(filename);
  if (!tags_file)
  {
    Serial.printf("%s: File not found\n", filename);
    return 0;
  }

  int r = tags.jload(tags_file);
  if (r != DICTIONARY_OK)
  {
    Serial.printf("%s: Error loading file (%d)\n", filename, r);
    return 0;
  }

  Serial.println("***************TAGS****************");
  for (int i = 0; i < tags.count(); i++)
  {
    Serial.printf("%s > %s\n", tags(i).c_str(), tags[i]);
  }
  Serial.println("***********************************");
  return tags.count();
}

void save_tags_dict(fs::FS &fs, const char *filename)
{
  Serial.printf("Saving tags info to file '%s'\n", filename);

  File tags_file = fs.open(filename, FILE_WRITE);
  if (!tags_file)
  {
    Serial.printf("%s: File not found\n", filename);
    return;
  }

  String tags_json = tags.json();
  if (!tags_file.print(tags_json.c_str()))
  {
    Serial.printf("%s: Error saving file\n", filename);
    return;
  }
}

int get_music_list(fs::FS &fs, const char *dirname, uint8_t levels, String wavlist[30])
{
  Serial.printf("Listing directory: %s\n", dirname);
  int i = 0;

  File root = fs.open(dirname);
  if (!root)
  {
    Serial.println("Failed to open directory");
    return i;
  }
  if (!root.isDirectory())
  {
    Serial.println("Not a directory");
    return i;
  }

  File file = root.openNextFile();
  while (file)
  {
    if (file.isDirectory())
    {
    }
    else
    {
      String temp = file.name();
      if (temp.endsWith(".wav"))
      {
        wavlist[i] = temp;
        i++;
      }
      else if (temp.endsWith(".mp3"))
      {
        wavlist[i] = temp;
        i++;
      }
    }
    file = root.openNextFile();
  }
  return i;
}

void setup()
{
  // Power
  pinMode(Pin_On_Off, OUTPUT);
  digitalWrite(Pin_On_Off, HIGH);

  // Serial
  Serial.begin(115200);

  // LCD
  Wire.begin(MAKEPYTHON_ESP32_SDA, MAKEPYTHON_ESP32_SCL);
  // SSD1306_SWITCHCAPVCC = generate display voltage from 3.3V internally
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C))
  { // Address 0x3C for 128x32
    Serial.println(F("SSD1306 allocation failed"));
    for (;;)
      ; // Don't proceed, loop forever
  }
  logoshow();

  nfc.begin();

  uint32_t versiondata = nfc.getFirmwareVersion();
  if (! versiondata) {
    Serial.print("Didn't find PN53x board");
    lcd_error_show("NFC ERR");
    while (1); // halt
  }

  // Got ok data, print it out!
  Serial.print("Found chip PN5"); Serial.println((versiondata>>24) & 0xFF, HEX);
  Serial.print("Firmware ver. "); Serial.print((versiondata>>16) & 0xFF, DEC);
  Serial.print('.'); Serial.println((versiondata>>8) & 0xFF, DEC);

  // configure board to read RFID tags
  nfc.SAMConfig();

  // SD(SPI)
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);
  SPI.setFrequency(1000000);
  if (!SD.begin(SD_CS, SPI))
  {
    Serial.println("Card Mount Failed");
    lcd_error_show("SD ERR");
    while (1)
      ;
  }
  else
  {
    // lcd_text("SD OK");
  }

  // Read SD
  file_num = get_music_list(SD, "/", 0, file_list);
  Serial.print("Music file count:");
  Serial.println(file_num);
  Serial.println("All music:");
  for (int i = 0; i < file_num; i++)
  {
    Serial.println(file_list[i]);
  }

  // Read tags dictionary from SD
  file_num = load_tags_dict(SD, "/tags.json");

  // WiFi
  /*
  WiFi.mode(WIFI_STA);
  wifiMulti.addAP(ssid.c_str(), password.c_str());
  wifiMulti.run();
  if (WiFi.status() != WL_CONNECTED)
  {
      WiFi.disconnect(true);
      wifiMulti.run();
  }
  */

  // Audio(I2S)
  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  audio.setVolume(16); // 0...21

  // audio.connecttoFS(SD, "/MoonlightBay.mp3"); //ChildhoodMemory.mp3  //MoonRiver.mp3 //320k_test.mp3
  // file_list[0] = "MoonlightBay.mp3";
  open_new_song(file_list[file_index]);

  batteryLevel.initialize();

  audioTask.enable();
  audioMetadataTask.enable();
  inputTask.enable();
  nfcTask.enable();
  displayTask.enable();
  poweroffTimerTask.enable();
  poweroffTimerTask.setTimeout(POWEROFF_TIMOUT);
  displaySleepTimerTask.enable();
  displaySleepTimerTask.setTimeout(DISPLAY_IDLE_TIMOUT);
  batteryUpdateTask.enable();
}

void loop()
{
  ts.execute();
}

//**********************************************
// optional
//**********************************************

void audio_info(const char *info) {
  log_d("info: %s", info);
}
void audio_id3data(const char *info) {  //id3 metadata
  log_d("id3data: %s", info);
}

//歌曲结束逻辑
void audio_eof_mp3(const char *info) {  //end of file
  log_d("eof_mp3: %s", info);
  file_index++;
  if (file_index >= file_num)
  {
     file_index = 0;
  }
  open_new_song(file_list[file_index]);
}
void audio_showstation(const char *info) {
  log_d("station: %s", info);

  // music_info.type = STREAM_TYPE;
  // music_info.title = String(info);
  // music_info.description = String("");
  // lcd_display_music();
}
void audio_showstreaminfo(const char *info) {
  log_d("streaminfo: %s", info);
}
void audio_showstreamtitle(const char *info) {
  log_d("streamtitle: %s", info);

  // music_info.description = String(info);
  // lcd_display_music();
}
void audio_bitrate(const char *info) {
  log_d("bitrate: %s", info);
}
void audio_commercial(const char *info) {  //duration in sec
  log_d("commercial: %s", info);
}
void audio_icyurl(const char *info) {  //homepage
  log_d("icyurl: %s", info);
}
void audio_lasthost(const char *info) {  //stream URL played
  log_d("lasthost: %s", info);
}
void audio_eof_speech(const char *info) {
  log_d("eof_speech: %s", info);
}