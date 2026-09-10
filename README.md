# Airtime Lamp

A Wemos D1 Mini with an SSD1306 OLED and an RGB LED, showing how much
2.4GHz radio activity is in the air around it.

It listens in promiscuous mode and hops channels 1-13, counting every frame
the radio hears -- beacons, traffic from phones and laptops, even
acknowledgements -- and weighting each by its received power. This measures
actual airtime rather than how many access points exist, so it responds to a
neighbour streaming video, not just to routers being present.

The LED fades blue when quiet through to red when busy.

## Wiring

The LED occupies four adjacent pins, so it pushes straight in with no
jumper wires:

| Pin | Role |
|---|---|
| D5 | Blue |
| D6 | Green |
| D7 | Common leg (held low in software as a ground) |
| D8 | Red |

D7 is driven permanently low rather than wired to the ground pin, which is
on the opposite header. This suits these LEDs, whose common leg is third of
four.

The OLED is on I2C: SDA on D1, SCL on D2. Note that `Wire.begin()` takes SDA
first.

No series resistors are fitted, so `MAX_DUTY` caps brightness at 200 of
1023. Every channel's current returns through the single D7 pin, against a
rating of roughly 12mA.

## Screens

Rotating every 8 seconds, or on a button press if one is fitted to D3:

1. Combined power in dBm, percentage, busiest channel, and a history graph
2. Activity per channel, scaled against the loudest
3. Live view of the channel currently being sampled

## Calibration

`QUIET_DBM` and `BUSY_DBM` set the range the lamp maps onto its colours.
The defaults are -95 and -35. Read the on-screen dBm figure somewhere quiet
and again beside a router, and set them from those.

## Build

```
arduino-cli compile --fqbn esp8266:esp8266:d1_mini airtime_lamp
arduino-cli upload --fqbn esp8266:esp8266:d1_mini --port /dev/cu.usbserial-0001 airtime_lamp
```

## micropython/

The earlier MicroPython version, kept for reference. It scans for networks
rather than sniffing traffic, so it only sees access points announcing
themselves -- MicroPython exposes no promiscuous mode. Restoring it means
reflashing MicroPython firmware.
