/*----------------------------------------------------------------------------/
/ TJpgDec - Tiny JPEG Decompressor R0.03 include file         (C)ChaN, 2021
/ Standalone version extracted from LVGL for GIF encoder use.
/----------------------------------------------------------------------------*/
#ifndef DEF_TJPGDEC
#define DEF_TJPGDEC

#ifdef __cplusplus
extern "C" {
#endif

#include <string.h>
#include <stdint.h>

/* Configuration — match LVGL's defaults for best performance */
#define JD_SZBUF    512     /* Stream input buffer size */
#define JD_FORMAT   0       /* 0=RGB888, 1=RGB565 */
#define JD_USE_SCALE 0      /* Disable scaling */
#define JD_TBLCLIP  1       /* Use clipping table */
#define JD_FASTDECODE 2     /* Fastest decode mode */

typedef int16_t jd_yuv_t;

/* Error code */
typedef enum {
    JDR_OK = 0,     /* 0: Succeeded */
    JDR_INTR,       /* 1: Interrupted by output function */
    JDR_INP,        /* 2: Device error or wrong termination of input stream */
    JDR_MEM1,       /* 3: Insufficient memory pool for the image */
    JDR_MEM2,       /* 4: Insufficient stream input buffer */
    JDR_PAR,        /* 5: Parameter error */
    JDR_FMT1,       /* 6: Data format error (may be broken data) */
    JDR_FMT2,       /* 7: Right format but not supported */
    JDR_FMT3        /* 8: Not supported JPEG standard */
} JRESULT;

/* Rectangular region in the output image */
typedef struct {
    uint16_t left;      /* Left end */
    uint16_t right;     /* Right end */
    uint16_t top;       /* Top end */
    uint16_t bottom;    /* Bottom end */
} JRECT;

/* Decompressor object structure */
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

/* TJpgDec API functions */
JRESULT gif_jd_prepare (JDEC* jd, size_t (*infunc)(JDEC*,uint8_t*,size_t), void* pool, size_t sz_pool, void* dev);
JRESULT gif_jd_decomp (JDEC* jd, int (*outfunc)(JDEC*,void*,JRECT*), uint8_t scale);

#ifdef __cplusplus
}
#endif

#endif /* DEF_TJPGDEC */
