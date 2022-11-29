// Waitrace.h
// PaintDream (paintdream@paintdream.com)
// 2021-8-29
//

#pragma once
#pragma warning(disable:4316) // warning C4316: 'std::_Ref_count_obj2<_Ty>': object allocated on the heap may not be aligned 64
#pragma warning(disable:26495) // warning C26495: always initialize member

#include <windows.h>

#ifdef min
#undef min
#endif

#ifdef max
#undef max
#endif

namespace Waitrace {
	typedef void (*WaitCallback)(BOOL preCall, HANDLE* objectHandles, DWORD handleCount, PVOID address, PLARGE_INTEGER timeout, const char* hookedApi, PVOID context);
}
