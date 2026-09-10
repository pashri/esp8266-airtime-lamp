# WiFi spectrum lamp for Wemos D1 Mini + SSD1306.
# Docstrings are kept short deliberately: this board has ~23KB free.

from machine import Pin, PWM, SoftI2C
import math
import network
import ssd1306
import time

# Four adjacent pins, D5 to D8, so the LED needs no bending.
# D7 is held low as a software ground for the common leg, which sits
# third of four on these LEDs. D1/D2 are the I2C bus.
PIN_BLUE = 14  # D5
PIN_GREEN = 12  # D6
PIN_GROUND = 13  # D7, common leg
PIN_RED = 15  # D8

# Of 1023. Every channel's current returns through PIN_GROUND, and the
# D1 Mini has no series resistors and only ~12mA per pin, so this is
# deliberately low.
MAX_DUTY = 200

# Red has the lowest forward voltage, so it draws most. Cap it harder.
RED_PERCENT = 45

SCAN_PERIOD_MS = 2000
AUTO_ROTATE_MS = 8000
HISTORY_LEN = 128
PIN_BUTTON = 0  # D3, has a pull-up already. Optional.

SCREEN_COUNT = 3

# 2.4GHz band drawn across the full 128px screen.
FREQ_MIN = 2401
FREQ_MAX = 2495
# A WiFi carrier is ~22MHz wide while channels sit 5MHz apart, so
# neighbouring networks overlap and their power adds.
CHANNEL_WIDTH = 22
FADE_STEPS = 12
WEAK_DBM = -95.0
STRONG_DBM = -45.0


def make_channels():
    """Hold the common pin low, then start PWM on the three colours."""
    Pin(PIN_GROUND, Pin.OUT, value=0)
    return [PWM(Pin(p), freq=1000, duty=0) for p in (PIN_RED, PIN_GREEN, PIN_BLUE)]


def gamma(value):
    """Perceptual correction, input and output 0.0-1.0."""
    return value * value


def set_rgb(channels, r, g, b):
    """Write a colour, each component 0.0-1.0."""
    scale = [RED_PERCENT / 100.0, 1.0, 1.0]
    for i, v in enumerate((r, g, b)):
        duty = int(gamma(max(0.0, min(1.0, v))) * MAX_DUTY * scale[i])
        channels[i].duty(duty)


def hsv_to_rgb(h, s, v):
    """Hue/saturation/value to r,g,b, all 0.0-1.0."""
    i = int(h * 6.0)
    f = h * 6.0 - i
    p = v * (1.0 - s)
    q = v * (1.0 - s * f)
    t = v * (1.0 - s * (1.0 - f))
    return ((v, t, p), (q, v, p), (p, v, t), (p, q, v), (t, p, v), (v, p, q))[i % 6]


def normalise(rssi):
    """Map dBm onto 0.0-1.0 between the weak and strong thresholds."""
    level = (rssi - WEAK_DBM) / (STRONG_DBM - WEAK_DBM)
    return max(0.0, min(1.0, level))


def total_dbm(nets):
    """Combine per-network dBm into one figure via linear power.

    Decibels cannot be added directly, so each is converted to linear
    power, summed, and converted back.
    """
    if not nets:
        return WEAK_DBM
    power = sum(10.0 ** (n[2] / 10.0) for n in nets)
    return 10.0 * math.log(power) / 2.302585092994046


def channel_freq(channel):
    """Centre frequency in MHz for a 2.4GHz WiFi channel."""
    return 2484 if channel == 14 else 2412 + (channel - 1) * 5


def spectrum_bins(nets, width=128):
    """Spread each network's power over its bandwidth, summed per pixel.

    Returns one linear-power value per horizontal pixel.
    """
    span = FREQ_MAX - FREQ_MIN
    bins = [0.0] * width
    for net in nets:
        centre = channel_freq(net[1])
        power = 10.0 ** (net[2] / 10.0)
        for x in range(width):
            freq = FREQ_MIN + span * x / width
            offset = abs(freq - centre)
            if offset < CHANNEL_WIDTH / 2:
                # Triangular envelope: strongest at the centre frequency.
                bins[x] += power * (1.0 - offset / (CHANNEL_WIDTH / 2))
    return bins


def scan(wlan):
    """Return networks as (ssid, channel, rssi), strongest first."""
    found = []
    for net in wlan.scan():
        try:
            name = net[0].decode()
        except Exception:
            name = '?'
        found.append((name, net[2], net[3]))
    found.sort(key=lambda n: -n[2])
    return found


def draw_networks(display, nets, level):
    """Header plus a bar per network."""
    display.fill(0)
    if not nets:
        display.text('no networks', 0, 0)
        display.show()
        return

    display.text('WiFi  %d found' % len(nets), 0, 0)
    for row, net in enumerate(nets[:4]):
        y = 14 + row * 12
        display.text('%-9s' % net[0][:9], 0, y)
        display.text('%d' % net[2], 100, y)
        width = int(normalise(net[2]) * 22)
        if width:
            display.fill_rect(74, y + 2, width, 4, 1)
    display.hline(0, 62, int(level * 127), 1)
    display.show()


def draw_total(display, nets, total, level, history):
    """Combined signal figure, a bar, and a scrolling history graph."""
    display.fill(0)
    display.text('Total signal', 0, 0)
    display.text('%d dBm' % int(total), 0, 14)
    display.text('%d nets' % len(nets), 80, 14)

    display.rect(0, 26, 128, 8, 1)
    display.fill_rect(1, 27, int(level * 126), 6, 1)

    base = 62
    for x, value in enumerate(history[-HISTORY_LEN:]):
        height = int(value * 24 / 255)
        if height:
            display.vline(x, base - height, height, 1)
    display.hline(0, base + 1, 128, 1)
    display.show()


def draw_spectrum(display, nets, bins):
    """Occupied spectrum across the 2.4GHz band."""
    display.fill(0)
    display.text('2.4GHz spectrum', 0, 0)

    peak = max(bins) if bins else 0.0
    base = 52
    if peak > 0:
        for x, value in enumerate(bins):
            height = int((value / peak) * 34)
            if height:
                display.vline(x, base - height, height, 1)
    else:
        display.text('nothing heard', 0, 28)

    display.hline(0, base + 1, 128, 1)
    for channel in (1, 6, 11):
        x = int((channel_freq(channel) - FREQ_MIN) * 128 / (FREQ_MAX - FREQ_MIN))
        display.vline(x, base + 2, 2, 1)
        display.text(str(channel), max(0, x - 4), base + 6)
    display.show()


def channel_test(channels, display):
    """Light each pin in turn so the leg order can be read off the LED."""
    for i, name in enumerate(('D8 = RED', 'D6 = GREEN', 'D5 = BLUE')):
        display.fill(0)
        display.text('Wiring test', 0, 0)
        display.text(name, 0, 20)
        display.show()
        set_rgb(channels, *[1.0 if j == i else 0.0 for j in range(3)])
        time.sleep(1.2)
    set_rgb(channels, 0, 0, 0)


def fade_to(channels, start, target):
    """Glide between two levels so the lamp never jumps."""
    for step in range(1, FADE_STEPS + 1):
        level = start + (target - start) * step / FADE_STEPS
        r, g, b = hsv_to_rgb(0.66 - level * 0.66, 1.0, 0.15 + level * 0.85)
        set_rgb(channels, r, g, b)
        time.sleep_ms(25)


def main(test=True):
    i2c = SoftI2C(scl=Pin(4), sda=Pin(5))
    display = ssd1306.SSD1306_I2C(128, 64, i2c)
    channels = make_channels()
    button = Pin(PIN_BUTTON, Pin.IN, Pin.PULL_UP)

    wlan = network.WLAN(network.STA_IF)
    wlan.active(True)

    if test:
        channel_test(channels, display)

    level = 0.0
    screen = 0
    history = []
    last_rotate = time.ticks_ms()

    while True:
        nets = scan(wlan)
        total = total_dbm(nets)
        target = normalise(total)

        history.append(int(target * 255))
        if len(history) > HISTORY_LEN:
            del history[0]

        if screen == 0:
            draw_total(display, nets, total, target, history)
        elif screen == 1:
            draw_spectrum(display, nets, spectrum_bins(nets))
        else:
            draw_networks(display, nets, target)

        fade_to(channels, level, target)
        level = target

        # Poll the button while waiting, so a press is never missed.
        waited = 0
        while waited < SCAN_PERIOD_MS:
            if not button.value():
                screen = (screen + 1) % SCREEN_COUNT
                last_rotate = time.ticks_ms()
                while not button.value():
                    time.sleep_ms(10)
                break
            if time.ticks_diff(time.ticks_ms(), last_rotate) > AUTO_ROTATE_MS:
                screen = (screen + 1) % SCREEN_COUNT
                last_rotate = time.ticks_ms()
                break
            time.sleep_ms(20)
            waited += 20


if __name__ == '__main__':
    main()
