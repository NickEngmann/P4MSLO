/**
 * @file sim_hal.h
 * @brief Hardware abstraction layer for the LVGL simulator
 *
 * Provides stub implementations for all ESP32 hardware functions
 * called by ui_extra.c: app_storage, app_album, app_video_stream, app_isp.
 */
#ifndef SIM_HAL_H
#define SIM_HAL_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/*-------------------------------------------------*/
/* Simulated hardware state (readable for debug)    */
/*-------------------------------------------------*/

typedef struct {
    /* Storage */
    bool     settings_saved;

    /* Album */
    int      album_image_count;
    int      album_current_index;
    bool     album_can_store;

    /* Video/Camera */
    bool     flash_on;
    bool     recording_video;
    int      photo_count;
    int      photo_resolution;  /* 0=480P, 1=720P, 2=1080P */
    bool     interval_active;
    uint16_t interval_minutes;

    /* ISP */
    uint32_t contrast;
    uint32_t saturation;
    uint32_t brightness;
    uint32_t hue;

    /* Gyroscope */
    bool     gyroscope_enabled;
    bool     coco_od_enabled;
} sim_hw_state_t;

/** Get pointer to simulated hardware state for inspection/modification */
sim_hw_state_t *sim_hal_get_state(void);

/** Reset all simulated hardware to default values */
void sim_hal_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* SIM_HAL_H */
