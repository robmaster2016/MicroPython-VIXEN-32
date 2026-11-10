# SPDX-License-Identifier: MIT
# Copyright (c) 2025-2026 Robert S. (aka Robmaster)
# weather.py -- current weather and a 4-day forecast, Berlin by default.
#
#   import weather
#   weather.show()                       # Berlin, once
#   weather.run()                        # refresh every 15 minutes (Ctrl-C ends)
#   weather.show(48.14, 11.58, "Munich") # anywhere else
#
# Data: Open-Meteo (https://open-meteo.com), free, no API key. In the LVGL
# build the weather is drawn as a 384x224 screen (needs bigdigits.py next to
# it); without LVGL it is printed on the console. Network via net.up().

import time
import requests

URL = ("https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s"
       "&current=temperature_2m,apparent_temperature,relative_humidity_2m,"
       "wind_speed_10m,weather_code"
       "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_sum"
       "&timezone=Europe%%2FBerlin&forecast_days=4")

BERLIN = (52.52, 13.41, "Berlin")

WMO = {
    0: "Clear sky", 1: "Mainly clear", 2: "Partly cloudy", 3: "Overcast",
    45: "Fog", 48: "Rime fog", 51: "Light drizzle", 53: "Drizzle",
    55: "Dense drizzle", 56: "Freezing drizzle", 57: "Freezing drizzle",
    61: "Light rain", 63: "Rain", 65: "Heavy rain", 66: "Freezing rain",
    67: "Freezing rain", 71: "Light snow", 73: "Snow", 75: "Heavy snow",
    77: "Snow grains", 80: "Showers", 81: "Showers", 82: "Heavy showers",
    85: "Snow showers", 86: "Snow showers", 95: "Thunderstorm",
    96: "Thunder + hail", 99: "Thunder + hail",
}
DAYS = ("Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat")


def _weekday(date):
    # "2026-11-10" -> "Tue" (Sakamoto)
    y, m, d = int(date[0:4]), int(date[5:7]), int(date[8:10])
    if m < 3:
        y -= 1
    t = (0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4)[m - 1]
    return DAYS[(y + y // 4 - y // 100 + y // 400 + t + d) % 7]


def fetch(lat=BERLIN[0], lon=BERLIN[1]):
    import network
    if not network.LAN().isconnected():
        import net
        net.up()
    r = requests.get(URL % (lat, lon))
    try:
        if r.status_code != 200:
            raise OSError("HTTP %d" % r.status_code)
        return r.json()
    finally:
        r.close()


def _text(w, name):
    c, d = w["current"], w["daily"]
    print("%s, %s" % (name, c["time"].replace("T", " ")))
    print("  %.1f C (feels %.1f C), %s" % (c["temperature_2m"], c["apparent_temperature"],
                                           WMO.get(c["weather_code"], "?")))
    print("  humidity %d %%, wind %.0f km/h" % (c["relative_humidity_2m"], c["wind_speed_10m"]))
    for i in range(len(d["time"])):
        print("  %s  %5.1f / %5.1f C  %4.1f mm  %s" % (
            _weekday(d["time"][i]), d["temperature_2m_max"][i], d["temperature_2m_min"][i],
            d["precipitation_sum"][i], WMO.get(d["weather_code"][i], "?")))


# ---- LVGL screen ------------------------------------------------------------
_ui = None


class _Screen:
    def __init__(self, lv):
        import vixen
        import bigdigits
        try:
            vixen.lv_display()
        except RuntimeError:
            pass                # already set up by an earlier program
        self.lv = lv
        scr = lv.obj()
        scr.set_style_bg_color(lv.color_hex(0x0B1A2E), 0)
        scr.remove_flag(lv.obj.FLAG.SCROLLABLE)
        self.scr = scr

        bar = lv.obj(scr)
        bar.remove_style_all()
        bar.set_size(384, 22)
        bar.set_style_bg_opa(lv.OPA.COVER, 0)
        bar.set_style_bg_color(lv.color_hex(0x16324F), 0)
        self.title = self._label(bar, 8, 4, lv.font_montserrat_14, 0xFFFFFF)
        self.stamp = self._label(bar, 0, 6, lv.font_montserrat_10, 0x8FB3D9)
        self.stamp.align(lv.ALIGN.TOP_RIGHT, -8, 6)

        self.temp = bigdigits.Display(scr, 16, 36, 58, 3, color=lv.color_hex(0xFFC34D))
        unit = self._label(scr, 16 + self.temp.width + 8, 36, lv.font_montserrat_14, 0xFFC34D)
        unit.set_text("\u00b0C")
        self.cond = self._label(scr, 200, 34, lv.font_montserrat_14, 0xFFFFFF)
        self.feels = self._label(scr, 200, 56, lv.font_montserrat_12, 0xB8C7D9)
        self.hum = self._label(scr, 200, 72, lv.font_montserrat_12, 0xB8C7D9)
        self.wind = self._label(scr, 200, 88, lv.font_montserrat_12, 0xB8C7D9)

        self.days = []
        w = 87
        for i in range(4):
            p = lv.obj(scr)
            p.set_size(w, 104)
            p.set_pos(8 + i * (w + 6), 110)
            p.set_style_pad_all(4, 0)
            p.set_style_radius(6, 0)
            p.set_style_border_width(0, 0)
            p.set_style_bg_color(lv.color_hex(0x16324F if i else 0x1F4A73), 0)
            p.remove_flag(lv.obj.FLAG.SCROLLABLE)
            day = self._label(p, 0, 0, lv.font_montserrat_14, 0xFFFFFF)
            hi = self._label(p, 0, 22, lv.font_montserrat_14, 0xFFC34D)
            lo = self._label(p, 0, 40, lv.font_montserrat_12, 0x7FB2FF)
            rain = self._label(p, 0, 58, lv.font_montserrat_10, 0xB8C7D9)
            cond = self._label(p, 0, 74, lv.font_montserrat_10, 0xFFFFFF)
            cond.set_width(w - 8)
            cond.set_long_mode(lv.label.LONG_MODE.WRAP)
            self.days.append((day, hi, lo, rain, cond))
        lv.screen_load(scr)

    def _label(self, parent, x, y, font, color):
        lv = self.lv
        lb = lv.label(parent)
        lb.set_pos(x, y)
        lb.set_style_text_font(font, 0)
        lb.set_style_text_color(lv.color_hex(color), 0)
        lb.set_text("")
        return lb

    def update(self, w, name):
        c, d = w["current"], w["daily"]
        self.title.set_text(name)
        self.stamp.set_text("Open-Meteo  " + c["time"].replace("T", "  "))
        t = int(c["temperature_2m"] + (0.5 if c["temperature_2m"] >= 0 else -0.5))
        self.temp.set(("%3d" % t)[-3:])
        self.cond.set_text(WMO.get(c["weather_code"], "?"))
        self.feels.set_text("Feels like %.0f \u00b0C" % c["apparent_temperature"])
        self.hum.set_text("Humidity %d %%" % c["relative_humidity_2m"])
        self.wind.set_text("Wind %.0f km/h" % c["wind_speed_10m"])
        for i, (day, hi, lo, rain, cond) in enumerate(self.days):
            day.set_text("Today" if i == 0 else _weekday(d["time"][i]))
            hi.set_text("%.0f\u00b0" % d["temperature_2m_max"][i])
            lo.set_text("%.0f\u00b0" % d["temperature_2m_min"][i])
            rain.set_text("%.1f mm" % d["precipitation_sum"][i])
            cond.set_text(WMO.get(d["weather_code"][i], "?"))


def show(lat=BERLIN[0], lon=BERLIN[1], name=BERLIN[2]):
    global _ui
    w = fetch(lat, lon)
    try:
        import lvgl as lv
    except ImportError:
        _text(w, name)
        return w
    if _ui is None:
        _ui = _Screen(lv)
    _ui.update(w, name)
    return w


def run(lat=BERLIN[0], lon=BERLIN[1], name=BERLIN[2], minutes=15):
    while True:
        try:
            show(lat, lon, name)
        except OSError as e:
            print("weather:", e)
        for _ in range(minutes * 60):
            time.sleep(1)      # LVGL keeps running while we wait
