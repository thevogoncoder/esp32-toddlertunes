#include "Arduino.h"
#include "WiFi.h"
#include "Audio.h"

#include "SPI.h"
#include "SD.h"
#include "FS.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include <Battery.h>

#include <PN532_HSU.h>
#include <PN532.h>
#include <NfcAdapter.h>

#include <AceButton.h>
using namespace ace_button;

#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE
#include "esp_log.h"

#define NDEF_USE_SERIAL
PN532_HSU pn532_hsu(Serial2);
NfcAdapter nfc = NfcAdapter(pn532_hsu);

//SD Card
#define SD_CS 22
#define SPI_MOSI 23
#define SPI_MISO 19
#define SPI_SCK 18

//Digital I/O used  //Makerfabs Audio V2.0
#define I2S_DOUT 27
#define I2S_BCLK 26
#define I2S_LRC 25

//SSD1306
#define MAKEPYTHON_ESP32_SDA 4
#define MAKEPYTHON_ESP32_SCL 5
#define SCREEN_WIDTH 128  // OLED display width, in pixels
#define SCREEN_HEIGHT 64  // OLED display height, in pixels
#define OLED_RESET -1     // Reset pin # (or -1 if sharing Arduino reset pin)

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

//Button
const int Pin_vol_up = 39;
const int Pin_vol_down = 36;
const int Pin_mute = 35;

const int Pin_previous = 15;
const int Pin_pause = 33;
const int Pin_next = 2;

AceButton btn_vol_up(Pin_vol_up);
AceButton btn_vol_down(Pin_vol_down);
AceButton btn_vol_mute(Pin_mute);

AceButton btn_previous(Pin_previous);
AceButton btn_pause(Pin_pause);
AceButton btn_next(Pin_next);

const int Pin_On_Off = 32;
const int Pin_Batt_Sense = 34;

const int LCD_FONT_SIZE = 2;
const int LCD_CHAR_WIDTH = 6 * LCD_FONT_SIZE;
const int LCD_CHAR_HEIGHT = 8 * LCD_FONT_SIZE;

const int LCD_LINE_WIDTH = SCREEN_WIDTH / LCD_CHAR_WIDTH;
const int LCD_LINE_HEIGHT = LCD_CHAR_HEIGHT + 2;
const int LCD_LINES = SCREEN_HEIGHT / LCD_LINE_HEIGHT;

const string LCD_CLEAR_LINE(LCD_LINE_WIDTH, ' ');

Audio audio;

String ssid = "YourWiFiIsInAnotherCastle";
String password = "wlpassword013";

Battery batt = Battery(3200, 4200, Pin_Batt_Sense);

#define LOGO_HEIGHT   16
#define LOGO_WIDTH    16
static const unsigned char PROGMEM logo_bmp[] =
{ 0b00000000, 0b11000000,
  0b00000001, 0b11000000,
  0b00000001, 0b11000000,
  0b00000011, 0b11100000,
  0b11110011, 0b11100000,
  0b11111110, 0b11111000,
  0b01111110, 0b11111111,
  0b00110011, 0b10011111,
  0b00011111, 0b11111100,
  0b00001101, 0b01110000,
  0b00011011, 0b10100000,
  0b00111111, 0b11100000,
  0b00111111, 0b11110000,
  0b01111100, 0b11110000,
  0b01110000, 0b01110000,
  0b00000000, 0b00110000 };

//****************************************************************************************
//                                   S E T U P                                           *
//****************************************************************************************

enum : uint8_t { FILE_TYPE,
                 STREAM_TYPE };

struct Music_info {
  String title;
  String description;
  uint8_t type;
  int length;
  int runtime;
  int volume;
  int status;
  int mute_volume;
} music_info = { "", "", FILE_TYPE, 0, 0, 0, 0, 0 };

String file_list[20];
int file_num = 0;
int file_index = 0;

void setup() {
  //IO mode init
  pinMode(Pin_vol_up, INPUT_PULLUP);
  pinMode(Pin_vol_down, INPUT_PULLUP);
  pinMode(Pin_mute, INPUT_PULLUP);
  pinMode(Pin_previous, INPUT_PULLUP);
  pinMode(Pin_pause, INPUT_PULLUP);
  pinMode(Pin_next, INPUT_PULLUP);

  ButtonConfig* config = ButtonConfig::getSystemButtonConfig();
  config->setEventHandler(buttonHandler);

  pinMode(Pin_On_Off, OUTPUT);
  digitalWrite(Pin_On_Off, HIGH);

  pinMode(Pin_Batt_Sense, INPUT);
  analogReadResolution(10);
  batt.begin(3300, 1.47, &linear);

  Serial.begin(115200);

  //LCD
  Wire.begin(MAKEPYTHON_ESP32_SDA, MAKEPYTHON_ESP32_SCL);
  // SSD1306_SWITCHCAPVCC = generate display voltage from 3.3V internally
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {  // Address 0x3C for 128x32
    log_e("SSD1306 allocation failed");
    for (;;)
      ;  // Don't proceed, loop forever
  }
  // display.clearDisplay();
  // logoshow();
  testdrawbitmap();

  WiFi.begin(ssid.c_str(), password.c_str());
  while (WiFi.status() != WL_CONNECTED) delay(500);

  //SD(SPI)
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);
  SPI.setFrequency(1000000);
  if (!SD.begin(SD_CS, SPI)) {
    Serial.println("Card Mount Failed");
    lcd_text("SD ERR");
    while (1)
      ;
  } else {
    lcd_text("SD OK");
  }

  //Read SD
  file_num = get_music_list(SD, "/", 0, file_list);
  Serial.print("Music file count:");
  Serial.println(file_num);
  Serial.println("All music:");
  for (int i = 0; i < file_num; i++) {
    Serial.println(file_list[i]);
  }

  nfc.begin(true);

  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  audio.setVolume(15);  // 0...21

  audio.connecttohost("http://mp3.ffh.de/radioffh/hqlivestream.mp3");
  music_info.volume = 15;
  audio.setVolume(15);
  log_i("current volume is: %d", audio.getVolume());
  log_i("current battery level is %d%%[%dmV]", batt.level(), batt.voltage());
}

//****************************************************************************************
//                                   L O O P                                             *
//****************************************************************************************

uint button_time = 0;
uint adc_time = 0;

void loop() {
  // if (millis() - adc_time > 1000) {
  //   music_info.status = batt.voltage();
  //   log_v("current battery level is %d%%[%dmV]", batt.level(music_info.status), music_info.status);
  //   adc_time = millis();
  // }

  // if (nfc.tagPresent()) {
  //   NfcTag tag = nfc.read();
  //   tag.print();
  // }

  // if (millis() - button_time > 300) {
  //   //Button logic
  //   if (digitalRead(Pin_next) == 0) {
  //     log_d("Pin_next");
  //     // if (file_index < file_num - 1)
  //     //     file_index++;
  //     // else
  //     //     file_index = 0;
  //     // open_new_song(file_list[file_index]);
  //     // print_song_time();
  //     button_time = millis();
  //   }
  //   if (digitalRead(Pin_previous) == 0) {
  //     log_d("Pin_previous");
  //     // if (file_index > 0)
  //     //     file_index--;
  //     // else
  //     //     file_index = file_num - 1;
  //     // open_new_song(file_list[file_index]);
  //     // print_song_time();
  //     button_time = millis();
  //   }
  //   if (digitalRead(Pin_vol_up) == 0) {
  //     log_d("Pin_vol_up");
  //     if (music_info.volume < 21)
  //       music_info.volume++;
  //     audio.setVolume(music_info.volume);
  //     lcd_display_music();
  //     button_time = millis();
  //   }
  //   if (digitalRead(Pin_vol_down) == 0) {
  //     log_d("Pin_vol_down");
  //     if (music_info.volume > 0)
  //       music_info.volume--;
  //     audio.setVolume(music_info.volume);
  //     lcd_display_music();
  //     button_time = millis();
  //   }
  //   if (digitalRead(Pin_mute) == 0) {
  //     log_d("Pin_mute");
  //     if (music_info.volume != 0) {
  //       music_info.mute_volume = music_info.volume;
  //       music_info.volume = 0;
  //     } else {
  //       music_info.volume = music_info.mute_volume;
  //     }
  //     audio.setVolume(music_info.volume);
  //     lcd_display_music();
  //     button_time = millis();
  //   }
  //   if (digitalRead(Pin_pause) == 0) {
  //     log_d("Pin_pause");
  //     // audio.pauseResume();
  //     digitalWrite(Pin_On_Off, LOW);
  //     button_time = millis();
  //   }
  // }

  btn_vol_up.check();
  btn_vol_down.check();
  btn_vol_mute.check();

  btn_previous.check();
  btn_pause.check();
  btn_next.check();

  audio.loop();
}

//****************************************************************************************
//                                 M E T H O D S                                         *
//****************************************************************************************

void testdrawbitmap(void) {
  display.clearDisplay();

  display.drawBitmap(
    (display.width()  - LOGO_WIDTH ) / 2,
    (display.height() - LOGO_HEIGHT) / 2,
    logo_bmp, LOGO_WIDTH, LOGO_HEIGHT, 1);
  display.display();
  // delay(1000);
}

void logoshow(void) {
  display.clearDisplay();

  display.setTextSize(2);               // Normal 1:1 pixel scale
  display.setTextColor(SSD1306_WHITE);  // Draw white text
  display.setCursor(0, 0);              // Start at top-left corner
  display.println(F("MakePython"));
  display.setCursor(0, 20);  // Start at top-left corner
  display.println(F("MUSIC"));
  display.setCursor(0, 40);  // Start at top-left corner
  display.println(F("PLAYER V2"));
  display.display();
  // delay(2000);
}

void lcd_text(String text) {
  display.clearDisplay();

  display.setTextSize(2);               // Normal 1:1 pixel scale
  display.setTextColor(SSD1306_WHITE);  // Draw white text
  display.setCursor(0, 0);              // Start at top-left corner
  display.println(text);
  display.display();
  // delay(500);
}

void lcd_display_music() {
  display.clearDisplay();
  display.setTextSize(LCD_FONT_SIZE);   // Normal 1:1 pixel scale
  display.setTextColor(SSD1306_WHITE);  // Draw white text
  display.setTextWrap(false);
  char buff[20];

  switch (music_info.type) {
    case FILE_TYPE:
      display.setCursor(0, 0);

      display.println(music_info.title);

      sprintf(buff, "%d:%d", music_info.runtime, music_info.length);
      display.println(buff);

      sprintf(buff, "Vol: %d", music_info.volume);
      display.println(buff);

      display.setTextSize(1);
      sprintf(buff, "Batt: %d", batt.level(music_info.status));
      display.println(buff);
      break;
    case STREAM_TYPE:
      display.setCursor(0, 0);

      display.println(music_info.title);

      display.setTextSize(1);
      display.setTextWrap(true);
      display.println(music_info.description);

      display.setTextSize(LCD_FONT_SIZE);
      sprintf(buff, "Vol: %d", music_info.volume);
      display.println(buff);

      display.setTextSize(1);
      sprintf(buff, "Batt: %d", batt.level(music_info.status));
      display.println(buff);
      break;
  }

  display.display();
}

int get_music_list(fs::FS &fs, const char *dirname, uint8_t levels, String wavlist[30]) {
  Serial.printf("Listing directory: %s\n", dirname);
  int i = 0;

  File root = fs.open(dirname);
  if (!root) {
    Serial.println("Failed to open directory");
    return i;
  }
  if (!root.isDirectory()) {
    Serial.println("Not a directory");
    return i;
  }

  File file = root.openNextFile();
  while (file) {
    if (file.isDirectory()) {
    } else {
      String temp = file.name();
      if (temp.endsWith(".wav")) {
        wavlist[i] = temp;
        i++;
      } else if (temp.endsWith(".mp3")) {
        wavlist[i] = temp;
        i++;
      }
    }
    file = root.openNextFile();
  }
  return i;
}

//*****************************************************************************************
//                                  E V E N T S                                           *
//*****************************************************************************************

void buttonHandler(AceButton* button, uint8_t eventType, uint8_t buttonState) {
  switch (button->getPin()) {
    default:
      log_d("Event: Button %d with type %d and state %d", button->getPin(), eventType, buttonState);
      break;
  }
}

void audio_info(const char *info) {
  log_d("info: %s", info);
}
void audio_id3data(const char *info) {  //id3 metadata
  log_d("id3data: %s", info);
}

//歌曲结束逻辑
void audio_eof_mp3(const char *info) {  //end of file
  log_d("eof_mp3: %s", info);
  //file_index++;
  //if (file_index >= file_num)
  //{
  //    file_index = 0;
  //}
  //open_new_song(file_list[file_index]);
}
void audio_showstation(const char *info) {
  log_d("station: %s", info);

  music_info.type = STREAM_TYPE;
  music_info.title = String(info);
  music_info.description = String("");
  lcd_display_music();
}
void audio_showstreaminfo(const char *info) {
  log_d("streaminfo: %s", info);
}
void audio_showstreamtitle(const char *info) {
  log_d("streamtitle: %s", info);

  music_info.description = String(info);
  lcd_display_music();
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