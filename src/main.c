#include <efi.h>
#include <efilib.h>

#define IMG_W 1024
#define IMG_H 576
#define FRAME_BYTES (IMG_W * IMG_H * 2)   /* RGB565 */

static EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
static void *frame_buf = NULL;
static UINTN frame_cap = 0;

static void wait_ms(UINTN ms) { uefi_call_wrapper(gBS->Stall, 1, ms * 1000); }

/* ---------------- 读文件（从本 EFI 所在的 FAT 卷） ---------------- */
static BOOLEAN load_frame(EFI_HANDLE img, CONST CHAR16 *path)
{
    EFI_STATUS s;
    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
    EFI_FILE_PROTOCOL *root = NULL, *f = NULL;
    UINTN sz;

    if (frame_buf == NULL) {
        s = uefi_call_wrapper(gBS->AllocatePool, 3, EfiLoaderData,
                              (frame_cap = FRAME_BYTES + 4096), &frame_buf);
        if (EFI_ERROR(s)) return FALSE;
    }

    s = uefi_call_wrapper(gBS->HandleProtocol, 3, img, &LoadedImageProtocol, (void **)&li);
    if (EFI_ERROR(s)) return FALSE;
    s = uefi_call_wrapper(gBS->HandleProtocol, 3, li->DeviceHandle, &FileSystemProtocol, (void **)&fs);
    if (EFI_ERROR(s)) return FALSE;
    s = uefi_call_wrapper(fs->OpenVolume, 2, fs, &root);
    if (EFI_ERROR(s)) return FALSE;
    s = uefi_call_wrapper(root->Open, 5, root, &f, (CHAR16 *)path, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(s)) { uefi_call_wrapper(root->Close, 2, root, NULL); return FALSE; }

    sz = FRAME_BYTES;
    s = uefi_call_wrapper(f->Read, 3, f, &sz, frame_buf);
    uefi_call_wrapper(f->Close, 2, f, NULL);
    uefi_call_wrapper(root->Close, 2, root, NULL);
    return (!EFI_ERROR(s) && sz == FRAME_BYTES) ? TRUE : FALSE;
}

/* ---------------- 贴图（等比缩放 + 居中，四周补黑） ---------------- */
static void present(void)
{
    if (!gop || !gop->Mode || !gop->Mode->Info || !frame_buf) return;

    UINT32 W = gop->Mode->Info->HorizontalResolution;
    UINT32 H = gop->Mode->Info->VerticalResolution;
    UINT32 stride = gop->Mode->Info->PixelsPerScanLine; /* 单位：像素 */
    UINT8 *fb = (UINT8 *)(UINTN)gop->Mode->FrameBufferBase;
    UINT32 fmt = (UINT32)gop->Mode->Info->PixelFormat;  /* 0=RGBX 1=BGRX */
    const UINT16 *src = (const UINT16 *)frame_buf;

    /* 整屏刷黑 */
    for (UINT32 y = 0; y < H; y++) {
        UINT32 *row = (UINT32 *)(fb + (UINTN)y * stride * 4);
        for (UINT32 x = 0; x < W; x++) row[x] = 0;
    }

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

/* ---------------- 键盘 ---------------- */
static void pump(UINTN ms, BOOLEAN *key)
{
    EFI_INPUT_KEY k;
    UINTN waited = 0;
    while (waited < ms) {
        if (ST->ConIn && uefi_call_wrapper(gBS->CheckEvent, 1, ST->ConIn->WaitForKey) == EFI_SUCCESS) {
            uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &k);
            *key = TRUE;
            return;
        }
        wait_ms(5); waited += 5;
    }
}

typedef struct { CHAR16 path[32]; UINTN hint_ms; BOOLEAN anyKey; } STEP;

static const STEP steps[] = {
    { L"\\frames\\000.bin", 3500, FALSE },   /* 开机 */
    { L"\\frames\\001.bin", 0,    TRUE  },   /* 选择操作系统 */
    { L"\\frames\\002.bin", 1200, FALSE },   /* 安装 1/5 */
    { L"\\frames\\003.bin", 1200, FALSE },
    { L"\\frames\\004.bin", 1200, FALSE },
    { L"\\frames\\005.bin", 1200, FALSE },
    { L"\\frames\\006.bin", 1500, FALSE },   /* 安装 5/5 */
    { L"\\frames\\007.bin", 0,    TRUE  },   /* 输入法 */
    { L"\\frames\\008.bin", 0,    TRUE  },   /* 连接 */
    { L"\\frames\\009.bin", 0,    TRUE  },   /* 名字 */
    { L"\\frames\\010.bin", 0,    TRUE  },   /* 密码 */
    { L"\\frames\\011.bin", 1200, FALSE },   /* 空白 */
    { L"\\frames\\012.bin", 3500, FALSE },   /* 请保持接通电源 */
    { L"\\frames\\013.bin", 2500, FALSE },   /* 好戏来了 */
    { L"\\frames\\014.bin", 15000, TRUE },   /* 桌面 */
};

EFI_STATUS efi_main(EFI_HANDLE img, EFI_SYSTEM_TABLE *st)
{
    EFI_STATUS s;
    UINTN n = sizeof(steps) / sizeof(steps[0]);

    InitializeLib(img, st);

    s = uefi_call_wrapper(gBS->LocateProtocol, 3, &GraphicsOutputProtocol, NULL, (void **)&gop);
    if (EFI_ERROR(s)) { gop = NULL; }

    for (UINTN i = 0; i < n; i++) {
        BOOLEAN key = FALSE;
        if (load_frame(img, steps[i].path)) present();
        if (steps[i].anyKey) {
            pump(steps[i].hint_ms ? steps[i].hint_ms : 100000, &key);
        } else {
            pump(steps[i].hint_ms, &key);
        }
    }

    /* 走完 → 关机，便于宿主脚本判定完成 */
    wait_ms(300);
    uefi_call_wrapper(gRT->ResetSystem, 4, EfiResetShutdown, EFI_SUCCESS, 0, NULL);
    for (;;) wait_ms(1000);
    return EFI_SUCCESS;
}
