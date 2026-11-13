#include <efi.h>
#include <efilib.h>

#define IMG_W 1024
#define IMG_H 576
#define FRAME_BYTES (IMG_W * IMG_H * 2)   /* RGB565 little-endian */

static EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
static EFI_SERIAL_IO_PROTOCOL *g_ser = NULL;
static void *frame_buf = NULL;

/* ---------------- 调试输出：直接写 COM1 ---------------- */
static void slog(const char *s)
{
    if (g_ser == NULL) return;
    UINTN n = 0;
    while (s[n] != 0) n++;
    UINTN sz = n;
    if (n == 0) return;
    uefi_call_wrapper(g_ser->Write, 3, g_ser, &sz, (void *)s);
}

static void slog_num(const char *tag, UINT64 v)
{
    char buf[64];
    int i = 0, j;
    for (j = 0; tag[j] != 0 && i < 40; j++) buf[i++] = tag[j];
    if (v == 0) { buf[i++] = '0'; }
    else {
        char tmp[24]; int t = 0;
        while (v > 0 && t < 24) { tmp[t++] = (char)('0' + (v % 10)); v /= 10; }
        while (t > 0) buf[i++] = tmp[--t];
    }
    buf[i++] = '\r'; buf[i++] = '\n'; buf[i] = 0;
    slog(buf);
}

static void slog_hex(const char *tag, UINT64 v)
{
    char buf[64];
    int i = 0, j;
    static const char *hx = "0123456789ABCDEF";
    for (j = 0; tag[j] != 0 && i < 40; j++) buf[i++] = tag[j];
    char tmp[16]; int t = 0;
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
        if (EFI_ERROR(uefi_call_wrapper(gop->QueryMode, 4, gop, i, &sz, &info)) || !info) continue;
        UINT32 w = info->HorizontalResolution, h = info->VerticalResolution;
        if (w < 1024 || h < 576) continue;
        UINT64 diff = (16 * (UINT64)h > 9 * (UINT64)w) ? (16 * (UINT64)h - 9 * (UINT64)w)
                                                       : (9 * (UINT64)w - 16 * (UINT64)h);
        UINT64 area = (UINT64)w * (UINT64)h;
        UINT64 score = diff * 10000000ULL + (20000000000ULL / area);
        if (score < bestscore) { bestscore = score; best = i; }
    }
    slog_num("chosen mode=", best);
    if (best != gop->Mode->Mode) {
        EFI_STATUS s2 = uefi_call_wrapper(gop->SetMode, 2, gop, best);
        slog_hex("SetMode err=", s2);
    }
}

/* ---------------- 屏幕 ---------------- */
static void clear_fb(UINT32 rgb)
{
    if (!gop || !gop->Mode || !gop->Mode->Info) return;
    UINT32 W = gop->Mode->Info->HorizontalResolution;
    UINT32 H = gop->Mode->Info->VerticalResolution;
    UINT32 stride = gop->Mode->Info->PixelsPerScanLine;
    UINT8 *fb = (UINT8 *)(UINTN)gop->Mode->FrameBufferBase;
    for (UINT32 y = 0; y < H; y++) {
        UINT32 *row = (UINT32 *)(fb + (UINTN)y * stride * 4);
        for (UINT32 x = 0; x < W; x++) row[x] = rgb;
    }
}

/* 8 条彩条：只要能看见，就证明 GOP / 帧缓冲写入通路是好的 */
static void test_pattern(void)
{
    if (!gop || !gop->Mode || !gop->Mode->Info) return;
    UINT32 W = gop->Mode->Info->HorizontalResolution;
    UINT32 H = gop->Mode->Info->VerticalResolution;
    UINT32 stride = gop->Mode->Info->PixelsPerScanLine;
    UINT8 *fb = (UINT8 *)(UINTN)gop->Mode->FrameBufferBase;
    static const UINT32 cols[8] = {
        0x00FF0000, 0x0000FF00, 0x000000FF, 0x00FFFFFF,
        0x00FFFF00, 0x0000FFFF, 0x00FF00FF, 0x00666666
    };
    for (UINT32 y = 0; y < H; y++) {
        UINT32 *row = (UINT32 *)(fb + (UINTN)y * stride * 4);
        for (UINT32 x = 0; x < W; x++) {
            UINT32 band = (W > 0) ? ((UINT64)x * 8 / W) : 0;
            if (band > 7) band = 7;
            row[x] = cols[band];
        }
    }
}

/* 等比缩放居中贴图，四周补黑 */
static void present(void)
{
    if (!gop || !gop->Mode || !gop->Mode->Info || !frame_buf) return;

    UINT32 W = gop->Mode->Info->HorizontalResolution;
    UINT32 H = gop->Mode->Info->VerticalResolution;
    UINT32 stride = gop->Mode->Info->PixelsPerScanLine;
    UINT8 *fb = (UINT8 *)(UINTN)gop->Mode->FrameBufferBase;
    UINT32 fmt = (UINT32)gop->Mode->Info->PixelFormat;
    const UINT16 *src = (const UINT16 *)frame_buf;

    clear_fb(0);

    UINT32 dw, dh;
    if ((UINT64)W * IMG_H >= (UINT64)H * IMG_W) { dw = (H * IMG_W) / IMG_H; dh = H; }
    else                                        { dw = W; dh = (W * IMG_H) / IMG_W; }
    UINT32 ox = (W - dw) / 2, oy = (H - dh) / 2;

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
}

/* ---------------- 读帧：枚举所有 SimpleFileSystem，逐个试路径 ---------------- */
static EFI_FILE_PROTOCOL *g_root_cache[8];
static UINTN g_root_n = 0;

static void collect_roots(EFI_HANDLE img)
{
    EFI_STATUS s;
    EFI_HANDLE *hs = NULL;
    UINTN cnt = 0;

    if (g_root_n > 0) return;

    /* 先放自身所在卷 */
    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    s = uefi_call_wrapper(gBS->HandleProtocol, 3, img, &LoadedImageProtocol, (void **)&li);
    if (!EFI_ERROR(s) && li && li->DeviceHandle) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
        s = uefi_call_wrapper(gBS->HandleProtocol, 3, li->DeviceHandle, &FileSystemProtocol, (void **)&fs);
        if (!EFI_ERROR(s)) {
            EFI_FILE_PROTOCOL *r = NULL;
            s = uefi_call_wrapper(fs->OpenVolume, 2, fs, &r);
            if (!EFI_ERROR(s)) { g_root_cache[g_root_n++] = r; slog("own volume OK\r\n"); }
            else slog_hex("own volume OpenVolume err=", s);
        } else slog_hex("own volume no FS err=", s);
    } else slog_hex("own volume no LoadedImage err=", s);

    s = uefi_call_wrapper(gBS->LocateHandleBuffer, 5, ByProtocol, &FileSystemProtocol,
                          NULL, &cnt, &hs);
    if (EFI_ERROR(s)) { slog_hex("LocateHandleBuffer err=", s); return; }
    slog_num("fs handles=", cnt);
    for (UINTN i = 0; i < cnt && g_root_n < 8; i++) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
        EFI_FILE_PROTOCOL *r = NULL;
        s = uefi_call_wrapper(gBS->HandleProtocol, 3, hs[i], &FileSystemProtocol, (void **)&fs);
        if (EFI_ERROR(s)) continue;
        s = uefi_call_wrapper(fs->OpenVolume, 2, fs, &r);
        if (EFI_ERROR(s)) continue;
        g_root_cache[g_root_n++] = r;
    }
    slog_num("roots ready=", g_root_n);
}

static void make_path(CHAR16 *out, UINTN idx, UINTN variant)
{
    UINTN i = 0;
    char num[4];
    const CHAR16 *dir;
    if (variant == 0)      dir = u"\\frames\\";
    else if (variant == 1) dir = u"\\FRAMES\\";
    else if (variant == 2) dir = u"frames\\";
    else                   dir = u"\\EFI\\BOOT\\frames\\";
    while (dir[i] != 0) { out[i] = dir[i]; i++; }
    num[0] = (char)('0' + (idx / 100) % 10);
    num[1] = (char)('0' + (idx / 10) % 10);
    num[2] = (char)('0' + idx % 10);
    out[i++] = (CHAR16)num[0];
    out[i++] = (CHAR16)num[1];
    out[i++] = (CHAR16)num[2];
    out[i++] = u'.'; out[i++] = u'b'; out[i++] = u'i'; out[i++] = u'n';
    out[i] = 0;
}

static UINTN try_read(EFI_FILE_PROTOCOL *root, CHAR16 *path, UINTN *got)
{
    EFI_STATUS s;
    EFI_FILE_PROTOCOL *f = NULL;
    UINTN sz = FRAME_BYTES + 4096;
    *got = 0;
    s = uefi_call_wrapper(root->Open, 5, root, &f, path, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(s)) return 0;
    s = uefi_call_wrapper(f->Read, 3, f, &sz, frame_buf);
    uefi_call_wrapper(f->Close, 1, f);
    if (EFI_ERROR(s)) return 0;
    *got = sz;
    return (sz == FRAME_BYTES) ? 1 : 2;
}

static BOOLEAN load_frame(EFI_HANDLE img, UINTN idx)
{
    EFI_STATUS s;
    CHAR16 path[64];

    if (frame_buf == NULL) {
        s = uefi_call_wrapper(gBS->AllocatePool, 3, EfiLoaderData, (UINTN)(FRAME_BYTES + 4096), &frame_buf);
        if (EFI_ERROR(s) || frame_buf == NULL) { slog_hex("AllocatePool err=", s); return FALSE; }
    }
    collect_roots(img);

    for (UINTN v = 0; v < 4; v++) {
        make_path(path, idx, v);
        for (UINTN r = 0; r < g_root_n; r++) {
            UINTN got = 0;
            UINTN rc = try_read(g_root_cache[r], path, &got);
            if (rc == 1) return TRUE;
            if (rc == 2) { slog_num("short read got=", got); }
        }
    }
    return FALSE;
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

    InitializeLib(img, st);

    s = uefi_call_wrapper(gBS->LocateProtocol, 3, &SerialIoProtocol, NULL, (void **)&g_ser);
    if (EFI_ERROR(s)) g_ser = NULL;
    slog("\r\nXPWIZARD: start\r\n");

    s = uefi_call_wrapper(gBS->LocateProtocol, 3, &GraphicsOutputProtocol, NULL, (void **)&gop);
    slog_hex("gop status=", s);
    if (EFI_ERROR(s) || gop == NULL) {
        slog("gop NOT FOUND, halt\r\n");
        for (;;) wait_ms(1000);
    }
    slog_num("mode=", gop->Mode->Mode);
    pick_mode();
    slog_num("final mode=", gop->Mode->Mode);
    slog_num("res x=", gop->Mode->Info->HorizontalResolution);
    slog_num("res y=", gop->Mode->Info->VerticalResolution);
    slog_num("stride=", gop->Mode->Info->PixelsPerScanLine);
    slog_num("pixelformat=", gop->Mode->Info->PixelFormat);
    slog_hex("fb=", gop->Mode->FrameBufferBase);
    slog_hex("fb size=", gop->Mode->FrameBufferSize);

    /* 1) 先画彩条，证明帧缓冲通路 */
    test_pattern();
    slog("test pattern drawn\r\n");
    wait_ms(2000);

    /* 2) 再逐帧播放 */
    for (UINTN i = 0; i < n; i++) {
        BOOLEAN key = FALSE;
        BOOLEAN ok = load_frame(img, steps[i].idx);
        slog_num("frame idx=", steps[i].idx);
        slog_num("  load=", ok ? 1 : 0);
        if (ok) present();
        else clear_fb(0x00333333);   /* 读帧失败 → 深灰，区别于纯黑 */
        if (steps[i].anyKey) pump(steps[i].hint_ms ? steps[i].hint_ms : 100000, &key);
        else pump(steps[i].hint_ms, &key);
    }

    slog("wizard done, shutdown\r\n");
    wait_ms(300);
    uefi_call_wrapper(gRT->ResetSystem, 4, EfiResetShutdown, EFI_SUCCESS, 0, NULL);
    for (;;) wait_ms(1000);
    return EFI_SUCCESS;
}
