/*----------------------------------------------------------------------------/
/ TJpgDec - Tiny JPEG Decompressor R0.03         (C)ChaN, 2021
/ Standalone copy for GIF encoder — renamed symbols to avoid LVGL conflict.
/----------------------------------------------------------------------------*/
#ifndef GIF_TJPGDEC_H
#define GIF_TJPGDEC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <string.h>
#include <stdint.h>

/* Configuration */
#define JD_SZBUF      512
#define JD_FORMAT      0       /* 0=RGB888 output */
#define JD_USE_SCALE   0
#define JD_TBLCLIP     1
#define JD_FASTDECODE  2

typedef int16_t jd_yuv_t;

typedef enum {
    JDR_OK = 0,
    JDR_INTR,
    JDR_INP,
    JDR_MEM1,
    JDR_MEM2,
    JDR_PAR,
    JDR_FMT1,
    JDR_FMT2,
    JDR_FMT3
} JRESULT;

typedef struct {
    uint16_t left, right, top, bottom;
} JRECT;

typedef struct JDEC JDEC;
struct JDEC {
    size_t dctr;
    uint8_t* dptr;
    uint8_t* inbuf;
    uint8_t dbit;
    uint8_t scale;
    uint8_t msx, msy;
    uint8_t qtid[3];
    uint8_t ncomp;
    int16_t dcv[3];
    uint16_t nrst;
    uint16_t width, height;
    uint8_t* huffbits[2][2];
    uint16_t* huffcode[2][2];
    uint8_t* huffdata[2][2];
    int32_t* qttbl[4];
    uint32_t wreg;
    uint8_t marker;
    uint8_t longofs[2][2];
    uint16_t* hufflut_ac[2];
    uint8_t* hufflut_dc[2];
    void* workbuf;
    jd_yuv_t* mcubuf;
    void* pool;
    size_t sz_pool;
    size_t (*infunc)(JDEC*, uint8_t*, size_t);
    void* device;
};

JRESULT gif_jd_prepare(JDEC* jd, size_t (*infunc)(JDEC*,uint8_t*,size_t), void* pool, size_t sz_pool, void* dev);
JRESULT gif_jd_decomp(JDEC* jd, int (*outfunc)(JDEC*,void*,JRECT*), uint8_t scale);

#ifdef __cplusplus
}
#endif

#endif
