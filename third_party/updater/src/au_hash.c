/*
 * au_hash.c — 文件 SHA-256，走 CNG（bcrypt.lib），无第三方加密库。
 */
#include "au_internal.h"
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef NT_SUCCESS
#define NT_SUCCESS(s) ((s) >= 0)
#endif

au_err_t au_sha256_file(const wchar_t* path, char out_hex[65]) {
    au_err_t rc = AU_ERR_HASH;
    BCRYPT_ALG_HANDLE hAlg = NULL;
    BCRYPT_HASH_HANDLE hHash = NULL;
    unsigned char* hashObj = NULL;
    unsigned char digest[32];
    HANDLE hFile = INVALID_HANDLE_VALUE;

    if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, NULL, 0)))
        return AU_ERR_HASH;

    DWORD objLen = 0, cb = 0;
    if (!NT_SUCCESS(BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH,
                                      (PUCHAR)&objLen, sizeof objLen, &cb, 0))) goto done;
    hashObj = (unsigned char*)malloc(objLen);
    if (!hashObj) { rc = AU_ERR_NOMEM; goto done; }

    if (!NT_SUCCESS(BCryptCreateHash(hAlg, &hHash, hashObj, objLen, NULL, 0, 0))) goto done;

    hFile = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) { rc = AU_ERR_IO; goto done; }

    unsigned char buf[65536];
    DWORD nread;
    for (;;) {
        if (!ReadFile(hFile, buf, sizeof buf, &nread, NULL)) { rc = AU_ERR_IO; goto done; }
        if (nread == 0) break;
        if (!NT_SUCCESS(BCryptHashData(hHash, buf, nread, 0))) goto done;
    }

    if (!NT_SUCCESS(BCryptFinishHash(hHash, digest, sizeof digest, 0))) goto done;

    static const char* hexd = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out_hex[i*2]   = hexd[digest[i] >> 4];
        out_hex[i*2+1] = hexd[digest[i] & 0xF];
    }
    out_hex[64] = '\0';
    rc = AU_OK;

done:
    if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
    if (hHash) BCryptDestroyHash(hHash);
    if (hashObj) free(hashObj);
    if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);
    return rc;
}
