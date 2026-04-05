/**
 * @file    Symbol.h
 * @brief   动态库符号查找类 —— 通过 SO 路径加载模块并按名称检索导出符号地址
 * @author  Song
 * @date    2025/11/23
 * @update  2026/03/05
 *
 * @details 封装了动态库（.so）的符号解析功能，内部使用 std::map 缓存已查找的符号地址，
 *          避免重复查找，提高运行时性能。
 */

#ifndef DOBBY_PROJECT_SYMBOL_H
#define DOBBY_PROJECT_SYMBOL_H



#include <map>
#include <string>

namespace Symbol{

    /**
     * @class Symbol
     * @brief 动态库符号查找类，负责加载 SO 模块并缓存符号地址映射
     */
    class Symbol{

    public:
        /**
         * @brief   构造函数，根据 SO 文件路径初始化符号查找器
         * @param   sopath SO 动态库的文件路径（例如 "libil2cpp.so"）
         */
        Symbol(const std::string & sopath);

        /**
         * @brief   根据符号名称查找对应的内存地址
         * @param   symbolName 要查找的导出符号名称
         * @return  成功返回符号地址指针，失败返回 nullptr
         */
        void* find(const std::string& symbolName);
    public:
        /**
         * @brief   重新设置 SO 文件路径
         * @param   sopath 新的 SO 动态库文件路径
         * @return  成功返回 true，失败返回 false
         */
        bool setSoPath(const std::string & sopath);
    private:
        void* m_Mmodule;                                            ///< 加载的动态库模块句柄
        std::shared_ptr<std::map<std::string,void*>> m_addressMap;  ///< 符号名称 -> 地址 的缓存映射表
        std::string m_sopath = nullptr;                             ///< 当前 SO 文件路径
    };
};

#endif //DOBBY_PROJECT_SYMBOL_H