/*
 * ============================================================
 *  EDVON BOARD — Line Follower + Serial Control (FINAL BUILD)
 *  Pairs with ESP32_Controller_FINAL.ino
 * ============================================================
 *
 *  WHAT'S IN HERE:
 *   1. OBSTACLE LATCH — 'X' stops the motors and HOLDS them stopped,
 *      so followLine() can no longer override the emergency stop.
 *   2. COMMS WATCHDOG — if no valid command arrives for 700ms the
 *      ESP link is assumed dead and the motors stop (fail-safe).
 *   3. BUFFER DRAIN — reads all pending bytes each loop so the
 *      incoming keep-alive stream can never build up a backlog.
 *   4. NOISE IMMUNITY — only the 8 real command letters reset the
 *      watchdog, so random noise on a floating RX pin can't keep
 *      a dead link looking "alive".
 *
 *  TIMING (must stay in sync with the ESP32 sketch):
 *    ESP sends 'X' every  70ms while an obstacle is present
 *    ESP sends 'A' every 250ms as an auto-mode keep-alive
 *    ESP repeats manual commands every 200ms while a button is held
 *    -> COMMS_TIMEOUT of 700ms sits safely above all of these
 * ============================================================
 *
 *  WIRING:
 *   EDVON RX (pin0) <- ESP32 GPIO17 (TX2)   REQUIRED
 *   EDVON GND       <- ESP32 GND            REQUIRED (common ground)
 *
 *  >>> IMPORTANT: unplug the wire from EDVON pin0 (RX) BEFORE you
 *      upload this sketch, or the upload will fail. Plug it back
 *      in after uploading.
 * ============================================================
 */

// ── Motor Pins ───────────────────────────────────────────────
#define LEFT_DIR1   2
#define LEFT_PWM    3
#define LEFT_DIR2   4
#define RIGHT_DIR1  5
#define RIGHT_PWM   6
#define RIGHT_DIR2  7

// ── IR Sensor Pins ───────────────────────────────────────────
#define SENSOR_LEFT    A4
#define SENSOR_CENTER  A3
#define SENSOR_RIGHT   A2

// ── Calibrated Sensor Thresholds ─────────────────────────────
#define THRESHOLD_LEFT    670
#define THRESHOLD_CENTER  274
#define THRESHOLD_RIGHT   51

// ── Speed Settings ───────────────────────────────────────────
const uint8_t SPD_FWD    = 200;
const uint8_t SPD_FAST   = 230;
const uint8_t SPD_SLOW   = 100;
const uint8_t SPD_SEARCH = 150;
const uint8_t SPD_MANUAL = 180;

// ── Mode + obstacle latch ────────────────────────────────────
bool autoMode     = true;    // true = line follow, false = manual
bool obstacleStop = false;   // latched by 'X', cleared by any other valid command

// ── Watchdog ─────────────────────────────────────────────────
const uint32_t COMMS_TIMEOUT = 700;   // ms without a valid command = link lost
uint32_t lastRxTime = 0;
bool     linkLost   = false;

// ── Line-follow state ────────────────────────────────────────
int8_t   lastDir  = 0;
uint32_t lostTime = 0;
bool     lost     = false;
const uint32_t SEARCH_TIMEOUT = 2000;

// ============================================================
//  MOTOR CONTROL
// ============================================================
void stopMotors() {
  digitalWrite(LEFT_DIR1,  LOW); analogWrite(LEFT_PWM,  0); digitalWrite(LEFT_DIR2,  LOW);
  digitalWrite(RIGHT_DIR1, LOW); analogWrite(RIGHT_PWM, 0); digitalWrite(RIGHT_DIR2, LOW);
}

void leftFwd(uint8_t s)  { digitalWrite(LEFT_DIR1,HIGH);  analogWrite(LEFT_PWM,s);  digitalWrite(LEFT_DIR2,LOW);  }
void leftRev(uint8_t s)  { digitalWrite(LEFT_DIR1,LOW);   analogWrite(LEFT_PWM,s);  digitalWrite(LEFT_DIR2,HIGH); }
void leftOff()           { digitalWrite(LEFT_DIR1,LOW);   analogWrite(LEFT_PWM,0);  digitalWrite(LEFT_DIR2,LOW);  }
void rightFwd(uint8_t s) { digitalWrite(RIGHT_DIR1,HIGH); analogWrite(RIGHT_PWM,s); digitalWrite(RIGHT_DIR2,LOW); }
void rightRev(uint8_t s) { digitalWrite(RIGHT_DIR1,LOW);  analogWrite(RIGHT_PWM,s); digitalWrite(RIGHT_DIR2,HIGH);}
void rightOff()          { digitalWrite(RIGHT_DIR1,LOW);  analogWrite(RIGHT_PWM,0); digitalWrite(RIGHT_DIR2,LOW); }

void goForward()  { leftFwd(SPD_FWD);    rightFwd(SPD_FWD);    }
void goLeft()     { leftOff();           rightFwd(SPD_FAST);   }
void goRight()    { leftFwd(SPD_FAST);   rightOff();           }
void gentleLeft() { leftFwd(SPD_SLOW);   rightFwd(SPD_FWD);    }
void gentleRight(){ leftFwd(SPD_FWD);    rightFwd(SPD_SLOW);   }
void searchLeft() { leftRev(SPD_SEARCH); rightFwd(SPD_SEARCH); }
void searchRight(){ leftFwd(SPD_SEARCH); rightRev(SPD_SEARCH); }

// ============================================================
//  SENSORS
// ============================================================
bool seeBlack(uint8_t pin, int threshold) {
  return analogRead(pin) > threshold;
}

// ============================================================
//  LINE FOLLOW LOGIC
// ============================================================
void followLine() {
  bool L = seeBlack(SENSOR_LEFT,   THRESHOLD_LEFT);
  bool C = seeBlack(SENSOR_CENTER, THRESHOLD_CENTER);
  bool R = seeBlack(SENSOR_RIGHT,  THRESHOLD_RIGHT);

  if (L && C && R)   { goForward();   lastDir= 0; lost=false; return; } // crossing
  if (C && !L && !R) { goForward();   lastDir= 0; lost=false; return; } // on line
  if (C && L && !R)  { gentleLeft();  lastDir=-1; lost=false; return; } // drifting right
  if (C && R && !L)  { gentleRight(); lastDir= 1; lost=false; return; } // drifting left
  if (L && !C && !R) { goLeft();      lastDir=-1; lost=false; return; } // sharp left
  if (R && !C && !L) { goRight();     lastDir= 1; lost=false; return; } // sharp right
  if (L && R && !C)  { goForward();               lost=false; return; } // gap

  // Line lost -> search in last known direction, then give up
  if (!lost) { lostTime = millis(); lost = true; }
  if (millis() - lostTime > SEARCH_TIMEOUT) { stopMotors(); return; }
  if (lastDir <= 0) searchLeft(); else searchRight();
}

// ============================================================
//  SERIAL COMMANDS FROM ESP32
// ============================================================
bool isValidCmd(char c) {
  return (c=='X'||c=='A'||c=='M'||c=='F'||c=='B'||c=='L'||c=='R'||c=='S');
}

// NOTE: every valid command EXCEPT 'X' clears the obstacle latch.
// Unknown bytes are ignored so line noise can never clear a stop.
void handleCommand(char cmd) {
  switch (cmd) {
    case 'X': obstacleStop = true;  stopMotors(); break;  // latched emergency stop
    case 'A': obstacleStop = false; autoMode = true;  break;
    case 'M': obstacleStop = false; autoMode = false; stopMotors(); break;
    case 'F': obstacleStop = false; if(!autoMode){ leftFwd(SPD_MANUAL); rightFwd(SPD_MANUAL); } break;
    case 'B': obstacleStop = false; if(!autoMode){ leftRev(SPD_MANUAL); rightRev(SPD_MANUAL); } break;
    case 'L': obstacleStop = false; if(!autoMode){ leftOff();           rightFwd(SPD_MANUAL); } break;
    case 'R': obstacleStop = false; if(!autoMode){ leftFwd(SPD_MANUAL); rightOff();           } break;
    case 'S': obstacleStop = false; if(!autoMode)  stopMotors();        break;
    default:  break;  // ignore noise
  }
}

// ============================================================
//  SETUP & LOOP
// ============================================================
void setup() {
  Serial.begin(9600);   // link to ESP32

  pinMode(LEFT_DIR1,  OUTPUT); pinMode(LEFT_PWM,  OUTPUT); pinMode(LEFT_DIR2,  OUTPUT);
  pinMode(RIGHT_DIR1, OUTPUT); pinMode(RIGHT_PWM, OUTPUT); pinMode(RIGHT_DIR2, OUTPUT);
  pinMode(SENSOR_LEFT,   INPUT);
  pinMode(SENSOR_CENTER, INPUT);
  pinMode(SENSOR_RIGHT,  INPUT);

  stopMotors();
  delay(2000);            // settle time before moving
  lastRxTime = millis();  // start the watchdog clock AFTER the delay
}

void loop() {
  // 1) Drain every pending byte (keep-alive stream must not back up)
  while (Serial.available()) {
    char cmd = Serial.read();
    if (isValidCmd(cmd)) {
      lastRxTime = millis();     // only real commands feed the watchdog
      if (linkLost) linkLost = false;
    }
    handleCommand(cmd);
  }

  // 2) WATCHDOG — link dead? stop and stay stopped until it returns
  if (millis() - lastRxTime > COMMS_TIMEOUT) {
    if (!linkLost) linkLost = true;
    stopMotors();
    return;                       // no line-following without the ESP
  }

  // 3) OBSTACLE LATCH — hold stopped, skip line-following
  if (obstacleStop) {
    stopMotors();
    return;
  }

  // 4) Normal autonomous line-following
  if (autoMode) followLine();
}
