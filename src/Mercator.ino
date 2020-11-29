// Mercator.ino
// Matt Welsh - mdw@mdw.la
// https://www.mdw.la/
//
//
// This is Arduino code for Mercator, a spherical persistence-of-vision display
// powered by an ESP32 dev board and Adafruit Dotstar LED strip. For more details,
// see: https://github.com/mdwelsh/mercator

#include <Adafruit_DotStar.h>
#include <SPI.h>

// Include each of the images. These are generated using genheader.py.
#include "images/earth.h"
#include "images/pacman.h"
#include "images/spaceinvaders.h"
#include "images/jackolantern.h"
#include "images/tselogo.h"
#include "images/octoml.h"
#include "images/octoml_name.h"
#include "images/tabbyslime.h"
#include "images/hollowknight.h"
#include "images/deathstar.h"
#include "images/deathstar3.h"

// Image data for Game of Life.
#define IMAGE_ROWS_life 36
#define IMAGE_COLUMNS_life 72
static uint8_t IMAGE_life[IMAGE_COLUMNS_life * IMAGE_ROWS_life * 3];

typedef struct _image_metadata {
  uint8_t* data;
  int columns;
  int rows;
} image_metadata;

#define MAKEIMAGE(_name, _columns, _rows) (image_metadata){ data: (uint8_t*)&_name[0], columns: _columns, rows: _rows, }

#define NUM_IMAGES 12
image_metadata images[NUM_IMAGES] = {
  MAKEIMAGE(IMAGE_earth, IMAGE_COLUMNS_earth, IMAGE_ROWS_earth),
  MAKEIMAGE(IMAGE_pacman, IMAGE_COLUMNS_pacman, IMAGE_ROWS_pacman),
  MAKEIMAGE(IMAGE_spaceinvaders, IMAGE_COLUMNS_spaceinvaders, IMAGE_ROWS_spaceinvaders),
  MAKEIMAGE(IMAGE_jackolantern, IMAGE_COLUMNS_jackolantern, IMAGE_ROWS_jackolantern),
  MAKEIMAGE(IMAGE_tselogo, IMAGE_COLUMNS_tselogo, IMAGE_ROWS_tselogo),
  MAKEIMAGE(IMAGE_octoml, IMAGE_COLUMNS_octoml, IMAGE_ROWS_octoml),
  MAKEIMAGE(IMAGE_octoml_name, IMAGE_COLUMNS_octoml_name, IMAGE_ROWS_octoml_name),
  MAKEIMAGE(IMAGE_tabby, IMAGE_COLUMNS_tabby, IMAGE_ROWS_tabby),
  MAKEIMAGE(IMAGE_hollowknight, IMAGE_COLUMNS_hollowknight, IMAGE_ROWS_hollowknight),
  MAKEIMAGE(IMAGE_deathstar, IMAGE_COLUMNS_deathstar, IMAGE_ROWS_deathstar),
  MAKEIMAGE(IMAGE_deathstar3, IMAGE_COLUMNS_deathstar3, IMAGE_ROWS_deathstar3),
  MAKEIMAGE(IMAGE_life, IMAGE_COLUMNS_life, IMAGE_ROWS_life),
};

#define LIFE_IMAGE_INDEX NUM_IMAGES-1
// Binary count data for each GOL cell.
static uint8_t life1[IMAGE_COLUMNS_life * IMAGE_ROWS_life];
static uint8_t life2[IMAGE_COLUMNS_life * IMAGE_ROWS_life];
uint8_t *cur_life = life1;

// Number of LEDs in the strip.
#define NUMPIXELS 72
// Data pin for the DotStar strip.
#define DATAPIN    14
// Clock pin for the DotStar strip.
#define CLOCKPIN   32
Adafruit_DotStar strip(NUMPIXELS, DATAPIN, CLOCKPIN, DOTSTAR_BGR);

// If defined, will flash red when Hall effect sensor triggers.
//#define HALLDEBUG
// Input pin for the Hall effect sensor.
#define HALLPIN  15
// Output pin for the motor PWM signal.
#define PWMPIN A0
// Input pin for the speed potentiometer.
#define SPEEDPOT A1
// Input pin for the brightness potentiometer.
#define BRTPOT A2
// Input pin for the phase potentiometer.
#define SHIFTPOT A3
// Input pin for the button potentiometer.
#define BUTTONPIN 33

// Configuration for PWM output.
#define LEDC_CHANNEL_0 0
#define LEDC_TIMER_13_BIT 13
#define LEDC_BASE_FREQ 10000

// Initial LED strip brightness value.
#define BRIGHTNESS 20
// Number of columns displayed in a single revolution.
#define NUM_COLUMNS 72

// Time in microsecoonds after which a failed Hall effect detection turns off the LEDs.
#define IDLE_TIME 1000000
// Alpha parameter for EWMA calculation of revolution time.
#define ALPHA 0.5
// If defined, only show pattern on the front LED strip (rather than on both front and back).
//#define FRONT_STRIP_ONLY
// If defined, flash red when the motor is spinning too fast to display all columns in one revolution.
#define WARN_IF_TOO_FAST
// Static offset for shifting pixel display in X and Y dimensions.
#define X_SHIFT 0
#define Y_SHIFT 0
// Y-offset in pixels for back strip versus front strip.
#define BACK_OFFSET 3
// Time in microseconds to trigger next longitudinal shift in the image.
#define ROTATE_INTERVAL 10000
// Time in microseconds to step Game of Life simulation.
#define LIFE_INTERVAL 200000

int started = 0;
int cur_hall = 0;
int cur_step = 0;
int cur_x_offset = 0;
uint32_t last_hall_time = 0;
uint32_t last_rotate_time = 0;
uint32_t last_life_time = 0;
uint32_t last_clock_time = 0;
uint32_t last_button_time = 0;
uint32_t cycle_time = 0;
uint32_t per_column_time = 0;
uint32_t next_column_time = 0;
uint32_t start_button_pressed = 0;

int cur_image_index = 0;
image_metadata *cur_image = &images[cur_image_index];

// Set all pixels to the given color.
void setAll(uint32_t color) {
  for (int i = 0; i < NUMPIXELS; i++) {
    strip.setPixelColor(i, color);
  }
  strip.show();
}

// Interpolate between color1 and color2. mix represents the amount of color2.
uint32_t interpolate(uint32_t color1, uint32_t color2, float mix) {
  uint32_t r1 = (color1 & 0xff0000) >> 16;
  uint32_t r2 = (color2 & 0xff0000) >> 16;
  uint32_t r3 = (uint32_t)(r1 * (1.0-mix)) + (uint32_t)(r2 * mix) << 16;

  uint32_t g1 = (color1 & 0x00ff00) >> 8;
  uint32_t g2 = (color2 & 0x00ff00) >> 8;
  uint32_t g3 = (uint32_t)(g1 * (1.0-mix)) + (uint32_t)(g2 * mix) << 8;

  uint32_t b1 = (color1 & 0x0000ff);
  uint32_t b2 = (color2 & 0x0000ff);
  uint32_t b3 = (uint32_t)(b1 * (1.0-mix)) + (uint32_t)(b2 * mix);

  return r3 | g3 | b3;
}

// Input a value 0 to 255 to get a color value.
// The colours are a transition r - g - b - back to r.
uint32_t color_wheel(byte WheelPos) {
  WheelPos = 255 - WheelPos;
  if(WheelPos < 85) {
    return strip.Color(255 - WheelPos * 3, 0, WheelPos * 3);
  }
  if(WheelPos < 170) {
    WheelPos -= 85;
    return strip.Color(0, WheelPos * 3, 255 - WheelPos * 3);
  }
  WheelPos -= 170;
  return strip.Color(WheelPos * 3, 255 - WheelPos * 3, 0);
}

// Arduino like analogWrite.value must be between 0 and valueMax.
void ledcAnalogWrite(uint8_t channel, uint32_t value, uint32_t valueMax = 255) {
  // calculate duty, 8191 from 2 ^ 13 - 1
  uint32_t duty = (8191 / valueMax) * min(value, valueMax);

  // write duty to LEDC
  ledcWrite(channel, duty);
}

// Get the pixel value for the Life image data at the provided (x, y) position as a uint8_t.
uint8_t getPixel(uint8_t *image, int x, int y) {
  int xi = x % IMAGE_COLUMNS_life;
  int yi = y % IMAGE_ROWS_life;
  int index = (yi * IMAGE_COLUMNS_life) + xi;
  return image[index];
}

// Set the pixel value for the Life image data at the provided (x, y) position.
void setPixel(uint8_t *image, int x, int y, uint8_t value) {
  int xi = x % IMAGE_COLUMNS_life;
  int yi = y % IMAGE_ROWS_life;
  int index = (yi * IMAGE_COLUMNS_life) + xi;
  image[index] = value;
}

// Value for newly-live cells.
#define LIVE 0xff

// Initialize Game of Life data.
void init_life() {
  for (int y = 0; y < IMAGE_ROWS_life; y++) {
    for (int x = 0; x < IMAGE_COLUMNS_life; x++) {
      if (random(0, 5) < 1) {
        setPixel(cur_life, x, y, LIVE);
      } else {
        setPixel(cur_life, x, y, 0);
      }
    }
  }
}

// Return the new value of the given Life pixel.
uint8_t life(uint8_t *image, int x, int y) {
  int neighborCount = 0;
  if (getPixel(image, x-1, y-1) > 0) neighborCount++;
  if (getPixel(image, x-1, y) > 0) neighborCount++;
  if (getPixel(image, x-1, y+1) > 0) neighborCount++;
  if (getPixel(image, x, y-1) > 0) neighborCount++;
  if (getPixel(image, x, y+1) > 0) neighborCount++;
  if (getPixel(image, x+1, y-1) > 0) neighborCount++;
  if (getPixel(image, x+1, y) > 0) neighborCount++;
  if (getPixel(image, x+1, y+1) > 0) neighborCount++;
  uint8_t self = getPixel(image, x, y);
  if (self > 0 && (neighborCount == 2 || neighborCount == 3)) {
    if (self == 1) return 1;
    else return self-1;  // Decay cells over time.
  }
  if (self == 0 && neighborCount == 3) return LIVE;
  return 0;
}

// Evolve the Game of Life data by one step.
void step_life() {
  uint8_t *from = cur_life;
  uint8_t *to = (cur_life == life1) ? life2 : life1;
  for (int y = 0; y < IMAGE_ROWS_life; y++) {
    for (int x = 0; x < IMAGE_COLUMNS_life; x++) {
      uint8_t val = life(from, x, y);
      setPixel(to, x, y, val);
      uint32_t color = val == 0 ? 0 : color_wheel(val);
      IMAGE_life[((y * IMAGE_COLUMNS_life) + x) * 3] = (color >> 16) & 0xff;
      IMAGE_life[(((y * IMAGE_COLUMNS_life) + x) * 3) + 1] = (color >> 8) & 0xff;
      IMAGE_life[(((y * IMAGE_COLUMNS_life) + x) * 3) + 2] = color & 0xff;
    }
  }
  // Swap from and to images.
  cur_life = to;
}

void setup() {
  Serial.begin(115200);
  Serial.println("Initializing...");
  
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(HALLPIN, INPUT_PULLUP);
  pinMode(PWMPIN, OUTPUT);
  pinMode(SPEEDPOT, INPUT);
  pinMode(SHIFTPOT, INPUT);
  pinMode(BRTPOT, INPUT);
  pinMode(BUTTONPIN, INPUT_PULLUP);
  ledcSetup(LEDC_CHANNEL_0, LEDC_BASE_FREQ, LEDC_TIMER_13_BIT);
  ledcAttachPin(PWMPIN, LEDC_CHANNEL_0);
  ledcAnalogWrite(LEDC_CHANNEL_0, 0);

  last_hall_time = micros();
  next_column_time = micros();

  strip.begin();
  strip.show();
  strip.setBrightness(BRIGHTNESS);
  setAll(0xff0000);
  delay(200);
  setAll(0x00ff00);
  delay(200);
  setAll(0x0000ff);
  delay(200);
}

uint32_t get_color(int x, int y) {
  if (x < 0 || x > cur_image->columns-1) return 0x0;
  if (y < 0 || y > cur_image->rows-1) return 0x0;
  int index = (y * cur_image->columns * 3) + (x * 3);
  uint8_t* p = (uint8_t*)&cur_image->data[index];
  uint32_t* pv = (uint32_t*)p;
  uint32_t pixel = (*pv) & 0xffffff;
  return pixel;
}

void doPaint(uint32_t cur_time) {
  if (cur_time < next_column_time) {
    return;
  }
  
  int x = (cur_step - X_SHIFT + cur_x_offset) % NUM_COLUMNS;
  int frontx = x;
  int backx = (x + (NUM_COLUMNS/2)) % NUM_COLUMNS;

  // Front strip.
  for (int i = 0; i < NUMPIXELS/2; i++) {
    int y = (NUMPIXELS/2) - i;   // Swap y-axis.
    strip.setPixelColor(i, get_color(frontx, y));
  }

  // Back strip.
  for (int i = NUMPIXELS/2; i < NUMPIXELS; i++) {
    int y = i - (NUMPIXELS/2) + BACK_OFFSET;
#ifdef FRONT_STRIP_ONLY
    strip.setPixelColor(i, 0x0);
#else
    strip.setPixelColor(i, get_color(backx, y));
#endif
  }

  strip.show();
  cur_step++;
  next_column_time = cur_time + per_column_time;
}

// White pulsing while waiting to start.
#define PULSE_LOW 0x0
#define PULSE_HIGH 0x404040
#define PULSE_STEP 0.0015
float cur_pulse = 0.0;
float pulse_step;
void pulse() {
  if (cur_pulse <= 0.0) {
    pulse_step = PULSE_STEP;
  } else if (cur_pulse >= 1.0) {
    pulse_step = -PULSE_STEP;
  }
  cur_pulse += pulse_step;
  setAll(interpolate(PULSE_LOW, PULSE_HIGH, cur_pulse));
}

void loop() {
  uint32_t cur_time = micros();

  int btn = digitalRead(BUTTONPIN);
  if (btn == LOW) {
    digitalWrite(LED_BUILTIN, HIGH);
  } else {
    digitalWrite(LED_BUILTIN, LOW);
  }

#define SAFE_START
#ifdef SAFE_START
  if (!started) {
    if (btn == HIGH) {
      start_button_pressed = 0;
    } else if (start_button_pressed == 0 && btn == LOW) {
      start_button_pressed = cur_time;
    } else if (start_button_pressed > 0 && cur_time - start_button_pressed > 1000000 && btn == LOW) {
      setAll(0x00ff00);
      delay(250);
      started = 1;
      return;
    }
    pulse();
    return;
  }
#endif  // SAFE_START
  
  int speedPotVal = analogRead(SPEEDPOT);
  ledcAnalogWrite(LEDC_CHANNEL_0, speedPotVal >> 5);
  
  int shiftPotVal = analogRead(SHIFTPOT);
  int brtPotVal = analogRead(BRTPOT);
  strip.setBrightness(brtPotVal >> 6);

  if (btn == HIGH) {
    start_button_pressed = 0;
  } else if (start_button_pressed == 0 && btn == LOW) {
    start_button_pressed = cur_time;
  } else if (start_button_pressed > 0 && cur_time - start_button_pressed > 250000 && btn == LOW) {
    cur_image_index++;
    cur_image_index %= NUM_IMAGES;
    cur_image = &images[cur_image_index];
    if (cur_image_index == LIFE_IMAGE_INDEX) {
      init_life();
    }
    start_button_pressed = 0;
  }

  int hall = digitalRead(HALLPIN);

  if (hall == LOW && cur_hall == 0) {
    if (cur_time - last_hall_time > 10000) {  // Debounce.
#ifdef HALLDEBUG
    setAll(0xff0000);
#endif      
      cur_hall = 1;

#ifdef WARN_IF_TOO_FAST
      if (cur_step < NUM_COLUMNS-2 && cur_image_index != LIFE_IMAGE_INDEX) {
        setAll(0xff0000);
      }
#endif
      cur_step = 0; // Reset cycle.
      cycle_time = (ALPHA * cycle_time) + ((1.0 - ALPHA) * (cur_time - last_hall_time));
      per_column_time = (cycle_time / NUM_COLUMNS);
      next_column_time = cur_time - per_column_time;
      last_hall_time = cur_time;

#ifdef ROTATE_INTERVAL
      if (cur_time - last_rotate_time > ROTATE_INTERVAL) {
        if (shiftPotVal > 512) {
          cur_x_offset += shiftPotVal >> 8;
          cur_x_offset %= NUM_COLUMNS;
        }
        last_rotate_time = cur_time;
      }
#else
      cur_x_offset = shiftPotVal >> 5;
#endif

      if (cur_image_index == LIFE_IMAGE_INDEX && cur_time - last_life_time > LIFE_INTERVAL) {
        step_life();
        last_life_time = cur_time;
      }
    }
  } else if (hall == HIGH && cur_hall == 1) {
    cur_hall = 0;
#ifdef HALLDEBUG
    setAll(0x0);
#endif
  }
#ifndef HALLDEBUG
  if (cur_time - last_hall_time < IDLE_TIME) {
    doPaint(cur_time);
  } else {
    setAll(0x0);
  }
#endif

}
