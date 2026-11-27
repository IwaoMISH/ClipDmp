/*
 * ClipDmp.cpp
 *
 * Clipboard image dumper for Visual C++ 6.0
 *
 * - Polls the clipboard until an image format is found.
 * - Saves original clipboard data as .bin where possible.
 * - Saves rasterized/decoded image data as 32-bit BITMAPV4HEADER BMP.
 * - Does not use the clipboard viewer chain.
 * - Uses Win32/GDI APIs only; no MFC, GDI+, or third-party libraries.
 *
 * Supported clipboard formats:
 *   CF_DIB, CF_DIBV5, CF_BITMAP, registered "PNG",
 *   CF_ENHMETAFILE, CF_METAFILEPICT.
 *
 * Notes:
 *   For CF_BITMAP, the clipboard contains an HBITMAP object, not a byte
 *   stream. Its .bin file is a serialized 32-bit DIB representation.
 *   For metafiles, .bin contains the native metafile bits.
 *   PNG is saved as .bin only; this program does not decode PNG.
 */

#pragma warning(disable:4996)

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <tchar.h>
#include <direct.h>
#include <conio.h>

#ifndef BI_ALPHABITFIELDS
#define BI_ALPHABITFIELDS 6L
#endif

#ifndef CF_DIBV5
#define CF_DIBV5 17
#endif

#ifndef INVALID_FILE_ATTRIBUTES
#define INVALID_FILE_ATTRIBUTES ((DWORD)0xFFFFFFFFUL)
#endif

/* BITMAPV4HEADER is not declared by some VC6-era headers. */
#ifndef LCS_sRGB
#define LCS_sRGB 'sRGB'
#endif

#pragma pack(push, 4)
typedef struct tagMYBITMAPV4HEADER {
    DWORD       bV4Size;
    LONG        bV4Width;
    LONG        bV4Height;
    WORD        bV4Planes;
    WORD        bV4BitCount;
    DWORD       bV4V4Compression;
    DWORD       bV4SizeImage;
    LONG        bV4XPelsPerMeter;
    LONG        bV4YPelsPerMeter;
    DWORD       bV4ClrUsed;
    DWORD       bV4ClrImportant;
    DWORD       bV4RedMask;
    DWORD       bV4GreenMask;
    DWORD       bV4BlueMask;
    DWORD       bV4AlphaMask;
    DWORD       bV4CSType;
    LONG        bV4Endpoints[9];
    DWORD       bV4GammaRed;
    DWORD       bV4GammaGreen;
    DWORD       bV4GammaBlue;
} MYBITMAPV4HEADER;
#pragma pack(pop)

typedef struct tagPIXEL32 {
    BYTE b;
    BYTE g;
    BYTE r;
    BYTE a;
} PIXEL32;

static const WORD BMP_MARKER = (WORD)('M' << 8) | (WORD)'B';

static BOOL MakeOutputDirectory(TCHAR* outDir, DWORD cch)
{
    TCHAR tempPath[MAX_PATH];
    DWORD n = ::GetTempPath(MAX_PATH, tempPath);
    if (n == 0 || n >= MAX_PATH) {
        return FALSE;
    }

    if (_tcslen(tempPath) + _tcslen(_T("clip_dump")) + 2 >= cch) {
        return FALSE;
    }

    _tcscpy(outDir, tempPath);
    if (outDir[_tcslen(outDir) - 1] != _T('\\')) {
        _tcscat(outDir, _T("\\"));
    }
    _tcscat(outDir, _T("clip_dump"));

    if (_tmkdir(outDir) != 0) {
        DWORD attr = ::GetFileAttributes(outDir);
        if (attr == INVALID_FILE_ATTRIBUTES ||
            (attr & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            return FALSE;
        }
    }
    return TRUE;
}

static void MakeTimeStamp(TCHAR* stamp, DWORD cch)
{
    SYSTEMTIME st;
    ::GetLocalTime(&st);
    _stprintf(stamp, _T("%04u%02u%02u_%02u%02u%02u"),
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond);
    stamp[cch - 1] = 0;
}

static BOOL MakeFileName(TCHAR* path, DWORD cch,
                         const TCHAR* dir, const TCHAR* stamp,
                         const TCHAR* formatName, const TCHAR* ext)
{
    int n = _sntprintf(path, cch - 1, _T("%s\\%s_%s.%s"),
                       dir, stamp, formatName, ext);
    path[cch - 1] = 0;
    return n >= 0 && (DWORD)n < cch;
}

static BOOL WriteBytes(const TCHAR* path, const void* data, DWORD size)
{
    FILE* fp = _tfopen(path, _T("wb"));
    if (fp == NULL) {
        _tprintf(_T("Cannot create: %s\n"), path);
        return FALSE;
    }

    BOOL ok = TRUE;
    if (size != 0 && fwrite(data, 1, size, fp) != size) {
        ok = FALSE;
    }
    if (fclose(fp) != 0) {
        ok = FALSE;
    }
    if (!ok) {
        _tprintf(_T("Write error: %s\n"), path);
    }
    return ok;
}

static BOOL SaveV4BMP(const TCHAR* path, LONG width, LONG height,
                      const PIXEL32* pixels)
{
    MYBITMAPV4HEADER v4;
    BITMAPFILEHEADER fh;
    DWORD imageSize;
    DWORD fileSize;
    FILE* fp;

    if (width <= 0 || height <= 0 || pixels == NULL) {
        return FALSE;
    }

    if ((DWORD)width > 0xFFFFFFFFUL / 4UL / (DWORD)height) {
        return FALSE;
    }
    imageSize = (DWORD)width * (DWORD)height * 4UL;
    if (imageSize > 0xFFFFFFFFUL - sizeof(BITMAPFILEHEADER) -
                    sizeof(MYBITMAPV4HEADER)) {
        return FALSE;
    }
    fileSize = sizeof(BITMAPFILEHEADER) + sizeof(MYBITMAPV4HEADER) +
               imageSize;

    memset(&v4, 0, sizeof(v4));
    v4.bV4Size = sizeof(v4);
    v4.bV4Width = width;
    v4.bV4Height = height;       /* bottom-up */
    v4.bV4Planes = 1;
    v4.bV4BitCount = 32;
    v4.bV4V4Compression = BI_BITFIELDS;
    v4.bV4SizeImage = imageSize;
    v4.bV4RedMask = 0x00FF0000;
    v4.bV4GreenMask = 0x0000FF00;
    v4.bV4BlueMask = 0x000000FF;
    v4.bV4AlphaMask = 0xFF000000;
    v4.bV4CSType = LCS_sRGB;

    memset(&fh, 0, sizeof(fh));
    fh.bfType = BMP_MARKER;
    fh.bfSize = fileSize;
    fh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(MYBITMAPV4HEADER);

    fp = _tfopen(path, _T("wb"));
    if (fp == NULL) {
        _tprintf(_T("Cannot create: %s\n"), path);
        return FALSE;
    }

    {
        BOOL ok = TRUE;
        if (fwrite(&fh, 1, sizeof(fh), fp) != sizeof(fh)) ok = FALSE;
        if (ok && fwrite(&v4, 1, sizeof(v4), fp) != sizeof(v4)) ok = FALSE;
        if (ok && fwrite(pixels, 1, imageSize, fp) != imageSize) ok = FALSE;
        if (fclose(fp) != 0) ok = FALSE;
        if (!ok) _tprintf(_T("Write error: %s\n"), path);
        return ok;
    }
}

static DWORD MaskShift(DWORD mask)
{
    DWORD shift = 0;
    if (mask == 0) return 0;
    while ((mask & 1) == 0) {
        mask >>= 1;
        ++shift;
    }
    return shift;
}

static BYTE MaskToByte(DWORD value, DWORD mask)
{
    DWORD shift;
    DWORD maxValue;
    DWORD v;

    if (mask == 0) return 255;
    shift = MaskShift(mask);
    maxValue = mask >> shift;
    v = (value & mask) >> shift;
    if (maxValue == 0) return 0;
    return (BYTE)((v * 255UL + maxValue / 2) / maxValue);
}

/*
 * Converts CF_DIB/CF_DIBV5 memory to bottom-up BGRA.
 * Supports 24-bit BI_RGB and 32-bit BI_RGB/BI_BITFIELDS.
 * For a 40-byte BI_RGB 32-bit DIB, the fourth byte is reserved,
 * so output Alpha is set to 255.
 */
static BOOL DIBToPixels(const BYTE* data, DWORD dataSize,
                        LONG* outW, LONG* outH, PIXEL32** outPixels)
{
    DWORD headerSize;
    LONG width, signedHeight, height;
    WORD planes, bitCount;
    DWORD compression, clrUsed;
    DWORD redMask = 0, greenMask = 0, blueMask = 0, alphaMask = 0;
    DWORD paletteBytes = 0;
    DWORD bitsOffset;
    DWORD rowBytes;
    DWORD y, x;
    PIXEL32* pixels;

    *outPixels = NULL;
    *outW = 0;
    *outH = 0;

    if (data == NULL || dataSize < sizeof(BITMAPINFOHEADER)) return FALSE;

    headerSize = *(const DWORD*)data;
    if (headerSize < sizeof(BITMAPINFOHEADER) || headerSize > dataSize) {
        return FALSE;
    }

    {
        const BITMAPINFOHEADER* bih = (const BITMAPINFOHEADER*)data;
        width = bih->biWidth;
        signedHeight = bih->biHeight;
        planes = bih->biPlanes;
        bitCount = bih->biBitCount;
        compression = bih->biCompression;
        clrUsed = bih->biClrUsed;
    }

    if (width <= 0 || signedHeight == 0 || signedHeight == (LONG)0x80000000 ||
        planes != 1) return FALSE;
    height = signedHeight < 0 ? -signedHeight : signedHeight;
    if (width > 32768 || height > 32768) return FALSE;
    if (bitCount != 24 && bitCount != 32) return FALSE;
    if (compression != BI_RGB && compression != BI_BITFIELDS &&
        compression != BI_ALPHABITFIELDS) return FALSE;

    if (headerSize >= 108) {
        const MYBITMAPV4HEADER* v4 = (const MYBITMAPV4HEADER*)data;
        redMask = v4->bV4RedMask;
        greenMask = v4->bV4GreenMask;
        blueMask = v4->bV4BlueMask;
        alphaMask = v4->bV4AlphaMask;
    } else if (compression == BI_BITFIELDS ||
               compression == BI_ALPHABITFIELDS) {
        DWORD maskCount = compression == BI_ALPHABITFIELDS ? 4 : 3;
        if (headerSize + maskCount * sizeof(DWORD) > dataSize) return FALSE;
        redMask = *(const DWORD*)(data + headerSize);
        greenMask = *(const DWORD*)(data + headerSize + 4);
        blueMask = *(const DWORD*)(data + headerSize + 8);
        if (maskCount == 4) alphaMask = *(const DWORD*)(data + headerSize + 12);
    }

    if (bitCount == 32 && compression == BI_RGB) {
        if (headerSize >= 108 && alphaMask != 0) {
            /* Explicit alpha mask is present. */
        } else {
            alphaMask = 0;
        }
    }

    bitsOffset = headerSize;
    if (headerSize == 40 &&
        (compression == BI_BITFIELDS || compression == BI_ALPHABITFIELDS)) {
        bitsOffset += (compression == BI_ALPHABITFIELDS ? 4 : 3) * 4;
    }
    if (bitCount <= 8) {
        paletteBytes = (clrUsed ? clrUsed : (1UL << bitCount)) * 4UL;
        bitsOffset += paletteBytes;
    }
    if (bitsOffset > dataSize) return FALSE;

    rowBytes = ((DWORD)width * bitCount + 31UL) / 32UL * 4UL;
    if (rowBytes == 0 || (DWORD)height > (dataSize - bitsOffset) / rowBytes) {
        return FALSE;
    }
    if ((DWORD)width > 0xFFFFFFFFUL / 4UL / (DWORD)height) return FALSE;

    pixels = (PIXEL32*)malloc((size_t)width * (size_t)height * sizeof(PIXEL32));
    if (pixels == NULL) return FALSE;

    for (y = 0; y < (DWORD)height; ++y) {
        DWORD srcY = signedHeight > 0 ? y : ((DWORD)height - 1 - y);
        const BYTE* row = data + bitsOffset + srcY * rowBytes;
        PIXEL32* dst = pixels + y * (DWORD)width;

        for (x = 0; x < (DWORD)width; ++x) {
            if (bitCount == 24) {
                dst[x].b = row[x * 3 + 0];
                dst[x].g = row[x * 3 + 1];
                dst[x].r = row[x * 3 + 2];
                dst[x].a = 255;
            } else {
                DWORD value = *(const DWORD*)(row + x * 4);
                if (compression == BI_RGB && alphaMask == 0) {
                    dst[x].b = (BYTE)(value & 0xFF);
                    dst[x].g = (BYTE)((value >> 8) & 0xFF);
                    dst[x].r = (BYTE)((value >> 16) & 0xFF);
                    dst[x].a = 255;
                } else {
                    if (redMask == 0) redMask = 0x00FF0000;
                    if (greenMask == 0) greenMask = 0x0000FF00;
                    if (blueMask == 0) blueMask = 0x000000FF;
                    dst[x].r = MaskToByte(value, redMask);
                    dst[x].g = MaskToByte(value, greenMask);
                    dst[x].b = MaskToByte(value, blueMask);
                    dst[x].a = alphaMask ? MaskToByte(value, alphaMask) : 255;
                }
            }
        }
    }

    *outW = width;
    *outH = height;
    *outPixels = pixels;
    return TRUE;
}

static BOOL SaveDIBFormat(HGLOBAL hGlobal, const TCHAR* dir,
                          const TCHAR* stamp, const TCHAR* formatName)
{
    SIZE_T size;
    const BYTE* p;
    TCHAR path[MAX_PATH * 2];
    PIXEL32* pixels = NULL;
    LONG width, height;
    BOOL ok = TRUE;

    size = ::GlobalSize(hGlobal);
    if (size == 0 || size > 0xFFFFFFFFUL) return FALSE;
    p = (const BYTE*)::GlobalLock(hGlobal);
    if (p == NULL) return FALSE;

    if (MakeFileName(path, MAX_PATH * 2, dir, stamp, formatName, _T("bin"))) {
        if (!WriteBytes(path, p, (DWORD)size)) ok = FALSE;
    } else {
        ok = FALSE;
    }

    if (DIBToPixels(p, (DWORD)size, &width, &height, &pixels)) {
        if (MakeFileName(path, MAX_PATH * 2, dir, stamp, formatName, _T("bmp"))) {
            if (!SaveV4BMP(path, width, height, pixels)) ok = FALSE;
        } else {
            ok = FALSE;
        }
        free(pixels);
    } else {
        _tprintf(_T("%s: DIB conversion not supported; .bin only\n"), formatName);
    }

    ::GlobalUnlock(hGlobal);
    return ok;
}

static BOOL SaveBitmapFormat(HBITMAP hBitmap, const TCHAR* dir,
                             const TCHAR* stamp)
{
    BITMAP bm;
    BITMAPINFO bmi;
    BYTE* bits = NULL;
    PIXEL32* pixels = NULL;
    DWORD imageSize;
    TCHAR path[MAX_PATH * 2];
    BOOL ok = TRUE;
    HDC hdc;

    if (hBitmap == NULL || ::GetObject(hBitmap, sizeof(bm), &bm) == 0) {
        return FALSE;
    }
    if (bm.bmWidth <= 0 || bm.bmHeight <= 0 ||
        bm.bmWidth > 32768 || bm.bmHeight > 32768) return FALSE;
    if ((DWORD)bm.bmWidth > 0xFFFFFFFFUL / 4UL / (DWORD)bm.bmHeight) return FALSE;

    imageSize = (DWORD)bm.bmWidth * (DWORD)bm.bmHeight * 4UL;
    bits = (BYTE*)malloc(imageSize);
    if (bits == NULL) return FALSE;

    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = bm.bmWidth;
    bmi.bmiHeader.biHeight = -bm.bmHeight; /* top-down */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    bmi.bmiHeader.biSizeImage = imageSize;

    hdc = ::GetDC(NULL);
    if (hdc == NULL) {
        free(bits);
        return FALSE;
    }
    if (::GetDIBits(hdc, hBitmap, 0, (UINT)bm.bmHeight,
                    bits, &bmi, DIB_RGB_COLORS) == 0) {
        ::ReleaseDC(NULL, hdc);
        free(bits);
        return FALSE;
    }
    ::ReleaseDC(NULL, hdc);

    /* HBITMAP does not define a portable Alpha channel. */
    pixels = (PIXEL32*)bits;
    {
        DWORD i, count = (DWORD)bm.bmWidth * (DWORD)bm.bmHeight;
        for (i = 0; i < count; ++i) pixels[i].a = 255;
    }

    if (MakeFileName(path, MAX_PATH * 2, dir, stamp, _T("CF_BITMAP"), _T("bin"))) {
        /* Serialized DIB payload: BITMAPINFOHEADER followed by BGRA pixels. */
        FILE* fp = _tfopen(path, _T("wb"));
        if (fp != NULL) {
            BITMAPINFOHEADER bih = bmi.bmiHeader;
            bih.biHeight = -bm.bmHeight; /* top-down serialized DIB */
            bih.biCompression = BI_RGB;
            if (fwrite(&bih, 1, sizeof(bih), fp) != sizeof(bih) ||
                fwrite(bits, 1, imageSize, fp) != imageSize) ok = FALSE;
            if (fclose(fp) != 0) ok = FALSE;
        } else ok = FALSE;
    }

    if (MakeFileName(path, MAX_PATH * 2, dir, stamp, _T("CF_BITMAP"), _T("bmp"))) {
        /* Convert top-down GetDIBits output to bottom-up BMP row order. */
        PIXEL32* bottomUp = (PIXEL32*)malloc(imageSize);
        if (bottomUp != NULL) {
            LONG y;
            for (y = 0; y < bm.bmHeight; ++y) {
                memcpy(bottomUp + y * bm.bmWidth,
                       pixels + (bm.bmHeight - 1 - y) * bm.bmWidth,
                       bm.bmWidth * sizeof(PIXEL32));
            }
            if (!SaveV4BMP(path, bm.bmWidth, bm.bmHeight, bottomUp)) ok = FALSE;
            free(bottomUp);
        } else ok = FALSE;
    }

    free(bits);
    return ok;
}

static BOOL SaveEnhMetaFile(HENHMETAFILE hemf, const TCHAR* dir,
                            const TCHAR* stamp)
{
    UINT size;
    BYTE* data;
    TCHAR path[MAX_PATH * 2];
    ENHMETAHEADER header;
    RECT rc;
    HDC screenDC, memDC;
    HBITMAP dib;
    HBITMAP oldBitmap;
    BITMAPINFO bmi;
    PIXEL32* pixels;
    LONG width, height;
    BOOL ok = TRUE;

    size = ::GetEnhMetaFileBits(hemf, 0, NULL);
    if (size == 0) return FALSE;
    data = (BYTE*)malloc(size);
    if (data == NULL) return FALSE;
    if (::GetEnhMetaFileBits(hemf, size, data) != size) {
        free(data);
        return FALSE;
    }
    if (MakeFileName(path, MAX_PATH * 2, dir, stamp, _T("CF_ENHMETAFILE"), _T("bin"))) {
        if (!WriteBytes(path, data, size)) ok = FALSE;
    }

    memset(&header, 0, sizeof(header));
    header.iType = 1;
    if (::GetEnhMetaFileHeader(hemf, sizeof(header), &header) == 0) {
        free(data);
        return ok;
    }

    width = header.rclBounds.right - header.rclBounds.left;
    height = header.rclBounds.bottom - header.rclBounds.top;
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192) {
        free(data);
        return ok;
    }

    pixels = NULL; /* CreateDIBSection supplies this pointer. */
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    screenDC = ::GetDC(NULL);
    memDC = ::CreateCompatibleDC(screenDC);
    dib = ::CreateDIBSection(screenDC, &bmi, DIB_RGB_COLORS,
                             (void**)&pixels, NULL, 0);
    if (memDC == NULL || dib == NULL) {
        if (dib) ::DeleteObject(dib);
        if (memDC) ::DeleteDC(memDC);
        if (screenDC) ::ReleaseDC(NULL, screenDC);
        free(data);
        return FALSE;
    }
    oldBitmap = (HBITMAP)::SelectObject(memDC, dib);
    memset(pixels, 255, (size_t)width * height * sizeof(PIXEL32));
    SetRect(&rc, 0, 0, width, height);
    if (!::PlayEnhMetaFile(memDC, hemf, &rc)) ok = FALSE;
    {
        DWORD i, count = (DWORD)width * (DWORD)height;
        for (i = 0; i < count; ++i) pixels[i].a = 255;
    }
    if (MakeFileName(path, MAX_PATH * 2, dir, stamp, _T("CF_ENHMETAFILE"), _T("bmp"))) {
        /* DIB section is top-down; SaveV4BMP expects bottom-up. */
        PIXEL32* bottomUp = (PIXEL32*)malloc((size_t)width * height * 4);
        if (bottomUp != NULL) {
            LONG y;
            for (y = 0; y < height; ++y)
                memcpy(bottomUp + y * width,
                       pixels + (height - 1 - y) * width,
                       width * sizeof(PIXEL32));
            if (!SaveV4BMP(path, width, height, bottomUp)) ok = FALSE;
            free(bottomUp);
        } else ok = FALSE;
    }

    ::SelectObject(memDC, oldBitmap);
    ::DeleteObject(dib);
    ::DeleteDC(memDC);
    ::ReleaseDC(NULL, screenDC);
    free(data);
    return ok;
}

static BOOL SaveMetaFilePict(HGLOBAL hGlobal, const TCHAR* dir,
                             const TCHAR* stamp)
{
    METAFILEPICT* mfp;
    UINT size;
    BYTE* data;
    TCHAR path[MAX_PATH * 2];
    HMETAFILE hmf;

    if (hGlobal == NULL) return FALSE;
    mfp = (METAFILEPICT*)::GlobalLock(hGlobal);
    if (mfp == NULL) return FALSE;
    hmf = mfp->hMF;
    size = ::GetMetaFileBitsEx(hmf, 0, NULL);
    if (size == 0) {
        ::GlobalUnlock(hGlobal);
        return FALSE;
    }
    data = (BYTE*)malloc(size);
    if (data == NULL) {
        ::GlobalUnlock(hGlobal);
        return FALSE;
    }
    if (::GetMetaFileBitsEx(hmf, size, data) != size) {
        free(data);
        ::GlobalUnlock(hGlobal);
        return FALSE;
    }
    if (MakeFileName(path, MAX_PATH * 2, dir, stamp, _T("CF_METAFILEPICT"), _T("bin"))) {
        WriteBytes(path, data, size);
    }

    /*
     * The METAFILEPICT mapping mode and extents are needed to render WMF.
     * A conservative 1024x768 target is used for mapping modes whose
     * logical extents cannot be converted reliably without a full mapper.
     */
    {
        HDC screenDC = ::GetDC(NULL);
        HDC memDC = ::CreateCompatibleDC(screenDC);
        BITMAPINFO bmi;
        HBITMAP dib;
        HBITMAP oldBitmap;
        PIXEL32* pixels = NULL;
        LONG width = 1024, height = 768;
        HMETAFILE copyMF = ::SetMetaFileBitsEx(size, data);

        if (copyMF != NULL && screenDC != NULL && memDC != NULL) {
            memset(&bmi, 0, sizeof(bmi));
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = width;
            bmi.bmiHeader.biHeight = -height;
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;
            dib = ::CreateDIBSection(screenDC, &bmi, DIB_RGB_COLORS,
                                     (void**)&pixels, NULL, 0);
            if (dib != NULL) {
                oldBitmap = (HBITMAP)::SelectObject(memDC, dib);
                memset(pixels, 255, (size_t)width * height * 4);
                ::PlayMetaFile(memDC, copyMF);
                {
                    DWORD i, count = (DWORD)width * height;
                    for (i = 0; i < count; ++i) pixels[i].a = 255;
                }
                if (MakeFileName(path, MAX_PATH * 2, dir, stamp,
                                 _T("CF_METAFILEPICT"), _T("bmp"))) {
                    PIXEL32* bottomUp = (PIXEL32*)malloc((size_t)width * height * 4);
                    if (bottomUp != NULL) {
                        LONG y;
                        for (y = 0; y < height; ++y)
                            memcpy(bottomUp + y * width,
                                   pixels + (height - 1 - y) * width,
                                   width * sizeof(PIXEL32));
                        SaveV4BMP(path, width, height, bottomUp);
                        free(bottomUp);
                    }
                }
                ::SelectObject(memDC, oldBitmap);
                ::DeleteObject(dib);
            }
        }
        if (copyMF != NULL) ::DeleteMetaFile(copyMF);
        if (memDC != NULL) ::DeleteDC(memDC);
        if (screenDC != NULL) ::ReleaseDC(NULL, screenDC);
    }

    free(data);
    ::GlobalUnlock(hGlobal);
    return TRUE;
}

static BOOL IsImageClipboardFormat(UINT format, UINT pngFormat)
{
    if (format == CF_DIB || format == CF_DIBV5 ||
        format == CF_BITMAP || format == CF_ENHMETAFILE ||
        format == CF_METAFILEPICT) return TRUE;
    if (pngFormat != 0 && format == pngFormat) return TRUE;
    return FALSE;
}

static void GetFormatName(UINT format, UINT pngFormat,
                          TCHAR* name, DWORD cch)
{
    if (format == CF_DIB) _tcsncpy(name, _T("CF_DIB"), cch);
    else if (format == CF_DIBV5) _tcsncpy(name, _T("CF_DIBV5"), cch);
    else if (format == CF_BITMAP) _tcsncpy(name, _T("CF_BITMAP"), cch);
    else if (format == CF_ENHMETAFILE) _tcsncpy(name, _T("CF_ENHMETAFILE"), cch);
    else if (format == CF_METAFILEPICT) _tcsncpy(name, _T("CF_METAFILEPICT"), cch);
    else if (pngFormat != 0 && format == pngFormat) _tcsncpy(name, _T("PNG"), cch);
    else {
        if (::GetClipboardFormatName(format, name, (int)cch) == 0) {
            _stprintf(name, _T("FORMAT_%u"), format);
        }
    }
    name[cch - 1] = 0;
}

static BOOL DumpClipboard(const TCHAR* dir, UINT pngFormat)
{
    UINT format = 0;
    UINT found = 0;
    TCHAR stamp[32];
    BOOL result = TRUE;

    MakeTimeStamp(stamp, 32);
    _tprintf(_T("\nClipboard detected: %s\n"), stamp);

    while ((format = ::EnumClipboardFormats(format)) != 0) {
        TCHAR formatName[128];
        HANDLE handle;

        if (!IsImageClipboardFormat(format, pngFormat)) continue;
        ++found;
        GetFormatName(format, pngFormat, formatName, 128);
        _tprintf(_T("  %s\n"), formatName);

        handle = ::GetClipboardData(format);
        if (handle == NULL) {
            _tprintf(_T("    GetClipboardData failed (%lu)\n"), ::GetLastError());
            result = FALSE;
            continue;
        }

        if (format == CF_DIB || format == CF_DIBV5) {
            if (!SaveDIBFormat((HGLOBAL)handle, dir, stamp, formatName)) result = FALSE;
        } else if (format == CF_BITMAP) {
            if (!SaveBitmapFormat((HBITMAP)handle, dir, stamp)) result = FALSE;
        } else if (format == CF_ENHMETAFILE) {
            if (!SaveEnhMetaFile((HENHMETAFILE)handle, dir, stamp)) result = FALSE;
        } else if (format == CF_METAFILEPICT) {
            if (!SaveMetaFilePict((HGLOBAL)handle, dir, stamp)) result = FALSE;
        } else if (format == pngFormat) {
            SIZE_T size = ::GlobalSize((HGLOBAL)handle);
            const BYTE* p = (const BYTE*)::GlobalLock((HGLOBAL)handle);
            TCHAR path[MAX_PATH * 2];
            if (p != NULL && size <= 0xFFFFFFFFUL &&
                MakeFileName(path, MAX_PATH * 2, dir, stamp, _T("PNG"), _T("bin"))) {
                if (!WriteBytes(path, p, (DWORD)size)) result = FALSE;
                ::GlobalUnlock((HGLOBAL)handle);
            } else {
                if (p != NULL) ::GlobalUnlock((HGLOBAL)handle);
                result = FALSE;
            }
        }
    }

    if (found == 0) {
        _tprintf(_T("No supported image format found.\n"));
        return FALSE;
    }
    _tprintf(_T("Dump finished.\n"));
    return result;
}

int _tmain(int argc, TCHAR* argv[])
{
    TCHAR outputDir[MAX_PATH * 2];
    UINT pngFormat;
    DWORD waitMilliseconds = 1000;

    if (!MakeOutputDirectory(outputDir, MAX_PATH * 2)) {
        _tprintf(_T("Cannot create temporary output directory.\n"));
        return 1;
    }

    if (argc >= 2) {
        long seconds = _ttol(argv[1]);
        if (seconds >= 1 && seconds <= 60) waitMilliseconds = (DWORD)seconds * 1000;
    }

    pngFormat = ::RegisterClipboardFormat(_T("PNG"));
    _tprintf(_T("ClipDmp - Clipboard image dumper\n"));
    _tprintf(_T("Output: %s\n"), outputDir);
    _tprintf(_T("Polling interval: %lu ms\n"), waitMilliseconds);
    _tprintf(_T("Waiting for clipboard image data...\n"));

    BOOL waiting = TRUE;

    for (;;) {
        // key
        if (::_kbhit()) {
            int ch = ::_getch();

            if (ch == 'q' || ch == 'Q') {
                break;
            }
            if (ch == '\r') {
                waiting = TRUE;
                _tprintf(_T("Waiting for next clipboard image...\n"));
            }
        }

        if (!waiting) {
            ::Sleep(100);
            continue;
        }

        if (::OpenClipboard(NULL)) {
            BOOL hasImage = FALSE;
            UINT f = 0;

            while ((f = ::EnumClipboardFormats(f)) != 0) {
                if (IsImageClipboardFormat(f, pngFormat)) {
                    hasImage = TRUE;
                    break;
                }
            }

            if (hasImage) {
                BOOL ok = DumpClipboard(outputDir, pngFormat);
                ::CloseClipboard();

                if (!ok) {
                    _tprintf(_T("Dump failed.\n"));
                }
                else {
                    _tprintf(_T("Dump completed.\n"));
                }

                waiting = FALSE;
                continue;
            }

            ::CloseClipboard();
        }

        ::Sleep(waitMilliseconds);
    }

/*
    for (;;) {
        if (::OpenClipboard(NULL)) {
            BOOL hasImage = FALSE;
            UINT f = 0;
            while ((f = ::EnumClipboardFormats(f)) != 0) {
                if (IsImageClipboardFormat(f, pngFormat)) {
                    hasImage = TRUE;
                    break;
                }
            }

            if (hasImage) {
                BOOL ok = DumpClipboard(outputDir, pngFormat);
                ::CloseClipboard();
                return ok ? 0 : 2;
            }
            ::CloseClipboard();
        }
        ::Sleep(waitMilliseconds);
    }
*/
    return	0 ;
}

