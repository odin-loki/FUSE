#include "platform/threads/threadPool.h"

void ThreadPool::WorkItem::process()
{
   execute();
   mExecuted = true;
}

ThreadPool::ThreadPool(const char* name, U32 /*numThreads*/) : mName(name) {}

ThreadPool::~ThreadPool() {}

void ThreadPool::queueWorkItem(WorkItem* item)
{
   if (item != nullptr) {
      item->process();
   }
}

ThreadPool& ThreadPool::GLOBAL()
{
   static ThreadPool pool("GLOBAL");
   return pool;
}
