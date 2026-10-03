/* efi.h — 最小 UEFI 引导接口定义 (UEFI 2.x, x86_64)
 *
 * 注意: EFI 固件使用 MS x64 ABI (rcx,rdx,r8,r9 + shadow space)，
 * 我们用 SysV gcc 编译，所有 EFI 调用必须经 efi_callN shim (efi_stub.S)。
 * 所有协议结构均为手工按 UEFI 规范排布的字段指针表。
 */
#ifndef EFI_H
#define EFI_H

typedef unsigned char   uint8_t;
typedef unsigned short  uint16_t;
typedef unsigned int    uint32_t;
typedef unsigned long long uint64_t;
typedef signed char     int8_t;
typedef short           int16_t;
typedef int             int32_t;
typedef long long       int64_t;
typedef uint16_t        CHAR16;
typedef void           *EFI_HANDLE;
typedef uint64_t        EFI_STATUS;

#define EFI_SUCCESS          ((EFI_STATUS)0)
#define EFI_ERROR_BIT        0x8000000000000000ULL
#define EFI_ERROR(s)         ((s) & EFI_ERROR_BIT)
#define EFI_NOT_FOUND        ((EFI_STATUS)14 | EFI_ERROR_BIT)
#define EFI_BUFFER_TOO_SMALL ((EFI_STATUS)5  | EFI_ERROR_BIT)

/* ---- 内存类型 (EFI_MEMORY_TYPE) ---- */
enum {
    EfiReservedMemoryType = 0,
    EfiLoaderCode, EfiLoaderData, EfiBootServicesCode, EfiBootServicesData,
    EfiRuntimeServicesCode, EfiRuntimeServicesData, EfiConventionalMemory,
    EfiUnusableMemory, EfiACPIReclaimMemory, EfiACPIMemoryNVS, EfiMemoryMappedIO,
    EfiMemoryMappedIOPortSpace, EfiPalCode, EfiMaxMemoryType
};
/* AllocatePages Type */
#define AllocateAnyPages     0
#define AllocateMaxAddress   1
#define AllocateAddress      3

typedef struct {
    uint32_t Type;
    uint32_t Pad;
    uint64_t PhysicalStart;
    uint64_t VirtualStart;
    uint64_t NumberOfPages;
    uint64_t Attribute;
} EFI_MEMORY_DESCRIPTOR;

/* ---- GUID ---- */
typedef struct {
    uint32_t Data1;
    uint16_t Data2, Data3;
    uint8_t  Data4[8];
} EFI_GUID;

#define EFI_GUID_(a,b,c,d0,d1,d2,d3,d4,d5,d6,d7) \
    { (a), (b), (c), { (d0),(d1),(d2),(d3),(d4),(d5),(d6),(d7) } }

#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
    EFI_GUID_(0x964e5b22,0x6459,0x11d2,0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b)
#define EFI_FILE_INFO_ID \
    EFI_GUID_(0x09576e92,0x6d3f,0x11d2,0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b)
#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
    EFI_GUID_(0x9042a9de,0x23dc,0x4a38,0x96,0xfb,0x7a,0xde,0xd0,0x80,0x51,0x6a)
#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    EFI_GUID_(0x5b1b31a1,0x9562,0x11d2,0x8e,0x3f,0x00,0xa0,0xc9,0x69,0x72,0x3b)
#define EFI_BLOCK_IO_PROTOCOL_GUID \
    EFI_GUID_(0x964e5b21,0x6459,0x11d2,0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b)

/* ---- Block IO (CD/磁盘 ISO9660 直读用) ---- */
typedef struct {
    uint32_t MediaId;
    uint8_t  RemovableMedia;
    uint8_t  MediaPresent;
    uint8_t  LogicalPartition;
    uint8_t  ReadOnly;
    uint8_t  WriteCaching;
    uint32_t BlockSize;
    uint32_t IoAlign;
    uint64_t LastBlock;
} EFI_BLOCK_IO_MEDIA;

typedef struct {
    uint64_t Revision;
    EFI_BLOCK_IO_MEDIA *Media;
    uint64_t Reset;              /* (this, ExtendedVerification) */
    uint64_t ReadBlocks;         /* (this, MediaId, LBA, BufferSize, *Buffer) */
    uint64_t WriteBlocks;
    uint64_t FlushBlocks;
} EFI_BLOCK_IO_PROTOCOL;

/* ---- Simple Text Output ---- */
typedef struct {
    uint64_t Reset;
    uint64_t OutputString;       /* (this, CHAR16*) */
    uint64_t TestString;
    uint64_t QueryMode;
    uint64_t SetMode;
    uint64_t SetAttribute;
    uint64_t ClearScreen;        /* (this) */
    uint64_t SetCursorPosition;
    uint64_t EnableCursor;
    void    *Mode;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

/* ---- Simple File System / File ---- */
typedef struct EFI_FILE_PROTOCOL_ EFI_FILE_PROTOCOL;
typedef struct {
    uint64_t Revision;
    uint64_t OpenVolume;         /* (this, EFI_FILE_PROTOCOL **Root) */
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

struct EFI_FILE_PROTOCOL_ {
    uint64_t Revision;
    uint64_t Open;               /* (this, EFI_FILE **New, CHAR16 *Name, uint64_t Mode, uint64_t Attr) */
    uint64_t Close;              /* (this) */
    uint64_t Delete;
    uint64_t Read;               /* (this, uint64_t *BufferSize, void *Buffer) */
    uint64_t Write;
    uint64_t GetPosition;
    uint64_t SetPosition;        /* (this, uint64_t Position; 0xFFFFFFFFFFFFFFFF = EOF) */
    uint64_t GetInfo;            /* (this, EFI_GUID *InfoType, uint64_t *Size, void *Info) */
    uint64_t SetInfo;
    uint64_t Flush;
};
#define EFI_FILE_MODE_READ  0x1

typedef struct {
    uint64_t Size;
    uint64_t FileSize;
    uint64_t PhysicalSize;
    uint64_t Attribute;          /* 之后还有 EFI_TIME*3 + FileName[]，用不到 */
} EFI_FILE_INFO_PACKED_HEAD;

/* ---- GOP ---- */
typedef struct {
    uint32_t Version;
    uint32_t HorizontalResolution;
    uint32_t VerticalResolution;
    uint32_t PixelFormat;        /* 0=RGBR(蓝最低) 1=BGRX 2=BitMask 3=仅Blt */
    uint32_t PixelInformation[4];/* EFI_PIXEL_BITMASK (16B!) — 写成 4B 会把
                                  * PixelsPerScanLine 错位 12 字节 (QEMU GOP
                                  * 的 BitMask 全 0 -> fb_pitch=0) */
    uint32_t PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    uint32_t MaxMode;
    uint32_t Mode;
    void    *Info;
    uint64_t SizeOfInfo;
    uint64_t FrameBufferBase;
    uint64_t FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct {
    uint64_t QueryMode;
    uint64_t SetMode;
    uint64_t Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

typedef struct {
    EFI_GUID VendorGuid;
    void *VendorTable;
} EFI_CONFIGURATION_TABLE;

/* ---- System Table ---- */
typedef struct {
    uint64_t Signature;          /* 'IBI SYST' = 0x5453595320494249 */
    uint32_t Revision;           /* 高16位.低16位，如 2.70 = 0x00020070 */
    uint32_t HeaderSize;
    uint32_t CRC32;
    uint32_t Reserved;
    CHAR16  *FirmwareVendor;
    uint32_t FirmwareRevision;
    uint32_t __pad0;
    EFI_HANDLE ConsoleInHandle;
    void *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE StandardErrorHandle;
    void *StdErr;
    void *RuntimeServices;
    void *BootServices;
    uint64_t NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE *ConfigurationTable;
} EFI_SYSTEM_TABLE;

/* ---- Boot Services (字段顺序必须与规范一致，只列到用到的+其后全部) ---- */
typedef struct {
    uint64_t Hdr[3];                       /* 24 字节表头 */
    uint64_t RaiseTPL;
    uint64_t RestoreTPL;
    /* 3 */ uint64_t AllocatePages;         /* (Type, MemoryType, Pages, EFI_PHYSICAL_ADDRESS *Memory) */
    uint64_t FreePages;
    /* 5 */ uint64_t GetMemoryMap;          /* (Size*, Map, MapKey*, DescSize*, DescVersion*) */
    uint64_t AllocatePool;
    uint64_t FreePool;
    uint64_t CreateEvent;
    uint64_t SetTimer;
    /* 10 */ uint64_t WaitForEvent;
    uint64_t SignalEvent;                   /* ← 官方表里有此字段, 漏掉会整体错位8字节! */
    uint64_t CloseEvent;
    uint64_t CheckEvent;
    uint64_t InstallProtocolInterface;
    uint64_t ReinstallProtocolInterface;
    /* 15 */ uint64_t UninstallProtocolInterface;
    uint64_t HandleProtocol;                /* (Handle, Protocol, void **Interface) */
    void *Reserved;
    uint64_t RegisterProtocolNotify;
    uint64_t LocateHandle;
    /* 20 */ uint64_t LocateDevicePath;
    uint64_t InstallConfigurationTable;
    uint64_t LoadImage;
    uint64_t StartImage;
    uint64_t Exit;
    /* 25 */ uint64_t UnloadImage;
    uint64_t ExitBootServices;              /* (ImageHandle, MapKey) */
    uint64_t GetNextMonotonicCount;
    uint64_t Stall;                         /* (Microseconds) */
    uint64_t SetWatchdogTimer;              /* (Timeout, WatchdogCode, DataSize, CHAR16*) */
    /* 30 */ uint64_t ConnectController;
    uint64_t DisconnectController;
    uint64_t OpenProtocol;
    uint64_t CloseProtocol;
    uint64_t OpenProtocolInformation;
    /* 35 */ uint64_t ProtocolsPerHandle;
    uint64_t LocateHandleBuffer;            /* (SearchType, Protocol, SearchKey, NoHandles*, Buffer**) */
    uint64_t LocateProtocol;                /* (Protocol, Registration, void **Interface) */
    uint64_t InstallMultipleProtocolInterfaces;
    uint64_t UninstallMultipleProtocolInterfaces;
    /* 40 */ uint64_t CalculateCrc32;
    uint64_t CopyMem;                       /* (Dest, Src, Length) */
    uint64_t SetMem;                        /* (Buffer, Size, Value) */
    uint64_t CreateEventEx;
} EFI_BOOT_SERVICES;

/* ---- Runtime Services (只用 ResetSystem) ---- */
typedef struct {
    uint64_t Hdr[3];
    uint64_t GetTime;
    uint64_t SetTime;
    uint64_t GetWakeupTime;
    uint64_t SetWakeupTime;
    /* 5 */ uint64_t SetVirtualAddressMap;
    uint64_t ConvertPointer;
    uint64_t GetVariable;
    uint64_t GetNextVariableName;
    uint64_t SetVariable;
    /* 10 */ uint64_t GetNextHighMonotonicCount;
    uint64_t ResetSystem;                   /* (ResetType, Status, DataSize, Data) */
} EFI_RUNTIME_SERVICES;
#define EFI_RESET_SHUTDOWN 2

/* ---- efi_stub.S 提供的 ABI shim: SysV(rdi=func, rsi..=args) → MS ABI ---- */
uint64_t efi_call1(void *f, uint64_t a1);
uint64_t efi_call2(void *f, uint64_t a1, uint64_t a2);
uint64_t efi_call3(void *f, uint64_t a1, uint64_t a2, uint64_t a3);
uint64_t efi_call4(void *f, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4);
uint64_t efi_call5(void *f, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);

/* ---- 供 bootloader 使用的内联封装 ---- */
static inline EFI_STATUS efi_stall(EFI_BOOT_SERVICES *bs, uint64_t us) {
    return (EFI_STATUS)efi_call1((void *)bs->Stall, us);
}
static inline EFI_STATUS efi_output_string(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *c, CHAR16 *s) {
    return (EFI_STATUS)efi_call2((void *)c->OutputString, (uint64_t)c, (uint64_t)s);
}
static inline EFI_STATUS efi_clear_screen(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *c) {
    return (EFI_STATUS)efi_call1((void *)c->ClearScreen, (uint64_t)c);
}
static inline EFI_STATUS efi_locate_protocol(EFI_BOOT_SERVICES *bs, EFI_GUID *g, void **out) {
    return (EFI_STATUS)efi_call3((void *)bs->LocateProtocol, (uint64_t)g, 0, (uint64_t)out);
}
static inline EFI_STATUS efi_handle_protocol(EFI_BOOT_SERVICES *bs, EFI_HANDLE h, EFI_GUID *g, void **out) {
    return (EFI_STATUS)efi_call3((void *)bs->HandleProtocol, (uint64_t)h, (uint64_t)g, (uint64_t)out);
}

/* ---- 小工具 ---- */
void *efi_memcpy(void *d, const void *s, uint64_t n);
void *efi_memset(void *d, uint8_t v, uint64_t n);

#endif /* EFI_H */
