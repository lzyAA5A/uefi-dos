/*
 * UEFI-DOS  --  minimal UEFI definitions
 *
 * Self-contained UEFI bindings (no gnu-efi dependency).
 * Only the structures used by UEFI-DOS are declared here.
 *
 * ABI notes:
 *  - x86_64 UEFI uses the Microsoft x64 calling convention.
 *  - IA32 UEFI uses cdecl (Microsoft compiler emits __cdecl on IA32).
 *  - char16_t in UEFI is 16-bit UTF-16, but gcc on mingw makes wchar_t 16-bit
 *    so we simply use CHAR16 = unsigned short everywhere.
 */
#ifndef UEFI_EFI_H
#define UEFI_EFI_H

typedef unsigned char       UINT8;
typedef signed char         INT8;
typedef unsigned short      UINT16;
typedef signed short        INT16;
typedef unsigned int        UINT32;
typedef signed int          INT32;
#ifdef _WIN64
typedef unsigned long long  UINT64;
typedef signed long long    INT64;
typedef unsigned long long  UINTN;
typedef signed long long    INTN;
#else
typedef unsigned long long  UINT64;
typedef signed long long    INT64;
typedef unsigned int        UINTN;
typedef signed int          INTN;
#endif

typedef UINTN   EFI_STATUS;
typedef void   *EFI_HANDLE;
typedef char    CHAR8;
typedef UINT16  CHAR16;
typedef UINT8   BOOLEAN;

/* UEFI on x86_64 uses the Microsoft x64 calling convention.  MinGW (and MSVC)
 * already default to it, but a Linux/macOS cross build defaults to SysV, so
 * request ms_abi explicitly there.  On IA32 UEFI uses the plain cdecl ABI. */
#if (defined(__x86_64__) || defined(_M_X64)) && defined(__GNUC__) && !defined(_WIN32)
#define EFIAPI __attribute__((ms_abi))
#else
#define EFIAPI
#endif

#define EFI_SUCCESS             0
#if defined(__x86_64__) || defined(_WIN64)
#define EFI_ERRBIT              0x8000000000000000ULL
#else
#define EFI_ERRBIT              0x80000000u
#endif
#define EFI_LOAD_ERROR          ((EFI_STATUS)(EFI_ERRBIT | 0x1))
#define EFI_INVALID_PARAMETER   ((EFI_STATUS)(EFI_ERRBIT | 0x2))
#define EFI_UNSUPPORTED         ((EFI_STATUS)(EFI_ERRBIT | 0x3))
#define EFI_BAD_BUFFER_SIZE     ((EFI_STATUS)(EFI_ERRBIT | 0x4))
#define EFI_BUFFER_TOO_SMALL    ((EFI_STATUS)(EFI_ERRBIT | 0x5))
#define EFI_NOT_READY           ((EFI_STATUS)(EFI_ERRBIT | 0x6))
#define EFI_DEVICE_ERROR        ((EFI_STATUS)(EFI_ERRBIT | 0x7))
#define EFI_WRITE_PROTECTED     ((EFI_STATUS)(EFI_ERRBIT | 0x8))
#define EFI_OUT_OF_RESOURCES    ((EFI_STATUS)(EFI_ERRBIT | 0x9))
#define EFI_NOT_FOUND           ((EFI_STATUS)(EFI_ERRBIT | 0xA))
#define EFI_ACCESS_DENIED       ((EFI_STATUS)(EFI_ERRBIT | 0xB))
#define EFI_TIMEOUT             ((EFI_STATUS)(EFI_ERRBIT | 0xC))
#define EFI_ABORTED             ((EFI_STATUS)(EFI_ERRBIT | 0xF))

#define EFI_ERROR(s)            (((INTN)(s)) < 0)

typedef struct {
    UINT32  Data1;
    UINT16  Data2;
    UINT16  Data3;
    UINT8   Data4[8];
} EFI_GUID;

typedef struct {
    UINT64  Signature;
    UINT32  Revision;
    UINT32  HeaderSize;
    UINT32  CRC32;
    UINT32  Reserved;
} EFI_TABLE_HEADER;

/* ---- simple text output ---- */
typedef struct {
    INT32  MaxMode;
    INT32  Mode;
    INT32  Attribute;
    INT32  CursorColumn;
    INT32  CursorRow;
    BOOLEAN CursorVisible;
} EFI_SIMPLE_TEXT_OUTPUT_MODE;

typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT;
struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_STATUS (*Reset)(EFI_SIMPLE_TEXT_OUTPUT *This, BOOLEAN ExtendedVerification);
    EFI_STATUS (*OutputString)(EFI_SIMPLE_TEXT_OUTPUT *This, CHAR16 *String);
    EFI_STATUS (*TestString)(EFI_SIMPLE_TEXT_OUTPUT *This, CHAR16 *String);
    EFI_STATUS (*QueryMode)(EFI_SIMPLE_TEXT_OUTPUT *This, UINTN ModeNumber, UINTN *Columns, UINTN *Rows);
    EFI_STATUS (*SetMode)(EFI_SIMPLE_TEXT_OUTPUT *This, UINTN ModeNumber);
    EFI_STATUS (*SetAttribute)(EFI_SIMPLE_TEXT_OUTPUT *This, UINTN Attribute);
    EFI_STATUS (*ClearScreen)(EFI_SIMPLE_TEXT_OUTPUT *This);
    EFI_STATUS (*SetCursorPosition)(EFI_SIMPLE_TEXT_OUTPUT *This, UINTN Column, UINTN Row);
    EFI_STATUS (*EnableCursor)(EFI_SIMPLE_TEXT_OUTPUT *This, BOOLEAN Visible);
    EFI_SIMPLE_TEXT_OUTPUT_MODE *Mode;
};

/* ---- simple text input ---- */
typedef struct {
    UINT16  ScanCode;
    CHAR16  UnicodeChar;
} EFI_INPUT_KEY;

typedef struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL EFI_SIMPLE_TEXT_INPUT;
struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
    EFI_STATUS (*Reset)(EFI_SIMPLE_TEXT_INPUT *This, BOOLEAN ExtendedVerification);
    EFI_STATUS (*ReadKeyStroke)(EFI_SIMPLE_TEXT_INPUT *This, EFI_INPUT_KEY *Key);
    EFI_HANDLE WaitForKey;
};

/* ---- serial I/O ---- */
typedef struct {
    UINT32  ControlMask;
} EFI_SERIAL_CONTROL_BITS;
/* bit definitions of control mask */
#define EFI_SERIAL_CTS_HANDSHAKE            0x0001
#define EFI_SERIAL_DSR_HANDSHAKE            0x0002
#define EFI_SERIAL_DCD_HANDSHAKE            0x0004
#define EFI_SERIAL_RTS_ACTIVE               0x0008
#define EFI_SERIAL_DTR_ACTIVE               0x0010
#define EFI_SERIAL_OUTPUT_BUFFER_EMPTY      0x0020
#define EFI_SERIAL_INPUT_BUFFER_EMPTY       0x0040
#define EFI_SERIAL_SOFTWARE_LOOPBACK        0x0040

typedef struct {
    UINT32  Timeout;
    UINT64  BaudRate;
    UINT32  ReceiveFifoDepth;
    UINT32  DataBits;
    UINT32  Parity;
    UINT32  StopBits;
} EFI_SERIAL_IO_MODE;

typedef struct EFI_SERIAL_IO_PROTOCOL EFI_SERIAL_IO;
struct EFI_SERIAL_IO_PROTOCOL {
    EFI_STATUS (*Reset)(EFI_SERIAL_IO *This);
    EFI_STATUS (*SetAttributes)(EFI_SERIAL_IO *This, UINT64 BaudRate, UINT32 ReceiveFifoDepth,
                                UINT32 Timeout, UINT32 Parity, UINT32 DataBits, UINT32 StopBits);
    EFI_STATUS (*SetControlBits)(EFI_SERIAL_IO *This, UINT32 Control);
    EFI_STATUS (*ClearControlBits)(EFI_SERIAL_IO *This, UINT32 Control);
    EFI_STATUS (*GetControlBits)(EFI_SERIAL_IO *This, UINT32 *Control);
    EFI_STATUS (*Write)(EFI_SERIAL_IO *This, UINTN *BufferSize, void *Buffer);
    EFI_STATUS (*Read)(EFI_SERIAL_IO *This, UINTN *BufferSize, void *Buffer);
    EFI_SERIAL_IO_MODE *Mode;
};

/* ---- block I/O ---- */
typedef struct {
    UINT32  MediaId;
    BOOLEAN RemovableMedia;
    BOOLEAN MediaPresent;
    BOOLEAN LogicalPartition;
    BOOLEAN ReadOnly;
    BOOLEAN WriteCaching;
    UINT32  BlockSize;
    UINT32  IoAlign;
    UINT64  LastBlock;
    UINT64  LowestAlignedLba;
    UINT32  LogicalBlocksPerPhysicalBlock;
    UINT32  OptimalTransferLengthGranularity;
    UINT32  OptimalTransferLength;
} EFI_BLOCK_IO_MEDIA;

typedef struct EFI_BLOCK_IO_PROTOCOL EFI_BLOCK_IO;
struct EFI_BLOCK_IO_PROTOCOL {
    UINT64  Revision;
    EFI_BLOCK_IO_MEDIA *Media;
    EFI_STATUS (*Reset)(EFI_BLOCK_IO *This, BOOLEAN ExtendedVerification);
    EFI_STATUS (*ReadBlocks)(EFI_BLOCK_IO *This, UINT32 MediaId, UINT64 Lba, UINTN BufferSize, void *Buffer);
    EFI_STATUS (*WriteBlocks)(EFI_BLOCK_IO *This, UINT32 MediaId, UINT64 Lba, UINTN BufferSize, void *Buffer);
    EFI_STATUS (*FlushBlocks)(EFI_BLOCK_IO *This);
};

/* ---- loaded image ---- */
typedef struct {
    UINT32  Revision;
    EFI_HANDLE ParentHandle;
    void    *SystemTable;
    EFI_HANDLE DeviceHandle;
    void    *FilePath;
    void    *Reserved;
    UINT32  LoadOptionsSize;
    void    *LoadOptions;
    void    *ImageBase;
    UINT64  ImageSize;
    UINT64  ImageCodeType;
    UINT64  ImageDataType;
    EFI_STATUS (*Unload)(EFI_HANDLE ImageHandle);
} EFI_LOADED_IMAGE_PROTOCOL;

/* ---- device path (generic node) ---- */
typedef struct {
    UINT8   Type;
    UINT8   SubType;
    UINT16  Length;
} EFI_DEVICE_PATH_PROTOCOL;

/* ---- system table ---- */
typedef struct EFI_SYSTEM_TABLE EFI_SYSTEM_TABLE;
typedef struct EFI_BOOT_SERVICES EFI_BOOT_SERVICES;
typedef struct EFI_RUNTIME_SERVICES EFI_RUNTIME_SERVICES;

struct EFI_RUNTIME_SERVICES {
    EFI_TABLE_HEADER Hdr;
    EFI_STATUS (*GetTime)(void *Time, void *Capabilities);
    EFI_STATUS (*SetTime)(void *Time);
    EFI_STATUS (*GetWakeupTime)(BOOLEAN *Enabled, BOOLEAN *Pending, void *Time);
    EFI_STATUS (*SetWakeupTime)(BOOLEAN Enable, void *Time);
    EFI_STATUS (*SetVirtualAddressMap)(UINTN MapSize, UINTN DescriptorSize, UINT32 DescriptorVersion, void *VirtualMap);
    EFI_STATUS (*ConvertPointer)(UINTN DebugDisposition, void **Address);
    EFI_STATUS (*GetVariable)(CHAR16 *VariableName, EFI_GUID *VendorGuid, UINT32 *Attributes, UINTN *DataSize, void *Data);
    EFI_STATUS (*GetNextVariableName)(UINTN *VariableNameSize, CHAR16 *VariableName, EFI_GUID *VendorGuid);
    EFI_STATUS (*SetVariable)(CHAR16 *VariableName, EFI_GUID *VendorGuid, UINT32 Attributes, UINTN DataSize, void *Data);
    EFI_STATUS (*GetNextHighMonotonicCount)(UINT32 *HighCount);
    EFI_STATUS (*ResetSystem)(UINT32 ResetType, EFI_STATUS ResetStatus, UINTN DataSize, void *ResetData);
};

struct EFI_BOOT_SERVICES {
    EFI_TABLE_HEADER Hdr;
    /* Task Priority Services */
    EFI_STATUS (*RaiseTPL)(UINTN NewTPL);
    void       (*RestoreTPL)(UINTN OldTPL);
    /* Memory Services */
    EFI_STATUS (*AllocatePages)(UINT32 Type, UINTN MemoryType, UINTN Pages, UINT64 *Memory);
    EFI_STATUS (*FreePages)(UINT64 Memory, UINTN Pages);
    EFI_STATUS (*GetMemoryMap)(UINTN *MemoryMapSize, void *MemoryMap, UINTN *MapKey,
                               UINTN *DescriptorSize, UINT32 *DescriptorVersion);
    EFI_STATUS (*AllocatePool)(UINTN PoolType, UINTN Size, void **Buffer);
    EFI_STATUS (*FreePool)(void *Buffer);
    /* Event & Timer Services */
    EFI_STATUS (*CreateEvent)(UINT32 Type, UINTN NotifyTpl, void *NotifyFunction, void *NotifyContext, void *Event);
    EFI_STATUS (*SetTimer)(void *Event, UINT32 Type, UINT64 TriggerTime);
    EFI_STATUS (*WaitForEvent)(UINTN NumberOfEvents, void **Event, UINTN *Index);
    EFI_STATUS (*SignalEvent)(void *Event);
    EFI_STATUS (*CloseEvent)(void *Event);
    EFI_STATUS (*CheckEvent)(void *Event);
    /* Protocol Handler Services */
    EFI_STATUS (*InstallProtocolInterface)(EFI_HANDLE *Handle, EFI_GUID *Protocol, UINT32 InterfaceType, void *Interface);
    EFI_STATUS (*ReinstallProtocolInterface)(EFI_HANDLE Handle, EFI_GUID *Protocol, void *OldInterface, void *NewInterface);
    EFI_STATUS (*UninstallProtocolInterface)(EFI_HANDLE Handle, EFI_GUID *Protocol, void *Interface);
    EFI_STATUS (*HandleProtocol)(EFI_HANDLE Handle, EFI_GUID *Protocol, void **Interface);
    void       *Reserved1;
    EFI_STATUS (*RegisterProtocolNotify)(EFI_GUID *Protocol, void *Event, void **Registration);
    EFI_STATUS (*LocateHandle)(UINT32 SearchType, EFI_GUID *Protocol, void *SearchKey, UINTN *BufferSize, EFI_HANDLE *Buffer);
    EFI_STATUS (*LocateDevicePath)(EFI_GUID *Protocol, EFI_DEVICE_PATH_PROTOCOL **DevicePath, EFI_HANDLE *Device);
    EFI_STATUS (*InstallConfigurationTable)(EFI_GUID *Guid, void *Table);
    /* Image Services */
    EFI_STATUS (*LoadImage)(BOOLEAN BootPolicy, EFI_HANDLE ParentImageHandle, EFI_DEVICE_PATH_PROTOCOL *DevicePath,
                            void *SourceBuffer, UINTN SourceSize, EFI_HANDLE *ImageHandle);
    EFI_STATUS (*StartImage)(EFI_HANDLE ImageHandle, UINTN *ExitDataSize, CHAR16 **ExitData);
    EFI_STATUS (*Exit)(EFI_HANDLE ImageHandle, EFI_STATUS ExitStatus, UINTN ExitDataSize, CHAR16 *ExitData);
    EFI_STATUS (*UnloadImage)(EFI_HANDLE ImageHandle);
    EFI_STATUS (*ExitBootServices)(EFI_HANDLE ImageHandle, UINTN MapKey);
    /* Miscellaneous Services */
    EFI_STATUS (*GetNextMonotonicCount)(UINT64 *Count);
    EFI_STATUS (*Stall)(UINTN Microseconds);
    EFI_STATUS (*SetWatchdogTimer)(UINTN Timeout, UINT64 WatchdogCode, UINTN DataSize, CHAR16 *WatchdogData);
    /* DriverSupport Services */
    EFI_STATUS (*ConnectController)(EFI_HANDLE ControllerHandle, EFI_HANDLE *DriverImageHandle,
                                    EFI_DEVICE_PATH_PROTOCOL *RemainingDevicePath, BOOLEAN Recursive);
    EFI_STATUS (*DisconnectController)(EFI_HANDLE ControllerHandle, EFI_HANDLE DriverImageHandle,
                                       EFI_HANDLE ChildHandle);
    /* Open and Close Protocol Services */
    EFI_STATUS (*OpenProtocol)(EFI_HANDLE Handle, EFI_GUID *Protocol, void **Interface, EFI_HANDLE AgentHandle,
                               EFI_HANDLE ControllerHandle, UINT32 Attributes);
    EFI_STATUS (*CloseProtocol)(EFI_HANDLE Handle, EFI_GUID *Protocol, EFI_HANDLE AgentHandle, EFI_HANDLE ControllerHandle);
    EFI_STATUS (*OpenProtocolInformation)(EFI_HANDLE Handle, EFI_GUID *Protocol, void *EntryBuffer, UINTN *EntryCount);
    /* Library Services */
    EFI_STATUS (*ProtocolsPerHandle)(EFI_HANDLE Handle, EFI_GUID ***ProtocolBuffer, UINTN *ProtocolBufferCount);
    EFI_STATUS (*LocateHandleBuffer)(UINT32 SearchType, EFI_GUID *Protocol, void *SearchKey,
                                     UINTN *NoHandles, EFI_HANDLE **Buffer);
    EFI_STATUS (*LocateProtocol)(EFI_GUID *Protocol, void *Registration, void **Interface);
    EFI_STATUS (*InstallMultipleProtocolInterfaces)(EFI_HANDLE *Handle, ...);
    EFI_STATUS (*UninstallMultipleProtocolInterfaces)(EFI_HANDLE Handle, ...);
    /* 32-bit CRC Services */
    EFI_STATUS (*CalculateCrc32)(void *Data, UINTN DataSize, UINT32 *Crc32);
    /* Miscellaneous Services */
    void       (*CopyMem)(void *Destination, void *Source, UINTN Length);
    void       (*SetMem)(void *Buffer, UINTN Size, UINT8 Value);
    EFI_STATUS (*CreateEventEx)(UINT32 Type, UINTN NotifyTpl, void *NotifyFunction, void *NotifyContext,
                                EFI_GUID *EventGroup, void *Event);
};

struct EFI_SYSTEM_TABLE {
    EFI_TABLE_HEADER Hdr;
    CHAR16 *FirmwareVendor;
    UINT32  FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    EFI_SIMPLE_TEXT_INPUT *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT *ConOut;
    EFI_HANDLE StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT *StdErr;
    EFI_RUNTIME_SERVICES *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
    UINTN   NumberOfTableEntries;
    void   *ConfigurationTable;
};

/* ---- console reset types for ResetSystem ---- */
#define EfiResetCold        0
#define EfiResetWarm        1
#define EfiResetShutdown    2
#define EfiResetPlatformSpecific 3

/* ---- protocol GUIDs ---- */
extern EFI_GUID gEfiLoadedImageProtocolGuid;
extern EFI_GUID gEfiSimpleFileSystemProtocolGuid;
extern EFI_GUID gEfiBlockIoProtocolGuid;
extern EFI_GUID gEfiSerialIoProtocolGuid;
extern EFI_GUID gEfiDevicePathProtocolGuid;

/* UEFI services handles (efi_plat.c) */
extern EFI_BOOT_SERVICES *gBS;

/* ---- simple file system (only used to locate the boot volume's device path) ---- */
typedef struct {
    EFI_STATUS (*Reset)(void *This, BOOLEAN ExtendedVerification);
    EFI_STATUS (*ReadKeyStroke)(void *This, void *Key);
    EFI_HANDLE WaitForKey;
} EFI_DUMMY_KEEP;
#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_REVISION 0x00010000

/* ---- device path: HD() node as found on partitions ---- */
#define DEVICE_PATH_TYPE_MEDIA_DEVICE_PATH          4
#define DEVICE_PATH_SUBTYPE_MEDIA_HARD_DRIVE        1
typedef struct {
    UINT8   Type;
    UINT8   SubType;
    UINT16  Length;
    UINT32  PartitionNumber;
    UINT64  PartitionStart;
    UINT64  PartitionSize;
    UINT8   Signature[16];
    UINT8   MBRType;
    UINT8   SignatureType;
} HARD_DRIVE_DEVICE_PATH;

#define DEVICE_PATH_END       0x7f

#endif /* UEFI_EFI_H */
