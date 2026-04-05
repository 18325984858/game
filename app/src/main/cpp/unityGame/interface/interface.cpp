//
// Created by Song on 2026/1/10.
//
#include "interface.h"
#include "../../Log/log.h"
#include <sstream>
#include <vector>
#include <iomanip>
#include <unordered_map>
#include <android/log.h>

fun::function::function(void* dqil2cppBase,
                        void *pCodeRegistration,
                        void *pMetadataRegistration,
                        void *pGlobalMetadataHeader,
                        void* pMetadataImagesTable) :
        li2cppApi::cUnityApi(dqil2cppBase,
                             static_cast<Il2CppCodeRegistration *>(pCodeRegistration),
                             static_cast<Il2CppMetadataRegistration *>(pMetadataRegistration),
                             static_cast<Il2CppGlobalMetadataHeader *>(pGlobalMetadataHeader),
                             static_cast<Il2CppImageGlobalMetadata *>(pMetadataImagesTable)){
    initPackPath("com.tencent.lolm");

    // 初始化文件输出流
    auto initFile = [&](std::shared_ptr<cMyfile>& file, const std::string& path) {
        if (file == nullptr) {
            file = std::make_shared<cMyfile>(path);
            if (file) file->openFile(cMyfile::FileMode::ReadWrite);
        }
    };
    initFile(m_outlog, m_pathlog);

    if(!m_pClassInfo){
        m_pClassInfo = std::make_shared<std::list<std::shared_ptr<CClassInfo>>>();
    }
}


/**
 * 日志记录函数
 * 负责将调试信息写入到 /data/data/com.DefaultCompany.Demo1/files/log.cs
 */
bool fun::function::writeLog(std::string str) {
    if (m_outlog == nullptr) {
        return false;
    }
    //LOG(LOG_LEVEL_INFO,"%s",str.c_str());
    // 调用你文件类 cMyfile 的接口写入一行
    //m_outlog->writeLine(str.c_str());

    // 实时冲刷缓冲区，防止日志丢失
    //m_outlog->flush();

    return true;
}

void fun::function::initPackPath(std::string strPackName) {
    m_pathlog = "/data/data/"+strPackName+"/files/testlog.cs";
}

uint64_t fun::function::GetStaticMember(std::string pMainModuleName, std::string pModuleName,
                                     std::string pClassName, std::string ptemplateName,
                                     std::string pStaticName, uint32_t* pOutOffset) {
    LOG(LOG_LEVEL_INFO,"[%s] 调用, 模块: %s, 类: %s, 字段: %s", __FUNCTION__, pModuleName.c_str(), pClassName.c_str(), pStaticName.c_str());
    if(m_pClassInfo && !m_pClassInfo->empty()){
        for(auto&ite:*m_pClassInfo){
            //判断模块名是否一致，如果一致说明在此链表中
            if(*ite->m_ModuleData->m_pName == pModuleName){
                LOG(LOG_LEVEL_INFO,"[%s] 匹配模块: %s", __FUNCTION__, pModuleName.c_str());
                //指向当前模块存储的所有数据结构开头,遍历当前所有结构
                for (auto&pClassStruct:*ite->m_pClassStruct) {
                    //判断类的名字，并判断泛型是否一致
                    if(*pClassStruct.m_pClassData->m_pName == pClassName &&
                    (ptemplateName == "" ? 1 : *pClassStruct.m_pClassData->m_pGenericsName == ptemplateName)){
                        LOG(LOG_LEVEL_INFO,"[%s] 匹配类: %s, 泛型: %s", __FUNCTION__, pClassName.c_str(), ptemplateName.c_str());
                        //如果找到类就遍历类下面的静态成员名，
                        for (auto& pStaticStruct : *pClassStruct.m_pVectorStaticData) {
                            // ================= 开始检索静态字段 =================
                            if (*pStaticStruct->m_Data.m_pName == pStaticName) {
                                LOG(LOG_LEVEL_INFO, "[%s][Step 1] 匹配到目标字段名称: %s", __FUNCTION__, pStaticName.c_str());

                                // 1. 获取 FieldInfo 指针 (保留位运算逻辑)
                                uint64_t rawFieldPtr = (uint64_t)pStaticStruct->fieldInfo;
                                FieldInfo *fieldInfo = (FieldInfo*)(rawFieldPtr & 0x000000FFFFFFFFFF);

                                if (fieldInfo == nullptr) {
                                    LOG(LOG_LEVEL_ERROR, "[%s][Error] fieldInfo 地址解析后为空！原始值: 0x%llX", __FUNCTION__, rawFieldPtr);
                                    return -1;
                                }
                                LOG(LOG_LEVEL_INFO, "[%s][Step 2] 字段信息已定位: fieldInfoPtr=%p, offset=0x%X", __FUNCTION__, fieldInfo, (uint32_t)fieldInfo->offset);

                                // 将字段偏移通过传出参数返回给调用方
                                if (pOutOffset != nullptr) {
                                    *pOutOffset = (uint32_t)fieldInfo->offset;
                                }

                                // 2. 第一次尝试：直接获取静态值（类可能已经初始化过了）
                                uint64_t valBuffer = 0;
                                try {
                                    this->il2cpp_field_static_get_value(fieldInfo, &valBuffer);
                                } catch (...) {
                                    LOG(LOG_LEVEL_WARN, "[%s][Exception] 首次获取静态值发生异常，准备进行类初始化...", __FUNCTION__);
                                    valBuffer = 0;
                                }

                                if (valBuffer != 0) {
                                    LOG(LOG_LEVEL_INFO, "[%s][Fast] 首次获取成功! [%s] -> Value: 0x%lX", __FUNCTION__, pStaticName.c_str(), (unsigned long)valBuffer);
                                    return valBuffer;
                                }
                                LOG(LOG_LEVEL_WARN, "[%s][Step 3] 首次获取值为 0，尝试初始化类后重试...", __FUNCTION__);

                                // 3. 获取该字段所属的类，执行类初始化
                                Il2CppClass* parentClass = il2cpp_field_get_parent(fieldInfo);
                                if (parentClass == nullptr) {
                                    LOG(LOG_LEVEL_ERROR, "[%s][Error] 无法获取字段 %s 所属的 ParentClass", __FUNCTION__, pStaticName.c_str());
                                    return -1;
                                }
                                const char* className = parentClass->name;
                                LOG(LOG_LEVEL_INFO, "[%s][Step 4] 字段所属类: %s, static_fields=%p", __FUNCTION__,
                                    className ? className : "Unknown", parentClass->static_fields);

                                // 4. 初始化类
                                if (parentClass->static_fields == nullptr) {
                                    LOG(LOG_LEVEL_WARN, "[%s] 类 %s 尚未初始化，调用 il2cpp_runtime_class_init...", __FUNCTION__, className);
                                    il2cpp_runtime_class_init(parentClass);
                                    if (parentClass->static_fields == nullptr) {
                                        LOG(LOG_LEVEL_ERROR, "[%s][Fatal] 类 %s 初始化失败，static_fields 仍然为空", __FUNCTION__, className);
                                        return -1;
                                    }
                                    LOG(LOG_LEVEL_INFO, "[%s] 类 %s 初始化成功! static_fields=%p", __FUNCTION__, className, parentClass->static_fields);
                                }

                                // 5. 第二次尝试：类初始化后再次获取静态值
                                valBuffer = 0;
                                this->il2cpp_field_static_get_value(fieldInfo, &valBuffer);

                                LOG(LOG_LEVEL_INFO, "[%s][Final] 重试结果: [%s] -> Value: 0x%lX (%s)",
                                    __FUNCTION__, pStaticName.c_str(), (unsigned long)valBuffer,
                                    valBuffer != 0 ? "成功" : "仍然为0");
                                return valBuffer;
                            }
                        }
                    }
                }
            }
        }
    }
    LOG(LOG_LEVEL_WARN, "[%s] 未找到目标: 模块=%s, 类=%s, 字段=%s", __FUNCTION__, pModuleName.c_str(), pClassName.c_str(), pStaticName.c_str());
    return -1;
}

bool fun::function::GetMember(std::string pMainModuleName,std::string pModuleName,
                              std::string pClassName,std::string pTemplateName,
                              std::string pFieldName,uint32_t* pOutOffset,
                              FieldInfo** pOutFieldInfo,bool* pOutIsStatic) {
    LOG(LOG_LEVEL_INFO, "[GetMember] 查找成员: %s.%s.%s", pModuleName.c_str(), pClassName.c_str(), pFieldName.c_str());

    if (pOutOffset == nullptr) {
        LOG(LOG_LEVEL_ERROR, "[GetMember] pOutOffset 不能为空");
        return false;
    }

    if (!m_pClassInfo || m_pClassInfo->empty()) {
        LOG(LOG_LEVEL_ERROR, "[GetMember] 类信息未初始化");
        return false;
    }

    for (auto& ite : *m_pClassInfo) {
        if (!ite || !ite->m_ModuleData || !ite->m_ModuleData->m_pName) continue;

        // 匹配模块名
        if (*ite->m_ModuleData->m_pName != pModuleName) continue;

        for (auto& pClassStruct : *ite->m_pClassStruct) {
            if (!pClassStruct.m_pClassData || !pClassStruct.m_pClassData->m_pName) continue;

            // 匹配类名
            if (*pClassStruct.m_pClassData->m_pName != pClassName) continue;

            // 匹配泛型名（为空则不限定）
            if (!pTemplateName.empty()) {
                if (!pClassStruct.m_pClassData->m_pGenericsName ||
                    *pClassStruct.m_pClassData->m_pGenericsName != pTemplateName) {
                    continue;
                }
            }

            LOG(LOG_LEVEL_INFO, "[GetMember] 找到类: %s", pClassName.c_str());

            // 遍历该��的所有字段（包括静态和实例）
            for (auto& pFieldStruct : *pClassStruct.m_pVectorStaticData) {
                if (!pFieldStruct || !pFieldStruct->m_Data.m_pName) continue;
                if (*pFieldStruct->m_Data.m_pName != pFieldName) continue;

                // 匹配成功 —— 获取 FieldInfo
                uint64_t rawFieldPtr = (uint64_t)pFieldStruct->fieldInfo;
                FieldInfo* fieldInfo = (FieldInfo*)(rawFieldPtr & 0x000000FFFFFFFFFF);
                if (fieldInfo == nullptr) {
                    LOG(LOG_LEVEL_ERROR, "[GetMember] fieldInfo 为空, 字段: %s", pFieldName.c_str());
                    return false;
                }

                // 获取偏移
                uint32_t offset = fieldInfo->offset;
                *pOutOffset = offset;

                // 判断是否静态字段
                uint32_t attrs = il2cpp_field_get_flags(fieldInfo);
                bool isStatic = (attrs & FIELD_ATTRIBUTE_STATIC) != 0;

                if (pOutFieldInfo != nullptr) {
                    *pOutFieldInfo = fieldInfo;
                }
                if (pOutIsStatic != nullptr) {
                    *pOutIsStatic = isStatic;
                }

                LOG(LOG_LEVEL_INFO, "[GetMember] 成功! 字段: %s, 偏移: 0x%X, 静态: %s",
                    pFieldName.c_str(), offset, isStatic ? "是" : "否");
                return true;
            }

            // 类匹配但字段未找到
            LOG(LOG_LEVEL_WARN, "[GetMember] 类 %s 中未找到字段: %s", pClassName.c_str(), pFieldName.c_str());
            return false;
        }
    }

    LOG(LOG_LEVEL_WARN, "[GetMember] 未找到类: %s (模块: %s)", pClassName.c_str(), pModuleName.c_str());
    return false;
}

uint64_t fun::function::GetMethodFun(std::string pMainModuleName, std::string pModuleName,
                                 std::string pClassName, std::string ptemplateName,
                                 std::string pMethodName,std::string pMethodtemplateName,
                                 int32_t pParamCount){
        LOG(LOG_LEVEL_INFO,"[Test Game] GetMethodFun call pMainModuleName : %s pModuleName : %s pClassName : %s ptemplateName : %s pMethodName : %s pMethodtemplateName : %s pParamCount : %d",
            pMainModuleName.c_str(), pModuleName.c_str(), pClassName.c_str(), ptemplateName.c_str(), pMethodName.c_str(), pMethodtemplateName.c_str(), pParamCount);
        if(m_pClassInfo && !m_pClassInfo->empty()) {
            for(auto&ite:*m_pClassInfo) {
                if (*ite->m_ModuleData->m_pName == pModuleName) {
                    LOG(LOG_LEVEL_INFO, "[Test Game] pModuleName : %s", pModuleName.c_str());
                    for (auto &pClassStruct: *ite->m_pClassStruct) {
                        const bool classNameMatched = pClassStruct.m_pClassData &&
                                                      pClassStruct.m_pClassData->m_pName &&
                                                      *pClassStruct.m_pClassData->m_pName == pClassName;
                        const bool templateMatched = ptemplateName.empty() ||
                                                     (pClassStruct.m_pClassData &&
                                                      pClassStruct.m_pClassData->m_pGenericsName &&
                                                      *pClassStruct.m_pClassData->m_pGenericsName == ptemplateName);
                        if (!classNameMatched || !templateMatched) {
                            continue;
                        }

                        LOG(LOG_LEVEL_INFO, "[Test Game] pClassName : %s ptemplateName : %s",pClassName.c_str(), ptemplateName.c_str());
                        if (pParamCount < 0) {
                            for (auto&pMethodData:*pClassStruct.m_pVectorFunctionData) {
                                if (!pMethodData || !pMethodData->m_pName || *pMethodData->m_pName != pMethodName) {
                                    continue;
                                }

                                const bool methodTemplateMatched = pMethodtemplateName.empty() ||
                                                                  (pMethodData->m_pGenericsName &&
                                                                   *pMethodData->m_pGenericsName == pMethodtemplateName);
                                if (!methodTemplateMatched) {
                                    continue;
                                }

                                if (pMethodData->m_offset != 0 && pMethodData->m_offset != static_cast<uint64_t>(-1)) {
                                    return pMethodData->m_offset;
                                }

                                LOG(LOG_LEVEL_WARN,
                                    "[GetMethodFun] cached method pointer is empty, fallback to runtime scan. class=%s method=%s",
                                    pClassName.c_str(),
                                    pMethodName.c_str());
                                break;
                            }
                        }

                        const uint64_t runtimeMethod = GetMethodFunByClass(pClassStruct.m_pKlass, pMethodName, pParamCount);
                        if (runtimeMethod != 0) {
                            return runtimeMethod;
                        }
                    }
                }
            }
        }
    LOG(LOG_LEVEL_WARN, "[GetMethodFun] not found module=%s class=%s template=%s method=%s methodTemplate=%s pParamCount=%d",
        pModuleName.c_str(), pClassName.c_str(), ptemplateName.c_str(), pMethodName.c_str(), pMethodtemplateName.c_str(), pParamCount);
    return 0;
}

Il2CppClass* fun::function::GetMethodOwnerClass(std::string pMainModuleName, std::string pModuleName,
                                                std::string pClassName, std::string ptemplateName,
                                                std::string pMethodName, std::string pMethodtemplateName,
                                                int32_t pParamCount) {
    LOG(LOG_LEVEL_INFO,
        "[GetMethodOwnerClass] call pMainModuleName : %s pModuleName : %s pClassName : %s ptemplateName : %s pMethodName : %s pMethodtemplateName : %s pParamCount : %d",
        pMainModuleName.c_str(), pModuleName.c_str(), pClassName.c_str(), ptemplateName.c_str(),
        pMethodName.c_str(), pMethodtemplateName.c_str(), pParamCount);

    if (m_pClassInfo && !m_pClassInfo->empty()) {
        for (auto& ite : *m_pClassInfo) {
            if (*ite->m_ModuleData->m_pName != pModuleName) {
                continue;
            }

            for (auto& pClassStruct : *ite->m_pClassStruct) {
                const bool classNameMatched = pClassStruct.m_pClassData &&
                                              pClassStruct.m_pClassData->m_pName &&
                                              *pClassStruct.m_pClassData->m_pName == pClassName;
                const bool templateMatched = ptemplateName.empty() ||
                                             (pClassStruct.m_pClassData &&
                                              pClassStruct.m_pClassData->m_pGenericsName &&
                                              *pClassStruct.m_pClassData->m_pGenericsName == ptemplateName);
                if (!classNameMatched || !templateMatched) {
                    continue;
                }

                if (pMethodName.empty()) {
                    return pClassStruct.m_pKlass;
                }

                if (pParamCount < 0) {
                    for (auto& pMethodData : *pClassStruct.m_pVectorFunctionData) {
                        if (!pMethodData || !pMethodData->m_pName || *pMethodData->m_pName != pMethodName) {
                            continue;
                        }

                        const bool methodTemplateMatched = pMethodtemplateName.empty() ||
                                                          (pMethodData->m_pGenericsName &&
                                                           *pMethodData->m_pGenericsName == pMethodtemplateName);
                        if (!methodTemplateMatched) {
                            continue;
                        }

                        return pClassStruct.m_pKlass;
                    }
                }

                if (GetMethodFunByClass(pClassStruct.m_pKlass, pMethodName, pParamCount) != 0) {
                    return pClassStruct.m_pKlass;
                }
            }
        }
    }

    LOG(LOG_LEVEL_WARN,
        "[GetMethodOwnerClass] not found module=%s class=%s template=%s method=%s methodTemplate=%s pParamCount=%d",
        pModuleName.c_str(), pClassName.c_str(), ptemplateName.c_str(), pMethodName.c_str(),
        pMethodtemplateName.c_str(), pParamCount);
    return nullptr;
}

uint64_t fun::function::GetMethodFunByClass(Il2CppClass* pKlass, std::string pMethodName, int32_t pParamCount) {
    if (pKlass == nullptr || pMethodName.empty()) {
        return 0;
    }

    const char* className = il2cpp_class_get_name(pKlass);
    const char* namespaceName = il2cpp_class_get_namespace(pKlass);
    const Il2CppType* classType = il2cpp_class_get_type(pKlass);
    const char* typeName = classType ? il2cpp_type_get_name(classType) : nullptr;

    auto scanMethodPointer = [&](const char* stage) -> uint64_t {
        void* iter = nullptr;
        while (auto method = il2cpp_class_get_methods(pKlass, &iter)) {
            const char* methodName = il2cpp_method_get_name(method);
            if (methodName == nullptr || pMethodName != methodName) {
                continue;
            }

            const uint32_t paramCount = il2cpp_method_get_param_count(method);
            if (pParamCount >= 0 && paramCount != static_cast<uint32_t>(pParamCount)) {
                continue;
            }

            const uint64_t methodAddr = reinterpret_cast<uint64_t>(method->methodPointer);
            LOG(LOG_LEVEL_INFO,
                "[GetMethodFunByClass][%s] class=%s namespace=%s type=%s method=%s params=%u addr=%p",
                stage,
                className ? className : "<null>",
                namespaceName ? namespaceName : "",
                typeName ? typeName : "<null>",
                methodName,
                paramCount,
                reinterpret_cast<void*>(methodAddr));
            if (methodAddr != 0) {
                return methodAddr;
            }
        }
        return 0;
    };

    uint64_t methodAddr = scanMethodPointer("first-scan");
    if (methodAddr != 0) {
        return methodAddr;
    }

    il2cpp_runtime_class_init(pKlass);
    il2cpp_class_init_all_method(pKlass);

    methodAddr = scanMethodPointer("after-init");
    if (methodAddr == 0) {
        LOG(LOG_LEVEL_WARN,
            "[GetMethodFunByClass] failed class=%s namespace=%s type=%s method=%s params=%d",
            className ? className : "<null>",
            namespaceName ? namespaceName : "",
            typeName ? typeName : "<null>",
            pMethodName.c_str(),
            pParamCount);
    }
    return methodAddr;
}

std::shared_ptr<fun::CClassInfo>
fun::function::AddModuleInfo(std::shared_ptr<fun::CClassInfo> pData,
                             std::string pModuleName, uint64_t offset) {
    if(pModuleName.empty() && pData==nullptr){
        return nullptr;
    }

    auto p = std::make_shared<CData>();
    if(p){
        p->m_pName = std::make_shared<std::string>(pModuleName);
        p->m_offset = offset;

        //将模块信息添加到指定的CClassInfo结构中
        pData->m_ModuleData = p;
    }
    return pData;
}

std::shared_ptr<fun::CClassData>
fun::function::AddClassStructInfo(std::shared_ptr<fun::CClassData> pData,std::string pClassName,
                                  std::string pGenericsName,uint64_t offset) {
    if(pData == nullptr && pClassName.empty()){
        return nullptr;
    }

    auto p = std::make_shared<fun::CTemplateData>();
    if(p){
        p->m_pName = std::make_shared<std::string>(pClassName);
        if(!pGenericsName.empty()){
            p->m_pGenericsName = std::make_shared<std::string>(pGenericsName);
        }
        p->m_offset = offset;

        pData->m_pClassData = p;
    }
    return pData;
}

std::shared_ptr<std::vector<std::shared_ptr<fun::CFieldData>>>
fun::function::AddVectorStaticData(std::shared_ptr<std::vector<std::shared_ptr<fun::CFieldData>>> pData,
                                   std::string pStaticName,FieldInfo *fieldInfo, uint64_t offset) {
    if (pData == nullptr && pStaticName.empty()) {
        return nullptr;
    }

    auto p = std::make_shared<fun::CFieldData>();
    if (p) {
        p->m_Data.m_pName = std::make_shared<std::string>(pStaticName);
        p->m_Data.m_offset = offset;
        p->fieldInfo = fieldInfo;
        pData->push_back(p);
    }
    return pData;
}

std::shared_ptr<std::vector<std::shared_ptr<fun::CTemplateData>>>
fun::function::AddVectorFunctionData(std::shared_ptr<std::vector<std::shared_ptr<fun::CTemplateData>>> pData,
                                     std::string pClassName, std::string pGenericsName,uint64_t offset) {
    if (pData == nullptr && pClassName.empty()) {
        return nullptr;
    }
    auto p = std::make_shared<fun::CTemplateData>();
    if (p) {
        p->m_pName = std::make_shared<std::string>(pClassName);
        if (!pGenericsName.empty()) {
            p->m_pGenericsName = std::make_shared<std::string>(pGenericsName);
        }
        p->m_offset = offset;
        pData->push_back(p);
    }
    return pData;
}

std::shared_ptr<std::list<std::shared_ptr<fun::CClassInfo>>>
fun::function::AddClassStruct(std::shared_ptr<std::list<std::shared_ptr<fun::CClassInfo>>> pData,
                              std::shared_ptr<fun::CData> pModuleData,std::shared_ptr<std::vector<fun::CClassData>> pClassStruct) {
    if (pData == nullptr) {
        return nullptr;
    }

    auto p = std::make_shared<fun::CClassInfo>();
    if (p) {
        p->m_ModuleData = pModuleData;
        p->m_pClassStruct = pClassStruct;

        pData->push_back(p);
    }
    return pData;
}

Il2CppClass* fun::function::FindClassByName(std::string pMainModuleName,std::string pModuleName, std::string pClassName , std::string pTemplateName ) {
    LOG(LOG_LEVEL_INFO, "[Test Game] FindClassByName call, className: %s, moduleName: %s, templateName: %s",
        pClassName.c_str(), pModuleName.c_str(), pTemplateName.c_str());

    if (pClassName.empty()) {
        LOG(LOG_LEVEL_ERROR, "[Test Game] FindClassByName: 类名参数为空!");
        return nullptr;
    }

    if (m_pClassInfo && !m_pClassInfo->empty()) {
        // 遍历所有模块
        for (auto& ite : *m_pClassInfo) {
            if (!ite || !ite->m_pClassStruct) continue;

            // 如果指定了模块名，则先过滤模块
            if (!pModuleName.empty()) {
                if (!ite->m_ModuleData || !ite->m_ModuleData->m_pName ||
                    *ite->m_ModuleData->m_pName != pModuleName) {
                    continue;
                }
            }

            // 遍历当前模块下的所有类
            for (auto& pClassStruct : *ite->m_pClassStruct) {
                if (!pClassStruct.m_pClassData || !pClassStruct.m_pClassData->m_pName) continue;

                // 匹配类名
                if (*pClassStruct.m_pClassData->m_pName != pClassName) continue;

                // 如果指定了泛型名，则进一步匹配泛型
                if (!pTemplateName.empty()) {
                    if (!pClassStruct.m_pClassData->m_pGenericsName ||
                        *pClassStruct.m_pClassData->m_pGenericsName != pTemplateName) {
                        continue;
                    }
                }

                // 匹配成功，返回 Il2CppClass* 类对象地址
                LOG(LOG_LEVEL_INFO, "[Test Game] FindClassByName: 匹配成功 [%s], 模块: %s, Il2CppClass*: %p",
                    pClassName.c_str(),
                    ite->m_ModuleData && ite->m_ModuleData->m_pName ? ite->m_ModuleData->m_pName->c_str() : "Unknown",
                    pClassStruct.m_pKlass);
                return pClassStruct.m_pKlass;
            }
        }
    }

    LOG(LOG_LEVEL_WARN, "[Test Game] FindClassByName: 未找到类 [%s]", pClassName.c_str());
    return nullptr;
}

fun::function::~function() {
    DeepClearClassInfo();       //释放 m_pClassInfo 成员

    if (m_outlog) { m_outlog->flush(); m_outlog->closeFile(); }
}

void fun::function::DeepClearClassInfo() {
    if (m_pClassInfo == nullptr) return;

    // 1. 遍历 list 中的每一个 CClassInfo
    for (auto it = m_pClassInfo->begin(); it != m_pClassInfo->end(); ++it) {
        auto pClassInfo = *it;
        if (!pClassInfo) continue;

        // --- 释放 CClassInfo 内部成员 ---

        // 释放模块信息
        if (pClassInfo->m_ModuleData) {
            pClassInfo->m_ModuleData.reset();
        }

        // 遍历并释放类结构 vector (m_pClassStruct)
        if (pClassInfo->m_pClassStruct) {
            for (auto& classData : *(pClassInfo->m_pClassStruct)) {

                // 释放 CClassData 内部的嵌套容器
                if (classData.m_pClassData) {
                    classData.m_pClassData.reset();
                }

                // 显式清空静态数据 vector
                if (classData.m_pVectorStaticData) {
                    classData.m_pVectorStaticData->clear();
                    classData.m_pVectorStaticData.reset();
                }

                // 显式清空函数数据 vector
                if (classData.m_pVectorFunctionData) {
                    classData.m_pVectorFunctionData->clear();
                    classData.m_pVectorFunctionData.reset();
                }
            }
            pClassInfo->m_pClassStruct->clear();
            pClassInfo->m_pClassStruct.reset();
        }
    }

    // 2. 最后清空主链表并重置顶层指针
    m_pClassInfo->clear();
    m_pClassInfo.reset();
    m_pClassInfo = nullptr;

    // 3. 清空泛型方法列表
    if (m_pGenericMethodList) {
        m_pGenericMethodList->clear();
        m_pGenericMethodList.reset();
        m_pGenericMethodList = nullptr;
    }
}

void fun::function::fillingClassInfo() {
    // 初始化顶层全局链表
    if (m_pClassInfo == nullptr) {
        m_pClassInfo = std::make_shared<std::list<std::shared_ptr<CClassInfo>>>();
    }
    char szBuf[0x1000] = {0};
    std::string Buf = "";
    auto domain = il2cpp_domain_get();
    size_t size;
    auto assemblies = il2cpp_domain_get_assemblies(domain, &size);
    LOG(LOG_LEVEL_INFO, "======== Start Filling Class Info (Assemblies: %zu) ========", size);

    for (size_t i = 0; i < size; ++i) {

        auto image = il2cpp_assembly_get_image(assemblies[i]);
        const char* imageName = il2cpp_image_get_name(image);


        Buf = "[Module] Processing ";
        Buf.append(imageName);
        writeLog(Buf);
        //LOG(LOG_LEVEL_INFO, "[Test MyStartPoint] %s", Buf.c_str());

        // 1. 准备模块数据
        auto pModuleData = std::make_shared<CData>();
        pModuleData->m_pName = std::make_shared<std::string>(imageName);
        pModuleData->m_offset = (uint64_t)image;

        auto pClassStructVec = std::make_shared<std::vector<CClassData>>();

        auto classCount = il2cpp_image_get_class_count(image);
        for (int j = 0; j < classCount; ++j) {
            auto klass = il2cpp_image_get_class(image, j);
            if (!klass) continue;

            // 2. 准备类的数据结构
            auto pClassDataObj = std::make_shared<CClassData>();
            pClassDataObj->m_pVectorStaticData = std::make_shared<std::vector<std::shared_ptr<CFieldData>>>();
            pClassDataObj->m_pVectorFunctionData = std::make_shared<std::vector<std::shared_ptr<CTemplateData>>>();

            // 3. 获取类名
            const char* ns = il2cpp_class_get_namespace(const_cast<Il2CppClass *>(klass));

            const char* name = il2cpp_class_get_name(const_cast<Il2CppClass *>(klass));

            std::string className = (ns && *ns) ? (std::string(ns) + "." + name) : name;
            Buf = "[className] name ";
            Buf.append(name);
            writeLog(Buf);
            //LOG(LOG_LEVEL_INFO, "[Test MyStartPoint]%s", Buf.c_str());

            std::string genericsName = "";
            auto type = il2cpp_class_get_type(const_cast<Il2CppClass *>(klass));
            const char* pTypeName = il2cpp_type_get_name(type); // 注意：某些版本返回 char*
            if (pTypeName) {
                genericsName = pTypeName;
                Buf = "[generics] name:";
                Buf.append(pTypeName);
                //LOG(LOG_LEVEL_INFO, "[Test MyStartPoint]%s", Buf.c_str());
                writeLog(Buf);
            }

            // 填充类基础信息
            AddClassStructInfo(pClassDataObj, name, genericsName, (uint64_t)klass);

            // 存储当前类的 Il2CppClass 对象指针
            pClassDataObj->m_pKlass = const_cast<Il2CppClass *>(klass);

            // 5. 遍历字段 (Fields)
            void* iter_field = nullptr;
            while (auto field = il2cpp_class_get_fields(const_cast<Il2CppClass *>(klass), &iter_field)) {
                const char* fieldName = il2cpp_field_get_name(field);
                uint32_t offset = il2cpp_field_get_offset(field);

                // 1. 获取字段属性标记
                uint32_t attrs = il2cpp_field_get_flags(field);

                Buf = "[Static Field] Name:";
                Buf.append(fieldName);
                writeLog(Buf);
                //LOG(LOG_LEVEL_INFO, "[Test MyStartPoint]%s", Buf.c_str());
                // 获取字段类型
                const Il2CppType* type = il2cpp_field_get_type(field);
                uint32_t typeType = type->type; // 或 il2cpp_type_get_type(type)

                // 记录数据
                AddVectorStaticData(pClassDataObj->m_pVectorStaticData, fieldName, field, (uint64_t)offset);
            }

            // 6. 初始化所有方法指针（确保 methodPointer 可用）
            il2cpp_class_init_all_method(const_cast<Il2CppClass *>(klass));

            // 7. 遍历方法 (Methods)
            void* iter_method = nullptr;
            while (auto method = il2cpp_class_get_methods(const_cast<Il2CppClass *>(klass), &iter_method)) {
                const char* methodName = il2cpp_method_get_name(method);
                // 获取方法的虚地址（在内存中的地址）
                uint64_t methodAddr = (uint64_t)method->methodPointer;
                sprintf(szBuf,"%p",methodAddr);
                //LOG(LOG_LEVEL_INFO, "    [Method] Name: %s, Addr: 0x%llX", methodName, methodAddr);
                Buf = "[Method] Name: ";
                Buf.append(methodName).append(" ").append(szBuf);
                writeLog(Buf);
                //LOG(LOG_LEVEL_INFO, "[Test MyStartPoint]%s", Buf.c_str());

                AddVectorFunctionData(pClassDataObj->m_pVectorFunctionData, methodName, "", methodAddr);
            }
            // 7. 存入模块向量
            pClassStructVec->push_back(*pClassDataObj);
        }

        // 8. 存入全局链表
        AddClassStruct(m_pClassInfo, pModuleData, pClassStructVec);
    }
    LOG(LOG_LEVEL_INFO, "======== Class Info Filling Completed ========");

    // 9. 遍历 genericMethodTable，填充泛型方法列表
    fillingGenericMethodInfo();
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// 辅助函数：获取 Il2CppType 的简短类型名
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
std::string fun::function::getTypeName(const Il2CppType* type) {
    if (!type) return "unknown";

    switch (type->type) {
        case IL2CPP_TYPE_VOID:    return "void";
        case IL2CPP_TYPE_BOOLEAN: return "bool";
        case IL2CPP_TYPE_CHAR:    return "char";
        case IL2CPP_TYPE_I1:      return "sbyte";
        case IL2CPP_TYPE_U1:      return "byte";
        case IL2CPP_TYPE_I2:      return "short";
        case IL2CPP_TYPE_U2:      return "ushort";
        case IL2CPP_TYPE_I4:      return "int";
        case IL2CPP_TYPE_U4:      return "uint";
        case IL2CPP_TYPE_I8:      return "long";
        case IL2CPP_TYPE_U8:      return "ulong";
        case IL2CPP_TYPE_R4:      return "float";
        case IL2CPP_TYPE_R8:      return "double";
        case IL2CPP_TYPE_STRING:  return "string";
        case IL2CPP_TYPE_OBJECT:  return "object";
        default: break;
    }

    // 尝试通过 il2cpp_type_get_name 获取
    const char* name = il2cpp_type_get_name(type);
    if (name && name[0] != '\0') return name;

    // 尝试通过 il2cpp_class_from_type 获取类名
    Il2CppClass* klass = il2cpp_class_from_type(type);
    if (klass) {
        const char* className = il2cpp_class_get_name(klass);
        if (className) return className;
    }

    return "unknown";
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// 辅助函数：获取泛型实例参数字符串 <T1, T2, ...>
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
std::string fun::function::getGenericInstName(GenericInstIndex index) {
    if (index == kTypeIndexInvalid ||
        index >= (GenericInstIndex)m_pil2CppMetadataRegistration->genericInstsCount) {
        return "";
    }

    const Il2CppGenericInst* inst = m_pil2CppMetadataRegistration->genericInsts[index];
    if (!inst || inst->type_argc == 0 || !inst->type_argv) return "";

    std::string ret = "<";
    for (uint32_t i = 0; i < inst->type_argc; ++i) {
        if (!inst->type_argv[i]) continue;
        if (i > 0) ret += ", ";
        ret += getTypeName(inst->type_argv[i]);
    }
    ret += ">";
    return ret;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// fillingGenericMethodInfo —— 遍历 genericMethodTable，构建泛型方法查询表
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void fun::function::fillingGenericMethodInfo() {
    if (m_pil2CppMetadataRegistration == nullptr || m_pIl2CppCodeRegistration == nullptr) {
        LOG(LOG_LEVEL_WARN, "[fillingGenericMethodInfo] MetadataRegistration 或 CodeRegistration 为空，跳过");
        return;
    }

    // 初始化列表
    if (m_pGenericMethodList == nullptr) {
        m_pGenericMethodList = std::make_shared<std::vector<std::shared_ptr<CGenericMethodData>>>();
    }
    m_pGenericMethodList->clear();

    int32_t tableCount = m_pil2CppMetadataRegistration->genericMethodTableCount;
    LOG(LOG_LEVEL_INFO, "[fillingGenericMethodInfo] genericMethodTableCount: %d", tableCount);

    int32_t validCount = 0;

    for (int32_t i = 0; i < tableCount; i++) {
        const Il2CppGenericMethodFunctionsDefinitions* genericMethodIndices =
                m_pil2CppMetadataRegistration->genericMethodTable + i;
        if (!genericMethodIndices) continue;

        // 校验 genericMethodIndex 范围
        if (genericMethodIndices->genericMethodIndex >= m_pil2CppMetadataRegistration->methodSpecsCount) continue;

        const Il2CppMethodSpec* methodSpec =
                m_pil2CppMetadataRegistration->methodSpecs + genericMethodIndices->genericMethodIndex;
        if (!methodSpec) continue;

        // 校验 methodIndex 范围
        if (genericMethodIndices->indices.methodIndex >= (MethodIndex)m_pIl2CppCodeRegistration->genericMethodPointersCount) continue;

        // 获取方法定义
        const Il2CppMethodDefinition* methodDef = GetMethodDefinitionFromIndex(methodSpec->methodDefinitionIndex);
        if (!methodDef) continue;

        // 获取方法指针（绝对地址）
        const Il2CppMethodPointer pMethodPointer =
                m_pIl2CppCodeRegistration->genericMethodPointers[genericMethodIndices->indices.methodIndex];
        if (!pMethodPointer) continue; // 跳过���指针

        uint64_t absAddr = (uint64_t)pMethodPointer;

        // 获取方法名
        const char* methodName = GetStringFromIndex(methodDef->nameIndex);
        if (!methodName || methodName[0] == '\0') continue;

        // 获取声明类型定义
        const Il2CppTypeDefinition* typeDef = GetTypeDefinitionForIndex(methodDef->declaringType);
        if (!typeDef) continue;

        // 获���类名（带反引号的原始名，如 DataShellList`4）
        std::string rawClassName = GetStringFromIndex(typeDef->nameIndex);

        // 去掉反引号部分得到纯类名（如 DataShellList）
        std::string cleanClassName = rawClassName;
        size_t backtickPos = cleanClassName.find_last_of('`');
        if (backtickPos != std::string::npos) {
            cleanClassName = cleanClassName.substr(0, backtickPos);
        }

        // 构建 Spec 完整名：类名<泛型参数>.方法名<方法泛型参数>
        std::string classGenerics = getGenericInstName(methodSpec->classIndexIndex);
        std::string methodGenerics = getGenericInstName(methodSpec->methodIndexIndex);
        std::string specName = cleanClassName + classGenerics + "." + methodName + methodGenerics;

        // 存入列表
        auto p = std::make_shared<CGenericMethodData>();
        p->m_pMethodName = methodName;
        p->m_pSpecName = specName;
        p->m_pClassName = cleanClassName;
        p->m_offset = absAddr;
        m_pGenericMethodList->push_back(p);
        validCount++;
    }

    LOG(LOG_LEVEL_INFO, "[fillingGenericMethodInfo] 完成，共存储 %d 条泛型方法", validCount);
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// GetGenericMethodFun —— 从泛型方法表中查找指定方法的地址
// 参数一：类名（去掉反引号），例如 "DataShellList"
// 参数二：方法名，例如 "get_Count"
// 参数三：泛型参数（可选），例如 "CherryTeam, IsDemData, IsSyncData, NotSharedPtr"
//        为空则只匹配类名+方法名，不限定泛型参数
// 返回值：成功返回方法绝对地址，失败返回 0
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
uint64_t fun::function::GetGenericMethodFun(std::string pClassName, std::string pMethodName, std::string pGenericArgs) {
    LOG(LOG_LEVEL_INFO, "[GetGenericMethodFun] 查找: className=%s, methodName=%s, genericArgs=%s",
        pClassName.c_str(), pMethodName.c_str(), pGenericArgs.c_str());

    if (!m_pGenericMethodList || m_pGenericMethodList->empty()) {
        LOG(LOG_LEVEL_WARN, "[GetGenericMethodFun] 泛型方法列表为空，请先调用 fillingClassInfo");
        return 0;
    }

    for (auto& pGenericMethod : *m_pGenericMethodList) {
        if (!pGenericMethod) continue;

        // 1. 匹配类名
        if (pGenericMethod->m_pClassName != pClassName) continue;

        // 2. 匹配方法名
        if (pGenericMethod->m_pMethodName != pMethodName) continue;

        // 3. 如果指定了泛型参数，则进一步匹配 specName 中是否包含该参数
        if (!pGenericArgs.empty()) {
            if (pGenericMethod->m_pSpecName.find(pGenericArgs) == std::string::npos) {
                continue;
            }
        }

        // 匹配成功
        LOG(LOG_LEVEL_INFO, "[GetGenericMethodFun] 匹配成功! Spec=%s, Addr=%p",
            pGenericMethod->m_pSpecName.c_str(), (void*)pGenericMethod->m_offset);
        return pGenericMethod->m_offset;
    }

    LOG(LOG_LEVEL_WARN, "[GetGenericMethodFun] 未找到: className=%s, methodName=%s, genericArgs=%s",
        pClassName.c_str(), pMethodName.c_str(), pGenericArgs.c_str());
    return 0;
}

#ifdef OBFU_ATTRS_END
OBFU_ATTRS_END
#endif