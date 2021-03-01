# MQTT Sample Firmware

This is based on the OpenEEW Watson IOT firmware, but simplified to allow custom backend solutions and non-authenticated MQTT brokers.

## Operation
In the program setup we check that the accelerometer is present, calibrate it and set the ODR, LPF and RANGE. The device reads the flash memory for saved networks and scans to see if they match any available. If there's a match, connect.

For the loop, the device checks if still connected to WiFi, if not, retry connection. When a PPS signal is present, it interrupts the system, gets the timestamp and starts a micros timer.When the interrupt coming from the ADXL is present, meaning that FIFO is full, the system takes the timestamp, and attaches the micro seconds that passed since the PPS started, giving time accuracy. Then the device reads the FIFO values, puts them into a JSON message and sends them to the udpDestination and udpPort specified in the config file. Multiple FIFOS can be concatenated in a message, number of fifos in a message can be specified in the config file.

For tracking purposes the traces have a consecutive id, this is not intended for a production firmware, their purpose is to count how many traces are sent and received.

### Instructions

This example requires installation of the MQTT broker on any hardware (cloud, server, laptop, rasperry pi, etc), and then configuration of the device's firmware.
- Create an [MQTT broker](https://mosquitto.org/download/) and keep the ip address and port for later.
- Install [VSCode](https://code.visualstudio.com/) and [PlatformIO for ESP32](https://platformio.org/).
- Create a [Watson IoT Platform](https://cloud.ibm.com/catalog/services/internet-of-things-platform) service instance
- Clone this repo and change the ip address to match your [MQTT broker](https://github.com/grillo/mqtt-sample/blob/4f73d4496a628dea1c99baa3dfe0725fe8c42c01/src/main.cpp#L18)
- Start your device and consume data using node-red, python, nodejs or any other MQTT client.

## Flash a new device

### Setup
### Install PlatformIO

Follow this guide to [install PlatformIO](https://docs.platformio.org/en/latest/integration/ide/vscode.html#installation) on your machine. PlaformIO offers several benefits to the Arduino IDE, particularly the ability to contain dependencies within a simple folder structure.

### Open project
Inside VSCode go to PlaformIO home, which is available on the bottom toolbar, and select `Projects`, then `Open Project`. Navigate to the root folder where you cloned this repository and open.

### Config.h
In the config.h file two levels of debugging can be set, first "debug" variable needs to be set true to allow serial communication and only basic status lines are part of the output. Second level is set by making LOG_L2 true, this would give specific output on the WiFi events.

The Sample rate needs to be defined by making true either of the 125Hz or 31.25Hz options. 

### Change IP to local MQTT endpoint
In the main.cpp file change the ip address to match that of your [MQTT broker](https://github.com/grillo/mqtt-sample/blob/4f73d4496a628dea1c99baa3dfe0725fe8c42c01/src/main.cpp#L18)

### Upload to an OpenEEW sensor
Build the project using the check mark on the bottom toolbar, then upload using the arrow button adjacent to it. The IDE should automatically detect the board of your connnected OpenEEW sensor and start to write the new firmware.

