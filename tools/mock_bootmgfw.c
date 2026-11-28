#include "../src/uefi_compat.h"

/*
 * Lightweight UEFI chainload target placed at \EFI\Microsoft\Boot\bootmgfw.efi
 * inside the virtual QEMU FAT32 ESP image so Stage 4 (ChainloadNextLoader)
 * can be verified end-to-end without a Windows installation.
 */
EFI_STATUS EFIAPI efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *system_table) {
    (void)image_handle;
    SIMPLE_TEXT_OUTPUT_INTERFACE *con = system_table->ConOut;
    EFI_BOOT_SERVICES            *bs  = system_table->BootServices;
    EFI_RUNTIME_SERVICES         *rt  = system_table->RuntimeServices;

    if (con) {
        con->SetAttribute(con, EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK);
        con->ClearScreen(con);
        con->EnableCursor(con, FALSE);

        con->SetAttribute(con, EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK);
        con->OutputString(con, L"\r\n  [  ");
        con->SetAttribute(con, EFI_LIGHTGREEN | EFI_BACKGROUND_BLACK);
        con->OutputString(con, L"OK");
        con->SetAttribute(con, EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK);
        con->OutputString(con, L"  ] Chainloaded ");
        con->SetAttribute(con, EFI_LIGHTCYAN | EFI_BACKGROUND_BLACK);
        con->OutputString(con, L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi");
        con->SetAttribute(con, EFI_WHITE | EFI_BACKGROUND_BLACK);
        con->OutputString(con, L" inside QEMU + OVMF.\r\n\r\n");

        con->SetAttribute(con, EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK);
        con->OutputString(con, L"         All stages (dmesg -> systemd -> 24-bit GOP->Blt -> Chainloader) succeeded.\r\n");
        con->OutputString(con, L"         Press [R] to reboot VM and replay, or any other key to power off.\r\n");
    }

    if (system_table->ConIn && system_table->ConIn->ReadKeyStroke) {
        EFI_INPUT_KEY key;
        while (!EFI_ERROR(system_table->ConIn->ReadKeyStroke(system_table->ConIn, &key))) {
        }
        for (;;) {
            if (!EFI_ERROR(system_table->ConIn->ReadKeyStroke(system_table->ConIn, &key))) {
                if ((key.UnicodeChar == L'r' || key.UnicodeChar == L'R') && rt && rt->ResetSystem) {
                    rt->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
                }
                break;
            }
            if (bs && bs->Stall) {
                bs->Stall(15000U);
            }
        }
    } else if (bs && bs->Stall) {
        bs->Stall(3000000U);
    }

    if (rt && rt->ResetSystem) {
        rt->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, NULL);
    }
    return EFI_SUCCESS;
}
