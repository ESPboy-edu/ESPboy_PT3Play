//v1.2 14.12.2019 linear interp for better quality, analyzer display fix, backlight off during startup
//v1.1 14.12.2019 hardware init fix, stereo and i2s support
//v1.0 13.12.2019 initial version
//by Shiru
//shiru@mail.ru
//https://www.patreon.com/shiru8bit

//configure output device
//if the i2s DAC is selected, but not connected, ESPboy crashes

enum {
  OUT_SPEAKER = 0,
  OUT_I2S
};

#define OUTPUT_DEVICE   OUT_SPEAKER
//#define OUTPUT_DEVICE   OUT_I2S



#include <Adafruit_MCP23017.h>
#include <Adafruit_MCP4725.h>
#include <Adafruit_ST7735.h>
#include <Adafruit_GFX.h>
#include <ESP8266WiFi.h>
#include <sigma_delta.h>
#include <i2s.h>
#include <i2s_reg.h>

#include "glcdfont.c"

#include "gfx\espboy.h"

#define MCP23017address 0 // actually it's 0x20 but in <Adafruit_MCP23017.h> lib there is (x|0x20) :)

//PINS
#define LEDPIN         D4
#define SOUNDPIN       D3

//SPI for LCD
#define csTFTMCP23017pin 8
#define TFT_RST       -1
#define TFT_DC        D8
#define TFT_CS        -1

Adafruit_MCP23017 mcp;
Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);

#define MCP4725address 0x60
Adafruit_MCP4725 dac;

#define AY_CLOCK      1773400         //pitch
#define SAMPLE_RATE   44010           //quality of the sound, i2s DAC can't handle more than 44100 by some reason (not even 48000)
#define FRAME_RATE    50              //speed


volatile uint32_t sound_stereo_dac;

int pad_state;
int pad_state_prev;
int pad_state_t;


#define PAD_LEFT        0x01
#define PAD_UP          0x02
#define PAD_DOWN        0x04
#define PAD_RIGHT       0x08
#define PAD_A           0x10
#define PAD_B           0x20
#define PAD_ANY         (PAD_UP|PAD_DOWN|PAD_LEFT|PAD_RIGHT|PAD_A|PAD_B)


#define SPEC_BANDS        42
#define SPEC_HEIGHT       48
#define SPEC_RANGE        1000
#define SPEC_SX           0
#define SPEC_SY           64
#define SPEC_BAND_WIDTH   3
#define SPEC_DECAY        3

#define SPEC_CHA_COL      0x041F
#define SPEC_CHB_COL      0xFC18
#define SPEC_CHC_COL      0xFC1F
#define SPEC_ENV_COL      0x87F0

volatile int16_t spec_levels[SPEC_BANDS];
volatile int16_t spec_levels_prev[SPEC_BANDS];
volatile uint16_t spec_colors[SPEC_BANDS];

uint8_t music_data[32768];
int music_data_size;

int output_device;



#include "ay_emu.h"

struct PT3_Channel_Parameters
{
  unsigned short Address_In_Pattern, OrnamentPointer, SamplePointer, Ton;
  unsigned char Loop_Ornament_Position, Ornament_Length, Position_In_Ornament, Loop_Sample_Position, Sample_Length, Position_In_Sample, Volume, Number_Of_Notes_To_Skip, Note, Slide_To_Note, Amplitude;
  bool Envelope_Enabled, Enabled, SimpleGliss;
  short Current_Amplitude_Sliding, Current_Noise_Sliding, Current_Envelope_Sliding, Ton_Slide_Count, Current_OnOff, OnOff_Delay, OffOn_Delay, Ton_Slide_Delay, Current_Ton_Sliding, Ton_Accumulator, Ton_Slide_Step, Ton_Delta;
  signed char Note_Skip_Counter;
};

struct PT3_Parameters
{
  unsigned char Env_Base_lo;
  unsigned char Env_Base_hi;
  short Cur_Env_Slide, Env_Slide_Add;
  signed char Cur_Env_Delay, Env_Delay;
  unsigned char Noise_Base, Delay, AddToNoise, DelayCounter, CurrentPosition;
  int Version;
};

struct PT3_SongInfo
{
  PT3_Parameters PT3;
  PT3_Channel_Parameters PT3_A, PT3_B, PT3_C;
};

struct AYSongInfo
{
  unsigned char* module;
  unsigned char* module1;
  int module_len;
  PT3_SongInfo data;
  PT3_SongInfo data1;
  bool is_ts;

  AYChipStruct chip0;
  AYChipStruct chip1;
};

struct AYSongInfo AYInfo;

void ay_resetay(AYSongInfo* info, int chipnum)
{
  if (!chipnum) ay_init(&info->chip0); else ay_init(&info->chip1);
}

void ay_writeay(AYSongInfo* info, int reg, int val, int chipnum)
{
  if (!chipnum) ay_out(&info->chip0, reg, val); else ay_out(&info->chip1, reg, val);
}

#include "PT3Play.h"



int interruptCnt;



void spec_add(int hz, int level, uint16_t color)
{
  int i, off;
  const int curve[5] = {SPEC_HEIGHT / 10, SPEC_HEIGHT / 5, SPEC_HEIGHT / 2, SPEC_HEIGHT / 5, SPEC_HEIGHT / 10};

  if (hz)
  {
    off = hz / (SPEC_RANGE / SPEC_BANDS) - 2;

    if (off > SPEC_BANDS - 1) off = SPEC_BANDS - 1;

    for (i = 0; i < 5; ++i)
    {
      if (off >= 0 && off < SPEC_BANDS)
      {
        spec_levels[off] += curve[i] * level / 16;

        if (spec_levels[off] > SPEC_HEIGHT) spec_levels[off] = SPEC_HEIGHT;

        spec_colors[off] = color;
      }

      ++off;
    }
  }
}

void spec_update()
{
  int i;

  for (i = 0; i < SPEC_BANDS; ++i)
  {
    spec_levels[i] -= SPEC_DECAY;
    if (spec_levels[i] < 0) spec_levels[i] = 0;
  }
}

void spec_add_ay(AYChipStruct* chip)
{
  int period;

  if (!(chip->reg[7] & 0x01) && !(chip->reg[8] & 0x10))
  {
    period = chip->reg[0] + chip->reg[1] * 256;
    if (period) spec_add(AY_CLOCK / 16 / period, chip->reg[8], SPEC_CHA_COL);
  }
  if (!(chip->reg[7] & 0x02) && !(chip->reg[9] & 0x10))
  {
    period = chip->reg[2] + chip->reg[3] * 256;
    if (period) spec_add(AY_CLOCK / 16 / period, chip->reg[9], SPEC_CHB_COL);
  }
  if (!(chip->reg[7] & 0x04) && !(chip->reg[10] & 0x10))
  {
    period = chip->reg[4] + chip->reg[5] * 256;
    if (period) spec_add(AY_CLOCK / 16 / period, chip->reg[10], SPEC_CHC_COL);
  }
  if ((chip->reg[8] & 0x10) || (chip->reg[9] & 0x10) || (chip->reg[10] & 0x10))
  {
    period = chip->reg[11] + chip->reg[12] * 256;
    if (period) spec_add(AY_CLOCK / 16 / 16 / period, 12, SPEC_ENV_COL);
  }
}



uint32_t emulate_sample(void)
{
  uint32_t out_l, out_r;

  if (interruptCnt++ >= (SAMPLE_RATE / FRAME_RATE))
  {
    spec_update();

    spec_add_ay(&AYInfo.chip0);

    if (AYInfo.is_ts) spec_add_ay(&AYInfo.chip1);

    PT3_Play_Chip(AYInfo, 0);

    interruptCnt = 0;
  }

  ay_tick(&AYInfo.chip0, (AY_CLOCK / SAMPLE_RATE / 8));

  out_l = AYInfo.chip0.out[0] + AYInfo.chip0.out[1] / 2;
  out_r = AYInfo.chip0.out[2] + AYInfo.chip0.out[1] / 2;

  if (AYInfo.is_ts)
  {
    ay_tick(&AYInfo.chip1, (AY_CLOCK / SAMPLE_RATE / 8));

    out_l += AYInfo.chip0.out[0] + AYInfo.chip0.out[1] / 2;
    out_r += AYInfo.chip0.out[2] + AYInfo.chip0.out[1] / 2;
  }

  if (out_l > 32767) out_l = 32767;
  if (out_r > 32767) out_r = 32767;

  return out_l | (out_r << 16);
}



void ICACHE_RAM_ATTR sound_speaker_ISR()
{
  sigmaDeltaWrite(0, sound_stereo_dac);

  uint32_t out = emulate_sample();

  sound_stereo_dac = ((out & 0xff00) >> 8) + ((out & 0xff000000) >> 24); //convert to 8-bit mono
}



void ICACHE_RAM_ATTR sound_i2s_ISR()
{
  i2s_write_sample_nb(sound_stereo_dac);

  sound_stereo_dac = emulate_sample();
}



int check_key()
{
  pad_state_prev = pad_state;
  pad_state = 0;

  for (int i = 0; i < 8; i++)
  {
    if (!mcp.digitalRead(i)) pad_state |= (1 << i);
  }

  pad_state_t = pad_state ^ pad_state_prev & pad_state;

  return pad_state;
}



//0 no timeout, otherwise timeout in ms

void wait_any_key(int timeout)
{
  timeout /= 100;

  while (1)
  {
    check_key();

    if (pad_state_t&PAD_ANY) break;

    if (timeout)
    {
      --timeout;

      if (timeout <= 0) break;
    }

    delay(100);
  }
}



//render part of a 8-bit uncompressed BMP file
//no clipping
//uses line buffer to draw it much faster than through writePixel

void drawBMP8Part(int16_t x, int16_t y, const uint8_t bitmap[], int16_t dx, int16_t dy, int16_t w, int16_t h)
{
  int32_t i, j, bw, bh, wa, off, col, rgb;
  static uint16_t buf[128];

  bw = pgm_read_dword(&bitmap[0x12]);
  bh = pgm_read_dword(&bitmap[0x16]);
  wa = (bw + 3) & ~3;

  if (w >= h)
  {
    for (i = 0; i < h; ++i)
    {
      off = 54 + 256 * 4 + (bh - 1 - (i + dy)) * wa + dx;

      for (j = 0; j < w; ++j)
      {
        col = pgm_read_byte(&bitmap[off++]);
        rgb = pgm_read_dword(&bitmap[54 + col * 4]);
        buf[j] = ((rgb & 0xf8) >> 3) | ((rgb & 0xfc00) >> 5) | ((rgb & 0xf80000) >> 8);
      }

      tft.drawRGBBitmap(x, y + i, buf, w, 1);
    }
  }
  else
  {
    for (i = 0; i < w; ++i)
    {
      off = 54 + 256 * 4 + (bh - 1 - dy) * wa + i + dx;

      for (j = 0; j < h; ++j)
      {
        col = pgm_read_byte(&bitmap[off]);
        rgb = pgm_read_dword(&bitmap[54 + col * 4]);
        buf[j] = ((rgb & 0xf8) >> 3) | ((rgb & 0xfc00) >> 5) | ((rgb & 0xf80000) >> 8);
        off -= wa;
      }

      tft.drawRGBBitmap(x + i, y, buf, 1, h);
    }
  }
}



void drawCharFast(int x, int y, int c, int16_t color, int16_t bg)
{
  int i, j, line;
  static uint16_t buf[5 * 8];

  for (i = 0; i < 5; ++i)
  {
    line = pgm_read_byte(&font[c * 5 + i]);

    for (j = 0; j < 8; ++j)
    {
      buf[j * 5 + i] = (line & 1) ? color : bg;
      line >>= 1;
    }
  }

  tft.drawRGBBitmap(x, y, buf, 5, 8);
}



void printFast(int x, int y, char* str, int16_t color)
{
  char c;

  while (1)
  {
    c = *str++;

    if (!c) break;

    drawCharFast(x, y, c, color, 0);
    x += 6;
  }
}



bool espboy_logo_effect(int out)
{
  int i, j, w, h, sx, sy, off, st, anim;

  sx = 32;
  sy = 28;
  w = 64;
  h = 72;
  st = 8;

  for (anim = 0; anim < st; ++anim)
  {
    if (check_key()&PAD_ANY) return false;

    //if (!out) set_speaker(200 + anim * 50, 5);

    for (i = 0; i < w / st; ++i)
    {
      for (j = 0; j < st; ++j)
      {
        off = anim - (7 - j);

        if (out) off += 8;

        if (off < 0 || off >= st) off = 0; else off += i * st;

        drawBMP8Part(sx + i * st + j, sy, g_espboy, off, 0, 1, h);
      }
    }

    delay(1000 / 30);
  }

  return true;
}



void music_open(const char* filename)
{
  fs::File f = SPIFFS.open(filename, "r");

  if (!f) return;

  music_data_size = f.size();
  f.readBytes((char*)music_data, music_data_size);
  f.close();
}

void music_play()
{
  memset(&AYInfo, 0, sizeof(AYInfo));

  ay_init(&AYInfo.chip0);
  ay_init(&AYInfo.chip1);

  AYInfo.module = music_data;
  AYInfo.module_len = music_data_size;

  PT3_Init(AYInfo);

  sound_stereo_dac = 0;
  interruptCnt = 0;

  switch (output_device)
  {
    case OUT_SPEAKER:

      noInterrupts();
      sigmaDeltaSetup(0, F_CPU / 256);
      sigmaDeltaAttachPin(SOUNDPIN);
      sigmaDeltaEnable();
      timer1_attachInterrupt(sound_speaker_ISR);
      timer1_enable(TIM_DIV1, TIM_EDGE, TIM_LOOP);
      timer1_write(ESP.getCpuFreqMHz() * 1000000 / SAMPLE_RATE);
      interrupts();
      break;

    case OUT_I2S:
      i2s_begin();
      i2s_set_rate(SAMPLE_RATE);
      timer1_attachInterrupt(sound_i2s_ISR);
      timer1_enable(TIM_DIV1, TIM_EDGE, TIM_LOOP);
      timer1_write(ESP.getCpuFreqMHz() * 1000000 / SAMPLE_RATE);
      break;
  }
}

void music_stop()
{
  noInterrupts();
  timer1_disable();

  switch (output_device)
  {
    case OUT_SPEAKER: sigmaDeltaDisable(); break;
    case OUT_I2S: i2s_end(); break;
  }

  interrupts();
  delay(10);
}



void playing_screen(const char* filename)
{
  int i, h, sx, sy, off, frame;
  char str[21];

  for (i = 0; i < SPEC_BANDS; ++i)
  {
    spec_levels[i] = 0;
    spec_levels_prev[i] = -1;
    spec_colors[i] = ST77XX_BLACK;
  }

  music_open(filename);

  tft.fillScreen(ST77XX_BLACK);

  printFast(4, 16, "Now playing...", ST77XX_YELLOW);
  tft.fillRect(0, 24, 128, 1, ST77XX_WHITE);

  memset(str, 0, sizeof(str));
  memcpy(str, &music_data[0x1e], 20);
  printFast(4, 26, str, ST77XX_WHITE);
  memcpy(str, &music_data[0x3f], 20);
  printFast(4, 34, str, ST77XX_WHITE);

  sx = SPEC_SX;

  for (i = 0; i < SPEC_BANDS; ++i)
  {
    tft.fillRect(sx, SPEC_SY + SPEC_HEIGHT + 1, SPEC_BAND_WIDTH - 1, 1, ST77XX_WHITE);
    sx += SPEC_BAND_WIDTH;
  }

  music_play();

  frame = 0;

  while (music_data)
  {
    sx = SPEC_SX;
    sy = SPEC_SY;

    for (i = 0; i < SPEC_BANDS; ++i)
    {
      h = spec_levels[i];

      if (spec_levels_prev[i] != h)
      {
        spec_levels_prev[i] = h;

        if (h > SPEC_HEIGHT) h = SPEC_HEIGHT;

        tft.fillRect(sx, sy, 5, SPEC_HEIGHT - h, ST77XX_BLACK);
        tft.fillRect(sx, sy + SPEC_HEIGHT - h, SPEC_BAND_WIDTH - 1, h, spec_colors[i]);
      }

      sx += SPEC_BAND_WIDTH;
    }

    check_key();

    if (pad_state_t) break;

    delay(1);
  }

  music_stop();
}


#define FILE_HEIGHT		14
#define FILE_FILTER   "pt3"

int file_cursor;

bool file_browser_ext(const char* name)
{
  while (1) if (*name++ == '.') break;

  return (strcasecmp(name, FILE_FILTER) == 0) ? true : false;
}



void file_browser(String path, const char* header, char* filename, int filename_len)
{
  int i, j, sy, pos, frame, file_count;
  bool change, filter;
  fs::Dir dir;
  fs::File entry;
  char name[19 + 1];
  const char* str;

  memset(filename, 0, filename_len);
  memset(name, 0, sizeof(name));

  tft.fillScreen(ST77XX_BLACK);

  dir = SPIFFS.openDir(path);

  file_count = 0;

  while (dir.next())
  {
    entry = dir.openFile("r");

    filter = file_browser_ext(entry.name());

    entry.close();

    if (filter) ++file_count;
  }

  if (!file_count)
  {
    printFast(24, 60, "No files found", ST77XX_RED);

    while (1) delay(1000);
  }

  printFast(4, 4, (char*)header, ST77XX_GREEN);
  tft.fillRect(0, 12, 128, 1, ST77XX_WHITE);

  change = true;
  frame = 0;

  while (1)
  {
    if (change)
    {
      pos = file_cursor - FILE_HEIGHT / 2;

      if (pos > file_count - FILE_HEIGHT) pos = file_count - FILE_HEIGHT;
      if (pos < 0) pos = 0;

      dir = SPIFFS.openDir(path);
      i = pos;
      while (dir.next())
      {
        entry = dir.openFile("r");

        filter = file_browser_ext(entry.name());

        entry.close();

        if (!filter) continue;

        --i;
        if (i <= 0) break;
      }

      sy = 14;
      i = 0;

      while (1)
      {
        entry = dir.openFile("r");

        filter = file_browser_ext(entry.name());

        if (filter)
        {
          str = entry.name() + 1;

          for (j = 0; j < sizeof(name) - 1; ++j)
          {
            if (*str != 0 && *str != '.') name[j] = *str++; else name[j] = ' ';
          }

          printFast(8, sy, name, ST77XX_WHITE);

          drawCharFast(2, sy, ' ', ST77XX_WHITE, ST77XX_BLACK);

          if (pos == file_cursor)
          {
            strncpy(filename, entry.name(), filename_len);

            if (frame & 32) drawCharFast(2, sy, 0xdb, ST77XX_WHITE, ST77XX_BLACK);
          }
        }

        entry.close();

        if (!dir.next()) break;

        if (filter)
        {
          sy += 8;
          ++pos;
          ++i;
          if (i >= FILE_HEIGHT) break;
        }
      }

      change = false;
    }

    check_key();

    if (pad_state_t & PAD_UP)
    {
      --file_cursor;

      if (file_cursor < 0) file_cursor = file_count - 1;

      change = true;
      frame = 32;

    }

    if (pad_state_t & PAD_DOWN)
    {
      ++file_cursor;

      if (file_cursor >= file_count) file_cursor = 0;

      change = true;
      frame = 32;
    }

    if (pad_state_t & (PAD_A | PAD_B)) break;

    delay(1);

    ++frame;

    if (!(frame & 31)) change = true;
  }
}



void setup()
{
  //serial init

  Serial.begin(115200);

  //disable wifi to save some battery power

  WiFi.mode(WIFI_OFF);
  WiFi.forceSleepBegin();

  //DAC init, LCD backlit off

  dac.begin(MCP4725address);
  delay(100);
  dac.setVoltage(0, false);

  //mcp23017 and buttons init, should preceed the TFT init

  mcp.begin(MCP23017address);
  delay(100);

  for (int i = 0; i < 8; i++)
  {
    mcp.pinMode(i, INPUT);
    mcp.pullUp(i, HIGH);
  }

  pad_state = 0;
  pad_state_prev = 0;
  pad_state_t = 0;

  //TFT init

  mcp.pinMode(csTFTMCP23017pin, OUTPUT);
  mcp.digitalWrite(csTFTMCP23017pin, LOW);
  tft.initR(INITR_144GREENTAB);
  delay(100);
  tft.setRotation(0);
  tft.fillScreen(ST77XX_BLACK);

  dac.setVoltage(4095, true);

  //filesystem init

  SPIFFS.begin();

  delay(300);

  output_device = OUTPUT_DEVICE;
}



void loop()
{
  char filename[64];

  file_cursor = 0;

  //logo (skippable)

  if (espboy_logo_effect(0))
  {
    wait_any_key(1000);
    espboy_logo_effect(1);
  }

  //main loop

  while (1)
  {
    file_browser("/", "Select PT3 file:", filename, sizeof(filename));
    playing_screen(filename);
  }
}
