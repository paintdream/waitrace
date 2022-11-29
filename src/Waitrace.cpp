#include "Waitrace.h"
#include "TraceSystem.h"
using namespace Waitrace;

#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "ntdll.lib")
#pragma comment(lib, "Rpcrt4.lib")

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
	return TraceSystem::GetInstance().DllMain(instance, reason, reserved);
}

