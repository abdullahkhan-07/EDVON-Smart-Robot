**FOR ESP32** 
 * ============================================================
 *  ESP32 — WiFi Robot Controller + BUZZER  
 *  Web control panel + ultrasonic obstacle stop for EDVON board
 *  Pairs with EDVON_Combined_FINAL.ino
 * ============================================================
 *
 *  NEW IN THIS VERSION: audible buzzer feedback
 *    • 2 short beeps at boot / WiFi ready
 *    • URGENT repeating alarm while an obstacle is detected
 *    • 1 short beep when the path clears and it resumes
 *    • 1 blip on every mode / button command
 *    • Mute toggle button in the web UI
 *
 *  The beeping is fully NON-BLOCKING (no delay()), so it never
 *  stalls the sensor reads or the command keep-alive stream.
 * ============================================================
 *
 *  BUZZER WIRING (choose your type below with BUZZER_ACTIVE):
 *    Buzzer +  ->  ESP32 GPIO25
 *    Buzzer -  ->  ESP32 GND
 *
 *    ACTIVE buzzer  = has a built-in tone, just needs power. Set BUZZER_ACTIVE 1
 *    PASSIVE buzzer = needs a driving frequency (like a tiny speaker).
 *                     Set BUZZER_ACTIVE 0
 *    If unsure: touch it to 3.3V briefly. If it makes a tone on its own,
 *    it is ACTIVE. If it only clicks, it is PASSIVE.
 *
 *    If your buzzer is loud/current-hungry, drive it through an NPN
 *    transistor (e.g. 2N2222: GPIO25 -> 1k -> base, emitter -> GND,
 *    collector -> buzzer -, buzzer + -> 5V) instead of straight off the pin.
 * ============================================================
 *
 *  WIFI MODES (set WIFI_MODE below):
 *   0 = AUTO -> try router (Station); if it fails, start own hotspot
 *   1 = STA  -> Station only: ESP joins your router
 *   2 = AP   -> Access Point only: ESP creates its own hotspot
 *
 *  OTHER WIRING:
 *   ESP32 GPIO17 (TX2) -> EDVON RX (pin0)   REQUIRED
 *   ESP32 GND          -> EDVON GND         REQUIRED (common ground)
 *   HC-SR04: VCC->5V  GND->GND  TRIG->GPIO5  ECHO->GPIO18

 **FOR EDVON BOARD**
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
 *   (put a 5V->3.3V divider on ECHO: 1k in series, 2k to GND)
 * ============================================================
## Contributors
- Abdullah Khan — [@abdullahkhan-07](https://github.com/abdullahkhan-07)
- Shehzeen Rizwan — [@rizwanshehzeen](https://github.com/rizwanshehzeen)
- Areeba Zehra — [@areebazehra-290](https://github.com/areebazehra-290)
