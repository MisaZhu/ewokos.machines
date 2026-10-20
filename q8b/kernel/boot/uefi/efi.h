/*
 * Minimal UEFI definitions for the Q8B AArch64 bootloader.
 *
 * AArch64 UEFI uses the standard AAPCS64 calling convention, so unlike the
 * x86_64 port no ABI shim is needed: all protocol functions can be called
 * directly through their prototypes.
 */
#ifndef Q8B_EFI_H
#define Q8B_EFI_H

typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef signed char        int8_t;
typedef short              int16_t;
typedef int                int32_t;
typedef long long          int64_t;
typedef uint16_t           CHAR16;
typedef uint8_t            BOOLEAN;
typedef unsigned long      UINTN;
typedef void*              EFI_HANDLE;
typedef uint64_t           EFI_STATUS;
typedef uint64_t           EFI_PHYSICAL_ADDRESS;
typedef uint64_t           EFI_VIRTUAL_ADDRESS;
typedef unsigned long      uintptr_t;

#define EFI_SUCCESS          0ULL
#define EFI_ERROR_BIT        0x8000000000000000ULL
#define EFI_ERROR(s)         ((s) & EFI_ERROR_BIT)
#define EFI_INVALID_PARAMETER (2ULL | EFI_ERROR_BIT)
#define EFI_BUFFER_TOO_SMALL  (5ULL | EFI_ERROR_BIT)
#define EFI_NOT_FOUND         (14ULL | EFI_ERROR_BIT)

#define EFI_PAGE_SIZE 4096ULL

/* Memory types */
enum {
    EfiReservedMemoryType = 0,
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
    EfiMaxMemoryType
};

#define AllocateAnyPages    0
#define AllocateMaxAddress  1
#define AllocateAddress     2

typedef struct {
    uint32_t Type;
    uint32_t Pad;
    uint64_t PhysicalStart;
    uint64_t VirtualStart;
    uint64_t NumberOfPages;
    uint64_t Attribute;
} EFI_MEMORY_DESCRIPTOR;

typedef struct {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t  Data4[8];
} EFI_GUID;

#define EFI_GUID_(a,b,c,d0,d1,d2,d3,d4,d5,d6,d7) \
    { (a), (b), (c), { (d0),(d1),(d2),(d3),(d4),(d5),(d6),(d7) } }

#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
    EFI_GUID_(0x964e5b22,0x6459,0x11d2,0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b)
#define EFI_BLOCK_IO_PROTOCOL_GUID \
    EFI_GUID_(0x964e5b21,0x6459,0x11d2,0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b)
#define EFI_FILE_INFO_ID \
    EFI_GUID_(0x09576e92,0x6d3f,0x11d2,0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b)
#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
    EFI_GUID_(0x9042a9de,0x23dc,0x4a38,0x96,0xfb,0x7a,0xde,0xd0,0x80,0x51,0x6a)
#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    EFI_GUID_(0x5b1b31a1,0x9562,0x11d2,0x8e,0x3f,0x00,0xa0,0xc9,0x69,0x72,0x3b)
#define EFI_LOAD_FILE_PROTOCOL_GUID \
    EFI_GUID_(0x56ec3091,0x954c,0x11d2,0x8e,0x3f,0x00,0xa0,0xc9,0x69,0x72,0x3b)
#define EFI_LOAD_FILE2_PROTOCOL_GUID \
    EFI_GUID_(0x4006c0c1,0xfcb3,0x403e,0x99,0x6d,0x4a,0x6c,0x87,0x24,0xe0,0x6d)
#define EFI_SERIAL_IO_PROTOCOL_GUID \
    EFI_GUID_(0xbb25cf6f,0xf1d4,0x11d2,0x9a,0x0c,0x00,0x90,0x27,0x3f,0xc1,0xfd)
#define LINUX_EFI_INITRD_MEDIA_GUID \
    EFI_GUID_(0x5568e427,0x68fc,0x4f3d,0xac,0x74,0xca,0x55,0x52,0x31,0xcc,0x68)
#define EFI_DTB_TABLE_GUID \
    EFI_GUID_(0xb1b621d5,0xf19c,0x41a5,0x83,0x0b,0xd9,0x15,0x2c,0x69,0xaa,0xe0)
typedef char CHAR8;
typedef void* EFI_EVENT;

#define EFI_MTFTP4_PROTOCOL_GUID \
    EFI_GUID_(0x78247c57,0x63db,0x4708,0x99,0xc2,0xa8,0xb4,0xa9,0xa6,0x1f,0x6b)

typedef struct { uint8_t Addr[4]; } EFI_IPv4_ADDRESS;

typedef struct {
    uint8_t UseDefaultSetting;
    EFI_IPv4_ADDRESS StationIp;
    EFI_IPv4_ADDRESS SubnetMask;
    uint16_t LocalPort;
    EFI_IPv4_ADDRESS GatewayIp;
    EFI_IPv4_ADDRESS ServerIp;
    uint16_t InitialServerPort;
    uint16_t TryCount;
    uint16_t TimeoutValue;
} EFI_MTFTP4_CONFIG_DATA;

typedef struct {
    EFI_STATUS Status;
    EFI_EVENT Event;
    void* OverrideData;
    CHAR8* Filename;
    CHAR8* ModeStr;
    uint32_t OptionCount;
    void* OptionList;
    uint64_t BufferSize;
    void* Buffer;
    void* Context;
    void* CheckPacket;
    void* TimeoutCallback;
    void* PacketNeeded;
} EFI_MTFTP4_TOKEN;

typedef struct {
    void* GetModeData;
    EFI_STATUS (*Configure)(void* This, EFI_MTFTP4_CONFIG_DATA* ConfigData);
    void* GetInfo;
    void* ParseOptions;
    EFI_STATUS (*ReadFile)(void* This, EFI_MTFTP4_TOKEN* Token);
} EFI_MTFTP4_PROTOCOL;

typedef struct {
    uint8_t* OptionStr;
    uint8_t* ValueStr;
} EFI_MTFTP4_OPTION;

#define EFI_NOT_READY 0x8000000000000006ULL
#define EFI_TIMEOUT   (18ULL | EFI_ERROR_BIT)
#define EVT_TIMER 0x80000000u
#define TimerRelative 1u

typedef EFI_STATUS (*EFI_CREATE_EVENT_FN)(uint32_t Type, UINTN NotifyTpl,
                                          void* NotifyFunction,
                                          void* NotifyContext, EFI_EVENT* Event);
typedef EFI_STATUS (*EFI_SET_TIMER_FN)(EFI_EVENT Event, uint32_t Type,
                                       uint64_t TriggerTime);
typedef EFI_STATUS (*EFI_WAIT_FOR_EVENT_FN)(UINTN NumberOfEvents,
                                            EFI_EVENT* Event, UINTN* Index);
typedef EFI_STATUS (*EFI_CLOSE_EVENT_FN)(EFI_EVENT Event);

#define EFI_PXE_BASE_CODE_PROTOCOL_GUID \
    EFI_GUID_(0x03c4e603,0xac28,0x11d3,0x9a,0x2d,0x00,0x90,0x27,0x3f,0xc1,0x4d)

typedef struct {
    uint8_t Addr[16];          /* IPv4 in the first 4 bytes, network order */
} EFI_IP_ADDRESS;

typedef uint16_t EFI_PXE_BASE_CODE_UDP_PORT;
#define EFI_PXE_BASE_CODE_UDP_OPFLAGS_ANY_SRC_IP     0x0001
#define EFI_PXE_BASE_CODE_UDP_OPFLAGS_ANY_SRC_PORT   0x0002
#define EFI_PXE_BASE_CODE_UDP_OPFLAGS_ANY_DEST_IP    0x0004
#define EFI_PXE_BASE_CODE_UDP_OPFLAGS_ANY_DEST_PORT  0x0008

typedef struct {
    uint64_t Revision;
    void* Start;
    void* Stop;
    void* Dhcp;
    void* Discover;
    EFI_STATUS (*Mtftp)(void* This, uint32_t Operation, void* BufferPtr,
                        BOOLEAN Overwrite, uint64_t* BufferSize,
                        UINTN BlockSize, EFI_IP_ADDRESS* ServerIp,
                        CHAR16* Filename, void* Info);
    EFI_STATUS (*UdpWrite)(void* This, uint16_t OpFlags,
                           EFI_IP_ADDRESS* DestIp,
                           EFI_PXE_BASE_CODE_UDP_PORT* DestPort,
                           EFI_IP_ADDRESS* GatewayIp,
                           EFI_IP_ADDRESS* SrcIp,
                           EFI_PXE_BASE_CODE_UDP_PORT* SrcPort,
                           UINTN* HeaderSize, void* HeaderPtr,
                           UINTN* BufferSize, void* BufferPtr);
    EFI_STATUS (*UdpRead)(void* This, uint16_t OpFlags,
                          EFI_IP_ADDRESS* DestIp,
                          EFI_PXE_BASE_CODE_UDP_PORT* DestPort,
                          EFI_IP_ADDRESS* SrcIp,
                          EFI_PXE_BASE_CODE_UDP_PORT* SrcPort,
                          UINTN* HeaderSize, void* HeaderPtr,
                          UINTN* BufferSize, void* BufferPtr);
    void* SetIpFilter;
    void* Arp;
    EFI_STATUS (*SetParameters)(void* This, BOOLEAN* NewAutoArp,
                                BOOLEAN* NewSendGUID, uint8_t* NewTTL,
                                uint8_t* NewToS, BOOLEAN* NewMakeCallback);
    EFI_STATUS (*SetStationIp)(void* This, EFI_IP_ADDRESS* NewStationIp,
                               EFI_IP_ADDRESS* NewSubnetMask);
} EFI_PXE_BASE_CODE_PROTOCOL;

#define EFI_PXE_BASE_CODE_TFTP_GET_FILE_SIZE 1
#define EFI_PXE_BASE_CODE_TFTP_READ_FILE     2

#define EFI_MEMORY_ATTRIBUTE_PROTOCOL_GUID \
    EFI_GUID_(0xf4560cf6,0x40ec,0x4b4a,0xa1,0x92,0xbf,0x1d,0x57,0xd0,0xb1,0x89)

#define EFI_MEMORY_RO 0x0000000000000002ULL
#define EFI_MEMORY_XP 0x0000000000004000ULL

typedef struct {
    EFI_STATUS (*GetMemoryAttributes)(uint64_t BaseAddress, uint64_t Length,
                                      uint64_t* Attributes);
    EFI_STATUS (*SetMemoryAttributes)(uint64_t BaseAddress, uint64_t Length,
                                      uint64_t Attributes);
    EFI_STATUS (*ClearMemoryAttributes)(uint64_t BaseAddress, uint64_t Length,
                                        uint64_t Attributes);
} EFI_MEMORY_ATTRIBUTE_PROTOCOL;

typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL_
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
typedef struct EFI_BOOT_SERVICES_ EFI_BOOT_SERVICES;
typedef struct EFI_CONFIGURATION_TABLE_ EFI_CONFIGURATION_TABLE;

typedef struct {
    uint8_t  Type;
    uint8_t  SubType;
    uint16_t Length;
} EFI_DEVICE_PATH_PROTOCOL;

typedef struct {
    EFI_DEVICE_PATH_PROTOCOL Header;   /* Type 4, SubType 4 */
    CHAR16 PathName[1];
} FILEPATH_DEVICE_PATH;

typedef struct {
    EFI_DEVICE_PATH_PROTOCOL Header;   /* Type 4, SubType 6 */
    EFI_GUID Guid;
} VENDOR_DEVICE_PATH;

typedef struct {
    uint64_t Signature;
    uint32_t Revision;
    uint32_t HeaderSize;
    uint32_t CRC32;
    uint32_t Reserved;
    CHAR16*  FirmwareVendor;
    uint32_t FirmwareRevision;
    uint32_t Pad0;
    EFI_HANDLE ConsoleInHandle;
    void* ConIn;
    EFI_HANDLE ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* ConOut;
    EFI_HANDLE StandardErrorHandle;
    void* StdErr;
    void* RuntimeServices;
    EFI_BOOT_SERVICES* BootServices;
    UINTN NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE* ConfigurationTable;
} EFI_SYSTEM_TABLE;

struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL_ {
    EFI_STATUS (*Reset)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL_* This,
                        BOOLEAN ExtendedVerification);
    EFI_STATUS (*OutputString)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL_* This,
                               CHAR16* String);
    void* TestString;
    void* QueryMode;
    void* SetMode;
    void* SetAttribute;
    void* ClearScreen;
    void* SetCursorPosition;
    void* EnableCursor;
    void* Mode;
};

typedef struct EFI_SERIAL_IO_PROTOCOL_ EFI_SERIAL_IO_PROTOCOL;

typedef struct {
    uint32_t ControlMask;
    uint32_t Timeout;
    uint64_t BaudRate;
    uint32_t ReceiveFifoDepth;
    uint32_t DataBits;
    uint32_t Parity;
    uint32_t StopBits;
} EFI_SERIAL_IO_MODE;

struct EFI_SERIAL_IO_PROTOCOL_ {
    uint32_t Revision;
    EFI_STATUS (*SetAttributes)(EFI_SERIAL_IO_PROTOCOL* This,
                                uint64_t BaudRate, uint32_t ReceiveFifoDepth,
                                uint32_t Timeout, uint32_t Parity,
                                uint32_t DataBits, uint32_t StopBits);
    EFI_STATUS (*SetControlBits)(EFI_SERIAL_IO_PROTOCOL* This, uint32_t Bits);
    EFI_STATUS (*ClearControlBits)(EFI_SERIAL_IO_PROTOCOL* This, uint32_t Bits);
    EFI_STATUS (*GetControlBits)(EFI_SERIAL_IO_PROTOCOL* This, uint32_t* Bits);
    EFI_STATUS (*Write)(EFI_SERIAL_IO_PROTOCOL* This, UINTN* BufferSize,
                        void* Buffer);
    EFI_STATUS (*Read)(EFI_SERIAL_IO_PROTOCOL* This, UINTN* BufferSize,
                       void* Buffer);
    EFI_STATUS (*Reset)(EFI_SERIAL_IO_PROTOCOL* This);
    EFI_SERIAL_IO_MODE* Mode;
};

struct EFI_CONFIGURATION_TABLE_ {
    EFI_GUID VendorGuid;
    void* VendorTable;
};

struct EFI_BOOT_SERVICES_ {
    uint64_t Hdr[3];
    uint64_t RaiseTPL;
    uint64_t RestoreTPL;
    EFI_STATUS (*AllocatePages)(uint32_t Type, uint32_t MemoryType,
                                UINTN Pages, EFI_PHYSICAL_ADDRESS* Memory);
    EFI_STATUS (*FreePages)(EFI_PHYSICAL_ADDRESS Memory, UINTN Pages);
    EFI_STATUS (*GetMemoryMap)(UINTN* MemoryMapSize,
                               EFI_MEMORY_DESCRIPTOR* MemoryMap,
                               UINTN* MapKey, UINTN* DescriptorSize,
                               uint32_t* DescriptorVersion);
    EFI_STATUS (*AllocatePool)(uint32_t PoolType, UINTN Size, void** Buffer);
    EFI_STATUS (*FreePool)(void* Buffer);
    void* CreateEvent;
    void* SetTimer;
    void* WaitForEvent;
    void* SignalEvent;
    void* CloseEvent;
    void* CheckEvent;
    void* InstallProtocolInterface;
    void* ReinstallProtocolInterface;
    void* UninstallProtocolInterface;
    EFI_STATUS (*HandleProtocol)(EFI_HANDLE Handle, EFI_GUID* Protocol,
                                 void** Interface);
    void* Reserved;
    void* RegisterProtocolNotify;
    EFI_STATUS (*LocateHandle)(uint32_t SearchType, EFI_GUID* Protocol,
                               void* SearchKey, UINTN* BufferSize,
                               EFI_HANDLE* Buffer);
    EFI_STATUS (*LocateDevicePath)(EFI_GUID* Protocol,
                                   EFI_DEVICE_PATH_PROTOCOL** DevicePath,
                                   EFI_HANDLE* Device);
    void* InstallConfigurationTable;
    void* LoadImage;
    void* StartImage;
    void* Exit;
    void* UnloadImage;
    EFI_STATUS (*ExitBootServices)(EFI_HANDLE ImageHandle, UINTN MapKey);
    void* GetNextMonotonicCount;
    EFI_STATUS (*Stall)(UINTN Microseconds);
    EFI_STATUS (*SetWatchdogTimer)(UINTN Timeout, uint64_t WatchdogCode,
                                   UINTN DataSize, CHAR16* WatchdogData);
    EFI_STATUS (*ConnectController)(EFI_HANDLE ControllerHandle,
                                    EFI_HANDLE DriverImageHandle,
                                    EFI_DEVICE_PATH_PROTOCOL* RemainingDevicePath,
                                    BOOLEAN Recursive);
    void* DisconnectController;
    void* OpenProtocol;
    void* CloseProtocol;
    void* OpenProtocolInformation;
    void* ProtocolsPerHandle;
    EFI_STATUS (*LocateHandleBuffer)(uint32_t SearchType, EFI_GUID* Protocol,
                                     void* SearchKey, UINTN* NoHandles,
                                     EFI_HANDLE** Buffer);
    EFI_STATUS (*LocateProtocol)(EFI_GUID* Protocol, void* Registration,
                                 void** Interface);
    void* InstallMultipleProtocolInterfaces;
    void* UninstallMultipleProtocolInterfaces;
    void* CalculateCrc32;
    void* CopyMem;
    void* SetMem;
    void* CreateEventEx;
};

typedef struct {
    uint64_t Revision;
    EFI_STATUS (*OpenVolume)(void* This, void** Root);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

typedef struct EFI_FILE_PROTOCOL_ {
    uint64_t Revision;
    EFI_STATUS (*Open)(struct EFI_FILE_PROTOCOL_* This,
                       struct EFI_FILE_PROTOCOL_** NewHandle,
                       CHAR16* FileName, uint64_t OpenMode, uint64_t Attributes);
    EFI_STATUS (*Close)(struct EFI_FILE_PROTOCOL_* This);
    void* Delete;
    EFI_STATUS (*Read)(struct EFI_FILE_PROTOCOL_* This,
                       UINTN* BufferSize, void* Buffer);
    void* Write;
    void* GetPosition;
    void* SetPosition;
    EFI_STATUS (*GetInfo)(struct EFI_FILE_PROTOCOL_* This, EFI_GUID* InfoType,
                          UINTN* InfoSize, void* Info);
    void* SetInfo;
    void* Flush;
} EFI_FILE_PROTOCOL;

#define EFI_FILE_MODE_READ 1ULL

typedef struct {
    uint64_t Size;
    uint64_t FileSize;
    uint64_t PhysicalSize;
    uint64_t Attribute;
} EFI_FILE_INFO_HEAD;

typedef struct {
    uint32_t Version;
    uint32_t HorizontalResolution;
    uint32_t VerticalResolution;
    uint32_t PixelFormat;
    uint32_t PixelInformation[4];
    uint32_t PixelsPerScanLine;
} EFI_GOP_MODE_INFO;

typedef struct {
    uint32_t MaxMode;
    uint32_t Mode;
    EFI_GOP_MODE_INFO* Info;
    UINTN SizeOfInfo;
    EFI_PHYSICAL_ADDRESS FrameBufferBase;
    UINTN FrameBufferSize;
} EFI_GOP_MODE;

typedef struct {
    void* QueryMode;
    void* SetMode;
    void* Blt;
    EFI_GOP_MODE* Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

typedef struct {
    uint32_t Revision;
    EFI_HANDLE ParentHandle;
    EFI_SYSTEM_TABLE* SystemTable;
    EFI_HANDLE DeviceHandle;
    EFI_DEVICE_PATH_PROTOCOL* FilePath;
    void* Reserved;
    uint32_t LoadOptionsSize;
    void* LoadOptions;
    void* ImageBase;
    uint64_t ImageSize;
    uint32_t ImageCodeType;
    uint32_t ImageDataType;
    void* Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

typedef struct {
    EFI_STATUS (*LoadFile)(void* This, EFI_DEVICE_PATH_PROTOCOL* FilePath,
                           BOOLEAN BootPolicy, UINTN* BufferSize,
                           void* Buffer);
} EFI_LOAD_FILE_PROTOCOL;

typedef struct {
    EFI_STATUS (*LoadFile)(void* This, EFI_DEVICE_PATH_PROTOCOL* FilePath,
                           BOOLEAN BootPolicy, UINTN* BufferSize,
                           void* Buffer);
} EFI_LOAD_FILE2_PROTOCOL;

uint64_t efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE* SystemTable);

#endif
