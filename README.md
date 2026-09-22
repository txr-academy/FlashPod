# BLE LED Matrix Scoreboard

## 📌 Project Overview
This project is a Bluetooth Low Energy (BLE) controlled, accessibility-first LED scoreboard built to support motor-skill and coordination exercises for people recovering from stroke and similar conditions. Built around an ESP32 and a 64x64 RGB LED matrix, it gives patients and therapists clear, high-contrast, at-a-glance feedback on repetitions completed during a therapy session.

The system runs in an **Endless Mode** (up to 99 rounds) so a session isn't interrupted by an arbitrary game-over condition, and a therapist or caregiver can inject a real-time score correction from a phone at any point (e.g. jumping to `20/50`) without disrupting the flow of the exercise.

---

## ✨ Key Features
* **Built-In Accessibility:** Dynamic colorblind-aware palettes (Normal, Red-Green/Protanopia-Deuteranopia, Blue-Yellow/Tritanopia, and Monochrome), so the scoreboard stays legible for patients with a range of visual presentations, including visual deficits that can follow a stroke.
* **Procedural Visual Feedback:** Math-driven LED animations — an expanding "Sonar Pulse" ring for a successful attempt and a pulsing "Breathing Border" for a missed one — so feedback is immediate and satisfying without relying on stored image/GIF files.
* **Shape-Based Status Indicator:** Connection status is shown with a filled vs. hollow square in the corner, so it reads correctly even in the monochrome palette.
* **Continuous Sessions:** Runs seamlessly up to 99 rounds without interrupting an exercise session.
* **Real-Time Score Override:** A therapist or caregiver can instantly set the displayed score (e.g. `15/30`) over BLE, useful for correcting a miscount without resetting the session.
* **Wired Fallback Control:** Every command also works over the USB Serial Monitor, so the board can be operated directly from a laptop if BLE isn't available.

---

## 🛠️ Components & Specifications

| Component | Specification / Notes |
| :--- | :--- |
| **ESP32 Development Board** | Standard 38-pin or 30-pin ESP32 WROOM-32 module with BLE support. Dual-core processing handles LED matrix rendering and the BLE stack concurrently. |
| **64x64 RGB LED Matrix** | HUB75 interface, 1/32 scan rate recommended. |
| **5V Power Supply** | A dedicated 5V power supply (minimum 3A–4A) is required to power the LED matrix safely. Do *not* power the matrix directly from the ESP32 pins. |
| **Connecting Cables** | Standard 16-pin HUB75 ribbon cable and jumper wires. |
| **Controller Device** | Any smartphone or tablet with Bluetooth Low Energy (BLE) support — Android or iOS both work, since this project uses a standard BLE UART service rather than classic Bluetooth SPP. |

---

## 📦 Boards & Required Libraries

To compile this code in the Arduino IDE or PlatformIO, install the following:

**Board Setup:**
* **ESP32 Core by Espressif:** Install via the Arduino Boards Manager. Select **"ESP32 Dev Module"** as your target board.

**Required Libraries (Install via Library Manager):**
1. **[ESP32-HUB75-MatrixPanel-I2S-DMA](https://github.com/mrfaptastic/ESP32-HUB75-MatrixPanel-I2S-DMA) (by mrfaptastic):** The core driver that uses DMA to render flicker-free graphics to the matrix.
2. **[Adafruit GFX Library](https://github.com/adafruit/Adafruit-GFX-Library) (by Adafruit):** The foundational graphics engine required by the matrix library to draw text, circles, and rectangles.
3. **BLEDevice / BLEServer / BLEUtils / BLE2902:** Built natively into the ESP32 Arduino Core (no separate download required) — these provide the BLE GATT server used for wireless control.

---

## 🔌 Connection Overview (ESP32 to HUB75)

This project uses the standard I2S DMA pinout for the ESP32. Ensure your wiring matches this configuration (the `E` line is explicitly set in code for 1/32 scan panels; the rest use the library's defaults):

* **R1, G1, B1:** GPIO 25, 26, 27
* **R2, G2, B2:** GPIO 14, 12, 13
* **A, B, C, D, E:** GPIO 23, 19, 5, 17, 18
* **LAT, OE, CLK:** GPIO 4, 15, 16

---

## 📱 How to Connect & Use

1. **Power On:** Power the ESP32 and LED matrix. The board starts advertising over BLE as `GAME_Scoreboard`.
2. **Install a BLE Terminal App:** Since this uses BLE (not classic Bluetooth SPP), you don't pair through your phone's OS Bluetooth settings — instead, use a generic BLE UART app such as **nRF Connect for Mobile** (Android & iOS) or **Serial Bluetooth Terminal** in its BLE mode (Android).
3. **Scan & Connect:** In the app, scan for BLE devices and connect to `GAME_Scoreboard`. The status square on the matrix fills in solid to confirm a connection.
4. **Select the UART Service:** Open the service with UUID `6E400001-...`, and write your commands to the RX characteristic (`6E400002-...`). No line-ending is required — each write is processed as soon as it's received.
5. **Run the Session:** Send single-character commands as reps are completed, or set up quick-tap macros in your app for `1`, `2`, `R`, and `q`.
6. **Wired Fallback:** If BLE isn't convenient, connect the ESP32 to a computer via USB and send the same commands through the Arduino Serial Monitor at 115200 baud.

> The `TX` characteristic (`6E400003-...`) is set up with notifications enabled but isn't currently used to send data back to the app — it's reserved for a future feature such as pushing live score updates to a connected phone.

---

## 🎮 Command Glossary

Send these as raw text to the RX characteristic (or type them into the Serial Monitor):

| Input | Action | Visual Result |
| :---: | :--- | :--- |
| `1` (or `a`) | **Register Successful Rep** | Score increases. "Sonar Pulse" ring expands from center. |
| `2` (or `d`) | **Register Missed Rep** | Round increases. "Breathing Border" pulses at the screen edges. |
| `R` (or `r`) | **Reset Session** | Wipes the board and resets score to `00/00`. |
| `q` (or `0`) | **Accessibility Toggle** | Cycles the palette (Normal ➔ Red-Green Blind ➔ Blue-Yellow Blind ➔ Monochrome). |
| `XX/YY` | **Score Override** | *Example: `15/30`*. Instantly sets the board to 15 successful reps out of 30 rounds. |

---