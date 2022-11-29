#include "TraceSystem.h"
#include <winternl.h>
#include <Shlwapi.h>
#include <TlHelp32.h>
#include <stdio.h>
#include <cassert>

using namespace Waitrace;

// Constants
static constexpr size_t PROXY_LENGTH = 0x1000;
static constexpr size_t TICK_INTERVAL = 16;
static constexpr size_t VISUALIZE_DATA_RESERVE_LENGTH = 16 * 1024;
static constexpr DWORD SHARED_RECORD_INFO_BUFFER_LENGTH = 64 * 1024; // 1.0 MB on win64
static constexpr DWORD SPINLOCK_IDLE = 0;
static constexpr DWORD SPINLOCK_LOCKING = 1;
static constexpr DWORD SPINLOCK_COMPLETE = 2;
static constexpr size_t MAX_HELPER_THREAD_COUNT = 2;
static constexpr size_t PRIORITY_HIGHEST = 0;
static constexpr size_t PRIORITY_NORMAL = 1;
static constexpr size_t PRIORITY_LOW = 2;
static constexpr const char* ObjectTypes[] = {
	"Other", "Event", "Mutex", "Semaphore", "Process", "Thread", "File", "IoCompletionPort", "Address"
};

#ifdef _WIN64
static constexpr size_t NATIVE_API_LENGTH = 0x20;
#else
static constexpr size_t NATIVE_API_LENGTH = 0x10;
#endif

// Shared memory
#pragma data_seg(".shared")
std::atomic<DWORD> SharedRecordWriteIndex = 0;
std::atomic<DWORD> SharedRecordMemoryLock = 0;
UUID SharedRecordMemoryGuid = {};
#pragma data_seg()
#pragma comment(linker, "/SECTION:.shared,RWS")

// Global callbacks

static NTSTATUS (WINAPI *NtCreateThreadEx)(
  OUT PHANDLE hThread,
  IN ACCESS_MASK DesiredAccess,
  IN LPVOID ObjectAttributes,
  IN HANDLE ProcessHandle,
  IN LPTHREAD_START_ROUTINE lpStartAddress,
  IN LPVOID lpParameter,
  IN BOOL CreateSuspended,
  IN ULONG StackZeroBits,
  IN ULONG SizeOfStackCommit,
  IN ULONG SizeOfStackReserve,
  OUT LPVOID lpBytesBuffer
);

typedef struct _NtCreateThreadExBuffer
{
  ULONG Size;
  ULONG Unknown1;
  ULONG Unknown2;
  PULONG Unknown3;
  ULONG Unknown4;
  ULONG Unknown5;
  ULONG Unknown6;
  PULONG Unknown7;
  ULONG Unknown8;
} NtCreateThreadExBuffer;

typedef struct _LDR_DLL_LOADED_NOTIFICATION_DATA {
	ULONG Flags;                    //Reserved.
	PCUNICODE_STRING FullDllName;   //The full path name of the DLL module.
	PCUNICODE_STRING BaseDllName;   //The base file name of the DLL module.
	PVOID DllBase;                  //A pointer to the base address for the DLL in memory.
	ULONG SizeOfImage;              //The size of the DLL image, in bytes.
} LDR_DLL_LOADED_NOTIFICATION_DATA, * PLDR_DLL_LOADED_NOTIFICATION_DATA;

typedef struct _LDR_DLL_UNLOADED_NOTIFICATION_DATA {
	ULONG Flags;                    //Reserved.
	PCUNICODE_STRING FullDllName;   //The full path name of the DLL module.
	PCUNICODE_STRING BaseDllName;   //The base file name of the DLL module.
	PVOID DllBase;                  //A pointer to the base address for the DLL in memory.
	ULONG SizeOfImage;              //The size of the DLL image, in bytes.
} LDR_DLL_UNLOADED_NOTIFICATION_DATA, * PLDR_DLL_UNLOADED_NOTIFICATION_DATA;

typedef union _LDR_DLL_NOTIFICATION_DATA {
	LDR_DLL_LOADED_NOTIFICATION_DATA Loaded;
	LDR_DLL_UNLOADED_NOTIFICATION_DATA Unloaded;
} LDR_DLL_NOTIFICATION_DATA, * PLDR_DLL_NOTIFICATION_DATA;

static void CALLBACK LdrDllNotification(ULONG notificationReason, PLDR_DLL_NOTIFICATION_DATA notificationData, PVOID context) {
	return TraceSystem::GetInstance().LdrDllNotification(notificationReason, notificationData, context);
}

static NTSTATUS NTAPI ProxyNtWaitForSingleObject(HANDLE handle, BOOLEAN alertable, PLARGE_INTEGER timeout) {
	return TraceSystem::GetInstance().ProxyNtWaitForSingleObject(handle, alertable, timeout);
}

static NTSTATUS NTAPI ProxyNtWaitForMultipleObjects(DWORD count, HANDLE* handles, DWORD waitType, BOOL alertable, PLARGE_INTEGER timeout) {
	return TraceSystem::GetInstance().ProxyNtWaitForMultipleObjects(count, handles, waitType, alertable, timeout);
}

static NTSTATUS NTAPI ProxyNtWaitForAlertByThreadId(PVOID address, ULONG_PTR threadId) {
	return TraceSystem::GetInstance().ProxyNtWaitForAlertByThreadId(address, threadId);
}

static NTSTATUS NTAPI ProxyNtRemoveIoCompletion(HANDLE completionPort, PULONG_PTR completionKey, LPOVERLAPPED* overlapped, PVOID result) {
	return TraceSystem::GetInstance().ProxyNtRemoveIoCompletion(completionPort, completionKey, overlapped, result);
}

static NTSTATUS NTAPI ProxyNtSignalAndWaitForSingleObject(HANDLE handleToSignal, HANDLE handleToWaitOn, BOOL alertable, PLARGE_INTEGER timeout) {
	return TraceSystem::GetInstance().ProxyNtSignalAndWaitForSingleObject(handleToSignal, handleToWaitOn, alertable, timeout);
}

static NTSTATUS NTAPI ProxyNtAlertThreadByThreadId(DWORD threadId) {
	return TraceSystem::GetInstance().ProxyNtAlertThreadByThreadId(threadId);
}

static NTSTATUS NTAPI ProxyNtSetIoCompletion(HANDLE completionPort, PVOID keyContext, PVOID apcContext, NTSTATUS ioStatus, ULONG completionInformation) {
	return TraceSystem::GetInstance().ProxyNtSetIoCompletion(completionPort, keyContext, apcContext, ioStatus, completionInformation);
}

static NTSTATUS NTAPI ProxyNtReleaseSemaphore(HANDLE semaphoreHandle, LONG releaseCount, PLONG previousCount) {
	return TraceSystem::GetInstance().ProxyNtReleaseSemaphore(semaphoreHandle, releaseCount, previousCount);
}

static NTSTATUS NTAPI ProxyNtSetEvent(HANDLE eventHandle, PLONG previousState) {
	return TraceSystem::GetInstance().ProxyNtSetEvent(eventHandle, previousState);
}

static NTSTATUS NTAPI ProxyNtReleaseMutant(HANDLE mutantHandle, PLONG releaseCount) {
	return TraceSystem::GetInstance().ProxyNtReleaseMutant(mutantHandle, releaseCount);
}

static inline LARGE_INTEGER GetTimestamp() {
	LARGE_INTEGER count;
	::QueryPerformanceCounter(&count);
	return count;
}

static void AtomicWriteMemory(PVOID source, const char* data) {
#ifdef _WIN64
	volatile LONG64* address = reinterpret_cast<volatile LONG64*>(source);
	LONG64 result[2] = { address[0], address[1] };
	while (!::InterlockedCompareExchange128(address, *reinterpret_cast<const LONG64*>(data + 8), *reinterpret_cast<const LONG64*>(data), result));
#else
	::InterlockedExchange64(reinterpret_cast<volatile LONG64*>(source), *reinterpret_cast<const LONG64*>(data));
#endif
}

static void InlineHookNativeApi(PVOID source, PVOID target, PVOID stub) {
	std::memcpy(stub, source, NATIVE_API_LENGTH);

	DWORD oldProtect;
	::VirtualProtect(source, NATIVE_API_LENGTH, PAGE_EXECUTE_READWRITE, &oldProtect);

#ifdef _WIN64
	// mov rax, $target
	// jmp rax
	char opcodes[16] = "\x48\xB8\x00\x00\x00\x00\x00\x00\x00\x00\xFF\xE0";
	*reinterpret_cast<PVOID*>(opcodes + 2) = target;
#else
	// jmp $target
	char opcodes[8] = "\xe9\x00\x00\x00\x00";
	*reinterpret_cast<DWORD*>(opcodes + 1) = (DWORD)target - (DWORD)source - 5;
#endif

	// std::memcpy(source, opcodes, sizeof(opcodes));
	AtomicWriteMemory(source, opcodes);
	::VirtualProtect(source, NATIVE_API_LENGTH, oldProtect, &oldProtect);
}

static void UnlineHookNativeApi(PVOID source, PVOID target, PVOID stub) {
	DWORD oldProtect;
	::VirtualProtect(source, NATIVE_API_LENGTH, PAGE_EXECUTE_READWRITE, &oldProtect);
	AtomicWriteMemory(source, reinterpret_cast<const char*>(stub));
	::VirtualProtect(source, NATIVE_API_LENGTH, oldProtect, &oldProtect);
}

extern "C" __declspec(dllexport) void SetWaitCallback(WaitCallback waitCallback, PVOID context) {
	TraceSystem::GetInstance().SetWaitCallback(waitCallback, context);
}

extern "C" __declspec(dllexport) void WINAPI Run(HWND hWnd, HINSTANCE inst, LPTSTR cmdline, int cmdShow) {
#ifndef _M_X64
#pragma comment(linker, "/EXPORT:" __FUNCTION__ "=" __FUNCDNAME__)
#endif
	// wait for trace system ends
	TraceSystem::GetInstance().LoopHost();
}

// Implementations
// static thread_local bool ExemptingWait = false; // not available for attached threads
DWORD ExemptingWaitTLSIndex = 0;
DWORD ThreadActiveTLSIndex = 0;

struct ExemptGuard {
	ExemptGuard(const char* name) noexcept {
		previousWait = ::TlsGetValue(ExemptingWaitTLSIndex);
		::TlsSetValue(ExemptingWaitTLSIndex, const_cast<char*>(name));
	}

	operator bool() const {
		return previousWait == nullptr;
	}

	~ExemptGuard() {
		::TlsSetValue(ExemptingWaitTLSIndex, previousWait);
	}

private:
	PVOID previousWait = nullptr;
};

TraceSystem theSystem; // static TraceSystem requires TLS initialization
TraceSystem& TraceSystem::GetInstance() {
	return theSystem;
}

inline TraceSystem::Lifetime::Lifetime(size_t id) {
	static constexpr const char* name = "WorkerThread";
	// Do not mark me
	::TlsSetValue(ExemptingWaitTLSIndex, const_cast<char*>(name));
}

inline TraceSystem::Lifetime::~Lifetime() {}

TraceSystem::TraceSystem() : hookInfos{
	{ "NtWaitForAlertByThreadId", reinterpret_cast<void*>(&::ProxyNtWaitForAlertByThreadId), nullptr },
	{ "NtWaitForSingleObject", reinterpret_cast<void*>(&::ProxyNtWaitForSingleObject), nullptr },
	{ "NtRemoveIoCompletion", reinterpret_cast<void*>(&::ProxyNtRemoveIoCompletion), nullptr },
	{ "NtWaitForMultipleObjects", reinterpret_cast<void*>(&::ProxyNtWaitForMultipleObjects), nullptr },
	{ "NtSignalAndWaitForSingleObject", reinterpret_cast<void*>(&::ProxyNtSignalAndWaitForSingleObject), nullptr },
	{ "NtSetIoCompletion", reinterpret_cast<void*>(&::ProxyNtSetIoCompletion), nullptr },
	{ "NtReleaseSemaphore", reinterpret_cast<void*>(&::ProxyNtReleaseSemaphore), nullptr },
	{ "NtSetEvent", reinterpret_cast<void*>(&::ProxyNtSetEvent), nullptr },
	{ "NtReleaseMutant", reinterpret_cast<void*>(&::ProxyNtReleaseMutant), nullptr },
	{ "NtAlertThreadByThreadId", reinterpret_cast<void*>(&::ProxyNtAlertThreadByThreadId), nullptr },
}, worker(std::min(MAX_HELPER_THREAD_COUNT, (size_t)std::thread::hardware_concurrency())), dispatcher(worker), balancer(worker), threadInfoFrameView(threadInfoList) {
	ntdllHandle = ::GetModuleHandleA("ntdll.dll");
	::QueryPerformanceFrequency(&performanceFrequency);

	if (ntdllHandle != nullptr) {
		NtCreateThreadEx = decltype(NtCreateThreadEx)(::GetProcAddress(ntdllHandle, "NtCreateThreadEx"));
		for (size_t i = 0; i < hookInfos.size(); i++) {
			hookInfos[i].originalAddress = reinterpret_cast<void*>(::GetProcAddress(ntdllHandle, hookInfos[i].name));
		}
	}

	worker.append([this]() { MasterWorkerThread(); });
}

TraceSystem::~TraceSystem() {
	if (proxyMemory != nullptr) {
		Uninitialize();
	}
}

void TraceSystem::LoopHost() {
	assert(isHost);
	worker.join();
}

void TraceSystem::Terminate() {
	worker.terminate();
}

bool TraceSystem::InjectToProcess(DWORD processId) {
	bool ret = false;
	HANDLE handle = ::OpenProcess(PROCESS_ALL_ACCESS, FALSE, processId);
	if (handle != INVALID_HANDLE_VALUE) {
		WCHAR executablePath[MAX_PATH * 4] = {};
		::GetModuleFileNameW(moduleHandle, executablePath, sizeof(executablePath) / sizeof(WCHAR) - 1);
		size_t length = wcslen(executablePath) * 2 + 2;
		PVOID nameAddress = ::VirtualAllocEx(handle, nullptr, (DWORD)length, MEM_COMMIT, PAGE_READONLY);
		if (nameAddress != nullptr) {
			::WriteProcessMemory(handle, nameAddress, executablePath, (DWORD)length, nullptr);
			NtCreateThreadExBuffer ntbuffer;

			memset(&ntbuffer, 0, sizeof(NtCreateThreadExBuffer));
			DWORD temp1 = 0;
			DWORD temp2 = 0;

			ntbuffer.Size = sizeof(NtCreateThreadExBuffer);
			ntbuffer.Unknown1 = 0x10003;
			ntbuffer.Unknown2 = 0x8;
			ntbuffer.Unknown3 = &temp2;
			ntbuffer.Unknown4 = 0;
			ntbuffer.Unknown5 = 0x10004;
			ntbuffer.Unknown6 = 4;
			ntbuffer.Unknown7 = &temp1;
			ntbuffer.Unknown8 = 0;

			HANDLE thread;
			if (NT_SUCCESS(::NtCreateThreadEx(&thread, 0x1FFFFF, nullptr, handle,
				(LPTHREAD_START_ROUTINE)::GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryW"),
				nameAddress, false, 0, 0, 0, &ntbuffer))) {
				::CloseHandle(thread);
				ret = true;
			}
		}

		::CloseHandle(handle);
	}

	return false;
}

bool TraceSystem::EjectFromProcess(DWORD processId) {
	bool ret = false;
	HANDLE handle = ::OpenProcess(PROCESS_ALL_ACCESS, FALSE, processId);
	if (handle != INVALID_HANDLE_VALUE) {
		HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, processId);
		if (snapshot != nullptr) {
			MODULEENTRY32W me32 = { sizeof(MODULEENTRY32W) };

			if (::Module32FirstW(snapshot, &me32)) {
				WCHAR executablePath[MAX_PATH * 4] = {};
				::GetModuleFileNameW(moduleHandle, executablePath, sizeof(executablePath) / sizeof(WCHAR) - 1);

				do {
					if (_wcsicmp(me32.szModule, executablePath) == 0) {
						HMODULE module = me32.hModule;
						NtCreateThreadExBuffer ntbuffer;

						memset(&ntbuffer, 0, sizeof(NtCreateThreadExBuffer));
						DWORD temp1 = 0;
						DWORD temp2 = 0;

						ntbuffer.Size = sizeof(NtCreateThreadExBuffer);
						ntbuffer.Unknown1 = 0x10003;
						ntbuffer.Unknown2 = 0x8;
						ntbuffer.Unknown3 = &temp2;
						ntbuffer.Unknown4 = 0;
						ntbuffer.Unknown5 = 0x10004;
						ntbuffer.Unknown6 = 4;
						ntbuffer.Unknown7 = &temp1;
						ntbuffer.Unknown8 = 0;

						HANDLE thread;
						if (NT_SUCCESS(::NtCreateThreadEx(&thread, 0x1FFFFF, nullptr, handle,
							(LPTHREAD_START_ROUTINE)::GetProcAddress(GetModuleHandleA("kernel32.dll"), "FreeLibrary"),
							(PVOID)module, false, 0, 0, 0, &ntbuffer))) {
							::CloseHandle(thread);
							ret = true;
						}

						ret = true;
						break;
					}
				} while (::Module32NextW(snapshot, &me32));
			}

			::CloseHandle(snapshot);
		}

		::CloseHandle(handle);
	}

	return ret;
}

void TraceSystem::MasterWorkerThread() {
	ExemptGuard guard("MasterWorkerThread");
	// Copied from grid_dispatcher
	size_t thread_index = worker.get_thread_count() - 2;
	Worker::get_current() = &worker;
	Worker::get_current_thread_index_internal() = thread_index;

	Worker::task_lifetime_t live(thread_index);

	while (!worker.is_terminated()) {
		DWORD index = currentRecordInfoReadIndex;
		DWORD writeIndex = SharedRecordWriteIndex.load(std::memory_order_acquire);
		// catch up
		if (writeIndex - index > SHARED_RECORD_INFO_BUFFER_LENGTH) {
			Catchup();
			currentRecordInfoReadIndex = writeIndex - SHARED_RECORD_INFO_BUFFER_LENGTH / 2;
			continue;
		}

		DWORD rindex = index % SHARED_RECORD_INFO_BUFFER_LENGTH;
		RecordInfo recordInfo = sharedRecordInfos[rindex];
		std::atomic_thread_fence(std::memory_order_acquire);

		if (recordInfo.recordIndex == index) { // completed
			switch (recordInfo.action) {
				case Action::LibraryLoad:
					break;
				case Action::LibraryUnload:
					break;
				case Action::ThreadCreation:
					threadInfoMap[recordInfo.threadId] = std::make_shared<ThreadInfo>(worker, (DWORD)recordInfo.threadId);
					break;
				case Action::ThreadDeletion:
					threadInfoMap.erase(recordInfo.threadId);
					break;
				default:
				{
					LARGE_INTEGER timestamp = GetTimestamp();
					if (recordInfo.preCall) {
						UpdateWaitingObjectsThreadMap(recordInfo);

						auto it = threadInfoMap.find(recordInfo.threadId);
						assert(it != threadInfoMap.end());
						if (it != threadInfoMap.end()) {
							it->second->GetWarp().queue_routine_post([this, thread = it->second, info = std::move(recordInfo), timestamp]() mutable {
								thread->Record(std::move(info), timestamp);
							});
						}
					} else {
						ThreadIdArray threadIds = GetThreadsByWaitingObject(recordInfo);
						for (auto& id : threadIds) {
							auto it = threadInfoMap.find(id);
							assert(it != threadInfoMap.end());
							if (it != threadInfoMap.end()) {
								it->second->GetWarp().queue_routine_post([this, thread = it->second, info = std::move(recordInfo), timestamp]() mutable {
									thread->Record(std::move(info), timestamp);
								});
							}
						}
					}

					break;
				}
			}

			currentRecordInfoReadIndex++;
		} else {
			if (!worker.poll(PRIORITY_HIGHEST + 1u)) {
				worker.delay(TICK_INTERVAL);
			}
		}
	}
}

void TraceSystem::UpdateWaitingObjectsThreadMap(const RecordInfo& recordInfo) {
	switch (recordInfo.action) {
		case NtWaitForSingleObject:
		case NtRemoveIoCompletion:
		case NtWaitForMultipleObjects:
		{
			ThreadIdArray& threadIdArray = waitingObjectsThreadMap[recordInfo.handle];
			if (recordInfo.preCall) {
				grid::grid_binary_insert(threadIdArray, recordInfo.threadId);
			} else {
				if (grid::grid_binary_erase(threadIdArray, recordInfo.threadId)) {
					if (threadIdArray.empty()) {
						waitingObjectsThreadMap.erase(recordInfo.handle);
					}
				}
			}

			break;
		}
	}
}

TraceSystem::ThreadIdArray TraceSystem::GetThreadsByWaitingObject(const RecordInfo& recordInfo) {
	static ThreadIdArray emptyThreadIdArray;
	switch (recordInfo.action) {
		case NtSignalAndWaitForSingleObject:
		case NtSetIoCompletion:
		case NtReleaseSemaphore:
		case NtSetEvent:
		case NtReleaseMutant:
		{
			auto it = waitingObjectsThreadMap.find(recordInfo.handle);
			if (it != waitingObjectsThreadMap.end()) {
				return it->second.view();
			} else {
				return {};
			}
		}
		case NtAlertThreadByThreadId:
		{
			ThreadIdArray threadIdArray;
			threadIdArray.push(recordInfo.targetThreadId);
			return threadIdArray;
		}
		default:
		{
			return {};
		}
	}
}

BOOL TraceSystem::DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
	BOOL ret = TRUE;

	switch (reason) {
		case DLL_PROCESS_DETACH:
			Uninitialize();

			::TlsFree(ThreadActiveTLSIndex);
			::TlsFree(ExemptingWaitTLSIndex);
			break;
		case DLL_PROCESS_ATTACH:
			ExemptingWaitTLSIndex = ::TlsAlloc();
			ThreadActiveTLSIndex = ::TlsAlloc();
			moduleHandle = instance;

			ret = Initialize();
			break;
		case DLL_THREAD_ATTACH:
			AttachThread(::GetCurrentThreadId());
			break;
		case DLL_THREAD_DETACH:
			DetachThread();
			break;
	}

	return TRUE;
}

bool TraceSystem::Initialize() {
	static constexpr size_t ENV_STR_LENGTH = 32;
	char executablePath[MAX_PATH * 4] = {};
	::GetModuleFileNameA(nullptr, executablePath, sizeof(executablePath) - 1);
	const char* name = ::PathFindFileNameA(executablePath);

	// individual?
	if (_stricmp(name, "rundll32.exe") == 0) {
		isHost = true;
	} else {
		char value[ENV_STR_LENGTH];
		if (::GetEnvironmentVariableA("waitrace::TraceSystem::isHost", value, ENV_STR_LENGTH - 1) != 0) {
			isHost = atoi(value) != 0;
		}
	}

	// Generate randomized guid
	if (SharedRecordMemoryLock.load(std::memory_order_acquire) != SPINLOCK_COMPLETE) {
		DWORD state;
		while ((state = SharedRecordMemoryLock.exchange(SPINLOCK_LOCKING, std::memory_order_relaxed)) == SPINLOCK_LOCKING) {
			::Sleep(50);
		}

		if (state == SPINLOCK_IDLE) {
			::UuidCreateSequential(&SharedRecordMemoryGuid);
		}
		
		SharedRecordMemoryLock.store(SPINLOCK_COMPLETE, std::memory_order_release);
	}

	RPC_CSTR str;
	::UuidToStringA(&SharedRecordMemoryGuid, &str);
	sharedRecordHandle = ::CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, SHARED_RECORD_INFO_BUFFER_LENGTH * sizeof(RecordInfo), reinterpret_cast<LPCTSTR>(str));
	::RpcStringFreeA(&str);

	if (sharedRecordHandle == nullptr) {
		return false;
	}

	sharedRecordInfos = reinterpret_cast<RecordInfo*>(::MapViewOfFile(sharedRecordHandle, FILE_MAP_ALL_ACCESS, 0, 0, 0));
	if (sharedRecordInfos == nullptr) {
		::CloseHandle(sharedRecordHandle);
		sharedRecordHandle = nullptr;
		return false;
	}

	proxyMemory = reinterpret_cast<PBYTE>(::VirtualAlloc(nullptr, PROXY_LENGTH, MEM_COMMIT, PAGE_EXECUTE_READWRITE));
	if (proxyMemory != nullptr) {
		if (!InstallHooks()) {
			::VirtualFree(proxyMemory, 0, MEM_RELEASE);
			proxyMemory = nullptr;
			return false;
		}

		if (isHost) {
			currentRecordInfoReadIndex = SharedRecordWriteIndex.load(std::memory_order_acquire);
			worker.start();
		}

		return true;
	} else {
		return false;
	}
}

void TraceSystem::Uninitialize() {
	if (sharedRecordHandle != nullptr) {
		assert(sharedRecordInfos != nullptr);
		::UnmapViewOfFile(sharedRecordInfos);
		::CloseHandle(sharedRecordHandle);

		sharedRecordHandle = nullptr;
	}

	if (proxyMemory != nullptr) {
		UninstallHooks();
		worker.terminate();
		::VirtualFree(proxyMemory, 0, MEM_RELEASE);
		proxyMemory = nullptr;
	}
}

void TraceSystem::InstallLibraryNotifier() {
	assert(libraryNotifier == nullptr);
	typedef void (CALLBACK* PLdrDllNotification)(ULONG notificationReason, PLDR_DLL_NOTIFICATION_DATA notificationData, PVOID context);
	NTSTATUS(WINAPI * LdrRegisterDllNotification)(ULONG, PLdrDllNotification, PVOID, PVOID*);
	LdrRegisterDllNotification = reinterpret_cast<decltype(LdrRegisterDllNotification)>(::GetProcAddress(ntdllHandle, "LdrRegisterDllNotification"));
	if (!NT_SUCCESS(LdrRegisterDllNotification(0, &::LdrDllNotification, this, &libraryNotifier))) {
		fprintf(stderr, "TraceSystem::InstallLibraryNotifier() -> Cannot register dll notification\n");
	}
}

void TraceSystem::UninstallLibraryNotifier() {
	if (libraryNotifier != nullptr) {
		NTSTATUS(WINAPI * LdrUnregisterDllNotification)(PVOID Cookie);
		LdrUnregisterDllNotification = reinterpret_cast<decltype(LdrUnregisterDllNotification)>(::GetProcAddress(ntdllHandle, "LdrUnregisterDllNotification"));

		LdrUnregisterDllNotification(libraryNotifier);
		libraryNotifier = nullptr;
	}
}

bool TraceSystem::SuspendOtherThreads() {
	DWORD currentThreadId = ::GetCurrentThreadId();
	DWORD currentProcessId = ::GetCurrentProcessId();
	HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (snapshot != nullptr) {
		THREADENTRY32 entry = {};
		entry.dwSize = sizeof(entry);

		if (::Thread32First(snapshot, &entry)) {
			do {
				if (entry.th32ThreadID != 0 && entry.th32ThreadID != currentThreadId && entry.th32OwnerProcessID == currentProcessId) {
					HANDLE handle = ::OpenThread(THREAD_ALL_ACCESS, FALSE, entry.th32ThreadID);
					if (handle != INVALID_HANDLE_VALUE) {
						::SuspendThread(handle);
						::CloseHandle(handle);
					}
				}
			} while (::Thread32Next(snapshot, &entry));
		}

		::CloseHandle(snapshot);
		return true;
	} else {
		fprintf(stderr, "TraceSystem::InstallHooks() -> Cannot resume other threads.\n");
		return false;
	}
}

bool TraceSystem::ResumeOtherThreads() {
	DWORD currentThreadId = ::GetCurrentThreadId();
	DWORD currentProcessId = ::GetCurrentProcessId();
	HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, currentProcessId);

	if (snapshot != nullptr) {
		THREADENTRY32 entry = {};
		entry.dwSize = sizeof(entry);

		if (::Thread32First(snapshot, &entry)) {
			do {
				if (entry.th32ThreadID != 0 && entry.th32ThreadID != currentThreadId && entry.th32OwnerProcessID == currentProcessId) {
					HANDLE handle = ::OpenThread(THREAD_ALL_ACCESS, FALSE, entry.th32ThreadID);
					if (handle != INVALID_HANDLE_VALUE) {
						::ResumeThread(handle);
						::CloseHandle(handle);
					}
				}
			} while (::Thread32Next(snapshot, &entry));
		}

		::CloseHandle(snapshot);
		return true;
	} else {
		fprintf(stderr, "TraceSystem::InstallHooks() -> Cannot resume other threads.\n");
		return false;
	}
}

bool TraceSystem::InstallHooks() {
	if (SuspendOtherThreads()) {
		for (size_t i = 0; i < hookInfos.size(); i++) {
			HookInfo& hookInfo = hookInfos[i];
			if (hookInfo.originalAddress != nullptr) {
				assert(proxyOffset < PROXY_LENGTH);
				InlineHookNativeApi(hookInfo.originalAddress, hookInfo.proxyAddress, proxyMemory + proxyOffset);
				hookInfo.stubAddress = proxyMemory + proxyOffset;
				proxyOffset += NATIVE_API_LENGTH;
			}
		}

		ResumeOtherThreads();
		return true;
	} else {
		return false;
	}
}

bool TraceSystem::UninstallHooks() {
	if (SuspendOtherThreads()) {
		for (size_t i = 0; i < hookInfos.size(); i++) {
			HookInfo& hookInfo = hookInfos[i];
			if (hookInfo.originalAddress != nullptr) {
				UnlineHookNativeApi(hookInfo.originalAddress, hookInfo.proxyAddress, hookInfo.stubAddress);
			}
		}

		ResumeOtherThreads();
		return true;
	} else {
		return false;
	}
}

void TraceSystem::SetWaitCallback(WaitCallback callback, PVOID context) {
	if (callback != nullptr) {
		waitContext = context;
	}

	waitCallback.store(callback, std::memory_order_release);
}

void TraceSystem::NotifyWaitCallbacks(BOOL preCall, HANDLE* objectHandles, DWORD count, PVOID address, PLARGE_INTEGER timeout, SHORT apiIndex) {
	const char* hookedApi = hookInfos[apiIndex].name;
	ExemptGuard guard(hookedApi);
	if (guard) {
		WaitCallback callback = waitCallback.load(std::memory_order_acquire);
		if (callback != nullptr) {
			callback(preCall, objectHandles, count, address, timeout, hookedApi, waitContext);
		}

		DWORD threadId = ::GetCurrentThreadId();
		AttachThread(threadId);

		RecordInfo info;
		info.action = 0;
		info.threadId = threadId;
		info.preCall = preCall;
		info.action = grid::grid_verify_cast<decltype(info.action)>(apiIndex);

		if (address != nullptr) {
			info.address = address;
			PostRecord(std::move(info));
		} else {
			for (DWORD i = 0; i < count; i++) {
				info.handle = grid::grid_verify_cast<DWORD>(reinterpret_cast<size_t>(objectHandles[i]));
				info.objectType = grid::grid_verify_cast<DWORD>(DetectObjectTypeByHandle(objectHandles[i]));
				PostRecord(std::move(info));
			}

			if (count > 1) {
				info.handle = 0;
				info.objectType = ObjectType::Address; // mark finished
				PostRecord(std::move(info));
			}
		}
	}
}

const char* TraceSystem::GetObjectTypeName(DWORD type) const {
	assert(type < sizeof(ObjectTypes) / sizeof(ObjectTypes[0]));
	return ObjectTypes[type];
}


TraceSystem::ObjectType TraceSystem::DetectObjectTypeByHandle(HANDLE handle) {
	PUBLIC_OBJECT_TYPE_INFORMATION info = {};
	ULONG length;
	if (NT_SUCCESS(::NtQueryObject(handle, ObjectTypeInformation, &info, sizeof(info), &length))) {
		if (::wcscmp(info.TypeName.Buffer, L"Event") == 0) {
			return ObjectType::Event;
		} else if (::wcscmp(info.TypeName.Buffer, L"Mutex") == 0) {
			return ObjectType::Mutex;
		} else if (::wcscmp(info.TypeName.Buffer, L"Semaphore") == 0) {
			return ObjectType::Semaphore;
		} else if (::wcscmp(info.TypeName.Buffer, L"Process") == 0) {
			return ObjectType::Process;
		} else if (::wcscmp(info.TypeName.Buffer, L"Thread") == 0) {
			return ObjectType::Thread;
		} else if (::wcscmp(info.TypeName.Buffer, L"File") == 0) {
			return ObjectType::File;
		} else if (::wcscmp(info.TypeName.Buffer, L"IoCompletionPort") == 0) {
			return ObjectType::IoCompletionPort;
		} else {
			return ObjectType::Other;
		}
	} else {
		return ObjectType::Other;
	}
}

// ThreadInfo
TraceSystem::ThreadInfo::ThreadInfo(Worker& worker, DWORD thread) noexcept : warp(worker, PRIORITY_HIGHEST), threadId(thread), visualizeDataFrameView(visualizeDataList) {}

void TraceSystem::ThreadInfo::Cleanup(const LARGE_INTEGER& timestampLimit) {
	eventInfoList.pop(GetLowerBound(timestampLimit) - eventInfoList.begin());
}

void TraceSystem::ThreadInfo::Reset() {
	eventInfoList.reset(0);
}

TraceSystem::EventInfoList::iterator TraceSystem::ThreadInfo::GetLowerBound(const LARGE_INTEGER& timestamp) noexcept {
	return std::lower_bound(eventInfoList.begin(), eventInfoList.end(), timestamp, [](const EventInfo& info, const LARGE_INTEGER& timestamp) noexcept {
		return info.timestamp.QuadPart < timestamp.QuadPart;
	});
}

TraceSystem::EventInfoList::iterator TraceSystem::ThreadInfo::GetUpperBound(const LARGE_INTEGER& timestamp) noexcept {
	return std::upper_bound(eventInfoList.begin(), eventInfoList.end(), timestamp, [](const LARGE_INTEGER& timestamp, const EventInfo& info) noexcept {
		return timestamp.QuadPart < info.timestamp.QuadPart;
	});
}

void TraceSystem::ThreadInfo::UpdateVisualizeData(const std::vector<HookInfo>& hookInfos, const LARGE_INTEGER& startTimestamp, const LARGE_INTEGER& endTimestamp) {
	auto start = GetLowerBound(startTimestamp);
	auto end = GetUpperBound(endTimestamp);

	VisualizeData data;
	data.name = nullptr;

	while (start != end) {
		const RecordInfo& recordInfo = start->recordInfo;
		const LARGE_INTEGER& timestamp = start->timestamp;
		switch (recordInfo.action) {
			// waits
			case Action::NtWaitForAlertByThreadId:
			case Action::NtWaitForSingleObject:
			case Action::NtRemoveIoCompletion:
			case Action::NtWaitForMultipleObjects:
			{
				if (recordInfo.preCall) {
					if (data.name == nullptr) {
						data.name = hookInfos[recordInfo.action].name;
						data.startTimestamp = timestamp;
					}
				} else {
					data.endTimestamp = timestamp;
					visualizeDataFrameView.push(std::move(data));
					data.name = nullptr;
					data.signals.clear();
				}

				break;
			}

			// signals
			case Action::NtSignalAndWaitForSingleObject:
			case Action::NtSetIoCompletion:
			case Action::NtReleaseSemaphore:
			case Action::NtSetEvent:
			case Action::NtReleaseMutant:
			case Action::NtAlertThreadByThreadId:
			{
				if (recordInfo.preCall) {
					SignalData signalData;
					signalData.apiIndex = recordInfo.action;
					signalData.objectType = grid::grid_verify_cast<USHORT>(recordInfo.objectType);
					signalData.invokeThread = recordInfo.threadId;
					data.signals.push(std::move(signalData));
				}

				break;
			}
		}

		++start;
	}

	if (!data.signals.empty()) {
		visualizeDataFrameView.push(std::move(data));
	}

	visualizeDataFrameView.release();
	updating.store(0, std::memory_order_release);
}

void TraceSystem::ThreadInfo::Record(RecordInfo&& recordInfo, const LARGE_INTEGER& timestamp) {
	eventInfoList.emplace(EventInfo{ std::move(recordInfo), timestamp });
}

void TraceSystem::Catchup() {
	LARGE_INTEGER current = GetTimestamp();
	for (auto& thread : threadInfoMap) {
		thread.second->GetWarp().queue_routine_post([info = thread.second, current]() mutable {
			info->Cleanup(current);
		});
	}
}

void TraceSystem::PostRecord(RecordInfo&& recordInfo) {
	DWORD index = SharedRecordWriteIndex.fetch_add(1, std::memory_order_relaxed);
	recordInfo.recordIndex = index;
	DWORD rindex = index % SHARED_RECORD_INFO_BUFFER_LENGTH;
	static_assert(std::is_trivially_copyable<RecordInfo>::value, "RecordInfo must be trivially copyable.");
	static_assert(offsetof(RecordInfo, recordIndex) + alignof(RecordInfo) >= sizeof(RecordInfo), "recordIndex must be placed at end of RecordInfo");
	sharedRecordInfos[rindex] = std::move(recordInfo); // x86: sequentially memory order
}

void TraceSystem::AttachThread(DWORD threadId) {
	PVOID value = ::TlsGetValue(ThreadActiveTLSIndex);
	if (value == nullptr) {
		::TlsSetValue(ThreadActiveTLSIndex, this);

		RecordInfo info = {};
		info.action = Action::ThreadCreation;
		info.threadId = threadId;
		PostRecord(std::move(info));
	}
}

void TraceSystem::DetachThread() {
	PVOID value = ::TlsGetValue(ThreadActiveTLSIndex);
	if (value != nullptr) {
		DWORD threadId = ::GetCurrentThreadId();

		RecordInfo info = {};
		info.action = Action::ThreadDeletion;
		info.threadId = threadId;
		PostRecord(std::move(info));

		::TlsSetValue(ThreadActiveTLSIndex, nullptr);
	}
}

// Callbacks

void TraceSystem::LdrDllNotification(ULONG notificationReason, PVOID data, PVOID context) {
	static constexpr ULONG LDR_DLL_NOTIFICATION_REASON_LOADED = 1;
	static constexpr ULONG LDR_DLL_NOTIFICATION_REASON_UNLOADED = 2;
	PLDR_DLL_NOTIFICATION_DATA notificationData = reinterpret_cast<PLDR_DLL_NOTIFICATION_DATA>(data);

	RecordInfo info = {};
	if (notificationReason == LDR_DLL_NOTIFICATION_REASON_LOADED) {
		info.action = Action::LibraryLoad;
		info.address = notificationData->Loaded.DllBase;
	} else {
		info.action = Action::LibraryUnload;
		info.address = notificationData->Unloaded.DllBase;
	}

	PostRecord(std::move(info));
}

NTSTATUS TraceSystem::ProxyNtWaitForSingleObject(HANDLE handle, BOOLEAN alertable, PLARGE_INTEGER timeout) {
	typedef NTSTATUS(WINAPI* pfn)(HANDLE handle, BOOLEAN alertable, PLARGE_INTEGER timeout);
	NotifyWaitCallbacks(true, &handle, 1, nullptr, timeout, Action::NtWaitForSingleObject);
	NTSTATUS status = reinterpret_cast<pfn>(hookInfos[Action::NtWaitForSingleObject].stubAddress)(handle, alertable, timeout);
	NotifyWaitCallbacks(false, &handle, 1, nullptr, timeout, Action::NtWaitForSingleObject);

	return status;
}

NTSTATUS TraceSystem::ProxyNtWaitForMultipleObjects(DWORD count, HANDLE* handles, DWORD waitType, BOOL alertable, PLARGE_INTEGER timeout) {
	typedef NTSTATUS(WINAPI* pfn)(DWORD count, const HANDLE* handles, DWORD waitType, BOOL alertable, PLARGE_INTEGER timeout);
	NotifyWaitCallbacks(true, handles, count, nullptr, nullptr, Action::NtWaitForMultipleObjects);
	NTSTATUS status = reinterpret_cast<pfn>(hookInfos[Action::NtWaitForMultipleObjects].stubAddress)(count, handles, waitType, alertable, timeout);
	NotifyWaitCallbacks(false, handles, count, nullptr, nullptr, Action::NtWaitForMultipleObjects);

	return status;
}

NTSTATUS TraceSystem::ProxyNtWaitForAlertByThreadId(PVOID address, ULONG_PTR threadId) {
	typedef NTSTATUS(WINAPI* pfn)(PVOID address, ULONG_PTR threadId);
	NotifyWaitCallbacks(true, nullptr, 0, address, nullptr, Action::NtWaitForAlertByThreadId);
	NTSTATUS status = reinterpret_cast<pfn>(hookInfos[Action::NtWaitForAlertByThreadId].stubAddress)(address, threadId);
	NotifyWaitCallbacks(false, nullptr, 0, address, nullptr, Action::NtWaitForAlertByThreadId);

	return status;
}

NTSTATUS TraceSystem::ProxyNtRemoveIoCompletion(HANDLE completionPort, PULONG_PTR completionKey, LPOVERLAPPED* overlapped, PVOID result) {
	typedef NTSTATUS(WINAPI* pfn)(HANDLE completionPort, PULONG_PTR completionKey, LPOVERLAPPED* overlapped, PVOID result);
	NotifyWaitCallbacks(true, &completionPort, 1, nullptr, nullptr, Action::NtRemoveIoCompletion);
	NTSTATUS status = reinterpret_cast<pfn>(hookInfos[Action::NtRemoveIoCompletion].stubAddress)(completionPort, completionKey, overlapped, result);
	NotifyWaitCallbacks(false, &completionPort, 1, nullptr, nullptr, Action::NtRemoveIoCompletion);

	return status;
}

NTSTATUS TraceSystem::ProxyNtSignalAndWaitForSingleObject(HANDLE handleToSignal, HANDLE handleToWaitOn, BOOL alertable, PLARGE_INTEGER timeout) {
	typedef NTSTATUS(WINAPI* pfn)(HANDLE handleToSignal, HANDLE handleToWaitOn, BOOL alertable, PLARGE_INTEGER milliseconds);
	NotifyWaitCallbacks(true, &handleToSignal, 1, nullptr, timeout, Action::NtSignalAndWaitForSingleObject);
	NotifyWaitCallbacks(true, &handleToWaitOn, 1, nullptr, timeout, Action::NtWaitForSingleObject);
	NTSTATUS status = reinterpret_cast<pfn>(hookInfos[Action::NtSignalAndWaitForSingleObject].stubAddress)(handleToSignal, handleToWaitOn, alertable, timeout);
	NotifyWaitCallbacks(false, &handleToWaitOn, 1, nullptr, timeout, Action::NtWaitForSingleObject);
	NotifyWaitCallbacks(false, &handleToSignal, 1, nullptr, timeout, Action::NtSignalAndWaitForSingleObject);

	return status;
}

NTSTATUS TraceSystem::ProxyNtAlertThreadByThreadId(DWORD threadId) {
	typedef NTSTATUS(WINAPI* pfn)(DWORD threadId);
	PVOID value = reinterpret_cast<PVOID>((size_t)threadId);
	NotifyWaitCallbacks(true, &value, 1, nullptr, 0, Action::NtAlertThreadByThreadId);
	NTSTATUS status = reinterpret_cast<pfn>(hookInfos[Action::NtAlertThreadByThreadId].stubAddress)(threadId);
	NotifyWaitCallbacks(false, &value, 1, nullptr, 0, Action::NtAlertThreadByThreadId);

	return status;
}

NTSTATUS TraceSystem::ProxyNtSetIoCompletion(HANDLE completionPort, PVOID keyContext, PVOID apcContext, NTSTATUS ioStatus, ULONG completionInformation) {
	typedef NTSTATUS(WINAPI* pfn)(HANDLE completionPort, PVOID keyContext, PVOID apcContext, NTSTATUS ioStatus, ULONG completionInformation);
	NotifyWaitCallbacks(true, &completionPort, 1, nullptr, 0, Action::NtSetIoCompletion);
	NTSTATUS status = reinterpret_cast<pfn>(hookInfos[Action::NtSetIoCompletion].stubAddress)(completionPort, keyContext, apcContext, ioStatus, completionInformation);
	NotifyWaitCallbacks(false, &completionPort, 1, nullptr, 0, Action::NtSetIoCompletion);

	return status;
}

NTSTATUS TraceSystem::ProxyNtReleaseSemaphore(HANDLE semaphoreHandle, LONG releaseCount, PLONG previousCount) {
	typedef NTSTATUS(WINAPI* pfn)(HANDLE semaphoreHandle, LONG releaseCount, PLONG previousCount);
	NotifyWaitCallbacks(true, &semaphoreHandle, 1, nullptr, 0, Action::NtReleaseSemaphore);
	NTSTATUS status = reinterpret_cast<pfn>(hookInfos[Action::NtReleaseSemaphore].stubAddress)(semaphoreHandle, releaseCount, previousCount);
	NotifyWaitCallbacks(false, &semaphoreHandle, 1, nullptr, 0, Action::NtReleaseSemaphore);

	return status;
}

NTSTATUS TraceSystem::ProxyNtSetEvent(HANDLE eventHandle, PLONG previousState) {
	typedef NTSTATUS(WINAPI* pfn)(HANDLE eventHandle, PLONG previousState);
	NotifyWaitCallbacks(true, &eventHandle, 1, nullptr, 0, Action::NtSetEvent);
	NTSTATUS status = reinterpret_cast<pfn>(hookInfos[Action::NtSetEvent].stubAddress)(eventHandle, previousState);
	NotifyWaitCallbacks(false, &eventHandle, 1, nullptr, 0, Action::NtSetEvent);

	return status;
}

NTSTATUS TraceSystem::ProxyNtReleaseMutant(HANDLE mutantHandle, PLONG releaseCount) {
	typedef NTSTATUS(WINAPI* pfn)(HANDLE mutantHandle, PLONG releaseCount);
	NotifyWaitCallbacks(true, &mutantHandle, 1, nullptr, 0, Action::NtReleaseMutant);
	NTSTATUS status = reinterpret_cast<pfn>(hookInfos[Action::NtReleaseMutant].stubAddress)(mutantHandle, releaseCount);
	NotifyWaitCallbacks(false, &mutantHandle, 1, nullptr, 0, Action::NtReleaseMutant);

	return status;
}

