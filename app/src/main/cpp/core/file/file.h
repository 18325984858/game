/**
 * @file    file.h
 * @brief   文件操作封装类 —— 提供统一的文件读写接口
 * @author  Song
 * @date    2025/11/29
 * @update  2026/03/05
 *
 * @details 封装了 std::ofstream，支持多种文件打开模式（只读、只写、追加、读写、二进制），
 *          并通过 shared_ptr 管理流对象生命周期，禁止拷贝以保证资源安全。
 */

#ifndef DOBBY_PROJECT_FILE_H
#define DOBBY_PROJECT_FILE_H

#include <fstream>
#include <string>
#include <memory>

/**
 * @class cMyfile
 * @brief 文件操作封装类，管理单个文件的打开、关闭、读写操作
 */
class cMyfile{
public:
    /**
     * @enum FileMode
     * @brief 文件打开模式枚举
     */
    enum class FileMode {
        noMode,     // 未指定模式
        Read,       // 只读模式
        Write,      // 只写模式
        Append,     // 追加模式
        ReadWrite,  // 读写模式
        Binary      // 二进制模式
    };

    /**
     * @enum WriteMode
     * @brief 文件写入模式枚举
     */
    enum class WriteMode {
        noMode,     // 未指定模式
        Overwrite,  // 覆盖模式
        Append,     // 追加模式
        Truncate    // 截断模式
    };
private:
    // 禁止拷贝构造和拷贝赋值，防止文件句柄被多处持有
    cMyfile(const cMyfile&) = delete;
    cMyfile& operator=(const cMyfile&) = delete;
public:
    /**
     * @brief 构造函数
     * @param filePath 文件路径，默认为 nullptr
     */
    cMyfile(const std::string filePath=nullptr);

    /** @brief 析构函数，自动关闭文件并释放资源 */
    ~cMyfile();

public:
    /**
     * @brief   以指定模式打开文件
     * @param   mode 文件打开模式
     * @return  成功返回 true，失败返回 false
     */
    bool openFile(cMyfile::FileMode mode);

    /**
     * @brief   关闭当前已打开的文件
     * @return  成功返回 true，失败返回 false
     */
    bool closeFile();

    /**
     * @brief   判断文件是否处于打开状态
     * @return  已打开返回 true，否则返回 false
     */
    bool isOpen()const;

    /**
     * @brief   获取文件大小（字节数）
     * @return  文件大小
     */
    size_t getFileSize();

    /**
     * @brief   刷新文件缓冲区，将缓冲数据写入磁盘
     * @return  成功返回 true，失败返回 false
     */
    bool flush();

    /**
     * @brief   向文件写入字符串数据
     * @param   data 要写入的字符串
     * @return  成功返回 true，失败返回 false
     */
    bool write(const std::string& data);

    /**
     * @brief   向文件写入一行字符串（自动追加换行符）
     * @param   line 要写入的行内容
     * @return  成功返回 true，失败返回 false
     */
    bool writeLine(const std::string& line);
private:
    std::shared_ptr<std::ofstream> m_stream;    ///< 文件输出流（智能指针管理）
    std::string m_filePath;                     ///< 文件路径
};


#endif //DOBBY_PROJECT_FILE_H
