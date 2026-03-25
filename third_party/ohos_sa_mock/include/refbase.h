#ifndef OHOS_SA_MOCK_REFBASE_H
#define OHOS_SA_MOCK_REFBASE_H

#include <memory>

namespace OHOS {

template <typename T>
using sptr = std::shared_ptr<T>;

template <typename T>
using wptr = std::weak_ptr<T>;

class RefBase : public std::enable_shared_from_this<RefBase> {
public:
    virtual ~RefBase() = default;
};

}  // namespace OHOS

#endif  // OHOS_SA_MOCK_REFBASE_H
