#ifndef OHOS_SA_MOCK_ERRORS_H
#define OHOS_SA_MOCK_ERRORS_H

#include <cstdint>

namespace OHOS {

using ErrCode = int32_t;

constexpr ErrCode ERR_OK = 0;
constexpr ErrCode ERR_INVALID_VALUE = -1;
constexpr ErrCode ERR_NULL_OBJECT = -2;
constexpr ErrCode ERR_NAME_NOT_FOUND = -3;

}  // namespace OHOS

#endif  // OHOS_SA_MOCK_ERRORS_H
