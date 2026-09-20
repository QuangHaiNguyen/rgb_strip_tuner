| Supported Targets | ESP32 | ESP32-C2 | ESP32-C3 | ESP32-C5 | ESP32-C6 | ESP32-C61 | ESP32-H2 | ESP32-H21 | ESP32-H4 | ESP32-P4 | ESP32-S2 | ESP32-S3 | Linux |
| ----------------- | ----- | -------- | -------- | -------- | -------- | --------- | -------- | --------- | -------- | -------- | -------- | -------- | ----- |

# Hello World Example

Starts a FreeRTOS task to print "Hello World".

(See the README.md file in the upper level 'examples' directory for more information about examples.)

## How to use example

Follow detailed instructions provided specifically for this example.

Select the instructions depending on Espressif chip installed on your development board:

- [ESP32 Getting Started Guide](https://docs.espressif.com/projects/esp-idf/en/stable/get-started/index.html)
- [ESP32-S2 Getting Started Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s2/get-started/index.html)


## Example folder contents

The project **hello_world** contains one source file in C language [hello_world_main.c](main/hello_world_main.c). The file is located in folder [main](main).

ESP-IDF projects are built using CMake. The project build configuration is contained in `CMakeLists.txt` files that provide set of directives and instructions describing the project's source files and targets (executable, library, or both).

Below is short explanation of remaining files in the project folder.

```
├── CMakeLists.txt
├── pytest_hello_world.py      Python script used for automated testing
├── main
│   ├── CMakeLists.txt
│   └── hello_world_main.c
└── README.md                  This is the file you are currently reading
```

For more information on structure and contents of ESP-IDF projects, please refer to Section [Build System](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/build-system.html) of the ESP-IDF Programming Guide.

## Wi-Fi setup (captive portal)

The device connects to a stored Wi-Fi network at boot. If none is stored, or the stored one cannot be reached after
five attempts (10 s each, 2 s apart), it starts provisioning mode. Specification: [docs/specs/captive-portal.md](docs/specs/captive-portal.md).

**Enter provisioning mode at any time** by holding the button on GPIO9 (active low) for 1 second while the firmware is
running. Do not hold it during reset: on the ESP32-C3, GPIO9 low at reset enters the ROM download mode.

**Provision a network**

1. Join the open Wi-Fi network `RGB-LED-Tuner-XXXX` (`XXXX` = last four hex digits of the device MAC address).
2. The sign-in page opens by itself on Android, iOS, Windows and Linux. If it does not, browse to `http://192.168.4.1/`.
3. Press **Refresh** to rescan, pick your network, enter the password and press **Connect**.
4. The page shows `Connecting...`, then `Connected successfully` (the access point switches off 3 seconds later) or
   `Connection failed` (the access point stays up; try again). A failed attempt keeps the previously stored network.

Enterprise (WPA/WPA2/WPA3-Enterprise), WEP and WPA1-only networks are listed but cannot be selected. Hidden networks and
manual SSID entry are not supported.

**Security notes**

- **The setup network is open and the page uses plain HTTP.** While provisioning mode is active, anyone in radio range
  can join it, open the page and replace the stored network, and can capture the Wi-Fi password as it is typed. This is
  an accepted risk of the design. Only provision in a place you trust, and keep provisioning mode short.
- Credentials are stored in encrypted NVS (namespace `wifi_cfg`). This project uses the **HMAC-based key protection**
  (`CONFIG_NVS_SEC_KEY_PROTECT_USING_HMAC`, eFuse key block 0), not flash encryption. **On the first boot the NVS key
  is generated and an eFuse key block is programmed. This is irreversible.** Use a board you can spare.
- The password is never logged or returned by the device.

## Troubleshooting

* Program upload failure

    * Hardware connection is not correct: run `idf.py -p PORT monitor`, and reboot your board to see if there are any output logs.
    * The baud rate for downloading is too high: lower your baud rate in the `menuconfig` menu, and try again.

## Technical support and feedback

Please use the following feedback channels:

* For technical queries, go to the [esp32.com](https://esp32.com/) forum
* For a feature request or bug report, create a [GitHub issue](https://github.com/espressif/esp-idf/issues)

We will get back to you as soon as possible.
