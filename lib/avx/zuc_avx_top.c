/*******************************************************************************
  Copyright (c) 2009-2020, Intel Corporation

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:

      * Redistributions of source code must retain the above copyright notice,
        this list of conditions and the following disclaimer.
      * Redistributions in binary form must reproduce the above copyright
        notice, this list of conditions and the following disclaimer in the
        documentation and/or other materials provided with the distribution.
      * Neither the name of Intel Corporation nor the names of its contributors
        may be used to endorse or promote products derived from this software
        without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
  SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
  OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*******************************************************************************/

/*-----------------------------------------------------------------------
* zuc_avx.c
*-----------------------------------------------------------------------
* An implementation of ZUC, the core algorithm for the
* 3GPP Confidentiality and Integrity algorithms.
*
*-----------------------------------------------------------------------*/

#include <string.h>

#include "include/zuc_internal.h"
#include "include/wireless_common.h"
#include "include/save_xmms.h"
#include "include/clear_regs_mem.h"
#include "intel-ipsec-mb.h"

#define SAVE_XMMS               save_xmms_avx
#define RESTORE_XMMS            restore_xmms_avx
#define CLEAR_SCRATCH_SIMD_REGS clear_scratch_xmms_avx

#define NUM_AVX_BUFS 4
#define KEYSTR_ROUND_LEN 16

static inline
void _zuc_eea3_1_buffer_avx(const void *pKey,
                            const void *pIv,
                            const void *pBufferIn,
                            void *pBufferOut,
                            const uint32_t length)
{
        DECLARE_ALIGNED(ZucState_t zucState, 16);
        DECLARE_ALIGNED(uint8_t keyStream[KEYSTR_ROUND_LEN], 16);
        const uint64_t *pIn64 = NULL;
        uint64_t *pOut64 = NULL, *pKeyStream64 = NULL;
        uint64_t *pTemp64 = NULL, *pdstTemp64 = NULL;

        uint32_t numKeyStreamsPerPkt = length/ KEYSTR_ROUND_LEN;
        const uint32_t numBytesLeftOver = length % KEYSTR_ROUND_LEN;

        /* need to set the LFSR state to zero */
        memset(&zucState, 0, sizeof(ZucState_t));

        /* initialize the zuc state */
        asm_ZucInitialization_avx(pKey, pIv, &(zucState));

        /* Loop Over all the Quad-Words in input buffer and XOR with the
         * 16 bytes of generated keystream */
        pOut64 = (uint64_t *) pBufferOut;
        pIn64 = (const uint64_t *) pBufferIn;

        while (numKeyStreamsPerPkt--) {
                /* Generate the key stream 16 bytes at a time */
                asm_ZucGenKeystream16B_avx((uint32_t *) &keyStream[0],
                                           &zucState);

                /* XOR The Keystream generated with the input buffer here */
                pKeyStream64 = (uint64_t *) keyStream;
                asm_XorKeyStream16B_avx(pIn64, pOut64, pKeyStream64);
                pIn64 += 2;
                pOut64 += 2;
        }

        /* Check for remaining 0 to 15 bytes */
        if (numBytesLeftOver) {
                /* buffer to store 16 bytes of keystream */
                DECLARE_ALIGNED(uint8_t tempSrc[KEYSTR_ROUND_LEN], 16);
                DECLARE_ALIGNED(uint8_t tempDst[KEYSTR_ROUND_LEN], 16);
                const uint8_t *pIn8 = (const uint8_t *) pBufferIn;
                uint8_t *pOut8 = (uint8_t *) pBufferOut;
                const uint64_t num4BRounds = ((numBytesLeftOver - 1) / 4) + 1;

                asm_ZucGenKeystream_avx((uint32_t *) &keyStream[0],
                                        &zucState, num4BRounds);

                /* copy the remaining bytes into temporary buffer and XOR with
                 * the 16-bytes of keystream. Then copy on the valid bytes back
                 * to the output buffer */

                memcpy(&tempSrc[0], &pIn8[length - numBytesLeftOver],
                       numBytesLeftOver);
                pKeyStream64 = (uint64_t *) &keyStream[0];
                pTemp64 = (uint64_t *) &tempSrc[0];
                pdstTemp64 = (uint64_t *) &tempDst[0];

                asm_XorKeyStream16B_avx(pTemp64, pdstTemp64,
                                        pKeyStream64);
                memcpy(&pOut8[length - numBytesLeftOver], &tempDst[0],
                       numBytesLeftOver);

#ifdef SAFE_DATA
                clear_mem(tempSrc, sizeof(tempSrc));
                clear_mem(tempDst, sizeof(tempDst));
#endif
        }
#ifdef SAFE_DATA
        /* Clear sensitive data in stack */
        clear_mem(keyStream, sizeof(keyStream));
        clear_mem(&zucState, sizeof(zucState));
#endif
}

IMB_DLL_LOCAL
void _zuc_eea3_4_buffer_avx(const void * const pKey[NUM_AVX_BUFS],
                            const void * const pIv[NUM_AVX_BUFS],
                            const void * const pBufferIn[NUM_AVX_BUFS],
                            void *pBufferOut[NUM_AVX_BUFS],
                            const uint32_t length[NUM_AVX_BUFS])
{
        DECLARE_ALIGNED(ZucState4_t state, 16);
        DECLARE_ALIGNED(ZucState_t singlePktState, 16);
        unsigned int i;
        /* Calculate the minimum input packet size */
        uint32_t bytes1 = (length[0] < length[1] ?
                           length[0] : length[1]);
        uint32_t bytes2 = (length[2] < length[3] ?
                           length[2] : length[3]);
        /* min number of bytes */
        uint32_t bytes = (bytes1 < bytes2) ? bytes1 : bytes2;
        uint32_t numKeyStreamsPerPkt = bytes/KEYSTR_ROUND_LEN;
        uint32_t remainBytes[NUM_AVX_BUFS] = {0};
        DECLARE_ALIGNED(uint8_t keyStr[NUM_AVX_BUFS][KEYSTR_ROUND_LEN], 64);
        /* structure to store the 4 keys */
        DECLARE_ALIGNED(ZucKey4_t keys, 64);
        /* structure to store the 4 IV's */
        DECLARE_ALIGNED(ZucIv4_t ivs, 64);
        uint32_t numBytesLeftOver = 0;
        const uint8_t *pTempBufInPtr = NULL;
        uint8_t *pTempBufOutPtr = NULL;
        const uint64_t *pIn64[NUM_AVX_BUFS]= {NULL};
        uint64_t *pOut64[NUM_AVX_BUFS] = {NULL};
        uint64_t *pKeyStream64 = NULL;
        uint32_t *pKeyStrArr[NUM_AVX_BUFS] = {NULL};

        /* rounded down minimum length */
        bytes = numKeyStreamsPerPkt * KEYSTR_ROUND_LEN;

        /* Need to set the LFSR state to zero */
        memset(&state, 0, sizeof(ZucState4_t));

        /*
         * Calculate the number of bytes left over for each packet,
         * and setup the Keys and IVs
         */
        for (i = 0; i< NUM_AVX_BUFS; i++) {
                remainBytes[i] = length[i] - bytes;
                keys.pKeys[i] = pKey[i];
                ivs.pIvs[i] = pIv[i];
        }

        asm_ZucInitialization_4_avx( &keys,  &ivs, &state);

        for (i = 0; i < NUM_AVX_BUFS; i++) {
                pOut64[i] = (uint64_t *) pBufferOut[i];
                pIn64[i] = (const uint64_t *) pBufferIn[i];
                pKeyStrArr[i] = (uint32_t *) &keyStr[i][0];
        }

        /* Loop for 16 bytes at a time generating 4 key-streams per loop */
        while (numKeyStreamsPerPkt) {
                /* Generate 16 bytes at a time */
                asm_ZucGenKeystream16B_4_avx(&state, pKeyStrArr);

                /* XOR the KeyStream with the input buffers and store in output
                 * buffer*/
                for (i = 0; i < NUM_AVX_BUFS; i++) {
                        pKeyStream64 = (uint64_t *) pKeyStrArr[i];
                        asm_XorKeyStream16B_avx(pIn64[i], pOut64[i],
                                                pKeyStream64);
                        pIn64[i] += 2;
                        pOut64[i] += 2;
                }

                /* Update keystream count */
                numKeyStreamsPerPkt--;

        }

        /* process each packet separately for the remaining bytes */
        for (i = 0; i < NUM_AVX_BUFS; i++) {
                if (remainBytes[i]) {
                        /* need to copy the zuc state to single packet state */
                        singlePktState.lfsrState[0] = state.lfsrState[0][i];
                        singlePktState.lfsrState[1] = state.lfsrState[1][i];
                        singlePktState.lfsrState[2] = state.lfsrState[2][i];
                        singlePktState.lfsrState[3] = state.lfsrState[3][i];
                        singlePktState.lfsrState[4] = state.lfsrState[4][i];
                        singlePktState.lfsrState[5] = state.lfsrState[5][i];
                        singlePktState.lfsrState[6] = state.lfsrState[6][i];
                        singlePktState.lfsrState[7] = state.lfsrState[7][i];
                        singlePktState.lfsrState[8] = state.lfsrState[8][i];
                        singlePktState.lfsrState[9] = state.lfsrState[9][i];
                        singlePktState.lfsrState[10] = state.lfsrState[10][i];
                        singlePktState.lfsrState[11] = state.lfsrState[11][i];
                        singlePktState.lfsrState[12] = state.lfsrState[12][i];
                        singlePktState.lfsrState[13] = state.lfsrState[13][i];
                        singlePktState.lfsrState[14] = state.lfsrState[14][i];
                        singlePktState.lfsrState[15] = state.lfsrState[15][i];

                        singlePktState.fR1 = state.fR1[i];
                        singlePktState.fR2 = state.fR2[i];

                        singlePktState.bX0 = state.bX0[i];
                        singlePktState.bX1 = state.bX1[i];
                        singlePktState.bX2 = state.bX2[i];
                        singlePktState.bX3 = state.bX3[i];

                        numKeyStreamsPerPkt = remainBytes[i] / KEYSTR_ROUND_LEN;
                        numBytesLeftOver = remainBytes[i]  % KEYSTR_ROUND_LEN;

                        pTempBufInPtr = pBufferIn[i];
                        pTempBufOutPtr = pBufferOut[i];

                        /* update the output and input pointers here to point
                         * to the i'th buffers */
                        pOut64[0] = (uint64_t *) &pTempBufOutPtr[length[i] -
                                                                remainBytes[i]];
                        pIn64[0] = (const uint64_t *) &pTempBufInPtr[length[i] -
                                                                remainBytes[i]];

                        while (numKeyStreamsPerPkt--) {
                                /* Generate the key stream 16 bytes at a time */
                                asm_ZucGenKeystream16B_avx(
                                                       (uint32_t *) keyStr[0],
                                                       &singlePktState);
                                pKeyStream64 = (uint64_t *) keyStr[0];
                                asm_XorKeyStream16B_avx(pIn64[0], pOut64[0],
                                                        pKeyStream64);
                                pIn64[0] += 2;
                                pOut64[0] += 2;
                        }

                        /* Check for remaining 0 to 15 bytes */
                        if (numBytesLeftOver) {
                                DECLARE_ALIGNED(uint8_t tempSrc[16], 64);
                                DECLARE_ALIGNED(uint8_t tempDst[16], 64);
                                uint64_t *pTempSrc64;
                                uint64_t *pTempDst64;
                                uint32_t offset = length[i] - numBytesLeftOver;
                                const uint64_t num4BRounds =
                                        ((numBytesLeftOver - 1) / 4) + 1;

                                asm_ZucGenKeystream_avx((uint32_t *)&keyStr[0],
                                                        &singlePktState,
                                                        num4BRounds);
                                /* copy the remaining bytes into temporary
                                 * buffer and XOR with the 16 bytes of
                                 * keystream. Then copy on the valid bytes back
                                 * to the output buffer */
                                memcpy(&tempSrc[0], &pTempBufInPtr[offset],
                                       numBytesLeftOver);
                                memset(&tempSrc[numBytesLeftOver], 0,
                                       16 - numBytesLeftOver);

                                pKeyStream64 = (uint64_t *) &keyStr[0][0];
                                pTempSrc64 = (uint64_t *) &tempSrc[0];
                                pTempDst64 = (uint64_t *) &tempDst[0];
                                asm_XorKeyStream16B_avx(pTempSrc64, pTempDst64,
                                                        pKeyStream64);

                                memcpy(&pTempBufOutPtr[offset],
                                       &tempDst[0], numBytesLeftOver);
#ifdef SAFE_DATA
                                clear_mem(tempSrc, sizeof(tempSrc));
                                clear_mem(tempDst, sizeof(tempDst));
#endif
                        }
                }
        }
#ifdef SAFE_DATA
        /* Clear sensitive data in stack */
        clear_mem(keyStr, sizeof(keyStr));
        clear_mem(&singlePktState, sizeof(singlePktState));
        clear_mem(&state, sizeof(state));
        clear_mem(&keys, sizeof(keys));
#endif
}

IMB_DLL_LOCAL
void zuc_eea3_4_buffer_job_avx(const void * const pKey[NUM_AVX_BUFS],
                               const void * const pIv[NUM_AVX_BUFS],
                               const void * const pBufferIn[NUM_AVX_BUFS],
                               void *pBufferOut[NUM_AVX_BUFS],
                               const uint16_t length[NUM_AVX_BUFS],
                               const void * const job_in_lane[NUM_AVX_BUFS])
{
        DECLARE_ALIGNED(ZucState4_t state, 64);
        DECLARE_ALIGNED(ZucState_t singlePktState, 64);
        unsigned int i;
        /* Calculate the minimum input packet size */
        uint32_t bytes1 = (length[0] < length[1] ?
                           length[0] : length[1]);
        uint32_t bytes2 = (length[2] < length[3] ?
                           length[2] : length[3]);
        /* min number of bytes */
        uint32_t bytes = (bytes1 < bytes2) ? bytes1 : bytes2;
        uint32_t numKeyStreamsPerPkt = bytes/KEYSTR_ROUND_LEN;
        uint32_t remainBytes[NUM_AVX_BUFS] = {0};
        /* structure to store the 4 keys */
        DECLARE_ALIGNED(ZucKey4_t keys, 64);
        /* structure to store the 4 IV's */
        DECLARE_ALIGNED(ZucIv4_t ivs, 64);
        uint32_t numBytesLeftOver = 0;
        const uint8_t *pTempBufInPtr = NULL;
        uint8_t *pTempBufOutPtr = NULL;

        const uint64_t *pIn64[NUM_AVX_BUFS] = {NULL};
        uint64_t *pOut64[NUM_AVX_BUFS] = {NULL};

        /* rounded down minimum length */
        bytes = numKeyStreamsPerPkt * KEYSTR_ROUND_LEN;

        /* Need to set the LFSR state to zero */
        memset(&state, 0, sizeof(ZucState4_t));

        /*
         * Calculate the number of bytes left over for each packet,
         * and setup the Keys and IVs
         */
        for (i = 0; i < NUM_AVX_BUFS; i++) {
                remainBytes[i] = length[i] - bytes;
                keys.pKeys[i] = pKey[i];
                ivs.pIvs[i] = pIv[i];
        }

        asm_ZucInitialization_4_avx(&keys,  &ivs, &state);

        for (i = 0; i < NUM_AVX_BUFS; i++) {
                pOut64[i] = (uint64_t *) pBufferOut[i];
                pIn64[i] = (const uint64_t *) pBufferIn[i];
        }

        asm_ZucCipherNx16B_4_avx(&state, pIn64, pOut64,
                                 numKeyStreamsPerPkt * 16);

        /* process each packet separately for the remaining bytes */
        for (i = 0; i < NUM_AVX_BUFS; i++) {
                if (remainBytes[i] && job_in_lane[i]) {
                        /* need to copy the zuc state to single packet state */
                        singlePktState.lfsrState[0] = state.lfsrState[0][i];
                        singlePktState.lfsrState[1] = state.lfsrState[1][i];
                        singlePktState.lfsrState[2] = state.lfsrState[2][i];
                        singlePktState.lfsrState[3] = state.lfsrState[3][i];
                        singlePktState.lfsrState[4] = state.lfsrState[4][i];
                        singlePktState.lfsrState[5] = state.lfsrState[5][i];
                        singlePktState.lfsrState[6] = state.lfsrState[6][i];
                        singlePktState.lfsrState[7] = state.lfsrState[7][i];
                        singlePktState.lfsrState[8] = state.lfsrState[8][i];
                        singlePktState.lfsrState[9] = state.lfsrState[9][i];
                        singlePktState.lfsrState[10] = state.lfsrState[10][i];
                        singlePktState.lfsrState[11] = state.lfsrState[11][i];
                        singlePktState.lfsrState[12] = state.lfsrState[12][i];
                        singlePktState.lfsrState[13] = state.lfsrState[13][i];
                        singlePktState.lfsrState[14] = state.lfsrState[14][i];
                        singlePktState.lfsrState[15] = state.lfsrState[15][i];

                        singlePktState.fR1 = state.fR1[i];
                        singlePktState.fR2 = state.fR2[i];

                        singlePktState.bX0 = state.bX0[i];
                        singlePktState.bX1 = state.bX1[i];
                        singlePktState.bX2 = state.bX2[i];
                        singlePktState.bX3 = state.bX3[i];

                        numKeyStreamsPerPkt = remainBytes[i] / KEYSTR_ROUND_LEN;
                        numBytesLeftOver = remainBytes[i]  % KEYSTR_ROUND_LEN;

                        pTempBufInPtr = pBufferIn[i];
                        pTempBufOutPtr = pBufferOut[i];

                        /* update the output and input pointers here to point
                         * to the i'th buffers */
                        pOut64[0] = (uint64_t *) &pTempBufOutPtr[length[i] -
                                                                remainBytes[i]];
                        pIn64[0] = (const uint64_t *) &pTempBufInPtr[length[i] -
                                                                remainBytes[i]];

                        while (numKeyStreamsPerPkt--) {
                                DECLARE_ALIGNED(uint32_t keyStream[4], 64);
                                uint64_t *pKeyStream64 = (uint64_t *) keyStream;

                                /* Generate the key stream 16 bytes at a time */
                                asm_ZucGenKeystream16B_avx(keyStream,
                                                       &singlePktState);
                                asm_XorKeyStream16B_avx(pIn64[0], pOut64[0],
                                                        pKeyStream64);
                                pIn64[0] += 2;
                                pOut64[0] += 2;
                        }


                        /* Check for remaining 0 to 15 bytes */
                        if (numBytesLeftOver) {
                                DECLARE_ALIGNED(uint8_t tempSrc[KEYSTR_ROUND_LEN], 64);
                                DECLARE_ALIGNED(uint8_t tempDst[KEYSTR_ROUND_LEN], 64);
                                uint64_t *pTempSrc64;
                                uint64_t *pTempDst64;
                                uint32_t offset = length[i] - numBytesLeftOver;
                                const uint64_t num4BRounds =
                                        ((numBytesLeftOver - 1) / 4) + 1;
                                DECLARE_ALIGNED(uint32_t keyStream[8], 64);
                                uint64_t *pKeyStream64 = (uint64_t *) keyStream;

                                asm_ZucGenKeystream_avx(keyStream,
                                                       &singlePktState,
                                                       num4BRounds);
                                /* copy the remaining bytes into temporary
                                 * buffer and XOR with the 16 bytes of
                                 * keystream. Then copy on the valid bytes back
                                 * to the output buffer */
                                memcpy(&tempSrc[0], &pTempBufInPtr[offset],
                                       numBytesLeftOver);
                                memset(&tempSrc[numBytesLeftOver], 0,
                                       KEYSTR_ROUND_LEN - numBytesLeftOver);

                                pTempSrc64 = (uint64_t *) &tempSrc[0];
                                pTempDst64 = (uint64_t *) &tempDst[0];
                                asm_XorKeyStream16B_avx(pTempSrc64, pTempDst64,
                                                        pKeyStream64);

                                memcpy(&pTempBufOutPtr[offset],
                                       &tempDst[0], numBytesLeftOver);
#ifdef SAFE_DATA
                                clear_mem(tempSrc, sizeof(tempSrc));
                                clear_mem(tempDst, sizeof(tempDst));
#endif
                        }
                }
        }
#ifdef SAFE_DATA
        /* Clear sensitive data in stack */
        clear_mem(&singlePktState, sizeof(singlePktState));
        clear_mem(&state, sizeof(state));
        clear_mem(&keys, sizeof(keys));
#endif
}

void zuc_eea3_1_buffer_avx(const void *pKey,
                           const void *pIv,
                           const void *pBufferIn,
                           void *pBufferOut,
                           const uint32_t length)
{
#ifndef LINUX
        DECLARE_ALIGNED(imb_uint128_t xmm_save[10], 16);

        SAVE_XMMS(xmm_save);
#endif
#ifdef SAFE_PARAM
        /* Check for NULL pointers */
        if (pKey == NULL || pIv == NULL || pBufferIn == NULL ||
            pBufferOut == NULL)
                return;

        /* Check input data is in range of supported length */
        if (length < ZUC_MIN_BYTELEN || length > ZUC_MAX_BYTELEN)
                return;
#endif
        _zuc_eea3_1_buffer_avx(pKey, pIv, pBufferIn, pBufferOut, length);

#ifdef SAFE_DATA
        /* Clear sensitive data in registers */
        CLEAR_SCRATCH_GPS();
        CLEAR_SCRATCH_SIMD_REGS();
#endif
#ifndef LINUX
        RESTORE_XMMS(xmm_save);
#endif
}

void zuc_eea3_4_buffer_avx(const void * const pKey[NUM_AVX_BUFS],
                           const void * const pIv[NUM_AVX_BUFS],
                           const void * const pBufferIn[NUM_AVX_BUFS],
                           void *pBufferOut[NUM_AVX_BUFS],
                           const uint32_t length[NUM_AVX_BUFS])
{
#ifndef LINUX
        DECLARE_ALIGNED(imb_uint128_t xmm_save[10], 16);

        SAVE_XMMS(xmm_save);
#endif
#ifdef SAFE_PARAM
        unsigned int i;

        /* Check for NULL pointers */
        if (pKey == NULL || pIv == NULL || pBufferIn == NULL ||
            pBufferOut == NULL || length == NULL)
                return;

        for (i = 0; i < NUM_AVX_BUFS; i++) {
                if (pKey[i] == NULL || pIv[i] == NULL ||
                    pBufferIn[i] == NULL || pBufferOut[i] == NULL)
                        return;

                /* Check input data is in range of supported length */
                if (length[i] < ZUC_MIN_BYTELEN || length[i] > ZUC_MAX_BYTELEN)
                        return;
        }
#endif

        _zuc_eea3_4_buffer_avx(pKey, pIv, pBufferIn, pBufferOut, length);

#ifdef SAFE_DATA
        /* Clear sensitive data in registers */
        CLEAR_SCRATCH_GPS();
        CLEAR_SCRATCH_SIMD_REGS();
#endif
#ifndef LINUX
        RESTORE_XMMS(xmm_save);
#endif
}

void zuc_eea3_n_buffer_avx(const void * const pKey[], const void * const pIv[],
                           const void * const pBufferIn[], void *pBufferOut[],
                           const uint32_t length[],
                           const uint32_t numBuffers)
{
#ifndef LINUX
        DECLARE_ALIGNED(imb_uint128_t xmm_save[10], 16);

        SAVE_XMMS(xmm_save);
#endif

        unsigned int i;
        unsigned int packetCount = numBuffers;

#ifdef SAFE_PARAM
        /* Check for NULL pointers */
        if (pKey == NULL || pIv == NULL || pBufferIn == NULL ||
            pBufferOut == NULL || length == NULL)
                return;

        for (i = 0; i < numBuffers; i++) {
                if (pKey[i] == NULL || pIv[i] == NULL ||
                    pBufferIn[i] == NULL || pBufferOut[i] == NULL)
                        return;

                /* Check input data is in range of supported length */
                if (length[i] < ZUC_MIN_BYTELEN || length[i] > ZUC_MAX_BYTELEN)
                        return;
        }
#endif
        i = 0;

        while(packetCount >= 4) {
                packetCount -=4;
                _zuc_eea3_4_buffer_avx(&pKey[i],
                                       &pIv[i],
                                       &pBufferIn[i],
                                       &pBufferOut[i],
                                       &length[i]);
                i+=4;
        }

        while(packetCount--) {
                _zuc_eea3_1_buffer_avx(pKey[i],
                                       pIv[i],
                                       pBufferIn[i],
                                       pBufferOut[i],
                                       length[i]);
                i++;
        }
#ifdef SAFE_DATA
        /* Clear sensitive data in registers */
        CLEAR_SCRATCH_GPS();
        CLEAR_SCRATCH_SIMD_REGS();
#endif
#ifndef LINUX
        RESTORE_XMMS(xmm_save);
#endif
}

static inline uint64_t rotate_left(uint64_t u, size_t r)
{
        return (((u) << (r)) | ((u) >> (64 - (r))));
}

static inline uint64_t load_uint64(const void *ptr)
{
        return *((const uint64_t *)ptr);
}

static inline
void _zuc_eia3_1_buffer_avx(const void *pKey,
                            const void *pIv,
                            const void *pBufferIn,
                            const uint32_t lengthInBits,
                            uint32_t *pMacI)
{
        DECLARE_ALIGNED(ZucState_t zucState, 64);
        DECLARE_ALIGNED(uint32_t keyStream[4 * 2], 64);
        const uint32_t keyStreamLengthInBits = KEYSTR_ROUND_LEN * 8;
        /* generate a key-stream 2 words longer than the input message */
        const uint32_t N = lengthInBits + (2 * ZUC_WORD_BITS);
        uint32_t L = (N + 31) / ZUC_WORD_BITS;
        uint32_t *pZuc = (uint32_t *) &keyStream[0];
        uint32_t remainingBits = lengthInBits;
        uint32_t T = 0;
        const uint8_t *pIn8 = (const uint8_t *) pBufferIn;

        memset(&zucState, 0, sizeof(ZucState_t));

        asm_ZucInitialization_avx(pKey, pIv, &(zucState));
        asm_ZucGenKeystream16B_avx(pZuc, &zucState);

        /* loop over the message bits */
        while (remainingBits >= keyStreamLengthInBits) {
                remainingBits -=  keyStreamLengthInBits;
                L -= (keyStreamLengthInBits / 32);

                /* Generate the next key stream 8 bytes or 16 bytes */
                if (!remainingBits)
                        asm_ZucGenKeystream8B_avx(&keyStream[4], &zucState);
                else
                        asm_ZucGenKeystream16B_avx(&keyStream[4], &zucState);
                T = asm_Eia3Round16BAVX(T, keyStream, pIn8);
                /* Copy the last keystream generated to the first 16 bytes */
                memcpy(&keyStream[0], &keyStream[4], KEYSTR_ROUND_LEN);
                pIn8 = &pIn8[KEYSTR_ROUND_LEN];
        }

        /*
         * If remaining bits has more than 2 ZUC WORDS (double words),
         * keystream needs to have up to another 2 ZUC WORDS (8B)
         */
        if (remainingBits > (2 * 32))
                asm_ZucGenKeystream8B_avx(&keyStream[4], &zucState);
        T ^= asm_Eia3RemainderAVX(&keyStream[0], pIn8, remainingBits);
        T ^= rotate_left(load_uint64(&keyStream[remainingBits / 32]),
                         remainingBits % 32);

        /* save the final MAC-I result */
        uint32_t keyBlock = keyStream[L - 1];
        *pMacI = bswap4(T ^ keyBlock);

#ifdef SAFE_DATA
        /* Clear sensitive data (in registers and stack) */
        clear_mem(keyStream, sizeof(keyStream));
        clear_mem(&zucState, sizeof(zucState));
#endif
}

IMB_DLL_LOCAL
void _zuc_eia3_4_buffer_avx(const void * const pKey[NUM_AVX_BUFS],
                            const void * const pIv[NUM_AVX_BUFS],
                            const void * const pBufferIn[NUM_AVX_BUFS],
                            const uint32_t lengthInBits[NUM_AVX_BUFS],
                            uint32_t *pMacI[NUM_AVX_BUFS])
{
        unsigned int i;
        DECLARE_ALIGNED(ZucState4_t state, 64);
        DECLARE_ALIGNED(ZucState_t singlePktState, 64);
        /* Calculate the minimum input packet size */
        uint32_t bits1 = (lengthInBits[0] < lengthInBits[1] ?
                           lengthInBits[0] : lengthInBits[1]);
        uint32_t bits2 = (lengthInBits[2] < lengthInBits[3] ?
                           lengthInBits[2] : lengthInBits[3]);
        /* min number of bytes */
        uint32_t commonBits = (bits1 < bits2) ? bits1 : bits2;
        DECLARE_ALIGNED(uint8_t keyStr[NUM_AVX_BUFS][2*KEYSTR_ROUND_LEN], 64);
        /* structure to store the 4 keys */
        DECLARE_ALIGNED(ZucKey4_t keys, 64);
        /* structure to store the 4 IV's */
        DECLARE_ALIGNED(ZucIv4_t ivs, 64);
        const uint8_t *pIn8[NUM_AVX_BUFS] = {NULL};
        uint32_t remainCommonBits = commonBits;
        uint32_t numKeyStr = 0;
        uint32_t T[NUM_AVX_BUFS] = {0};
        const uint32_t keyStreamLengthInBits = KEYSTR_ROUND_LEN * 8;
        uint32_t *pKeyStrArr[NUM_AVX_BUFS] = {NULL};

        for (i = 0; i < NUM_AVX_BUFS; i++) {
                pIn8[i] = (const uint8_t *) pBufferIn[i];
                pKeyStrArr[i] = (uint32_t *) &keyStr[i][0];
                keys.pKeys[i] = pKey[i];
                ivs.pIvs[i] = pIv[i];
        }

        /* Need to set the LFSR state to zero */
        memset(&state, 0, sizeof(ZucState4_t));

        asm_ZucInitialization_4_avx( &keys,  &ivs, &state);

        /* Generate 16 bytes at a time */
        asm_ZucGenKeystream16B_4_avx(&state, pKeyStrArr);

        /* Point at the next 16 bytes of the key */
        for (i = 0; i < NUM_AVX_BUFS; i++)
                pKeyStrArr[i] = (uint32_t *) &keyStr[i][KEYSTR_ROUND_LEN];

        /* loop over the message bits */
        while (remainCommonBits >= keyStreamLengthInBits) {
                remainCommonBits -= keyStreamLengthInBits;
                numKeyStr++;
                /* Generate the next key stream 8 bytes or 16 bytes */
                if (!remainCommonBits)
                        asm_ZucGenKeystream8B_4_avx(&state, pKeyStrArr);
                else
                        asm_ZucGenKeystream16B_4_avx(&state, pKeyStrArr);
                for (i = 0; i < NUM_AVX_BUFS; i++) {
                        T[i] = asm_Eia3Round16BAVX(T[i], keyStr[i], pIn8[i]);
                        /* Copy the last keystream generated
                         * to the first 16 bytes */
                        memcpy(&keyStr[i][0], &keyStr[i][KEYSTR_ROUND_LEN],
                               KEYSTR_ROUND_LEN);
                        pIn8[i] = &pIn8[i][KEYSTR_ROUND_LEN];
                }
        }

        /* Process each packet separately for the remaining bits */
        for (i = 0; i < NUM_AVX_BUFS; i++) {
                const uint32_t N = lengthInBits[i] + (2 * ZUC_WORD_BITS);
                uint32_t L = ((N + 31) / ZUC_WORD_BITS) -
                             numKeyStr*(keyStreamLengthInBits / 32);
                uint32_t remainBits = lengthInBits[i] -
                                      numKeyStr*keyStreamLengthInBits;
                uint32_t *keyStr32 = (uint32_t *) keyStr[i];

                /* If remaining bits are more than 8 bytes, we need to generate
                 * at least 8B more of keystream, so we need to copy
                 * the zuc state to single packet state first */
                if (remainBits > (2*32)) {
                        singlePktState.lfsrState[0] = state.lfsrState[0][i];
                        singlePktState.lfsrState[1] = state.lfsrState[1][i];
                        singlePktState.lfsrState[2] = state.lfsrState[2][i];
                        singlePktState.lfsrState[3] = state.lfsrState[3][i];
                        singlePktState.lfsrState[4] = state.lfsrState[4][i];
                        singlePktState.lfsrState[5] = state.lfsrState[5][i];
                        singlePktState.lfsrState[6] = state.lfsrState[6][i];
                        singlePktState.lfsrState[7] = state.lfsrState[7][i];
                        singlePktState.lfsrState[8] = state.lfsrState[8][i];
                        singlePktState.lfsrState[9] = state.lfsrState[9][i];
                        singlePktState.lfsrState[10] = state.lfsrState[10][i];
                        singlePktState.lfsrState[11] = state.lfsrState[11][i];
                        singlePktState.lfsrState[12] = state.lfsrState[12][i];
                        singlePktState.lfsrState[13] = state.lfsrState[13][i];
                        singlePktState.lfsrState[14] = state.lfsrState[14][i];
                        singlePktState.lfsrState[15] = state.lfsrState[15][i];

                        singlePktState.fR1 = state.fR1[i];
                        singlePktState.fR2 = state.fR2[i];

                        singlePktState.bX0 = state.bX0[i];
                        singlePktState.bX1 = state.bX1[i];
                        singlePktState.bX2 = state.bX2[i];
                        singlePktState.bX3 = state.bX3[i];
                }

                while (remainBits >= keyStreamLengthInBits) {
                        remainBits -= keyStreamLengthInBits;
                        L -= (keyStreamLengthInBits / 32);

                        /* Generate the next key stream 8 bytes or 16 bytes */
                        if (!remainBits)
                                asm_ZucGenKeystream8B_avx(&keyStr32[4],
                                                          &singlePktState);
                        else
                                asm_ZucGenKeystream16B_avx(&keyStr32[4],
                                                           &singlePktState);
                        T[i] = asm_Eia3Round16BAVX(T[i], keyStr32, pIn8[i]);
                        /* Copy the last keystream generated
                         * to the first 16 bytes */
                        memcpy(keyStr32, &keyStr32[4], KEYSTR_ROUND_LEN);
                        pIn8[i] = &pIn8[i][KEYSTR_ROUND_LEN];
                }

                /*
                 * If remaining bits has more than 2 ZUC WORDS (double words),
                 * keystream needs to have up to another 2 ZUC WORDS (8B)
                 */

                if (remainBits > (2 * 32))
                        asm_ZucGenKeystream8B_avx(&keyStr32[4],
                                                  &singlePktState);

                uint32_t keyBlock = keyStr32[L - 1];

                T[i] ^= asm_Eia3RemainderAVX(keyStr32, pIn8[i], remainBits);
                T[i] ^= rotate_left(load_uint64(&keyStr32[remainBits / 32]),
                                 remainBits % 32);

                /* save the final MAC-I result */
                *(pMacI[i]) = bswap4(T[i] ^ keyBlock);
        }

#ifdef SAFE_DATA
        /* Clear sensitive data (in registers and stack) */
        clear_mem(keyStr, sizeof(keyStr));
        clear_mem(&singlePktState, sizeof(singlePktState));
        clear_mem(&state, sizeof(state));
        clear_mem(&keys, sizeof(keys));
#endif
}

void zuc_eia3_1_buffer_avx(const void *pKey,
                           const void *pIv,
                           const void *pBufferIn,
                           const uint32_t lengthInBits,
                           uint32_t *pMacI)
{
#ifndef LINUX
        DECLARE_ALIGNED(imb_uint128_t xmm_save[10], 16);

        SAVE_XMMS(xmm_save);
#endif
#ifdef SAFE_PARAM
        /* Check for NULL pointers */
        if (pKey == NULL || pIv == NULL || pBufferIn == NULL || pMacI == NULL)
                return;

        /* Check input data is in range of supported length */
        if (lengthInBits < ZUC_MIN_BITLEN || lengthInBits > ZUC_MAX_BITLEN)
                return;
#endif

        _zuc_eia3_1_buffer_avx(pKey, pIv, pBufferIn, lengthInBits, pMacI);

#ifdef SAFE_DATA
        CLEAR_SCRATCH_GPS();
        CLEAR_SCRATCH_SIMD_REGS();
#endif
#ifndef LINUX
        RESTORE_XMMS(xmm_save);
#endif
}

void zuc_eia3_4_buffer_job_avx(const void * const pKey[NUM_AVX_BUFS],
                               const void * const pIv[NUM_AVX_BUFS],
                               const void * const pBufferIn[NUM_AVX_BUFS],
                               uint32_t *pMacI[NUM_AVX_BUFS],
                               const uint16_t lengthInBits[NUM_AVX_BUFS],
                               const void * const job_in_lane[NUM_AVX_BUFS])
{
        unsigned int i;
        DECLARE_ALIGNED(ZucState4_t state, 64);
        DECLARE_ALIGNED(ZucState_t singlePktState, 64);
        /* Calculate the minimum input packet size */
        uint32_t bits1 = (lengthInBits[0] < lengthInBits[1] ?
                           lengthInBits[0] : lengthInBits[1]);
        uint32_t bits2 = (lengthInBits[2] < lengthInBits[3] ?
                           lengthInBits[2] : lengthInBits[3]);
        /* min number of bytes */
        uint32_t commonBits = (bits1 < bits2) ? bits1 : bits2;
        DECLARE_ALIGNED(uint8_t keyStr[NUM_AVX_BUFS][2*KEYSTR_ROUND_LEN], 64);
        /* structure to store the 4 keys */
        DECLARE_ALIGNED(ZucKey4_t keys, 64);
        /* structure to store the 4 IV's */
        DECLARE_ALIGNED(ZucIv4_t ivs, 64);
        const uint8_t *pIn8[NUM_AVX_BUFS] = {NULL};
        uint32_t remainCommonBits = commonBits;
        uint32_t numKeyStr = 0;
        uint32_t T[NUM_AVX_BUFS] = {0};
        const uint32_t keyStreamLengthInBits = KEYSTR_ROUND_LEN * 8;
        uint32_t *pKeyStrArr[NUM_AVX_BUFS] = {NULL};

        for (i = 0; i < NUM_AVX_BUFS; i++) {
                pIn8[i] = (const uint8_t *) pBufferIn[i];
                pKeyStrArr[i] = (uint32_t *) &keyStr[i][0];
                keys.pKeys[i] = pKey[i];
                ivs.pIvs[i] = pIv[i];
        }

        /* Need to set the LFSR state to zero */
        memset(&state, 0, sizeof(ZucState4_t));

        asm_ZucInitialization_4_avx( &keys,  &ivs, &state);

        /* Generate 16 bytes at a time */
        asm_ZucGenKeystream16B_4_avx(&state, pKeyStrArr);

        /* Point at the next 16 bytes of the key */
        for (i = 0; i < NUM_AVX_BUFS; i++)
                pKeyStrArr[i] = (uint32_t *) &keyStr[i][KEYSTR_ROUND_LEN];

        /* loop over the message bits */
        while (remainCommonBits >= keyStreamLengthInBits) {
                remainCommonBits -= keyStreamLengthInBits;
                numKeyStr++;
                /* Generate the next key stream 8 bytes or 16 bytes */
                if (!remainCommonBits)
                        asm_ZucGenKeystream8B_4_avx(&state, pKeyStrArr);
                else
                        asm_ZucGenKeystream16B_4_avx(&state, pKeyStrArr);
                for (i = 0; i < NUM_AVX_BUFS; i++) {
                        if (job_in_lane[i] == NULL)
                                continue;
                        T[i] = asm_Eia3Round16BAVX(T[i], keyStr[i], pIn8[i]);
                        /* Copy the last keystream generated
                         * to the first 16 bytes */
                        memcpy(&keyStr[i][0], &keyStr[i][KEYSTR_ROUND_LEN],
                               KEYSTR_ROUND_LEN);
                        pIn8[i] = &pIn8[i][KEYSTR_ROUND_LEN];
                }
        }

        /* Process each packet separately for the remaining bits */
        for (i = 0; i < NUM_AVX_BUFS; i++) {
                if (job_in_lane[i] == NULL)
                        continue;

                const uint32_t N = lengthInBits[i] + (2 * ZUC_WORD_BITS);
                uint32_t L = ((N + 31) / ZUC_WORD_BITS) -
                             numKeyStr*(keyStreamLengthInBits / 32);
                uint32_t remainBits = lengthInBits[i] -
                                      numKeyStr*keyStreamLengthInBits;
                uint32_t *keyStr32 = (uint32_t *) keyStr[i];

                /* If remaining bits are more than 8 bytes, we need to generate
                 * at least 8B more of keystream, so we need to copy
                 * the zuc state to single packet state first */
                if (remainBits > (2*32)) {
                        singlePktState.lfsrState[0] = state.lfsrState[0][i];
                        singlePktState.lfsrState[1] = state.lfsrState[1][i];
                        singlePktState.lfsrState[2] = state.lfsrState[2][i];
                        singlePktState.lfsrState[3] = state.lfsrState[3][i];
                        singlePktState.lfsrState[4] = state.lfsrState[4][i];
                        singlePktState.lfsrState[5] = state.lfsrState[5][i];
                        singlePktState.lfsrState[6] = state.lfsrState[6][i];
                        singlePktState.lfsrState[7] = state.lfsrState[7][i];
                        singlePktState.lfsrState[8] = state.lfsrState[8][i];
                        singlePktState.lfsrState[9] = state.lfsrState[9][i];
                        singlePktState.lfsrState[10] = state.lfsrState[10][i];
                        singlePktState.lfsrState[11] = state.lfsrState[11][i];
                        singlePktState.lfsrState[12] = state.lfsrState[12][i];
                        singlePktState.lfsrState[13] = state.lfsrState[13][i];
                        singlePktState.lfsrState[14] = state.lfsrState[14][i];
                        singlePktState.lfsrState[15] = state.lfsrState[15][i];

                        singlePktState.fR1 = state.fR1[i];
                        singlePktState.fR2 = state.fR2[i];

                        singlePktState.bX0 = state.bX0[i];
                        singlePktState.bX1 = state.bX1[i];
                        singlePktState.bX2 = state.bX2[i];
                        singlePktState.bX3 = state.bX3[i];
                }

                while (remainBits >= keyStreamLengthInBits) {
                        remainBits -= keyStreamLengthInBits;
                        L -= (keyStreamLengthInBits / 32);

                        /* Generate the next key stream 8 bytes or 16 bytes */
                        if (!remainBits)
                                asm_ZucGenKeystream8B_avx(&keyStr32[4],
                                                          &singlePktState);
                        else
                                asm_ZucGenKeystream16B_avx(&keyStr32[4],
                                                           &singlePktState);
                        T[i] = asm_Eia3Round16BAVX(T[i], keyStr32, pIn8[i]);
                        /* Copy the last keystream generated
                         * to the first 16 bytes */
                        memcpy(keyStr32, &keyStr32[4], KEYSTR_ROUND_LEN);
                        pIn8[i] = &pIn8[i][KEYSTR_ROUND_LEN];
                }

                /*
                 * If remaining bits has more than 2 ZUC WORDS (double words),
                 * keystream needs to have up to another 2 ZUC WORDS (8B)
                 */
                if (remainBits > (2 * 32))
                        asm_ZucGenKeystream8B_avx(&keyStr32[4],
                                                  &singlePktState);

                uint32_t keyBlock = keyStr32[L - 1];

                T[i] ^= asm_Eia3RemainderAVX(keyStr32, pIn8[i], remainBits);
                T[i] ^= rotate_left(load_uint64(&keyStr32[remainBits / 32]),
                                 remainBits % 32);

                /* save the final MAC-I result */
                *(pMacI[i]) = bswap4(T[i] ^ keyBlock);
        }

#ifdef SAFE_DATA
        /* Clear sensitive data (in registers and stack) */
        clear_mem(keyStr, sizeof(keyStr));
        clear_mem(&singlePktState, sizeof(singlePktState));
        clear_mem(&state, sizeof(state));
        clear_mem(&keys, sizeof(keys));
#endif
}

void zuc_eia3_n_buffer_avx(const void * const pKey[],
                           const void * const pIv[],
                           const void * const pBufferIn[],
                           const uint32_t lengthInBits[],
                           uint32_t *pMacI[],
                           const uint32_t numBuffers)
{
#ifndef LINUX
        DECLARE_ALIGNED(imb_uint128_t xmm_save[10], 16);

        SAVE_XMMS(xmm_save);
#endif

        unsigned int i;
        unsigned int packetCount = numBuffers;

#ifdef SAFE_PARAM
        /* Check for NULL pointers */
        if (pKey == NULL || pIv == NULL || pBufferIn == NULL ||
            lengthInBits == NULL || pMacI == NULL)
                return;

        for (i = 0; i < numBuffers; i++) {
                if (pKey[i] == NULL || pIv[i] == NULL ||
                    pBufferIn[i] == NULL || pMacI[i] == NULL)
                        return;

                /* Check input data is in range of supported length */
                if (lengthInBits[i] < ZUC_MIN_BITLEN ||
                    lengthInBits[i] > ZUC_MAX_BITLEN)
                        return;
        }
#endif
        i = 0;

        while(packetCount >= 4) {
                packetCount -=4;
                _zuc_eia3_4_buffer_avx(&pKey[i],
                                       &pIv[i],
                                       &pBufferIn[i],
                                       &lengthInBits[i],
                                       &pMacI[i]);
                i+=4;
        }

        while(packetCount--) {
                _zuc_eia3_1_buffer_avx(pKey[i],
                                       pIv[i],
                                       pBufferIn[i],
                                       lengthInBits[i],
                                       pMacI[i]);
                i++;
        }

#ifdef SAFE_DATA
        /* Clear sensitive data in registers */
        CLEAR_SCRATCH_GPS();
        CLEAR_SCRATCH_SIMD_REGS();
#endif
#ifndef LINUX
        RESTORE_XMMS(xmm_save);
#endif
}
