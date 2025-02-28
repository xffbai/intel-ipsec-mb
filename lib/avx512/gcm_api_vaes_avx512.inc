;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;  Copyright(c) 2021, Intel Corporation All rights reserved.
;
;  Redistribution and use in source and binary forms, with or without
;  modification, are permitted provided that the following conditions
;  are met:
;    * Redistributions of source code must retain the above copyright
;      notice, this list of conditions and the following disclaimer.
;    * Redistributions in binary form must reproduce the above copyright
;      notice, this list of conditions and the following disclaimer in
;      the documentation and/or other materials provided with the
;      distribution.
;    * Neither the name of Intel Corporation nor the names of its
;      contributors may be used to endorse or promote products derived
;      from this software without specific prior written permission.
;
;  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
;  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
;  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
;  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
;  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
;  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
;  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
;  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
;  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
;  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
;  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

%include "avx512/gcm_vaes_avx512.inc"

%ifndef GCM_API_VAES_AVX512_INC
%define GCM_API_VAES_AVX512_INC

section .text
default rel

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;void   aes_gcm_precomp_128_vaes_avx512 /
;       aes_gcm_precomp_192_vaes_avx512 /
;       aes_gcm_precomp_256_vaes_avx512
;       (struct gcm_key_data *key_data)
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
MKGLOBAL(FN_NAME(precomp,_),function,)
FN_NAME(precomp,_):
        endbranch64
;; Parameter is passed through register
%ifdef SAFE_PARAM
        ;; Check key_data != NULL
        cmp     arg1, 0
        jz      exit_precomp
%endif

        FUNC_SAVE

        vpxor   xmm6, xmm6
        ENCRYPT_SINGLE_BLOCK    arg1, xmm6              ; xmm6 = HashKey

        vpshufb  xmm6, [rel SHUF_MASK]
        ;;;;;;;;;;;;;;;  PRECOMPUTATION of HashKey<<1 mod poly from the HashKey;;;;;;;;;;;;;;;
        vmovdqa  xmm2, xmm6
        vpsllq   xmm6, xmm6, 1
        vpsrlq   xmm2, xmm2, 63
        vmovdqa  xmm1, xmm2
        vpslldq  xmm2, xmm2, 8
        vpsrldq  xmm1, xmm1, 8
        vpor     xmm6, xmm6, xmm2
        ;reduction
        vpshufd  xmm2, xmm1, 00100100b
        vpcmpeqd xmm2, [rel TWOONE]
        vpand    xmm2, xmm2, [rel POLY]
        vpxor    xmm6, xmm6, xmm2                       ; xmm6 holds the HashKey<<1 mod poly
        ;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
        vmovdqu  [arg1 + HashKey], xmm6                 ; store HashKey<<1 mod poly


        PRECOMPUTE arg1, xmm6, xmm0, xmm1, xmm2, xmm3, xmm4, xmm5, xmm7, xmm8

        FUNC_RESTORE
exit_precomp:

        ret

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;void   aes_gcm_enc_128_vaes_avx512 / aes_gcm_enc_192_vaes_avx512 / aes_gcm_enc_256_vaes_avx512
;       (const struct gcm_key_data *key_data,
;        struct gcm_context_data *context_data,
;        u8       *out,
;        const u8 *in,
;        u64      plaintext_len,
;        u8       *iv,
;        const u8 *aad,
;        u64      aad_len,
;        u8       *auth_tag,
;        u64      auth_tag_len);
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
MKGLOBAL(FN_NAME(enc,_),function,)
FN_NAME(enc,_):
        endbranch64
        FUNC_SAVE

%ifdef SAFE_PARAM
        ;; Check key_data != NULL
        cmp     arg1, 0
        jz      exit_enc

        ;; Check context_data != NULL
        cmp     arg2, 0
        jz      exit_enc

        ;; Check IV != NULL
        cmp     arg6, 0
        jz      exit_enc

        ;; Check auth_tag != NULL
        cmp     arg9, 0
        jz      exit_enc

        ;; Check auth_tag_len == 0 or > 16
        cmp     arg10, 0
        jz      exit_enc

        cmp     arg10, 16
        ja      exit_enc

        ;; Check if plaintext_len == 0
        cmp     arg5, 0
        jz      skip_in_out_check_enc

        ;; Check out != NULL (plaintext_len != 0)
        cmp     arg3, 0
        jz      exit_enc

        ;; Check in != NULL (plaintext_len != 0)
        cmp     arg4, 0
        jz      exit_enc

skip_in_out_check_enc:
        ;; Check if aad_len == 0
        cmp     arg8, 0
        jz      skip_aad_check_enc

        ;; Check aad != NULL (aad_len != 0)
        cmp     arg7, 0
        jz      exit_enc

skip_aad_check_enc:
%endif
        GCM_INIT arg1, arg2, arg6, arg7, arg8, r10, r11, r12, k1, xmm14, xmm2, \
                zmm1, zmm3, zmm4, zmm5, zmm6, zmm7, zmm8, zmm9, zmm10, zmm11, \
                zmm12, zmm13, zmm15, zmm16, zmm17, zmm18, zmm19, zmm20, single_call
        GCM_ENC_DEC  arg1, arg2, arg3, arg4, arg5, ENC, single_call
        GCM_COMPLETE arg1, arg2, arg9, arg10, single_call, k1, r10, r11, r12

exit_enc:
        FUNC_RESTORE
        ret

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;void   aes_gcm_dec_128_vaes_avx512 / aes_gcm_dec_192_vaes_avx512 / aes_gcm_dec_256_vaes_avx512
;       (const struct gcm_key_data *key_data,
;        struct gcm_context_data *context_data,
;        u8       *out,
;        const u8 *in,
;        u64      plaintext_len,
;        u8       *iv,
;        const u8 *aad,
;        u64      aad_len,
;        u8       *auth_tag,
;        u64      auth_tag_len);
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
MKGLOBAL(FN_NAME(dec,_),function,)
FN_NAME(dec,_):
        endbranch64
        FUNC_SAVE

%ifdef SAFE_PARAM
        ;; Check key_data != NULL
        cmp     arg1, 0
        jz      exit_dec

        ;; Check context_data != NULL
        cmp     arg2, 0
        jz      exit_dec

        ;; Check IV != NULL
        cmp     arg6, 0
        jz      exit_dec

        ;; Check auth_tag != NULL
        cmp     arg9, 0
        jz      exit_dec

        ;; Check auth_tag_len == 0 or > 16
        cmp     arg10, 0
        jz      exit_dec

        cmp     arg10, 16
        ja      exit_dec

        ;; Check if plaintext_len == 0
        cmp     arg5, 0
        jz      skip_in_out_check_dec

        ;; Check out != NULL (plaintext_len != 0)
        cmp     arg3, 0
        jz      exit_dec

        ;; Check in != NULL (plaintext_len != 0)
        cmp     arg4, 0
        jz      exit_dec

skip_in_out_check_dec:
        ;; Check if aad_len == 0
        cmp     arg8, 0
        jz      skip_aad_check_dec

        ;; Check aad != NULL (aad_len != 0)
        cmp     arg7, 0
        jz      exit_dec

skip_aad_check_dec:
%endif
        GCM_INIT arg1, arg2, arg6, arg7, arg8, r10, r11, r12, k1, xmm14, xmm2, \
                zmm1, zmm3, zmm4, zmm5, zmm6, zmm7, zmm8, zmm9, zmm10, zmm11, \
                zmm12, zmm13, zmm15, zmm16, zmm17, zmm18, zmm19, zmm20, single_call
        GCM_ENC_DEC  arg1, arg2, arg3, arg4, arg5, DEC, single_call
        GCM_COMPLETE arg1, arg2, arg9, arg10, single_call, k1, r10, r11, r12

exit_dec:
        FUNC_RESTORE
        ret

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;void   aes_gcm_enc_var_iv_128_vaes_avx512 / aes_gcm_enc_var_iv_192_vaes_avx512 /
;       aes_gcm_enc_var_iv_256_vaes_avx512
;       (const struct gcm_key_data *key_data,
;        struct gcm_context_data *context_data,
;        u8        *out,
;        const u8  *in,
;        u64       plaintext_len,
;        u8        *iv,
;        const u64 iv_len,
;        const u8  *aad,
;        const u64 aad_len,
;        u8        *auth_tag,
;        const u64 auth_tag_len);
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
MKGLOBAL(FN_NAME(enc_var_iv,_),function,)
FN_NAME(enc_var_iv,_):
        endbranch64
        FUNC_SAVE

%ifdef SAFE_PARAM
        ;; Check key_data != NULL
        cmp     arg1, 0
        jz      exit_enc_IV

        ;; Check context_data != NULL
        cmp     arg2, 0
        jz      exit_enc_IV

        ;; Check IV != NULL
        cmp     arg6, 0
        jz      exit_enc_IV

        ;; Check IV len != 0
        cmp     arg7, 0
        jz      exit_enc_IV

        ;; Check auth_tag != NULL
        cmp     arg10, 0
        jz      exit_enc_IV

        ;; Check auth_tag_len == 0 or > 16
        cmp     arg11, 0
        jz      exit_enc_IV

        cmp     arg11, 16
        ja      exit_enc_IV

        ;; Check if plaintext_len == 0
        cmp     arg5, 0
        jz      skip_in_out_check_enc_IV

        ;; Check out != NULL (plaintext_len != 0)
        cmp     arg3, 0
        jz      exit_enc_IV

        ;; Check in != NULL (plaintext_len != 0)
        cmp     arg4, 0
        jz      exit_enc_IV

skip_in_out_check_enc_IV:
        ;; Check if aad_len == 0
        cmp     arg9, 0
        jz      skip_aad_check_enc_IV

        ;; Check aad != NULL (aad_len != 0)
        cmp     arg8, 0
        jz      exit_enc_IV

skip_aad_check_enc_IV:
%endif
        cmp     arg7, 12
        je      iv_len_12_enc_IV

        GCM_INIT arg1, arg2, arg6, arg8, arg9, r10, r11, r12, k1, xmm14, xmm2, \
                zmm1, zmm11, zmm3, zmm4, zmm5, zmm6, zmm7, zmm8, zmm9, zmm10, \
                zmm12, zmm13, zmm15, zmm16, zmm17, zmm18, zmm19, zmm20, single_call, arg7
        jmp     skip_iv_len_12_enc_IV

iv_len_12_enc_IV:
        GCM_INIT arg1, arg2, arg6, arg8, arg9, r10, r11, r12, k1, xmm14, xmm2, \
                zmm1, zmm11, zmm3, zmm4, zmm5, zmm6, zmm7, zmm8, zmm9, zmm10, \
                zmm12, zmm13, zmm15, zmm16, zmm17, zmm18, zmm19, zmm20, single_call

skip_iv_len_12_enc_IV:
        GCM_ENC_DEC  arg1, arg2, arg3, arg4, arg5, ENC, single_call
        GCM_COMPLETE arg1, arg2, arg10, arg11, single_call, k1, r10, r11, r12

exit_enc_IV:
        FUNC_RESTORE
        ret

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;void   aes_gcm_dec_128_vaes_avx512 / aes_gcm_dec_192_vaes_avx512 /
;       aes_gcm_dec_256_vaes_avx512
;       (const struct gcm_key_data *key_data,
;        struct gcm_context_data *context_data,
;        u8        *out,
;        const u8  *in,
;        u64       plaintext_len,
;        u8        *iv,
;        const u64 iv_len,
;        const u8  *aad,
;        const u64 aad_len,
;        u8        *auth_tag,
;        const u64 auth_tag_len);
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
MKGLOBAL(FN_NAME(dec_var_iv,_),function,)
FN_NAME(dec_var_iv,_):
        endbranch64
        FUNC_SAVE

%ifdef SAFE_PARAM
        ;; Check key_data != NULL
        cmp     arg1, 0
        jz      exit_dec_IV

        ;; Check context_data != NULL
        cmp     arg2, 0
        jz      exit_dec_IV

        ;; Check IV != NULL
        cmp     arg6, 0
        jz      exit_dec_IV

        ;; Check IV len != 0
        cmp     arg7, 0
        jz      exit_dec_IV

        ;; Check auth_tag != NULL
        cmp     arg10, 0
        jz      exit_dec_IV

        ;; Check auth_tag_len == 0 or > 16
        cmp     arg11, 0
        jz      exit_dec_IV

        cmp     arg11, 16
        ja      exit_dec_IV

        ;; Check if plaintext_len == 0
        cmp     arg5, 0
        jz      skip_in_out_check_dec_IV

        ;; Check out != NULL (plaintext_len != 0)
        cmp     arg3, 0
        jz      exit_dec_IV

        ;; Check in != NULL (plaintext_len != 0)
        cmp     arg4, 0
        jz      exit_dec_IV

skip_in_out_check_dec_IV:
        ;; Check if aad_len == 0
        cmp     arg9, 0
        jz      skip_aad_check_dec_IV

        ;; Check aad != NULL (aad_len != 0)
        cmp     arg8, 0
        jz      exit_dec_IV

skip_aad_check_dec_IV:
%endif
        cmp     arg7, 12
        je      iv_len_12_dec_IV

        GCM_INIT arg1, arg2, arg6, arg8, arg9, r10, r11, r12, k1, xmm14, xmm2, \
                zmm1, zmm11, zmm3, zmm4, zmm5, zmm6, zmm7, zmm8, zmm9, zmm10, zmm12, \
                zmm13, zmm15, zmm16, zmm17, zmm18, zmm19, zmm20, single_call, arg7
        jmp     skip_iv_len_12_dec_IV

iv_len_12_dec_IV:
        GCM_INIT arg1, arg2, arg6, arg8, arg9, r10, r11, r12, k1, xmm14, xmm2, \
                zmm1, zmm11, zmm3, zmm4, zmm5, zmm6, zmm7, zmm8, zmm9, zmm10, \
                zmm12, zmm13, zmm15, zmm16, zmm17, zmm18, zmm19, zmm20, single_call

skip_iv_len_12_dec_IV:
        GCM_ENC_DEC  arg1, arg2, arg3, arg4, arg5, DEC, single_call
        GCM_COMPLETE arg1, arg2, arg10, arg11, single_call, k1, r10, r11, r12

exit_dec_IV:
        FUNC_RESTORE
        ret


%ifdef LINUX
section .note.GNU-stack noalloc noexec nowrite progbits
%endif

%endif ; GCM_API_VAES_AVX512_INC
