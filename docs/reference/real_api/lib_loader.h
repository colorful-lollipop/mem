/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 */

#ifndef LIB_LOADER_H
#define LIB_LOADER_H

#include <string>
#include <dlfcn.h>

#include "virus_protection_service_define.h"
#include "i_virus_engine.h"

namespace OHOS::Security::VirusProtectionService {

class LibLoader {
public:
    explicit LibLoader(const std::string soPath);
    ~LibLoader();
    int32_t CreateVirusEngine(const VirusEngineConfig*);
    int32_t DestroyVirusEngine();
    IVirusEngine* GetVirusEngine() const;
private:
    using CallCreateVirusEngine = IVirusEngine* (*)(const VirusEngineConfig*);
    using CallDestroyVirusEngine = void (*)(IVirusEngine*);
    void* handle_{ nullptr };
    const std::string libPath_;
    IVirusEngine *virusEngine_{ nullptr };
};
}
#endif // LIB_LOADER_H
