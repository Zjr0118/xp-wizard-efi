#include <efi.h>
#include <efilib.h>

#define IMG_W 1024
#define IMG_H 576
#define FRAME_BYTES (IMG_W * IMG_H * 2)   /* RGB565 little-endian */

/* 15 帧全部编进 PE，运行时不再依赖任何文件系统 */
extern const unsigned char _binary_frames_bin_start[];
extern const unsigned char _binary_frames_bin_end[];

static EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
static EFI_SERIAL_IO_PROTOCOL *g_ser = NULL;

/* ---------------- 调试输出：直接写 COM1 ---------------- */
static void slog(const char *s)
{
    UINTN n = 0, sz;
    if (g_ser == NULL) return;
    while (s[n] != 0) n++;
    if (n == 0) return;
    sz = n;
    uefi_call_wrapper(g_ser->Write, 3, g_ser, &sz, (void *)s);
}

static void slog_num(const char *tag, UINT64 v)
{
    char buf[72];
    int i = 0, j;
    char tmp[24];
    int t = 0;
    for (j = 0; tag[j] != 0 && i < 40; j++) buf[i++] = tag[j];
    if (v == 0) buf[i++] = '0';
    else {
        while (v > 0 && t < 24) { tmp[t++] = (char)('0' + (v % 10)); v /= 10; }
        while (t > 0) buf[i++] = tmp[--t];
    }
    buf[i++] = '\r'; buf[i++] = '\n'; buf[i] = 0;
    slog(buf);
}

static void slog_hex(const char *tag, UINT64 v)
{
    char buf[72];
    int i = 0, j;
    char tmp[16];
    int t = 0;
    const char *hx = "0123456789ABCDEF";
    for (j = 0; tag[j] != 0 && i < 40; j++) buf[i++] = tag[j];
    if (v == 0) tmp[t++] = '0';
    while (v > 0 && t < 16) { tmp[t++] = hx[v & 0xF]; v >>= 4; }
    while (t > 0) buf[i++] = tmp[--t];
    buf[i++] = '\r'; buf[i++] = '\n'; buf[i] = 0;
    slog(buf);
}

static void wait_ms(UINTN ms) { uefi_call_wrapper(gBS->Stall, 1, ms * 1000); }

/* 优先挑最接近 16:9 的 GOP 模式，避免上下黑边 */
static void pick_mode(void)
{
    UINT32 best = 0;
    UINT64 bestscore = (UINT64)-1;
    UINT32 maxmode;
    if (!gop || !gop->Mode) return;
    maxmode = gop->Mode->MaxMode;
    slog_num("maxmode=", maxmode);
    for (UINT32 i = 0; i < maxmode; i++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = NULL;
        UINTN sz = 0;
        UINT32 w, h;
        UINT64 diff, area, score;
        if (EFI_ERROR(uefi_call_wrapper(gop->QueryMode, 4, gop, i, &sz, &info)) || !info) continue;
        w = info->HorizontalResolution;
        h = info->VerticalResolution;
        if (w < 1024 || h < 576) continue;
        diff = (16 * (UINT64)h > 9 * (UINT64)w) ? (16 * (UINT64)h - 9 * (UINT64)w)
                                                : (9 * (UINT64)w - 16 * (UINT64)h);
        area = (UINT64)w * (UINT64)h;
        score = diff * 10000000ULL + (20000000000ULL / area);
        if (score < bestscore) { bestscore = score; best = i; }
    }
    slog_num("chosen mode=", best);
    if (best != gop->Mode->Mode) {
        slog_hex("SetMode err=", (UINT64)uefi_call_wrapper(gop->SetMode, 2, gop, best));
    }
}

static void clear_fb(UINT32 rgb)
{
    UINT32 W, H, stride;
    UINT8 *fb;
    if (!gop || !gop->Mode || !gop->Mode->Info) return;
    W = gop->Mode->Info->HorizontalResolution;
    H = gop->Mode->Info->VerticalResolution;
    stride = gop->Mode->Info->PixelsPerScanLine;
    fb = (UINT8 *)(UINTN)gop->Mode->FrameBufferBase;
    for (UINT32 y = 0; y < H; y++) {
        UINT32 *row = (UINT32 *)(fb + (UINTN)y * stride * 4);
        for (UINT32 x = 0; x < W; x++) row[x] = rgb;
    }
}

/* 等比缩放居中贴图，四周补黑 */
static void present(UINTN idx)
{
    UINT32 W, H, stride, fmt, dw, dh, ox, oy;
    UINT8 *fb;
    const UINT16 *src;

    if (!gop || !gop->Mode || !gop->Mode->Info) return;
    if ((idx + 1) * FRAME_BYTES > (UINTN)(_binary_frames_bin_end - _binary_frames_bin_start)) return;

    W = gop->Mode->Info->HorizontalResolution;
    H = gop->Mode->Info->VerticalResolution;
    stride = gop->Mode->Info->PixelsPerScanLine;
    fb = (UINT8 *)(UINTN)gop->Mode->FrameBufferBase;
    fmt = (UINT32)gop->Mode->Info->PixelFormat;
    src = (const UINT16 *)(_binary_frames_bin_start + idx * FRAME_BYTES);

    clear_fb(0);

    if ((UINT64)W * IMG_H >= (UINT64)H * IMG_W) { dw = (H * IMG_W) / IMG_H; dh = H; }
    else                                        { dw = W; dh = (W * IMG_H) / IMG_W; }
    ox = (W - dw) / 2; oy = (H - dh) / 2;

    for (UINT32 y = 0; y < dh; y++) {
        UINT32 sy = (y * IMG_H) / dh;
        UINT32 *row = (UINT32 *)(fb + (UINTN)(oy + y) * stride * 4) + ox;
        const UINT16 *srow = src + (UINTN)sy * IMG_W;
        for (UINT32 x = 0; x < dw; x++) {
            UINT32 sx = (x * IMG_W) / dw;
            UINT16 v = srow[sx];
            UINT32 r = (UINT32)((v >> 11) & 0x1F) * 255 / 31;
            UINT32 g = (UINT32)((v >> 5) & 0x3F) * 255 / 63;
            UINT32 b = (UINT32)(v & 0x1F) * 255 / 31;
            row[x] = (fmt == 1) ? (b | (g << 8) | (r << 16)) : (r | (g << 8) | (b << 16));
        }
    }
    slog_num("present idx=", idx);
}

/* ---------------- 键盘 ---------------- */
static void pump(UINTN ms, BOOLEAN *key)
{
    EFI_INPUT_KEY k;
    UINTN waited = 0;
    if (ms == 0) ms = 1;
    while (waited < ms) {
        if (ST->ConIn && uefi_call_wrapper(gBS->CheckEvent, 1, ST->ConIn->WaitForKey) == EFI_SUCCESS) {
            uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &k);
            *key = TRUE;
            return;
        }
        wait_ms(5); waited += 5;
    }
}

/* ---------------- 演完后把控制权交给苹果自己的 boot.efi ---------------- */
static EFI_STATUS chainload(EFI_HANDLE parent, EFI_LOADED_IMAGE_PROTOCOL *li, CHAR16 *path)
{
    EFI_STATUS s;
    EFI_DEVICE_PATH *dp;
    EFI_HANDLE img = NULL;

    slog("chainload try\r\n");
    dp = FileDevicePath(li->DeviceHandle, path);
    if (dp == NULL) { slog("FileDevicePath null\r\n"); return EFI_NOT_FOUND; }
    s = uefi_call_wrapper(gBS->LoadImage, 6, FALSE, parent, dp, NULL, 0, &img);
    slog_hex("LoadImage err=", (UINT64)s);
    if (EFI_ERROR(s) || img == NULL) return s;
    s = uefi_call_wrapper(gBS->StartImage, 3, img, NULL, NULL);
    slog_hex("StartImage err=", (UINT64)s);
    return s;
}

typedef struct { UINTN idx; UINTN hint_ms; BOOLEAN anyKey; } STEP;

static const STEP steps[] = {
    { 0,  3500,  FALSE },
    { 1,  0,     TRUE  },
    { 2,  1200,  FALSE },
    { 3,  1200,  FALSE },
    { 4,  1200,  FALSE },
    { 5,  1200,  FALSE },
    { 6,  1500,  FALSE },
    { 7,  0,     TRUE  },
    { 8,  0,     TRUE  },
    { 9,  0,     TRUE  },
    { 10, 0,     TRUE  },
    { 11, 1200,  FALSE },
    { 12, 3500,  FALSE },
    { 13, 2500,  FALSE },
    { 14, 15000, TRUE  },
};

EFI_STATUS efi_main(EFI_HANDLE img, EFI_SYSTEM_TABLE *st)
{
    EFI_STATUS s;
    UINTN n = sizeof(steps) / sizeof(steps[0]);
    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;

    InitializeLib(img, st);

    s = uefi_call_wrapper(gBS->LocateProtocol, 3, &SerialIoProtocol, NULL, (void **)&g_ser);
    if (EFI_ERROR(s)) g_ser = NULL;
    slog("\r\nXPWIZARD: start\r\n");
    slog_num("frames bytes=", (UINT64)(_binary_frames_bin_end - _binary_frames_bin_start));

    uefi_call_wrapper(gBS->HandleProtocol, 3, img, &LoadedImageProtocol, (void **)&li);
    if (li != NULL) slog_hex("device handle=", (UINT64)(UINTN)li->DeviceHandle);

    s = uefi_call_wrapper(gBS->LocateProtocol, 3, &GraphicsOutputProtocol, NULL, (void **)&gop);
    slog_hex("gop status=", (UINT64)s);
    if (EFI_ERROR(s) || gop == NULL) { slog("gop not found\r\n"); for (;;) wait_ms(1000); }
    slog_num("res x=", gop->Mode->Info->HorizontalResolution);
    slog_num("res y=", gop->Mode->Info->VerticalResolution);
    slog_num("pixelformat=", gop->Mode->Info->PixelFormat);
    pick_mode();

    for (UINTN i = 0; i < n; i++) {
        BOOLEAN key = FALSE;
        present(steps[i].idx);
        if (steps[i].anyKey) pump(steps[i].hint_ms ? steps[i].hint_ms : 100000, &key);
        else pump(steps[i].hint_ms, &key);
    }

    slog("wizard done, handoff\r\n");
    if (li != NULL) {
        if (!EFI_ERROR(chainload(img, li, u"\\System\\Library\\CoreServices\\boot-orig.efi"))) {
            slog("handoff ok boot-orig\r\n");
            for (;;) wait_ms(1000);
        }
        if (!EFI_ERROR(chainload(img, li, u"\\EFI\\BOOT\\APPLE.EFI"))) {
            slog("handoff ok APPLE.EFI\r\n");
            for (;;) wait_ms(1000);
        }
    }

    slog("handoff FAILED, shutdown\r\n");
    wait_ms(500);
    uefi_call_wrapper(gRT->ResetSystem, 4, EfiResetShutdown, EFI_SUCCESS, 0, NULL);
    for (;;) wait_ms(1000);
    return EFI_SUCCESS;
}
