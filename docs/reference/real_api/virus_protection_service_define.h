/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 */
#ifndef VIRUS_PROTECTION_SERVICE_DEFINE_H
#define VIRUS_PROTECTION_SERVICE_DEFINE_H

#include <string>
#include <unordered_set>
#include "cJSON.h"

namespace OHOS::Security::VirusProtectionService {

inline constexpr int32_t VIRUS_PROTECTION_SERVICE_MANAGER_SA_ID = 1250;
inline constexpr int32_t VIRUS_PROTECTION_EXECUTOR_SA_ID = 1251;
inline constexpr size_t DEFAULT_CERT_NUM = 3;
inline constexpr int32_t CACHE_FILE_PERMISSION = 0600;
inline const std::string DATA_SERVICE_EL2 = "/data/service/el2/";
inline const std::string ISOLATION_PATH_SUFFIX = "/virus_protection_service/isolation/";
inline const std::string ISOLATION_SUFFIX = ".quarantine";
inline const std::string BACK_UP_SUFFIX = ".bak";
inline const std::string FEATURE_ROOT_DIR = "/data/service/el1/public/virus_protection_service/";
inline const std::string QOWL_FEATURE_PATH = "/data/service/el1/public/virus_protection_service/qowl_static_engine/";
inline const std::string QOWL_FEATURE_PATH_ABANDONED =
    "/data/service/el1/public/virus_protection_service/qowl_static_engine_abandoned/";
inline const std::string QOWL_FEATURE_OUT_PATH =
    "/data/service/el1/public/virus_protection_service/qowl_static_engine_tmp";
inline const std::string QOWL_RELEASE_NOTE_PATH =
    "/data/service/el1/public/virus_protection_service/releaseNoteFile.json";
inline const std::string ALLOW_LIST_PATH = "/data/service/el1/public/virus_protection_service/allowlist.dat";
inline const std::string BLOCK_LIST_PATH = "/data/service/el1/public/virus_protection_service/blocklist.dat";
inline const std::string SECURITY_POLICY_PATH = "/data/service/el1/public/virus_protection_service/securitypolicy.dat";
inline const std::string STORAGE_ALIAS = "StorageAlias";
inline const std::string CSPL_STATIC_ENGINE = "cspl_static_engine";
inline const std::string CSPL_DYNAMIC_ENGINE = "cspl_dynamic_engine";
inline const std::string TRUSTONE_STATIC_ENGINE = "trustone_static_engine";
inline const std::string QOWL_STATIC_ENGINE = "qowl_static_engine";
inline const std::string ATCORE_STATIC_ENGINE = "atcore_static_engine";
inline const std::string ATCORE_FEATURE_DIR = FEATURE_ROOT_DIR + ATCORE_STATIC_ENGINE;
inline const std::string ATCORE_FEATURE_NAME = "updatePkg.zip";
inline const std::string BEHAVIOR_DETECTION_ENGINE = "huawei_behavior_engine";
inline const std::string BEHAVIOR_DETECTION_ROOT_DIR = FEATURE_ROOT_DIR + "behavior_detection/";
inline const std::string ACTIVE_CACHE_FILENAME = "active_cache.dat";
inline const std::string BACKUP_CACHE_FILENAME = "backup_cache.dat";
inline const std::string ACTIVE_CACHE_PATH = BEHAVIOR_DETECTION_ROOT_DIR + ACTIVE_CACHE_FILENAME;
inline const std::string BACKUP_CACHE_PATH = BEHAVIOR_DETECTION_ROOT_DIR + BACKUP_CACHE_FILENAME;
inline const std::string REAL_TIME_PROTECTION = "real_time_protection";
inline const std::string GLOBAL_SWITCH_CONFIGURATION = "global_switch_configuration";
inline const std::string SECURITY_POLICY = "securitypolicy";
inline const std::string ALLOW_LIST = "allowlist";
inline const std::string BLOCK_LIST = "blocklist";
inline const std::string SAMPLE_SUBMISSION = "sample_submission";
inline const std::string AGREEMENT_STATUS = "agreement_status";
inline const std::string JOIN_IMPROVEMENT_PROGRAM = "join_improvement_program";
inline const std::string TRUE_VALUE = "true";
inline const std::string FALSE_VALUE = "false";
inline const std::string SG_EVENT_VERSION = "1.0.0";
inline const std::string OS_INTEGRATION = "os_integration";
inline const std::string APP_GALLERY = "app_gallery";
inline const std::string EXIT_POWER_SAVING = "ohos.event.notification.vps.EXIT_POWER_SAVING";
inline const uint32_t NONCE_LEN = 12;
inline const uint32_t TAG_LEN = 16;
inline const uint32_t DEFAULT_CIPHERTEXT_LEN = 2048;
inline uint8_t VPS_APP_PK_ALIAS_DATA[] = "VPS_APP_PK_ALIAS";
inline uint32_t VPS_APP_PK_ALIAS_SIZE = 17;
inline constexpr int64_t SG_EVENT_ID = 0x02B001200;
inline constexpr int64_t SG_HANDLE_EVENT_ID = 0x02B001202;
inline constexpr int64_t SAMPLE_AUTO_UPLOAD_SUCCESS_INTERVAL = 5;
inline constexpr int64_t SAMPLE_AUTO_UPLOAD_FAILED_INTERVAL = 300;
inline constexpr int64_t SAMPLE_AUTO_UPLOAD_TIMEOUT = 600;
inline constexpr int64_t FEATURE_AUTO_DOWNLOAD_INTERVAL = 720;
inline constexpr int64_t FEATURE_UPDATE_CONN_HSDR_TIMEOUT = 10;
inline constexpr int64_t FEATURE_UPDATE_QUERY_WAIT_TIME_MAX = 20;
inline constexpr int64_t FEATURE_UPDATE_QUERY_WAIT_TIME_INIT = 5;
inline constexpr int64_t FEATURE_UPDATE_QUERY_WAIT_TIME_ADD = 10;
inline constexpr int64_t FEATURE_DOWNLOAD_TIMEOUT = 600;
inline constexpr int32_t FEATURE_DOWNLOAD_RETRY_TIMES = 5;
inline constexpr int64_t ACTIVATE_PUSH_TOKEN_TIMEOUT = 5;
inline constexpr int32_t RIGISTER_CALLBACK_WAIT_TIME = 2;
inline constexpr int32_t BACKUP_CALLBACK_WAIT_TIME = 60;
inline constexpr int32_t PRIVACY_CENTER_CALLBACK_WAIT_TIME = 5;
inline constexpr int32_t SECONDS_IN_ONE_DAY = 86400;
inline constexpr int32_t COMMON_USER_ID = 0;
inline constexpr int32_t AUTO_DEEP_SCAN_INTERVAL = 3600;
inline constexpr int32_t GLOBAL_CONFIGURATION_REPORT_INTERVAL = 3600;
inline constexpr uint32_t DEFAULT_MONITOR_TIME = 600000;
inline constexpr uint32_t MIN_MONITOR_TIME = 300000;
inline constexpr uint32_t MAX_MONITOR_TIME = 1800000;
inline constexpr uint64_t DEFAULT_CACHE_FILE_SIZE = 10 * 1024 * 1024;
inline constexpr uint64_t MIN_CACHE_FILE_SIZE = 1 * 1024 * 1024;
inline constexpr uint64_t MAX_CACHE_FILE_SIZE = 100 * 1024 * 1024;
inline constexpr uint32_t MAX_MONITOR_ITEMS = 1500;
inline constexpr uint32_t MAX_FILTER_LENGTH = 64;
inline constexpr uint32_t ANALYSIS_THRESHOLD_EXTENSION_PERCENT = 15;
inline constexpr uint32_t MAX_REALTIME_SCAN_FILE_SIZE = 128 * 1024 * 1024;
inline constexpr int32_t MAX_EVENT_DRIVER_SCAN_FILE_SIZE = 512 * 1024 * 1024;
inline constexpr size_t MAX_CUSTOM_SCAN_FILE_SIZE = 2048 * 1024 * 1024L;
inline constexpr size_t MAX_ROOT_KEY_SIZE = 1024 * 1024;
inline constexpr uint32_t MAX_ZIP_SIZE = 300 * 1024 * 1024;
inline constexpr uint32_t MAX_UNCOMPRESSED_SIZE = 500 * 1024 * 1024;
inline constexpr uint32_t MAX_FILE_COUNT = 300;
inline constexpr int32_t MAX_FILE_PATH = 4096;
inline constexpr int32_t VIRUS_RETENTION_DAYS = 60;
inline constexpr int32_t JIP_REMIND_MAX_TIMES = 3;
inline constexpr int32_t JIP_REMIND_INTERVAL = 7 * SECONDS_IN_ONE_DAY;
inline constexpr int32_t USER_ZERO = 0;

const std::vector<uint8_t> ZIP_HEADER_BYTES = {0x50, 0x4B, 0x03, 0x04};
const std::vector<uint8_t> ELF_HEADER_BYTES = {0x7F, 0x45, 0x4C, 0x46};
const std::unordered_set<std::string> VALID_DOWNLOADED_ITEMS = {CSPL_STATIC_ENGINE,
    CSPL_DYNAMIC_ENGINE,
    TRUSTONE_STATIC_ENGINE,
    QOWL_STATIC_ENGINE,
    ATCORE_STATIC_ENGINE,
    BEHAVIOR_DETECTION_ENGINE,
    SECURITY_POLICY,
    ALLOW_LIST,
    BLOCK_LIST};

enum JsErrCode : int32_t {
    JS_ERR_SUCCESS = 0,
    JS_ERR_NO_PERMISSION = 201,
    JS_ERR_NO_SYSTEMCALL = 202,
    JS_ERR_BAD_PARAM = 401,
    JS_ERR_SYS_ERR = 21200001,
};

enum ErrorCode : int32_t {
    SUCCESS = 0,
    FAILED = 1,
    NO_VIRUS_INFO_ERROR = 2,
    OPERATION_ERROR = 3,
    MALLOC_FAILED = 4,
    SKIP_SCAN = 5,
    NOT_FOUND = 6,
    NULL_OBJECT = 7,
    JSON_ERR = 8,
    INVALID_SCAN_TASK_TYPE = 9,
    EMPTY_PATH = 10,
    ERROR_UNKNOWN_ERROR = 11,
    ERROR_INVALID_PARAMS = 12,
    ERROR_DESCRIPTOR = 13,
    ERROR_NULL_POINTER = 14,
    UPDATE_ERROR = 15,
    INVALID_OPERATION = 16,
    MEMORY_COPY_ERR = 17,
    INVALID_INPUT = 18,
    VIRUS_INFO_INSERT_ERR = 19,
    LOCAL_FILE_LOAD_ERR = 20,
    PERMISSION_DENIED = 21,
    INVALID_DOWNLOADED_ITEM = 22,
    ERROR_MEMORY_ALLOCATION = 23,
    ERROR_HSDR_BUSY = 10001,
    ERROR_HSDR_BUILD_CLOUD_CHANNEL = 10002,
    ERROR_HSDR_COMMON_FAIL = 10003,
    ERROR_MALLOC_FAIL = 10004,
    ERROR_FETCH_USERID = 10005,
    ERROR_CONNECT_HSDR_ABILITY = 10006,
    ERROR_REQUEST_TIME_OUT = 10007,
    ERROR_NETWORK_UNAVAILABLE = 10008,
    ERROR_WRITE_PARCEL = 10009,
    ERROR_SERVER_REPLY = 10010,
    ERROR_SERVER_REPLY_INVALID_ARGUMENT = 10011,
    ERROR_SERVICE_THROTTLING = 10012,
    CJSON_CREATE_ERR = 20001,
    ERROR_JSON_FORMAT = 20002,
    ERROR_JSON_PARSE = 20003,
    ERROR_JSON_UNWANTED_VALUE = 20004,
    CJSON_PARSE_ERR = 20005,
    CJSON_GET_ERR = 20006,
    ERROR_BASE64_DECODE = 20007,
    FILE_OPEN_ERR = 30001,
    FILE_NOT_FOUND_ERR = 30002,
    FILE_READ_ERR = 30003,
    FILE_RECURSIVE_ERR = 30004,
    EMPTY_FILE_ERR = 30005,
    DLOPEN_LIB_ERR = 40001,
    DLSYM_LIB_ERR = 40002,
    DLCLOSE_LIB_ERR = 40003,
    SCAN_ENGINE_INTERNAL_ERR = 50001,
    SCAN_BAD_PARAM_ERR = 50002,
    SCAN_NULL_OBJECT = 50003,
    SCAN_IN_BLOCK_ALLOW_LIST = 50004,
    SCAN_ISOLATE_ERR = 50005,
    GET_BUNDLE_MGR_ERR = 60001,
    GET_BUNDLE_INFO_ERR = 60002,
    GET_BUNDLE_INFOS_ERR = 60003,
    GET_APPLICATION_INFO_ERR = 60004,
    STATIC_ENGINE_INIT_ERR = 70001,
    STATIC_ENGINE_DEINIT_ERR = 70002,
    STATIC_ENGINE_LOAD_FEATURE_LIB_ERR = 70003,
    STATIC_ENGINE_SCANNER_CREATE_ERR = 70004,
    STATIC_ENGINE_SCAN_FILE_ERR = 70005,
    STATIC_ENGINE_NOT_INIT_ERR = 70006,
    STATIC_ENGINE_NO_IDLE_SCANNER_ERR = 70007,
    FILE_UNZIP_ERR = 90001,
    HASH_MATCH_ERR = 90002,
    PREPARE_KEY_ERR = 90003,
    DECRYPT_WITH_KEY_ERR = 90004,
    FILE_RETRIEVE_TIMEOUT_ERR = 90005,
    QUERY_FEATURE_VERSION_ERR = 90006,
    ENABLE_REAL_TIME_MONITOR_ERR = 90007,
    ENABLE_EMERGENCY_PUSH_ERR = 90008,
    ENABLE_FEATURE_AUTO_UPDATE_ERR = 90009,
    ENABLE_SAMPLE_AUTO_SUBMIT_ERR = 90010,
    DISABLE_REAL_TIME_MONITOR_ERR = 90011,
    DISABLE_EMERGENCY_PUSH_ERR = 90012,
    DISABLE_FEATURE_AUTO_UPDATE_ERR = 90013,
    DISABLE_SAMPLE_AUTO_SUBMIT_ERR = 90014,
    REAL_TIME_UNOPENED = 90015,
    UPDATE_LOCAL_LIST_ERR = 90016,
    ENABLE_AUTO_DEEP_SCAN_ERR = 90017,
    DISABLE_AUTO_DEEP_SCAN_ERR = 90018,
    ENABLE_BEHAVIOR_DETECTION_ERR = 90019,
    DISABLE_BEHAVIOR_DETECTION_ERR = 90020,
    DYNAMIC_ENGINE_INIT_ERR = 100001,
    DYNAMIC_ENGINE_DEINIT_ERR = 100002,
    DYNAMIC_ENGINE_LOAD_FEATURE_LIB_ERR = 100003,
    DYNAMIC_ENGINE_SCANNER_CREATE_ERR = 100004,
    DYNAMIC_ENGINE_SCAN_BEHAVIOR_ERR = 100005,
    DYNAMIC_ENGINE_NOT_INIT_ERR = 100006,
    OWL_ENGINE_INIT_ERR = 110001,
    OWL_ENGINE_DEINIT_ERR = 110002,
    OWL_ENGINE_DLOPEN_LIB_ERR = 110003,
    OWL_ENGINE_DLSYM_LIB_ERR = 110004,
    OWL_ENGINE_CREATE_ERR = 110005,
    OWL_ENGINE_QUERY_ERR = 110006,
    OWL_ENGINE_NOT_INIT_ERR = 110007,
    OWL_INVALID_ARG_ERR = 110008,
    OWL_NEWOBJ_FAIL_ERR = 110009,
    OWL_NO_SCANNER_ERR = 110010,
    OWL_FILE_TYPE_NOT_SUPPORT_ERR = 110011,
    OWL_ARCHIVE_POOL_FULL_ERR = 110012,
    OWL_ARCHIVE_GET_ITEM_FAIL_ERR = 110013,
    OWL_ARCHIVE_OPEN_FAILED_ERR = 110014,
    OWL_ERROR_UNDEFINED_ERR = 110015,
    OWL_KR_NONE_ERR = 110016,
    OWL_KR_FAILED_ERR = 110017,
    OWL_KR_DELETE_FAILED_ERR = 110018,
    OWL_KR_COMPOUND_NOT_SUPPORT_ERR = 110019,
    OWL_KR_WRITE_OPEN_FAILED_ERR = 110020,
    OWL_KR_UNDEFINED_ERR = 110021,
    TRUSTONE_STATIC_ENGINE_INIT_ERR = 120001,
    TRUSTONE_STATIC_ENGINE_GET_FUNC_ERR = 120002,
    TRUSTONE_STATIC_ENGINE_DEINIT_ERR = 120003,
    TRUSTONE_STATIC_ENGINE_LOAD_LIB_ERR = 120004,
    TRUSTONE_STATIC_ENGINE_LOAD_FEATURE_LIB_ERR = 120005,
    TRUSTONE_STATIC_ENGINE_SCAN_FILE_ERR = 120006,
    TRUSTONE_STATIC_ENGINE_NOT_INIT_ERR = 120007,
    TRUSTONE_STATIC_ENGINE_SET_OPTION_ERR = 120008,
    TRUSTONE_STATIC_ENGINE_SCAN_RESULT_NULL = 120009,
    TEST_TASK_BUSY = 130000,
};

enum class VirusStatus : int32_t {
    VIRUS_STATUS_UNHANDLED = 0,
    VIRUS_STATUS_ISOLATED = 1,
    VIRUS_STATUS_IGNORED = 2,
    VIRUS_STATUS_DELETED = 3,
    VIRUS_STATUS_CLEARED = 4,
    COUNT = 5
};

enum class VirusFeatureUpdateStatus : int32_t { UPDATE_SUCCEED = 0, UPDATE_FAILED = 1, UPDATING = 2, TO_UPDATE = 3 };
enum class ListType : int32_t { BLOCK = 0, ALLOW = 1, POLICY = 2 };

inline const std::string &ToString(const ListType type)
{
    static const std::unordered_map<ListType, const std::string> stringMap = {
        {ListType::ALLOW, "allowlist"},
        {ListType::BLOCK, "blocklist"},
        {ListType::POLICY, "securitypolicy"},
    };
    return stringMap.at(type);
}

enum class BlockAllowResult : int32_t { NOT_LISTED, IN_BLOCK_LIST, IN_ALLOW_LIST };
enum class ActionType : int32_t { NONE = 0, ISOLATE = 1, IGNORE = 2, DELETE = 3, CLEAR = 4, RESTORE = 5 };
enum class DisposeStatus : int32_t { PENDING = 0, ISOLATED = 1, IGNORED = 2, DELETED = 3, CLEARED = 4 };
enum class AuthorizedStatus : int32_t { NONE = 0, AGREE = 1, REFUSE = 2 };
enum class ScanStatus : int32_t { NONE = 0, END = 1, WORK = 2, PAUSE = 3 };
enum class ScanTaskType : int32_t { NONE = 0, DEEP = 1, QUICK = 2, CUSTOM = 3, IDLE = 4, REAL_TIME = 5, EVENT_DRIVEN = 6 };
enum class VirusEngine : int32_t {
    CSPL_STATIC_ENGINE = 0,
    CSPL_DYNAMIC_ENGINE = 1,
    TRUSTONE_STATIC_ENGINE = 2,
    QOWL_STATIC_ENGINE = 3,
    ATCORE_STATIC_ENGINE = 4,
    COUNT = 5,
};
enum class VirusType : int32_t {
    VIRUS_TYPE_TROJAN,
    VIRUS_TYPE_VIRUS,
    VIRUS_TYPE_WORM,
    VIRUS_TYPE_RANSOMWARE,
    VIRUS_TYPE_MINER,
    VIRUS_TYPE_SPYWARE,
    VIRUS_TYPE_ADWARE,
    VIRUS_TYPE_HACKTOOL,
    VIRUS_TYPE_EXPLOIT,
    VIRUS_TYPE_BOTNET,
    VIRUS_TYPE_DDOS,
    VIRUS_TYPE_PHISHING,
    VIRUS_TYPE_SCRIPT_THREAT,
    VIRUS_TYPE_ROOTKIT,
    VIRUS_TYPE_GRAYWARE,
    VIRUS_TYPE_TEST_SAMPLE,
    VIRUS_TYPE_ADVANCED_THREAT,
    VIRUS_TYPE_ARCHIVEBOMB,
    VIRUS_TYPE_UNKNOWN,
};
enum class ThreatLevel : int32_t { NONE = 0, NO_RISK = 1, LOW_RISK = 2, MEDIUM_RISK = 3, HIGH_RISK = 4, CLEAR_RISK = 5, CRASH_RISK = 6 };
enum class EventType : int32_t {
    FILE_EVENT = 0,
    APP_EVENT = 1,
    FEATURE_UPDATED_AND_SCAN = 2,
    PRIVACY_CENTER = 3,
    EMERGENCY_FEATURE_UPDATED = 4,
    VIRUS_INFO_UPDATED = 5,
    QUICK_SCAN = 6,
    RESTART_VPS = 7,
    SCREEN_ON = 8,
    SCREEN_OFF = 9,
    SCREEN_LOCKED = 10,
    SCREEN_UNLOCKED = 11,
    POWER_CONNECTED = 12,
    POWER_DISCONNECTED = 13,
    DB_NEED_BACKUP = 14,
    CONFIG_UPDATED = 15,
    USER_SWITCH = 16,
    DYNAMIC_FEATURE_UPDATE = 17,
    FEATURE_UPDATED = 18,
    DYNAMIC_FEATURE_APP_INSPECT = 19,
    POWER_SAVING_ACTION = 20,
    PACKAGE_REMOVED = 21,
};
enum class SwitchType : int32_t { REAL_TIME_PROTECTION = 0, SAMPLE_SUBMISSION = 1, UNDEFINED = 2 };
enum class SwitchAction : int32_t { CLOSE = 0, OPEN = 1, UNDEFINED = 2 };
enum class CallerType : int32_t { NORMAL = 0, MDM = 1, ENHANCE = 2 };

struct BasicFileInfo {
    std::string filePath;
    std::string fileHash;
    std::string subFilePath;
    std::string subFileHash;
    int64_t inode;
    int64_t mtime;
    uint64_t fileSize;
};

struct BundleInfo {
    std::string bundleName;
    std::string appDistributionType;
    std::string versionName;
    std::string label;
    bool isEncrypted;
};

struct ScanTask {
    std::string bundleName{""};
    std::shared_ptr<BundleInfo> bundleInfo{nullptr};
    std::vector<std::shared_ptr<BasicFileInfo>> fileInfos;
    ScanTaskType scanTaskType;
    int32_t accountId{100};
    ScanTask() = default;
    explicit ScanTask(ScanTaskType type, int32_t inputAccountId) : scanTaskType(type), accountId(inputAccountId) {}
    explicit ScanTask(std::string name, std::shared_ptr<BundleInfo> info,
        std::vector<std::shared_ptr<BasicFileInfo>> files, ScanTaskType type, int32_t inputAccountId)
        : bundleName(std::move(name)), bundleInfo(info), fileInfos(std::move(files)), scanTaskType(type),
          accountId(inputAccountId) {}
};

struct EngineResult {
    std::string virusName;
    std::string virusType;
    std::string errorMsg;
    int32_t errorCode;
    ThreatLevel level;
};

struct MDEResult {
    int32_t fileType;
    EngineResult engineResult{};
};

struct ScanResult {
    std::string bundleName;
    std::shared_ptr<BundleInfo> bundleInfo;
    std::vector<std::shared_ptr<BasicFileInfo>> fileInfos;
    std::string bakPath;
    ThreatLevel threatLevel;
    ScanTaskType scanTaskType;
    int32_t accountId;
    std::vector<EngineResult> engineResults{std::vector<EngineResult>(static_cast<int32_t>(VirusEngine::COUNT))};
    ScanResult() = default;
    explicit ScanResult(const std::string &inputName, std::shared_ptr<BundleInfo> inputInfo,
        std::vector<std::shared_ptr<BasicFileInfo>> files, ThreatLevel inputThreatLevel, ScanTaskType inputScanType,
        int32_t inputAccountId)
        : bundleName(inputName), bundleInfo(inputInfo), fileInfos(std::move(files)), threatLevel(inputThreatLevel),
          scanTaskType(inputScanType), accountId(inputAccountId) {}
};

struct ConfigurationSetEvent {
    std::string switchType;
    std::string action;
    int32_t accountId;
    int32_t errorCode;
};

struct VirusScanEvent {
    std::string scanPath;
    ScanTaskType scanType;
    uint32_t fileCount;
    uint32_t virusCount;
    int32_t accountId;
    int32_t errorCode;
    int64_t scanDuration;
};

struct VirusHandleEvent {
    std::string fileName;
    std::string fileHash;
    int32_t handleType;
    int32_t accountId;
    int32_t handleResult;
};

struct FeatureUpdateEvent {
    std::string featureLibraryId;
    std::string featureLibrary;
    int32_t accountId;
    int32_t featureUpdateResult;
};

struct RealTimeStatisticsEvent {
    int32_t scanCount{0};
    uint64_t totalSize{0};
};

struct YaraCompileErrorEvent {
    int32_t errorLevel;
    int32_t errorLineNumber;
    std::string yaraRule;
    std::string engineType;
    std::string compileErrorMessage;
};

struct VirusFileInfo {
    std::string filePath;
    int64_t scanTime;
    std::string fileName;
    std::string fileHash;
};

struct BehaviorScanResult {
    std::string eventId;
    std::string time;
    std::string ruleName;
    std::string bundleName;
};

class ScanResultListener {
public:
    ScanResultListener() = default;
    virtual ~ScanResultListener() = default;
    virtual void OnReport(uint32_t accessToken, BehaviorScanResult &scanResult){};
};
}  // namespace OHOS::Security::VirusProtectionService
#endif  // VIRUS_PROTECTION_SERVICE_DEFINE_H
