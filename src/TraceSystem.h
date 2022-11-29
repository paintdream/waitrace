// TraceSystem.h
// PaintDream (paintdream@paintdream.com)
// 2021-8-29
//

#pragma once

#include "Waitrace.h"
#include "../ref/grid_dispatcher/grid_dispatcher.h"
#include "../ref/grid_dispatcher/grid_buffer.h"
#include <vector>
#include <map>
#include <unordered_map>
#include <mutex>

namespace Waitrace {
	class TraceSystem {
	public:
		TraceSystem();
		~TraceSystem();

		struct RecordInfo {
			union {
				ULONG_PTR hash;
				PVOID address;
				struct {
					DWORD objectType;
					DWORD handle; // MSDN: handle is always 32 bit
				};
				struct {
					DWORD reserved;
					DWORD targetThreadId;
				};
			};

			DWORD threadId : 24;
			DWORD preCall : 1;
			DWORD action : 7;
			DWORD recordIndex;
		};

		friend class GraphicsUI;

	protected:
		struct HookInfo {
			const char* name;
			void* proxyAddress;
			void* originalAddress;
			void* stubAddress;
		};

		struct Lifetime {
			Lifetime(size_t id);
			~Lifetime();
		};

		using ThreadIdArray = grid::grid_buffer_t<DWORD>;

	public:
		using Worker = grid::grid_async_worker_t<std::thread, Lifetime>;
		using Warp = grid::grid_warp_t<Worker>;
		using Dispatcher = grid::grid_dispatcher_t<Warp>;
		using Balancer = grid::grid_async_balancer_t<Worker>;

		static TraceSystem& GetInstance();
		BOOL DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved);
		void SetWaitCallback(WaitCallback waitCallback, PVOID context);
		void LoopHost();
		void Terminate();
		const LARGE_INTEGER& GetPerformanceFrequency() const noexcept { return performanceFrequency; }
		bool InjectToProcess(DWORD processId);
		bool EjectFromProcess(DWORD processId);
		const std::vector<HookInfo>& GetHookInfos() const { return hookInfos; }
		const char* GetObjectTypeName(DWORD type) const;

		TraceSystem(const TraceSystem& rhs) = delete;
		TraceSystem(TraceSystem&& rhs) = delete;

	public:
		enum ObjectType : DWORD {
			Other = 0,
			Event,
			Mutex,
			Semaphore,
			Process,
			Thread,
			File,
			IoCompletionPort,
			Address
		};

		enum Action : DWORD {
			NtWaitForAlertByThreadId = 0,
			NtWaitForSingleObject,
			NtRemoveIoCompletion,
			NtWaitForMultipleObjects,
			NtSignalAndWaitForSingleObject,
			NtSetIoCompletion,
			NtReleaseSemaphore,
			NtSetEvent,
			NtReleaseMutant,
			NtAlertThreadByThreadId,
			ThreadCreation,
			ThreadDeletion,
			LibraryLoad,
			LibraryUnload,
		};

		// Waits
		NTSTATUS ProxyNtWaitForSingleObject(HANDLE handle, BOOLEAN alertable, PLARGE_INTEGER timeout);
		NTSTATUS ProxyNtWaitForMultipleObjects(DWORD count, HANDLE* handles, DWORD waitType, BOOL alertable, PLARGE_INTEGER timeout);
		NTSTATUS ProxyNtWaitForAlertByThreadId(PVOID address, ULONG_PTR threadId);
		NTSTATUS ProxyNtRemoveIoCompletion(HANDLE completionPort, PULONG_PTR completionKey, LPOVERLAPPED* overlapped, PVOID result);
		// Notifications
		NTSTATUS ProxyNtSignalAndWaitForSingleObject(HANDLE handleToSignal, HANDLE handleToWaitOn, BOOL alertable, PLARGE_INTEGER milliseconds);
		NTSTATUS ProxyNtAlertThreadByThreadId(DWORD threadId);
		NTSTATUS ProxyNtSetIoCompletion(HANDLE completionPort, PVOID keyContext, PVOID apcContext, NTSTATUS ioStatus, ULONG completionInformation);
		NTSTATUS ProxyNtReleaseSemaphore(HANDLE semaphoreHandle, LONG releaseCount, PLONG previousCount);
		NTSTATUS ProxyNtSetEvent(HANDLE eventHandle, PLONG previousState);
		NTSTATUS ProxyNtReleaseMutant(HANDLE mutantHandle, PLONG releaseCount);

		// Callbacks
		void LdrDllNotification(ULONG notificationReason, PVOID notificationData, PVOID context);

	protected:
		struct EventInfo {
			RecordInfo recordInfo;
			LARGE_INTEGER timestamp;
		};
		
		using EventInfoList = grid::grid_queue_list_t<EventInfo>;

		struct SignalData {
			USHORT apiIndex;
			USHORT objectType;
			DWORD invokeThread;
			DWORD timestampOffset;
		};

		using SignalList = grid::grid_buffer_t<SignalData>;

		struct VisualizeData {
			const char* name;
			LARGE_INTEGER startTimestamp;
			LARGE_INTEGER endTimestamp;
			SignalList signals;
		};

		using VisualizeDataList = grid::grid_queue_list_t<VisualizeData>;
		using VisualizeDataFrameView = grid::grid_queue_frame_t<VisualizeDataList>;

		struct ThreadInfo {
		public:
			ThreadInfo(Worker& worker, DWORD thread) noexcept;
			void Cleanup(const LARGE_INTEGER& timestampThreshold);
			void Reset();
			void Record(RecordInfo&& recordInfo, const LARGE_INTEGER& timestamp);
			void UpdateVisualizeData(const std::vector<HookInfo>& hookInfos, const LARGE_INTEGER& startTimestamp, const LARGE_INTEGER& endTimstamp);
			Warp& GetWarp() noexcept { return warp; }
			DWORD GetThreadId() const noexcept { return threadId; }
			bool PrepareVisualizeDataUpdating() noexcept { return updating.exchange(1, std::memory_order_acquire) == 0; }
			constexpr bool IsVisualizing() const noexcept { return true; } // always visualizing by now
			VisualizeDataFrameView& GetVisualizeData() noexcept { return visualizeDataFrameView; }
			
		protected:
			EventInfoList::iterator GetLowerBound(const LARGE_INTEGER& timestamp) noexcept;
			EventInfoList::iterator GetUpperBound(const LARGE_INTEGER& timestamp) noexcept;

		protected:
			EventInfoList eventInfoList;
			VisualizeDataList visualizeDataList;
			VisualizeDataFrameView visualizeDataFrameView;
			std::atomic<DWORD> updating = 0;
			DWORD threadId;
			Warp warp;
		};

		void InstallLibraryNotifier();
		void UninstallLibraryNotifier();
		bool InstallHooks();
		bool UninstallHooks();
		bool SuspendOtherThreads();
		bool ResumeOtherThreads();
		void MasterWorkerThread();
		void NotifyWaitCallbacks(BOOL preCall, HANDLE* objectHandles, DWORD handleCount, PVOID address, PLARGE_INTEGER timeout, SHORT apiIndex);
		void PostRecord(RecordInfo&& recordInfo);
		void Catchup();
		void UpdateWaitingObjectsThreadMap(const RecordInfo& recordInfo);
		ThreadIdArray GetThreadsByWaitingObject(const RecordInfo& recordInfo);
		static ObjectType DetectObjectTypeByHandle(HANDLE handle);

	protected:
		bool Initialize();
		void Uninitialize();
		void AttachThread(DWORD threadId);
		void DetachThread();

	protected:
		// Configurations
		LARGE_INTEGER performanceFrequency;
		bool isHost = false;
		bool isPaused = false;
		bool isStopped = false;
		bool isShowGraphicsUI = false;

		// Callbacks
		std::atomic<WaitCallback> waitCallback = nullptr;

		// Platform-related Handles & Resources
		HMODULE moduleHandle = nullptr;
		HMODULE ntdllHandle = nullptr;
		PBYTE proxyMemory = nullptr;
		RecordInfo* sharedRecordInfos = nullptr;
		HANDLE sharedRecordHandle = nullptr;
		void* libraryNotifier = nullptr;
		PVOID waitContext = nullptr;
		DWORD proxyOffset = 0;

		// Thread event record management
		DWORD currentRecordInfoReadIndex = 0;
		std::vector<HookInfo> hookInfos;
		std::map<DWORD, std::shared_ptr<ThreadInfo>> threadInfoMap;
		std::unordered_map<DWORD, ThreadIdArray> waitingObjectsThreadMap;

		// Processing queues
		using ThreadInfoList = grid::grid_queue_list_t<std::shared_ptr<TraceSystem::ThreadInfo>>;
		using ThreadInfoFrameView = grid::grid_queue_frame_t<ThreadInfoList>;
		ThreadInfoList threadInfoList;
		ThreadInfoFrameView threadInfoFrameView;

		// Core objects
		Worker worker;
		Dispatcher dispatcher;
		Balancer balancer;
	};
}

