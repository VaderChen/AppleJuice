#pragma once
// Windows DirectMusic COM ABI：GUID、vtable 順序與參數寬度。
// ABI 比對 MinGW Windows SDK 的 dmusici.h／dmusicc.h；語義參照 Microsoft Learn。
#include "x64arm/windows_abi.hpp"
#include <array>
namespace x64arm::dmusic {
using Guid=std::array<std::uint8_t,16>;
inline constexpr Guid IUnknown{0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
inline constexpr Guid CLSID_DirectMusicLoader{0x92,0x28,0xac,0xd2,0x9b,0xb3,0xd1,0x11,0x87,0x4,0x0,0x60,0x8,0x93,0xb1,0xbd};
inline constexpr Guid CLSID_DirectMusicPerformance{0x81,0x28,0xac,0xd2,0x9b,0xb3,0xd1,0x11,0x87,0x4,0x0,0x60,0x8,0x93,0xb1,0xbd};
inline constexpr Guid CLSID_DirectMusicSegment{0x82,0x28,0xac,0xd2,0x9b,0xb3,0xd1,0x11,0x87,0x4,0x0,0x60,0x8,0x93,0xb1,0xbd};
inline constexpr Guid IID_IDirectMusicLoader{0xa2,0xac,0xfa,0x2f,0xca,0x5d,0xd2,0x11,0xaf,0xa6,0x0,0xaa,0x0,0x24,0xd8,0xb6};
inline constexpr Guid IID_IDirectMusicLoader8{0x8c,0xc0,0xe7,0x19,0x44,0xa,0x6a,0x4e,0xa1,0x16,0x59,0x5a,0x7c,0xd5,0xde,0x8c};
inline constexpr Guid IID_IDirectMusicPerformance{0x3,0x3d,0xd4,0x7,0x23,0x65,0xd2,0x11,0x87,0x1d,0x0,0x60,0x8,0x93,0xb1,0xbd};
inline constexpr Guid IID_IDirectMusicPerformance2{0xe0,0xca,0xc2,0x6f,0x78,0xbc,0xd2,0x11,0xaf,0xa6,0x0,0xaa,0x0,0x24,0xd8,0xb6};
inline constexpr Guid IID_IDirectMusicPerformance8{0x37,0x41,0x9c,0x67,0x2e,0xc6,0x47,0x41,0xb2,0xb4,0x9d,0x56,0x9a,0xcb,0x25,0x4c};
inline constexpr Guid IID_IDirectMusicSegment{0xa2,0x29,0x60,0xf9,0x82,0x42,0xd2,0x11,0x87,0x17,0x0,0x60,0x8,0x93,0xb1,0xbd};
inline constexpr Guid IID_IDirectMusicSegment2{0xd1,0x94,0x88,0xd3,0x52,0xc0,0xd2,0x11,0x87,0x2f,0x0,0x60,0x8,0x93,0xb1,0xbd};
inline constexpr Guid IID_IDirectMusicSegment8{0x88,0x44,0x78,0xc6,0xa3,0x41,0x8f,0x41,0xaa,0x15,0xb3,0x50,0x93,0xba,0x42,0xd4};
inline constexpr Guid IID_IDirectMusicSegmentState{0xc7,0xdc,0xaf,0xa3,0xee,0xd3,0xd1,0x11,0xbc,0x8d,0x0,0xa0,0xc9,0x22,0xe6,0xeb};
inline constexpr Guid IID_IDirectMusicSegmentState8{0x30,0x47,0xe,0xa5,0xe4,0xa,0xa7,0x48,0x98,0x39,0xbc,0x4,0xbf,0xe0,0x77,0x72};
inline constexpr Guid GUID_StandardMIDIFile{0x75,0x10,0x62,0x6,0x2e,0xe9,0xd1,0x11,0xa8,0xc5,0x0,0xc0,0x4f,0xa3,0x72,0x6e};
inline constexpr Guid GUID_Download{0xa7,0x28,0xac,0xd2,0x9b,0xb3,0xd1,0x11,0x87,0x4,0x0,0x60,0x8,0x93,0xb1,0xbd};
inline constexpr Guid GUID_Unload{0xa8,0x28,0xac,0xd2,0x9b,0xb3,0xd1,0x11,0x87,0x4,0x0,0x60,0x8,0x93,0xb1,0xbd};
inline constexpr Guid IID_IDirectMusicAudioPath{0xf5,0x31,0x76,0xc8,0xbe,0x23,0x86,0x49,0x88,0x36,0x5,0x83,0x2f,0xcc,0x48,0xf9};
inline constexpr Guid IID_IDirectMusicObject{0xb5,0x28,0xac,0xd2,0x9b,0xb3,0xd1,0x11,0x87,0x4,0x0,0x60,0x8,0x93,0xb1,0xbd};
inline constexpr Guid GUID_PerfMasterTempo{0xb0,0x28,0xac,0xd2,0x9b,0xb3,0xd1,0x11,0x87,0x4,0x0,0x60,0x8,0x93,0xb1,0xbd};
inline constexpr Guid GUID_PerfMasterVolume{0xb1,0x28,0xac,0xd2,0x9b,0xb3,0xd1,0x11,0x87,0x4,0x0,0x60,0x8,0x93,0xb1,0xbd};
inline constexpr Guid GUID_PerfAutoDownload{0x5b,0x56,0x9,0xfb,0x31,0x36,0xd2,0x11,0xbc,0xb8,0x0,0xa0,0xc9,0x22,0xe6,0xeb};
inline constexpr Guid GUID_TempoParam{0xa5,0x28,0xac,0xd2,0x9b,0xb3,0xd1,0x11,0x87,0x4,0x0,0x60,0x8,0x93,0xb1,0xbd};
inline constexpr Guid GUID_TimeSignature{0xa4,0x28,0xac,0xd2,0x9b,0xb3,0xd1,0x11,0x87,0x4,0x0,0x60,0x8,0x93,0xb1,0xbd};
inline constexpr Guid GUID_DirectMusicAllTypes{0x93,0x28,0xac,0xd2,0x9b,0xb3,0xd1,0x11,0x87,0x4,0x0,0x60,0x8,0x93,0xb1,0xbd};
inline constexpr Guid GUID_NOTIFICATION_SEGMENT{0x99,0x28,0xac,0xd2,0x9b,0xb3,0xd1,0x11,0x87,0x4,0x0,0x60,0x8,0x93,0xb1,0xbd};
inline constexpr Guid GUID_NOTIFICATION_PERFORMANCE{0xc5,0x5b,0xf7,0x81,0x5d,0x4e,0xd2,0x11,0xbc,0xc7,0x0,0xa0,0xc9,0x22,0xe6,0xeb};
inline constexpr Guid IClassFactory{1,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
inline constexpr Guid CLSID_DirectMusic{0x10,0x9f,0x6b,0x63,0x7d,0x0c,0xd1,0x11,0x95,0xb2,0x00,0x20,0xaf,0xdc,0x74,0x21};
inline constexpr Guid CLSID_DirectMusicSynth{0xd0,0xb4,0xc2,0x58,0xe7,0x46,0xd1,0x11,0x89,0xac,0x00,0xa0,0xc9,0x05,0x41,0x29};
inline constexpr Guid IID_IDirectMusic{0x5a,0x11,0x36,0x65,0x2d,0x7b,0xd2,0x11,0xba,0x18,0x00,0x00,0xf8,0x75,0xac,0x12};
inline constexpr Guid IID_IDirectMusic2{0xe1,0xca,0xc2,0x6f,0x78,0xbc,0xd2,0x11,0xaf,0xa6,0x00,0xaa,0x00,0x24,0xd8,0xb6};
inline constexpr Guid IID_IDirectMusic8{0xf7,0x29,0x36,0x2d,0x3d,0x81,0x39,0x49,0x85,0x08,0xf0,0x5c,0x6b,0x75,0xfd,0x97};
inline constexpr Guid IID_IDirectMusicBuffer{0x78,0x28,0xac,0xd2,0x9b,0xb3,0xd1,0x11,0x87,0x04,0x00,0x60,0x08,0x93,0xb1,0xbd};
inline constexpr Guid IID_IDirectMusicPort{0xc9,0xd8,0xf2,0x08,0xc2,0x37,0xd2,0x11,0xb9,0xf9,0x00,0x00,0xf8,0x75,0xac,0x12};
inline constexpr Guid IID_IReferenceClock{0x97,0x68,0xa8,0x56,0xd4,0x0a,0xce,0x11,0xb0,0x3a,0x00,0x20,0xaf,0x0b,0xa7,0x70};
inline constexpr Guid CLSID_SystemClock{0xb0,0xeb,0x36,0xe4,0x4f,0x52,0xce,0x11,0x9f,0x53,0x00,0x20,0xaf,0x0b,0xa7,0x70};
inline constexpr Guid MIDI_BufferFormat{0x60,0x27,0x26,0x1d,0x57,0xe9,0xcf,0x11,0xa5,0xd6,0x28,0xdb,0x04,0xc1,0x00,0x00};
inline constexpr WindowsServiceDescriptor C_methods[]{
    {"internal-dmusic","C_QueryInterface",3},{"internal-dmusic","C_AddRef",1},{"internal-dmusic","C_Release",1},
    {"internal-dmusic","C_EnumPort",3,0,0,2},{"internal-dmusic","C_CreateMusicBuffer",4},{"internal-dmusic","C_CreatePort",5},
    {"internal-dmusic","C_EnumMasterClock",3,0,0,2},{"internal-dmusic","C_GetMasterClock",3},{"internal-dmusic","C_SetMasterClock",2},
    {"internal-dmusic","C_Activate",2,0,0,2},{"internal-dmusic","C_GetDefaultPort",2},{"internal-dmusic","C_SetDirectSound",3},
    {"internal-dmusic","C_SetExternalMasterClock",2}
};
inline constexpr WindowsServiceDescriptor B_methods[]{
    {"internal-dmusic","B_QueryInterface",3},{"internal-dmusic","B_AddRef",1},{"internal-dmusic","B_Release",1},
    {"internal-dmusic","B_Flush",1},{"internal-dmusic","B_TotalTime",2},{"internal-dmusic","B_PackStructured",4,2,0,12},
    {"internal-dmusic","B_PackUnstructured",5,2,0,12},{"internal-dmusic","B_ResetReadPtr",1},{"internal-dmusic","B_GetNextEvent",5},
    {"internal-dmusic","B_GetRawBufferPtr",2},{"internal-dmusic","B_GetStartTime",2},{"internal-dmusic","B_GetUsedBytes",2},
    {"internal-dmusic","B_GetMaxBytes",2},{"internal-dmusic","B_GetBufferFormat",2},{"internal-dmusic","B_SetStartTime",2,2},
    {"internal-dmusic","B_SetUsedBytes",2,0,0,2}
};
inline constexpr WindowsServiceDescriptor R_methods[]{
    {"internal-dmusic","R_QueryInterface",3},{"internal-dmusic","R_AddRef",1},{"internal-dmusic","R_Release",1},
    {"internal-dmusic","R_PlayBuffer",2},{"internal-dmusic","R_SetReadNotificationHandle",2},{"internal-dmusic","R_Read",2},
    {"internal-dmusic","R_DownloadInstrument",5,0,0,16},{"internal-dmusic","R_UnloadInstrument",2},{"internal-dmusic","R_GetLatencyClock",2},
    {"internal-dmusic","R_GetRunningStats",2},{"internal-dmusic","R_Compact",1},{"internal-dmusic","R_GetCaps",2},
    {"internal-dmusic","R_DeviceIoControl",8,0,0,42},{"internal-dmusic","R_SetNumChannelGroups",2,0,0,2},
    {"internal-dmusic","R_GetNumChannelGroups",2},{"internal-dmusic","R_Activate",2,0,0,2},
    {"internal-dmusic","R_SetChannelPriority",4,0,0,14},{"internal-dmusic","R_GetChannelPriority",4,0,0,6},
    {"internal-dmusic","R_SetDirectSound",3},{"internal-dmusic","R_GetFormat",4}
};
inline constexpr WindowsServiceDescriptor K_methods[]{
    {"internal-dmusic","K_QueryInterface",3},{"internal-dmusic","K_AddRef",1},{"internal-dmusic","K_Release",1},
    {"internal-dmusic","K_GetTime",2},{"internal-dmusic","K_AdviseTime",5,6},{"internal-dmusic","K_AdvisePeriodic",5,6},
    {"internal-dmusic","K_Unadvise",2}
};
inline constexpr WindowsServiceDescriptor F_methods[]{
    {"internal-dmusic","F_QueryInterface",3},{"internal-dmusic","F_AddRef",1},{"internal-dmusic","F_Release",1},
    {"internal-dmusic","F_CreateInstance",4},{"internal-dmusic","F_LockServer",2,0,0,2}
};
inline constexpr WindowsServiceDescriptor L_methods[]{
    {"internal-dmusic","L_QueryInterface",3,0}, // 0
    {"internal-dmusic","L_AddRef",1,0}, // 1
    {"internal-dmusic","L_Release",1,0}, // 2
    {"internal-dmusic","L_GetObject",4,0}, // 3
    {"internal-dmusic","L_SetObject",2,0}, // 4
    {"internal-dmusic","L_SetSearchDirectory",4,0,0,8}, // 5
    {"internal-dmusic","L_ScanDirectory",4,0}, // 6
    {"internal-dmusic","L_CacheObject",2,0}, // 7
    {"internal-dmusic","L_ReleaseObject",2,0}, // 8
    {"internal-dmusic","L_ClearCache",2,0}, // 9
    {"internal-dmusic","L_EnableCache",3,0,0,4}, // 10
    {"internal-dmusic","L_EnumObject",4,0,0,4}, // 11
    {"internal-dmusic","L_CollectGarbage",1,0}, // 12
    {"internal-dmusic","L_ReleaseObjectByUnknown",2,0}, // 13
    {"internal-dmusic","L_LoadObjectFromFile",5,0}, // 14
};
inline constexpr WindowsServiceDescriptor P_methods[]{
    {"internal-dmusic","P_QueryInterface",3,0}, // 0
    {"internal-dmusic","P_AddRef",1,0}, // 1
    {"internal-dmusic","P_Release",1,0}, // 2
    {"internal-dmusic","P_Init",4,0}, // 3
    {"internal-dmusic","P_PlaySegment",5,8,0,4}, // 4
    {"internal-dmusic","P_Stop",5,0,0,24}, // 5
    {"internal-dmusic","P_GetSegmentState",3,0,0,4}, // 6
    {"internal-dmusic","P_SetPrepareTime",2,0,0,2}, // 7
    {"internal-dmusic","P_GetPrepareTime",2,0}, // 8
    {"internal-dmusic","P_SetBumperLength",2,0,0,2}, // 9
    {"internal-dmusic","P_GetBumperLength",2,0}, // 10
    {"internal-dmusic","P_SendPMsg",2,0}, // 11
    {"internal-dmusic","P_MusicToReferenceTime",3,0,0,2}, // 12
    {"internal-dmusic","P_ReferenceToMusicTime",3,2}, // 13
    {"internal-dmusic","P_IsPlaying",3,0}, // 14
    {"internal-dmusic","P_GetTime",3,0}, // 15
    {"internal-dmusic","P_AllocPMsg",3,0,0,2}, // 16
    {"internal-dmusic","P_FreePMsg",2,0}, // 17
    {"internal-dmusic","P_GetGraph",2,0}, // 18
    {"internal-dmusic","P_SetGraph",2,0}, // 19
    {"internal-dmusic","P_SetNotificationHandle",3,4}, // 20
    {"internal-dmusic","P_GetNotificationPMsg",2,0}, // 21
    {"internal-dmusic","P_AddNotificationType",2,0}, // 22
    {"internal-dmusic","P_RemoveNotificationType",2,0}, // 23
    {"internal-dmusic","P_AddPort",2,0}, // 24
    {"internal-dmusic","P_RemovePort",2,0}, // 25
    {"internal-dmusic","P_AssignPChannelBlock",4,0,0,10}, // 26
    {"internal-dmusic","P_AssignPChannel",5,0,0,26}, // 27
    {"internal-dmusic","P_PChannelInfo",5,0}, // 28
    {"internal-dmusic","P_DownloadInstrument",9,0,0,36}, // 29
    {"internal-dmusic","P_Invalidate",3,0,0,6}, // 30
    {"internal-dmusic","P_GetParam",7,0,0,28}, // 31
    {"internal-dmusic","P_SetParam",6,0,0,28}, // 32
    {"internal-dmusic","P_GetGlobalParam",4,0,0,8}, // 33
    {"internal-dmusic","P_SetGlobalParam",4,0,0,8}, // 34
    {"internal-dmusic","P_GetLatencyTime",2,0}, // 35
    {"internal-dmusic","P_GetQueueTime",2,0}, // 36
    {"internal-dmusic","P_AdjustTime",2,2}, // 37
    {"internal-dmusic","P_CloseDown",1,0}, // 38
    {"internal-dmusic","P_GetResolvedTime",4,2,0,8}, // 39
    {"internal-dmusic","P_MIDIToMusic",6,0,0,26}, // 40
    {"internal-dmusic","P_MusicToMIDI",6,0,0,26}, // 41
    {"internal-dmusic","P_TimeToRhythm",7,0,0,2}, // 42
    {"internal-dmusic","P_RhythmToTime",7,0,0,30}, // 43
    {"internal-dmusic","P_InitAudio",8,0,0,112}, // 44
    {"internal-dmusic","P_PlaySegmentEx",9,32,0,16}, // 45
    {"internal-dmusic","P_StopEx",4,4,0,8}, // 46
    {"internal-dmusic","P_ClonePMsg",3,0}, // 47
    {"internal-dmusic","P_CreateAudioPath",4,0,0,4}, // 48
    {"internal-dmusic","P_CreateStandardAudioPath",5,0,0,14}, // 49
    {"internal-dmusic","P_SetDefaultAudioPath",2,0}, // 50
    {"internal-dmusic","P_GetDefaultAudioPath",2,0}, // 51
    {"internal-dmusic","P_GetParamEx",8,0,0,60}, // 52
};
inline constexpr WindowsServiceDescriptor S_methods[]{
    {"internal-dmusic","S_QueryInterface",3,0}, // 0
    {"internal-dmusic","S_AddRef",1,0}, // 1
    {"internal-dmusic","S_Release",1,0}, // 2
    {"internal-dmusic","S_GetLength",2,0}, // 3
    {"internal-dmusic","S_SetLength",2,0,0,2}, // 4
    {"internal-dmusic","S_GetRepeats",2,0}, // 5
    {"internal-dmusic","S_SetRepeats",2,0,0,2}, // 6
    {"internal-dmusic","S_GetDefaultResolution",2,0}, // 7
    {"internal-dmusic","S_SetDefaultResolution",2,0,0,2}, // 8
    {"internal-dmusic","S_GetTrack",5,0,0,12}, // 9
    {"internal-dmusic","S_GetTrackGroup",3,0}, // 10
    {"internal-dmusic","S_InsertTrack",3,0,0,4}, // 11
    {"internal-dmusic","S_RemoveTrack",2,0}, // 12
    {"internal-dmusic","S_InitPlay",4,0,0,8}, // 13
    {"internal-dmusic","S_GetGraph",2,0}, // 14
    {"internal-dmusic","S_SetGraph",2,0}, // 15
    {"internal-dmusic","S_AddNotificationType",2,0}, // 16
    {"internal-dmusic","S_RemoveNotificationType",2,0}, // 17
    {"internal-dmusic","S_GetParam",7,0,0,28}, // 18
    {"internal-dmusic","S_SetParam",6,0,0,28}, // 19
    {"internal-dmusic","S_Clone",4,0,0,6}, // 20
    {"internal-dmusic","S_SetStartPoint",2,0,0,2}, // 21
    {"internal-dmusic","S_GetStartPoint",2,0}, // 22
    {"internal-dmusic","S_SetLoopPoints",3,0,0,6}, // 23
    {"internal-dmusic","S_GetLoopPoints",3,0}, // 24
    {"internal-dmusic","S_SetPChannelsUsed",3,0,0,2}, // 25
    {"internal-dmusic","S_SetTrackConfig",6,0,0,60}, // 26
    {"internal-dmusic","S_GetAudioPathConfig",2,0}, // 27
    {"internal-dmusic","S_Compose",5,0,0,4}, // 28
    {"internal-dmusic","S_Download",2,0}, // 29
    {"internal-dmusic","S_Unload",2,0}, // 30
};
inline constexpr WindowsServiceDescriptor T_methods[]{
    {"internal-dmusic","T_QueryInterface",3,0}, // 0
    {"internal-dmusic","T_AddRef",1,0}, // 1
    {"internal-dmusic","T_Release",1,0}, // 2
    {"internal-dmusic","T_GetRepeats",2,0}, // 3
    {"internal-dmusic","T_GetSegment",2,0}, // 4
    {"internal-dmusic","T_GetStartTime",2,0}, // 5
    {"internal-dmusic","T_GetSeek",2,0}, // 6
    {"internal-dmusic","T_GetStartPoint",2,0}, // 7
    {"internal-dmusic","T_SetTrackConfig",6,0,0,60}, // 8
    {"internal-dmusic","T_GetObjectInPath",8,0,0,46}, // 9
};

inline constexpr WindowsServiceDescriptor A_methods[]{
    {"internal-dmusic","A_QueryInterface",3},{"internal-dmusic","A_AddRef",1},{"internal-dmusic","A_Release",1},
    {"internal-dmusic","A_GetObjectInPath",8,0,0,46},{"internal-dmusic","A_Activate",2,0,0,2},{"internal-dmusic","A_SetVolume",3,0,0,6},{"internal-dmusic","A_ConvertPChannel",3,0,0,2}
};
inline constexpr WindowsServiceDescriptor O_methods[]{
    {"internal-dmusic","O_QueryInterface",3},{"internal-dmusic","O_AddRef",1},{"internal-dmusic","O_Release",1},
    {"internal-dmusic","O_GetDescriptor",2},{"internal-dmusic","O_SetDescriptor",2},{"internal-dmusic","O_ParseDescriptor",3}
};
inline Guid read_guid(const GuestMemory& memory,std::uint64_t p){Guid g{};memory.read(GuestAddress{p},g);return g;}
inline bool is_class(const GuestMemory& memory,std::uint64_t p){const auto g=read_guid(memory,p);return g==CLSID_DirectMusicLoader || g==CLSID_DirectMusicPerformance || g==CLSID_DirectMusicSegment || g==CLSID_DirectMusic;}
}
