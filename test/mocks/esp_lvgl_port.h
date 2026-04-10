/**
 * @brief LVGL port mock
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef void* lv_disp_t;
typedef void* lv_indev_t;
typedef int lv_disp_rot_t;
typedef void* lv_obj_t;

typedef struct {
    uint32_t task_priority;
    uint32_t task_stack;
    int task_affinity;
    uint32_t timer_period_ms;
    uint32_t task_max_sleep_ms;
} lvgl_port_cfg_t;

typedef struct {
    void *io_handle;
    void *panel_handle;
    uint32_t buffer_size;
    bool double_buffer;
    uint32_t hres;
    uint32_t vres;
    bool monochrome;
    struct { bool swap_xy; bool mirror_x; bool mirror_y; } rotation;
    struct { bool buff_dma; bool buff_spiram; bool sw_rotate; } flags;
} lvgl_port_display_cfg_t;

static inline int lvgl_port_init(const lvgl_port_cfg_t *cfg) { (void)cfg; return 0; }
static inline lv_disp_t *lvgl_port_add_disp(const lvgl_port_display_cfg_t *cfg) { (void)cfg; return (void*)1; }
static inline bool lvgl_port_lock(uint32_t ms) { (void)ms; return true; }
static inline void lvgl_port_unlock(void) {}
static inline void lv_disp_set_rotation(lv_disp_t *disp, lv_disp_rot_t rot) { (void)disp; (void)rot; }

/* LVGL object stubs */
#define LV_OBJ_FLAG_HIDDEN 0x01
static inline bool lv_obj_has_flag(lv_obj_t *obj, uint32_t flag) { (void)obj; (void)flag; return false; }
