#ifndef _THREADPOOL_H_
#define _THREADPOOL_H_

#ifndef _TORQUE_TYPES_H_
#include "platform/types.h"
#endif
#ifndef _TORQUE_STRING_H_
#include "core/util/str.h"
#endif
#ifndef _THREADSAFEREFCOUNT_H_
#include "platform/threads/threadSafeRefCount.h"
#endif

class ThreadPool
{
public:
   class Context
   {
   public:
      Context(const char* /*name*/, Context* /*parent*/, F32 /*priorityBias*/) {}
      static Context* ROOT_CONTEXT() { return nullptr; }
   };

   class WorkItem : public ThreadSafeRefCount<WorkItem>
   {
   public:
      typedef ThreadSafeRefCount<WorkItem> Parent;

      WorkItem(Context* /*context*/ = nullptr) : mExecuted(false) {}

      virtual ~WorkItem() {}

      void process();
      virtual void execute() = 0;

   protected:
      bool mExecuted;
   };

   ThreadPool(const char* name, U32 numThreads = 0);
   ~ThreadPool();

   void queueWorkItem(WorkItem* item);

   static ThreadPool& GLOBAL();

private:
   String mName;
};

#endif // !_THREADPOOL_H_
