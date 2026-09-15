#pragma once
/**
 * @file    bin_reader.h
 * @brief   BIN文件大文件mmap分块读取器
 * @details 使用内存映射(mmap)实现大文件高效分段读取，避免一次性加载全部文件。
 *          RAII管理模式，自动释放系统资源。
 *          跨平台：Windows使用CreateFileMapping/MapViewOfFile，
 *                   Linux/macOS使用mmap。
 *
 *          帧级解析由 parser::BinParser 直接遍历分块缓冲区完成（含跨块拼接），
 *          本文件只负责"映射/推进/释放分块"这一件事。
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
     * @brief 构造函数：打开文件并映射第一个分块
     * @param filepath BIN文件完整路径（Windows下按系统 ANSI 代码页解释）
     * @param chunk_size 每次映射的分块大小，默认64MB。
     *        构造时会归一化：0 → 一个映射粒度；非粒度整数倍 → 向上取整到
     *        粒度整数倍（Windows 要求视图偏移按 dwAllocationGranularity(64KiB)
     *        对齐，POSIX 按页大小对齐）。
     * @throws std::runtime_error 文件打开/映射失败时抛出
     * @note  构造过程中抛异常时，已获得的句柄/映射会在重抛前显式释放，
     *        不会因为析构函数不被调用而泄漏。
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
     * @brief 检查当前分块是否已覆盖到文件末尾
     * @return true=当前映射块即为最后一块（本块读完后无更多数据）
     * @note  语义为"最后一块已映射"，而非"读取位置已越过末尾"：
     *        offset_ 表示当前块的起始偏移，映射到末尾时
     *        offset_ + current_chunk_size_ == file_size_。
     */
    bool eof() const noexcept { return offset_ + current_chunk_size_ >= file_size_; }

    /**
     * @brief 重置读取位置到文件开头
     * @note  会先解除当前映射再归零偏移：否则 Linux 上残留的映射会因
     *        current_chunk_size_ 被清零而以 munmap(addr, 0) 失败，导致每次
     *        reset() 泄漏一个分块的映射。
     */
    void reset() noexcept { unmap_current(); offset_ = 0; current_chunk_size_ = 0; }

private:
    /** 解除当前映射（若已映射） */
    void unmap_current() noexcept;

    /** 关闭文件/映射句柄（幂等，供析构与构造失败清理共用） */
    void close_handles() noexcept;

    std::string filepath_;       ///< 文件路径
    size_t chunk_size_;          ///< 分块大小（字节，已归一化为映射粒度整数倍）
    size_t file_size_ = 0;       ///< 文件总大小
    size_t offset_ = 0;          ///< 当前分块在文件中的起始偏移
    size_t current_chunk_size_ = 0; ///< 当前映射块大小（字节，0=未映射）

#ifdef _WIN32
    HANDLE file_handle_ = INVALID_HANDLE_VALUE;      ///< Windows文件句柄
    HANDLE mapping_handle_ = nullptr;                ///< Windows映射对象（失败时为nullptr）
    void*  mapped_addr_ = nullptr;                   ///< 映射视图基址
#else
    int    fd_ = -1;              ///< Linux文件描述符
    void*  mapped_addr_ = nullptr; ///< mmap映射地址
#endif
};

} // namespace bin_io
} // namespace gnss
