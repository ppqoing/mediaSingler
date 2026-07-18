// DiskInfo.cpp : DLL entry point and implementation of GetPhysicalDiskNumber
//
// Implementation:
//   1. Extract drive letter from the input path (e.g. "C:" from "C:\Users\foo")
//   2. Open the volume device "\\.\X:"
//   3. Send IOCTL_STORAGE_GET_DEVICE_NUMBER to retrieve STORAGE_DEVICE_NUMBER
//   4. Return DeviceNumber (the physical drive index)

#include <windows.h>
#include <winioctl.h>
#include <string>
#include <vector>
#include <set>
#include <algorithm>
#include <cstring>
#include <exception>
#include <new>

#include "DiskInfo.h"

// Link theSetupapi / runtime libraries that we use
#pragma comment(lib, "kernel32.lib")

/** @brief 在所有 C++ 返回和异常路径关闭有效 Win32 HANDLE。 */
class ScopedHandle final {
public:
    explicit ScopedHandle(HANDLE handle = INVALID_HANDLE_VALUE) noexcept : handle_(handle) {}
    ~ScopedHandle() { if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_); }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    HANDLE get() const noexcept { return handle_; }

private:
    HANDLE handle_;
};

// ---------------------------------------------------------------------------
// Helper: UTF-8 -> UTF-16 wstring
// ---------------------------------------------------------------------------
static std::wstring Utf8ToWide(const char* utf8) {
    if (!utf8 || !*utf8) return L"";
    const int sourceLength = static_cast<int>(strlen(utf8));
    int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, sourceLength, nullptr, 0);
    if (len <= 0) return L"";
    std::wstring wide(len, L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, sourceLength, wide.data(), len) != len) {
        return L"";
    }
    return wide;
}

/**
 * @brief 把 UTF-16 文本安全写入固定 UTF-8 结果缓冲。
 * @param value UTF-16 文本。
 * @param destination 目标缓冲。
 * @param capacity 目标字节容量。
 * @return 完整写入返回 true，编码失败或容量不足返回 false。
 */
static bool WideToUtf8Buffer(const std::wstring& value, char* destination, const std::size_t capacity) {
    if (destination == nullptr || capacity == 0) return false;
    destination[0] = '\0';
    if (value.empty()) return true;
    const int length = WideCharToMultiByte(CP_UTF8,
                                           WC_ERR_INVALID_CHARS,
                                           value.data(),
                                           static_cast<int>(value.size()),
                                           nullptr,
                                           0,
                                           nullptr,
                                           nullptr);
    if (length <= 0 || static_cast<std::size_t>(length) >= capacity) return false;
    if (WideCharToMultiByte(CP_UTF8,
                            WC_ERR_INVALID_CHARS,
                            value.data(),
                            static_cast<int>(value.size()),
                            destination,
                            length,
                            nullptr,
                            nullptr) != length) {
        destination[0] = '\0';
        return false;
    }
    destination[length] = '\0';
    return true;
}

// ---------------------------------------------------------------------------
// Extract drive letter prefix from a path.
//   "C:\Users\foo"  -> "C:"
//   "D:\\video.mp4" -> "D:"
//   "E:"            -> "E:"
//   "\\server\share"-> "" (UNC, no drive letter)
// Returns empty string on failure.
// ---------------------------------------------------------------------------
static std::wstring ExtractDriveLetter(const std::wstring& path) {
    // Reject UNC paths \\server\share
    if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') {
        return L"";
    }
    // Drive letter must be: <letter>:
    if (path.size() < 2) return L"";
    wchar_t c = path[0];
    if (!((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z'))) return L"";
    if (path[1] != L':') return L"";
    // Normalize to uppercase letter + colon
    if (c >= L'a' && c <= L'z') c = c - L'a' + L'A';
    return std::wstring(1, c) + L":";
}

/**
 * @brief 打开 GetVolumePathNameW 返回的卷设备。
 * @param volumeGuid 带尾部反斜杠的卷 GUID。
 * @return 成功时返回卷句柄，失败返回 INVALID_HANDLE_VALUE。
 */
static HANDLE OpenVolumeDevice(const std::wstring& volumeGuid) {
    std::wstring device = volumeGuid;
    while (!device.empty() && (device.back() == L'\\' || device.back() == L'/')) {
        device.pop_back();
    }
    return CreateFileW(device.c_str(),
                       0,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr,
                       OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL,
                       nullptr);
}

/**
 * @brief 查询物理盘是否具有机械寻道惩罚。
 * @param diskNumber Windows PhysicalDrive 编号。
 * @param incursSeekPenalty 输出寻道惩罚标志；未知时保持 -1。
 * @return DISKINFO_MEDIA_* 介质类型。
 */
static int QueryPhysicalMediaType(const DWORD diskNumber, int* incursSeekPenalty) {
    if (incursSeekPenalty != nullptr) *incursSeekPenalty = -1;
    const std::wstring device = L"\\\\.\\PhysicalDrive" + std::to_wstring(diskNumber);
    HANDLE disk = CreateFileW(device.c_str(),
                              0,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (disk == INVALID_HANDLE_VALUE) return DISKINFO_MEDIA_UNKNOWN;

    STORAGE_PROPERTY_QUERY seekQuery{};
    seekQuery.PropertyId = StorageDeviceSeekPenaltyProperty;
    seekQuery.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR seekDescriptor{};
    DWORD returned = 0;
    if (DeviceIoControl(disk,
                        IOCTL_STORAGE_QUERY_PROPERTY,
                        &seekQuery,
                        sizeof(seekQuery),
                        &seekDescriptor,
                        sizeof(seekDescriptor),
                        &returned,
                        nullptr) &&
        returned >= sizeof(seekDescriptor)) {
        CloseHandle(disk);
        if (incursSeekPenalty != nullptr) *incursSeekPenalty = seekDescriptor.IncursSeekPenalty ? 1 : 0;
        return seekDescriptor.IncursSeekPenalty ? DISKINFO_MEDIA_HDD : DISKINFO_MEDIA_SSD;
    }

    STORAGE_PROPERTY_QUERY trimQuery{};
    trimQuery.PropertyId = StorageDeviceTrimProperty;
    trimQuery.QueryType = PropertyStandardQuery;
    DEVICE_TRIM_DESCRIPTOR trimDescriptor{};
    returned = 0;
    const BOOL trimKnown = DeviceIoControl(disk,
                                           IOCTL_STORAGE_QUERY_PROPERTY,
                                           &trimQuery,
                                           sizeof(trimQuery),
                                           &trimDescriptor,
                                           sizeof(trimDescriptor),
                                           &returned,
                                           nullptr);
    CloseHandle(disk);
    if (trimKnown && returned >= sizeof(trimDescriptor) && trimDescriptor.TrimEnabled) {
        return DISKINFO_MEDIA_SSD;
    }
    return DISKINFO_MEDIA_UNKNOWN;
}

/**
 * @brief 使用卷磁盘区间获取全部物理盘编号。
 * @param volume 已打开卷句柄。
 * @param diskNumbers 输出去重并排序的物理盘编号。
 * @return 查询成功且至少包含一个区间时返回 true。
 */
static bool QueryVolumeDiskNumbers(HANDLE volume, std::vector<DWORD>& diskNumbers) {
    constexpr std::size_t kMaximumExtents = 128;
    const std::size_t bufferSize = sizeof(VOLUME_DISK_EXTENTS) + sizeof(DISK_EXTENT) * (kMaximumExtents - 1);
    std::vector<std::uint8_t> buffer(bufferSize);
    DWORD returned = 0;
    if (!DeviceIoControl(volume,
                         IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                         nullptr,
                         0,
                         buffer.data(),
                         static_cast<DWORD>(buffer.size()),
                         &returned,
                         nullptr)) {
        return false;
    }
    const auto* extents = reinterpret_cast<const VOLUME_DISK_EXTENTS*>(buffer.data());
    if (extents->NumberOfDiskExtents == 0 || extents->NumberOfDiskExtents > kMaximumExtents) return false;
    std::set<DWORD> unique;
    for (DWORD index = 0; index < extents->NumberOfDiskExtents; ++index) {
        unique.insert(extents->Extents[index].DiskNumber);
    }
    diskNumbers.assign(unique.begin(), unique.end());
    return !diskNumbers.empty();
}

/**
 * @brief 读取卷的全部物理磁盘区间。
 * @param volume 已打开的卷句柄。
 * @param extents 输出卷区间；返回前按原始卷顺序保留。
 * @param error 输出 Win32 错误码。
 * @return 至少得到一个完整区间时返回 true。
 */
static bool QueryVolumeDiskExtents(HANDLE volume,
                                   std::vector<DISK_EXTENT>& extents,
                                   DWORD& error) {
    constexpr std::size_t kMaximumExtents = 128;
    const std::size_t bufferSize = sizeof(VOLUME_DISK_EXTENTS) + sizeof(DISK_EXTENT) * (kMaximumExtents - 1);
    std::vector<std::uint8_t> buffer(bufferSize);
    DWORD returned = 0;
    if (!DeviceIoControl(volume,
                         IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                         nullptr,
                         0,
                         buffer.data(),
                         static_cast<DWORD>(buffer.size()),
                         &returned,
                         nullptr)) {
        error = GetLastError();
        return false;
    }
    const auto* result = reinterpret_cast<const VOLUME_DISK_EXTENTS*>(buffer.data());
    if (result->NumberOfDiskExtents == 0 || result->NumberOfDiskExtents > kMaximumExtents) {
        error = ERROR_INVALID_DATA;
        return false;
    }
    extents.assign(result->Extents, result->Extents + result->NumberOfDiskExtents);
    error = ERROR_SUCCESS;
    return true;
}

/**
 * @brief 生成单盘 PhysicalDriveN 或多盘 PhysicalSet:n,m 调度键。
 * @param diskNumbers 已排序去重的物理盘编号。
 * @return 稳定调度键。
 */
static std::wstring BuildStorageTargetKey(const std::vector<DWORD>& diskNumbers) {
    if (diskNumbers.size() == 1) {
        return L"PhysicalDrive" + std::to_wstring(diskNumbers.front());
    }
    std::wstring key = L"PhysicalSet:";
    for (std::size_t index = 0; index < diskNumbers.size(); ++index) {
        if (index != 0) key += L",";
        key += std::to_wstring(diskNumbers[index]);
    }
    return key;
}

static int QueryDiskTopologyImpl(const char* path, DiskTopologyInfo* outInfo) {
    if (outInfo == nullptr) return 0;
    *outInfo = {};
    outInfo->statusCode = DISKINFO_TOPOLOGY_INVALID_ARGUMENT;
    outInfo->physicalDiskNumber = -1;
    outInfo->partitionNumber = -1;
    outInfo->incursSeekPenalty = -1;
    if (path == nullptr || path[0] == '\0') return 0;

    std::wstring widePath = Utf8ToWide(path);
    if (widePath.empty()) return 0;
    if (widePath.size() == 2 && widePath[1] == L':') widePath += L'\\';

    wchar_t volumePath[MAX_PATH]{};
    if (!GetVolumePathNameW(widePath.c_str(), volumePath, MAX_PATH)) {
        outInfo->statusCode = DISKINFO_TOPOLOGY_VOLUME_NOT_FOUND;
        outInfo->win32Error = GetLastError();
        return 0;
    }
    wchar_t volumeGuid[MAX_PATH]{};
    if (!GetVolumeNameForVolumeMountPointW(volumePath, volumeGuid, MAX_PATH)) {
        outInfo->statusCode = DISKINFO_TOPOLOGY_VOLUME_NOT_FOUND;
        outInfo->win32Error = GetLastError();
        return 0;
    }
    WideToUtf8Buffer(volumeGuid, outInfo->volumeGuid, sizeof(outInfo->volumeGuid));

    ScopedHandle volume(OpenVolumeDevice(volumeGuid));
    if (volume.get() == INVALID_HANDLE_VALUE) {
        outInfo->statusCode = DISKINFO_TOPOLOGY_OPEN_FAILED;
        outInfo->win32Error = GetLastError();
        return 0;
    }

    STORAGE_DEVICE_NUMBER storageNumber{};
    DWORD returned = 0;
    if (DeviceIoControl(volume.get(),
                        IOCTL_STORAGE_GET_DEVICE_NUMBER,
                        nullptr,
                        0,
                        &storageNumber,
                        sizeof(storageNumber),
                        &returned,
                        nullptr) &&
        returned >= sizeof(storageNumber)) {
        outInfo->partitionNumber = static_cast<int>(storageNumber.PartitionNumber);
    }

    std::vector<DWORD> diskNumbers;
    if (!QueryVolumeDiskNumbers(volume.get(), diskNumbers) && returned >= sizeof(storageNumber)) {
        diskNumbers.push_back(storageNumber.DeviceNumber);
    }
    if (diskNumbers.empty()) {
        outInfo->statusCode = DISKINFO_TOPOLOGY_QUERY_FAILED;
        outInfo->win32Error = GetLastError();
        return 0;
    }

    outInfo->physicalDiskNumber = diskNumbers.size() == 1 ? static_cast<int>(diskNumbers.front()) : -1;
    int combinedMediaType = DISKINFO_MEDIA_UNKNOWN;
    int combinedSeekPenalty = -1;
    for (const DWORD diskNumber : diskNumbers) {
        int seekPenalty = -1;
        const int mediaType = QueryPhysicalMediaType(diskNumber, &seekPenalty);
        if (combinedMediaType == DISKINFO_MEDIA_UNKNOWN) {
            combinedMediaType = mediaType;
            combinedSeekPenalty = seekPenalty;
        } else if (combinedMediaType != mediaType || combinedSeekPenalty != seekPenalty) {
            combinedMediaType = DISKINFO_MEDIA_UNKNOWN;
            combinedSeekPenalty = -1;
            break;
        }
    }
    outInfo->mediaType = combinedMediaType;
    outInfo->incursSeekPenalty = combinedSeekPenalty;
    WideToUtf8Buffer(BuildStorageTargetKey(diskNumbers),
                     outInfo->storageTargetKey,
                     sizeof(outInfo->storageTargetKey));
    outInfo->statusCode = DISKINFO_TOPOLOGY_OK;
    outInfo->win32Error = ERROR_SUCCESS;
    return 1;
}

// ===========================================================================
// GetPhysicalDiskNumber
// ===========================================================================
static int GetPhysicalDiskNumberImpl(const char* path) {
    if (!path || !path[0]) return -1;
    DiskTopologyInfo info{};
    if (!QueryDiskTopology(path, &info)) {
        if (info.statusCode == DISKINFO_TOPOLOGY_OPEN_FAILED) return -2;
        return info.statusCode == DISKINFO_TOPOLOGY_INVALID_ARGUMENT ? -1 : -3;
    }
    return info.physicalDiskNumber >= 0 ? info.physicalDiskNumber : -3;
}

static int QueryFilePhysicalLocationImpl(const char* path,
                                         FilePhysicalLocation* outLocation) {
    if (outLocation == nullptr) return 0;
    *outLocation = {};
    outLocation->statusCode = DISKINFO_FILE_LOCATION_INVALID_ARGUMENT;
    outLocation->physicalDiskNumber = -1;
    if (path == nullptr || path[0] == '\0') return 0;

    const std::wstring widePath = Utf8ToWide(path);
    if (widePath.empty()) return 0;
    ScopedHandle file(CreateFileW(widePath.c_str(),
                                  FILE_READ_ATTRIBUTES,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr,
                                  OPEN_EXISTING,
                                  FILE_FLAG_SEQUENTIAL_SCAN,
                                  nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) {
        outLocation->statusCode = DISKINFO_FILE_LOCATION_OPEN_FAILED;
        outLocation->win32Error = GetLastError();
        return 0;
    }

    STARTING_VCN_INPUT_BUFFER input{};
    RETRIEVAL_POINTERS_BUFFER output{};
    DWORD returned = 0;
    const BOOL located = DeviceIoControl(file.get(),
                                         FSCTL_GET_RETRIEVAL_POINTERS,
                                         &input,
                                         sizeof(input),
                                         &output,
                                         sizeof(output),
                                         &returned,
                                         nullptr);
    const DWORD retrievalError = located ? ERROR_SUCCESS : GetLastError();
    if ((!located && retrievalError != ERROR_MORE_DATA) || output.ExtentCount == 0 ||
        output.Extents[0].Lcn.QuadPart < 0) {
        outLocation->statusCode = DISKINFO_FILE_LOCATION_QUERY_FAILED;
        outLocation->win32Error = retrievalError == ERROR_SUCCESS ? ERROR_INVALID_DATA : retrievalError;
        return 0;
    }

    wchar_t volumePath[MAX_PATH]{};
    if (!GetVolumePathNameW(widePath.c_str(), volumePath, MAX_PATH)) {
        outLocation->statusCode = DISKINFO_FILE_LOCATION_QUERY_FAILED;
        outLocation->win32Error = GetLastError();
        return 0;
    }
    wchar_t volumeGuid[MAX_PATH]{};
    if (!GetVolumeNameForVolumeMountPointW(volumePath, volumeGuid, MAX_PATH)) {
        outLocation->statusCode = DISKINFO_FILE_LOCATION_QUERY_FAILED;
        outLocation->win32Error = GetLastError();
        return 0;
    }
    ScopedHandle volume(OpenVolumeDevice(volumeGuid));
    if (volume.get() == INVALID_HANDLE_VALUE) {
        outLocation->statusCode = DISKINFO_FILE_LOCATION_OPEN_FAILED;
        outLocation->win32Error = GetLastError();
        return 0;
    }
    std::vector<DISK_EXTENT> volumeExtents;
    DWORD extentError = ERROR_SUCCESS;
    const bool hasExtents = QueryVolumeDiskExtents(volume.get(), volumeExtents, extentError);
    if (!hasExtents) {
        outLocation->statusCode = DISKINFO_FILE_LOCATION_QUERY_FAILED;
        outLocation->win32Error = extentError;
        return 0;
    }
    if (volumeExtents.size() != 1) {
        outLocation->statusCode = DISKINFO_FILE_LOCATION_COMPLEX_VOLUME;
        outLocation->win32Error = ERROR_NOT_SUPPORTED;
        return 0;
    }

    DWORD sectorsPerCluster = 0;
    DWORD bytesPerSector = 0;
    DWORD freeClusters = 0;
    DWORD totalClusters = 0;
    if (!GetDiskFreeSpaceW(volumePath,
                           &sectorsPerCluster,
                           &bytesPerSector,
                           &freeClusters,
                           &totalClusters)) {
        outLocation->statusCode = DISKINFO_FILE_LOCATION_QUERY_FAILED;
        outLocation->win32Error = GetLastError();
        return 0;
    }
    const ULONGLONG bytesPerCluster =
        static_cast<ULONGLONG>(sectorsPerCluster) * static_cast<ULONGLONG>(bytesPerSector);
    const ULONGLONG logicalOffset =
        static_cast<ULONGLONG>(output.Extents[0].Lcn.QuadPart) * bytesPerCluster;
    const DISK_EXTENT& extent = volumeExtents.front();
    if (extent.StartingOffset.QuadPart < 0 || logicalOffset > static_cast<ULONGLONG>(extent.ExtentLength.QuadPart)) {
        outLocation->statusCode = DISKINFO_FILE_LOCATION_QUERY_FAILED;
        outLocation->win32Error = ERROR_ARITHMETIC_OVERFLOW;
        return 0;
    }
    outLocation->physicalDiskNumber = static_cast<int>(extent.DiskNumber);
    outLocation->physicalStartByte = static_cast<ULONGLONG>(extent.StartingOffset.QuadPart) + logicalOffset;
    outLocation->statusCode = DISKINFO_FILE_LOCATION_OK;
    outLocation->win32Error = ERROR_SUCCESS;
    return 1;
}

// ---------------------------------------------------------------------------
// C ABI exception barriers
// ---------------------------------------------------------------------------

/**
 * @brief 捕获磁盘拓扑实现的 C++ 异常并初始化稳定失败结果。
 * @return 成功返回 1，任何失败返回 0。
 */
DISKINFO_API int __cdecl QueryDiskTopology(const char* path, DiskTopologyInfo* outInfo) {
    if (outInfo == nullptr) return 0;
    *outInfo = {};
    outInfo->physicalDiskNumber = -1;
    outInfo->partitionNumber = -1;
    outInfo->incursSeekPenalty = -1;
    try {
        return QueryDiskTopologyImpl(path, outInfo);
    } catch (const std::bad_alloc&) {
        *outInfo = {};
        outInfo->statusCode = DISKINFO_TOPOLOGY_UNEXPECTED_FAILURE;
        outInfo->physicalDiskNumber = -1;
        outInfo->partitionNumber = -1;
        outInfo->incursSeekPenalty = -1;
        outInfo->win32Error = ERROR_NOT_ENOUGH_MEMORY;
    } catch (...) {
        *outInfo = {};
        outInfo->statusCode = DISKINFO_TOPOLOGY_UNEXPECTED_FAILURE;
        outInfo->physicalDiskNumber = -1;
        outInfo->partitionNumber = -1;
        outInfo->incursSeekPenalty = -1;
        outInfo->win32Error = ERROR_UNHANDLED_EXCEPTION;
    }
    return 0;
}

/** @brief 捕获兼容磁盘号查询接口异常并返回新增的 -4 状态。 */
DISKINFO_API int __cdecl GetPhysicalDiskNumber(const char* path) {
    try { return GetPhysicalDiskNumberImpl(path); } catch (...) { return -4; }
}

/**
 * @brief 捕获物理位置实现的 C++ 异常并初始化稳定失败结果。
 * @return 成功返回 1，任何失败返回 0。
 */
DISKINFO_API int __cdecl QueryFilePhysicalLocation(const char* path,
                                                    FilePhysicalLocation* outLocation) {
    if (outLocation == nullptr) return 0;
    *outLocation = {};
    outLocation->physicalDiskNumber = -1;
    try {
        return QueryFilePhysicalLocationImpl(path, outLocation);
    } catch (const std::bad_alloc&) {
        *outLocation = {};
        outLocation->statusCode = DISKINFO_FILE_LOCATION_UNEXPECTED_FAILURE;
        outLocation->physicalDiskNumber = -1;
        outLocation->win32Error = ERROR_NOT_ENOUGH_MEMORY;
    } catch (...) {
        *outLocation = {};
        outLocation->statusCode = DISKINFO_FILE_LOCATION_UNEXPECTED_FAILURE;
        outLocation->physicalDiskNumber = -1;
        outLocation->win32Error = ERROR_UNHANDLED_EXCEPTION;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// DLL entry
// ---------------------------------------------------------------------------
BOOL APIENTRY DllMain(HMODULE hModule,
                      DWORD  ul_reason_for_call,
                      LPVOID lpReserved)
{
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
