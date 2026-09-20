// Minimal Torque::FS for FUSE_T3D_LEGACY_ENGINE_PROBE (fileStream.cpp).
// POSIX-backed OpenFile/CreatePath — no MountSystem/console closure.

#include "platform/platform.h"
#include "core/volume.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <vector>

namespace Torque
{
namespace FS
{

namespace
{

bool mkdirOne(const String& dir)
{
   if (dir.isEmpty()) {
      return true;
   }

   struct stat st {};
   if (stat(dir.c_str(), &st) == 0) {
      return S_ISDIR(st.st_mode);
   }

   return mkdir(dir.c_str(), 0755) == 0 || errno == EEXIST;
}

bool mkdirRecursive(const String& path)
{
   if (path.isEmpty()) {
      return true;
   }

   String cleaned = Path::CleanSeparators(path);
   if (cleaned.isEmpty()) {
      return true;
   }

   String built;
   const char* cursor = cleaned.c_str();
   if (cursor[0] == '/') {
      built = "/";
      ++cursor;
   }

   String segment;
   while (*cursor != '\0') {
      while (*cursor == '/') {
         ++cursor;
      }
      if (*cursor == '\0') {
         break;
      }

      segment.clear();
      while (*cursor != '\0' && *cursor != '/') {
         segment += *cursor;
         ++cursor;
      }

      if (built.isEmpty() || built.c_str()[built.length() - 1] != '/') {
         built = Path::Join(built, '/', segment);
      } else {
         built += segment;
      }

      if (!mkdirOne(built)) {
         return false;
      }
   }

   return true;
}

String parentDirectory(const Path& path)
{
   String full = path.getFullPath();
   const String::SizeType slash = full.find('/', 0, String::Right);
   if (slash == String::NPos) {
      return String();
   }
   if (slash == 0) {
      return String("/");
   }
   return full.substr(0, slash);
}

class PosixProbeFile : public File
{
public:
   explicit PosixProbeFile(const Path& path)
      : mPath(path), mHandle(nullptr), mStatus(Closed), mSize(0)
   {
   }

   Path getName() const override { return mPath; }

   NodeStatus getStatus() const override { return mStatus; }

   bool getAttributes(Attributes* attr) override
   {
      if (attr == nullptr) {
         return false;
      }

      struct stat st {};
      if (stat(mPath.getFullPath().c_str(), &st) != 0) {
         return false;
      }

      attr->flags = FileNode::File;
      attr->name = mPath.getFileName();
      attr->size = static_cast<U64>(st.st_size);
      return true;
   }

   U32 calculateChecksum() override { return 0; }

   U32 getPosition() override
   {
      if (mHandle == nullptr) {
         return 0;
      }
      return static_cast<U32>(std::ftell(mHandle));
   }

   U32 setPosition(U32 pos, SeekMode mode) override
   {
      if (mHandle == nullptr) {
         return 0;
      }

      int whence = SEEK_SET;
      switch (mode) {
         case Begin:
            whence = SEEK_SET;
            break;
         case Current:
            whence = SEEK_CUR;
            break;
         case End:
            whence = SEEK_END;
            break;
      }

      if (std::fseek(mHandle, static_cast<long>(pos), whence) != 0) {
         mStatus = UnknownError;
         return getPosition();
      }

      if (mode == End && pos == 0) {
         mStatus = EndOfFile;
      } else {
         mStatus = Open;
      }

      return getPosition();
   }

   bool open(AccessMode mode) override
   {
      close();

      const char* path = mPath.getFullPath().c_str();
      const char* fopenMode = "rb";
      switch (mode) {
         case Read:
            fopenMode = "rb";
            break;
         case Write:
            fopenMode = "wb";
            break;
         case WriteAppend:
            fopenMode = "ab";
            break;
         case ReadWrite:
            fopenMode = "rb+";
            break;
      }

      mHandle = std::fopen(path, fopenMode);
      if (mHandle == nullptr) {
         mStatus = NoSuchFile;
         return false;
      }

      if (std::fseek(mHandle, 0, SEEK_END) == 0) {
         mSize = static_cast<U64>(std::ftell(mHandle));
         std::fseek(mHandle, 0, SEEK_SET);
      }

      mStatus = Open;
      return true;
   }

   bool close() override
   {
      if (mHandle != nullptr) {
         std::fclose(mHandle);
         mHandle = nullptr;
      }
      mStatus = Closed;
      return true;
   }

   U32 read(void* dst, U32 size) override
   {
      if (mHandle == nullptr || dst == nullptr || size == 0) {
         return 0;
      }

      const size_t got = std::fread(dst, 1, size, mHandle);
      if (got < size && std::feof(mHandle)) {
         mStatus = EndOfFile;
      }
      return static_cast<U32>(got);
   }

   U32 write(const void* src, U32 size) override
   {
      if (mHandle == nullptr || src == nullptr || size == 0) {
         return 0;
      }

      const size_t wrote = std::fwrite(src, 1, size, mHandle);
      if (wrote > 0) {
         const U64 endPos = static_cast<U64>(std::ftell(mHandle));
         if (endPos > mSize) {
            mSize = endPos;
         }
      }
      return static_cast<U32>(wrote);
   }

   U64 getSize() override { return mSize; }

private:
   Path mPath;
   FILE* mHandle;
   NodeStatus mStatus;
   U64 mSize;
};

} // namespace

File::File() {}
File::~File() {}

FileNode::FileNode() : mChecksum(0) {}

Time FileNode::getModifiedTime()
{
   Attributes attrs;
   if (!getAttributes(&attrs)) {
      return Time();
   }
   return attrs.mtime;
}

Time FileNode::getCreatedTime()
{
   Attributes attrs;
   if (!getAttributes(&attrs)) {
      return Time();
   }
   return attrs.ctime;
}

U64 FileNode::getSize()
{
   Attributes attrs;
   if (!getAttributes(&attrs)) {
      return 0;
   }
   return attrs.size;
}

U32 FileNode::getChecksum()
{
   return 0;
}

FileRef OpenFile(const Path& path, File::AccessMode mode)
{
   FileRef file(new PosixProbeFile(path));
   if (!file->open(mode)) {
      return FileRef();
   }
   return file;
}

bool CreatePath(const Path& path)
{
   return mkdirRecursive(parentDirectory(path));
}

bool IsFile(const Path& path)
{
   struct stat st {};
   if (stat(path.getFullPath().c_str(), &st) != 0) {
      return false;
   }
   return S_ISREG(st.st_mode);
}

bool Remove(const Path& path)
{
   if (path.isEmpty()) {
      return false;
   }
   return std::remove(path.getFullPath().c_str()) == 0;
}

bool Rename(const Path& from, const Path& to)
{
   if (from.isEmpty() || to.isEmpty()) {
      return false;
   }
   return std::rename(from.getFullPath().c_str(), to.getFullPath().c_str()) == 0;
}

} // namespace FS
} // namespace Torque
