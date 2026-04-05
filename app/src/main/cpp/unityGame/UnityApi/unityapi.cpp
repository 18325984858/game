//
// Created by Song on 2025/12/7.
//

#include "unityapi.h"
#include "../../Log/log.h"

li2cppApi::cUnityApi::~cUnityApi() {
    m_li2cppso = nullptr;
    m_il2cppbase = 0;
}


li2cppApi::cUnityApi::cUnityApi(void*dqil2cppBase,
                                Il2CppCodeRegistration *pCodeRegistration,
                                Il2CppMetadataRegistration *pMetadataRegistration,
                                Il2CppGlobalMetadataHeader *pGlobalMetadataHeader,
                                Il2CppImageGlobalMetadata*pMetadataImagesTable):
        m_il2cppbase((uint64_t)dqil2cppBase),
        m_pGlobalMetadata(pGlobalMetadataHeader),
        m_pIl2CppCodeRegistration(pCodeRegistration),
        m_pil2CppMetadataRegistration(pMetadataRegistration),
        m_pGlobalMetadataHeader(pGlobalMetadataHeader),
        m_pMetadataImagesTable(pMetadataImagesTable)
{

    LOG(LOG_LEVEL_INFO,"pCodeRegistration :%p pMetadataRegistration:%p pGlobalMetadataHeader:%p",pCodeRegistration,pMetadataRegistration,pGlobalMetadataHeader);

    if(m_li2cppso == nullptr){
        m_li2cppso = std::make_shared<Symbol::Symbol>("libil2cpp.so");
    }


    // 此处删除
    // 因为lol类已被作为主类
    /*
    if(m_lolgame == nullptr) {
        m_lolgame = std::make_shared<lol::lol>(
                (const Il2CppGlobalMetadataHeader *) pGlobalMetadataHeader);
    }
    */

    LOG(LOG_LEVEL_INFO,"[DumpStr] cUnityApi Init Success! ");
}

uint64_t li2cppApi::cUnityApi::Getil2cppModuleBase() {
    return m_il2cppbase;
}

Il2CppClass *li2cppApi::cUnityApi::il2cpp_class_from_type(const Il2CppType *type) {
    //il2cpp_class_from_type函数指针
    typedef Il2CppClass* (*pil2cpp_class_from_type)(const Il2CppType* type);
    static pil2cpp_class_from_type m_ppil2cpp_class_from_type = nullptr;

    if(m_ppil2cpp_class_from_type == nullptr){
        m_ppil2cpp_class_from_type = (pil2cpp_class_from_type) m_li2cppso->find(
                "il2cpp_class_from_type");

        if (m_ppil2cpp_class_from_type) {

        }
    }
    return m_ppil2cpp_class_from_type(type);
}

const char *li2cppApi::cUnityApi::il2cpp_class_get_name(Il2CppClass *klass) {
    //il2cpp_class_get_name函数指针
    typedef const char* (*pil2cpp_class_get_name)(Il2CppClass *klass);
    static pil2cpp_class_get_name m_ppil2cpp_class_get_name = nullptr;

    if(m_ppil2cpp_class_get_name == nullptr){
        m_ppil2cpp_class_get_name = (pil2cpp_class_get_name) m_li2cppso->find(
                "il2cpp_class_get_name");

        if (m_ppil2cpp_class_get_name) {

        }
    }
    return m_ppil2cpp_class_get_name(klass);
}

const Il2CppAssembly **li2cppApi::cUnityApi::il2cpp_domain_get_assemblies(const Il2CppDomain *domain, size_t *size) {
    //il2cpp_domain_get_assemblies函数指针
    typedef const Il2CppAssembly** (*pil2cpp_domain_get_assemblies)(const Il2CppDomain* domain, size_t* size);
    static pil2cpp_domain_get_assemblies m_ppil2cpp_domain_get_assemblies = nullptr;

    if(m_ppil2cpp_domain_get_assemblies == nullptr){
        m_ppil2cpp_domain_get_assemblies = (pil2cpp_domain_get_assemblies) m_li2cppso->find(
                "il2cpp_domain_get_assemblies");

        if (m_ppil2cpp_domain_get_assemblies) {

        }
    }
    return m_ppil2cpp_domain_get_assemblies(domain,size);
}

Il2CppDomain *li2cppApi::cUnityApi::il2cpp_domain_get() {
    //il2cpp_domain_get函数指针
    typedef Il2CppDomain* (*pil2cpp_domain_get)();
    static pil2cpp_domain_get m_ppil2cpp_domain_get = nullptr;

    if(m_ppil2cpp_domain_get == nullptr){
        m_ppil2cpp_domain_get = (pil2cpp_domain_get) m_li2cppso->find(
                "il2cpp_domain_get");
        if (m_ppil2cpp_domain_get) {

        }
    }
    return m_ppil2cpp_domain_get();
}

int32_t li2cppApi::cUnityApi::il2cpp_class_instance_size(Il2CppClass *klass)
{
    //il2cpp_assembly_get_image函数指针
    typedef int32_t (*pil2cpp_class_instance_size)(Il2CppClass *klass);
    static pil2cpp_class_instance_size m_ppil2cpp_class_instance_size = nullptr;

    if(m_ppil2cpp_class_instance_size == nullptr){
        m_ppil2cpp_class_instance_size = (pil2cpp_class_instance_size) m_li2cppso->find(
                "il2cpp_class_instance_size");
        if (m_ppil2cpp_class_instance_size) {

        }
    }
    return m_ppil2cpp_class_instance_size(klass);
}

const Il2CppImage *li2cppApi::cUnityApi::il2cpp_assembly_get_image(const Il2CppAssembly *assembly) {
    //il2cpp_assembly_get_image函数指针
    typedef const Il2CppImage* (*pil2cpp_assembly_get_image)(const Il2CppAssembly *assembly);
    static pil2cpp_assembly_get_image m_ppil2cpp_assembly_get_image = nullptr;

    if(m_ppil2cpp_assembly_get_image == nullptr){
        m_ppil2cpp_assembly_get_image = (pil2cpp_assembly_get_image) m_li2cppso->find(
                "il2cpp_assembly_get_image");
        if (m_ppil2cpp_assembly_get_image) {

        }
    }
    return m_ppil2cpp_assembly_get_image(assembly);
}

const char *li2cppApi::cUnityApi::il2cpp_image_get_name(const Il2CppImage *image) {
    //il2cpp_image_get_name函数指针
    typedef const char* (*pil2cpp_image_get_name)(const Il2CppImage *image);
    static pil2cpp_image_get_name m_ppil2cpp_image_get_name = nullptr;

    if(m_ppil2cpp_image_get_name == nullptr){
        m_ppil2cpp_image_get_name = (pil2cpp_image_get_name) m_li2cppso->find(
                "il2cpp_image_get_name");
        if (m_ppil2cpp_image_get_name) {

        }
    }
    return m_ppil2cpp_image_get_name(image);
}

const Il2CppClass *li2cppApi::cUnityApi::il2cpp_image_get_class(const Il2CppImage *image, size_t index) {
    //il2cpp_image_get_class函数指针
    typedef const Il2CppClass* (*pil2cpp_image_get_class)(const Il2CppImage * image, size_t index);
    static pil2cpp_image_get_class m_ppil2cpp_image_get_class = nullptr;

    if(m_ppil2cpp_image_get_class == nullptr){
        m_ppil2cpp_image_get_class = (pil2cpp_image_get_class) m_li2cppso->find(
                "il2cpp_image_get_class");
        if (m_ppil2cpp_image_get_class) {

        }
    }
    return m_ppil2cpp_image_get_class(image,index);
}

const Il2CppType *li2cppApi::cUnityApi::il2cpp_class_get_type(Il2CppClass *klass) {
    //il2cpp_class_get_type函数指针
    typedef const Il2CppType* (*pil2cpp_class_get_type)(Il2CppClass *klass);
    static pil2cpp_class_get_type m_ppil2cpp_class_get_type = nullptr;

    if(m_ppil2cpp_class_get_type == nullptr){
        m_ppil2cpp_class_get_type = (pil2cpp_class_get_type) m_li2cppso->find(
                "il2cpp_class_get_type");
        if (m_ppil2cpp_class_get_type) {

        }
    }
    return m_ppil2cpp_class_get_type(klass);
}

Il2CppClass* li2cppApi::cUnityApi::il2cpp_type_get_class_or_element_class(const Il2CppType *type) {
    //il2cpp_type_get_class_or_element_class函数指针
    typedef Il2CppClass* (*pil2cpp_type_get_class_or_element_class)(const Il2CppType *type);
    static pil2cpp_type_get_class_or_element_class m_ppil2cpp_type_get_class_or_element_class = nullptr;

    if(m_ppil2cpp_type_get_class_or_element_class == nullptr){
        m_ppil2cpp_type_get_class_or_element_class = (pil2cpp_type_get_class_or_element_class) m_li2cppso->find(
                "il2cpp_type_get_class_or_element_class");
        if (m_ppil2cpp_type_get_class_or_element_class) {

        }
    }
    return m_ppil2cpp_type_get_class_or_element_class(type);
}

size_t li2cppApi::cUnityApi::il2cpp_image_get_class_count(const Il2CppImage *image) {
    //il2cpp_image_get_class_count函数指针
    typedef size_t (*pil2cpp_image_get_class_count)(const Il2CppImage * image);
    static pil2cpp_image_get_class_count m_ppil2cpp_image_get_class_count = nullptr;

    if(m_ppil2cpp_image_get_class_count == nullptr){
        m_ppil2cpp_image_get_class_count = (pil2cpp_image_get_class_count) m_li2cppso->find(
                "il2cpp_image_get_class_count");
        if (m_ppil2cpp_image_get_class_count) {

        }
    }
    return m_ppil2cpp_image_get_class_count(image);
}

bool li2cppApi::cUnityApi::il2cpp_class_is_inited(const Il2CppClass *klass) {
    //il2cpp_class_is_inited函数指针
    typedef bool (*pil2cpp_class_is_inited)(const Il2CppClass *klass);
    static pil2cpp_class_is_inited m_ppil2cpp_class_is_inited = nullptr;

    if(m_ppil2cpp_class_is_inited == nullptr){
        m_ppil2cpp_class_is_inited = (pil2cpp_class_is_inited) m_li2cppso->find(
                "il2cpp_class_is_inited");
        if (m_ppil2cpp_class_is_inited) {

        }
    }
    return m_ppil2cpp_class_is_inited(klass);
}

const Il2CppType *li2cppApi::cUnityApi::GetIl2CppTypeFromIndex(TypeIndex index) {
    if (index == kTypeIndexInvalid)
        return NULL;

    IL2CPP_ASSERT(index < m_pil2CppMetadataRegistration->typesCount && "Invalid type index ");

    return m_pil2CppMetadataRegistration->types[index];
}

void *li2cppApi::cUnityApi::il2cpp_api_lookup_symbol(const char *name) {
    if (!name || !*name || m_li2cppso == nullptr) {
        return nullptr;
    }

    // 优先直接解析目标符号，避免在游戏线程里再次走 il2cpp_api_lookup_symbol 导致卡顿。
    if (void* directSymbol = m_li2cppso->find(name)) {
        return directSymbol;
    }

    typedef void* (*pil2cpp_api_lookup_symbol)(const char* name);
    static pil2cpp_api_lookup_symbol m_ppil2cpp_api_lookup_symbol = nullptr;
    static bool s_lookupResolved = false;

    if (!s_lookupResolved) {
        s_lookupResolved = true;
        m_ppil2cpp_api_lookup_symbol = (pil2cpp_api_lookup_symbol)m_li2cppso->find(
                "il2cpp_api_lookup_symbol");
    }

    return m_ppil2cpp_api_lookup_symbol ? m_ppil2cpp_api_lookup_symbol(name) : nullptr;
}

bool li2cppApi::cUnityApi::il2cpp_is_debugger_attached() {
    //il2cpp_is_debugger_attached函数指针
    typedef bool (*pil2cpp_is_debugger_attached)();
    static pil2cpp_is_debugger_attached m_ppil2cpp_is_debugger_attached = nullptr;
    if(m_ppil2cpp_is_debugger_attached == nullptr){
        m_ppil2cpp_is_debugger_attached = (pil2cpp_is_debugger_attached) m_li2cppso->find(
                "il2cpp_is_debugger_attached");
        if (m_ppil2cpp_is_debugger_attached) {

        }
    }
    return m_ppil2cpp_is_debugger_attached();
}

const char *li2cppApi::cUnityApi::il2cpp_class_get_namespace(Il2CppClass *klass) {
    //il2cpp_class_get_namespace 函数指针
    typedef const char* (*pil2cpp_class_get_namespace)(Il2CppClass *klass);
    static pil2cpp_class_get_namespace m_ppil2cpp_class_get_namespace = nullptr;
    if(m_ppil2cpp_class_get_namespace == nullptr){
        m_ppil2cpp_class_get_namespace = (pil2cpp_class_get_namespace)m_li2cppso->find(
                "il2cpp_class_get_namespace");
        if (m_ppil2cpp_class_get_namespace) {

        }
    }
    return m_ppil2cpp_class_get_namespace(klass);
}

Il2CppClass *li2cppApi::cUnityApi::il2cpp_class_get_parent(Il2CppClass *klass) {
    //il2cpp_class_get_parent 函数指针
    typedef Il2CppClass* (*pil2cpp_class_get_parent)(Il2CppClass *klass);
    static pil2cpp_class_get_parent m_ppil2cpp_class_get_parent = nullptr;
    if(m_ppil2cpp_class_get_parent == nullptr){
        m_ppil2cpp_class_get_parent = (pil2cpp_class_get_parent)m_li2cppso->find(
                "il2cpp_class_get_parent");
        if (m_ppil2cpp_class_get_parent) {

        }
    }
    return m_ppil2cpp_class_get_parent(klass);
}

Il2CppClass *li2cppApi::cUnityApi::il2cpp_class_get_declaring_type(Il2CppClass *klass) {
    //il2cpp_class_get_declaring_type 函数指针
    typedef  Il2CppClass* (*pil2cpp_class_get_declaring_type)(Il2CppClass* klas);
    static pil2cpp_class_get_declaring_type m_ppil2cpp_class_get_declaring_type = nullptr;
    if(m_ppil2cpp_class_get_declaring_type == nullptr){
        m_ppil2cpp_class_get_declaring_type = (pil2cpp_class_get_declaring_type)m_li2cppso->find(
                "il2cpp_class_get_declaring_type");
        if (m_ppil2cpp_class_get_declaring_type) {

        }
    }
    return m_ppil2cpp_class_get_declaring_type(klass);
}

bool li2cppApi::cUnityApi::il2cpp_class_is_abstract(const Il2CppClass *klass) {
    //il2cpp_class_is_abstract 函数指针
    typedef bool (*pil2cpp_class_is_abstract)(const Il2CppClass *klass);

    static pil2cpp_class_is_abstract m_ppil2cpp_class_is_abstract = nullptr;
    if(m_ppil2cpp_class_is_abstract == nullptr){
        m_ppil2cpp_class_is_abstract = (pil2cpp_class_is_abstract)m_li2cppso->find(
                "il2cpp_class_is_abstract");
        if (m_ppil2cpp_class_is_abstract) {

        }
    }
    return m_ppil2cpp_class_is_abstract(klass);
}

bool li2cppApi::cUnityApi::il2cpp_class_is_interface(const Il2CppClass *klass) {
    //il2cpp_class_is_interface 函数指针
    typedef bool (*pil2cpp_class_is_interface)(const Il2CppClass *klass);
    static pil2cpp_class_is_interface m_ppil2cpp_class_is_interface = nullptr;
    if(m_ppil2cpp_class_is_interface == nullptr){
        m_ppil2cpp_class_is_interface = (pil2cpp_class_is_interface)m_li2cppso->find(
                "il2cpp_class_is_interface");
        if (m_ppil2cpp_class_is_interface) {

        }
    }
    return m_ppil2cpp_class_is_interface(klass);
}

bool li2cppApi::cUnityApi::il2cpp_class_is_enum(const Il2CppClass *klass) {
    //il2cpp_class_is_enum 函数指针
    typedef bool (*pil2cpp_class_is_enum)(const Il2CppClass *klass);
    static pil2cpp_class_is_enum m_ppil2cpp_class_is_enum = nullptr;
    if(m_ppil2cpp_class_is_enum == nullptr){
        m_ppil2cpp_class_is_enum = (pil2cpp_class_is_enum)m_li2cppso->find(
                "il2cpp_class_is_enum");
        if (m_ppil2cpp_class_is_enum) {

        }
    }
    return m_ppil2cpp_class_is_enum(klass);
}

const char *li2cppApi::cUnityApi::il2cpp_field_get_name(FieldInfo *field) {
    //il2cpp_class_is_enum 函数指针
    typedef const char* (*pil2cpp_field_get_name)(FieldInfo *field);
    static pil2cpp_field_get_name m_ppil2cpp_field_get_name = nullptr;
    if(m_ppil2cpp_field_get_name == nullptr){
        m_ppil2cpp_field_get_name = (pil2cpp_field_get_name)m_li2cppso->find(
                "il2cpp_field_get_name");
        if (m_ppil2cpp_field_get_name) {

        }
    }
    return m_ppil2cpp_field_get_name(field);
}

int li2cppApi::cUnityApi::il2cpp_class_get_flags(const Il2CppClass *klass) {
    //il2cpp_class_is_enum 函数指针
    typedef int (*pil2cpp_class_get_flags)(const Il2CppClass *klass);
    static pil2cpp_class_get_flags m_ppil2cpp_class_get_flags = nullptr;
    if(m_ppil2cpp_class_get_flags == nullptr){
        m_ppil2cpp_class_get_flags = (pil2cpp_class_get_flags)m_li2cppso->find(
                "il2cpp_class_get_flags");
        if (m_ppil2cpp_class_get_flags) {

        }
    }
    return m_ppil2cpp_class_get_flags(klass);
}

bool li2cppApi::cUnityApi::il2cpp_class_is_valuetype(const Il2CppClass *klass) {
    //il2cpp_class_is_valuetype 函数指针
    typedef bool (*pil2cpp_class_is_valuetype)(const Il2CppClass* klass);
    static pil2cpp_class_is_valuetype m_ppil2cpp_class_is_valuetype = nullptr;
    if(m_ppil2cpp_class_is_valuetype == nullptr){
        m_ppil2cpp_class_is_valuetype = (pil2cpp_class_is_valuetype)m_li2cppso->find(
                "il2cpp_class_is_valuetype");
        if (m_ppil2cpp_class_is_valuetype) {

        }
    }
    return m_ppil2cpp_class_is_valuetype(klass);
}

Il2CppClass *li2cppApi::cUnityApi::il2cpp_class_get_interfaces(Il2CppClass *klass, void **iter) {
    //il2cpp_class_get_interfaces 函数指针
    typedef Il2CppClass* (*pil2cpp_class_get_interfaces)(Il2CppClass *klass, void* *iter);
    static pil2cpp_class_get_interfaces m_ppil2cpp_class_get_interfaces = nullptr;
    if(m_ppil2cpp_class_get_interfaces == nullptr){
        m_ppil2cpp_class_get_interfaces = (pil2cpp_class_get_interfaces)m_li2cppso->find(
                "il2cpp_class_get_interfaces");
        if (m_ppil2cpp_class_get_interfaces) {

        }
    }
    return m_ppil2cpp_class_get_interfaces(klass,iter);
}

FieldInfo *li2cppApi::cUnityApi::il2cpp_class_get_fields(Il2CppClass *klass, void **iter) {
    //il2cpp_class_get_fields 函数指针
    typedef FieldInfo* (*pil2cpp_class_get_fields)(Il2CppClass *klass, void* *iter);
    static pil2cpp_class_get_fields m_ppil2cpp_class_get_fields = nullptr;
    if(m_ppil2cpp_class_get_fields == nullptr){
        m_ppil2cpp_class_get_fields = (pil2cpp_class_get_fields)m_li2cppso->find(
                "il2cpp_class_get_fields");
        if (m_ppil2cpp_class_get_fields) {

        }
    }
    return m_ppil2cpp_class_get_fields(klass,iter);
}

int li2cppApi::cUnityApi::il2cpp_field_get_flags(FieldInfo *field) {
    //il2cpp_field_get_flags 函数指针
    typedef int (*pil2cpp_field_get_flags)(FieldInfo *field);
    static pil2cpp_field_get_flags m_ppil2cpp_field_get_flags = nullptr;
    if(m_ppil2cpp_field_get_flags == nullptr){
        m_ppil2cpp_field_get_flags = (pil2cpp_field_get_flags)m_li2cppso->find(
                "il2cpp_field_get_flags");
        if (m_ppil2cpp_field_get_flags) {

        }
    }
    return m_ppil2cpp_field_get_flags(field);
}

const Il2CppType *li2cppApi::cUnityApi::il2cpp_field_get_type(FieldInfo *field) {
    //il2cpp_field_get_type 函数指针
    typedef const Il2CppType* (*pil2cpp_field_get_type)(FieldInfo *field);
    static pil2cpp_field_get_type m_ppil2cpp_field_get_type = nullptr;
    if(m_ppil2cpp_field_get_type == nullptr){
        m_ppil2cpp_field_get_type = (pil2cpp_field_get_type)m_li2cppso->find(
                "il2cpp_field_get_type");
        if (m_ppil2cpp_field_get_type) {

        }
    }
    return m_ppil2cpp_field_get_type(field);
}

void li2cppApi::cUnityApi::il2cpp_field_static_get_value(FieldInfo *field, void *value) {
    //il2cpp_field_static_get_value 函数指针
    typedef void (*pil2cpp_field_static_get_value)(FieldInfo *field, void *value);
    static pil2cpp_field_static_get_value m_ppil2cpp_field_static_get_value = nullptr;
    if(m_ppil2cpp_field_static_get_value == nullptr){
        m_ppil2cpp_field_static_get_value = (pil2cpp_field_static_get_value)m_li2cppso->find(
                "il2cpp_field_static_get_value");
        if (m_ppil2cpp_field_static_get_value) {
        }
    }
    m_ppil2cpp_field_static_get_value(field,value);
}

size_t li2cppApi::cUnityApi::il2cpp_field_get_offset(FieldInfo *field) {

    typedef size_t (*pil2cpp_field_get_offset)(FieldInfo *field);
    static pil2cpp_field_get_offset m_ppil2cpp_field_get_offset = nullptr;
    if(m_ppil2cpp_field_get_offset == nullptr){
        m_ppil2cpp_field_get_offset = (pil2cpp_field_get_offset)m_li2cppso->find(
                "il2cpp_field_get_offset");
        if (m_ppil2cpp_field_get_offset) {
        }
    }
    return m_ppil2cpp_field_get_offset(field);
}

void li2cppApi::cUnityApi::il2cpp_runtime_class_init(Il2CppClass* klass)  {

    typedef void (*pil2cpp_runtime_class_init)(Il2CppClass* klass);
    static pil2cpp_runtime_class_init m_ppil2cpp_runtime_class_init = nullptr;
    if(m_ppil2cpp_runtime_class_init == nullptr){
        m_ppil2cpp_runtime_class_init = (pil2cpp_runtime_class_init)m_li2cppso->find(
                "il2cpp_runtime_class_init");
        if (m_ppil2cpp_runtime_class_init) {
        }
    }
    m_ppil2cpp_runtime_class_init(klass);
}

const PropertyInfo *
li2cppApi::cUnityApi::il2cpp_class_get_properties(Il2CppClass *klass, void **iter) {

    typedef const PropertyInfo* (*pil2cpp_class_get_properties)(Il2CppClass *klass, void* *iter);
    static pil2cpp_class_get_properties m_ppil2cpp_class_get_properties = nullptr;
    if(m_ppil2cpp_class_get_properties == nullptr){
        m_ppil2cpp_class_get_properties = (pil2cpp_class_get_properties)m_li2cppso->find(
                "il2cpp_class_get_properties");
        if (m_ppil2cpp_class_get_properties) {
        }
    }
    return m_ppil2cpp_class_get_properties(klass,iter);
}


// =========================================================================
// 1. const MethodInfo* il2cpp_property_get_get_method(PropertyInfo *prop)
// =========================================================================
const MethodInfo* li2cppApi::cUnityApi::il2cpp_property_get_get_method(PropertyInfo *prop) {
    // 1. 定义函数指针类型
    typedef const MethodInfo* (*pil2cpp_property_get_get_method)(PropertyInfo *prop);

    // 2. 静态存储函数指针
    static pil2cpp_property_get_get_method m_ppil2cpp_property_get_get_method = nullptr;

    // 3. 首次调用时查找函数地址
    if (m_ppil2cpp_property_get_get_method == nullptr) {
        m_ppil2cpp_property_get_get_method = (pil2cpp_property_get_get_method)m_li2cppso->find(
                "il2cpp_property_get_get_method");

        if (m_ppil2cpp_property_get_get_method) {

        }
    }

    // 4. 调用原始函数
    if (m_ppil2cpp_property_get_get_method) {
        return m_ppil2cpp_property_get_get_method(prop);
    }
    return nullptr;
}

// =========================================================================
// 2. const MethodInfo* il2cpp_property_get_set_method(PropertyInfo *prop)
// =========================================================================
const MethodInfo* li2cppApi::cUnityApi::il2cpp_property_get_set_method(PropertyInfo *prop) {
    // 1. 定义函数指针类型
    typedef const MethodInfo* (*pil2cpp_property_get_set_method)(PropertyInfo *prop);

    // 2. 静态存储函数指针
    static pil2cpp_property_get_set_method m_ppil2cpp_property_get_set_method = nullptr;

    // 3. 首次调用时查找函数地址
    if (m_ppil2cpp_property_get_set_method == nullptr) {
        m_ppil2cpp_property_get_set_method = (pil2cpp_property_get_set_method)m_li2cppso->find(
                "il2cpp_property_get_set_method");

        if (m_ppil2cpp_property_get_set_method) {

         }
    }

    // 4. 调用原始函数
    if (m_ppil2cpp_property_get_set_method) {
        return m_ppil2cpp_property_get_set_method(prop);
    }
    return nullptr;
}


// =========================================================================
// 3. const char* il2cpp_property_get_name(PropertyInfo *prop)
// =========================================================================
const char* li2cppApi::cUnityApi::il2cpp_property_get_name(PropertyInfo *prop) {
    // 1. 定义函数指针类型
    typedef const char *(*pil2cpp_property_get_name)(PropertyInfo *prop);

    // 2. 静态存储函数指针
    static pil2cpp_property_get_name m_ppil2cpp_property_get_name = nullptr;

    // 3. 首次调用时查找函数地址
    if (m_ppil2cpp_property_get_name == nullptr) {
        m_ppil2cpp_property_get_name = (pil2cpp_property_get_name) m_li2cppso->find(
                "il2cpp_property_get_name");

        if (m_ppil2cpp_property_get_name) {

        }
    }

    // 4. 调用原始函数
    if (m_ppil2cpp_property_get_name) {
        return m_ppil2cpp_property_get_name(prop);
    }
    // 找不到函数时返回安全值
    return "";
}

// =========================================================================
// 1. uint32_t il2cpp_method_get_flags(const MethodInfo *method, uint32_t *iflags)
// =========================================================================
uint32_t li2cppApi::cUnityApi::il2cpp_method_get_flags(const MethodInfo *method, uint32_t *iflags) {
    // 1. 定义函数指针类型
    typedef uint32_t (*pil2cpp_method_get_flags)(const MethodInfo *method, uint32_t *iflags);

    // 2. 静态存储函数指针
    static pil2cpp_method_get_flags m_ppil2cpp_method_get_flags = nullptr;

    // 3. 首次调用时查找函数地址
    if (m_ppil2cpp_method_get_flags == nullptr) {
        m_ppil2cpp_method_get_flags = (pil2cpp_method_get_flags)m_li2cppso->find(
                "il2cpp_method_get_flags");

        if (m_ppil2cpp_method_get_flags) {

        }
    }

    // 4. 调用原始函数
    if (m_ppil2cpp_method_get_flags) {
        return m_ppil2cpp_method_get_flags(method, iflags);
    }
    return 0; // 找不到函数时返回安全值 0
}

// =========================================================================
// 2. const Il2CppType* il2cpp_method_get_return_type(const MethodInfo* method)
// =========================================================================
const Il2CppType* li2cppApi::cUnityApi::il2cpp_method_get_return_type(const MethodInfo* method) {
    // 1. 定义函数指针类型
    typedef const Il2CppType* (*pil2cpp_method_get_return_type)(const MethodInfo* method);

    // 2. 静态存储函数指针
    static pil2cpp_method_get_return_type m_ppil2cpp_method_get_return_type = nullptr;

    // 3. 首次调用时查找函数地址
    if (m_ppil2cpp_method_get_return_type == nullptr) {
        m_ppil2cpp_method_get_return_type = (pil2cpp_method_get_return_type)m_li2cppso->find(
                "il2cpp_method_get_return_type");

        if (m_ppil2cpp_method_get_return_type) {

         }
    }

    // 4. 调用原始函数
    if (m_ppil2cpp_method_get_return_type) {
        return m_ppil2cpp_method_get_return_type(method);
    }
    return nullptr; // 找不到函数时返回安全值 nullptr
}


// =========================================================================
// 3. const Il2CppType* il2cpp_method_get_param(const MethodInfo *method, uint32_t index)
// =========================================================================
const Il2CppType* li2cppApi::cUnityApi::il2cpp_method_get_param(const MethodInfo *method, uint32_t index) {
    // 1. 定义函数指针类型
    typedef const Il2CppType* (*pil2cpp_method_get_param)(const MethodInfo *method, uint32_t index);

    // 2. 静态存储函数指针
    static pil2cpp_method_get_param m_ppil2cpp_method_get_param = nullptr;

    // 3. 首次调用时查找函数地址
    if (m_ppil2cpp_method_get_param == nullptr) {
        m_ppil2cpp_method_get_param = (pil2cpp_method_get_param)m_li2cppso->find(
                "il2cpp_method_get_param");

        if (m_ppil2cpp_method_get_param) {

        }
    }

    // 4. 调用原始函数
    if (m_ppil2cpp_method_get_param) {
        return m_ppil2cpp_method_get_param(method, index);
    }
    return nullptr; // 找不到函数时返回安全值 nullptr
}

// =========================================================================
// 1. const MethodInfo* il2cpp_class_get_methods(Il2CppClass *klass, void* *iter)
// =========================================================================
const MethodInfo* li2cppApi::cUnityApi::il2cpp_class_get_methods(Il2CppClass *klass, void* *iter) {
    // 1. 定义函数指针类型
    typedef const MethodInfo* (*pil2cpp_class_get_methods)(Il2CppClass *klass, void* *iter);

    // 2. 静态存储函数指针
    static pil2cpp_class_get_methods m_ppil2cpp_class_get_methods = nullptr;

    // 3. 首次调用时查找函数地址
    if (m_ppil2cpp_class_get_methods == nullptr) {
        m_ppil2cpp_class_get_methods = (pil2cpp_class_get_methods)m_li2cppso->find(
                "il2cpp_class_get_methods");

        if (m_ppil2cpp_class_get_methods) {

       }
    }

    // 4. 调用原始函数
    if (m_ppil2cpp_class_get_methods) {
        return m_ppil2cpp_class_get_methods(klass, iter);
    }
    return nullptr; // 找不到函数时返回安全值 nullptr
}

// =========================================================================
// 2. uint32_t il2cpp_method_get_param_count(const MethodInfo *method)
// =========================================================================
uint32_t li2cppApi::cUnityApi::il2cpp_method_get_param_count(const MethodInfo *method) {
    // 1. 定义函数指针类型
    typedef uint32_t (*pil2cpp_method_get_param_count)(const MethodInfo *method);

    // 2. 静态存储函数指针
    static pil2cpp_method_get_param_count m_ppil2cpp_method_get_param_count = nullptr;

    // 3. 首次调用时查找函数地址
    if (m_ppil2cpp_method_get_param_count == nullptr) {
        m_ppil2cpp_method_get_param_count = (pil2cpp_method_get_param_count)m_li2cppso->find(
                "il2cpp_method_get_param_count");

        if (m_ppil2cpp_method_get_param_count) {

        }
    }

    // 4. 调用原始函数
    if (m_ppil2cpp_method_get_param_count) {
        return m_ppil2cpp_method_get_param_count(method);
    }
    return 0; // 找不到函数时返回安全值 0
}


// =========================================================================
// 3. const char* il2cpp_method_get_name(const MethodInfo *method)
// =========================================================================
const char* li2cppApi::cUnityApi::il2cpp_method_get_name(const MethodInfo *method) {
    // 1. 定义函数指针类型
    typedef const char* (*pil2cpp_method_get_name)(const MethodInfo *method);

    // 2. 静态存储函数指针
    static pil2cpp_method_get_name m_ppil2cpp_method_get_name = nullptr;

    // 3. 首次调用时查找函数地址
    if (m_ppil2cpp_method_get_name == nullptr) {
        m_ppil2cpp_method_get_name = (pil2cpp_method_get_name)m_li2cppso->find(
                "il2cpp_method_get_name");

        if (m_ppil2cpp_method_get_name) {

        }
    }

    // 4. 调用原始函数
    if (m_ppil2cpp_method_get_name) {
        return m_ppil2cpp_method_get_name(method);
    }
    return ""; // 找不到函数时返回安全值 ""
}

// =========================================================================
// 4. const char* il2cpp_method_get_param_name(const MethodInfo *method, uint32_t index)
// =========================================================================
const char* li2cppApi::cUnityApi::il2cpp_method_get_param_name(const MethodInfo *method, uint32_t index) {
    // 1. 定义函数指针类型
    typedef const char* (*pil2cpp_method_get_param_name)(const MethodInfo *method, uint32_t index);

    // 2. 静态存储函数指针
    static pil2cpp_method_get_param_name m_ppil2cpp_method_get_param_name = nullptr;

    // 3. 首次调用时查找函数地址
    if (m_ppil2cpp_method_get_param_name == nullptr) {
        m_ppil2cpp_method_get_param_name = (pil2cpp_method_get_param_name)m_li2cppso->find(
                "il2cpp_method_get_param_name");

        if (m_ppil2cpp_method_get_param_name) {

       }
    }

    // 4. 调用原始函数
    if (m_ppil2cpp_method_get_param_name) {
        return m_ppil2cpp_method_get_param_name(method, index);
    }
    return ""; // 找不到函数时返回安全值 ""
}

bool li2cppApi::cUnityApi::_il2cpp_type_is_byref(const Il2CppType *type) {
    auto byref = type->byref;

    return il2cpp_type_is_byref(type);
}

bool li2cppApi::cUnityApi::il2cpp_type_is_byref(const Il2CppType *type) {

    typedef bool (*pil2cpp_type_is_byref)(const Il2CppType *type);

    // 2. 静态存储函数指针
    static pil2cpp_type_is_byref m_ppil2cpp_type_is_byref = nullptr;

    // 3. 首次调用时查找函数地址
    if (m_ppil2cpp_type_is_byref == nullptr) {
        m_ppil2cpp_type_is_byref = (pil2cpp_type_is_byref)m_li2cppso->find(
                "il2cpp_type_is_byref");

        if (m_ppil2cpp_type_is_byref) {

       }
    }

    // 4. 调用原始函数
    if (m_ppil2cpp_type_is_byref) {
        return m_ppil2cpp_type_is_byref(type);
    }
    return ""; // 找不到函数时返回安全值 ""
}

bool li2cppApi::cUnityApi::il2cpp_method_is_generic(const MethodInfo *method) {

    typedef bool (*pil2cpp_method_is_generic)(const MethodInfo *method);
    static pil2cpp_method_is_generic m_ppil2cpp_method_is_generic = nullptr;
    if (m_ppil2cpp_method_is_generic == nullptr) {
        m_ppil2cpp_method_is_generic = (pil2cpp_method_is_generic)m_li2cppso->find(
                "il2cpp_method_is_generic");

        if (m_ppil2cpp_method_is_generic) {

        }
    }
    // 4. 调用原始函数
    if (m_ppil2cpp_method_is_generic) {
        return m_ppil2cpp_method_is_generic(method);
    }
    return false; // 找不到函数时返回安全值 ""
}

bool li2cppApi::cUnityApi::il2cpp_method_is_instance(const MethodInfo *method) {
    typedef bool (*pil2cpp_method_is_instance)(const MethodInfo *method);
    static pil2cpp_method_is_instance m_ppil2cpp_method_is_instance = nullptr;
    if (m_ppil2cpp_method_is_instance == nullptr) {
        m_ppil2cpp_method_is_instance = (pil2cpp_method_is_instance)m_li2cppso->find(
                "il2cpp_method_is_instance");

        if (m_ppil2cpp_method_is_instance) {

        }
    }
    // 4. 调用原始函数
    if (m_ppil2cpp_method_is_instance) {
        return m_ppil2cpp_method_is_instance(method);
    }
    return false; // 找不到函数时返回安全值 ""
}
Il2CppMetadataUsage li2cppApi::cUnityApi::GetEncodedIndexType(EncodedMethodIndex index) {
    return (Il2CppMetadataUsage)((index) >> 29);
}
template<typename T>
bool li2cppApi::cUnityApi::IsRuntimeMetadataInitialized(T item) {
    return !((uintptr_t)item & 1);
}
uint32_t li2cppApi::cUnityApi::GetDecodedMethodIndex(EncodedMethodIndex index)
{
    return (index & 0x1FFFFFFF);
}
Il2CppString *li2cppApi::cUnityApi::GetStringLiteralFromIndex(StringLiteralIndex index) {

    if (index == kStringLiteralIndexInvalid)
        return NULL;
    Il2CppString* newString = nullptr;

#ifdef UNITY_2018_4_16F1
    // 原始地址: 000000000B9BBE1B
    // 大小: 256 bytes
    const uint8_t byte_B9BBE1B[256] = {
            0xC4, 0xCA, 0x42, 0x38, 0xA0, 0xB9, 0x23, 0x82, 0x0D, 0xCC, 0x50, 0x9A, 0x6F, 0x75, 0x84, 0x9B,
            0xC8, 0x1E, 0x72, 0x8D, 0x9D, 0x4C, 0x2F, 0x63, 0x6F, 0x06, 0x7F, 0x89, 0xCC, 0x14, 0x86, 0x2C,
            0xEC, 0xCB, 0xC8, 0x7E, 0x4B, 0x5C, 0xE2, 0xFE, 0x28, 0x30, 0x8F, 0xD9, 0xF2, 0xA7, 0xBA, 0xF3,
            0xA8, 0x7F, 0xF6, 0x79, 0xA2, 0xF3, 0xE7, 0x1D, 0x91, 0x81, 0xA6, 0x7B, 0x75, 0x42, 0x12, 0x2C,
            0xE4, 0xDA, 0x3B, 0x7F, 0xBB, 0xCE, 0x23, 0x45, 0xD7, 0x77, 0x2B, 0x06, 0x74, 0xA3, 0x18, 0xD5,
            0x16, 0x79, 0x09, 0x1C, 0x5A, 0x88, 0x0F, 0xAF, 0x6F, 0xB5, 0xE6, 0x08, 0x7E, 0xB1, 0xB2, 0xDC,
            0x8F, 0x14, 0xE4, 0x5F, 0xCE, 0xEA, 0x16, 0x7A, 0x5A, 0x36, 0xDE, 0xDD, 0x4B, 0xEA, 0x25, 0x43,
            0xC9, 0xF0, 0xF8, 0x95, 0xFB, 0x98, 0xAB, 0x91, 0x59, 0xF5, 0x1F, 0xD0, 0x29, 0x7E, 0x23, 0x6D,
            0x45, 0xC4, 0x8C, 0xCE, 0x2E, 0x2D, 0x7F, 0xBD, 0xEA, 0x1A, 0xFC, 0x51, 0xC7, 0xC6, 0xAD, 0x26,
            0xD3, 0xD9, 0x44, 0x68, 0x02, 0xA4, 0x42, 0x59, 0x75, 0x5D, 0x38, 0xE6, 0xD1, 0x63, 0xE8, 0x20,
            0x65, 0x12, 0xBD, 0x43, 0xD9, 0xCA, 0xA6, 0xE0, 0x2C, 0x99, 0x0B, 0x0A, 0x82, 0x65, 0x2D, 0xCA,
            0xC2, 0x0A, 0xD4, 0xD7, 0x6F, 0xE9, 0x77, 0x59, 0xAA, 0x27, 0xA0, 0xC9, 0x9B, 0xFF, 0x67, 0x10,
            0xC5, 0x1C, 0xE4, 0x10, 0xC1, 0x24, 0xA1, 0x0E, 0x0D, 0xB5, 0xE4, 0xB9, 0x7F, 0xC2, 0xAF, 0x39,
            0xAA, 0xB3, 0x23, 0x89, 0x22, 0xBC, 0xC2, 0x5A, 0x6F, 0x60, 0x6E, 0xB5, 0x25, 0xFF, 0xDC, 0x56,
            0x9B, 0xF3, 0x1C, 0x7F, 0xF0, 0x62, 0x93, 0x6A, 0x96, 0xD3, 0xC8, 0xBD, 0x1F, 0x8F, 0x2F, 0xF3,
            0xC7, 0x4D, 0x97, 0xB0, 0x1E, 0xAE, 0x25, 0x7E, 0x44, 0xAA, 0x9D, 0x5B, 0xAD, 0xE9, 0x7B, 0xAF
    };

    const Il2CppStringLiteral* stringLiteral = (const Il2CppStringLiteral*)
                                                       ((const char*)m_pGlobalMetadata + m_pGlobalMetadataHeader->stringLiteralOffset) + index;
    char *pstr = (char*)((uint64_t)m_pGlobalMetadata + m_pGlobalMetadataHeader->stringLiteralDataOffset) + stringLiteral->dataIndex;
    char v8 = 0;
    int v7 = 0;
    int v6 = 0;
    int v5 = 0;
    int strlen = 0;
    if(stringLiteral && stringLiteral->length>0){
        v7 = 16 * (stringLiteral->length & 0xF);
        do
        {
            v8 = byte_B9BBE1B[v6 | v7];
            v6 = ((char)v6 + 1) & 0xF;
            *(char*)(pstr + v5) ^= v8;
            strlen = stringLiteral->length;
            ++v5;
        }
        while ( v5 < stringLiteral->length );
    }
     newString = il2cpp_string_new_len(pstr,strlen);
#else
    LOG(LOG_LEVEL_INFO,"StringLiteralIndex : %d",index);
    const Il2CppStringLiteral* stringLiteral = (const Il2CppStringLiteral*)
                                                       ((const char*)m_pGlobalMetadata + m_pGlobalMetadataHeader->stringLiteralOffset) + index;

    //解密
    if(m_lolgame != nullptr) {
        LOG(LOG_LEVEL_INFO,"m_lolgame in");
        //m_lolgame->decryPtthestring((char *) stringLiteral, index);
    }

    Il2CppString* newString = il2cpp_string_new_len(
            (const char*)m_pGlobalMetadata + m_pGlobalMetadataHeader->stringLiteralDataOffset + stringLiteral->dataIndex
            ,stringLiteral->length);
#endif

    return newString;
}

Il2CppString *li2cppApi::cUnityApi::il2cpp_string_new_len(const char *str, uint32_t length) {
    typedef Il2CppString* (*pil2cpp_string_new_len)(const char* str, uint32_t length);
    static pil2cpp_string_new_len m_ppil2cpp_string_new_len = nullptr;
    if (m_ppil2cpp_string_new_len == nullptr) {
        m_ppil2cpp_string_new_len = (pil2cpp_string_new_len)m_li2cppso->find(
                "il2cpp_string_new_len");
        if (m_ppil2cpp_string_new_len) {

        }
    }
    // 4. 调用原始函数
    if (m_ppil2cpp_string_new_len) {
        return m_ppil2cpp_string_new_len(str,length);
    }
    return nullptr; // 找不到函数时返回安全值 ""
}

Il2CppClass *li2cppApi::cUnityApi::il2cpp_class_get_nested_types(Il2CppClass *klass, void **iter) {
    typedef Il2CppClass* (*pil2cpp_class_get_nested_types)(Il2CppClass *klass, void **iter);
    static pil2cpp_class_get_nested_types m_ppil2cpp_class_get_nested_types = nullptr;
    if (m_ppil2cpp_class_get_nested_types == nullptr) {
        m_ppil2cpp_class_get_nested_types = (pil2cpp_class_get_nested_types)m_li2cppso->find(
                "il2cpp_class_get_nested_types");
        if (m_ppil2cpp_class_get_nested_types) {

        }
    }
    // 4. 调用原始函数
    if (m_ppil2cpp_class_get_nested_types) {
        return m_ppil2cpp_class_get_nested_types(klass,iter);
    }
    return nullptr; // 找不到函数时返回安全值 ""
}

const char *li2cppApi::cUnityApi::il2cpp_type_get_name(const Il2CppType *type) {

    typedef char* (*pil2cpp_type_get_name)(const Il2CppType *type);
    static pil2cpp_type_get_name m_ppil2cpp_type_get_name = nullptr;
    if (m_ppil2cpp_type_get_name == nullptr) {
        m_ppil2cpp_type_get_name = (pil2cpp_type_get_name)m_li2cppso->find(
                "il2cpp_type_get_name");
        if (m_ppil2cpp_type_get_name) {

        }
    }
    // 4. 调用原始函数
    if (m_ppil2cpp_type_get_name) {
        return m_ppil2cpp_type_get_name(type);
    }
    return nullptr; // 找不到函数时返回安全值 ""
}

int li2cppApi::cUnityApi::il2cpp_class_get_rank(const Il2CppClass *klass){

    typedef int (*pil2cpp_class_get_rank)(const Il2CppClass *klass);
    static pil2cpp_class_get_rank m_ppil2cpp_class_get_rank = nullptr;
    if (m_ppil2cpp_class_get_rank == nullptr) {
        m_ppil2cpp_class_get_rank = (pil2cpp_class_get_rank)m_li2cppso->find(
                "il2cpp_class_get_rank");
        if (m_ppil2cpp_class_get_rank) {

        }
    }
    // 4. 调用原始函数
    if (m_ppil2cpp_class_get_rank) {
        return m_ppil2cpp_class_get_rank(klass);
    }
    return 0; // 找不到函数时返回安全值 ""
}


std::string li2cppApi::cUnityApi::Utf16ToUtf8(const Il2CppChar *utf16String) {
    return Utf16ToUtf8(utf16String,-1);
}

template<typename u16_type>
inline uint16_t li2cppApi::cUnityApi::mask16(u16_type oc)
{
    return static_cast<uint16_t>(0xffff & oc);
}

template <typename u16>
inline bool li2cppApi::cUnityApi::is_lead_surrogate(u16 cp)
{
    return (cp >= LEAD_SURROGATE_MIN && cp <= LEAD_SURROGATE_MAX);
}

template <typename u16>
inline bool li2cppApi::cUnityApi::is_trail_surrogate(u16 cp)
{
    return (cp >= TRAIL_SURROGATE_MIN && cp <= TRAIL_SURROGATE_MAX);
}

template <typename octet_iterator>
octet_iterator li2cppApi::cUnityApi::append(uint32_t cp, octet_iterator result)
{
    if (cp < 0x80)                        // one octet
        *(result++) = static_cast<uint8_t>(cp);
    else if (cp < 0x800) {                // two octets
        *(result++) = static_cast<uint8_t>((cp >> 6)          | 0xc0);
        *(result++) = static_cast<uint8_t>((cp & 0x3f)        | 0x80);
    }
    else if (cp < 0x10000) {              // three octets
        *(result++) = static_cast<uint8_t>((cp >> 12)         | 0xe0);
        *(result++) = static_cast<uint8_t>(((cp >> 6) & 0x3f) | 0x80);
        *(result++) = static_cast<uint8_t>((cp & 0x3f)        | 0x80);
    }
    else {                                // four octets
        *(result++) = static_cast<uint8_t>((cp >> 18)         | 0xf0);
        *(result++) = static_cast<uint8_t>(((cp >> 12) & 0x3f)| 0x80);
        *(result++) = static_cast<uint8_t>(((cp >> 6) & 0x3f) | 0x80);
        *(result++) = static_cast<uint8_t>((cp & 0x3f)        | 0x80);
    }
    return result;
}

template<typename u16bit_iterator, typename octet_iterator>
octet_iterator
li2cppApi::cUnityApi::utf16to8(u16bit_iterator start, u16bit_iterator end, octet_iterator result) {
    static const uint32_t replacement_marker = mask16(0xfffd);
    while (start != end) {
        uint32_t cp = mask16(*start++);
        // Take care of surrogate pairs first
        if (is_lead_surrogate(cp)) {
            if (start != end) {
                uint32_t trail_surrogate = mask16(*start++);
                if (is_trail_surrogate(trail_surrogate))
                    cp = (cp << 10) + trail_surrogate + SURROGATE_OFFSET;
                else
                    cp = replacement_marker;
            }
            else
                cp = replacement_marker;

        }
            // Lone trail surrogate
        else if (is_trail_surrogate(cp))
            cp = replacement_marker;

        result = append(cp, result);
    }
    return result;
}

std::string li2cppApi::cUnityApi::Utf16ToUtf8(const Il2CppChar *utf16String, int maximumSize) {
    const Il2CppChar* ptr = utf16String;
    size_t length = 0;
    while (*ptr)
    {
        ptr++;
        length++;
        if (maximumSize != -1 && length == maximumSize)
            break;
    }

    std::string utf8String;
    utf8String.reserve(length);
    utf16to8(utf16String, ptr, std::back_inserter(utf8String));

    return utf8String;
}

std::string li2cppApi::cUnityApi::il2cpp_Il2CppString_toCString(const Il2CppString *pstr) {
    return Utf16ToUtf8(pstr->chars,pstr->length);
}

const Il2CppGenericMethod *
li2cppApi::cUnityApi::GetGenericMethodFromIndex(GenericMethodIndex index) {
    IL2CPP_ASSERT(index < m_pil2CppMetadataRegistration->methodSpecsCount);

    const Il2CppMethodSpec* methodSpec = m_pil2CppMetadataRegistration->methodSpecs + index;
    const MethodInfo* methodDefinition = GetMethodInfoFromMethodDefinitionIndex(methodSpec->methodDefinitionIndex);



    return nullptr;
}

const MethodInfo* li2cppApi::cUnityApi::GetMethodInfoFromMethodDefinitionIndex(MethodIndex index){
    IL2CPP_ASSERT(index >= 0 && static_cast<uint32_t>(index) <= m_pGlobalMetadataHeader->methodsCount / sizeof(Il2CppMethodDefinition));
    const Il2CppMethodDefinition* methodDefinition = GetMethodDefinitionFromIndex(index);
    //GetStringFromIndex(methodDefinition->nameIndex);

    const Il2CppTypeDefinition* typeDefinitions = (const Il2CppTypeDefinition*)((const char*)m_pGlobalMetadata + m_pGlobalMetadataHeader->typeDefinitionsOffset);
    const Il2CppTypeDefinition* typeDefinition = typeDefinitions + index;


    return nullptr;
}

template<typename T>
T li2cppApi::cUnityApi::MetadataOffset(const void* metadata, size_t sectionOffset, size_t itemIndex){
    return reinterpret_cast<T>(reinterpret_cast<uint8_t*>(const_cast<void*>(metadata)) + sectionOffset) + itemIndex;
}


const Il2CppMethodDefinition* li2cppApi::cUnityApi::GetMethodDefinitionFromIndex(MethodIndex index){
#ifdef UNITY_2018_4_16F1
    IL2CPP_ASSERT(index >= 0 && static_cast<uint32_t>(index) <= m_pGlobalMetadataHeader->methodsCount / sizeof(Il2CppMethodDefinition));
#else
    IL2CPP_ASSERT(index >= 0 && static_cast<uint32_t>(index) <= m_pGlobalMetadataHeader->methodsSize / sizeof(Il2CppMethodDefinition));
#endif
    return MetadataOffset<const Il2CppMethodDefinition*>(m_pGlobalMetadata, m_pGlobalMetadataHeader->methodsOffset, index);
}

const char *li2cppApi::cUnityApi::GetStringFromIndex(StringIndex index) {
#ifdef UNITY_2018_4_16F1
    IL2CPP_ASSERT(index <= m_pGlobalMetadataHeader->stringCount);
    char* pstr= (char*)((uint64_t)m_pGlobalMetadata + m_pGlobalMetadataHeader->stringOffset+ index);
    //LOG(LOG_LEVEL_INFO,"m_pGlobalMetadata : %p stringOffset : %d index ：%d",
    //    m_pGlobalMetadata,m_pGlobalMetadataHeader->stringOffset,index);
    //m_lolgame->decryPtthestring(pstr,index);
    return (const char *)pstr;
#else
    IL2CPP_ASSERT(index <= m_pGlobalMetadataHeader->stringSize);
    return MetadataOffset<const char*>(m_pGlobalMetadata, m_pGlobalMetadataHeader->stringOffset, index);
#endif
}
const char* li2cppApi::cUnityApi::GetWindowsRuntimeStringFromIndex(StringIndex index){
#ifdef UNITY_2018_4_16F1
    IL2CPP_ASSERT(index <= m_pGlobalMetadataHeader->windowsRuntimeTypeNamesSize);
    return MetadataOffset<const char*>(m_pGlobalMetadata, m_pGlobalMetadataHeader->windowsRuntimeTypeNamesOffset, index);
#else
    IL2CPP_ASSERT(index <= m_pGlobalMetadataHeader->windowsRuntimeStringsSize);
     return MetadataOffset<const char*>(m_pGlobalMetadata, m_pGlobalMetadataHeader->windowsRuntimeStringsOffset, index);
#endif
}

const Il2CppGenericClass *
li2cppApi::cUnityApi::il2cpp_class_get_generic_class(const Il2CppClass *klass) {
    if(klass){
        return klass->generic_class;
    }
    return nullptr;
}

const Il2CppGenericContext*
li2cppApi::cUnityApi::il2cpp_generic_class_get_context(const Il2CppGenericClass *klass) {
    if(klass){
        return &klass->context;
    }
    return nullptr;
}

const Il2CppGenericInst *li2cppApi::cUnityApi::il2cpp_generic_context_get_generic_class_inst(
        const Il2CppGenericContext *context) {
    if(context){
        return context->class_inst;
    }
    return nullptr;
}

const Il2CppTypeDefinition *
li2cppApi::cUnityApi::GetTypeDefinitionForIndex(TypeDefinitionIndex index) {
    if (index == kTypeDefinitionIndexInvalid)
        return NULL;

    IL2CPP_ASSERT(index >= 0 && static_cast<uint32_t>(index) < m_pGlobalMetadata->typeDefinitionsSize / sizeof(Il2CppTypeDefinition));
    const Il2CppTypeDefinition* typeDefinitions = (const Il2CppTypeDefinition*)((const char*)m_pGlobalMetadata + m_pGlobalMetadataHeader->typeDefinitionsOffset);
    return typeDefinitions + index;
}

Il2CppClass *li2cppApi::cUnityApi::il2cpp_field_get_parent(FieldInfo *field) {
    typedef Il2CppClass* (*pil2cpp_field_get_parent)(FieldInfo *field);
    static pil2cpp_field_get_parent m_ppil2cpp_field_get_parent = nullptr;
    if (m_ppil2cpp_field_get_parent == nullptr) {
        m_ppil2cpp_field_get_parent = (pil2cpp_field_get_parent)m_li2cppso->find(
                "il2cpp_field_get_parent");
        if (m_ppil2cpp_field_get_parent) {

        }
    }
    // 4. 调用原始函数
    if (m_ppil2cpp_field_get_parent) {
        return m_ppil2cpp_field_get_parent(field);
    }
    return nullptr; // 找不到函数时返回安全值 ""
}

void* li2cppApi::cUnityApi::il2cpp_class_init_all_method(Il2CppClass* klass){
    typedef void* (*pil2cpp_class_init_all_method)(Il2CppClass *klass);
    static pil2cpp_class_init_all_method m_ppil2cpp_class_init_all_method = nullptr;
    if (m_ppil2cpp_class_init_all_method == nullptr) {
        m_ppil2cpp_class_init_all_method = (pil2cpp_class_init_all_method)m_li2cppso->find(
                "il2cpp_class_init_all_method");
        if (m_ppil2cpp_class_init_all_method) {

        }
    }
    // 4. 调用原始函数
    if (m_ppil2cpp_class_init_all_method) {
        return m_ppil2cpp_class_init_all_method(klass);
    }
    return nullptr; // 找不到函数时返回安全值 ""
}

// =========================================================================
// il2cpp_type_get_object: 将 Il2CppType* 转换为 System.Type 反射对象
// =========================================================================
Il2CppReflectionType* li2cppApi::cUnityApi::il2cpp_type_get_object(const Il2CppType* type) {
    typedef Il2CppReflectionType* (*pfn)(const Il2CppType*);
    static pfn s_fn = nullptr;
    if (s_fn == nullptr) {
        s_fn = (pfn)m_li2cppso->find("il2cpp_type_get_object");
    }
    if (s_fn) {
        return s_fn(type);
    }
    return nullptr;
}

// =========================================================================
// il2cpp_runtime_invoke: 通过 MethodInfo 调用方法
// =========================================================================
Il2CppObject* li2cppApi::cUnityApi::il2cpp_runtime_invoke(const MethodInfo* method, void* obj, void** params, Il2CppException** exc) {
    typedef Il2CppObject* (*pfn)(const MethodInfo*, void*, void**, Il2CppException**);
    static pfn s_fn = nullptr;
    if (s_fn == nullptr) {
        s_fn = (pfn)m_li2cppso->find("il2cpp_runtime_invoke");
    }
    if (s_fn) {
        return s_fn(method, obj, params, exc);
    }
    return nullptr;
}

#ifdef OBFU_ATTRS_END
OBFU_ATTRS_END
#endif
