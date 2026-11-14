/* 第二阶段占位程序：只在 chainload 自测里使用，
   用来证明“向导演完之后真的把控制权交给了另一个 EFI 程序”。 */
#include <efi.h>
#include <efilib.h>

static void wait_ms(UINTN ms) { uefi_call_wrapper(gBS->Stall, 1, ms * 1000); }

static void fill(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, UINT32 rgb)
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

EFI_STATUS efi_main(EFI_HANDLE img, EFI_SYSTEM_TABLE *st)
{
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    InitializeLib(img, st);
    if (!EFI_ERROR(uefi_call_wrapper(gBS->LocateProtocol, 3, &GraphicsOutputProtocol, NULL, (void **)&gop)))
        fill(gop, 0x0000FF00);   /* 纯绿：只要看到整屏绿，就说明 handoff 成功 */
    Print(u"STAGE2 reached\r\n");
    for (;;) wait_ms(1000);
    return EFI_SUCCESS;
}
