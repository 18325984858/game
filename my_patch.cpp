
bool lol::FEVisi::checkWallCollision(const UnityVector3& pos, float worldDirX, float worldDirZ, float stepSize) {
    if (!m_pfunctionInfo) return false;

    static void* s_FixNavMeshEdInst = nullptr;
    static const MethodInfo* s_RaycastMethod = nullptr;
    static bool s_initialized = false;
    static double s_divideOfOne = 1.0 / 4294967296.0;

    if (!s_initialized) {
        s_initialized = true;

        typedef void* (*il2cpp_object_new_t)(void* klass);
        auto il2cpp_object_new_fn = (il2cpp_object_new_t)m_pfunctionInfo->il2cpp_api_lookup_symbol("il2cpp_object_new");

        auto Fix64Class = (::Il2CppClass*)m_pfunctionInfo->FindClassByName("ilbil2cpp.so", "Assembly-CSharp.dll", "Fix64");
        if (Fix64Class) {
            void* iter = nullptr;
            while (auto* field = m_pfunctionInfo->il2cpp_class_get_fields(Fix64Class, &iter)) {
                if (strcmp(m_pfunctionInfo->il2cpp_field_get_name(field), "divideOfOne") == 0) {
                    float divideVal = 0.0f;
                    m_pfunctionInfo->il2cpp_field_static_get_value(field, &divideVal);
                    if (divideVal != 0.0f) {
                        s_divideOfOne = divideVal;
                    }
                    break;
                }
            }
        }

        auto builderAssetClass = (::Il2CppClass*)m_pfunctionInfo->FindClassByName("ilbil2cpp.so", "Assembly-CSharp.dll", "FixBuilderAsset");
        auto ObjectClass = (::Il2CppClass*)m_pfunctionInfo->FindClassByName("ilbil2cpp.so", "UnityEngine.CoreModule.dll", "Object");
        auto ResourcesClass = (::Il2CppClass*)m_pfunctionInfo->FindClassByName("ilbil2cpp.so", "UnityEngine.CoreModule.dll", "Resources");

        if (builderAssetClass && ObjectClass && ResourcesClass && il2cpp_object_new_fn) {
            const MethodInfo* findObjectsOfTypeAll = nullptr;
            void* iter = nullptr;
            while (auto* method = m_pfunctionInfo->il2cpp_class_get_methods(ResourcesClass, &iter)) {
                if (strcmp(m_pfunctionInfo->il2cpp_method_get_name(method), "FindObjectsOfTypeAll") == 0 && m_pfunctionInfo->il2cpp_method_get_param_count(method) == 1) {
                    findObjectsOfTypeAll = method;
                    break;
                }
            }

            if (findObjectsOfTypeAll) {
                auto typeObject = m_pfunctionInfo->il2cpp_type_get_object(m_pfunctionInfo->il2cpp_class_get_type(builderAssetClass));
                void* args[1] = { typeObject };
                void* exc = nullptr;
                auto assetsArray = (void**)m_pfunctionInfo->il2cpp_runtime_invoke(findObjectsOfTypeAll, nullptr, args, &exc);

                if (assetsArray && !exc) {
                    uint32_t length = *(uint32_t*)((uintptr_t)assetsArray + 0x18);
                    if (length > 0) {
                        void* builderAssetInst = *(void**)((uintptr_t)assetsArray + 0x20); // 取第一个元素
                        if (builderAssetInst) {
                            uint32_t arrOffset = 0xFFFFFFFF;
                            m_pfunctionInfo->GetStaticMember("ilbil2cpp.so", "Assembly-CSharp.dll", "FixBuilderAsset", "", "navmeshBuilderData", &arrOffset);
                            if (arrOffset == 0xFFFFFFFF) arrOffset = 0xa0; // Fallback
                            
                            void* navmeshBuilderDataArr = *(void**)((uintptr_t)builderAssetInst + arrOffset);
                            if (navmeshBuilderDataArr) {
                                uint32_t dataLength = *(uint32_t*)((uintptr_t)navmeshBuilderDataArr + 0x18);
                                if(dataLength > 0) {
                                    void* firstBuilderData = *(void**)((uintptr_t)navmeshBuilderDataArr + 0x20);
                                    if (firstBuilderData) {
                                        auto builderDataClass = (::Il2CppClass*)m_pfunctionInfo->FindClassByName("ilbil2cpp.so", "Assembly-CSharp.dll", "FixNavmeshBuilderData");       
                                        uint32_t rawDataOffset = 0xFFFFFFFF;
                                        m_pfunctionInfo->GetStaticMember("ilbil2cpp.so", "Assembly-CSharp.dll", "FixNavmeshBuilderData", "", "navmeshRawData", &rawDataOffset);
                                        if (rawDataOffset == 0xFFFFFFFF) rawDataOffset = 0x20;

                                        void* rawData = *(void**)((uintptr_t)firstBuilderData + rawDataOffset);

                                        auto navMeshEdClass = (::Il2CppClass*)m_pfunctionInfo->FindClassByName("ilbil2cpp.so", "Assembly-CSharp.dll", "FixNavMeshEd");
                                        if (navMeshEdClass && rawData) {
                                            s_FixNavMeshEdInst = il2cpp_object_new_fn(navMeshEdClass);

                                            const MethodInfo* ctor = nullptr;
                                            const MethodInfo* initNavmeshEd = nullptr;
                                            void* methodIter = nullptr;
                                            while (auto* method = m_pfunctionInfo->il2cpp_class_get_methods(navMeshEdClass, &methodIter)) {
                                                const char* mName = m_pfunctionInfo->il2cpp_method_get_name(method);
                                                if (strcmp(mName, ".ctor") == 0 && m_pfunctionInfo->il2cpp_method_get_param_count(method) == 0) {
                                                    ctor = method;
                                                }
                                                else if (strcmp(mName, "InitNavmeshEd") == 0 && m_pfunctionInfo->il2cpp_method_get_param_count(method) == 1) {
                                                    initNavmeshEd = method;
                                                }
                                                else if (strcmp(mName, "Raycast") == 0 && m_pfunctionInfo->il2cpp_method_get_param_count(method) == 4) {
                                                    s_RaycastMethod = method;
                                                }
                                            }

                                            if (ctor) {
                                                void* excCtor = nullptr;
                                                m_pfunctionInfo->il2cpp_runtime_invoke(ctor, s_FixNavMeshEdInst, nullptr, &excCtor);
                                            }

                                            if (initNavmeshEd) {
                                                void* initArgs[1] = { rawData };
                                                void* excInit = nullptr;
                                                m_pfunctionInfo->il2cpp_runtime_invoke(initNavmeshEd, s_FixNavMeshEdInst, initArgs, &excInit);
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    if (!s_FixNavMeshEdInst || !s_RaycastMethod) {
        return false;
    }

    UnityVector3 tgtPos = pos;
    tgtPos.x += worldDirX * stepSize;
    tgtPos.z += worldDirZ * stepSize;

    struct FixVector3_Fix64 {
        int64_t x;
        int64_t y;
        int64_t z;
    };

    auto toFix64 = [](float val) -> int64_t {
         return (int64_t)((double)val / s_divideOfOne);
    };

    FixVector3_Fix64 srcFix;
    srcFix.x = toFix64(pos.x);
    srcFix.y = toFix64(pos.y);
    srcFix.z = toFix64(pos.z);

    FixVector3_Fix64 tgtFix;
    tgtFix.x = toFix64(tgtPos.x);
    tgtFix.y = toFix64(tgtPos.y);
    tgtFix.z = toFix64(tgtPos.z);

    int32_t navFlag = 1;

    struct FixNavHit {
        bool hit;
        FixVector3_Fix64 pos;
        FixVector3_Fix64 norm;
        bool hitCampNode;
    } hitResult;
    memset(&hitResult, 0, sizeof(hitResult));

    void* args[4];
    args[0] = &srcFix;
    args[1] = &tgtFix;
    args[2] = &navFlag;
    void* hitPtr = &hitResult; // out/ref 需要传指针的指针
    args[3] = &hitPtr;

    void* exc = nullptr;
    m_pfunctionInfo->il2cpp_runtime_invoke(s_RaycastMethod, s_FixNavMeshEdInst, args, &exc);

    if (exc) return false;

    return hitResult.hit;
}

bool lol::FEVisi::canMoveUp(float stepSize) { return !checkWallCollision(m_miniMapData.myWorldPos, 0.0f, 1.0f, stepSize); }
bool lol::FEVisi::canMoveDown(float stepSize) { return !checkWallCollision(m_miniMapData.myWorldPos, 0.0f, -1.0f, stepSize); }
bool lol::FEVisi::canMoveLeft(float stepSize) { return !checkWallCollision(m_miniMapData.myWorldPos, -1.0f, 0.0f, stepSize); }
bool lol::FEVisi::canMoveRight(float stepSize) { return !checkWallCollision(m_miniMapData.myWorldPos, 1.0f, 0.0f, stepSize); }

lol::SurroundingWalls lol::FEVisi::scanSurroundingObstacles(float stepSize) {
    SurroundingWalls result;
    result.stepSize = stepSize;
    if (m_miniMapData.myWorldPos.x == 0.0f && m_miniMapData.myWorldPos.y == 0.0f && m_miniMapData.myWorldPos.z == 0.0f) {
        result.valid = false;
        return result;
    }
    result.valid = true;
    const auto& pos = m_miniMapData.myWorldPos;
    result.up    = checkWallCollision(pos,  0.0f,  1.0f, stepSize);
    result.down  = checkWallCollision(pos,  0.0f, -1.0f, stepSize);
    result.left  = checkWallCollision(pos, -1.0f,  0.0f, stepSize);
    result.right = checkWallCollision(pos,  1.0f,  0.0f, stepSize);
    return result;
}

OBFU_ATTRS_END
