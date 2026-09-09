#pragma once
/**
 * @file    bin_reader.h
 * @brief   BIN文件大文件mmap分块读取器
 * @details 使用内存映射(mmap)实现大文件高效分段读取，避免一次性加载全部文件。
 *          RAII管理模式，自动释放系统资源。
 *          跨平台：Windows使用CreateFileMapping/MapViewOfFile，
 *                   Linux/macOS使用mmap。
 */

#include <cstdint>
#include <cstddef>
#include <string>
#include <stdexcept>
#include <memory>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <sys/mman.h>
    #include <sys/stat.h>
    #include <fcntl.h>
    #include <unistd.h>
#endif

namespace gnss {
namespace bin_io {

/**
 * @brief 跨平台内存映射文件RAII封装
 * @note  每次映射一个分块（默认64MB），处理超大BIN文件时不会耗尽内存。
 *         析构时自动解除映射并关闭文件句柄。
 */
class MmapFile {
public:
    /**
     * @brief 构造函数：打开文件并获取文件大小
     * @param filepath BIN文件完整路径
     * @param chunk_size 每次映射的分块大小，默认64MB
     * @throws std::runtime_error 文件打开失败时抛出
     */
    explicit MmapFile(const std::string& filepath, size_t chunk_size = 64 * 1024 * 1024);

    /** @brief 析构：解除映射，关闭文件句柄 */
    ~MmapFile() noexcept;

    // 禁止拷贝和移动（管理系统资源）
    MmapFile(const MmapFile&) = delete;
    MmapFile& operator=(const MmapFile&) = delete;
    MmapFile(MmapFile&&) = delete;
    MmapFile& operator=(MmapFile&&) = delete;

    /**
     * @brief 映射文件的下一个分块到内存
     * @return true=映射成功, false=已到文件末尾
     * @throws std::runtime_error 映射失败时抛出
     */
    bool next_chunk();

    /**
     * @brief 获取当前映射块的起始指针
     * @return 指向当前映射块的只读指针，未映射时返回nullptr
     */
    const uint8_t* data() const noexcept { return static_cast<const uint8_t*>(mapped_addr_); }

    /**
     * @brief 获取当前映射块的有效字节数
     * @return 当前块大小（字节），最后一块可能小于chunk_size
     */
    size_t current_chunk_size() const noexcept { return current_chunk_size_; }

    /**
     * @brief 获取文件总大小
     * @return 文件总字节数
     */
    size_t file_size() const noexcept { return file_size_; }

    /**
     * @brief 检查文件是否已全部读完
     * @return true=已读完
     */
    bool eof() const noexcept { return offset_ >= file_size_; }

    /**
     * @brief 重置读取位置到文件开头
     */
    void reset() noexcept { offset_ = 0; current_chunk_size_ = 0; }

private:
    /** 解除当前映射 */
    void unmap_current() noexcept;

    std::string filepath_;       ///< 文件路径
    size_t chunk_size_;          ///< 分块大小（字节）
    size_t file_size_ = 0;       ///< 文件总大小
    size_t offset_ = 0;          ///< 当前读取偏移
    size_t current_chunk_size_ = 0; ///< 当前映射块大小

#ifdef _WIN32
    HANDLE file_handle_ = INVALID_HANDLE_VALUE;      ///< Windows文件句柄
    HANDLE mapping_handle_ = INVALID_HANDLE_VALUE;   ///< Windows映射对象
    void*  mapped_addr_ = nullptr;                   ///< 映射视图基址
#else
    int    fd_ = -1;              ///< Linux文件描述符
    void*  mapped_addr_ = nullptr; ///< mmap映射地址
#endif
};

/**
 * @brief BIN文件字节流顺序读取器
 * @details 在MmapFile提供的当前分块内顺序读取基础类型。
 *          自动处理大端→小端转换。
 *          当当前分块读完时自动推进到下一分块。
 */
class BinStreamReader {
public:
    /**
     * @brief 构造函数
     * @param mmap_file 已打开的内存映射文件（引用，不获取所有权）
     */
    explicit BinStreamReader(MmapFile& mmap_file) noexcept;

    /**
     * @brief 检查剩余可读字节数是否足够
     * @param need_bytes 需要的字节数
     * @return true=足够, false=不足
     */
    bool can_read(size_t need_bytes) const noexcept;

    /**
     * @brief 读取uint8_t（单字节，无需字节序转换）
     */
    uint8_t read_u8();

    /**
     * @brief 读取uint16_t（大端→主机字节序）
     */
    uint16_t read_u16();

    /**
     * @brief 读取uint32_t（大端→主机字节序）
     */
    uint32_t read_u32();

    /**
     * @brief 读取uint64_t（大端→主机字节序）
     */
    uint64_t read_u64();

    /**
     * @brief 读取float（大端→主机字节序）
     */
    float read_float();

    /**
     * @brief 读取double（大端→主机字节序）
     */
    double read_double();

    /**
     * @brief 从当前偏移读取指定数量的原始字节到目标缓冲区
     * @param dst 目标缓冲区
     * @param count 字节数
     * @throws std::runtime_error 数据不足时抛出
     */
    void read_bytes(void* dst, size_t count);

    /**
     * @brief 跳过指定字节数
     * @param count 跳过的字节数
     */
    void skip(size_t count);

    /**
     * @brief 获取当前在文件中的绝对偏移（用于调试）
     */
    size_t current_offset() const noexcept { return chunk_offset_ + pos_; }

    /**
     * @brief 检查是否已读到文件末尾
     */
    bool eof() const noexcept;

private:
    /**
     * @brief 确保当前分块内有need_bytes字节可读，否则推进到下一块
     * @param need_bytes 需要的字节数
     * @throws std::runtime_error 无法满足时抛出
     */
    void ensure_buffer(size_t need_bytes);

    MmapFile& mmap_file_;       ///< 引用的内存映射文件
    const uint8_t* chunk_ptr_;  ///< 当前分块起始指针
    size_t chunk_offset_;       ///< 当前分块的全局起始偏移
    size_t chunk_size_;         ///< 当前分块有效大小
    size_t pos_;                ///< 当前分块内读取位置
};

} // namespace bin_io
} // namespace gnss
