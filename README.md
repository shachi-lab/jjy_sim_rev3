# 📡 JJY Simulator Rev.3 for ESP32-C3 / ESP8684

Build your own JJY time-signal simulator with an ESP32-C3 or ESP8684!  
This compact simulator reproduces the 40 kHz / 60 kHz JJY radio time signal using PWM output,  
based on accurate time obtained from NTP.  
Rev.3 is developed with ESP-IDF.  


👉 [README 日本語版はこちら](README_ja.md)

---

## 🪛 Features

* Supports both **ESP32-C3** and **ESP8684**
* Full-featured implementation based on **ESP-IDF**
* **Easy AP mode** for Wi-Fi setup with a single button
* **OLED display** for checking the current time and sync status
* Automatic time synchronization via **NTP**
* Switchable **JJY 40 kHz (eastern Japan) / 60 kHz (western Japan)**
* Output **band optimization** can be selected with an on-board switch
* Signal is radiated from a built-in **simple antenna** on the board
* **H-bridge output stage** for higher output power (may transmit farther than expected)
* Configuration page accessible from a **web browser**
* Supports **time zones** and **DST (Daylight Saving Time)**
* Displays a **clock screen** during operation

<a href="./images/jjy-sim-r3.jpg">
<img src="./images/jjy-sim-r3.jpg" alt="JJY Simulator Rev.3" width=400 style="max-width:100%; height:auto;"></a><br/>

---

## 🛠 Build and Flash (ESP-IDF / CLI)

JJY-SIM R3 is built and flashed using ESP-IDF.  
R2 was Arduino-based,  
but R3 has moved to ESP-IDF for better timing accuracy and a cleaner overall architecture.

> ### ⚠️ Note
> * This project cannot be built in the Arduino environment
> * This is an ESP-IDF-only project

### ■ Environment

* ESP-IDF v5.x
* Python (with a working `idf.py` environment)

For ESP-IDF setup, please refer to the official documentation.

### ■ Build

Move to the project directory and run:

```bash
idf.py build
```

### ■ Flash

```bash
idf.py flash
```

If you need to specify the serial port:

```bash
idf.py -p COMx flash
```

### ■ Serial Monitor

```bash
idf.py monitor
```

### ■ Full Command

```bash
idf.py -p COMx flash monitor
```

This single command lets you flash the firmware and open the log output at the same time.

### ■ Target Selection

Set the target once during initial setup.
The required target depends on which module you are using.

* When using **ESP32-C3**:

```bash
idf.py set-target esp32c3
```

* When using **ESP8684**:
  Since the ESP8684 is based on **ESP32-C2**,
  select `esp32c2` as the chip target.

```bash
idf.py set-target esp32c2
```

---

## 🛠 Build and Flash (VSCode / GUI)

You can also develop this project using **VSCode + the ESP-IDF Extension**.

### ■ Recommended Extension

* ESP-IDF Extension (Espressif)

By installing the extension in VSCode, you can perform the following from the GUI:

* Build
* Flash
* Serial monitor

### ■ Setup

Set up your environment according to the ESP-IDF Extension instructions.

Once the extension is installed, you can run actions such as:

* `Build`
* `Flash`
* `Monitor`

with a single click.

### ■ Workspace

When opening the project in VSCode,
open **`jjy-sim-r3.code-workspace`** in the source-code folder
to work with the ESP-IDF Extension settings already included.

---

## ⚙️ Usage

### ■ Wi-Fi Setup

* After power-on or reset, press the **CONFIG** switch within 5 seconds  
  On first boot, the device enters Wi-Fi setup mode automatically
* An AP named `JJY-SIM-R3-XXXXXXXX` will start
* Connect from a smartphone or PC → a captive portal will appear
* Select the SSID and enter the password
* You can also configure the band, local time, and DST (Daylight Saving Time) as needed
* Click **[Save and Reboot]** to save the settings and restart the device

### ■ Operation Check

* After power-on or reset, the logo is shown. If left untouched for 5 seconds, the device will try to connect to the configured Wi-Fi AP
* Once connected, it obtains the current time from an NTP server
* The current time is shown on the OLED, and JJY signal output starts from second 0 using PWM
* Just press the **receive button** on your radio-controlled clock and place it nearby

### ■ Controls During Operation

While the clock screen is displayed, the following operations are available:

* Press the **[CONFIG]** button for about 1 second to show the information screen  
  Press **[CONFIG]** again to return to the clock screen
* Press and hold the **[CONFIG]** button for 5 seconds or longer to stop JJY transmission output  
  Press and hold **[CONFIG]** again for 5 seconds or longer to resume JJY transmission output

---

## 🔌 Pin Assignment (Main Signals)

| Function       | GPIO | Notes                                                        |
| -------------- | ---- | ------------------------------------------------------------ |
| JJY PWM output | 10,4 | Modulated output signals, inverted A/B pair for the H-bridge |
| CONFIG switch  | 9    | Used for Wi-Fi setup (**shared with BOOT pin**)              |
| ACT LED        | 5    | Activity LED                                                 |
| IND LED        | 0    | Status indicator LED (can be shared)                         |
| OLED Reset     | 2    | OLED module reset                                            |
| OLED SDA       | 7    | OLED I2C data                                                |
| OLED SCL       | 6    | OLED I2C clock                                               |

---

## 🧾 File Structure

### 💻 Firmware

* The firmware source code is located in the **`firmware/idf`** folder.
* This firmware uses a rebuilt library, ported as an IDF component, based on the Arduino OLED driver library used in R2.  
This component is publicly available on GitHub.  
👉 [SSD1306 OLED display driver](https://github.com/Shachi-lab/oled_display)
* `storage_key.h` contains placeholder values and can be used as-is.  
  For better security, you may want to replace them with your own values before building.

### 🧰 Hardware

* The hardware design files are located in the **`hardware`** folder.
* The schematic and PCB design data are in **`hardware/KiCad`**.

  * Compatible with **KiCad Ver.9** only (cannot be opened with Ver.8 or earlier)
  * **Gerber data** is included in **`KiCad/PLOT`**, so the board can be fabricated as-is
* The 3D case data is in **`hardware/case_3d`**.

---

## 📸 Appearance / Assembly Examples

These are the board and case exterior views.  
The design uses a black PCB and a black case so that only the display appears to float visually.

<a href="./images/jjy-sim-r3_pcb.jpg">
<img src="./images/jjy-sim-r3_pcb.jpg" alt="JJY Simulator Rev.3 PCB" width=300 style="max-width:100%; height:auto;"></a>
<a href="./images/jjy-sim-r3_case.jpg">
<img src="./images/jjy-sim-r3_case.jpg" alt="JJY Simulator Rev.3 with case" width=300 style="max-width:100%; height:auto;"></a><br/>

---

## ⚠️ Caution

This device may radiate farther than expected.  
Please make sure to comply with applicable radio regulations when experimenting with it.

---

## 🔗 Related Links

* Blog article with detailed explanation  
  👉 [https://blog.shachi-lab.com/054_jjy-sim-r3-esp8684-idf/](https://blog.shachi-lab.com/054_jjy-sim-r3-esp8684-idf/)

