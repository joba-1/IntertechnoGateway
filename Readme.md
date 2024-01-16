# IntertechnoGateway

Use OOK 443 MHz sender (like FS1000A) to send Intertechno commands initiated by mqtt requests or via web page

![image](https://github.com/joba-1/IntertechnoGateway/assets/32450554/06cb5db0-e65a-42fc-ad5d-504dc0eb79b9)

## Circuit

| ESP32 | FS1000A |
|-------|---------|
| VCC*  | VCC     |
| GND   | GND     |
| IO16**| DATA    |

*) 3.3V will, work, but with reduced range  
**) most GPIO pins will work, just define PIN_DATA 

## Build

### Prerequisite

* PlatformIO (e.g. as VS Code extension or standalone)
* ESP32 (any with AsyncWebserver support)

### Usage

* Edit platformio.ini: 
    * change upload and monitor ports to where the ESP is plugged in
    * change default env to what matches your ESP best
* Start build and upload
  ```
  pio run --target buildfs
  pio run --target uploadfs
  pio run --target upload
  ```
* Optional: check serial output to see what's going on on the ESP
  ```
  pio device monitor
  ```
* Connect to AP intertechnogateway-1 and configure wifi via http://192.168.4.1
* Open http://intertechnogateway-1 in a browser
* or publish on mqtt topic intertechnogateway/1/cmd
    * payload has 3 characters: 
        1) family (0-f)
        1) device (0-f)
        1) off or on (0 or 1)

## Other

* Uses Bootstrap (5.2.3) for flexible layout (served as local files. Size: ~60k)
* Uses JQuery (3.6.1) (Size: ~30k)
* Uses base64 encoded favicon converted by https://www.base64-image.de/ (Size: ~300 bytes)
* Uses Preferences lib to store current family and device for the web page
* Optional: the ESP will contact ntp, syslog and mqtt broker. See platformio.ini for configuration.
