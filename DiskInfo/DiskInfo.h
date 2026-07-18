// DiskInfo.h : Public export API of DiskInfo DLL
//
// Feature: Get physical disk number for a given file path.
//
// A "physical disk number" is the integer index used by Windows to enumerate
// physical drives (e.g. \\.\PHYSICALDRIVE0, \\.\PHYSICALDRIVE1, ...). It is
// obtained via IOCTL_STORAGE_GET_DEVICE_NUMBER on the volume handle.

#ifndef DISKINFO_H
#define DISKINFO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef DISKINFO_EXPORTS
#define DISKINFO_API __declspec(dllexport)
#else
#define DISKINFO_API __declspec(dllimport)
#endif

// Get the physical disk number for the volume that contains the given path.
//
// Parameter:
//   path - UTF-8 file or directory path. Any path on a mounted volume works,
//          e.g. "C:\\Users\\foo", "D:\\video.mp4", "E:". Only the drive letter
//          prefix is examined; the rest of the path is ignored.
//
// Returns:
//   >= 0 : physical disk number (matches the number in \\.\PHYSICALDRIVE<n>)
//   -1   : invalid argument (null / empty / no drive letter)
//   -2   : cannot open volume device (e.g. access denied or invalid drive)
//   -3   : IOCTL_STORAGE_GET_DEVICE_NUMBER failed
//   -4   : unexpected C++ exception at the DLL boundary
DISKINFO_API int __cdecl GetPhysicalDiskNumber(const char* path);

/** @brief 未知、机械盘和固态盘枚举。 */
#define DISKINFO_MEDIA_UNKNOWN 0
#define DISKINFO_MEDIA_HDD     1
#define DISKINFO_MEDIA_SSD     2

/** @brief 磁盘拓扑查询状态码。 */
#define DISKINFO_TOPOLOGY_OK                 0
#define DISKINFO_TOPOLOGY_INVALID_ARGUMENT   1
#define DISKINFO_TOPOLOGY_VOLUME_NOT_FOUND   2
#define DISKINFO_TOPOLOGY_OPEN_FAILED        3
#define DISKINFO_TOPOLOGY_QUERY_FAILED       4
#define DISKINFO_TOPOLOGY_UNEXPECTED_FAILURE 5

/**
 * @brief 路径所属卷和物理存储目标信息。
 *
 * 多磁盘卷的 physicalDiskNumber 为 -1，storageTargetKey 使用 PhysicalSet:n,m，
 * 调度器据此避免把同一复合卷错误地分配到多个独立通道。
 */
typedef struct DiskTopologyInfo {
    int      statusCode;
    int      physicalDiskNumber;
    int      partitionNumber;
    int      mediaType;
    int      incursSeekPenalty;
    uint32_t win32Error;
    char     volumeGuid[128];
    char     storageTargetKey[256];
} DiskTopologyInfo;

/**
 * @brief 查询任意已挂载路径的卷 GUID、物理盘集合和介质类型。
 * @param path UTF-8 文件或目录路径，必须位于本机已挂载卷。
 * @param outInfo 必须非空，函数始终初始化该结构。
 * @return 成功返回 1，失败返回 0；详细原因写入 statusCode 和 win32Error。
 */
DISKINFO_API int __cdecl QueryDiskTopology(const char* path, DiskTopologyInfo* outInfo);

/** @brief 文件物理起始位置查询成功。 */
#define DISKINFO_FILE_LOCATION_OK               0
/** @brief 文件路径或输出参数无效。 */
#define DISKINFO_FILE_LOCATION_INVALID_ARGUMENT 1
/** @brief 无法打开文件或文件所在卷。 */
#define DISKINFO_FILE_LOCATION_OPEN_FAILED      2
/** @brief 文件系统不支持或无法返回文件簇位置。 */
#define DISKINFO_FILE_LOCATION_QUERY_FAILED     3
/** @brief 复合卷无法安全映射为单一物理偏移，调用方应退化为普通队列。 */
#define DISKINFO_FILE_LOCATION_COMPLEX_VOLUME   4
#define DISKINFO_FILE_LOCATION_UNEXPECTED_FAILURE 5

/**
 * @brief 文件第一段数据在物理磁盘上的位置。
 *
 * 该位置只用于机械盘电梯调度，不作为文件身份或持久化数据。稀疏、驻留、复合卷等无法
 * 安全定位的文件返回失败，由调度器按未定位文件处理。
 */
typedef struct FilePhysicalLocation {
    int      statusCode;
    int      physicalDiskNumber;
    uint32_t win32Error;
    uint64_t physicalStartByte;
} FilePhysicalLocation;

/**
 * @brief 查询普通文件第一段数据的物理磁盘字节位置。
 * @param path UTF-8 文件路径。
 * @param outLocation 必须非空，函数始终初始化该结构。
 * @return 成功返回 1；失败返回 0，调用方应使用普通队列继续扫描。
 */
DISKINFO_API int __cdecl QueryFilePhysicalLocation(const char* path, FilePhysicalLocation* outLocation);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // DISKINFO_H
