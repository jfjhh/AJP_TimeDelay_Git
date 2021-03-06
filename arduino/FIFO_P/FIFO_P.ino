/*******************************************************************************
 * Time Delay Device Arduino Control
 * Edgar Perez and Alex Striff
 * Reed College Physics Department
 *
 * For more information, contact Lucas Illing <illing@reed.edu>.
 *
 * Version  Date        Author       Changes
 * -------  ----------  -----------  -------------------------------------------
 * v1.0     2016-12-15  Edgar Perez  Prototype completed.
 * v2.0     2019-07-XX  Alex Striff  Modified for publication board.
 *
 ******************************************************************************/

// Code options (feature selection)
#define SCREEN   // Uncomment to enable screen output
#define SCONTROL // Uncomment for serial control and logging
#define HISTORY  // Uncomment for FIFO initial state programming (req: SCONTROL)
#define DEBUG    // Uncomment for serial debug output

#if defined(HISTORY) && defined(SCONTROL)
#define HIST
#include <CRC32.h>
#endif

#ifdef SCREEN
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
// #include <Fonts/FreeSans9pt7b.h>
#include "lato24.h"

#define DIGIT_FONT  &Lato_Regular_24 // 24 px high
#define TEXT_FONT   NULL  // 8 px high, NULL for default font in Adafruit GFX

extern TwoWire Wire1; // Use second I2C pins on the Due

#define SCREEN_WIDTH 128 // OLED display width, in pixels
#define SCREEN_HEIGHT 32 // OLED display height, in pixels
#define OLED_RESET    A0 // Reset pin (or -1 if sharing Arduino reset pin)
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire1, OLED_RESET);
boolean screen = true;
boolean draw   = true;
char s_digits[16];

enum menu_mode {
    MENU = 0, // Main menu
    DELAY,    // Adjust the word count using the rotary encoder
    FIFO,     // Show FIFO status
    MENU_MODES,
} menu_mode = DELAY;

const PROGMEM char menu_names[][16] = {
    "Menu",
    "Delay",
    "FIFO",
};

uint32_t menu_pos = MENU + 1;
int menu_press = 0;

// Double-press parameters
int dpress = 0;
long dpress_delay = 100L;
long dpress_time = 0L;

// Repeated drawing parameters (in ms)
long last_update = 0L;
long update_interval = 100L; 

#endif // SCREEN

typedef const uint8_t pin;

typedef struct {
    pin p;                      // The pin that the button is connected to.
    long debounce_delay;        // The delay (ms) for debouncing a button press.
    int state;                  // The current state of the button.
    int last_state;             // The last state of the button.
    long last_debounce_time;    // The last debounce time for the button.
} button;

typedef struct {
    pin a;          // Output A
    pin b;          // Output B
    button btn;     // Button (knob of encoder)
} rotary_encoder;

// FIFO word limits
// min_words = 1 or 2 give the same delay.
// min_words = 3 is off-by-one depending upon the clock.
// FIFO words:  0        1  2  3     4   5  ...
// Delay words: invalid  8  8  9,10  11  12 ...
const uint32_t min_words  = 4; // FIFO flags
const uint32_t max_words  = 262144; // FIFO flags
const uint32_t off_words  = 3 + 2 + 2; // ADC + DAC + FIFO.
const uint32_t init_words = 100; // FIFO + offset (user-visible)

#ifdef HIST
// History programming buffer
#define HBUF_SIZE   16 // Make hist relatively large, given SRAM limitations
byte hist[HBUF_SIZE];  // Holds (HBUF_SIZE / 2) code words (uint16_t)
#endif // HIST

// Rotary encoder pin mapping
rotary_encoder enc = {
    .a   = 48, // ENC_A pin
    .b   = 50, // ENC_B pin
    .btn = {
        .p = 52, // ENC_BTN pin
        .debounce_delay = 50,
        .state = 0,
        .last_state = 0,
        .last_debounce_time = 0
    }
};

uint32_t words; // The number of delay words for the FIFO
uint32_t words_digit = 0;

int32_t  count_min = min_words;
int32_t  count_max = max_words;
int32_t  count_mul = 1000;

// FIFO pin mappings:
// Active LOW unless stated otherwise
pin RT    = 42;
pin OE    = 45;
pin REN   = 43;
pin OR    = 40;
pin PAE   = 41;
pin HF    = 38;
pin PAF   = 39;
pin FWFT  = 37; // FWFT/SI. Active HIGH
pin IR    = 36;
pin LD    = 34;
pin MRS   = 35;
pin PRS   = 32;
pin WEN   = 31;
pin SEN   = 30;

// Clock control pin mappings:
// Active HIGH unless stated otherwise
pin nPROG  = 22;
pin CLK1   = 26; // 1CLK (single-clock operation)
pin WCLK_S = 24;
pin ADC_CLK_S = 28;
pin PROG_D = DAC1;
pin STRIG  = 33;

// FIFO flags to show in status. All active low.
enum fifo_flag {
    F_IR = 0, // Input ready
    F_OR,     // Output ready
    F_PAF,    // Partially almost full
    F_PAE,    // Partially almost empty
    F_HF,     // Half full
    F_SIZE,   // The number of flags (not on FIFO)
} fifo_flags;

const pin fifo_flag_pins[] = {
    IR,
    OR,
    PAF,
    PAE,
    HF,
};

const PROGMEM char fifo_flag_names[][16] = {
    "IR",
    "OR",
    "PAF",
    "PAE",
    "HF",
};

// DAC pin mapping
pin DAC_LDAC = 46;
pin DAC_CS   = 44;

// Serial command buffer
#ifdef SCONTROL
#define SBLEN   16
char sb[SBLEN];
#endif // SCONTROL


void setup(void)
{
    // Serial communication
    Serial.begin(115200);
    Serial.print(F(
        "====================\n"
        " FIFO_P3 Time Delay \n"
        "====================\n"
        "    Reed College    \n"
        " Physics Department \n"
        "        2019        \n"   
        "====================\n"
        "    Alex Striff     \n"
        "    Edgar Perez     \n"
        "    Lucas Illing    \n"
        "====================\n"
        "Enabled features: "
        #ifdef DEBUG
        "DEBUG "
        #endif // DEBUG
        #ifdef SCREEN
        "SCREEN "
        #endif // SCREEN
        #ifdef SCONTROL
        "SCONTROL "
        #endif // SCONTROL
        #ifdef HIST
        "HIST "
        #endif // HIST
        "\n"
        "Type 'h' for help.\n\n"
        "Setting up I/O ... "
        ));

    // DAC initialization
    delay(10);
    digitalWrite(nPROG, LOW); // x. Check not flipped
    digitalWrite(DAC_LDAC, HIGH);
    delay(5);
    digitalWrite(nPROG, HIGH); // x.
    digitalWrite(DAC_LDAC, LOW);

    // Due (self) DAC initialization
    analogWriteResolution(12);
    pinMode(PROG_D, OUTPUT);
    analogWrite(PROG_D, 1u << (12 - 2)); // Half full scale

    // Clock control pins
    pinMode(nPROG,     OUTPUT);
    pinMode(CLK1,      OUTPUT);
    pinMode(WCLK_S,    OUTPUT);
    pinMode(ADC_CLK_S, OUTPUT);
    pinMode(STRIG,     OUTPUT);

    // Clock default state (single clock, not programming)
    digitalWrite(CLK1,      HIGH);
    digitalWrite(WCLK_S,    LOW);
    digitalWrite(ADC_CLK_S, LOW);
    digitalWrite(STRIG,     LOW);

    digitalWrite(nPROG, HIGH); // Disable operation while setting up.

    // FIFO pins
    pinMode(REN,  OUTPUT);
    pinMode(OR,   INPUT_PULLUP);
    pinMode(PAE,  INPUT_PULLUP);
    pinMode(HF,   INPUT_PULLUP);
    pinMode(PAF,  INPUT_PULLUP);
    pinMode(IR,   INPUT_PULLUP);
    pinMode(OE,   OUTPUT);
    pinMode(RT,   OUTPUT);
    pinMode(FWFT, OUTPUT);
    pinMode(LD,   OUTPUT);
    pinMode(MRS,  OUTPUT);
    pinMode(PRS,  OUTPUT);
    pinMode(WEN,  OUTPUT);
    pinMode(SEN,  OUTPUT);

    // FIFO default states
    digitalWrite(MRS,  HIGH);
    digitalWrite(PRS,  HIGH);
    digitalWrite(RT,   HIGH);
    //digitalWrite(FWFT, HIGH);
    digitalWrite(FWFT, LOW); // Let's try IDT mode
    digitalWrite(LD,   HIGH);
    digitalWrite(WEN,  HIGH);
    digitalWrite(REN,  HIGH);
    digitalWrite(SEN,  HIGH);
    digitalWrite(OE,   LOW);

    // DAC pins
    delay(100);
    pinMode(nPROG,    OUTPUT);
    pinMode(DAC_LDAC, OUTPUT);
    pinMode(DAC_CS,   OUTPUT);

    // More DAC initialization
    delay(10);
    pinMode(DAC_CS,   OUTPUT);
    digitalWrite(DAC_CS, HIGH);
    delay(5);
    digitalWrite(DAC_CS, LOW);
    delay(5);
    digitalWrite(DAC_CS, HIGH);
    delay(5);
    digitalWrite(DAC_CS, LOW);
    delay(10);

    // Rotary encoder pins
    pinMode(enc.btn.p, INPUT_PULLUP);
    pinMode(enc.a,     INPUT_PULLUP);
    pinMode(enc.b,     INPUT_PULLUP);
    read_encoder(&enc); // Read to initialize previous quadrature state.
    
    // Screen initialization
    #ifdef SCREEN
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { // Address 0x3C for 128x32
        Serial.println(F("SSD1306 allocation failed. Unable to use screen."));
        screen = false;
    } else {
        screen = true;
        display.clearDisplay();
        display.cp437(true);
        display.setTextColor(WHITE);
        display.setTextSize(1);
        display.setCursor(0, 0);
        display.print(F("Time Delay Device vP3"));
        display.setCursor(0, 8);
        display.print(F("Reed College Physics"));
        display.setCursor(0, 24);
        display.print(F("2-press knob for menu"));
        display.display();

        for (uint8_t i = 0; i < SCREEN_WIDTH; i++) {
            display.drawPixel(i, 16 + (i*i / 7) % 8, WHITE);
            display.display();
        }
    }
    #endif // SCREEN

    // Do a master reset to initialize the FIFO
    Serial.print(F(" complete. Initializing FIFO:\n\t"));
    delay(5);
    master_reset(false);
    Serial.println();
    set_delay(init_words, true);
}


void loop(void)
{
    uint32_t narg;

    #ifdef SCONTROL
    for (int i = 0; i < SBLEN; i++)
        sb[i] = '\0';

    // TODO: Update help.
    if (Serial.available() > 0) {
        Serial.readBytesUntil(';', sb, SBLEN);
        
        narg = atol(sb + 2);
        switch (sb[0]) {
            case 'h': // Fallthrough.
            case 'H': // Fallthrough.
            case '?': serial_help(); break;
            case 'm':
                switch (sb[1]) {
                    case 'r': master_reset(true); break;
                    default:  unknown(); break;
                }
                break;
            case 'p':
                switch (sb[1]) {
                    case 'r': partial_reset(true); break;
                    #ifdef DEBUG
                    case 'p': prog_debug(); break;
                    case 'c': prog_clock_debug(); break;
                    #endif // DEBUG
                    #ifdef HIST
                    case 'h': prog_hist(narg); break;
                    #endif // HIST
                    default:  unknown(); break;
                }
                break;
            case 'd': set_delay(narg, true); break;
            #ifdef HIST
            case 'g':
                switch (sb[1]) {
                    case 'o': start(); break;
                    default:  unknown(); break;
                }
                break;
            #endif // HIST
            case 'c':
                switch (sb[1]) {
                    case '1': single_clock(); break;
                    case '2': dual_clock(); break;
                    default:  unknown(); break;
                }
                break;
            case 's': report_status(); break;
            case 'x': pause(narg); break;
            case 'i': initialize(); break;
            case 'q': quit(); break;
            default:  unknown(); break;
                
        }
    }
    #endif // SCONTROL

    #ifdef SCREEN
    if (screen) {
        switch (menu_mode) {
            case MENU:
                menu_pos = enc_adjust_nodigit(&menu_pos,
                        MENU + 1, MENU_MODES - 1, true);
                if (draw) {
                    display.clearDisplay();
                    display.setFont(TEXT_FONT);
                    for (size_t i = MENU + 1; i < MENU_MODES; i++) {
                        int16_t x = 8 + ((i - 1) / 4) * SCREEN_WIDTH;
                        int16_t y = ((i - 1) % 4) * 8;
                        display.setCursor(x, y);
                        display.print(menu_names[i]);
                    }

                    int16_t alt = ((menu_pos - 1) / 4) * SCREEN_WIDTH;
                    int16_t mid = 2 + ((menu_pos - 1) % 4) * 8;
                    display.fillTriangle(alt, mid - 2, alt, mid + 2, alt + 3,
                            mid, WHITE);
                    display.display();
                    draw = false;
                }
                
                if (read_button(&(enc.btn))) {
                    menu_press = 1;
                } else if (menu_press == 1) {
                    // button was pressed and now it is not
                    menu_mode = (enum menu_mode) menu_pos;
                    menu_press = 0;
                    draw = true;
                }
                
                break;
              
            case DELAY:
                { // Surrounding block to disambiguate scope
                    set_delay(enc_adjust(&words, &words_digit, count_min,
                                count_max, false, false), true);
                    int16_t  x, y;
                    uint16_t w, h;
                    int16_t digit_x = 0;
                    int16_t digit_y = 24;

                    draw |= millis() - last_update >= update_interval;
                    if (draw) {
                        display.clearDisplay();
                        sprintf(s_digits, "%06u", words + off_words);
                        display.setFont(DIGIT_FONT);
                        display.setCursor(digit_x, digit_y);
                        display.print(s_digits);
                        
                        int16_t mid = 6 + (6 - words_digit - 1) * 15;
                        int16_t base = 31;
                        display.fillTriangle(mid - 3, base, mid + 3, base, mid,
                                base - 4, WHITE);

                        display.setFont(TEXT_FONT);
                        display.getTextBounds(s_digits, digit_x, digit_y, &x,
                                &y, &w, &h);
                        display.setCursor(SCREEN_WIDTH - w + 6,
                                digit_y - 2*h + 1);
                        display.print(F("Delay"));
                        display.getTextBounds(s_digits, digit_x, digit_y,
                                &x, &y, &w, &h);
                        display.setCursor(SCREEN_WIDTH - w + 6,
                                digit_y - h + 1);
                        display.print(F("words"));
                        
                        display.display();
                        draw = false;
                    }
                }
                break;

            case FIFO:
                {
                    draw |= millis() - last_update >= update_interval;
                    if (draw) {
                        display.clearDisplay();
                        display.setFont(TEXT_FONT);
                        for (size_t i = 0; i < F_SIZE; i++) {
                            int16_t x = 8 + (i / 4)
                                * SCREEN_WIDTH / ((F_SIZE / 4) + 1);
                            int16_t y = (i % 4) * 8;
                            display.setCursor(x, y);
                            display.print(fifo_flag_names[i]);
                            display.setCursor(x + 4*8, y);
                            // Invert to show understandable logic
                            display.print(digitalRead(fifo_flag_pins[i])
                                    ? 0 : 1, DEC);
                        }
                        display.display();
                        draw = false;
                        last_update = millis();
                    }

                    press_menu_return(&enc);
                }
                break;

            default:
                Serial.println("Menu error!");
                break;
        }

        // Double-press detection
        if (menu_mode != MENU) {
            int press = read_button(&(enc.btn));
            int quick = millis() - dpress_time <= dpress_delay;
            if ((dpress == 0 && press)
                || (dpress == 1 && !press && quick)
                || (dpress == 2 && press && quick)) {
                dpress_time = millis();
                dpress++;
            } else if (dpress == 3 && !press && quick) {
                dpress = 0;
                menu_mode = menu_mode != MENU ? MENU : DELAY;
                draw = true;
            } else if (!quick) {
                dpress = 0;
            }
        }
    }
    #endif // SCREEN
}

#ifdef SCREEN
void press_menu_return(rotary_encoder *e)
{
    if (read_button(&(e->btn))) {
        menu_press = HIGH;
    } else if (menu_press == HIGH) { // Button was pressed and now it is not
        menu_press = LOW;
        menu_mode = MENU;
        menu_press = LOW;
        draw = true;
    }
}
#endif // SCREEN

#ifdef SCONTROL
void unknown(void)
{
    Serial.println(F("Unknown Command. Type 'h' for help."));
    serial_help();
}
#endif // SCONTROL


#ifdef SCONTROL
void serial_help(void)
{
    Serial.print(F(
    "Time Delay Device Serial Commands\n"
    "=================================\n"
    "mr\tPerforms a master reset of FIFO memory.\n"
    "pr\tPerforms a partial reset of FIFO memory.\n"
    "d N\tSets delay to N words, where N is a decimal number between "));
    Serial.print(min_words + off_words, DEC);
    Serial.print(F(" and "));
    Serial.print(max_words + off_words, DEC);
    Serial.print(F(
    ".\n"
    "\tE.g.: 'd 9' and 'd 101' produce delays of 9 and 101 words, respectively.\n"
    "\tValues outside the possible range will be clamped, so 'd 0' is the same as 'd "
    ));
    Serial.print(min_words + off_words, DEC);
    Serial.print(F(
    "'.\n"
    "\t<time delay> = <delay words> * <clock period>.\n"
    "ph N;<data>\tPrograms N words of initial history into the FIFO.\n"
    "\tAfter the semicolon, 2N bytes of big-endian data must follow.\n"
    "\tThe data are 12-bit unsigned code words representing analog signals,\n"
    "\twhere 0x0000 gives -2.5V and 0x0FFF gives +2.5V.\n"
    "s\tReports FIFO status.\n"
    "x N\tPauses for N seconds (neglecting serial delays; not precise).\n"
    "i\tInitializes the program.\n"
    "q\tQuits the program.\n"
    "h H ?\tShows this help.\n"
    "\n"
    ));
}
#endif // SCONTROL


void partial_reset(boolean start)
{
    long t1, t2;
    Serial.print(F("Partial Reset ... "));
    t1 = micros();
    digitalWrite(nPROG, LOW);
    delay(1);
    
    digitalWrite(REN, HIGH);
    digitalWrite(WEN, HIGH);
    digitalWrite(RT,  HIGH);
    delayMicroseconds(3);
    digitalWrite(PRS, LOW);
    delayMicroseconds(3);
    digitalWrite(PRS, HIGH);
    delayMicroseconds(3);
    digitalWrite(WEN, LOW);
    if (start) digitalWrite(nPROG, HIGH);
    
    t2 = micros() - t1;
    delayMicroseconds(1);
    Serial.print("done (");
    Serial.print(t2);
    Serial.println(" us).");
}


void master_reset(boolean start)
{
    long t1, t2;
    Serial.print(F("Master Reset ... "));
    t1 = micros();
    digitalWrite(nPROG, LOW);
    //digitalWrite(FWFT, HIGH);
    digitalWrite(FWFT, LOW); // Let's try IDT mode
    digitalWrite(LD,    HIGH);
    digitalWrite(MRS,   LOW);
    delayMicroseconds(1);
    digitalWrite(MRS, HIGH);
    if (start) digitalWrite(nPROG, HIGH);
    
    t2 = micros() - t1;
    delayMicroseconds(1);
    Serial.print(F("done ("));
    Serial.print(t2);
    Serial.println(F(" us)."));
    Serial.print(F("Device in FWFT with Serial Loading.\n"));
}


#ifdef SCONTROL
void report_status(void)
{
    Serial.print(F(
        "FIFO Status\n"
        "===========\n"));

    for (size_t i = 0; i < F_SIZE; i++) {
        Serial.print(F("~"));
        Serial.print(fifo_flag_names[i]);
        Serial.print(F("\t"));
        Serial.println(digitalRead(fifo_flag_pins[i]), DEC);
    }
    Serial.println();

    #ifdef DEBUG
    Serial.println(words_digit);
    Serial.println(words + off_words);
    Serial.println(ipow(3, 0));
    Serial.println(ipow(3, 1));
    Serial.println(ipow(3, 5));
    #endif // DEBUG
}
#endif // SCONTROL


#ifdef SCONTROL
void pause(uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        Serial.println(n - i);
        delay(1000);
    }
    Serial.println("0.");
}
#endif // SCONTROL


void set_delay(uint32_t n, boolean start)
{
    unsigned int m;
    int bits;
    
    n = clamp(n - off_words, min_words, max_words);
    if (n == words && start) return;
    words = n;
    m = max_words - n;
    
    Serial.print(F("Sending a "));
    Serial.print(n);
    Serial.print(F("-word delay ... "));
    digitalWrite(nPROG, 0);
    delayMicroseconds(10);

    digitalWrite(WCLK_S, 0);
    digitalWrite(LD, 0);
    digitalWrite(SEN, 0);
    delayMicroseconds(2);

    // The following number is defined because PAF triggers at the given
    // number of words AWAY FROM the end, not AT the given word like PAE.
    for (int i = 0; i < 36 ; i++) {
        bits = i == 0 ? 1 : bitRead(m, i - 18);
        digitalWrite(FWFT, bits);
        delayMicroseconds(1);
        digitalWrite(WCLK_S, 1);
        delayMicroseconds(1);
        digitalWrite(WCLK_S, 0);
    }

    digitalWrite(LD, 1);
    digitalWrite(SEN, 1);
    digitalWrite(nPROG, 1);
    Serial.print(F("done. Resetting: "));
    partial_reset(start);
}


void set_fifo_delay(uint32_t n, boolean start)
{
    set_delay(n + off_words, start);
}


void initialize(void)
{
    digitalWrite(WEN, 0);
    digitalWrite(REN, 0);
    Serial.println(F("Initiated program."));
}


void quit(void)
{
    digitalWrite(WEN, 1);
    digitalWrite(REN, 1);
    Serial.println(F("Quit program."));
}


int read_encoder(rotary_encoder *e)
{
    static int8_t enc_states[4][4] = {
        { 0, -1,  1,  0},
        { 1,  0,  0, -1},
        {-1,  0,  0,  1},
        { 0,  1, -1,  0}
    };
    static uint8_t old_AB = 0;
    int8_t direction;
    uint8_t AB = digitalRead(e->a) << 1 | digitalRead(e->b);

    direction = enc_states[old_AB][AB];
    old_AB = AB;

    return direction;
}


int32_t clamp(int32_t x, int32_t min, int32_t max)
{
    return x <= min ? min : x >= max ? max : x;
}


int ipow(int base, unsigned int exp)
{
    int result = 1;
    
    for (; exp; base *= base, exp >>= 1)
        if (exp & 1)
            result *= base;

    return result;
}


uint32_t enc_adjust(uint32_t *n, uint32_t *digit, uint32_t n_min,
        uint32_t n_max, boolean reverse, boolean dreverse)
{
    int     direction;
    int32_t count = *n;
    direction = read_encoder(&enc);

    if (direction) {
        if (read_button(&(enc.btn))) {
            *digit = clamp(*digit + direction * (dreverse ? -1 : 1), 0, 5);
            #ifdef DEBUG
            Serial.print(F("Button press: "));
            Serial.println(*digit);
            #endif // DEBUG
        } else {
            count = clamp(count + direction * (reverse ? -1 : 1)
                    * ipow(10, *digit), n_min, n_max);
            #ifdef DEBUG
            Serial.print(F("Count: "));
            Serial.println(count);
            #endif // DEBUG
        }
        #ifdef SCREEN
        draw = true;
        dpress = 0;
        #endif // SCREEN
    }

    return (uint32_t) count + off_words;
}


uint32_t enc_adjust_nodigit(uint32_t *n, uint32_t n_min, uint32_t n_max,
        boolean reverse)
{
    int     direction;
    int32_t count = *n;
    direction = read_encoder(&enc) * (reverse ? -1 : 1);

    if (direction) {
        count = clamp(count + direction, n_min, n_max);
        #ifdef DEBUG
        Serial.print("ND Count: ");
        Serial.println(count);
        #endif // DEBUG
        #ifdef SCREEN
        draw = true;
        dpress = 0;
        #endif // SCREEN
    }

    return (uint32_t) count;
}


int read_button(button *button)
{
    int reading = digitalRead(button->p);
    
    if (reading != button->last_state)
        button->last_debounce_time = millis();
    if (millis() - button->last_debounce_time > button->debounce_delay)
        button->state = reading;
    
    return button->last_state = reading;
}


#ifdef HIST
int prog_hist(uint32_t n)
{
    // Reads n code words (uint16_t values for DAC) and programs them into the
    // FIFO memory. Little-endian.
    uint32_t nwords = n;
    n *= 2; // Words to bytes.
    uint32_t bufs = 0u;
    uint32_t bytes = 0u;
    uint32_t col = 8;
    uint32_t cols = 2;
    uint32_t row = col * cols;
    char hex[16];
    byte msb, lsb, sbyte;
    uint16_t data;
    uint32_t read_bytes;
    uint32_t buf_remaining;
    uint32_t adc_off = 3u;
    uint32_t adc_ins = 0u;
    CRC32 crc;

    // Enter programming mode.
    digitalWrite(nPROG, LOW);
    digitalWrite(STRIG, LOW);
    Serial.print(F("Programming "));
    Serial.print(nwords, DEC);
    Serial.print(F(" words of history...\n\n"));
    master_reset(false);
    //set_fifo_delay(nwords, false);
    set_delay(nwords, false);

    // Program the data one hist[] buffer at a time.
    while (bytes < n) {
        while (!Serial.available());
        
        buf_remaining = min(HBUF_SIZE, n - bytes);
        read_bytes = Serial.readBytes(hist, buf_remaining);
        bytes += read_bytes;
        boolean last = read_bytes < HBUF_SIZE;
        
        if (read_bytes != buf_remaining) {
            Serial.print(F("Error reading history data! (Received "));
            Serial.print(bytes, DEC);
            Serial.print(F(" bytes, but expected "));
            Serial.print(n, DEC);
            Serial.println(F(" bytes.)"));
            return 0;
        }

        digitalWrite(ADC_CLK_S, LOW);
        digitalWrite(WCLK_S, LOW);
        for (uint32_t j = 0; j < (read_bytes / row) + (last ? 1 : 0); j++) {
            uint32_t end_bytes = last ? read_bytes % row : row;

            // The actual programming
            for (uint32_t k = 0; k < end_bytes; k++) {
                // This may be decreased, depending upon signal speed
                delayMicroseconds(100);
                sbyte = hist[row*j + k];
                crc.update(sbyte);
                if (k % 2 == 0) {
                    lsb = sbyte;
                } else {
                    msb = sbyte;
                    // Programming circuitry inverts, so reinvert
                    data = 0x0FFF - ((msb << 8) | lsb);
                    //data = ((msb << 8) | lsb); // Inverted
                    analogWrite(PROG_D, data);
                    // This may be decreased, depending upon signal speed
                    delayMicroseconds(1000);
                    // Write ADC output N-3 to the FIFO before latching in
                    // the next in put to the ADC.
                    if (adc_ins >= adc_off) digitalWrite(WCLK_S, HIGH);
                    // These microsecond delays are probably unnecessary.
                    // Check FIFO clock hold times.
                    delayMicroseconds(10);
                    if (adc_ins >= adc_off) digitalWrite(WCLK_S, LOW);
                    delayMicroseconds(10);
                    digitalWrite(ADC_CLK_S, HIGH);
                    delayMicroseconds(10);
                    digitalWrite(ADC_CLK_S, LOW);
                    if (adc_ins < adc_off) adc_ins++;
                }
            }
            // The last 3 (adc_off) history data words remain in the ADC.
            // They will be clocked in after the switch to normal operation
            // (external WCLK).

            // Printing a hexdump back
            sprintf(hex, "%06X:\t", bufs * HBUF_SIZE + j * row);
            Serial.print(hex);
            for (uint32_t k = 0; k < end_bytes; k++) {
                sprintf(hex, "%02X", hist[row*j + k]);
                Serial.print(hex);
                if (k % col == col - 1) Serial.print(' ');
            }
            if (last) { // Pad last row
                for (uint32_t k = 0; k < 2 * (row - end_bytes); k++)
                    Serial.print(' ');
            }
            Serial.print('\t');
            for (uint32_t k = 0; k < end_bytes; k++) {
                byte b = hist[row*j + k];
                Serial.print(b < 32 ? '.' : (char) b);
            }
            Serial.println();
        }

        bufs++;
    }
    Serial.print(F("CRC32: "));
    Serial.println(crc.finalize(), HEX);
    Serial.println(F(
    "\nDone programming history."
    "Waiting for start command ('go')."));

    return 1;
}
#endif // HIST


#ifdef HIST
void start()
{
    // Start normal delay operation after programming the history.
    digitalWrite(nPROG, HIGH);
    digitalWrite(STRIG, HIGH);
    Serial.print(F("Started."));
}
#endif // HIST


#ifdef SCONTROL
void single_clock(void)
{
    digitalWrite(CLK1, HIGH);
}
#endif // SCONTROL


#ifdef SCONTROL
void dual_clock(void)
{
    digitalWrite(CLK1, LOW);
}
#endif // SCONTROL


void prog_debug(void)
{
    uint32_t nwords = 1 << 13;
    digitalWrite(nPROG, LOW);
    digitalWrite(STRIG, LOW);
    Serial.print(F("DEBUG: Programming "));
    Serial.print(nwords, DEC);
    Serial.print(F(" words of history...\n\n"));
    master_reset(false);
    set_fifo_delay(nwords, false);
    digitalWrite(nPROG, LOW);
    delayMicroseconds(10);
    // Does not account for first three words into ADC, but OK for debugging
    for (uint16_t triangle = 0u ;; triangle++) {
    //for (uint16_t triangle = 0u; triangle < nwords; triangle++) {
        // High 4 bits are discarded. Subtraction inverts.
        analogWrite(PROG_D, ((1<<12) - 1) - 4 * triangle);
        digitalWrite(WCLK_S, HIGH);
        digitalWrite(WCLK_S, LOW);
        digitalWrite(ADC_CLK_S, HIGH);
        digitalWrite(ADC_CLK_S, LOW);
    }
    Serial.println(F(
    "\nDEBUG: Done programming history."
    "Waiting for start command ('go')."));
}


void prog_clock_debug(void)
{
    digitalWrite(WCLK_S, HIGH);
    digitalWrite(WCLK_S, LOW);
    digitalWrite(ADC_CLK_S, HIGH);
    digitalWrite(ADC_CLK_S, LOW);
}

// vim:ts=4:sts=4:sw=4:et:
