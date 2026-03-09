/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 */
#ifndef I_VIRUS_ENGINE_H
#define I_VIRUS_ENGINE_H

#include <cstdint>
#include <memory>
#include "virus_protection_service_define.h"

namespace OHOS::Security::VirusProtectionService {

struct VirusEngineConfig {
    bool allowVirusClear = true;
};

class IVirusEngine
{
public:
    virtual ~IVirusEngine() = default;

    virtual int32_t Init(const VirusEngineConfig *config = nullptr);
    virtual int32_t DeInit();
    virtual int32_t ScanFile(const ScanTask *scanTask, ScanResult *scanResult);

    virtual int32_t ScanBehavior(uint32_t accessToken, const std::string &event, const std::string &bundleName)
    {
        return SUCCESS;
    };

    virtual int32_t IsExistAnalysisEngine(uint32_t accessToken)
    {
        return SUCCESS;
    };

    virtual int32_t CreateAnalysisEngine(uint32_t accessToken)
    {
        return SUCCESS;
    };

    virtual int32_t DestroyAnalysisEngine(uint32_t accessToken)
    {
        return SUCCESS;
    };

    virtual int32_t RegisterScanResultListener(std::shared_ptr<ScanResultListener> &listener)
    {
        return SUCCESS;
    };

    virtual int32_t UnRegisterScanResultListener(std::shared_ptr<ScanResultListener> &listener)
    {
        return SUCCESS;
    };
};
}  // namespace OHOS::Security::VirusProtectionService
#endif  // I_VIRUS_ENGINE_H
