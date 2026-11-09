/*
 * ============================================================
 *  UNIVERSITÀ DEGLI STUDI DI PERUGIA
 *  Corso di Laurea: Ingegneria Informatica
 *  Insegnamento:    Reti Logiche e VHDL
 *  Docente:         Federico Alimenti
 * ============================================================
 *  PROJECT:  Lightning Detection
 *  GROUP:    A-03
 *
 *  MEMBERS:
 *    - Giulia Emili                 - [364565]
 *    - Roberto Matteucci            - [365604]
 *    - Francesca Moretti            - [369131]
 *    - Luis Alessandro Siancas Curi - [370778]
 *    - Lorenzo Trinati              - [367257]
 *
 *  ACADEMIC YEAR: 2025/2026
 *  DATE:          [15/05/2026]
 *  VERSION:       4.5
 * ============================================================
 *
 *  BRIEF DESCRIPTION:
 *    THIS IS A DEMO CODE JUST FOR LIVE DEMONSTATION (do not work with real storm)
 *    Lightning detection station based on the AS3935 Franklin
 *    Lightning Sensor IC. 
 *    The system monitors lightning activity within a 40 km radius 
 *    and displays real-time data on a 16×2 LCD.
 *    A push-button toggles monitoring ON/OFF and others two  
 *    button to navigate recording history NEXT - PREV.
 *
 *    ON -> actively polls the IRQ line of the AS3935; for each event,  
 *          the lightning symbol flashes, the LCD display updates with 
 *          the event type, and the last event is stored in Arduino's 
 *          memory until the buffer limit is reached, then the buffer 
 *          begins to be cyclically replaced.
 *         
 *    OFF -> the system is no more sensible to the IRQ, no more update. 
 *           You can still navigate the past records.
 *
 *  HARDWARE COMPONENTS:
 *    - Arduino Uno
 *    - 16×2 LCD Display (HD44780-compatible, 4-bit mode)
 *    - AS3935 Franklin Lightning Sensor IC (ams AG)
 *    - Push-button (momentary, normally open)
 *    - 10 kΩ pull-up resistor (button line)
 *    - 100 nF decoupling capacitors (AS3935 VDD / VREG)
 *    - Active Buzzer
 *    - Breadboard & jumper wires
 *
 *    NOTE:
 *    Check the Pin Configuration before running the code. 
 *
 * ============================================================
 */

// ============================================================
//  LIBRARIES
// ============================================================
#include <SPI.h>
#include <LiquidCrystal.h>
#include "SparkFun_AS3935.h"


// ============================================================
//  HARDWARE
// ============================================================
LiquidCrystal lcd(A0, A1, 4, 5, 6, 7);

const int BTN_ON   = 9;
const int BTN_NEXT = A4;
const int BTN_PREV = A3;

const int IRQ_PIN    = 2;
const int CS_PIN     = 3;
const int BUZZER_PIN = 8;

SparkFun_AS3935 lightning;


// ============================================================
//  AS3935 SETTINGS
// ============================================================
#define INDOOR         0x12
#define OUTDOOR        0x0E
#define LIGHTNING_INT  0x08
#define DISTURBER_INT  0x04
#define NOISE_INT      0x01

const byte CAPACITOR_VALUE  = 120;
const byte NOISE_FLOOR      = 7;
const byte WATCHDOG_VAL     = 7;
const byte SPIKE_REJECTION  = 2;
const byte LIGHTNING_THRESH = 1;


// ============================================================
//  EVENT HISTORY  (circular buffer)
// ============================================================
#define HISTORY_SIZE 24

struct EventRecord {
  String label;     
  byte distance;   
  long energy;
  unsigned long timestamp;  
};

EventRecord history[HISTORY_SIZE];
int historyCount = 0;   // number of valid entries (max HISTORY_SIZE)
int writeIndex = 0;   // next write slot in the circular buffer


// ============================================================
//  LABELS
// ============================================================
const char* LABEL_LIGHTNING = "LIGHTNING ";
const char* LABEL_DISTURBER = "DISTURBER ";
const char* LABEL_NOISE     = "NOISE     ";
const char* LABEL_WAITING   = "WAITING...";


// ============================================================
//  SYSTEM STATE
// ============================================================
bool systemActive = false; 

const unsigned long DEBOUNCE_MS = 50; // buttons debounce
const int BUZZER_FREQ = 5500; // buzzer sound

// History browse state
bool historyView  = false;
int viewIndex = 0;  
unsigned long lastBrowseMs = 0;

const unsigned long BROWSE_TIMEOUT_MS = 3000;  // ms before browse auto-exits

// Label shown in row 0 during LIVE mode
const char* currentLabel = LABEL_WAITING;

//  After each sensor interrupt the lightning icon blinks ON/OFF
const unsigned long FLASH_INTERVAL_MS = 325;  // toggle every 125 ms (4 blinks)

bool flashActive = false;
bool flashState = false; 
unsigned long flashStart = 0;
unsigned long lastFlashToggle = 0;

// Live label timer
const unsigned long LABEL_SHOW_MS = 2000; 
bool labelActive = false;
unsigned long labelStart = 0;

// ============================================================
//  CUSTOM LCD ICON
// ============================================================
byte lightningOff[8] = {
  0b00001, // ....#
  0b00110, // ..##.
  0b01100, // .##..
  0b10011, // #..##
  0b11001, // ##..#
  0b00110, // ..##.
  0b01100, // .##..
  0b10000  // #....
};

byte lightningOn[8] = {
  0b00001, // ....#
  0b00110, // ..##.
  0b01100, // .##..
  0b11111, // #####
  0b11111, // #####
  0b00110, // ..##.
  0b01100, // .##..
  0b10000  // #....
};


// ============================================================
//  SETUP
// ============================================================
void setup() {

  Serial.begin(115200);
  Serial.println(F("[Setup] Booting..."));

  pinMode(BTN_ON,   INPUT_PULLUP);
  pinMode(BTN_NEXT, INPUT_PULLUP);
  pinMode(BTN_PREV, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(IRQ_PIN, INPUT);

  lcd.begin(16, 2);
  lcd.createChar(0, lightningOff);
  lcd.createChar(1, lightningOn);

  SPI.begin();
  initAS3935();

  updateDisplay();
  Serial.println(F("[Setup] Ready. Press ON to start monitoring."));
}


// ============================================================
//  MAIN LOOP
// ============================================================
void loop() {
  handleButtons();

  // Auto-exit browse mode after inactivity timeout
  if (historyView && millis() - lastBrowseMs > BROWSE_TIMEOUT_MS) {
    historyView = false;
    updateDisplay();
  }

  // Poll sensor only when the system is active
  if (systemActive && digitalRead(IRQ_PIN) == HIGH) {
    processAS3935Event();
  }else if(!systemActive){
    lightning.readInterruptReg();   // clear IRQ
  }

  // blink the flash icon + buzzer sound
  if (flashActive) {  
    unsigned long now = millis();

    if (now - lastFlashToggle >= FLASH_INTERVAL_MS) {
      lastFlashToggle = now;
      flashState = !flashState;
      lcd.setCursor(0, 0);
      lcd.write(flashState ? byte(1) : byte(0));
      flashActive = false;

      //buzzer sound 
      playSound(BUZZER_FREQ, 500); 
    }
  }

  // live label
  if (labelActive && millis() - labelStart >= LABEL_SHOW_MS) {
    labelActive  = false;
    currentLabel = LABEL_WAITING;
    if (!historyView) updateDisplay();
  }
}
// ============================================================
//  AS3935 INITIALISATION
// ============================================================
void initAS3935() {
  if (!lightning.beginSPI(CS_PIN)) {
    Serial.println(F("[ERROR] AS3935 not found — check wiring. Halting."));
    lcd.setCursor(0, 0); lcd.print(F("AS3935 ERROR    "));
    lcd.setCursor(0, 1); lcd.print(F("Check wiring!   "));
    while (1);  // halt execution
  }
  Serial.println(F("[INFO] AS3935 found."));

  lightning.tuneCap(CAPACITOR_VALUE);
  lightning.maskDisturber(false);       // set true to ignore disturbers
  lightning.setIndoorOutdoor(INDOOR);
  lightning.setNoiseLevel(NOISE_FLOOR);
  lightning.watchdogThreshold(WATCHDOG_VAL);
  lightning.spikeRejection(SPIKE_REJECTION);
  lightning.lightningThreshold(LIGHTNING_THRESH);


  Serial.println(F("[INFO] AS3935 configuration:"));
  Serial.print(F("  Environment   : "));
  Serial.println(lightning.readIndoorOutdoor() == INDOOR ? F("Indoor") : F("Outdoor"));
  Serial.print(F("  Noise floor   : ")); Serial.println(lightning.readNoiseLevel());
  Serial.print(F("  Watchdog      : ")); Serial.println(lightning.readWatchdogThreshold());
  Serial.print(F("  Spike reject  : ")); Serial.println(lightning.readSpikeRejection());
  Serial.print(F("  Strike thresh : ")); Serial.println(lightning.readLightningThreshold());
  Serial.print(F("  Mask disturber: ")); Serial.println(lightning.readMaskDisturber() ? F("YES") : F("NO"));
}



// ============================================================
//  CIRCULAR HISTORY BUFFER
// ============================================================
/*
 * Pushes a new event into the circular buffer.
 * When the buffer is full it overwrites the oldest entry.
 */
void pushEvent(const char* label, byte dist, long energy,   
               unsigned long ts) {
  history[writeIndex] = { label, dist, energy, ts };
  writeIndex = (writeIndex + 1) % HISTORY_SIZE;
  if (historyCount < HISTORY_SIZE) historyCount++;
  viewIndex = 0;  // reset browse cursor to the newest entry
}

/*
 * Returns the physical index of the most recent event
 * in the circular buffer.
 */
int newestSlot() {
  return (writeIndex - 1 + HISTORY_SIZE) % HISTORY_SIZE;
}
/*
 * Converts a logical viewIndex (0 = newest, N-1 = oldest)
 * to the physical array index in the circular buffer.
 */
int historySlot(int viewIdx) {
  return (writeIndex - 1 - viewIdx + HISTORY_SIZE) % HISTORY_SIZE;
}



// ============================================================
//  FUNCTIONS
// ============================================================

/**
 * Reads the AS3935 IRQ register, classifies the event, updates
 * the snapshot and triggers a display refresh.
 */
void processAS3935Event() {
  delay(2);   // AS3935 datasheet 5.4: wait 2 ms before reading IRQ register
  byte intVal = lightning.readInterruptReg();

  const char* label = nullptr;
  byte dist = 0;
  long energy = 0;

  if (intVal == NOISE_INT) {
    label  = LABEL_NOISE;
    Serial.println(F("[EVENT] Noise"));

  } else if (intVal == DISTURBER_INT) {
    label  = LABEL_LIGHTNING; // DONE JUST FOR DEMO SHOULD BE DISTURBER
    Serial.print(F("[EVENT] Lightning "));
    Serial.print(dist);

  } else if (intVal == LIGHTNING_INT) {
    label  = LABEL_LIGHTNING;
    Serial.print(F("[EVENT] Lightning! "));
    /*dist   = lightning.distanceToStorm();
    energy = lightning.lightningEnergy();
    Serial.print(dist);
    Serial.print(F(" km | energy: "));
    Serial.println(energy);*/

  } else {
    return;  // unknown interrupt — ignore
  }

  // Store the event and update the live label
  currentLabel = label;
  pushEvent(label, dist, energy, millis());

  labelActive = true;
  labelStart  = millis();

  // Start flash icon
  flashActive = true;
  flashState = true;
  flashStart = millis();
  lastFlashToggle = millis();

  // Refresh display
  if (!historyView) updateDisplay();
}


/**
 * Refreshes the 16×2 LCD.
 *
 *  Layout:
 *
 *  When system is OFF:
 *  ------------------
 *  |   SENSOR OFF   |  // SENSOR OFF
 *  | 00:05:32 - EVN |  // [TimeStamp]-[Last Event Labe] 
 *  ------------------
 *
 *  When system is ON:
 *  ------------------
 *  | ⚡ LIGHTNING    | // [⚡] - [EVENT DETECTED] ("LIGHTNING", "DISTURBER", "NOISE", "WAITING")
 *  | 00:05:32 - EVN | // [TimeStamp]-[Last Event Labe] 
 *  ------------------
 *
 *  When historyView:
 *  ------------------
 *  |[i/N ] 00:05:32  | 
 *  |EVENT  EN - KM   | 
 *  ------------------
 */
void updateDisplay() {
  lcd.clear();

  // --- BROWSE mode ---
  if (historyView && historyCount > 0) {
    int slot = historySlot(viewIndex);

    // Row 0: "[index/TOT] HH:MM:SS"
    lcd.setCursor(0, 0);

    lcd.print("[");
    lcd.print(viewIndex + 1);
    lcd.print("/");
    lcd.print(historyCount);
    lcd.print("] ");

    lcd.print(formatTimestamp(history[slot].timestamp));

    // Row 1: full label + distance (if available)
    lcd.setCursor(0, 1);
    if(history[slot].distance > 0 && history[slot].energy > 0){
      lcd.print(history[slot].label.substring(0,4));
    
      lcd.print(" ");
      lcd.print(history[slot].distance);
      lcd.print("km - ");

      lcd.print(formatEnergy(history[slot].energy));
    }else{
      lcd.print(history[slot].label);
    }
    return;
  }

  
  if (!systemActive) {
    // Row 0: SENSOR OFF
    lcd.setCursor(0, 0);
    lcd.print(F("   SENSOR OFF   "));

    // Raw 1: [TimeStamp]-[Last Event Labe] 
    lcd.setCursor(0, 1);
    if (historyCount == 0) {
    lcd.print(F("No events yet"));
    } else {
    int slot = newestSlot();
    lcd.print(formatTimestamp(history[slot].timestamp));
    lcd.print(" - ");
    lcd.print(history[slot].label.substring(0,4));
    }
    return;
  } 

    // --- SENSOR ON ---
  if(systemActive){
    // Row 0: [⚡] - [EVENT DETECTED]
    lcd.setCursor(0, 0);
    lcd.write(flashState ? byte(1) : byte(0));
    lcd.setCursor(3, 0);
    lcd.print(currentLabel);

    // Row 1: newest event summary or placeholder
    lcd.setCursor(0, 1);
    if (historyCount == 0) {
    lcd.print(F("No events yet"));
    } else {
    int slot = newestSlot();
    lcd.print(formatTimestamp(history[slot].timestamp));
    lcd.print(" - ");
    lcd.print(history[slot].label.substring(0,4));
    }
  }
  
}


//  BUTTON HANDLING
/*
 * Returns true exactly once when a button transitions from HIGH
 * to LOW, with software debounce. Uses one set of static
 * variables per pin.
 */
bool readButton(int pin) {
  static unsigned long lastTimeOn   = 0;
  static unsigned long lastTimeNext = 0;
  static unsigned long lastTimePrev = 0;
  static bool lastOn   = HIGH;
  static bool lastNext = HIGH;
  static bool lastPrev = HIGH;

  bool reading = digitalRead(pin);
  bool pressed = false;
  unsigned long now = millis();

  if (pin == BTN_ON) {
    if (lastOn == HIGH && reading == LOW && now - lastTimeOn > DEBOUNCE_MS) {
      pressed = true;
      lastTimeOn = now;
    }
    lastOn = reading;

  } else if (pin == BTN_NEXT) {
    if (lastNext == HIGH && reading == LOW && now - lastTimeNext > DEBOUNCE_MS) {
      pressed = true;
      lastTimeNext = now;
    }
    lastNext = reading;

  } else if (pin == BTN_PREV) {
    if (lastPrev == HIGH && reading == LOW && now - lastTimePrev > DEBOUNCE_MS) {
      pressed = true;
      lastTimePrev = now;
    }
    lastPrev = reading;
  }

  return pressed;
}

void handleButtons() {
  bool onPressed   = readButton(BTN_ON);
  bool nextPressed = readButton(BTN_NEXT);
  bool prevPressed = readButton(BTN_PREV);

  // ON button: toggle monitoring on/off 
  if (onPressed) {
    systemActive = !systemActive;
    historyView  = false;
    flashActive  = false;
    flashState   = false;
    labelActive  = false;

    if (systemActive) currentLabel = LABEL_WAITING;

    Serial.print(F("[INFO] System -> "));
    Serial.println(systemActive ? F("ON") : F("OFF"));
    updateDisplay();
  }

  // NEXT and PREV do nothing if there are no events yet
  if (historyCount == 0) return;

  //  NEXT button: scroll toward older events 
  if (nextPressed) {
    lastBrowseMs = millis();
    if (!historyView) {
      historyView = true;
      viewIndex   = 0;
    } else if (viewIndex < historyCount - 1) {
      viewIndex++;
    }
    Serial.print(F("[BROWSE] "));
    Serial.print(viewIndex + 1);
    Serial.print('/');
    Serial.println(historyCount);
    updateDisplay();
  }

  // PREV button: scroll toward newer events
  if (prevPressed) {
    lastBrowseMs = millis();
    if (!historyView) {
      historyView = true;
      viewIndex   = 0;
    } else if (viewIndex > 0) {
      viewIndex--;
    }
    Serial.print(F("[BROWSE] "));
    Serial.print(viewIndex + 1);
    Serial.print('/');
    Serial.println(historyCount);
    updateDisplay();
  }
}

// ============================================================
//  UTILITIES
// ============================================================
/* Generates a tone on the active buzzer. */
void playSound(int hz, int ms) {
  tone(BUZZER_PIN, hz);
  delay(ms);
  noTone(BUZZER_PIN);
  delay(ms / 2);
}

/*
 * Converts millis() to the string "HH:MM:SS".
 * out must point to a buffer of at least 9 bytes.
 */
String formatTimestamp(unsigned long ms) {
  unsigned long totalSec = ms / 1000UL;
  unsigned long h = totalSec / 3600UL;
  unsigned long m = (totalSec % 3600UL) / 60UL;
  unsigned long s = totalSec % 60UL;

  char buf[9];
  snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu", h % 100UL, m, s);
  return String(buf);
}

/*format the energy value to a printable string*/
String formatEnergy(long energy) {
  if (energy < 0) energy = 0;

  if (energy < 1000) {
    return String(energy);
  }

  if (energy < 10000) {
    long v = (energy + 9) / 10;   // ceil a 3 cifre totali
    return String(v) + " ";
  }

  if (energy < 100000) {
    long v = (energy + 99) / 100; // ceil
    return String(v);
  }

  if (energy < 1000000) {
    long v = (energy + 999) / 1000; // ceil in k
    return String(v) + "k";
  }

  return ">999k";
}


// ============================================================
//  END
// ============================================================
