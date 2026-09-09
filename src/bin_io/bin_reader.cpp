/**
 * @file    bin_reader.cpp
 * @brief   BIN文件内存映射读取器实现
 * @details 跨平台mmap实现：
 *          - Windows: CreateFileMappingW + MapViewOfFile
 *          - Linux/macOS: mmap + madvise(MADV_SEQUENTIAL)
 *          采用RAII模式，构造时打开文件，析构时自动释放所有系统资源。
 */

#include "bin_io/bin_reader.h"
#include "bin_io/endian_utils.h"

#include <cstring>
#include <sstream>

#ifdef _WIN32
    #include <io.h>
#else
    #include <sys/stat.h>
    #include <fcntl.h>
    #include <unistd.h>
#endif

namespace gnss {
namespace bin_io {

// ============================================================
// MmapFile 实现
// ============================================================

MmapFile::MmapFile(const std::string& filepath, size_t chunk_size)
    : filepath_(filepath)
    , chunk_size_(chunk_size)
{
#ifdef _WIN32
    // ============================================================
    // Windows实现：CreateFileMapping + MapViewOfFile
    // ============================================================

    // 打开文件（只读，共享读）
    file_handle_ = CreateFileW(
        std::wstring(filepath.begin(), filepath.end()).c_str(),
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
        CloseHandle(file_handle_);
        file_handle_ = INVALID_HANDLE_VALUE;
        throw std::runtime_error("无法获取文件大小: " + filepath);
    }
    file_size_ = static_cast<size_t>(li_size.QuadPart);

    // 空文件：创建空映射（允许此情况，解析时自然无帧）
    if (file_size_ == 0) {
        mapping_handle_ = INVALID_HANDLE_VALUE;
        mapped_addr_ = nullptr;
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
        CloseHandle(file_handle_);
        file_handle_ = INVALID_HANDLE_VALUE;
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
        close(fd_);
        fd_ = -1;
        throw std::runtime_error("无法获取文件状态: " + filepath);
    }
    file_size_ = static_cast<size_t>(st.st_size);

    if (file_size_ == 0) {
        mapped_addr_ = nullptr;
        return;
    }
#endif

    // 映射第一个分块
    if (!next_chunk()) {
        throw std::runtime_error("无法映射文件第一个分块: " + filepath);
    }
}

MmapFile::~MmapFile() noexcept {
    // 解除当前映射
    unmap_current();

#ifdef _WIN32
    if (mapping_handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(mapping_handle_);
        mapping_handle_ = INVALID_HANDLE_VALUE;
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
        throw std::runtime_error("mmap失败，偏移=" + std::to_string(offset_));
    }

    // 提示内核采用顺序读取模式，提升预读效率
    madvise(mapped_addr_, current_chunk_size_, MADV_SEQUENTIAL);
#endif

    return true;
}

void MmapFile::unmap_current() noexcept {
    if (mapped_addr_ == nullptr) return;

#ifdef _WIN32
    UnmapViewOfFile(mapped_addr_);
#else
    munmap(mapped_addr_, current_chunk_size_);
#endif

    mapped_addr_ = nullptr;
    offset_ += current_chunk_size_;
    current_chunk_size_ = 0;
}

// ============================================================
// BinStreamReader 实现
// ============================================================

BinStreamReader::BinStreamReader(MmapFile& mmap_file) noexcept
    : mmap_file_(mmap_file)
    , chunk_ptr_(mmap_file.data())
    , chunk_offset_(0)
    , chunk_size_(mmap_file.current_chunk_size())
    , pos_(0)
{}

bool BinStreamReader::can_read(size_t need_bytes) const noexcept {
    // 当前chunk不够用，但后面还有数据
    if (pos_ + need_bytes > chunk_size_) {
        return chunk_offset_ + chunk_size_ < mmap_file_.file_size();
    }
    return pos_ + need_bytes <= chunk_size_;
}

void BinStreamReader::ensure_buffer(size_t need_bytes) {
    if (pos_ + need_bytes <= chunk_size_) {
        return; // 当前块足够
    }

    // 当前块不够：推进到下一个分块
    if (!mmap_file_.next_chunk()) {
        throw std::runtime_error(
            "读取超出文件末尾: 需要" + std::to_string(need_bytes) + "字节");
    }

    chunk_ptr_   = mmap_file_.data();
    chunk_offset_ = mmap_file_.file_size() - mmap_file_.current_chunk_size()
                    - (mmap_file_.eof() ? 0 : 0);
    chunk_size_  = mmap_file_.current_chunk_size();
    pos_         = 0;

    // 推进分块后再次检查
    if (pos_ + need_bytes > chunk_size_) {
        throw std::runtime_error(
            "文件末尾数据不足: 需要" + std::to_string(need_bytes)
            + "字节, 实际剩余" + std::to_string(chunk_size_ - pos_) + "字节");
    }
}

uint8_t BinStreamReader::read_u8() {
    ensure_buffer(1);
    return chunk_ptr_[pos_++];
}

uint16_t BinStreamReader::read_u16() {
    ensure_buffer(2);
    uint16_t raw = 0;
    std::memcpy(&raw, chunk_ptr_ + pos_, sizeof(raw));
    pos_ += 2;
    return little_to_host_u16(raw);
}

uint32_t BinStreamReader::read_u32() {
    ensure_buffer(4);
    uint32_t raw = 0;
    std::memcpy(&raw, chunk_ptr_ + pos_, sizeof(raw));
    pos_ += 4;
    return little_to_host_u32(raw);
}

uint64_t BinStreamReader::read_u64() {
    ensure_buffer(8);
    uint64_t raw = 0;
    std::memcpy(&raw, chunk_ptr_ + pos_, sizeof(raw));
    pos_ += 8;
    return little_to_host_u64(raw);
}

float BinStreamReader::read_float() {
    ensure_buffer(4);
    float raw = 0.0f;
    std::memcpy(&raw, chunk_ptr_ + pos_, sizeof(raw));
    pos_ += 4;
    return little_to_host_float(raw);
}

double BinStreamReader::read_double() {
    ensure_buffer(8);
    double raw = 0.0;
    std::memcpy(&raw, chunk_ptr_ + pos_, sizeof(raw));
    pos_ += 8;
    return little_to_host_double(raw);
}

void BinStreamReader::read_bytes(void* dst, size_t count) {
    if (count == 0) return;
    ensure_buffer(count);
    std::memcpy(dst, chunk_ptr_ + pos_, count);
    pos_ += count;
}

void BinStreamReader::skip(size_t count) {
    // 跨chunk跳过
    while (count > 0) {
        size_t available = chunk_size_ - pos_;
        if (count <= available) {
            pos_ += count;
            return;
        }
        count -= available;
        ensure_buffer(1); // 推进到下一块
    }
}

bool BinStreamReader::eof() const noexcept {
    return mmap_file_.eof() && pos_ >= chunk_size_;
}

} // namespace bin_io
} // namespace gnss
