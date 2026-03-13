#ifndef MEMRPC_CORE_TASK_EXECUTOR_H_
#define MEMRPC_CORE_TASK_EXECUTOR_H_

#include <chrono>
#include <functional>

namespace MemRpc {

class TaskExecutor {
 public:
  virtual ~TaskExecutor() = default;

  // Submit returns false if the executor cannot accept new work.
  virtual bool TrySubmit(std::function<void()> task) = 0;
  // HasCapacity + TrySubmit must be consistent for a single producer.
  virtual bool HasCapacity() const = 0;
  virtual bool WaitForCapacity(std::chrono::milliseconds timeout) = 0;
  virtual void Stop() = 0;
};

}  // namespace MemRpc

#endif  // MEMRPC_CORE_TASK_EXECUTOR_H_
