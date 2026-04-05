//
// Created by Song on 2025/11/29.
//

#include "file.h"
#include <system_error>
#include <type_traits>
#include <vector>
#include <fstream>
#include <iostream>
#include <filesystem>

cMyfile::cMyfile(const std::string filePath):m_filePath(filePath){
    m_stream = nullptr;
}

bool cMyfile::isOpen()const{
return m_stream && m_stream.get()->is_open();
}

bool cMyfile::openFile(cMyfile::FileMode mode) {
    do {
        if(mode == FileMode::noMode){
            mode = FileMode::ReadWrite;
        }

        m_stream = std::make_shared<std::ofstream>(m_filePath, ((int)mode-1));
        if(!m_stream->is_open()){
            break;
        }
        return true;
    }while(0);
    return false;
}

bool cMyfile::closeFile() {
    if (m_stream && m_stream->is_open()) {
        m_stream->close();
        return true;
    }
    return false;
}

cMyfile::~cMyfile() {
    closeFile();
}

bool cMyfile::write(const std::string &data) {
    if (!isOpen()){
        return false;
    }
    m_stream->write(data.c_str(), data.size());
    return m_stream->good();
}

bool cMyfile::flush() {
    if (isOpen()) {
        m_stream->flush();
        return m_stream->good();
    }
    return false;
}

size_t cMyfile::getFileSize(){
    if (!isOpen()){
        return 0;
    }

    std::streampos current_pos = m_stream->tellp();
    m_stream->seekp(0, std::ios::end);
    std::streampos size = m_stream->tellp();
    m_stream->seekp(current_pos);

    return static_cast<size_t>(size);
}

bool cMyfile::writeLine(const std::string& line) {
    return write(line + '\n');
}

#ifdef OBFU_ATTRS_END
OBFU_ATTRS_END
#endif