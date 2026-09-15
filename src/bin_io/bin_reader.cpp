/**
 * @file    bin_reader.cpp
 * @brief   BIN文件内存映射读取器实现
 * @details 跨平台mmap实现：
 *          - Windows: CreateFileMappingW + MapViewOfFile
 *          - Linux/macOS: mmap + madvise(MADV_SEQUENTIAL)
 *          采用RAII模式，构造时打开文件，析构时自动释放所有系统资源。
 */

#include "bin_io/bin_reader.h"

#include <algorithm>
#include <string>

#ifdef _WIN32
    // 仅依赖 windows.h（经 bin_reader.h 引入）提供的 MultiByteToWideChar
#else
    #include <sys/stat.h>
    #include <fcntl.h>
    #include <unistd.h>
#endif

namespace gnss {
namespace bin_io {

namespace {

/**
 * @brief 取得内存映射的粒度（起始偏移必须按此对齐）
 * @return Windows=dwAllocationGranularity(通常64KiB)，POSIX=页大小
 */
size_t mapping_granularity() noexcept {
#ifdef _WIN32
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const size_t gran = static_cast<size_t>(si.dwAllocationGranularity);
    return (gran == 0) ? (64 * 1024) : gran;
#else
    const long ps = sysconf(_SC_PAGESIZE);
    return (ps > 0) ? static_cast<size_t>(ps) : static_cast<size_t>(4096);
#endif
}

#ifdef _WIN32
/**
 * @brief 把窄字符串路径按系统 ANSI 代码页转换为宽字符串
 * @param path 输入路径（来自 main() 的 argv，Windows 上为 ANSI/CP_ACP 编码）
 * @return UTF-16 宽路径
 * @note  这里用 CP_ACP 而非 CP_UTF8：本工程 main() 使用窄字符 argv，
 *        MSVC 运行时按系统 ANSI 代码页填充，因此中文等非 ASCII 路径只有
 *        按 CP_ACP 转码才能正确打开。若将来改为 wmain()，或统一约定命令行
 *        为 UTF-8 并调用 SetConsoleCP/清单声明，此处需同步改为 CP_UTF8。
 *        旧实现 `std::wstring(path.begin(), path.end())` 只是逐字节加宽，
 *        非 ASCII 路径必然打不开。
 */
std::wstring to_wide_path(const std::string& path) {
    if (path.empty()) {
        return std::wstring();
    }

    const int len = static_cast<int>(path.size());
    const int needed = MultiByteToWideChar(CP_ACP, 0, path.c_str(), len, nullptr, 0);
    if (needed <= 0) {
        // 转码失败（如非法字节序列）：退化为逐字节加宽，至少保证 ASCII 可用
        return std::wstring(path.begin(), path.end());
    }

    std::wstring wide(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_ACP, 0, path.c_str(), len, wide.data(), needed);
    return wide;
}
#endif

} // namespace

// ============================================================
// MmapFile 实现
// ============================================================

MmapFile::MmapFile(const std::string& filepath, size_t chunk_size)
    : filepath_(filepath)
{
    // ------------------------------------------------------------
    // 归一化分块大小（必须在任何映射之前完成）
    //   - 0 会被 MapViewOfFile 解释为"映射到文件末尾"，导致
    //     current_chunk_size_ 恒为 0、next_chunk() 永远返回 true → 死循环；
    //   - Windows 要求视图起始偏移按 dwAllocationGranularity 对齐，
    //     非粒度整数倍的 chunk_size 会让第 2 块起 MapViewOfFile 失败。
    // ------------------------------------------------------------
    const size_t gran = mapping_granularity();
    if (chunk_size == 0) {
        chunk_size_ = gran;
    } else {
        chunk_size_ = ((chunk_size + gran - 1) / gran) * gran;
    }

#ifdef _WIN32
    // ============================================================
    // Windows实现：CreateFileMapping + MapViewOfFile
    // ============================================================

    // 打开文件（只读，共享读）；路径按系统 ANSI 代码页转宽字符
    file_handle_ = CreateFileW(
        to_wide_path(filepath).c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr
    );

    if (file_handle_ == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("无法打开BIN文件: " + filepath);
    }

    // 获取文件大小
    LARGE_INTEGER li_size;
    if (!GetFileSizeEx(file_handle_, &li_size)) {
        close_handles();
        throw std::runtime_error("无法获取文件大小: " + filepath);
    }
    file_size_ = static_cast<size_t>(li_size.QuadPart);

    // 空文件：不创建映射（允许此情况，解析时自然无帧）
    if (file_size_ == 0) {
        return;
    }

    // 创建文件映射对象（只读）
    mapping_handle_ = CreateFileMappingW(
        file_handle_,
        nullptr,
        PAGE_READONLY,
        0, 0,  // 映射整个文件
        nullptr
    );

    if (mapping_handle_ == nullptr) {
        close_handles();
        throw std::runtime_error("创建文件映射失败: " + filepath);
    }

#else
    // ============================================================
    // Linux/macOS实现：open + mmap
    // ============================================================

    fd_ = open(filepath.c_str(), O_RDONLY);
    if (fd_ < 0) {
        throw std::runtime_error("无法打开BIN文件: " + filepath);
    }

    // 获取文件大小
    struct stat st;
    if (fstat(fd_, &st) != 0) {
        close_handles();
        throw std::runtime_error("无法获取文件状态: " + filepath);
    }
    file_size_ = static_cast<size_t>(st.st_size);

    if (file_size_ == 0) {
        return;
    }
#endif

    // 映射第一个分块。
    // 注意：此刻对象尚未构造完成，若此处抛异常，析构函数不会被调用，
    // 因此必须显式释放已获得的句柄/映射后再重抛，否则文件句柄泄漏。
    try {
        if (!next_chunk()) {
            throw std::runtime_error("无法映射文件第一个分块: " + filepath);
        }
    } catch (...) {
        unmap_current();
        close_handles();
        throw;
    }
}

MmapFile::~MmapFile() noexcept {
    // 解除当前映射
    unmap_current();
    // 关闭文件/映射句柄
    close_handles();
}

void MmapFile::close_handles() noexcept {
#ifdef _WIN32
    if (mapping_handle_ != nullptr) {
        CloseHandle(mapping_handle_);
        mapping_handle_ = nullptr;
    }
    if (file_handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(file_handle_);
        file_handle_ = INVALID_HANDLE_VALUE;
    }
#else
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
#endif
}

bool MmapFile::next_chunk() {
    // 先解除当前映射
    unmap_current();

    // 已读完
    if (offset_ >= file_size_) {
        return false;
    }

    // 计算本次映射大小
    current_chunk_size_ = std::min(chunk_size_, file_size_ - offset_);

    // 防御：映射长度为 0 时 MapViewOfFile 会映射到文件末尾，
    // 使调用方陷入"永远还有下一块"的死循环
    if (current_chunk_size_ == 0) {
        return false;
    }

#ifdef _WIN32
    // Windows：MapViewOfFile
    DWORD offset_high = static_cast<DWORD>((offset_ >> 32) & 0xFFFFFFFFULL);
    DWORD offset_low  = static_cast<DWORD>(offset_ & 0xFFFFFFFFULL);

    mapped_addr_ = MapViewOfFile(
        mapping_handle_,
        FILE_MAP_READ,
        offset_high,
        offset_low,
        current_chunk_size_
    );

    if (mapped_addr_ == nullptr) {
        current_chunk_size_ = 0;
        throw std::runtime_error("MapViewOfFile失败，偏移=" + std::to_string(offset_));
    }
#else
    // Linux/macOS：mmap
    mapped_addr_ = mmap(
        nullptr,
        current_chunk_size_,
        PROT_READ,
        MAP_PRIVATE,
        fd_,
        static_cast<off_t>(offset_)
    );

    if (mapped_addr_ == MAP_FAILED) {
        mapped_addr_ = nullptr;
        current_chunk_size_ = 0;
        throw std::runtime_error("mmap失败，偏移=" + std::to_string(offset_));
    }

    // 提示内核采用顺序读取模式，提升预读效率
    madvise(mapped_addr_, current_chunk_size_, MADV_SEQUENTIAL);
#endif

    return true;
}

void MmapFile::unmap_current() noexcept {
    if (mapped_addr_ == nullptr) {
        current_chunk_size_ = 0;
        return;
    }

#ifdef _WIN32
    UnmapViewOfFile(mapped_addr_);
#else
    // 传入真实的映射长度：若先清零 current_chunk_size_ 再 munmap，
    // Linux 上 munmap(addr, 0) 会失败并泄漏整个分块的映射
    munmap(mapped_addr_, current_chunk_size_);
#endif

    mapped_addr_ = nullptr;
    offset_ += current_chunk_size_;
    current_chunk_size_ = 0;
}

} // namespace bin_io
} // namespace gnss
