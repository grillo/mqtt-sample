# MQTT Sample Firmware

This is based on the [OpenEEW Watson IOT firmware](https://github.com/openeew/openeew-sensor/tree/master/firmware/WatsonIoT), but simplified to allow custom backend solutions and non-authenticated MQTT brokers.

## Operation
The sensor will connect to an MQTT endpoint of your choosing, which you will be able to subscribe to and ingest data.

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

Follow this guide to [install PlatformIO and VSCode](https://docs.platformio.org/en/latest/integration/ide/vscode.html#installation) on your machine. PlaformIO offers several benefits to the Arduino IDE, particularly the ability to contain dependencies within a simple folder structure.

### Open project
Inside VSCode go to PlaformIO home, which is available on the bottom toolbar, and select `Projects`, then `Open Project`. Navigate to the root folder where you cloned this repository and open.

### Config.h
In the config.h file two levels of debugging can be set, first "debug" variable needs to be set true to allow serial communication and only basic status lines are part of the output. Second level is set by making LOG_L2 true, this would give specific output on the WiFi events.

The Sample rate needs to be defined by making true either of the 125Hz or 31.25Hz options. 

### Change IP to local MQTT endpoint
In the main.cpp file change the ip address to match that of your [MQTT broker](https://github.com/grillo/mqtt-sample/blob/4f73d4496a628dea1c99baa3dfe0725fe8c42c01/src/main.cpp#L18)

### Upload to an OpenEEW sensor
Build the project using the check mark on the bottom toolbar, then upload using the arrow button adjacent to it. The IDE should automatically detect the board of your connnected OpenEEW sensor and start to write the new firmware.

## Consume data
One method for quickly consuming data is to use mosquitto subscribe and publish commands. Once Mosquitto is installed you can run these commands using the IP address of your MQTT broker:

## Start device
Plug in your sensor to power.

### Send local Wifi details
Use the Grillo+ [Android](https://play.google.com/store/apps/details?id=com.grilloplus.iot_esptouch_demo&hl=en_US&gl=US) or [iOS](https://play.google.com/store/apps/details?id=com.grilloplus.iot_esptouch_demo&hl=en&gl=US) apps to transmit your 2.4Ghz wifi (not 5Ghz) to your sensor device.

### Subscribe to traces
In a terminal subscribe to all traces:

`mosquitto_sub -t "iot-2/evt/+/fmt/json" -h 192.168.0.4 -p 1883 -i "a:5yrusp:mosquitto"`

### Start publishing
In a separate terminal start the trace sending, setting the Live Data Duration in seconds:

`mosquitto_pub -h 192.168.0.4 -t iot-2/cmd/sendacceldata/fmt/json -m {LiveDataDuration:5}`

### Change sample rate
You can change the sample rate of the device as follows:

`mosquitto_pub -h 192.168.0.4 -t iot-2/cmd/samplerate/fmt/json -m {SampleRate:125}`
or

`mosquitto_pub -h 192.168.0.4 -t iot-2/cmd/samplerate/fmt/json -m {SampleRate:31}`
