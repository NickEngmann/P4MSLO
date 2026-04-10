# LVGL Simulator Screenshots

The simulator generates PNG screenshots covering every page and interaction state. Run the simulator in headless mode to regenerate:

```bash
cd test/simulator/build
./p4eye_sim --screenshot
```

Screenshots are saved to `test/simulator/screenshots/`.

## Screenshot Map (44 screenshots)

### Main Menu (00-06)
Carousel-style menu scrolling through all 7 items.

| # | File | Description |
|---|------|-------------|
| 00 | `main_menu_camera.png` | Camera selected |
| 01 | `main_menu_interval_cam.png` | Interval Camera selected |
| 02 | `main_menu_video_mode.png` | Video Mode selected |
| 03 | `main_menu_ai_detect.png` | AI Detect selected |
| 04 | `main_menu_album.png` | Album selected |
| 05 | `main_menu_usb_disk.png` | USB Disk selected |
| 06 | `main_menu_settings.png` | Settings selected |

### Camera Page (07-12)

| # | File | Description |
|---|------|-------------|
| 07 | `camera_main.png` | Camera mode popup |
| 08 | `camera_btn_down.png` | Down button press |
| 09 | `camera_btn_up.png` | Up button press |
| 10 | `camera_take_photo.png` | Encoder press (take photo) |
| 11 | `camera_zoom_in.png` | Knob right (zoom in) |
| 12 | `camera_zoom_out.png` | Knob left (zoom out) |

### Album Page (13-17)
Uses colored placeholder photos (blue, green, red, orange, purple).

| # | File | Description |
|---|------|-------------|
| 13 | `album_photo1.png` | Photo 1/5 (blue) |
| 14 | `album_photo2.png` | Photo 2/5 (green) |
| 15 | `album_photo3.png` | Photo 3/5 (red) |
| 16 | `album_photo4.png` | Photo 4/5 (orange) |
| 17 | `album_photo3_back.png` | Navigate back to Photo 3/5 |

### Settings Page (18-26)

| # | File | Description |
|---|------|-------------|
| 18 | `settings_main.png` | Settings list (Display Rotate, Obj Det, Resolution, Flash) |
| 19-23 | `settings_item2-6.png` | Scrolling through items |
| 24 | `settings_toggle_right.png` | Toggle setting right |
| 25 | `settings_toggle_left.png` | Toggle setting left |
| 26 | `settings_encoder_press.png` | Encoder press on setting |

### Video Mode (27-30)

| # | File | Description |
|---|------|-------------|
| 27 | `video_mode_main.png` | Video mode popup |
| 28 | `video_mode_record.png` | Start recording |
| 29 | `video_mode_btn_down.png` | Down button |
| 30 | `video_mode_btn_up.png` | Up button |

### AI Detection (31-35)

| # | File | Description |
|---|------|-------------|
| 31 | `ai_detect_main.png` | AI detect popup |
| 32 | `ai_detect_mode2.png` | Cycle to mode 2 |
| 33 | `ai_detect_mode3.png` | Cycle to mode 3 |
| 34 | `ai_detect_zoom_in.png` | Knob right |
| 35 | `ai_detect_zoom_out.png` | Knob left |

### Interval Camera (36-41)

| # | File | Description |
|---|------|-------------|
| 36 | `interval_cam_main.png` | Interval mode popup |
| 37 | `interval_cam_start.png` | Start interval |
| 38 | `interval_cam_btn_up.png` | Up button |
| 39 | `interval_cam_btn_down.png` | Down button |
| 40 | `interval_cam_time_plus.png` | Increase interval time |
| 41 | `interval_cam_time_minus.png` | Decrease interval time |

### USB Disk & Final (42-43)

| # | File | Description |
|---|------|-------------|
| 42 | `usb_disk_main.png` | USB disk connection page |
| 43 | `final_main_menu.png` | Return to main menu |
