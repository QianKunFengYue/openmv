/*
 * This file is part of the OpenMV project.
 *
 * Copyright (c) 2013-2021 Ibrahim Abdelkader <iabdalkader@openmv.io>
 * Copyright (c) 2013-2021 Kwabena W. Agyeman <kwagyeman@openmv.io>
 *
 * This work is licensed under the MIT license, see the file LICENSE for details.
 *
 * Minimalistic JPEG baseline encoder.
 * Ported from public domain JPEG writer by Jon Olick - http://jonolick.com
 * DCT implementation is based on Arai, Agui, and Nakajima's algorithm for scaled DCT.
 */
#include <stdio.h>

#include "ff_wrapper.h"
#include "imlib.h"
#include "omv_boardconfig.h"

#define TIME_JPEG   (0)
#if (TIME_JPEG == 1)
#include "py/mphal.h"
#endif

// Expand 4 bits to 32 for binary to grayscale - process 4 pixels at a time
static const uint32_t u32Expand[16] = {0x0, 0xff, 0xff00, 0xffff, 0xff0000,
    0xff00ff, 0xffff00, 0xffffff, 0xff000000, 0xff0000ff, 0xff00ff00,
    0xff00ffff, 0xffff0000, 0xffff00ff, 0xffffff00, 0xffffffff};

#if (OMV_HARDWARE_JPEG == 1)
#include STM32_HAL_H
#include "irq.h"

#define MCU_W                       (8)
#define MCU_H                       (8)
#define JPEG_444_GS_MCU_SIZE        ((MCU_W) * (MCU_H))
#define JPEG_444_YCBCR_MCU_SIZE     ((JPEG_444_GS_MCU_SIZE) * 3)
#define FB_ALLOC_PADDING            ((__SCB_DCACHE_LINE_SIZE) * 4)
#define OUTPUT_CHUNK_SIZE           (512) // The minimum output buffer size is 2x this - so 1KB.
#define JPEG_INPUT_FIFO_BYTES       (32)
#define JPEG_OUTPUT_FIFO_BYTES      (32)

static JPEG_HandleTypeDef JPEG_Handle = {};
static JPEG_ConfTypeDef JPEG_Config = {};
MDMA_HandleTypeDef JPEG_MDMA_Handle_In = {};
MDMA_HandleTypeDef JPEG_MDMA_Handle_Out = {};

static int JPEG_out_data_length_max = 0;
static volatile int JPEG_out_data_length = 0;
static volatile bool JPEG_input_paused = false;
static volatile bool JPEG_output_paused = false;

// JIFF-APP0 header designed to be injected at the start of the JPEG byte stream.
// Contains a variable sized COM header at the end for cache alignment.
static const uint8_t JPEG_APP0[] = {
    0xFF, 0xE0, // JIFF-APP0
    0x00, 0x10, // 16
    0x4A, 0x46, 0x49, 0x46, 0x00, // JIFF
    0x01, 0x01, // V1.01
    0x01, // DPI
    0x00, 0x00, // Xdensity 0
    0x00, 0x00, // Ydensity 0
    0x00, // Xthumbnail 0
    0x00, // Ythumbnail 0
    0xFF, 0xFE // COM
};

void JPEG_IRQHandler()
{
    IRQ_ENTER(JPEG_IRQn);
    HAL_JPEG_IRQHandler(&JPEG_Handle);
    IRQ_EXIT(JPEG_IRQn);
}

void HAL_JPEG_GetDataCallback(JPEG_HandleTypeDef *hjpeg, uint32_t NbDecodedData)
{
    HAL_JPEG_Pause(hjpeg, JPEG_PAUSE_RESUME_INPUT);
    JPEG_input_paused = true;
}

void HAL_JPEG_DataReadyCallback(JPEG_HandleTypeDef *hjpeg, uint8_t *pDataOut, uint32_t OutDataLength)
{
    // We have received this much data.
    JPEG_out_data_length += OutDataLength;

    if ((JPEG_out_data_length + OUTPUT_CHUNK_SIZE) > JPEG_out_data_length_max) {
        // We will overflow if we receive anymore data.
        HAL_JPEG_Pause(hjpeg, JPEG_PAUSE_RESUME_OUTPUT);
        JPEG_output_paused = true;
    } else {
        uint8_t *new_pDataOut = pDataOut + OutDataLength;

        // DMA will write data to the output buffer in __SCB_DCACHE_LINE_SIZE aligned chunks. At the
        // end of JPEG compression the processor will manually transfer the remaining parts of the
        // image in randomly aligned chunks. We only want to invalidate the cache of the output
        // buffer for the initial DMA chunks. So, this code below will do that and then only
        // invalidate aligned regions when the processor is moving the final parts of the image.
        if (!(((uint32_t) new_pDataOut) % __SCB_DCACHE_LINE_SIZE)) {
            SCB_InvalidateDCache_by_Addr((uint32_t *) new_pDataOut, OUTPUT_CHUNK_SIZE);
        }

        // We are ok to receive more data.
        HAL_JPEG_ConfigOutputBuffer(hjpeg, new_pDataOut, OUTPUT_CHUNK_SIZE);
    }
}

bool jpeg_compress(image_t *src, image_t *dst, int quality, bool realloc)
{
#if (TIME_JPEG==1)
    mp_uint_t start = mp_hal_ticks_ms();
#endif

    int mcu_size = 0;
    JPEG_ConfTypeDef JPEG_Info;
    JPEG_Info.ImageWidth    = src->w;
    JPEG_Info.ImageHeight   = src->h;
    JPEG_Info.ImageQuality  = quality;

    switch (src->bpp) {
        case IMAGE_BPP_BINARY:
        case IMAGE_BPP_GRAYSCALE:
            mcu_size                    = JPEG_444_GS_MCU_SIZE;
            JPEG_Info.ColorSpace        = JPEG_GRAYSCALE_COLORSPACE;
            JPEG_Info.ChromaSubsampling = JPEG_444_SUBSAMPLING;
            break;
        case IMAGE_BPP_RGB565:
        case IMAGE_BPP_BAYER:
            mcu_size                    = JPEG_444_YCBCR_MCU_SIZE;
            JPEG_Info.ColorSpace        = JPEG_YCBCR_COLORSPACE;
            JPEG_Info.ChromaSubsampling = JPEG_444_SUBSAMPLING;
            break;
    }

    if (memcmp(&JPEG_Config, &JPEG_Info, sizeof(JPEG_ConfTypeDef))) {
        HAL_JPEG_ConfigEncoding(&JPEG_Handle, &JPEG_Info);
        memcpy(&JPEG_Config, &JPEG_Info, sizeof(JPEG_ConfTypeDef));
    }

    int src_w_mcus = (src->w + MCU_W - 1) / MCU_W;
    int src_w_mcus_bytes = src_w_mcus * mcu_size;
    int src_w_mcus_bytes_2 = src_w_mcus_bytes * 2;

    // If dst->data == NULL then we need to fb_alloc() space for the payload which will be fb_free()'d
    // by the caller. We have to alloc this memory for all cases if we return from the method.
    if (!dst->data) {
        uint32_t avail = fb_avail();
        uint32_t space = src_w_mcus_bytes_2 + FB_ALLOC_PADDING;

        if (avail < space) {
            fb_alloc_fail();
        }

        dst->bpp = avail - space;
        dst->data = fb_alloc(dst->bpp, FB_ALLOC_PREFER_SIZE | FB_ALLOC_CACHE_ALIGN);
    }

    // Compute size of the APP0 header with cache alignment padding.
    int app0_size = sizeof(JPEG_APP0);
    int app0_unalign_size = app0_size % __SCB_DCACHE_LINE_SIZE;
    int app0_padding_size = app0_unalign_size ? (__SCB_DCACHE_LINE_SIZE - app0_unalign_size) : 0;
    int app0_total_size = app0_size + app0_padding_size;

    if (dst->bpp < app0_total_size) {
        return true; // overflow
    }

    // Adjust JPEG size and address by app0 header size.
    dst->bpp -= app0_total_size;
    uint8_t *dma_buffer = dst->data + app0_total_size;

    // Destination is too small.
    if (dst->bpp < (OUTPUT_CHUNK_SIZE * 2)) {
        return true; // overflow
    }

    JPEG_out_data_length_max = dst->bpp;
    JPEG_out_data_length = 0;
    JPEG_input_paused = false;
    JPEG_output_paused = false;

    uint8_t *mcu_row_buffer = fb_alloc(src_w_mcus_bytes_2, FB_ALLOC_PREFER_SPEED | FB_ALLOC_CACHE_ALIGN);

    for (int y_offset = 0; y_offset < src->h; y_offset += MCU_H) {
        uint8_t *mcu_row_buffer_ptr = mcu_row_buffer + (src_w_mcus_bytes * ((y_offset / MCU_H) % 2));

        int dy = src->h - y_offset;
        if (dy > MCU_H) {
            dy = MCU_H;
        }

        for (int x_offset = 0; x_offset < src->w; x_offset += MCU_W) {
            uint8_t *Y0 = mcu_row_buffer_ptr + (mcu_size * (x_offset / MCU_W));
            uint8_t *CB = Y0 + JPEG_444_GS_MCU_SIZE;
            uint8_t *CR = CB + JPEG_444_GS_MCU_SIZE;

            int dx = src->w - x_offset;
            if (dx > MCU_W) {
                dx = MCU_W;
            }

            // Copy 8x8 MCUs.
            switch (src->bpp) {
                case IMAGE_BPP_BINARY: {
                    if ((dx != MCU_W) || (dy != MCU_H)) { // partial MCU, fill with 0's to start
                        memset(Y0, 0, JPEG_444_GS_MCU_SIZE);
                    }

                    for (int y = y_offset, yy = y + dy; y < yy; y++) {
                        uint32_t *rp = IMAGE_COMPUTE_BINARY_PIXEL_ROW_PTR(src, y);
                        uint8_t pixels = rp[x_offset >> UINT32_T_SHIFT] >> (x_offset & UINT32_T_MASK);

                        if (dx == MCU_W) {
                            *((uint32_t *) Y0) = u32Expand[pixels & 0xf];
                            *(((uint32_t *) Y0) + 1) = u32Expand[pixels >> 4];
                        } else if (dx >= 4) {
                            *((uint32_t *) Y0) = u32Expand[pixels & 0xf];

                            if (dx >= 6) {
                                *(((uint16_t *) Y0) + 2) = u32Expand[pixels >> 4];

                                if (dx & 1) {
                                    Y0[6] = (pixels & 0x40) ? 0xff : 0x00;
                                }
                            } else if (dx & 1) {
                                Y0[4] = (pixels & 0x10) ? 0xff : 0x00;
                            }
                        } else if (dx >= 2) {
                            *((uint16_t *) Y0) = u32Expand[pixels & 0x3];

                            if (dx & 1) {
                                Y0[2] = (pixels & 0x4) ? 0xff : 0x00;
                            }
                        } else {
                            *Y0 = (pixels & 0x1) ? 0xff : 0x00;
                        }

                        Y0 += MCU_W;
                    }
                    break;
                }
                case IMAGE_BPP_GRAYSCALE: {
                    if ((dx != MCU_W) || (dy != MCU_H)) { // partial MCU, fill with 0's to start
                        memset(Y0, 0, JPEG_444_GS_MCU_SIZE);
                    }

                    for (int y = y_offset, yy = y + dy; y < yy; y++) {
                        uint8_t *rp = IMAGE_COMPUTE_GRAYSCALE_PIXEL_ROW_PTR(src, y) + x_offset;

                        if (dx == MCU_W) {
                            *((uint32_t *) Y0) = *((uint32_t *) rp);
                            *(((uint32_t *) Y0) + 1) = *(((uint32_t *) rp) + 1);
                        } else if (dx >= 4) {
                            *((uint32_t *) Y0) = *((uint32_t *) rp);

                            if (dx >= 6) {
                                *(((uint16_t *) Y0) + 2) = *(((uint16_t *) rp) + 2);

                                if (dx & 1) {
                                    Y0[6] = rp[6];
                                }
                            } else if (dx & 1) {
                                Y0[4] = rp[4];
                            }
                        } else if (dx >= 2) {
                            *((uint16_t *) Y0) = *((uint16_t *) rp);

                            if (dx & 1) {
                                Y0[2] = rp[2];
                            }
                        } else {
                            *Y0 = *rp;
                        }

                        Y0 += MCU_W;
                    }
                    break;
                }
                case IMAGE_BPP_RGB565: {
                    if ((dx != MCU_W) || (dy != MCU_H)) { // partial MCU, fill with 0's to start
                        memset(Y0, 0, JPEG_444_YCBCR_MCU_SIZE);
                    }

                    for (int y = y_offset, yy = y + dy, index = 0; y < yy; y++) {
                        uint32_t *rp = (uint32_t *) (IMAGE_COMPUTE_RGB565_PIXEL_ROW_PTR(src, y) + x_offset);

                        for (int x = 0, xx = dx - 1; x < xx; x += 2, index += 2) {
                            int pixels = *rp++;
                            int r_pixels = ((pixels >> 8) & 0xf800f8) | ((pixels >> 13) & 0x70007);
                            int g_pixels = ((pixels >> 3) & 0xfc00fc) | ((pixels >> 9) & 0x30003);
                            int b_pixels = ((pixels << 3) & 0xf800f8) | ((pixels >> 2) & 0x70007);

                            int y = ((r_pixels * 38) + (g_pixels * 75) + (b_pixels * 15)) >> 7;
                            Y0[index] = y, Y0[index + 1] = y >> 16;

                            int u = (__SSUB16(b_pixels * 64, (r_pixels * 21) + (g_pixels * 43)) >> 7) ^ 0x800080;
                            CB[index] = u, CB[index + 1] = u >> 16;

                            int v = (__SSUB16(r_pixels * 64, (g_pixels * 54) + (b_pixels * 10)) >> 7) ^ 0x800080;
                            CR[index] = v, CR[index + 1] = v >> 16;
                        }

                        if (dx & 1) {
                            int pixel = *((uint16_t *) rp);
                            int r = COLOR_RGB565_TO_R8(pixel);
                            int g = COLOR_RGB565_TO_G8(pixel);
                            int b = COLOR_RGB565_TO_B8(pixel);
                            Y0[index] = COLOR_RGB888_TO_Y(r, g, b);
                            CB[index] = COLOR_RGB888_TO_U(r, g, b) - 128;
                            CR[index++] = COLOR_RGB888_TO_V(r, g, b) - 128;
                        }

                        index += MCU_W - dx;
                    }
                    break;
                }
                case IMAGE_BPP_BAYER: {
                    if ((dx != MCU_W) || (dy != MCU_H)) { // partial MCU, fill with 0's to start
                        memset(Y0, 0, JPEG_444_YCBCR_MCU_SIZE);
                    }

                    int src_w = src->w, w_limit = src_w - 1, w_limit_m_1 = w_limit - 1;
                    int src_h = src->h, h_limit = src_h - 1, h_limit_m_1 = h_limit - 1;

                    if (x_offset && y_offset && (x_offset < (src_w - MCU_W)) && (y_offset < (src_h - MCU_H))) {
                        for (int y = y_offset - 1, yy = y + MCU_H - 1, index_e = 0, index_o = MCU_W; y < yy; y += 2,
                                index_e += MCU_W,
                                index_o += MCU_W) {
                            uint8_t *rowptr_grgr_0 = src->data + (y * src_w);
                            uint8_t *rowptr_bgbg_1 = rowptr_grgr_0 + src_w;
                            uint8_t *rowptr_grgr_2 = rowptr_bgbg_1 + src_w;
                            uint8_t *rowptr_bgbg_3 = rowptr_grgr_2 + src_w;

                            for (int x = x_offset - 1, xx = x + MCU_W - 1; x < xx; x += 2, index_e += 2, index_o += 2) {
                                uint32_t row_grgr_0 = *((uint32_t *) (rowptr_grgr_0 + x));
                                uint32_t row_bgbg_1 = *((uint32_t *) (rowptr_bgbg_1 + x));
                                uint32_t row_grgr_2 = *((uint32_t *) (rowptr_grgr_2 + x));
                                uint32_t row_bgbg_3 = *((uint32_t *) (rowptr_bgbg_3 + x));

                                int row_01 = __UHADD8(row_grgr_0, row_grgr_2);
                                int row_1g = __UHADD8(row_bgbg_1, __PKHTB(row_bgbg_1, row_bgbg_1, 16));

                                int r_pixels_0 = __UXTB16(__UHADD8(row_01, __PKHTB(row_01, row_01, 16)));
                                int g_pixels_0 = __UXTB16(__UHADD8(row_1g, __PKHTB(row_1g, row_01, 8)));
                                int b_pixels_0 = __UXTB16_RORn(__UHADD8(row_bgbg_1, __PKHBT(row_bgbg_1, row_bgbg_1, 16)), 8);

                                int y0 = ((r_pixels_0 * 38) + (g_pixels_0 * 75) + (b_pixels_0 * 15)) >> 7;
                                Y0[index_e] = y0, Y0[index_e + 1] = y0 >> 16;

                                int u0 = (__SSUB16(b_pixels_0 * 64, (r_pixels_0 * 21) + (g_pixels_0 * 43)) >> 7) ^ 0x800080;
                                CB[index_e] = u0, CB[index_e + 1] = u0 >> 16;

                                int v0 = (__SSUB16(r_pixels_0 * 64, (g_pixels_0 * 54) + (b_pixels_0 * 10)) >> 7) ^ 0x800080;
                                CR[index_e] = v0, CR[index_e + 1] = v0 >> 16;

                                int row_13 = __UHADD8(row_bgbg_1, row_bgbg_3);
                                int row_2g = __UHADD8(row_grgr_2, __PKHBT(row_grgr_2, row_grgr_2, 16));

                                int r_pixels_1 = __UXTB16(__UHADD8(row_grgr_2, __PKHTB(row_grgr_2, row_grgr_2, 16)));
                                int g_pixels_1 = __UXTB16_RORn(__UHADD8(row_2g, __PKHBT(row_2g, row_13, 8)), 8);
                                int b_pixels_1 = __UXTB16_RORn(__UHADD8(row_13, __PKHBT(row_13, row_13, 16)), 8);

                                int y1 = ((r_pixels_1 * 38) + (g_pixels_1 * 75) + (b_pixels_1 * 15)) >> 7;
                                Y0[index_o] = y1, Y0[index_o + 1] = y1 >> 16;

                                int u1 = (__SSUB16(b_pixels_1 * 64, (r_pixels_1 * 21) + (g_pixels_1 * 43)) >> 7) ^ 0x800080;
                                CB[index_o] = u1, CB[index_o + 1] = u1 >> 16;

                                int v1 = (__SSUB16(r_pixels_1 * 64, (g_pixels_1 * 54) + (b_pixels_1 * 10)) >> 7) ^ 0x800080;
                                CR[index_o] = v1, CR[index_o + 1] = v1 >> 16;
                            }
                        }
                    } else {
                        // If dy is odd this loop will produce 1 extra boundary row in the MCU.
                        // This is okay given the boundary checking code below.
                        for (int y = y_offset, yy = y + dy, index_e = 0, index_o = MCU_W; y < yy; y += 2) {
                            uint8_t *rowptr_grgr_0, *rowptr_bgbg_1, *rowptr_grgr_2, *rowptr_bgbg_3;

                            // keep row pointers in bounds
                            if (y == 0) {
                                rowptr_bgbg_1 = src->data;
                                rowptr_grgr_2 = rowptr_bgbg_1 + ((src_h >= 2) ? src_w : 0);
                                rowptr_bgbg_3 = rowptr_bgbg_1 + ((src_h >= 3) ? (src_w * 2) : 0);
                                rowptr_grgr_0 = rowptr_grgr_2;
                            } else if (y == h_limit_m_1) {
                                rowptr_grgr_0 = src->data + ((y - 1) * src_w);
                                rowptr_bgbg_1 = rowptr_grgr_0 + src_w;
                                rowptr_grgr_2 = rowptr_bgbg_1 + src_w;
                                rowptr_bgbg_3 = rowptr_bgbg_1;
                            } else if (y >= h_limit) {
                                rowptr_grgr_0 = src->data + ((y - 1) * src_w);
                                rowptr_bgbg_1 = rowptr_grgr_0 + src_w;
                                rowptr_grgr_2 = rowptr_grgr_0;
                                rowptr_bgbg_3 = rowptr_bgbg_1;
                            } else { // get 4 neighboring rows
                                rowptr_grgr_0 = src->data + ((y - 1) * src_w);
                                rowptr_bgbg_1 = rowptr_grgr_0 + src_w;
                                rowptr_grgr_2 = rowptr_bgbg_1 + src_w;
                                rowptr_bgbg_3 = rowptr_grgr_2 + src_w;
                            }

                            // If dx is odd this loop will produce 1 extra boundary column in the MCU.
                            // This is okay given the boundary checking code below.
                            for (int x = x_offset, xx = x + dx; x < xx; x += 2, index_e += 2, index_o += 2) {
                                uint32_t row_grgr_0, row_bgbg_1, row_grgr_2, row_bgbg_3;

                                // keep pixels in bounds
                                if (x == 0) {
                                    if (src_w >= 4) {
                                        row_grgr_0 = *((uint32_t *) rowptr_grgr_0);
                                        row_bgbg_1 = *((uint32_t *) rowptr_bgbg_1);
                                        row_grgr_2 = *((uint32_t *) rowptr_grgr_2);
                                        row_bgbg_3 = *((uint32_t *) rowptr_bgbg_3);
                                    } else if (src_w >= 3) {
                                        row_grgr_0 = *((uint16_t *) rowptr_grgr_0) | (*(rowptr_grgr_0 + 2) << 16);
                                        row_bgbg_1 = *((uint16_t *) rowptr_bgbg_1) | (*(rowptr_bgbg_1 + 2) << 16);
                                        row_grgr_2 = *((uint16_t *) rowptr_grgr_2) | (*(rowptr_grgr_2 + 2) << 16);
                                        row_bgbg_3 = *((uint16_t *) rowptr_bgbg_3) | (*(rowptr_bgbg_3 + 2) << 16);
                                    } else if (src_w >= 2) {
                                        row_grgr_0 = *((uint16_t *) rowptr_grgr_0);
                                        row_grgr_0 = (row_grgr_0 << 16) | row_grgr_0;
                                        row_bgbg_1 = *((uint16_t *) rowptr_bgbg_1);
                                        row_bgbg_1 = (row_bgbg_1 << 16) | row_bgbg_1;
                                        row_grgr_2 = *((uint16_t *) rowptr_grgr_2);
                                        row_grgr_2 = (row_grgr_2 << 16) | row_grgr_2;
                                        row_bgbg_3 = *((uint16_t *) rowptr_bgbg_3);
                                        row_bgbg_3 = (row_bgbg_3 << 16) | row_bgbg_3;
                                    } else {
                                        row_grgr_0 = *(rowptr_grgr_0) * 0x01010101;
                                        row_bgbg_1 = *(rowptr_bgbg_1) * 0x01010101;
                                        row_grgr_2 = *(rowptr_grgr_2) * 0x01010101;
                                        row_bgbg_3 = *(rowptr_bgbg_3) * 0x01010101;
                                    }
                                    // The starting point needs to be offset by 1. The below patterns are actually
                                    // rgrg, gbgb, rgrg, and gbgb. So, shift left and backfill the missing border pixel.
                                    row_grgr_0 = (row_grgr_0 << 8) | __UXTB_RORn(row_grgr_0, 8);
                                    row_bgbg_1 = (row_bgbg_1 << 8) | __UXTB_RORn(row_bgbg_1, 8);
                                    row_grgr_2 = (row_grgr_2 << 8) | __UXTB_RORn(row_grgr_2, 8);
                                    row_bgbg_3 = (row_bgbg_3 << 8) | __UXTB_RORn(row_bgbg_3, 8);
                                } else if (x == w_limit_m_1) {
                                    row_grgr_0 = *((uint32_t *) (rowptr_grgr_0 + x - 2));
                                    row_grgr_0 = (row_grgr_0 >> 8) | ((row_grgr_0 << 8) & 0xff000000);
                                    row_bgbg_1 = *((uint32_t *) (rowptr_bgbg_1 + x - 2));
                                    row_bgbg_1 = (row_bgbg_1 >> 8) | ((row_bgbg_1 << 8) & 0xff000000);
                                    row_grgr_2 = *((uint32_t *) (rowptr_grgr_2 + x - 2));
                                    row_grgr_2 = (row_grgr_2 >> 8) | ((row_grgr_2 << 8) & 0xff000000);
                                    row_bgbg_3 = *((uint32_t *) (rowptr_bgbg_3 + x - 2));
                                    row_bgbg_3 = (row_bgbg_3 >> 8) | ((row_bgbg_1 << 8) & 0xff000000);
                                } else if (x >= w_limit) {
                                    row_grgr_0 = *((uint16_t *) (rowptr_grgr_0 + x - 1));
                                    row_grgr_0 = (row_grgr_0 << 16) | row_grgr_0;
                                    row_bgbg_1 = *((uint16_t *) (rowptr_bgbg_1 + x - 1));
                                    row_bgbg_1 = (row_bgbg_1 << 16) | row_bgbg_1;
                                    row_grgr_2 = *((uint16_t *) (rowptr_grgr_2 + x - 1));
                                    row_grgr_2 = (row_grgr_2 << 16) | row_grgr_2;
                                    row_bgbg_3 = *((uint16_t *) (rowptr_bgbg_3 + x - 1));
                                    row_bgbg_3 = (row_bgbg_3 << 16) | row_bgbg_3;
                                } else { // get 4 neighboring rows
                                    row_grgr_0 = *((uint32_t *) (rowptr_grgr_0 + x - 1));
                                    row_bgbg_1 = *((uint32_t *) (rowptr_bgbg_1 + x - 1));
                                    row_grgr_2 = *((uint32_t *) (rowptr_grgr_2 + x - 1));
                                    row_bgbg_3 = *((uint32_t *) (rowptr_bgbg_3 + x - 1));
                                }

                                int row_01 = __UHADD8(row_grgr_0, row_grgr_2);
                                int row_1g = __UHADD8(row_bgbg_1, __PKHTB(row_bgbg_1, row_bgbg_1, 16));

                                int r_pixels_0 = __UXTB16(__UHADD8(row_01, __PKHTB(row_01, row_01, 16)));
                                int g_pixels_0 = __UXTB16(__UHADD8(row_1g, __PKHTB(row_1g, row_01, 8)));
                                int b_pixels_0 = __UXTB16_RORn(__UHADD8(row_bgbg_1, __PKHBT(row_bgbg_1, row_bgbg_1, 16)), 8);

                                int y0 = ((r_pixels_0 * 38) + (g_pixels_0 * 75) + (b_pixels_0 * 15)) >> 7;
                                Y0[index_e] = y0, Y0[index_e + 1] = y0 >> 16;

                                int u0 = (__SSUB16(b_pixels_0 * 64, (r_pixels_0 * 21) + (g_pixels_0 * 43)) >> 7) ^ 0x800080;
                                CB[index_e] = u0, CB[index_e + 1] = u0 >> 16;

                                int v0 = (__SSUB16(r_pixels_0 * 64, (g_pixels_0 * 54) + (b_pixels_0 * 10)) >> 7) ^ 0x800080;
                                CR[index_e] = v0, CR[index_e + 1] = v0 >> 16;

                                int row_13 = __UHADD8(row_bgbg_1, row_bgbg_3);
                                int row_2g = __UHADD8(row_grgr_2, __PKHBT(row_grgr_2, row_grgr_2, 16));

                                int r_pixels_1 = __UXTB16(__UHADD8(row_grgr_2, __PKHTB(row_grgr_2, row_grgr_2, 16)));
                                int g_pixels_1 = __UXTB16_RORn(__UHADD8(row_2g, __PKHBT(row_2g, row_13, 8)), 8);
                                int b_pixels_1 = __UXTB16_RORn(__UHADD8(row_13, __PKHBT(row_13, row_13, 16)), 8);

                                int y1 = ((r_pixels_1 * 38) + (g_pixels_1 * 75) + (b_pixels_1 * 15)) >> 7;
                                Y0[index_o] = y1, Y0[index_o + 1] = y1 >> 16;

                                int u1 = (__SSUB16(b_pixels_1 * 64, (r_pixels_1 * 21) + (g_pixels_1 * 43)) >> 7) ^ 0x800080;
                                CB[index_o] = u1, CB[index_o + 1] = u1 >> 16;

                                int v1 = (__SSUB16(r_pixels_1 * 64, (g_pixels_1 * 54) + (b_pixels_1 * 10)) >> 7) ^ 0x800080;
                                CR[index_o] = v1, CR[index_o + 1] = v1 >> 16;
                            }

                            int inc = (MCU_W * 2) - (((dx + 1) / 2) * 2); // Handle boundary column.
                            index_e += inc;
                            index_o += inc;
                        }
                    }
                    break;
                }
            }
        }

        // Flush the MCU row for DMA...
        SCB_CleanDCache_by_Addr((uint32_t *) mcu_row_buffer_ptr, src_w_mcus_bytes);

        if (!y_offset) {
            // Invalidate the output buffer.
            SCB_InvalidateDCache_by_Addr(dma_buffer, OUTPUT_CHUNK_SIZE);
            // Start the DMA process off on the first row of MCUs.
            HAL_JPEG_Encode_DMA(&JPEG_Handle, mcu_row_buffer_ptr, src_w_mcus_bytes, dma_buffer, OUTPUT_CHUNK_SIZE);
        } else {

            // Wait for the last row MCUs to be processed before starting the next row.
            while (!JPEG_input_paused) {
                __WFI();

                if (JPEG_output_paused) {
                    memset(&JPEG_Config, 0, sizeof(JPEG_ConfTypeDef));
                    HAL_JPEG_Abort(&JPEG_Handle);
                    fb_free(); // mcu_row_buffer (after DMA is aborted)
                    return true; // overflow
                }
            }

            // Reset the lock.
            JPEG_input_paused = false;

            // Restart the DMA process on the next row of MCUs (that were already prepared).
            HAL_JPEG_ConfigInputBuffer(&JPEG_Handle, mcu_row_buffer_ptr, src_w_mcus_bytes);
            HAL_JPEG_Resume(&JPEG_Handle, JPEG_PAUSE_RESUME_INPUT);
        }
    }

    // After writing the last MCU to the JPEG core it will eventually generate an end-of-conversion
    // interrupt which will finish the JPEG encoding process and clear the busy flag.

    while (HAL_JPEG_GetState(&JPEG_Handle) == HAL_JPEG_STATE_BUSY_ENCODING) {
        __WFI();

        if (JPEG_output_paused) {
            memset(&JPEG_Config, 0, sizeof(JPEG_ConfTypeDef));
            HAL_JPEG_Abort(&JPEG_Handle);
            fb_free(); // mcu_row_buffer (after DMA is aborted)
            return true; // overflow
        }
    }

    fb_free(); // mcu_row_buffer

    // Set output size.
    dst->bpp = JPEG_out_data_length;

    // STM32H7 BUG FIX! The JPEG Encoder will ocassionally trigger the EOCF interrupt before writing
    // a final 0x000000D9 long into the output fifo as the end of the JPEG image. When this occurs
    // the output fifo will have a single 0 value in it after the encoding process finishes.
    if (__HAL_JPEG_GET_FLAG(&JPEG_Handle, JPEG_FLAG_OFNEF) && (!JPEG_Handle.Instance->DOR)) {
        // The encoding output process always aborts before writing OUTPUT_CHUNK_SIZE bytes
        // to the end of the dma_buffer. So, it is always safe to add one extra byte.
        dma_buffer[dst->bpp] = 0xD9;
        dst->bpp += sizeof(uint8_t);
    }

    // Update the JPEG image size by the new APP0 header and it's padding. However, we have to move
    // the SOI header to the front of the image first...
    dst->bpp += app0_total_size;
    memcpy(dst->data, dma_buffer, sizeof(uint16_t)); // move SOI
    memcpy(dst->data + sizeof(uint16_t), JPEG_APP0, sizeof(JPEG_APP0)); // inject APP0

    // Add on a comment header with 0 padding to ensure cache alignment after the APP0 header.
    *((uint16_t *) (dst->data + sizeof(uint16_t) + sizeof(JPEG_APP0))) = __REV16(app0_padding_size); // size
    memset(dst->data + sizeof(uint32_t) + sizeof(JPEG_APP0), 0, app0_padding_size - sizeof(uint16_t)); // data

    // Clean trailing data after 0xFFD9 at the end of the jpeg byte stream.
    dst->bpp = jpeg_clean_trailing_bytes(dst->bpp, dst->data);

#if (TIME_JPEG==1)
    printf("time: %u ms\n", mp_hal_ticks_ms() - start);
#endif

    return false;
}

void imlib_jpeg_compress_init()
{
    JPEG_Handle.Instance = JPEG;
    HAL_JPEG_Init(&JPEG_Handle);
    NVIC_SetPriority(JPEG_IRQn, IRQ_PRI_JPEG);
    HAL_NVIC_EnableIRQ(JPEG_IRQn);

    JPEG_MDMA_Handle_In.Instance                        = MDMA_Channel7; // in has a lower pri than out
    JPEG_MDMA_Handle_In.Init.Request                    = MDMA_REQUEST_JPEG_INFIFO_TH;
    JPEG_MDMA_Handle_In.Init.TransferTriggerMode        = MDMA_BUFFER_TRANSFER;
    JPEG_MDMA_Handle_In.Init.Priority                   = MDMA_PRIORITY_LOW;
    JPEG_MDMA_Handle_In.Init.Endianness                 = MDMA_LITTLE_ENDIANNESS_PRESERVE;
    JPEG_MDMA_Handle_In.Init.SourceInc                  = MDMA_SRC_INC_DOUBLEWORD;
    JPEG_MDMA_Handle_In.Init.DestinationInc             = MDMA_DEST_INC_DISABLE;
    JPEG_MDMA_Handle_In.Init.SourceDataSize             = MDMA_SRC_DATASIZE_DOUBLEWORD;
    JPEG_MDMA_Handle_In.Init.DestDataSize               = MDMA_DEST_DATASIZE_WORD;
    JPEG_MDMA_Handle_In.Init.DataAlignment              = MDMA_DATAALIGN_PACKENABLE;
    JPEG_MDMA_Handle_In.Init.BufferTransferLength       = JPEG_INPUT_FIFO_BYTES;
    JPEG_MDMA_Handle_In.Init.SourceBurst                = MDMA_SOURCE_BURST_4BEATS;
    JPEG_MDMA_Handle_In.Init.DestBurst                  = MDMA_DEST_BURST_8BEATS;
    JPEG_MDMA_Handle_In.Init.SourceBlockAddressOffset   = 0;
    JPEG_MDMA_Handle_In.Init.DestBlockAddressOffset     = 0;

    HAL_MDMA_Init(&JPEG_MDMA_Handle_In);
    __HAL_LINKDMA(&JPEG_Handle, hdmain, JPEG_MDMA_Handle_In);

    JPEG_MDMA_Handle_Out.Instance                       = MDMA_Channel6; // out has a higher pri than in
    JPEG_MDMA_Handle_Out.Init.Request                   = MDMA_REQUEST_JPEG_OUTFIFO_TH;
    JPEG_MDMA_Handle_Out.Init.TransferTriggerMode       = MDMA_BUFFER_TRANSFER;
    JPEG_MDMA_Handle_Out.Init.Priority                  = MDMA_PRIORITY_LOW;
    JPEG_MDMA_Handle_Out.Init.Endianness                = MDMA_LITTLE_ENDIANNESS_PRESERVE;
    JPEG_MDMA_Handle_Out.Init.SourceInc                 = MDMA_SRC_INC_DISABLE;
    JPEG_MDMA_Handle_Out.Init.DestinationInc            = MDMA_DEST_INC_DOUBLEWORD;
    JPEG_MDMA_Handle_Out.Init.SourceDataSize            = MDMA_SRC_DATASIZE_WORD;
    JPEG_MDMA_Handle_Out.Init.DestDataSize              = MDMA_DEST_DATASIZE_DOUBLEWORD;
    JPEG_MDMA_Handle_Out.Init.DataAlignment             = MDMA_DATAALIGN_PACKENABLE;
    JPEG_MDMA_Handle_Out.Init.BufferTransferLength      = JPEG_OUTPUT_FIFO_BYTES;
    JPEG_MDMA_Handle_Out.Init.SourceBurst               = MDMA_SOURCE_BURST_8BEATS;
    JPEG_MDMA_Handle_Out.Init.DestBurst                 = MDMA_DEST_BURST_4BEATS;
    JPEG_MDMA_Handle_Out.Init.SourceBlockAddressOffset  = 0;
    JPEG_MDMA_Handle_Out.Init.DestBlockAddressOffset    = 0;

    HAL_MDMA_Init(&JPEG_MDMA_Handle_Out);
    __HAL_LINKDMA(&JPEG_Handle, hdmaout, JPEG_MDMA_Handle_Out);
}

void imlib_jpeg_compress_deinit()
{
    memset(&JPEG_Config, 0, sizeof(JPEG_ConfTypeDef));
    HAL_JPEG_Abort(&JPEG_Handle);
    HAL_MDMA_DeInit(&JPEG_MDMA_Handle_Out);
    HAL_MDMA_DeInit(&JPEG_MDMA_Handle_In);
    HAL_NVIC_DisableIRQ(JPEG_IRQn);
    HAL_JPEG_DeInit(&JPEG_Handle);
}

#else

//
// Convert 8x8 Bayer source pixels directly into YCbCr for JPEG encoding
//
// Theory of operation:
// The Bayer pattern from the sensor looks like this:
// +---+---+---+---+---+---+
// | B | G | B | G | B | G |
// +---+---+---+---+---+---+
// | G |*R*|*G*| R | G | R | * = Example of current pair of pixels being processed
// +---+---+---+---+---+---+ Each iteration below will advance 2 pixels to the right
// | B | G | B | G | B | G |
// +---+---+---+---+---+---+
// | G | R | G | R | G | R |
// +---+---+---+---+---+---+
// Each of the color stimuli above is stored as 1 byte
// The slower algorithm above reads each byte around the current pixel individually to
// average the colors together to simulate the colors not present at the current pixel
// e.g. At location 0,0, only the blue value is present; red and green must be estimated from
// neighboring pixels
//
// The optimized algorithm below minimizes memory accesses by reading 2 bytes at a time
// and re-using the last pair as it progresses from left to right. Since the ARM CPU enforces a
// memory policy of generating an exception on unaligned reads, we read 16-bits at a time and
// OR them into a 32-bit variable to hold on to the pixels left and right of the current pair.
// This way we can work on 2 pixels at a time from 3 32-bit variables containing 3 lines of 4 pixels.
// The variables l0,l1,l2 hold the 4 pixels (left, current left, current_right, right)
// in lines above the current (l0), current (l1) and below (l2)
//
static void bayer_to_ycbcr(image_t *img, int x_offset, int y_offset, uint8_t *Y0, uint8_t *CB, uint8_t *CR, int bYUV)
{
    uint16_t *s;
    uint32_t l0, l1, l2; // current, prev and next lines of current pixel(s)
    uint8_t u8YDelta, u8UVDelta;
    int x, y, dy=8, idx, x_end, r, g, b;
    int pitch = img->w; // keep in local var
    int w2 = pitch/2; // pitch for a uint16_t pointer
    int prev_offset, next_offset;
    x_end = -1; // assume we don't need this
    if (bYUV) {
        u8YDelta = 0x80;
        u8UVDelta = 0x00;
    } else { // YCbCr
        u8YDelta = 0x00;
        u8UVDelta = 0x80;
    }
    if (x_offset == 0 || y_offset == 0 || x_offset + 8 >= img->w || y_offset + 8 >= img->h) { // slower bounds checking version
        if (y_offset+dy > img->h) // don't let it go beyond bottom line
            dy = img->h - y_offset;
        if (x_offset + 8 >= img->w) // right edge of Bayer data
            x_end = 6; // keep it from reading past right edge
        for (y=0, idx=0; y<dy; y++) {
            s = (uint16_t*)&img->pixels[(y_offset+y) * pitch + x_offset];
            prev_offset = -w2; next_offset = w2; // default values
            if (y+y_offset == 0) // top line, don't read the line below
                prev_offset = w2; // use the next line twice
            else if (y+y_offset == img->h-1) // bottom line
                next_offset = -w2; // use previous line twice
            // Prepare current pixels
            if (x_offset == 0) { // left edge, don't read beyond it
                l0 = s[prev_offset];
                l1 = s[0];
                l2 = s[next_offset];
                l0 |= (l0 << 16); // use them twice
                l1 |= (l1 << 16);
                l2 |= (l2 << 16); // since we're missing the actual ones
            } else { // the rest of the image is ok to read the -1 pixel
                l0 = *(uint32_t *)&s[prev_offset-1];
                l1 = *(uint32_t *)&s[-1];
                l2 = *(uint32_t *)&s[next_offset-1];
            }
            s++;
            if (y & 1) { // odd line
                for (x=0; x<8; x+=2, idx+=2) {
                    g = (l1 & 0xff0000) >> 16; // (0,0) green pixel
                    b = ((l0 & 0xff0000) + (l2 & 0xff0000)) >> 17;
                    r = (((l1 >> 8) & 0xff) + (l1 >> 24)) >> 1;
                    // faster to keep all calculations in integer math with 15-bit fractions
                    Y0[idx] = (uint8_t)(((r * 9770) + (g * 19182) + (b * 3736)) >> 15) - u8YDelta; // .299*r + .587*g + .114*b
                    CB[idx] = (uint8_t)(((b << 14) - (r * 5529) - (g * 10855)) >> 15) - u8UVDelta; // -0.168736*r + -0.331264*g + 0.5*b
                    CR[idx] = (uint8_t)(((r << 14) - (g * 13682) - (b * 2664)) >> 15) - u8UVDelta; // 0.5*r + -0.418688*g + -0.081312*b
                    l0 >>= 16; l1 >>= 16; l2 >>= 16; // L-CL-CR-R becomes L-CL-0-0
                    if (x == x_end) {
                        l0 |= (l0 << 16); l1 |= (l1 << 16); l2 |= (l2 << 16);
                    } else {
                        l0 |= (s[prev_offset] << 16); // grab 3 more pairs of pixels and put in upper 16-bits
                        l1 |= (s[0] << 16);
                        l2 |= (s[next_offset] << 16);
                    }
                    s++;
                    r = (l1 & 0xff00) >> 8; // (1, 0) red pixel
                    g = (((l1 >> 16) & 0xff) + (l1 & 0xff) + ((l0 >> 8) & 0xff) + ((l2 >> 8) & 0xff)) >> 2;
                    b = ((l0 & 0xff) + (l2 & 0xff) + ((l0 >> 16) & 0xff) + ((l2 >> 16) & 0xff)) >> 2;
                    Y0[idx+1] = (uint8_t)(((r * 9770) + (g * 19182) + (b * 3736)) >> 15) - u8YDelta; // .299*r + .587*g + .114*b
                    CB[idx+1] = (uint8_t)(((b << 14) - (r * 5529) - (g * 10855)) >> 15) - u8UVDelta; // -0.168736*r + -0.331264*g + 0.5*b
                    CR[idx+1] = (uint8_t)(((r << 14) - (g * 13682) - (b * 2664)) >> 15) - u8UVDelta; // 0.5*r + -0.418688*g + -0.081312*b
                } // for x
            } else { // even line
                for (x=0; x<8; x+=2, idx+=2) {
                    b = (l1 & 0xff0000) >> 16; // (0,0) blue pixel at current-right
                    g = (((l1 >> 8) & 0xff) + (l1 >> 24) + ((l0 >> 16) & 0xff) + ((l2 >> 16) & 0xff)) >> 2;
                    r = (((l0 >> 8) & 0xff) + (l0 >> 24) + ((l2 >> 8) & 0xff) + (l2 >> 24)) >> 2;
                    Y0[idx] = (uint8_t)(((r * 9770) + (g * 19182) + (b * 3736)) >> 15) - u8YDelta; // .299*r + .587*g + .114*b
                    CB[idx] = (uint8_t)(((b << 14) - (r * 5529) - (g * 10855)) >> 15) - u8UVDelta; // -0.168736*r + -0.331264*g + 0.5*b
                    CR[idx] = (uint8_t)(((r << 14) - (g * 13682) - (b * 2664)) >> 15) - u8UVDelta; // 0.5*r + -0.418688*g + -0.081312*b
                    // prepare for the next set of source pixels
                    l0 >>= 16; l1 >>= 16; l2 >>= 16; // L-CL-CR-R becomes L-CL-0-0
                    if (x == x_end) { // check for right edge
                        l0 |= (l0 << 16); l1 |= (l1 << 16); l2 |= (l2 << 16);
                    } else {
                        l0 |= (s[prev_offset] << 16); // grab 3 more pairs of pixels and put in upper 16-bits
                        l1 |= (s[0] << 16);
                        l2 |= (s[next_offset] << 16);
                    }
                    s++;
                    g = (l1 & 0xff00) >> 8; // (1, 0) green pixel
                    b = ((l1 & 0xff) + ((l1 >> 16) & 0xff)) >> 1;
                    r = ((l0 & 0xff00) + (l2 & 0xff00)) >> 9;
                    Y0[idx+1] = (uint8_t)(((r * 9770) + (g * 19182) + (b * 3736)) >> 15) - u8YDelta; // .299*r + .587*g + .114*b
                    CB[idx+1] = (uint8_t)(((b << 14) - (r * 5529) - (g * 10855)) >> 15) - u8UVDelta; // -0.168736*r + -0.331264*g + 0.5*b
                    CR[idx+1] = (uint8_t)(((r << 14) - (g * 13682) - (b * 2664)) >> 15) - u8UVDelta; // 0.5*r + -0.418688*g + -0.081312*b
                } // for x
            } // even line
        } // for y
    } else { // faster code without bounds checking
        for (y=0, idx=0; y<dy; y++) {
            s = (uint16_t*)&img->pixels[(y_offset+y) * pitch + x_offset];
            // Prepare current pixels
            l0 = *(uint32_t *)&s[-w2-1];
            l1 = *(uint32_t *)&s[-1];
            l2 = *(uint32_t *)&s[w2-1];
            s++;
            if (y & 1) { // odd line
                for (x=0; x<8; x+=2, idx+=2) {
                    g = (l1 & 0xff0000) >> 16; // (0,0) green pixel
                    b = ((l0 & 0xff0000) + (l2 & 0xff0000)) >> 17;
                    r = (((l1 >> 8) & 0xff) + (l1 >> 24)) >> 1;
                    // faster to keep all calculations in integer math with 15-bit fractions
                    Y0[idx] = (uint8_t)(((r * 9770) + (g * 19182) + (b * 3736)) >> 15) - u8YDelta; // .299*r + .587*g + .114*b
                    CB[idx] = (uint8_t)(((b << 14) - (r * 5529) - (g * 10855)) >> 15) - u8UVDelta; // -0.168736*r + -0.331264*g + 0.5*b
                    CR[idx] = (uint8_t)(((r << 14) - (g * 13682) - (b * 2664)) >> 15) - u8UVDelta; // 0.5*r + -0.418688*g + -0.081312*b
                    l0 = *(uint32_t *)&s[-w2-1];
                    l1 = *(uint32_t *)&s[-1];
                    l2 = *(uint32_t *)&s[w2-1];
                    s++;
                    r = (l1 & 0xff00) >> 8; // (1, 0) red pixel
                    g = (((l1 >> 16) & 0xff) + (l1 & 0xff) + ((l0 >> 8) & 0xff) + ((l2 >> 8) & 0xff)) >> 2;
                    b = ((l0 & 0xff) + (l2 & 0xff) + ((l0 >> 16) & 0xff) + ((l2 >> 16) & 0xff)) >> 2;
                    Y0[idx+1] = (uint8_t)(((r * 9770) + (g * 19182) + (b * 3736)) >> 15) - u8YDelta; // .299*r + .587*g + .114*b
                    CB[idx+1] = (uint8_t)(((b << 14) - (r * 5529) - (g * 10855)) >> 15) - u8UVDelta; // -0.168736*r + -0.331264*g + 0.5*b
                    CR[idx+1] = (uint8_t)(((r << 14) - (g * 13682) - (b * 2664)) >> 15) - u8UVDelta; // 0.5*r + -0.418688*g + -0.081312*b
                } // for x
            } else { // even line
                for (x=0; x<8; x+=2, idx+=2) {
                    b = (l1 & 0xff0000) >> 16; // (0,0) blue pixel at current-right
                    g = (((l1 >> 8) & 0xff) + (l1 >> 24) + ((l0 >> 16) & 0xff) + ((l2 >> 16) & 0xff)) >> 2;
                    r = (((l0 >> 8) & 0xff) + (l0 >> 24) + ((l2 >> 8) & 0xff) + (l2 >> 24)) >> 2;
                    Y0[idx] = (uint8_t)(((r * 9770) + (g * 19182) + (b * 3736)) >> 15) - u8YDelta; // .299*r + .587*g + .114*b
                    CB[idx] = (uint8_t)(((b << 14) - (r * 5529) - (g * 10855)) >> 15) - u8UVDelta; // -0.168736*r + -0.331264*g + 0.5*b
                    CR[idx] = (uint8_t)(((r << 14) - (g * 13682) - (b * 2664)) >> 15) - u8UVDelta; // 0.5*r + -0.418688*g + -0.081312*b
                    // load next set
                    l0 = *(uint32_t *)&s[-w2-1];
                    l1 = *(uint32_t *)&s[-1];
                    l2 = *(uint32_t *)&s[w2-1];
                    s++;
                    g = (l1 & 0xff00) >> 8; // (1, 0) green pixel
                    b = ((l1 & 0xff) + ((l1 >> 16) & 0xff)) >> 1;
                    r = ((l0 & 0xff00) + (l2 & 0xff00)) >> 9;
                    Y0[idx+1] = (uint8_t)(((r * 9770) + (g * 19182) + (b * 3736)) >> 15) - u8YDelta; // .299*r + .587*g + .114*b
                    CB[idx+1] = (uint8_t)(((b << 14) - (r * 5529) - (g * 10855)) >> 15) - u8UVDelta; // -0.168736*r + -0.331264*g + 0.5*b
                    CR[idx+1] = (uint8_t)(((r << 14) - (g * 13682) - (b * 2664)) >> 15) - u8UVDelta; // 0.5*r + -0.418688*g + -0.081312*b
                } // for x
            } // even line
        } // for y
    } // faster version
} /* bayer_to_ycbcr() */

// Software JPEG implementation.
#define FIX_0_382683433  ((int32_t)   98)
#define FIX_0_541196100  ((int32_t)  139)
#define FIX_0_707106781  ((int32_t)  181)
#define FIX_1_306562965  ((int32_t)  334)

#define DESCALE(x, y)   (x>>y)
#define MULTIPLY(x, y)  DESCALE((x) * (y), 8)

typedef struct {
    int idx;
    int length;
    uint8_t *buf;
    int bitc, bitb;
    bool realloc;
    bool overflow;
} jpeg_buf_t;

// Quantization tables
static float fdtbl_Y[64], fdtbl_UV[64];
static uint8_t YTable[64], UVTable[64];

static const uint8_t s_jpeg_ZigZag[] = {
    0,  1,   5,  6, 14, 15, 27, 28,
    2,  4,   7, 13, 16, 26, 29, 42,
    3,  8,  12, 17, 25, 30, 41, 43,
    9,  11, 18, 24, 31, 40, 44, 53,
    10, 19, 23, 32, 39, 45, 52, 54,
    20, 22, 33, 38, 46, 51, 55, 60,
    21, 34, 37, 47, 50, 56, 59, 61,
    35, 36, 48, 49, 57, 58, 62, 63
};

static const uint8_t YQT[] = {
    16, 11, 10, 16, 24,  40,  51,  61,
    12, 12, 14, 19, 26,  58,  60,  55,
    14, 13, 16, 24, 40,  57,  69,  56,
    14, 17, 22, 29, 51,  87,  80,  62,
    18, 22, 37, 56, 68,  109, 103, 77,
    24, 35, 55, 64, 81,  104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101,
    72, 92, 95, 98, 112, 100, 103, 99
};

static const uint8_t UVQT[] = {
    17,18,24,47,99,99,99,99,
    18,21,26,66,99,99,99,99,
    24,26,56,99,99,99,99,99,
    47,66,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99
};

static const float aasf[] = {
    1.0f, 1.387039845f, 1.306562965f, 1.175875602f,
    1.0f, 0.785694958f, 0.541196100f, 0.275899379f
};


static const uint8_t std_dc_luminance_nrcodes[] = {0,0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0};
static const uint8_t std_dc_luminance_values[] = {0,1,2,3,4,5,6,7,8,9,10,11};
static const uint8_t std_ac_luminance_nrcodes[] = {0,0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,0x7d};
static const uint8_t std_ac_luminance_values[] = {
    0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,0x61,0x07,0x22,0x71,0x14,0x32,0x81,0x91,0xa1,0x08,
    0x23,0x42,0xb1,0xc1,0x15,0x52,0xd1,0xf0,0x24,0x33,0x62,0x72,0x82,0x09,0x0a,0x16,0x17,0x18,0x19,0x1a,0x25,0x26,0x27,0x28,
    0x29,0x2a,0x34,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,
    0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x83,0x84,0x85,0x86,0x87,0x88,0x89,
    0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,
    0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe1,0xe2,
    0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,0xfa
};

static const uint8_t std_dc_chrominance_nrcodes[] = {0,0,3,1,1,1,1,1,1,1,1,1,0,0,0,0,0};
static const uint8_t std_dc_chrominance_values[] = {0,1,2,3,4,5,6,7,8,9,10,11};
static const uint8_t std_ac_chrominance_nrcodes[] = {0,0,2,1,2,4,4,3,4,7,5,4,4,0,1,2,0x77};
static const uint8_t std_ac_chrominance_values[] = {
    0x00,0x01,0x02,0x03,0x11,0x04,0x05,0x21,0x31,0x06,0x12,0x41,0x51,0x07,0x61,0x71,0x13,0x22,0x32,0x81,0x08,0x14,0x42,0x91,
    0xa1,0xb1,0xc1,0x09,0x23,0x33,0x52,0xf0,0x15,0x62,0x72,0xd1,0x0a,0x16,0x24,0x34,0xe1,0x25,0xf1,0x17,0x18,0x19,0x1a,0x26,
    0x27,0x28,0x29,0x2a,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,
    0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x82,0x83,0x84,0x85,0x86,0x87,
    0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,
    0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,
    0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,0xfa
};

// Huffman tables
static const uint16_t YDC_HT[12][2] = { {0,2},{2,3},{3,3},{4,3},{5,3},{6,3},{14,4},{30,5},{62,6},{126,7},{254,8},{510,9}};
static const uint16_t UVDC_HT[12][2] = { {0,2},{1,2},{2,2},{6,3},{14,4},{30,5},{62,6},{126,7},{254,8},{510,9},{1022,10},{2046,11}};
static const uint16_t YAC_HT[256][2] = {
    {0x000A, 0x0004},{0x0000, 0x0002},{0x0001, 0x0002},{0x0004, 0x0003},{0x000B, 0x0004},{0x001A, 0x0005},{0x0078, 0x0007},{0x00F8, 0x0008},
    {0x03F6, 0x000A},{0xFF82, 0x0010},{0xFF83, 0x0010},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},
    {0x0000, 0x0000},{0x000C, 0x0004},{0x001B, 0x0005},{0x0079, 0x0007},{0x01F6, 0x0009},{0x07F6, 0x000B},{0xFF84, 0x0010},{0xFF85, 0x0010},
    {0xFF86, 0x0010},{0xFF87, 0x0010},{0xFF88, 0x0010},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},
    {0x0000, 0x0000},{0x001C, 0x0005},{0x00F9, 0x0008},{0x03F7, 0x000A},{0x0FF4, 0x000C},{0xFF89, 0x0010},{0xFF8A, 0x0010},{0xFF8B, 0x0010},
    {0xFF8C, 0x0010},{0xFF8D, 0x0010},{0xFF8E, 0x0010},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},
    {0x0000, 0x0000},{0x003A, 0x0006},{0x01F7, 0x0009},{0x0FF5, 0x000C},{0xFF8F, 0x0010},{0xFF90, 0x0010},{0xFF91, 0x0010},{0xFF92, 0x0010},
    {0xFF93, 0x0010},{0xFF94, 0x0010},{0xFF95, 0x0010},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},
    {0x0000, 0x0000},{0x003B, 0x0006},{0x03F8, 0x000A},{0xFF96, 0x0010},{0xFF97, 0x0010},{0xFF98, 0x0010},{0xFF99, 0x0010},{0xFF9A, 0x0010},
    {0xFF9B, 0x0010},{0xFF9C, 0x0010},{0xFF9D, 0x0010},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},
    {0x0000, 0x0000},{0x007A, 0x0007},{0x07F7, 0x000B},{0xFF9E, 0x0010},{0xFF9F, 0x0010},{0xFFA0, 0x0010},{0xFFA1, 0x0010},{0xFFA2, 0x0010},
    {0xFFA3, 0x0010},{0xFFA4, 0x0010},{0xFFA5, 0x0010},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},
    {0x0000, 0x0000},{0x007B, 0x0007},{0x0FF6, 0x000C},{0xFFA6, 0x0010},{0xFFA7, 0x0010},{0xFFA8, 0x0010},{0xFFA9, 0x0010},{0xFFAA, 0x0010},
    {0xFFAB, 0x0010},{0xFFAC, 0x0010},{0xFFAD, 0x0010},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},
    {0x0000, 0x0000},{0x00FA, 0x0008},{0x0FF7, 0x000C},{0xFFAE, 0x0010},{0xFFAF, 0x0010},{0xFFB0, 0x0010},{0xFFB1, 0x0010},{0xFFB2, 0x0010},
    {0xFFB3, 0x0010},{0xFFB4, 0x0010},{0xFFB5, 0x0010},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},
    {0x0000, 0x0000},{0x01F8, 0x0009},{0x7FC0, 0x000F},{0xFFB6, 0x0010},{0xFFB7, 0x0010},{0xFFB8, 0x0010},{0xFFB9, 0x0010},{0xFFBA, 0x0010},
    {0xFFBB, 0x0010},{0xFFBC, 0x0010},{0xFFBD, 0x0010},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},
    {0x0000, 0x0000},{0x01F9, 0x0009},{0xFFBE, 0x0010},{0xFFBF, 0x0010},{0xFFC0, 0x0010},{0xFFC1, 0x0010},{0xFFC2, 0x0010},{0xFFC3, 0x0010},
    {0xFFC4, 0x0010},{0xFFC5, 0x0010},{0xFFC6, 0x0010},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},
    {0x0000, 0x0000},{0x01FA, 0x0009},{0xFFC7, 0x0010},{0xFFC8, 0x0010},{0xFFC9, 0x0010},{0xFFCA, 0x0010},{0xFFCB, 0x0010},{0xFFCC, 0x0010},
    {0xFFCD, 0x0010},{0xFFCE, 0x0010},{0xFFCF, 0x0010},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},
    {0x0000, 0x0000},{0x03F9, 0x000A},{0xFFD0, 0x0010},{0xFFD1, 0x0010},{0xFFD2, 0x0010},{0xFFD3, 0x0010},{0xFFD4, 0x0010},{0xFFD5, 0x0010},
    {0xFFD6, 0x0010},{0xFFD7, 0x0010},{0xFFD8, 0x0010},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},
    {0x0000, 0x0000},{0x03FA, 0x000A},{0xFFD9, 0x0010},{0xFFDA, 0x0010},{0xFFDB, 0x0010},{0xFFDC, 0x0010},{0xFFDD, 0x0010},{0xFFDE, 0x0010},
    {0xFFDF, 0x0010},{0xFFE0, 0x0010},{0xFFE1, 0x0010},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},
    {0x0000, 0x0000},{0x07F8, 0x000B},{0xFFE2, 0x0010},{0xFFE3, 0x0010},{0xFFE4, 0x0010},{0xFFE5, 0x0010},{0xFFE6, 0x0010},{0xFFE7, 0x0010},
    {0xFFE8, 0x0010},{0xFFE9, 0x0010},{0xFFEA, 0x0010},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},
    {0x0000, 0x0000},{0xFFEB, 0x0010},{0xFFEC, 0x0010},{0xFFED, 0x0010},{0xFFEE, 0x0010},{0xFFEF, 0x0010},{0xFFF0, 0x0010},{0xFFF1, 0x0010},
    {0xFFF2, 0x0010},{0xFFF3, 0x0010},{0xFFF4, 0x0010},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},
    {0x07F9, 0x000B},{0xFFF5, 0x0010},{0xFFF6, 0x0010},{0xFFF7, 0x0010},{0xFFF8, 0x0010},{0xFFF9, 0x0010},{0xFFFA, 0x0010},{0xFFFB, 0x0010},
    {0xFFFC, 0x0010},{0xFFFD, 0x0010},{0xFFFE, 0x0010},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},
};

static const uint16_t UVAC_HT[256][2] = {
    {0x0000, 0x0002},{0x0001, 0x0002},{0x0004, 0x0003},{0x000A, 0x0004},{0x0018, 0x0005},{0x0019, 0x0005},{0x0038, 0x0006},{0x0078, 0x0007},
    {0x01F4, 0x0009},{0x03F6, 0x000A},{0x0FF4, 0x000C},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},
    {0x0000, 0x0000},{0x000B, 0x0004},{0x0039, 0x0006},{0x00F6, 0x0008},{0x01F5, 0x0009},{0x07F6, 0x000B},{0x0FF5, 0x000C},{0xFF88, 0x0010},
    {0xFF89, 0x0010},{0xFF8A, 0x0010},{0xFF8B, 0x0010},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},
    {0x0000, 0x0000},{0x001A, 0x0005},{0x00F7, 0x0008},{0x03F7, 0x000A},{0x0FF6, 0x000C},{0x7FC2, 0x000F},{0xFF8C, 0x0010},{0xFF8D, 0x0010},
    {0xFF8E, 0x0010},{0xFF8F, 0x0010},{0xFF90, 0x0010},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},
    {0x0000, 0x0000},{0x001B, 0x0005},{0x00F8, 0x0008},{0x03F8, 0x000A},{0x0FF7, 0x000C},{0xFF91, 0x0010},{0xFF92, 0x0010},{0xFF93, 0x0010},
    {0xFF94, 0x0010},{0xFF95, 0x0010},{0xFF96, 0x0010},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},
    {0x0000, 0x0000},{0x003A, 0x0006},{0x01F6, 0x0009},{0xFF97, 0x0010},{0xFF98, 0x0010},{0xFF99, 0x0010},{0xFF9A, 0x0010},{0xFF9B, 0x0010},
    {0xFF9C, 0x0010},{0xFF9D, 0x0010},{0xFF9E, 0x0010},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},
    {0x0000, 0x0000},{0x003B, 0x0006},{0x03F9, 0x000A},{0xFF9F, 0x0010},{0xFFA0, 0x0010},{0xFFA1, 0x0010},{0xFFA2, 0x0010},{0xFFA3, 0x0010},
    {0xFFA4, 0x0010},{0xFFA5, 0x0010},{0xFFA6, 0x0010},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},
    {0x0000, 0x0000},{0x0079, 0x0007},{0x07F7, 0x000B},{0xFFA7, 0x0010},{0xFFA8, 0x0010},{0xFFA9, 0x0010},{0xFFAA, 0x0010},{0xFFAB, 0x0010},
    {0xFFAC, 0x0010},{0xFFAD, 0x0010},{0xFFAE, 0x0010},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},
    {0x0000, 0x0000},{0x007A, 0x0007},{0x07F8, 0x000B},{0xFFAF, 0x0010},{0xFFB0, 0x0010},{0xFFB1, 0x0010},{0xFFB2, 0x0010},{0xFFB3, 0x0010},
    {0xFFB4, 0x0010},{0xFFB5, 0x0010},{0xFFB6, 0x0010},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},
    {0x0000, 0x0000},{0x00F9, 0x0008},{0xFFB7, 0x0010},{0xFFB8, 0x0010},{0xFFB9, 0x0010},{0xFFBA, 0x0010},{0xFFBB, 0x0010},{0xFFBC, 0x0010},
    {0xFFBD, 0x0010},{0xFFBE, 0x0010},{0xFFBF, 0x0010},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},
    {0x0000, 0x0000},{0x01F7, 0x0009},{0xFFC0, 0x0010},{0xFFC1, 0x0010},{0xFFC2, 0x0010},{0xFFC3, 0x0010},{0xFFC4, 0x0010},{0xFFC5, 0x0010},
    {0xFFC6, 0x0010},{0xFFC7, 0x0010},{0xFFC8, 0x0010},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},
    {0x0000, 0x0000},{0x01F8, 0x0009},{0xFFC9, 0x0010},{0xFFCA, 0x0010},{0xFFCB, 0x0010},{0xFFCC, 0x0010},{0xFFCD, 0x0010},{0xFFCE, 0x0010},
    {0xFFCF, 0x0010},{0xFFD0, 0x0010},{0xFFD1, 0x0010},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},
    {0x0000, 0x0000},{0x01F9, 0x0009},{0xFFD2, 0x0010},{0xFFD3, 0x0010},{0xFFD4, 0x0010},{0xFFD5, 0x0010},{0xFFD6, 0x0010},{0xFFD7, 0x0010},
    {0xFFD8, 0x0010},{0xFFD9, 0x0010},{0xFFDA, 0x0010},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},
    {0x0000, 0x0000},{0x01FA, 0x0009},{0xFFDB, 0x0010},{0xFFDC, 0x0010},{0xFFDD, 0x0010},{0xFFDE, 0x0010},{0xFFDF, 0x0010},{0xFFE0, 0x0010},
    {0xFFE1, 0x0010},{0xFFE2, 0x0010},{0xFFE3, 0x0010},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},
    {0x0000, 0x0000},{0x07F9, 0x000B},{0xFFE4, 0x0010},{0xFFE5, 0x0010},{0xFFE6, 0x0010},{0xFFE7, 0x0010},{0xFFE8, 0x0010},{0xFFE9, 0x0010},
    {0xFFEA, 0x0010},{0xFFEB, 0x0010},{0xFFEC, 0x0010},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},
    {0x0000, 0x0000},{0x3FE0, 0x000E},{0xFFED, 0x0010},{0xFFEE, 0x0010},{0xFFEF, 0x0010},{0xFFF0, 0x0010},{0xFFF1, 0x0010},{0xFFF2, 0x0010},
    {0xFFF3, 0x0010},{0xFFF4, 0x0010},{0xFFF5, 0x0010},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},
    {0x03FA, 0x000A},{0x7FC3, 0x000F},{0xFFF6, 0x0010},{0xFFF7, 0x0010},{0xFFF8, 0x0010},{0xFFF9, 0x0010},{0xFFFA, 0x0010},{0xFFFB, 0x0010},
    {0xFFFC, 0x0010},{0xFFFD, 0x0010},{0xFFFE, 0x0010},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},{0x0000, 0x0000},
};

static void jpeg_put_char(jpeg_buf_t *jpeg_buf, char c)
{
    if ((jpeg_buf->idx+1) >= jpeg_buf->length) {
        if (jpeg_buf->realloc == false) {
            // Can't realloc buffer
            jpeg_buf->overflow = true;
            return;
        }
        jpeg_buf->length += 1024;
        jpeg_buf->buf = xrealloc(jpeg_buf->buf, jpeg_buf->length);
    }

    jpeg_buf->buf[jpeg_buf->idx++]=c;
}

static void jpeg_put_bytes(jpeg_buf_t *jpeg_buf, const void *data, int size)
{
    if ((jpeg_buf->idx+size) >= jpeg_buf->length) {
        if (jpeg_buf->realloc == false) {
            // Can't realloc buffer
            jpeg_buf->overflow = true;
            return;
        }
        jpeg_buf->length += 1024;
        jpeg_buf->buf = xrealloc(jpeg_buf->buf, jpeg_buf->length);
    }

    memcpy(jpeg_buf->buf+jpeg_buf->idx, data, size);
    jpeg_buf->idx += size;
}

static void jpeg_writeBits(jpeg_buf_t *jpeg_buf, const uint16_t *bs)
{
    jpeg_buf->bitc += bs[1];
    jpeg_buf->bitb |= bs[0] << (24 - jpeg_buf->bitc);

    while (jpeg_buf->bitc > 7) {
        uint8_t c = (jpeg_buf->bitb >> 16) & 255;
        jpeg_put_char(jpeg_buf, c);
        if(c == 255) {
            jpeg_put_char(jpeg_buf, 0);
        }
        jpeg_buf->bitb <<= 8;
        jpeg_buf->bitc -= 8;
    }
}

//Huffman-encoded magnitude value
static void jpeg_calcBits(int val, uint16_t bits[2]) {
    int t1=val;
    if (val<0) {
        t1 = -val;
        val = val-1;
    }
    bits[1] = 32-__CLZ(t1);
    bits[0] = val & ((1<<bits[1])-1);
}

static int jpeg_processDU(jpeg_buf_t *jpeg_buf, int8_t *CDU, float *fdtbl, int DC, const uint16_t (*HTDC)[2], const uint16_t (*HTAC)[2])
{
    int DU[64];
    int DUQ[64];
    int z1, z2, z3, z4, z5, z11, z13;
    int t0, t1, t2, t3, t4, t5, t6, t7, t10, t11, t12, t13;
    const uint16_t EOB[2] = { HTAC[0x00][0], HTAC[0x00][1] };
    const uint16_t M16zeroes[2] = { HTAC[0xF0][0], HTAC[0xF0][1] };

    // DCT rows
    for (int i=8, *p=DU; i>0; i--, p+=8, CDU+=8) {
        t0 = CDU[0] + CDU[7];
        t1 = CDU[1] + CDU[6];
        t2 = CDU[2] + CDU[5];
        t3 = CDU[3] + CDU[4];

        t7 = CDU[0] - CDU[7];
        t6 = CDU[1] - CDU[6];
        t5 = CDU[2] - CDU[5];
        t4 = CDU[3] - CDU[4];

        // Even part
        t10 = t0 + t3;
        t13 = t0 - t3;
        t11 = t1 + t2;
        t12 = t1 - t2;
        z1 = MULTIPLY(t12 + t13, FIX_0_707106781); // c4

        p[0] = t10 + t11;
        p[4] = t10 - t11;
        p[2] = t13 + z1;
        p[6] = t13 - z1;

        // Odd part
        t10 = t4 + t5;// phase 2
        t11 = t5 + t6;
        t12 = t6 + t7;

        // The rotator is modified from fig 4-8 to avoid extra negations.
        z5 = MULTIPLY(t10 - t12, FIX_0_382683433); // c6
        z2 = MULTIPLY(t10, FIX_0_541196100) + z5; // 1.306562965f-c6
        z4 = MULTIPLY(t12, FIX_1_306562965) + z5; // 1.306562965f+c6
        z3 = MULTIPLY(t11, FIX_0_707106781); // c4
        z11 = t7 + z3;    // phase 5
        z13 = t7 - z3;

        p[5] = z13 + z2;// phase 6
        p[3] = z13 - z2;
        p[1] = z11 + z4;
        p[7] = z11 - z4;
    }

    // DCT columns
    for (int i=8, *p=DU; i>0; i--, p++) {
        t0 = p[0]  + p[56];
        t1 = p[8]  + p[48];
        t2 = p[16] + p[40];
        t3 = p[24] + p[32];

        t7 = p[0]  - p[56];
        t6 = p[8]  - p[48];
        t5 = p[16] - p[40];
        t4 = p[24] - p[32];

        // Even part
        t10 = t0 + t3;	// phase 2
        t13 = t0 - t3;
        t11 = t1 + t2;
        t12 = t1 - t2;
        z1 = MULTIPLY(t12 + t13, FIX_0_707106781); // c4

        p[0] = t10 + t11; 		// phase 3
        p[32] = t10 - t11;
        p[16] = t13 + z1; 		// phase 5
        p[48] = t13 - z1;

        // Odd part
        t10 = t4 + t5; 		// phase 2
        t11 = t5 + t6;
        t12 = t6 + t7;

        // The rotator is modified from fig 4-8 to avoid extra negations.
        z5 = MULTIPLY(t10 - t12, FIX_0_382683433); // c6
        z2 = MULTIPLY(t10, FIX_0_541196100) + z5; // 1.306562965f-c6
        z4 = MULTIPLY(t12, FIX_1_306562965) + z5; // 1.306562965f+c6
        z3 = MULTIPLY(t11, FIX_0_707106781); // c4
        z11 = t7 + z3;		// phase 5
        z13 = t7 - z3;

        p[40] = z13 + z2;// phase 6
        p[24] = z13 - z2;
        p[8] = z11 + z4;
        p[56] = z11 - z4;
    }

    // first non-zero element in reverse order
    int end0pos = 0;
    // Quantize/descale/zigzag the coefficients
    for(int i=0; i<64; ++i) {
		DUQ[s_jpeg_ZigZag[i]] = fast_roundf(DU[i]*fdtbl[i]);
        if (s_jpeg_ZigZag[i] > end0pos && DUQ[s_jpeg_ZigZag[i]]) {
            end0pos = s_jpeg_ZigZag[i];
        }
    }

    // Encode DC
    int diff = DUQ[0] - DC;
    if (diff == 0) {
        jpeg_writeBits(jpeg_buf, HTDC[0]);
    } else {
        uint16_t bits[2];
        jpeg_calcBits(diff, bits);
        jpeg_writeBits(jpeg_buf, HTDC[bits[1]]);
        jpeg_writeBits(jpeg_buf, bits);
    }

    // Encode ACs
    if(end0pos == 0) {
        jpeg_writeBits(jpeg_buf, EOB);
        return DUQ[0];
    }

    for(int i = 1; i <= end0pos; ++i) {
        int startpos = i;
        for (; DUQ[i]==0 && i<=end0pos ; ++i) {
        }
        int nrzeroes = i-startpos;
        if ( nrzeroes >= 16 ) {
            int lng = nrzeroes>>4;
            for (int nrmarker=1; nrmarker <= lng; ++nrmarker)
                jpeg_writeBits(jpeg_buf, M16zeroes);
            nrzeroes &= 15;
        }
        uint16_t bits[2];
        jpeg_calcBits(DUQ[i], bits);
        jpeg_writeBits(jpeg_buf, HTAC[(nrzeroes<<4)+bits[1]]);
        jpeg_writeBits(jpeg_buf, bits);
    }
    if(end0pos != 63) {
        jpeg_writeBits(jpeg_buf, EOB);
    }
    return DUQ[0];
}

static void jpeg_init(int quality)
{
    static int q =0;

    quality = quality < 50 ? 5000 / quality : 200 - quality * 2;

    // If quality changed, update quantization matrix
    if (q != quality) {
        q = quality;
        for(int i = 0; i < 64; ++i) {
            int yti = (YQT[i]*quality+50)/100;
            YTable[s_jpeg_ZigZag[i]] = yti < 1 ? 1 : yti > 255 ? 255 : yti;
            int uvti  = (UVQT[i]*quality+50)/100;
            UVTable[s_jpeg_ZigZag[i]] = uvti < 1 ? 1 : uvti > 255 ? 255 : uvti;
        }

        for(int r = 0, k = 0; r < 8; ++r) {
            for(int c = 0; c < 8; ++c, ++k) {
                fdtbl_Y[k]  = 1.0f / (aasf[r] * aasf[c] * YTable [s_jpeg_ZigZag[k]] * 8.0f);
                fdtbl_UV[k] = 1.0f / (aasf[r] * aasf[c] * UVTable[s_jpeg_ZigZag[k]] * 8.0f);
            }
        }
    }
}

static void jpeg_write_headers(jpeg_buf_t *jpeg_buf, int w, int h, int bpp, jpeg_subsample_t jpeg_subsample)
{
    // Number of components (1 or 3)
    uint8_t nr_comp = (bpp == 1)? 1 : 3;

    // JPEG headers
    uint8_t m_soi[] = {
        0xFF, 0xD8          // SOI
    };

    uint8_t m_app0[] =  {
        0xFF, 0xE0,         // APP0
        0x00, 0x10,  'J',  'F',  'I',  'F', 0x00, 0x01,
        0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00
    };

    uint8_t m_dqt[] = {
        0xFF, 0xDB,         // DQT
        (bpp*65+2)>>8,      // Header length MSB
        (bpp*65+2)&0xFF,    // Header length LSB
    };

    uint8_t m_sof0[] = {
        0xFF, 0xC0,         // SOF0
        (nr_comp*3+8)>>8,   // Header length MSB
        (nr_comp*3+8)&0xFF, // Header length LSB
        0x08,               // Bits per sample
        h>>8, h&0xFF,       // Height
        w>>8, w&0xFF,       // Width
        nr_comp,            // Number of components
    };

    uint8_t m_dht[] = {
        0xFF, 0xC4,         // DHT
        (bpp*208+2)>>8,     // Header length MSB
        (bpp*208+2)&0xFF,   // Header length LSB
    };

    uint8_t m_sos[] = {
        0xFF, 0xDA,         // SOS
        (nr_comp*2+6)>>8,   // Header length MSB
        (nr_comp*2+6)&0xFF, // Header length LSB
        nr_comp,            // Number of components
    };

    // Write SOI marker
    jpeg_put_bytes(jpeg_buf, m_soi, sizeof(m_soi));
    // Write APP0 marker
    jpeg_put_bytes(jpeg_buf, m_app0, sizeof(m_app0));

    // Write DQT marker
    jpeg_put_bytes(jpeg_buf, m_dqt, sizeof(m_dqt));
    // Write Y quantization table (index, table)
    jpeg_put_char (jpeg_buf, 0);
    jpeg_put_bytes(jpeg_buf, YTable, sizeof(YTable));

    if (bpp > 1) {
        // Write UV quantization table (index, table)
        jpeg_put_char (jpeg_buf, 1);
        jpeg_put_bytes(jpeg_buf, UVTable, sizeof(UVTable));
    }

    // Write SOF0 marker
    jpeg_put_bytes(jpeg_buf, m_sof0, sizeof(m_sof0));
    for (int i=0; i<nr_comp; i++) {
        // Component ID, HV sampling, q table idx
        jpeg_put_bytes(jpeg_buf, (uint8_t [3]){i+1, (i==0 && bpp==2)? jpeg_subsample:0x11, (i>0)}, 3);

    }

    // Write DHT marker
    jpeg_put_bytes(jpeg_buf, m_dht, sizeof(m_dht));

    // Write DHT-YDC
    jpeg_put_char (jpeg_buf, 0x00);
    jpeg_put_bytes(jpeg_buf, std_dc_luminance_nrcodes+1, sizeof(std_dc_luminance_nrcodes)-1);
    jpeg_put_bytes(jpeg_buf, std_dc_luminance_values, sizeof(std_dc_luminance_values));

    // Write DHT-YAC
    jpeg_put_char (jpeg_buf, 0x10);
    jpeg_put_bytes(jpeg_buf, std_ac_luminance_nrcodes+1, sizeof(std_ac_luminance_nrcodes)-1);
    jpeg_put_bytes(jpeg_buf, std_ac_luminance_values, sizeof(std_ac_luminance_values));

    if (bpp > 1) {
        // Write DHT-UDC
        jpeg_put_char (jpeg_buf, 0x01);
        jpeg_put_bytes(jpeg_buf, std_dc_chrominance_nrcodes+1, sizeof(std_dc_chrominance_nrcodes)-1);
        jpeg_put_bytes(jpeg_buf, std_dc_chrominance_values, sizeof(std_dc_chrominance_values));

        // Write DHT-UAC
        jpeg_put_char (jpeg_buf, 0x11);
        jpeg_put_bytes(jpeg_buf, std_ac_chrominance_nrcodes+1, sizeof(std_ac_chrominance_nrcodes)-1);
        jpeg_put_bytes(jpeg_buf, std_ac_chrominance_values, sizeof(std_ac_chrominance_values));
    }

    // Write SOS marker
    jpeg_put_bytes(jpeg_buf, m_sos, sizeof(m_sos));
    for (int i=0; i<nr_comp; i++) {
        jpeg_put_bytes(jpeg_buf, (uint8_t [2]){i+1, (i==0)? 0x00:0x11}, 2);
    }

    // Spectral selection
    jpeg_put_bytes(jpeg_buf, (uint8_t [3]){0x00, 0x3F, 0x0}, 3);
}

void jpeg_get_mcu(image_t *img, int mcu_w, int mcu_h, int x_offs, int y_offs, int bpp, void *buf)
{
    switch (bpp) {
        case 0: {
            uint8_t *mcu = (uint8_t*) buf;
            if (y_offs+mcu_h > img->h || x_offs+mcu_w > img->w) { // clipped
                for (int y=y_offs; y<y_offs+mcu_h; y++) {
                    for (int x=x_offs; x<x_offs+mcu_w; x++) {
                        if (x >= img->w || y >= img->h) {
                            *mcu++ = 0;
                        } else {
                            *mcu++ = COLOR_BINARY_TO_GRAYSCALE(IMAGE_GET_BINARY_PIXEL(img, x, y)) - 128;
                        }
                    }
                }
            } // clipped
            else {
                int iPitch = ((img->w + 31) >> 3) & 0xfffc; // dword align
                uint8_t u8Pixels;
                uint32_t *d32 = (uint32_t *)mcu;
                for (int y=y_offs; y<(y_offs + 8); y++) {
                    // read 8 binary pixels in one shot
                    int index = (y * iPitch) + (x_offs>>3); // get byte offset
                    uint8_t *s = &img->data[index];
                    u8Pixels = s[0]; // get 8 binary pixels (1 byte)
                    *d32++ = u32Expand[u8Pixels & 0xf] ^ 0x80808080; // first 4 pixels
                    *d32++ = u32Expand[u8Pixels >> 4] ^ 0x80808080;  // second 4 pixels
                } // for y
            } // not clipped
            break;
        }
        case 1: {
            uint8_t *mcu = (uint8_t*) buf;
            //memset(mcu, 0, 64);
            if (y_offs+mcu_h > img->h || x_offs+mcu_w > img->w) { // truncated MCU
                for (int y=y_offs; y<y_offs+mcu_h; y++) {
                    for (int x=x_offs; x<x_offs+mcu_w; x++) {
                        if (x >= img->w || y >= img->h) {
                            *mcu++ = 0;
                        } else {
                            *mcu++ = IMAGE_GET_GRAYSCALE_PIXEL(img, x, y) - 128;
                        }
                    }
                }
            } // needs to be clipped
            else // no need to check bounds per pixel
            {
                uint32_t *mcu32 = (uint32_t *)mcu;
                for (int y=y_offs; y<y_offs+mcu_h; y++) {
                    uint32_t *pRow = (uint32_t *)&img->data[(y * img->w) + x_offs];
                    mcu32[0] = pRow[0] ^ 0x80808080; // do 4 pixels at a time and "subtract" 128
                    mcu32[1] = pRow[1] ^ 0x80808080;
                    mcu32 += 2;
                }
            }
            break;
        }
        case 2: {
            uint16_t *mcu = (uint16_t*) buf;
            for (int y=y_offs; y<y_offs+mcu_h; y++) {
                for (int x=x_offs; x<x_offs+mcu_w; x++) {
                    if (x >= img->w || y >= img->h) {
                        *mcu++ = 0;
                    } else {
                        *mcu++ = IMAGE_GET_RGB565_PIXEL(img, x, y);
                    }
                }
            }
            break;
        }
        default:
            break;
    }
}

bool jpeg_compress(image_t *src, image_t *dst, int quality, bool realloc)
{
    int DCY=0, DCU=0, DCV=0;

    #if (TIME_JPEG==1)
    uint32_t start = mp_hal_ticks_ms();
    #endif

    if (!dst->data) {
        dst->data = fb_alloc_all((uint32_t *) &dst->bpp, FB_ALLOC_PREFER_SIZE | FB_ALLOC_CACHE_ALIGN);
    }

    // JPEG buffer
    jpeg_buf_t  jpeg_buf = {
        .idx =0,
        .buf = dst->pixels,
        .length = dst->bpp,
        .bitc = 0,
        .bitb = 0,
        .realloc = realloc,
        .overflow = false,
    };

    // Initialize quantization tables
    jpeg_init(quality);

    jpeg_subsample_t jpeg_subsample;


    if (quality >= 60) {
        jpeg_subsample = JPEG_SUBSAMPLE_1x1;
    } else if (quality > 35) {
        jpeg_subsample = JPEG_SUBSAMPLE_2x1;
    } else { // <= 35
        jpeg_subsample = JPEG_SUBSAMPLE_2x2;
    }

    // Write JPEG headers
    if (src->bpp == 3) { // BAYER
        // Will be converted to RGB565
        jpeg_write_headers(&jpeg_buf, src->w, src->h, 2, jpeg_subsample);
    } else {
        jpeg_write_headers(&jpeg_buf, src->w, src->h, (src->bpp == 0) ? 1 : src->bpp, jpeg_subsample);
    }

    // Encode 8x8 macroblocks
    if (src->bpp == 0) {
        int8_t YDU[64];
        // Copy 8x8 MCUs
        for (int y=0; y<src->h; y+=8) {
            for (int x=0; x<src->w; x+=8) {
                jpeg_get_mcu(src, 8, 8, x, y, src->bpp, YDU);
                DCY = jpeg_processDU(&jpeg_buf, YDU, fdtbl_Y, DCY, YDC_HT, YAC_HT);
            }
            if (jpeg_buf.overflow) {
                goto jpeg_overflow;
            }
        }
    } else if (src->bpp == 1) {
        int8_t YDU[64];
        // Copy 8x8 MCUs
        for (int y=0; y<src->h; y+=8) {
            for (int x=0; x<src->w; x+=8) {
                jpeg_get_mcu(src, 8, 8, x, y, src->bpp, YDU);
                DCY = jpeg_processDU(&jpeg_buf, YDU, fdtbl_Y, DCY, YDC_HT, YAC_HT);
            }
            if (jpeg_buf.overflow) {
                goto jpeg_overflow;
            }
        }
    } else if (src->bpp == 2) {// TODO assuming RGB565
        switch (jpeg_subsample) {
            case JPEG_SUBSAMPLE_1x1: {
                uint16_t pixel, *pRow;;
                int dx, dy;
                int r, g, b; // to separate RGB565 into R8,G8,B8
                int8_t YDU[64], UDU[64], VDU[64];
                int8_t *pY, *pU, *pV;
                for (int y=0; y<src->h; y+=8) {
                    dy = 8;
                    if (y+8 > src->h) // over bottom edge
                        dy = src->h - y;
                    for (int x=0; x<src->w; x+=8) {
                        dx = 8;
                        if (x+8 > src->w) // over right edge, reduce capture size
                            dx = src->w - x;
                        if (dx != 8 || dy != 8) { // fill unused portion with 0
                            memset(YDU,0,sizeof(YDU));
                            memset(UDU,0,sizeof(UDU));
                            memset(VDU,0,sizeof(VDU));
                        }
                        for (int ty=0; ty<dy; ty++) { // rows
                            pRow = IMAGE_COMPUTE_RGB565_PIXEL_ROW_PTR(src, y+ty);
                            pRow += x;
                            pY = &YDU[(ty*8)]; pU = &UDU[ty*8]; pV=&VDU[ty*8];
                            for (int tx=0; tx<dx; tx++) { // columns
                                pixel = *pRow++;
                                r = COLOR_RGB565_TO_R8(pixel);
                                g = COLOR_RGB565_TO_G8(pixel);
                                b = COLOR_RGB565_TO_B8(pixel);
                                // faster to keep all calculations in integer math with 15-bit fractions
                                *pY++ = (uint8_t)(((r * 9770) + (g * 19182) + (b * 3736)) >> 15) -128; // .299*r + .587*g + .114*b
                                *pU++ = (uint8_t)(((b << 14) - (r * 5529) - (g * 10855)) >> 15); // -0.168736*r + -0.331264*g + 0.5*b
                                *pV++ = (uint8_t)(((r << 14) - (g * 13682) - (b * 2664)) >> 15); // 0.5*r + -0.418688*g + -0.081312*b
                            } // for tx
                        } // for ty

                        DCY = jpeg_processDU(&jpeg_buf, YDU, fdtbl_Y, DCY, YDC_HT, YAC_HT);
                        DCU = jpeg_processDU(&jpeg_buf, UDU, fdtbl_UV, DCU, UVDC_HT, UVAC_HT);
                        DCV = jpeg_processDU(&jpeg_buf, VDU, fdtbl_UV, DCV, UVDC_HT, UVAC_HT);
                    }
                    if (jpeg_buf.overflow) {
                        goto jpeg_overflow;
                    }
                }
                break;
            }
            case JPEG_SUBSAMPLE_2x1: {
                uint16_t pixel, *pRow;
                int dx, dy;
                int r, g, b; // to separate RGB565 into R8,G8,B8
                int8_t YDU[128], UDU[64], VDU[64];
                int8_t *pY, *pU, *pV;
                for (int y=0; y<src->h; y+=8) {
                    dy = 8;
                    if (y+8 > src->h) // over bottom edge
                        dy = src->h - y;
                    for (int x=0; x<src->w; x+=16) {
                        dx = 16;
                        if (x+16 > src->w) // over right edge
                        dx = src->w - x;
                        for (int ty=0; ty<dy; ty++) { // rows
                            pRow = IMAGE_COMPUTE_RGB565_PIXEL_ROW_PTR(src, y+ty);
                            pRow += x;
                            pY = &YDU[(ty*8)]; pU = &UDU[ty*8]; pV=&VDU[ty*8];
                            for (int tx=0; tx<dx; tx+=2) { // column pairs
                                if (tx == 8) // second column of Y MCUs
                                   pY += (64-8);

                                pixel = pRow[0]; // left
                                r = COLOR_RGB565_TO_R8(pixel);
                                g = COLOR_RGB565_TO_G8(pixel);
                                b = COLOR_RGB565_TO_B8(pixel);
                                // faster to keep all calculations in integer math with 15-bit fractions
                                pY[0] = (uint8_t)(((r * 9770) + (g * 19182) + (b * 3736)) >> 15) -128; // .299*r + .587*g + .114*b
                                *pU++ = (uint8_t)(((b << 14) - (r * 5529) - (g * 10855)) >> 15); // -0.168736*r + -0.331264*g + 0.5*b
                                *pV++ = (uint8_t)(((r << 14) - (g * 13682) - (b * 2664)) >> 15); // 0.5*r + -0.418688*g + -0.081312*b
                                pixel = pRow[1]; // right
                                r = COLOR_RGB565_TO_R8(pixel);
                                g = COLOR_RGB565_TO_G8(pixel);
                                b = COLOR_RGB565_TO_B8(pixel);
                                // faster to keep all calculations in integer math with 15-bit fractions
                                pY[1] = (uint8_t)(((r * 9770) + (g * 19182) + (b * 3736)) >> 15)-128; // .299*r + .587*g + .114*b

                                pY += 2; pRow += 2;
                            } // for tx
                        } // for ty

                        DCY = jpeg_processDU(&jpeg_buf, YDU,    fdtbl_Y, DCY, YDC_HT, YAC_HT);
                        DCY = jpeg_processDU(&jpeg_buf, YDU+64, fdtbl_Y, DCY, YDC_HT, YAC_HT);
                        DCU = jpeg_processDU(&jpeg_buf, UDU, fdtbl_UV, DCU, UVDC_HT, UVAC_HT);
                        DCV = jpeg_processDU(&jpeg_buf, VDU, fdtbl_UV, DCV, UVDC_HT, UVAC_HT);
                    }
                    if (jpeg_buf.overflow) {
                        goto jpeg_overflow;
                    }
                }
                break;
            }
            case JPEG_SUBSAMPLE_2x2: {
                uint16_t pixel, *pRow;
                int dx, dy;
                int r, g, b; // to separate RGB565 into R8,G8,B8
                int8_t YDU[256], UDU[64], VDU[64];
                int8_t *pY, *pU, *pV;

                for (int y=0; y<src->h; y+=16) {
                    dy = 16;
                    if (y+16 > src->h) // over bottom edge
                        dy = src->h - y;
                    for (int x=0; x<src->w; x+=16) {
                        dx = 16;
                        if (x+16 > src->w) // over right edge, reduce capture size
                            dx = src->w - x;
                        if (dx != 16 || dy != 16) { // fill unused portion with 0
                            memset(YDU,0,sizeof(YDU));
                            memset(UDU,0,sizeof(UDU));
                            memset(VDU,0,sizeof(VDU));
                        }
                        for (int ty=0; ty<dy; ty+=2) { // row pairs
                            pRow = IMAGE_COMPUTE_RGB565_PIXEL_ROW_PTR(src, y+ty);
                            pRow += x;
                            pY = &YDU[(ty*8)]; pU = &UDU[ty*4]; pV=&VDU[ty*4];
                            if (ty >= 8) // second row of Y MCUs
                                pY += (128 - 64);
                            for (int tx=0; tx<dx; tx+=2) { // column pairs
                                if (tx == 8) // second column of Y MCUs
                                   pY += (64-8);

                                pixel = pRow[0]; // top left
                                r = COLOR_RGB565_TO_R8(pixel); // extract R8/G8/B8
                                g = COLOR_RGB565_TO_G8(pixel);
                                b = COLOR_RGB565_TO_B8(pixel);
                                // faster to keep all calculations in integer math with 15-bit fractions
                                pY[0] = (uint8_t)(((r * 9770) + (g * 19182) + (b * 3736)) >> 15) -128; // .299*r + .587*g + .114*b
                                pU[0] = (uint8_t)(((b << 14) - (r * 5529) - (g * 10855)) >> 15); // -0.168736*r + -0.331264*g + 0.5*b
                                pV[0] = (uint8_t)(((r << 14) - (g * 13682) - (b * 2664)) >> 15); // 0.5*r + -0.418688*g + -0.081312*b
                                pixel = pRow[1]; // top right
                                r = COLOR_RGB565_TO_R8(pixel); // extract R8/G8/B8
                                g = COLOR_RGB565_TO_G8(pixel);
                                b = COLOR_RGB565_TO_B8(pixel);
                                // faster to keep all calculations in integer math with 15-bit fractions
                                pY[1] = (uint8_t)(((r * 9770) + (g * 19182) + (b * 3736)) >> 15)-128; // .299*r + .587*g + .114*b

                                pixel = pRow[src->w]; // bottom left
                                r = COLOR_RGB565_TO_R8(pixel); // extract R8/G8/B8
                                g = COLOR_RGB565_TO_G8(pixel);
                                b = COLOR_RGB565_TO_B8(pixel);
                                // faster to keep all calculations in integer math with 15-bit fractions
                                pY[8] = (uint8_t)(((r * 9770) + (g * 19182) + (b * 3736)) >> 15)-128; // .299*r + .587*g + .114*b

                                pixel = pRow[1+src->w]; // bottom right
                                r = COLOR_RGB565_TO_R8(pixel); // extract R8/G8/B8
                                g = COLOR_RGB565_TO_G8(pixel);
                                b = COLOR_RGB565_TO_B8(pixel);
                                // faster to keep all calculations in integer math with 15-bit fractions
                                pY[9] = (uint8_t)(((r * 9770) + (g * 19182) + (b * 3736)) >> 15)-128; // .299*r + .587*g + .114*b
                                pY += 2; pU++; pV++; pRow += 2;
                            } // for tx
                        } // for ty

                        DCY = jpeg_processDU(&jpeg_buf, YDU,     fdtbl_Y, DCY, YDC_HT, YAC_HT);
                        DCY = jpeg_processDU(&jpeg_buf, YDU+64,  fdtbl_Y, DCY, YDC_HT, YAC_HT);
                        DCY = jpeg_processDU(&jpeg_buf, YDU+128, fdtbl_Y, DCY, YDC_HT, YAC_HT);
                        DCY = jpeg_processDU(&jpeg_buf, YDU+192, fdtbl_Y, DCY, YDC_HT, YAC_HT);
                        DCU = jpeg_processDU(&jpeg_buf, UDU, fdtbl_UV, DCU, UVDC_HT, UVAC_HT);
                        DCV = jpeg_processDU(&jpeg_buf, VDU, fdtbl_UV, DCV, UVDC_HT, UVAC_HT);
                    }
                    if (jpeg_buf.overflow) {
                        goto jpeg_overflow;
                    }
                }
                break;
            }
        }
    } else if (src->bpp == 3) { //RAW/BAYER
        switch (jpeg_subsample) {
            case JPEG_SUBSAMPLE_1x1: {
                int8_t YDU[64], UDU[64], VDU[64];
                for (int y=0; y<src->h; y+=8) {
                    for (int x=0; x<src->w; x+=8) {
                        bayer_to_ycbcr(src, x, y, (uint8_t *)YDU, (uint8_t *)UDU, (uint8_t *)VDU, 1);
                        DCY = jpeg_processDU(&jpeg_buf, YDU, fdtbl_Y, DCY, YDC_HT, YAC_HT);
                        DCU = jpeg_processDU(&jpeg_buf, UDU, fdtbl_UV, DCU, UVDC_HT, UVAC_HT);
                        DCV = jpeg_processDU(&jpeg_buf, VDU, fdtbl_UV, DCV, UVDC_HT, UVAC_HT);
                        if (jpeg_buf.overflow) {
                            goto jpeg_overflow;
                        }
                    }
                }
                break;
            }
            case JPEG_SUBSAMPLE_2x1: {
                int8_t YDU[128], UDU[128], VDU[128];
                int idx;
                for (int y=0; y<src->h; y+=8) {
                    for (int x=0; x<src->w; x+=16) {
                        bayer_to_ycbcr(src, x, y, (uint8_t *)YDU, (uint8_t *)UDU, (uint8_t *)VDU, 1); // left block
                        bayer_to_ycbcr(src, x+8, y, (uint8_t *)&YDU[64], (uint8_t *)&UDU[64], (uint8_t *)&VDU[64], 1); // right block
                        // horizontal subsampling of U & V
                        for (idx=0; idx<64; idx++) {
                            UDU[idx] = (int8_t)((UDU[idx] + UDU[idx+64] + 1) >> 1);
                            VDU[idx] = (int8_t)((VDU[idx] + VDU[idx+64] + 1) >> 1);
                        } // for idx
                        DCY = jpeg_processDU(&jpeg_buf, YDU,    fdtbl_Y, DCY, YDC_HT, YAC_HT);
                        DCY = jpeg_processDU(&jpeg_buf, YDU+64, fdtbl_Y, DCY, YDC_HT, YAC_HT);
                        DCU = jpeg_processDU(&jpeg_buf, UDU, fdtbl_UV, DCU, UVDC_HT, UVAC_HT);
                        DCV = jpeg_processDU(&jpeg_buf, VDU, fdtbl_UV, DCV, UVDC_HT, UVAC_HT);
                    }
                    if (jpeg_buf.overflow) {
                        goto jpeg_overflow;
                    }
                }
                break;
            }
            case JPEG_SUBSAMPLE_2x2: {
                int8_t YDU[256], UDU[256], VDU[256];
                int idx;
                for (int y=0; y<src->h; y+=16) {
                    for (int x=0; x<src->w; x+=16) {
                        bayer_to_ycbcr(src, x, y, (uint8_t *)YDU, (uint8_t *)UDU, (uint8_t *)VDU, 1); // left block
                        bayer_to_ycbcr(src, x+8, y, (uint8_t *)&YDU[64], (uint8_t *)&UDU[64], (uint8_t *)&VDU[64], 1); // right block
                        bayer_to_ycbcr(src, x, y+8, (uint8_t *)&YDU[128], (uint8_t *)&UDU[128], (uint8_t *)&VDU[128], 1); // left block
                        bayer_to_ycbcr(src, x+8, y+8, (uint8_t *)&YDU[192], (uint8_t *)&UDU[192], (uint8_t *)&VDU[192], 1); // right block
                        // horiz+vert subsampling of U & V
                        for (idx=0; idx<64; idx++) {
                            UDU[idx] = (int8_t)((UDU[idx] + UDU[idx+64] + UDU[idx+128] + UDU[idx+192] + 2) >> 2);
                            VDU[idx] = (int8_t)((VDU[idx] + VDU[idx+64] + VDU[idx+128] + VDU[idx+192] + 2) >> 2);
                        } // for idx

                        DCY = jpeg_processDU(&jpeg_buf, YDU,     fdtbl_Y, DCY, YDC_HT, YAC_HT);
                        DCY = jpeg_processDU(&jpeg_buf, YDU+64,  fdtbl_Y, DCY, YDC_HT, YAC_HT);
                        DCY = jpeg_processDU(&jpeg_buf, YDU+128, fdtbl_Y, DCY, YDC_HT, YAC_HT);
                        DCY = jpeg_processDU(&jpeg_buf, YDU+192, fdtbl_Y, DCY, YDC_HT, YAC_HT);
                        DCU = jpeg_processDU(&jpeg_buf, UDU, fdtbl_UV, DCU, UVDC_HT, UVAC_HT);
                        DCV = jpeg_processDU(&jpeg_buf, VDU, fdtbl_UV, DCV, UVDC_HT, UVAC_HT);
                    }
                    if (jpeg_buf.overflow) {
                        goto jpeg_overflow;
                    }
                }
                break;
            }
        }
    }


    // Do the bit alignment of the EOI marker
    static const uint16_t fillBits[] = {0x7F, 7};
    jpeg_writeBits(&jpeg_buf, fillBits);

    // EOI
    jpeg_put_char(&jpeg_buf, 0xFF);
    jpeg_put_char(&jpeg_buf, 0xD9);

    dst->bpp = jpeg_buf.idx;
    dst->data = jpeg_buf.buf;

    #if (TIME_JPEG==1)
    printf("time: %lums\n", mp_hal_ticks_ms() - start);
    #endif

jpeg_overflow:
    return jpeg_buf.overflow;
}
#endif //defined OMV_HARDWARE_JPEG

int jpeg_clean_trailing_bytes(int bpp, uint8_t *data)
{
    while ((bpp > 1) && ((data[bpp-2] != 0xFF) || (data[bpp-1] != 0xD9))) {
        bpp -= 1;
    }

    return bpp;
}

#if defined(IMLIB_ENABLE_IMAGE_FILE_IO)
// This function inits the geometry values of an image.
void jpeg_read_geometry(FIL *fp, image_t *img, const char *path, jpg_read_settings_t *rs)
{
    for (;;) {
        uint16_t header;
        read_word(fp, &header);
        header = __REV16(header);
        if ((0xFFD0 <= header) && (header <= 0xFFD9)) {
            continue;
        } else if (((0xFFC0 <= header) && (header <= 0xFFCF))
                || ((0xFFDA <= header) && (header <= 0xFFDF))
                || ((0xFFE0 <= header) && (header <= 0xFFEF))
                || ((0xFFF0 <= header) && (header <= 0xFFFE)))
        {
            uint16_t size;
            read_word(fp, &size);
            size = __REV16(size);
            if (((0xFFC0 <= header) && (header <= 0xFFC3))
             || ((0xFFC5 <= header) && (header <= 0xFFC7))
             || ((0xFFC9 <= header) && (header <= 0xFFCB))
             || ((0xFFCD <= header) && (header <= 0xFFCF)))
            {
                read_byte_ignore(fp);
                uint16_t width;
                read_word(fp, &width);
                width = __REV16(width);
                uint16_t height;
                read_word(fp, &height);
                height = __REV16(height);
                rs->jpg_w = img->w = width;
                rs->jpg_h = img->h = height;
                rs->jpg_size = img->bpp = f_size(fp);
                return;
            } else {
                file_seek(fp, f_tell(fp) + size - 2);
            }
        } else {
            ff_file_corrupted(fp);
        }
    }
}

// This function reads the pixel values of an image.
void jpeg_read_pixels(FIL *fp, image_t *img)
{
    file_seek(fp, 0);
    read_data(fp, img->pixels, img->bpp);
}

void jpeg_read(image_t *img, const char *path)
{
    FIL fp;
    jpg_read_settings_t rs;

    file_read_open(&fp, path);

    // Do not use file_buffer_on() here.
    jpeg_read_geometry(&fp, img, path, &rs);

    if (!img->pixels) {
        img->pixels = xalloc(img->bpp);
    }

    jpeg_read_pixels(&fp, img);
    file_close(&fp);
}

void jpeg_write(image_t *img, const char *path, int quality)
{
    FIL fp;
    file_write_open(&fp, path);
    if (IM_IS_JPEG(img)) {
        write_data(&fp, img->pixels, img->bpp);
    } else {
        image_t out = { .w=img->w, .h=img->h, .bpp=0, .pixels=NULL }; // alloc in jpeg compress
        // When jpeg_compress needs more memory than in currently allocated it
        // will try to realloc. MP will detect that the pointer is outside of
        // the heap and return NULL which will cause an out of memory error.
        jpeg_compress(img, &out, quality, false);
        write_data(&fp, out.pixels, out.bpp);
        fb_free(); // frees alloc in jpeg_compress()
    }
    file_close(&fp);
}
#endif //IMLIB_ENABLE_IMAGE_FILE_IO)
