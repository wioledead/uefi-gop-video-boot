#pragma once

#include <stdint.h>
#include <stddef.h>

/*
 * Uses official GNU-EFI headers (<efi.h>, <efilib.h>) when available
 * (e.g. inside the HackBGRT repository with gnu-efi/inc or WSL /usr/include/efi).
 * Falls back to self-contained UEFI 2.x x86_64 definitions when building
 * standalone with x86_64-w64-mingw32-gcc or clang without external headers.
 */
#if defined(USE_GNUEFI) || __has_include(<efi.h>)

#ifndef GNU_EFI_USE_MS_ABI
#define GNU_EFI_USE_MS_ABI 1
#endif

#include <efi.h>
#include <efilib.h>

#else /* Standalone UEFI 2.x definitions for x86_64 */

#if defined(__x86_64__) || defined(_M_X64)
#define EFIAPI __attribute__((ms_abi))
#else
#define EFIAPI
#endif

#define IN
#define OUT
#define OPTIONAL
#define CONST const

typedef uint8_t   BOOLEAN;
typedef int8_t    INT8;
typedef uint8_t   UINT8;
typedef int16_t   INT16;
typedef uint16_t  UINT16;
typedef int32_t   INT32;
typedef uint32_t  UINT32;
typedef int64_t   INT64;
typedef uint64_t  UINT64;
typedef intptr_t  INTN;
typedef uintptr_t UINTN;
typedef char      CHAR8;
typedef uint16_t  CHAR16;
typedef void      VOID;
typedef UINTN     EFI_STATUS;
typedef VOID*     EFI_HANDLE;
typedef VOID*     EFI_EVENT;
typedef UINT64    EFI_PHYSICAL_ADDRESS;
typedef UINT64    EFI_VIRTUAL_ADDRESS;

#ifndef FALSE
#define FALSE ((BOOLEAN)0)
#endif
#ifndef TRUE
#define TRUE  ((BOOLEAN)1)
#endif

#define EFI_ERROR_BIT        ((UINTN)1 << (sizeof(UINTN) * 8 - 1))
#define EFI_ERROR(s)         (((INTN)(s)) < 0)
#define EFI_SUCCESS          ((EFI_STATUS)0)
#define EFI_LOAD_ERROR       (EFI_ERROR_BIT | 1)
#define EFI_INVALID_PARAMETER (EFI_ERROR_BIT | 2)
#define EFI_UNSUPPORTED      (EFI_ERROR_BIT | 3)
#define EFI_NOT_FOUND        (EFI_ERROR_BIT | 14)

typedef struct {
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8  Data4[8];
} EFI_GUID;

typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HEADER;

/* Console text attributes (UEFI Spec 12.4) */
#define EFI_BLACK            0x00
#define EFI_BLUE             0x01
#define EFI_GREEN            0x02
#define EFI_CYAN             0x03
#define EFI_RED              0x04
#define EFI_MAGENTA          0x05
#define EFI_BROWN            0x06
#define EFI_LIGHTGRAY        0x07
#define EFI_DARKGRAY         0x08
#define EFI_LIGHTBLUE        0x09
#define EFI_LIGHTGREEN       0x0A
#define EFI_LIGHTCYAN        0x0B
#define EFI_LIGHTRED         0x0C
#define EFI_LIGHTMAGENTA     0x0D
#define EFI_YELLOW           0x0E
#define EFI_WHITE            0x0F

#define EFI_BACKGROUND_BLACK 0x00

/* Simple Text Input Protocol */
typedef struct {
    UINT16 ScanCode;
    CHAR16 UnicodeChar;
} EFI_INPUT_KEY;

struct _SIMPLE_INPUT_INTERFACE;
typedef EFI_STATUS (EFIAPI *EFI_INPUT_RESET)(
    IN struct _SIMPLE_INPUT_INTERFACE *This,
    IN BOOLEAN ExtendedVerification
);
typedef EFI_STATUS (EFIAPI *EFI_INPUT_READ_KEY)(
    IN struct _SIMPLE_INPUT_INTERFACE *This,
    OUT EFI_INPUT_KEY *Key
);

typedef struct _SIMPLE_INPUT_INTERFACE {
    EFI_INPUT_RESET    Reset;
    EFI_INPUT_READ_KEY ReadKeyStroke;
    EFI_EVENT          WaitForKey;
} SIMPLE_INPUT_INTERFACE;

/* Simple Text Output Protocol */
typedef struct {
    INT32   MaxMode;
    INT32   Mode;
    INT32   Attribute;
    INT32   CursorColumn;
    INT32   CursorRow;
    BOOLEAN CursorVisible;
} SIMPLE_TEXT_OUTPUT_MODE;

struct _SIMPLE_TEXT_OUTPUT_INTERFACE;
typedef EFI_STATUS (EFIAPI *EFI_TEXT_RESET)(
    IN struct _SIMPLE_TEXT_OUTPUT_INTERFACE *This,
    IN BOOLEAN ExtendedVerification
);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_OUTPUT_STRING)(
    IN struct _SIMPLE_TEXT_OUTPUT_INTERFACE *This,
    IN CHAR16 *String
);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_TEST_STRING)(
    IN struct _SIMPLE_TEXT_OUTPUT_INTERFACE *This,
    IN CHAR16 *String
);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_QUERY_MODE)(
    IN struct _SIMPLE_TEXT_OUTPUT_INTERFACE *This,
    IN UINTN ModeNumber,
    OUT UINTN *Columns,
    OUT UINTN *Rows
);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_SET_MODE)(
    IN struct _SIMPLE_TEXT_OUTPUT_INTERFACE *This,
    IN UINTN ModeNumber
);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_SET_ATTRIBUTE)(
    IN struct _SIMPLE_TEXT_OUTPUT_INTERFACE *This,
    IN UINTN Attribute
);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_CLEAR_SCREEN)(
    IN struct _SIMPLE_TEXT_OUTPUT_INTERFACE *This
);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_SET_CURSOR_POSITION)(
    IN struct _SIMPLE_TEXT_OUTPUT_INTERFACE *This,
    IN UINTN Column,
    IN UINTN Row
);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_ENABLE_CURSOR)(
    IN struct _SIMPLE_TEXT_OUTPUT_INTERFACE *This,
    IN BOOLEAN Visible
);

typedef struct _SIMPLE_TEXT_OUTPUT_INTERFACE {
    EFI_TEXT_RESET               Reset;
    EFI_TEXT_OUTPUT_STRING       OutputString;
    EFI_TEXT_TEST_STRING         TestString;
    EFI_TEXT_QUERY_MODE          QueryMode;
    EFI_TEXT_SET_MODE            SetMode;
    EFI_TEXT_SET_ATTRIBUTE       SetAttribute;
    EFI_TEXT_CLEAR_SCREEN        ClearScreen;
    EFI_TEXT_SET_CURSOR_POSITION SetCursorPosition;
    EFI_TEXT_ENABLE_CURSOR       EnableCursor;
    SIMPLE_TEXT_OUTPUT_MODE      *Mode;
} SIMPLE_TEXT_OUTPUT_INTERFACE;

/* Memory & Locate Types */
typedef enum {
    EfiReservedMemoryType,
    EfiLoaderCode,
    EfiLoaderData,
    EfiBootServicesCode,
    EfiBootServicesData,
    EfiRuntimeServicesCode,
    EfiRuntimeServicesData,
    EfiConventionalMemory,
    EfiUnusableMemory,
    EfiACPIReclaimMemory,
    EfiACPIMemoryNVS,
    EfiMemoryMappedIO,
    EfiMemoryMappedIOPortSpace,
    EfiPalCode,
    EfiPersistentMemory,
    EfiMaxMemoryType
} EFI_MEMORY_TYPE;

typedef enum {
    AllHandles,
    ByRegisterNotify,
    ByProtocol
} EFI_LOCATE_SEARCH_TYPE;

/* Device Path Protocol */
#define EFI_DEVICE_PATH_PROTOCOL_GUID \
    { 0x09576e91, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

typedef struct _EFI_DEVICE_PATH_PROTOCOL {
    UINT8 Type;
    UINT8 SubType;
    UINT8 Length[2];
} EFI_DEVICE_PATH;

typedef struct {
    EFI_DEVICE_PATH Header;
    CHAR16          PathName[1];
} FILEPATH_DEVICE_PATH;

#define MEDIA_DEVICE_PATH               0x04
#define MEDIA_FILEPATH_DP               0x04
#define SIZE_OF_FILEPATH_DEVICE_PATH    offsetof(FILEPATH_DEVICE_PATH, PathName)

#define END_DEVICE_PATH_TYPE            0x7f
#define END_ENTIRE_DEVICE_PATH_SUBTYPE  0xff
#define END_INSTANCE_DEVICE_PATH_SUBTYPE 0x01

#define DevicePathType(a)           (((a)->Type) & 0x7f)
#define DevicePathSubType(a)        ((a)->SubType)
#define DevicePathNodeLength(a)     ((UINTN)(((a)->Length[0]) | ((a)->Length[1] << 8)))
#define NextDevicePathNode(a)       ((EFI_DEVICE_PATH *)(((UINT8 *)(a)) + DevicePathNodeLength(a)))
#define IsDevicePathEndType(a)      (DevicePathType(a) == END_DEVICE_PATH_TYPE)
#define IsDevicePathEndSubType(a)   ((a)->SubType == END_ENTIRE_DEVICE_PATH_SUBTYPE)
#define IsDevicePathEnd(a)          (IsDevicePathEndType(a) && IsDevicePathEndSubType(a))

/* Boot Services Table (exact UEFI 2.x layout & offsets) */
typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_POOL)(
    IN EFI_MEMORY_TYPE PoolType,
    IN UINTN Size,
    OUT VOID **Buffer
);
typedef EFI_STATUS (EFIAPI *EFI_FREE_POOL)(
    IN VOID *Buffer
);
typedef EFI_STATUS (EFIAPI *EFI_HANDLE_PROTOCOL)(
    IN EFI_HANDLE Handle,
    IN EFI_GUID *Protocol,
    OUT VOID **Interface
);
typedef EFI_STATUS (EFIAPI *EFI_LOCATE_HANDLE)(
    IN EFI_LOCATE_SEARCH_TYPE SearchType,
    IN EFI_GUID *Protocol OPTIONAL,
    IN VOID *SearchKey OPTIONAL,
    IN OUT UINTN *BufferSize,
    OUT EFI_HANDLE *Buffer
);
typedef EFI_STATUS (EFIAPI *EFI_IMAGE_LOAD)(
    IN BOOLEAN BootPolicy,
    IN EFI_HANDLE ParentImageHandle,
    IN EFI_DEVICE_PATH *FilePath,
    IN VOID *SourceBuffer OPTIONAL,
    IN UINTN SourceSize,
    OUT EFI_HANDLE *ImageHandle
);
typedef EFI_STATUS (EFIAPI *EFI_IMAGE_START)(
    IN EFI_HANDLE ImageHandle,
    OUT UINTN *ExitDataSize,
    OUT CHAR16 **ExitData OPTIONAL
);
typedef EFI_STATUS (EFIAPI *EFI_IMAGE_UNLOAD)(
    IN EFI_HANDLE ImageHandle
);
typedef EFI_STATUS (EFIAPI *EFI_STALL)(
    IN UINTN Microseconds
);
typedef VOID (EFIAPI *EFI_COPY_MEM)(
    IN VOID *Destination,
    IN VOID *Source,
    IN UINTN Length
);
typedef VOID (EFIAPI *EFI_SET_MEM)(
    IN VOID *Buffer,
    IN UINTN Size,
    IN UINT8 Value
);
typedef EFI_STATUS (EFIAPI *EFI_LOCATE_PROTOCOL)(
    IN EFI_GUID *Protocol,
    IN VOID *Registration OPTIONAL,
    OUT VOID **Interface
);

typedef struct {
    EFI_TABLE_HEADER     Hdr;
    /* Task Priority Services */
    VOID                 *RaiseTPL;
    VOID                 *RestoreTPL;
    /* Memory Services */
    VOID                 *AllocatePages;
    VOID                 *FreePages;
    VOID                 *GetMemoryMap;
    EFI_ALLOCATE_POOL    AllocatePool;
    EFI_FREE_POOL        FreePool;
    /* Event & Timer Services */
    VOID                 *CreateEvent;
    VOID                 *SetTimer;
    VOID                 *WaitForEvent;
    VOID                 *SignalEvent;
    VOID                 *CloseEvent;
    VOID                 *CheckEvent;
    /* Protocol Handler Services */
    VOID                 *InstallProtocolInterface;
    VOID                 *ReinstallProtocolInterface;
    VOID                 *UninstallProtocolInterface;
    EFI_HANDLE_PROTOCOL  HandleProtocol;
    VOID                 *PCHandleProtocol;
    VOID                 *RegisterProtocolNotify;
    EFI_LOCATE_HANDLE    LocateHandle;
    VOID                 *LocateDevicePath;
    VOID                 *InstallConfigurationTable;
    /* Image Services */
    EFI_IMAGE_LOAD       LoadImage;
    EFI_IMAGE_START      StartImage;
    VOID                 *Exit;
    EFI_IMAGE_UNLOAD     UnloadImage;
    VOID                 *ExitBootServices;
    /* Miscellaneous Services */
    VOID                 *GetNextMonotonicCount;
    EFI_STALL            Stall;
    VOID                 *SetWatchdogTimer;
    /* DriverSupport Services */
    VOID                 *ConnectController;
    VOID                 *DisconnectController;
    /* Open and Close Protocol Services */
    VOID                 *OpenProtocol;
    VOID                 *CloseProtocol;
    VOID                 *OpenProtocolInformation;
    /* Library Services */
    VOID                 *ProtocolsPerHandle;
    VOID                 *LocateHandleBuffer;
    EFI_LOCATE_PROTOCOL  LocateProtocol;
    VOID                 *InstallMultipleProtocolInterfaces;
    VOID                 *UninstallMultipleProtocolInterfaces;
    /* 32-bit CRC Services */
    VOID                 *CalculateCrc32;
    /* Miscellaneous Services */
    EFI_COPY_MEM         CopyMem;
    EFI_SET_MEM          SetMem;
    VOID                 *CreateEventEx;
} EFI_BOOT_SERVICES;

typedef struct {
    EFI_TABLE_HEADER              Hdr;
    CHAR16                        *FirmwareVendor;
    UINT32                        FirmwareRevision;
    EFI_HANDLE                    ConsoleInHandle;
    SIMPLE_INPUT_INTERFACE        *ConIn;
    EFI_HANDLE                    ConsoleOutHandle;
    SIMPLE_TEXT_OUTPUT_INTERFACE  *ConOut;
    EFI_HANDLE                    StandardErrorHandle;
    SIMPLE_TEXT_OUTPUT_INTERFACE  *StdErr;
    VOID                          *RuntimeServices;
    EFI_BOOT_SERVICES             *BootServices;
    UINTN                         NumberOfTableEntries;
    VOID                          *ConfigurationTable;
} EFI_SYSTEM_TABLE;

/* Loaded Image Protocol */
#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    { 0x5b1b31a1, 0x9562, 0x11d2, { 0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

typedef struct {
    UINT32            Revision;
    EFI_HANDLE        ParentHandle;
    EFI_SYSTEM_TABLE  *SystemTable;
    EFI_HANDLE        DeviceHandle;
    EFI_DEVICE_PATH   *FilePath;
    VOID              *Reserved;
    UINT32            LoadOptionsSize;
    VOID              *LoadOptions;
    VOID              *ImageBase;
    UINT64            ImageSize;
    EFI_MEMORY_TYPE   ImageCodeType;
    EFI_MEMORY_TYPE   ImageDataType;
    EFI_IMAGE_UNLOAD  Unload;
} EFI_LOADED_IMAGE;

/* Simple File System Protocol */
#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
    { 0x964e5b22, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

#define EFI_FILE_MODE_READ 0x0000000000000001ULL

struct _EFI_FILE_HANDLE;
typedef EFI_STATUS (EFIAPI *EFI_FILE_OPEN)(
    IN struct _EFI_FILE_HANDLE *File,
    OUT struct _EFI_FILE_HANDLE **NewHandle,
    IN CHAR16 *FileName,
    IN UINT64 OpenMode,
    IN UINT64 Attributes
);
typedef EFI_STATUS (EFIAPI *EFI_FILE_CLOSE)(
    IN struct _EFI_FILE_HANDLE *File
);
typedef EFI_STATUS (EFIAPI *EFI_FILE_READ)(
    IN struct _EFI_FILE_HANDLE *File,
    IN OUT UINTN *BufferSize,
    OUT VOID *Buffer
);
typedef EFI_STATUS (EFIAPI *EFI_FILE_GET_POSITION)(
    IN struct _EFI_FILE_HANDLE *File,
    OUT UINT64 *Position
);
typedef EFI_STATUS (EFIAPI *EFI_FILE_SET_POSITION)(
    IN struct _EFI_FILE_HANDLE *File,
    IN UINT64 Position
);

typedef struct _EFI_FILE_HANDLE {
    UINT64                Revision;
    EFI_FILE_OPEN         Open;
    EFI_FILE_CLOSE        Close;
    VOID                  *Delete;
    EFI_FILE_READ         Read;
    VOID                  *Write;
    EFI_FILE_GET_POSITION GetPosition;
    EFI_FILE_SET_POSITION SetPosition;
    VOID                  *GetInfo;
    VOID                  *SetInfo;
    VOID                  *Flush;
} EFI_FILE_PROTOCOL, *EFI_FILE_HANDLE;

struct _EFI_FILE_IO_INTERFACE;
typedef EFI_STATUS (EFIAPI *EFI_VOLUME_OPEN)(
    IN struct _EFI_FILE_IO_INTERFACE *This,
    OUT EFI_FILE_HANDLE *Root
);

typedef struct _EFI_FILE_IO_INTERFACE {
    UINT64          Revision;
    EFI_VOLUME_OPEN OpenVolume;
} EFI_FILE_IO_INTERFACE;

/* Graphics Output Protocol (GOP) */
#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
    { 0x9042a9de, 0x23dc, 0x4a38, { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } }

typedef enum {
    PixelRedGreenBlueReserved8BitPerColor,
    PixelBlueGreenRedReserved8BitPerColor,
    PixelBitMask,
    PixelBltOnly,
    PixelFormatMax
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
    UINT32 RedMask;
    UINT32 GreenMask;
    UINT32 BlueMask;
    UINT32 ReservedMask;
} EFI_PIXEL_BITMASK;

typedef struct {
    UINT32                    Version;
    UINT32                    HorizontalResolution;
    UINT32                    VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
    EFI_PIXEL_BITMASK         PixelInformation;
    UINT32                    PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    UINT32                               MaxMode;
    UINT32                               Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN                                SizeOfInfo;
    EFI_PHYSICAL_ADDRESS                 FrameBufferBase;
    UINTN                                FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct {
    UINT8 Blue;
    UINT8 Green;
    UINT8 Red;
    UINT8 Reserved;
} EFI_GRAPHICS_OUTPUT_BLT_PIXEL;

typedef enum {
    EfiBltVideoFill,
    EfiBltVideoToBltBuffer,
    EfiBltBufferToVideo,
    EfiBltVideoToVideo,
    EfiGraphicsOutputBltOperationMax
} EFI_GRAPHICS_OUTPUT_BLT_OPERATION;

struct _EFI_GRAPHICS_OUTPUT_PROTOCOL;
typedef EFI_STATUS (EFIAPI *EFI_GRAPHICS_OUTPUT_PROTOCOL_QUERY_MODE)(
    IN struct _EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
    IN UINT32 ModeNumber,
    OUT UINTN *SizeOfInfo,
    OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info
);
typedef EFI_STATUS (EFIAPI *EFI_GRAPHICS_OUTPUT_PROTOCOL_SET_MODE)(
    IN struct _EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
    IN UINT32 ModeNumber
);
typedef EFI_STATUS (EFIAPI *EFI_GRAPHICS_OUTPUT_PROTOCOL_BLT)(
    IN struct _EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
    IN OUT EFI_GRAPHICS_OUTPUT_BLT_PIXEL *BltBuffer OPTIONAL,
    IN EFI_GRAPHICS_OUTPUT_BLT_OPERATION BltOperation,
    IN UINTN SourceX,
    IN UINTN SourceY,
    IN UINTN DestinationX,
    IN UINTN DestinationY,
    IN UINTN Width,
    IN UINTN Height,
    IN UINTN Delta OPTIONAL
);

typedef struct _EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_GRAPHICS_OUTPUT_PROTOCOL_QUERY_MODE QueryMode;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_SET_MODE   SetMode;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_BLT        Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE       *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

#endif /* USE_GNUEFI || __has_include(<efi.h>) */
