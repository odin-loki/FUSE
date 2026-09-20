#include "platform/platformNet.h"

NetSocket NetSocket::INVALID = NetSocket::fromHandle(-1);

Net::Error Net::sendto(const NetAddress* /*address*/, const U8* /*buffer*/, S32 /*bufferSize*/)
{
    return Net::NoError;
}
