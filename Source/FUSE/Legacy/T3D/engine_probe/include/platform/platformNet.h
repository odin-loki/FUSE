#ifndef _PLATFORM_PLATFORMNET_H_
#define _PLATFORM_PLATFORMNET_H_

#ifndef _TORQUE_TYPES_H_
#include "platform/types.h"
#endif
#ifndef _RAWDATA_H_
#include "core/util/rawData.h"
#endif

#ifndef MAXPACKETSIZE
#define MAXPACKETSIZE 1500
#endif

typedef S32 NetConnectionId;

struct NetAddress
{
   S32 type;

   enum Type
   {
      IPAddress,
      IPV6Address,
      IPBroadcastAddress,
      IPV6MulticastAddress
   };

   union
   {
      struct
      {
         U8 netNum[4];
      } ipv4;

      struct
      {
         U8 netNum[16];
         U32 netFlow;
         U32 netScope;
      } ipv6;

      struct
      {
         U8 netNum[16];
         U8 netFlow[4];
         U8 netScope[4];
      } ipv6_raw;

   } address;

   U16 port;

   U32 getHash() const { return 0; }
};

class NetSocket
{
protected:
   S32 mHandle;

public:
   NetSocket() : mHandle(-1) {}

   inline void setHandle(S32 handleId) { mHandle = handleId; }
   inline S32 getHandle() const { return mHandle; }
   inline U32 getHash() const { return static_cast<U32>(mHandle); }

   bool operator==(const NetSocket& other) const { return mHandle == other.mHandle; }
   bool operator!=(const NetSocket& other) const { return mHandle != other.mHandle; }

   static NetSocket fromHandle(S32 handleId)
   {
      NetSocket ret;
      ret.mHandle = handleId;
      return ret;
   }

   static NetSocket INVALID;
};

struct Net
{
    enum Error
    {
        NoError,
        WrongProtocolType,
        InvalidPacketProtocol,
        WouldBlock,
        NotASocket,
        UnknownError,
        NeedHostLookup
    };

    static const S32 MaxPacketDataSize = MAXPACKETSIZE;

    static Error sendto(const NetAddress* address, const U8* buffer, S32 bufferSize);
};

#endif // _PLATFORM_PLATFORMNET_H_
