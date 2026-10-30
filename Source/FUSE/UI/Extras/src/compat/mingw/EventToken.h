// mingw-w64 lacks the Windows SDK's EventToken.h, which WebView2.h includes; this is its whole content.
#pragma once
#ifndef __eventtoken_h__
#define __eventtoken_h__
typedef struct EventRegistrationToken { __int64 value; } EventRegistrationToken;
#endif
