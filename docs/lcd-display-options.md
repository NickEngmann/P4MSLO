# LCD Display Replacement Options for ESP32-P4-EYE

## Current display

The P4-EYE ships with an **LHS154KC-IF17** module:

| Spec | Value |
|---|---|
| Controller | ST7789 |
| Resolution | 240 × 240 |
| Physical size | 1.54" |
| Color depth | 16-bit RGB565 |
| Interface | SPI (SPI2_HOST on the P4) |
| Pixel clock | 80 MHz |
| Connector | 12-pin 0.5mm pitch FPC, ~6.5mm wide |

Driver config lives in `common_components/esp32_p4_eye/`:

```c
#define BSP_LCD_SPI_MOSI          GPIO_NUM_16
#define BSP_LCD_SPI_CLK           GPIO_NUM_17
#define BSP_LCD_SPI_CS            GPIO_NUM_18
#define BSP_LCD_DC                GPIO_NUM_19
#define BSP_LCD_RST               GPIO_NUM_15
#define BSP_LCD_BACKLIGHT         GPIO_NUM_20
#define BSP_LCD_PIXEL_CLOCK_HZ    (80 * 1000 * 1000)
#define BSP_LCD_SPI_NUM           SPI2_HOST
```

UI stack: `esp_lcd` → LVGL 8.3.11 → SquareLine-Studio-generated pages. Changing resolution requires regenerating the UI project at a new canvas size.

---

## The 12-pin FPC pinout (standard ST7789)

```
Pin 1:  LEDK      (backlight cathode → GND)
Pin 2:  LEDA      (backlight anode   ← GPIO20 via BL transistor)
Pin 3:  GND
Pin 4:  VDD       (3.3V logic)
Pin 5:  SCL       (SPI clock         ← GPIO17)
Pin 6:  SDA       (SPI MOSI          ← GPIO16)
Pin 7:  DC / RS   (data/command      ← GPIO19)
Pin 8:  RST       (reset             ← GPIO15)
Pin 9:  CS        (chip select       ← GPIO18)
Pin 10: FMARK / TE (tearing effect — often NC)
Pin 11: IM0       (interface mode — tied for 4-wire SPI)
Pin 12: GND
```

**Always verify pin-1 orientation with a multimeter before plugging in a replacement** — OEM variants sometimes flip it.

---

## Replacement paths, ranked by effort

### Path 1 — Drop-in larger ST7789 (easiest)

Same driver, same SPI bus, same pin signals. Just bigger. ST7789's native max is 240×320 so you can scale up to ~2.4" without changing controllers.

| Size | Resolution | Typical module / Part |
|---|---|---|
| 1.9" | 170 × 320 | Waveshare 1.9" LCD Module (~$9) |
| 2.0" | 240 × 320 | Adafruit #4311 (~$20), or AliExpress bare-FPC (~$6) |
| 2.4" | 240 × 320 | Generic "TFT 2.4 ST7789" on AliExpress (~$6–10) |

**Important caveats:**
- Most retail ST7789 modules sold online are **breakout boards with 0.1" headers** (VCC/GND/CS/DC/RST/SCL/SDA/BL), NOT bare FPC panels. Those would need hand-wiring, not a plug-in.
- Bare-FPC panels do exist (look for OEM part numbers like `ZJY240-002A`, `ZJY240-ST7789-24`), but pinouts vary vendor-to-vendor. Get the datasheet first.
- Firmware changes needed:
  - `BSP_LCD_H_RES` and `BSP_LCD_V_RES` → 240 / 320
  - LVGL draw-buffer size adjustment
  - Regenerate the SquareLine UI at 240×320 canvas (non-trivial — all UI layout needs re-authoring)

### Path 2 — Different SPI controllers, 2.8–3.5"

If you drop ST7789, you can go bigger with similar SPI plumbing. `esp_lcd` supports these via drop-in panel components:

| Controller | Size | Resolution | Notes |
|---|---|---|---|
| ILI9341 | 2.8–3.2" | 240×320 | Mature, `esp_lcd_ili9341` component exists |
| **ST7796** ⭐ | 3.5–4.0" | 320×480 | Faster than ILI9488 over SPI, good esp_lcd support |
| ILI9488 | 3.5" | 320×480 | 18-bit native; SPI works but slower framerate |
| ILI9486 | 3.5" | 320×480 | Similar to 9488, often parallel-only |

**ST7796 is the sweet spot** for a 3.5" SPI upgrade. Waveshare has a 3.5" 480×320 ST7796 module (~$18) that uses the same MOSI/SCLK/CS/DC/RST/BL signals as the current panel — the P4's GPIOs 15–20 can be reused directly.

**Framerate reality at 80 MHz SPI:**
- 240×240×16bpp at 80 MHz: fast, headroom available
- 320×480×16bpp at 80 MHz: ~2.7× the pixels → ~30–40 fps practical, noticeable lag on full-screen transitions

### Path 3 — MIPI DSI (real upgrade, biggest lift)

The ESP32-P4 has a native MIPI DSI controller — it's designed to drive larger, higher-resolution displays than the SPI interface can handle. ESP-IDF v5.5's `esp_lcd_mipi_dsi` component supports many modern panels:

| Controller | Typical size / res |
|---|---|
| SPD2010 | 412×412 round 2.8" (used on ESP32-P4-Function-EV-Board) |
| ST7701 | 480×480 / 480×800 (many sizes) |
| EK79007 | 1024×600 7" |
| ILI9881C | 800×1280 portrait 8" |
| JD9165 / JD9365 | 800×1280 |

**But:**
- DSI uses a **40-pin 0.5mm FPC** carrying differential lanes + I²C for touch + power. Zero compatibility with the 12-pin SPI FPC.
- Requires: new enclosure, different GPIO routing (P4 has dedicated DSI pins, separate from the current display GPIOs), new driver init, possibly new touch controller support.

**Waveshare sells complete ESP32-P4 dev kits** with DSI displays already integrated. If you're going this direction, it's cheaper and faster to start there than to retrofit the P4-EYE:
- [ESP32-P4 WiFi6 Touch LCD 3.4"](https://www.waveshare.com/esp32-p4-wifi6-touch-lcd-3.4.htm) — 800×800 round
- [ESP32-P4 WiFi6 Touch LCD 4B"](https://www.waveshare.com/esp32-p4-wifi6-touch-lcd-4b.htm) — 720×720
- [7-inch DSI LCD C](https://www.waveshare.com/7inch-dsi-lcd-c.htm) — 1024×600

---

## Form factor considerations

| Display size | Fits current enclosure? |
|---|---|
| 1.9" / 2.0" | Marginal — new faceplate opening needed but PCB footprint similar |
| 2.4" | No — new faceplate + likely new mounting posts |
| 2.8"+ | No — new enclosure entirely |
| 3.5" MIPI DSI | No — fully custom build |

Current P4-EYE enclosure is ~40mm wide. A 2.8" display is ~70mm diagonal and won't fit without a custom back-housing design.

---

## Recommendation

**If "larger" means bigger numbers on screen:** a **2.4" bare-FPC 240×320 ST7789 with hand-wired adapter** is the lowest-risk path. Same driver, same SPI bus, cheap part (~$10), ~2 hours of firmware work, new faceplate. Only real pain: re-authoring the SquareLine UI for 240×320 aspect.

**If "larger" means a real upgrade:** jump straight to **ST7796 3.5" 320×480 SPI (~$18)**. Same `esp_lcd_panel_io_spi` framework, 2.7× pixel count, no MIPI DSI complexity. Needs a new enclosure but no GPIO or bus changes. UI re-authoring still needed but with more canvas to work with.

**MIPI DSI is a different project, not a display swap.** If you want a 5"+ display with touch, buy a Waveshare ESP32-P4 DSI kit instead of retrofitting the P4-EYE.

---

## Driver-change summary (Path 1 or Path 2)

**Path 1 (ST7789 240×240 → ST7789 240×320)** — firmware touch points:
- `common_components/esp32_p4_eye/include/bsp/esp32_p4_eye.h`: bump `BSP_LCD_H_RES` / `BSP_LCD_V_RES`
- `common_components/esp32_p4_eye/esp32_p4_eye.c`: panel init already works at 240×320
- SquareLine: regenerate UI at new canvas → replace generated files in `factory_demo/main/ui/`
- LVGL buffers auto-scale with `BSP_LCD_DRAW_BUFF_SIZE`

**Path 2 (ST7789 → ST7796)** — firmware touch points:
- Swap `esp_lcd_new_panel_st7789` call for `esp_lcd_new_panel_st7796` (new component dependency)
- Adjust init commands (ST7796 has different init sequence)
- Update `BSP_LCD_H_RES` / `BSP_LCD_V_RES` to 320 / 480
- Regenerate SquareLine UI at 320×480
- Potentially tune `BSP_LCD_PIXEL_CLOCK_HZ` down if the ST7796 module's FPC can't cleanly handle 80 MHz over longer traces

Neither path affects the SPI camera pipeline or any other system work — these are pure display-stack changes.
