# ESP32 RGB control

This project runs a local browser color picker and MQTT broker on the computer.
The ESP32 subscribes to `lights/rgb` and expects retained `#RRGGBB` messages.

## RGB LED wiring

The defaults are for a common-cathode, non-addressable RGB LED:

| LED connection | ESP32 connection |
| --- | --- |
| Red, through a 220–330 Ω resistor | GPIO 32 |
| Green, through a 220–330 Ω resistor | GPIO 33 |
| Blue, through a 220–330 Ω resistor | GPIO 25 |
| Common cathode | GND |

Override the pins or set `RGB_COMMON_ANODE=true` in `.env` when needed. Do not
connect high-power LEDs or LED strips directly to ESP32 GPIO pins; use a suitable
driver and external power supply.

## Run

Start the local web and MQTT service:

```sh
cd server
npm install
npm start
```

Open <http://127.0.0.1:8080> and choose a color.

Build and upload the ESP32 firmware from the project root:

```sh
uvx --from platformio platformio run --target upload
```

Watch ESP32 logs:

```sh
uvx --from platformio platformio device monitor \
  --port /dev/cu.usbserial-0001 \
  --baud 115200
```
