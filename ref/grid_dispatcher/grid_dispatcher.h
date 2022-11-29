/*
Grid-based Task Dispatcher System

This software is a C++ 11 Header-Only reimplementation of core part from project PaintsNow.

The MIT License (MIT)

Copyright (c) 2014-2022 PaintDream

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

*/

#pragma once

#include "grid_common.h"
#include <functional>
#include <vector>
#include <mutex>
#include <chrono>
#include <thread>
#include <condition_variable>

namespace grid {
	namespace {
		// storage for queued tasks
		template <typename queue_buffer_t, bool>
		struct storage_t {
			storage_t() noexcept {}
			storage_t(storage_t&& rhs) noexcept {
				queue_buffer = std::move(rhs.queue_buffer);
			}

			storage_t& operator = (storage_t&& rhs) noexcept {
				queue_buffer = std::move(rhs.queue_buffer);
				return *this;
			}

			bool empty() const noexcept {
				return queue_buffer.empty();
			}

			queue_buffer_t queue_buffer;
			std::mutex mutex;
		};

		template <typename queue_buffer_t>
		struct storage_t<queue_buffer_t, false> {
			bool empty() const noexcept {
				for (size_t i = 0; i < queue_buffers.size(); i++) {
					if (!queue_buffers[i].empty())
						return false;
				}

				return true;
			}

			std::vector<queue_buffer_t> queue_buffers;
		};
	}

	// dispatch routines:
	//     1. from warp to warp. (queue_routine/queue_routine_post).
	//     2. from external thread to warp (queue_routine_external).
	//     3. from warp to external in parallel (queue_routine_parallel).
	// you can select implemention from warp/strand via 'strand' template parameter.
	template <typename worker_t, typename func_t = std::function<void()>, template <typename...> typename allocator_t = grid_default_block_allocator_t, bool strand = false>
	struct grid_warp_t {
		// for exception safe!
		struct suspend_guard_t {
			suspend_guard_t(grid_warp_t* w) noexcept : warp(w) {}
			void cleanup() noexcept { warp = nullptr; }

			~suspend_guard_t() {
				// if compiler detects warp is nullptr
				// it can remove the ~suspend_guard_t() calling
				if (warp != nullptr) {
					warp->resume();
				}
			}

		private:
			grid_warp_t* warp;
		};

		struct preempt_guard_t {
			preempt_guard_t(grid_warp_t& warp_instance) noexcept : warp(warp_instance) {
				if (grid_warp_t::get_current_warp() == &warp) {
					state = already = true;
				} else {
					state = warp.preempt();
					already = false;
				}
			}

			~preempt_guard_t() { if (state && !already) warp.yield(); }

			operator bool() const {
				return state;
			}

		protected:
			grid_warp_t& warp;
			bool state;
			bool already;
		};

		using function_t = func_t;
		using queue_buffer_t = grid_queue_list_t<function_t, allocator_t>;
		using async_worker_t = worker_t;
		static constexpr size_t block_size = grid_extract_block_size<function_t, allocator_t>::value;
		static constexpr size_t queue_state_idle = 0u;
		static constexpr size_t queue_state_pending = 1u;
		static constexpr size_t queue_state_executing = 2u;

		// moving capture is not supported until C++ 14
		// so we wrap some functors here

		struct execute_t {
			execute_t(grid_warp_t& w) noexcept : warp(w) {}
			void operator () () {
				warp.template execute<strand>();
			}

			grid_warp_t& warp;
		};

		template <typename callable_t>
		struct external_t {
			template <typename proc_t>
			external_t(grid_warp_t& w, proc_t&& c) noexcept : warp(w), callable(std::forward<proc_t>(c)) {}
			void operator () () {
				warp.queue_routine_post(std::move(callable));
			}

			grid_warp_t& warp;
			callable_t callable;
		};

		template <typename callable_t>
		struct suspend_t {
			template <typename proc_t>
			suspend_t(grid_warp_t& w, proc_t&& c) noexcept : warp(w), callable(std::forward<proc_t>(c)) {}
			void operator () () {
				suspend_guard_t guard(&warp);
				callable();
				guard.cleanup();

				warp.resume();
			}

			grid_warp_t& warp;
			callable_t callable;
		};

		// do not copy this structure, only to move
		grid_warp_t(const grid_warp_t& rhs) = delete;
		grid_warp_t& operator = (const grid_warp_t& rhs) = delete;
		grid_warp_t& operator = (grid_warp_t&& rhs) = delete;

		// for strands, we prepare just one queue protected by a mutex
		// for warps, we prepare one queue for each thread to remove mutex requirements

		template <bool s>
		typename std::enable_if<s>::type init_buffers(size_t thread_count) noexcept {}

		template <bool s>
		typename std::enable_if<!s>::type init_buffers(size_t thread_count) noexcept(noexcept(std::declval<grid_warp_t>().storage.queue_buffers.resize(thread_count))) {
			storage.queue_buffers.resize(thread_count);
		}

		// initialize with specified priority, all tasks that runs on this warp will be scheduled with this priority
		explicit grid_warp_t(async_worker_t& worker, size_t prior = 0) : async_worker(worker), priority(prior), stack_next_warp(nullptr) {
			init_buffers<strand>(worker.get_thread_count());

			thread_warp.store(nullptr, std::memory_order_relaxed);
			suspend_count.store(0, std::memory_order_relaxed);
			interrupting.store(0, std::memory_order_relaxed);
			queueing.store(queue_state_idle, std::memory_order_release);
		}

		grid_warp_t(grid_warp_t&& rhs) noexcept : async_worker(rhs.async_worker), priority(rhs.priority), stack_next_warp(rhs.stack_next_warp), storage(std::move(rhs.storage)) {
			thread_warp.store(rhs.thread_warp.load(std::memory_order_relaxed), std::memory_order_relaxed);
			suspend_count.store(rhs.suspend_count.load(std::memory_order_relaxed), std::memory_order_relaxed);
			interrupting.store(rhs.interrupting.load(std::memory_order_relaxed), std::memory_order_relaxed);
			queueing.store(rhs.queueing.load(std::memory_order_relaxed), std::memory_order_relaxed);

			rhs.stack_next_warp = nullptr;
			rhs.thread_warp.store(nullptr, std::memory_order_relaxed);
			rhs.suspend_count.store(0, std::memory_order_relaxed);
			rhs.interrupting.store(0, std::memory_order_relaxed);
			rhs.queueing.store(queue_state_idle, std::memory_order_release);
		}

		~grid_warp_t() {
			assert(get_current_warp_internal() == null());
			while (!join()) {}
		}

		// null() grid_warp_t* means detached warp environment
		static constexpr grid_warp_t* null() {
			return nullptr;
		}

		// take execution atomically, returns true on success.
		bool preempt() noexcept {
			grid_warp_t** expected = nullptr;
			grid_warp_t* current = get_current_warp_internal();
			if (thread_warp.compare_exchange_strong(expected, &get_current_warp_internal(), std::memory_order_acquire)) {
				get_current_warp_internal() = this;
				stack_next_warp = current;

				return true;
			} else {
				assert(get_current_warp_internal() != this);
				return false;
			}
		}

		// interrupt warp on running
		bool interrupt() noexcept {
			return interrupting.exchange(1, std::memory_order_relaxed) == 0;
		}

		// check if running, the result is meaningless for most calls in multithreaded context
		bool running() const noexcept {
			return thread_warp.load(std::memory_order_acquire) != nullptr;
		}

		bool empty() const noexcept {
			// must called while not running!
			assert(suspend_count.load(std::memory_order_acquire) != 0 && !running());
			return storage.empty();
		}

		// yield execution atomically, returns true on success.
		bool yield() noexcept(noexcept(std::declval<grid_warp_t>().flush())) {
			grid_warp_t** exp = &get_current_warp_internal();
			if (thread_warp.compare_exchange_strong(exp, nullptr, std::memory_order_release)) {
				get_current_warp_internal() = stack_next_warp;
				stack_next_warp = nullptr;

				if (queueing.exchange(queue_state_idle, std::memory_order_relaxed) == queue_state_pending) {
					flush();
				}

				return true;
			} else {
				return false;
			}
		}

		// blocks all tasks preemptions, stacked with internally counting.
		void suspend() noexcept {
			suspend_count.fetch_add(1, std::memory_order_acquire);
		}

		// allows all tasks preemptions, stacked with internally counting.
		// returns true on final resume.
		bool resume() noexcept(noexcept(std::declval<grid_warp_t>().flush())) {
			bool ret = suspend_count.fetch_sub(1, std::memory_order_release) == 1;

			if (ret) {
				// all suspend requests removed, try to flush me
				queueing.store(queue_state_idle, std::memory_order_relaxed);
				flush();
			}

			return ret;
		}

		// send task to this warp. call it directly if we are on warp.
		template <typename callable_t>
		void queue_routine(callable_t&& func) noexcept(noexcept(func()) &&
			noexcept(std::declval<grid_warp_t>().template push<strand>(std::forward<callable_t>(func)))) {
			size_t thread_index = async_worker.get_current_thread_index();
			assert(thread_index != ~size_t(0));

			// can be executed immediately?
			if (get_current_warp_internal() == this
				&& thread_warp.load(std::memory_order_relaxed) == &get_current_warp_internal()
				&& suspend_count.load(std::memory_order_acquire) == 0) {
				func();
			} else {
				// send to current thread slot of current warp.
				push<strand>(std::forward<callable_t>(func));
			}
		}

		// send task to warp indicated by warp. always post it to queue.
		template <typename callable_t>
		void queue_routine_post(callable_t&& func) noexcept(noexcept(std::declval<grid_warp_t>().template push<strand>(std::forward<callable_t>(func)))) {
			// always send to current thread slot of current warp.
			push<strand>(std::forward<callable_t>(func));
		}

		// queue external routine from non-warp/yielded warp
		template <typename callable_t>
		void queue_routine_external(callable_t&& func) {
			assert(async_worker.get_current_thread_index() == ~size_t(0));
			async_worker.queue(external_t<typename std::remove_reference<callable_t>::type>(*this, std::forward<callable_t>(func)));
		}

		// queue task parallelly to async_worker, blocking the execution of current warp at the same time
		// it is useful to implement read-lock affairs
		template <typename callable_t>
		void queue_routine_parallel(callable_t&& func, size_t priority = 0) {
			assert(get_current_warp_internal() == this);
			suspend();

			suspend_guard_t guard(this);
			async_worker.queue(suspend_t<typename std::remove_reference<callable_t>::type>(*this, std::forward<callable_t>(func)));
			guard.cleanup();
		}

		// cleanup the dispatcher, pass true to 'execute_remaining' to make sure all tasks are executed finally.
		template <bool execute_remaining = true, typename iterator_t = grid_warp_t*>
		static bool join(iterator_t begin, iterator_t end) {
			// suspend all warps so we can take over tasks
			for (iterator_t p = begin; p != end; ++p) {
				(*p).suspend();
			}

			// do cleanup
			bool empty = true;
			for (iterator_t p = begin; p != end; ++p) {
				while (true) {
					preempt_guard_t preempt_guard(*p);
					if (!preempt_guard) {
						std::this_thread::sleep_for(std::chrono::milliseconds(50));
					} else {
						// execute remaining
						if (execute_remaining) {
							(*p).template execute_internal<strand, true>();
						}

						break;
					}
				} 

				empty = empty && (*p).empty();
			}

			// resume warps
			for (iterator_t p = begin; p != end; ++p) {
				(*p).resume();
			}

			return empty;
		}

		template <bool execute_remaining = true>
		bool join() {
			return join<execute_remaining>(this, this + 1);
		}

		// get current thread's warp binding instance
		static grid_warp_t* get_current_warp() noexcept {
			return get_current_warp_internal();
		}

		async_worker_t& get_async_worker() noexcept {
			return async_worker;
		}

		const async_worker_t& get_async_worker() const noexcept {
			return async_worker;
		}

	protected:
		// get current warp index (saved in thread_local storage)
		// be aware of multi-dll linkage!
		static grid_warp_t*& get_current_warp_internal() noexcept {
			static thread_local grid_warp_t* current_warp = nullptr;
			return current_warp;
		}

		// execute all tasks scheduled at once.
		template <bool s, bool force = false>
		typename std::enable_if<s>::type execute_internal() noexcept(
			noexcept(std::declval<grid_warp_t>().flush()) && noexcept(std::declval<function_t>()())) {
			// mark for queueing, avoiding flush me more than once.
			queueing.store(queue_state_executing, std::memory_order_relaxed);

			// execute tasks in queue_buffer until suspended or interruption occurred
			queue_buffer_t& buffer = storage.queue_buffer;
			while (!buffer.empty()) {
				typename queue_buffer_t::type func = std::move(buffer.top());
				buffer.pop();

				func(); // we have already thread_fence acquired above

				if ((!force && suspend_count.load(std::memory_order_acquire) != 0)
					|| thread_warp.load(std::memory_order_relaxed) != &get_current_warp_internal()
					|| get_current_warp_internal() != this) {
					break;
				}

				if (!force && interrupting.load(std::memory_order_relaxed) != 0) {
					interrupting.store(0, std::memory_order_release);
					break;
				}
			}
		}

		template <bool s, bool force = false>
		typename std::enable_if<!s>::type execute_internal() noexcept(
			noexcept(std::declval<grid_warp_t>().flush()) && noexcept(std::declval<function_t>()())) {
			// mark for queueing, avoiding flush me more than once.
			queueing.store(queue_state_executing, std::memory_order_relaxed);

			// execute tasks in queue_buffer until suspended or interruption occurred
			std::vector<queue_buffer_t>& queue_buffers = storage.queue_buffers;
			for (size_t i = 0; i < queue_buffers.size(); i++) {
				queue_buffer_t& buffer = queue_buffers[i];
				while (!buffer.empty()) {
					typename queue_buffer_t::type func = std::move(buffer.top());
					buffer.pop(); // pop up before calling

					func(); // may throws exceptions

					if ((!force && suspend_count.load(std::memory_order_acquire) != 0)
						|| thread_warp.load(std::memory_order_relaxed) != &get_current_warp_internal()
						|| get_current_warp_internal() != this) {
						i = queue_buffers.size();
						break;
					}

					if (!force && interrupting.load(std::memory_order_relaxed) != 0) {
						interrupting.store(0, std::memory_order_release);
						break;
					}
				}
			}
		}

		template <bool s, bool force = false>
		void execute() noexcept(noexcept(std::declval<grid_warp_t>().template execute_internal<s, force>())) {
			if (suspend_count.load(std::memory_order_acquire) == 0) {
				// try to acquire execution, if it fails, there must be another thread doing the same thing
				// and it's ok to return immediately.
				preempt_guard_t preempt_guard(*this);
				if (preempt_guard) {
					if (suspend_count.load(std::memory_order_acquire) == 0) { // double check for suspend_count
						execute_internal<s, force>();

						if (!yield()) {
							// already yielded? try to repost me to process remaining tasks.
							flush();
						}
					} else {
						queueing.store(queue_state_pending, std::memory_order_relaxed);
					}
				}
			}
		}

		// commit execute request to specified worker.
		void flush() noexcept(noexcept(std::declval<grid_warp_t>().async_worker.queue(std::declval<function_t>()))) {
			// if current state is executing, the executing routine will reinvoke flush() if it detected pending state while exiting
			// so we just need to queue a flush routine as soon as current state is idle
			if (queueing.exchange(queue_state_pending, std::memory_order_acq_rel) == queue_state_idle) {
				async_worker.queue(execute_t(*this), priority);
			}
		}

		// queue task from specified thread.
		template <bool s, typename callable_t>
		typename std::enable_if<s>::type push(callable_t&& func) {
			do {
				std::lock_guard<std::mutex> guard(storage.mutex);
				storage.queue_buffer.push(std::forward<callable_t>(func));
			} while (false);

			flush();
		}

		template <bool s, typename callable_t>
		typename std::enable_if<!s>::type push(callable_t&& func) noexcept(
			noexcept(std::declval<queue_buffer_t>().push(std::forward<callable_t>(func))) &&
			noexcept(std::declval<grid_warp_t>().flush())) {
			size_t thread_index = async_worker.get_current_thread_index();
			if (thread_index != ~size_t(0)) {
				std::vector<queue_buffer_t>& queue_buffers = storage.queue_buffers;
				assert(thread_index < queue_buffers.size());
				queue_buffer_t& buffer = queue_buffers[thread_index];
				buffer.push(std::forward<callable_t>(func));

				// flush the task immediately
				flush();
			} else {
				assert(async_worker.is_terminated());
				assert(!storage.queue_buffers.empty());
				storage.queue_buffers[0].push(std::forward<callable_t>(func));
			}
		}

	protected:
		async_worker_t& async_worker; // host async worker
		std::atomic<grid_warp_t**> thread_warp; // save the running thread warp address.
		std::atomic<size_t> suspend_count; // current suspend count
		std::atomic<size_t> interrupting; // is interrupting by external request?
		std::atomic<size_t> queueing; // is flush request sent to async_worker? 0 : not yet, 1 : yes, 2 : is to flush right away.
		storage_t<queue_buffer_t, strand> storage; // task storage
		size_t priority;
		grid_warp_t* stack_next_warp;
	};

	// dispatcher based-on directed-acyclic graph
	template <typename warp_t>
	struct grid_dispatcher_t {
	protected:
		// wraps task data
		using function_t = typename warp_t::function_t;
		struct routine_data_t {
			template <typename func_t>
			routine_data_t(warp_t* w, size_t prior, func_t&& func)
				: routine(std::forward<func_t>(func)), total_lock_count(0), priority(prior), warp(w) {}

			function_t routine;
			std::vector<size_t> next_routines;
			size_t total_lock_count;
			size_t priority;
			warp_t* warp;
		};

		// on execution of tasks
		template <typename callback_t>
		struct execute_t {
			template <typename func_t>
			execute_t(grid_dispatcher_t& d, size_t i, func_t&& f) noexcept : dispatcher(d), id(i), callback(std::forward<func_t>(f)) {}

			void operator () () {
				callback();
				dispatcher.complete(id);
			}

			grid_dispatcher_t& dispatcher;
			size_t id;
			callback_t callback;
		};

		// mark runtime task dependency
		struct routine_runtime_t {
			std::atomic<size_t> current_lock_count;
		};

		// for exception safe, roll back atomic operations as needed
		enum guard_operation {
			add, sub, invalidate
		};

		template <guard_operation operation>
		struct atomic_guard_t {
			atomic_guard_t(std::atomic<size_t>& var) : variable(&var) {}
			~atomic_guard_t() noexcept {
				if (variable != nullptr) {
					if /* constexpr */ (operation == add) {
						variable->fetch_add(1, std::memory_order_release);
					} else if /* constexpr */ (operation == sub) {
						variable->fetch_sub(1, std::memory_order_release);
					} else {
						variable->store(~size_t(0), std::memory_order_release);
					}
				}
			}

			void cleanup() {
				variable = nullptr;
			}

		private:
			std::atomic<size_t>* variable;
		};

	public:
		using async_worker_t = typename warp_t::async_worker_t;

		// all_complete will be called each time all tasks complete
		template <typename func_t>
		grid_dispatcher_t(async_worker_t& worker, func_t&& all_complete) noexcept
			: async_worker(worker), completion(std::forward<func_t>(all_complete)) {
			pending_count.store(0, std::memory_order_release);
		}

		grid_dispatcher_t(async_worker_t& worker) noexcept : async_worker(worker) {
			pending_count.store(0, std::memory_order_release);
		}

		// queue a routine, notice that priority takes effect if and only if warp == 0
		template <typename func_t>
		size_t queue_routine(warp_t* warp, func_t&& func, size_t priority = 0) {
			assert(get_pending_count() == 0);
			size_t id = data.size();
			data.emplace_back(warp, priority, execute_t<typename std::remove_reference<func_t>::type>(*this, id, std::forward<func_t>(func)));

			return id;
		}

		// queue an empty routine as junction node
		size_t queue_routine() {
			assert(get_pending_count() == 0);
			size_t id = data.size();
			data.emplace_back(nullptr, 0, []() noexcept {});

			return id;
		}

		// set routine dependency [from] -> [to]
		void order(size_t from, size_t to) {
			assert(get_pending_count() == 0);
			assert(from < data.size() && to < data.size());
			routine_data_t& from_data = data[from];
			routine_data_t& to_data = data[to];
			from_data.next_routines.emplace_back(to);
			to_data.total_lock_count++;
		}

		// suspend a task temporarily, must called before it actually runs
		void suspend(size_t id) noexcept {
			size_t pending = pending_count.fetch_add(1, std::memory_order_acquire);
			assert(pending != 0);
			assert(id < data.size());
			size_t count = runtime[id].current_lock_count.fetch_add(1, std::memory_order_acquire);
			assert(count != 0);
		}

		// resume a task previously suspended by suspend()
		void resume(size_t id) {
			assert(pending_count.load(std::memory_order_acquire) != 0);
			assert(id < data.size());

			std::atomic<size_t>& counter = runtime[id].current_lock_count;
			atomic_guard_t<sub> pending_guard(pending_count); // must not sub to zero
			atomic_guard_t<invalidate> guard(counter);
			if (counter.fetch_sub(1, std::memory_order_release) == 1) {
				dispatch(id);
			}
			
			guard.cleanup();
			pending_guard.cleanup();

			finalize();
		}

		// flush all tasks, must be called after previous running
		void flush() {
			assert(get_pending_count() == 0);
			assert(validate());

			if (data.size() != runtime.size()) {
				// avoid std::atomic move
				std::vector<routine_runtime_t> new_runtime(data.size());
				std::swap(runtime, new_runtime);
			}

			for (size_t i = 0; i < runtime.size(); i++) {
				assert(runtime[i].current_lock_count.load(std::memory_order_acquire) == 0);
				runtime[i].current_lock_count.store(data[i].total_lock_count + 1, std::memory_order_relaxed);
			}

			pending_count.fetch_add(1, std::memory_order_release);

			// dispatch non-blocking routines
			for (size_t i = 0; i < runtime.size(); i++) {
				if (runtime[i].current_lock_count.fetch_sub(1, std::memory_order_relaxed) == 1) {
					dispatch(i);
				}
			}

			finalize();
		}

		size_t get_pending_count() const {
			return pending_count.load(std::memory_order_acquire);
		}

		// resurrect from exception on complete()
		void resurrect() {
			assert(pending_count.load(std::memory_order_acquire) != 0);

			// dispatch unfinished routines
			for (size_t i = 0; i < runtime.size(); i++) {
				std::atomic<size_t>& counter = runtime[i].current_lock_count;
				if (counter.load(std::memory_order_relaxed) == ~size_t(0)) {
					dispatch(i);
					// mark as dispatched, it is important because dispatch(i) may still throw exceptions
					counter.store(0, std::memory_order_relaxed);
				}
			}

			finalize();
		}

	protected:
		// check graph cycle
		bool validate() const {
			std::vector<bool> visited(data.size(), false);
			std::vector<bool> iterated(data.size(), false);

			// mark all root nodes
			std::vector<size_t> next;
			for (size_t i = 0; i < data.size(); i++) {
				if (data[i].total_lock_count == 0) {
					visited[i] = true;
					next.push_back(i);
				}
			}

			// iterate remaining nodes
			while (!next.empty()) {
				size_t m = next.back();
				next.pop_back();
				iterated[m] = true;
				
				routine_data_t dt = data[m];
				for (size_t i = 0; i < dt.next_routines.size(); i++) {
					size_t n = dt.next_routines[i];
					if (!visited[n]) {
						visited[n] = true;
						next.push_back(n);

						// visit an already-iterated node, cycle detected
						if (iterated[n])
							return false;
					}
				}
			}

			return std::find(visited.begin(), visited.end(), false) == visited.end();
		}

		// proceed to cleanup remaining counters, but not to dispatch them
		void unfinish_complete(size_t id, size_t start) noexcept {
			routine_data_t& from_data = data[id];
			if (start < from_data.next_routines.size()) {
				size_t next = from_data.next_routines[start];
				std::atomic<size_t>& counter = runtime[next].current_lock_count;
				assert(counter.load(std::memory_order_acquire) == 0);
				counter.store(~size_t(0), std::memory_order_relaxed);

				for (size_t i = start + 1; i < from_data.next_routines.size(); i++) {
					size_t next = from_data.next_routines[i];
					// mark as unfinished
					std::atomic<size_t>& counter = runtime[next].current_lock_count;
					if (counter.fetch_sub(1, std::memory_order_relaxed) == 1) {
						counter.store(~size_t(0), std::memory_order_relaxed);
					}
				}

				std::atomic_thread_fence(std::memory_order_release);
			}
		}

		// guard for exception on dispatching
		struct dispatch_guard_t {
			dispatch_guard_t(grid_dispatcher_t* disp, size_t id, size_t& i) : dispatcher(disp), task_id(id), index(i) {}
			~dispatch_guard_t() {
				dispatcher->unfinish_complete(task_id, index);
			}

		private:
			grid_dispatcher_t* dispatcher;
			size_t task_id;
			size_t& index;
		};

		// after finshing a routine, unlock the next_routines
		void complete(size_t id) {
			routine_data_t& from_data = data[id];
			size_t i = 0;
			dispatch_guard_t guard(this, id, i);

			for (i = 0; i < from_data.next_routines.size(); i++) {
				size_t next = from_data.next_routines[i];
				if (runtime[next].current_lock_count.fetch_sub(1, std::memory_order_release) == 1) {
					dispatch(next);
				}
			}

			finalize();
		}

		void finalize() {
			// all pending routines finished?
			if (pending_count.fetch_sub(1, std::memory_order_release) == 1) {
				for (size_t i = 0; i < runtime.size(); i++) {
					assert(runtime[i].current_lock_count.load(std::memory_order_acquire) == 0);
				}

				// if completion throws exception, we still do not care about pending_count anyway
				if (completion) {
					completion(*this);
				}
			}
		}

		// dispatch a routine by id
		void dispatch(size_t id) {
			routine_data_t to_data = data[id];
			atomic_guard_t<sub> guard(pending_count);
			pending_count.fetch_add(1, std::memory_order_release);

			// if not a warped routine, queue it to worker directly.
			if (to_data.warp == nullptr) {
				async_worker.queue(to_data.routine, to_data.priority);
			} else {
				to_data.warp->queue_routine(to_data.routine);
			}

			guard.cleanup();
		}

	protected:
		async_worker_t& async_worker;
		std::vector<routine_data_t> data;
		std::vector<routine_runtime_t> runtime;
		std::atomic<size_t> pending_count;
		std::function<void(grid_dispatcher_t&)> completion;
	};

	// here we code a trivial worker demo
	// could be replaced by your implementation
	template <typename thread_t = std::thread, typename lifetime_t = size_t, typename callback_t = std::function<void()>, template <typename...> typename allocator_t = grid_default_block_allocator_t>
	struct grid_async_worker_t {
		// task wrapper
		struct alignas(64) task_t {
			template <typename func_t>
			task_t(func_t&& func, task_t* n) noexcept(noexcept(callback_t(std::forward<func_t>(func))))
				: task(std::forward<func_t>(func)), next(n) {}

			task_t(task_t&& rhs) noexcept : task(std::move(rhs.task)), next(rhs.next) {
				rhs.next = nullptr;
			}

			task_t& operator = (task_t&& rhs) noexcept {
				task = std::move(rhs.task);
				next = rhs.next;
				rhs.next = nullptr;
				return *this;
			}

			callback_t task;
			task_t* next;
		};

		static constexpr size_t task_head_duplicate_count = 4;
		using task_allocator_t = allocator_t<task_t>;
		using task_lifetime_t = lifetime_t;

		explicit grid_async_worker_t(size_t thread_count) : threads(thread_count), internal_thread_count(thread_count) {
			waiting_thread_count = 0;
			limit_count = 0;
			running_count.store(0, std::memory_order_relaxed);
			terminated.store(1, std::memory_order_release);
		}

		grid_async_worker_t(size_t thread_count, const task_allocator_t& alloc) : task_allocator(alloc), threads(thread_count), internal_thread_count(thread_count) {
			waiting_thread_count = 0;
			limit_count = 0;
			running_count.store(0, std::memory_order_relaxed);
			terminated.store(1, std::memory_order_release);
		}

		// initialize and start thread poll
		void start() {
			assert(task_heads.empty()); // must not started

			std::vector<std::atomic<task_t*>> heads(threads.size() * task_head_duplicate_count);
			for (size_t i = 0; i < heads.size(); i++) {
				heads[i].store(nullptr, std::memory_order_relaxed);
			}

			task_heads = std::move(heads);
			terminated.store(0, std::memory_order_release);

			for (size_t i = 0; i < internal_thread_count; i++) {
				threads[i] = thread_t([this, i]() {
					try {
						get_current() = this;
						get_current_thread_index_internal() = i;

						lifetime_t live(i);
						while (!is_terminated()) {
							if (!poll()) {
								delay();
							}
						}

						(void)live; // avoid compiler warnings
					} catch (std::bad_alloc&) {
						throw; // by default, terminate
					} catch (std::exception&) {
						throw;
					}
				});
			}
		}

		// guard for exception on wait_for
		struct waiting_guard_t {
			waiting_guard_t(grid_async_worker_t* w) noexcept : worker(w) {
				++worker->waiting_thread_count;
			}

			~waiting_guard_t() {
				--worker->waiting_thread_count;
			}

		private:
			grid_async_worker_t* worker;
		};

		friend struct waiting_guard_t;

		// append new customized thread to worker
		// must be called before start()
		// notice that we allow a dummy thread placeholder here (if you just want to do polling outside the internal threads)
		template <typename... args_t>
		size_t append(args_t&&... args) {
			assert(is_terminated());
			size_t id = threads.size();
			threads.emplace_back(std::forward<args_t>(args)...);
			return id;
		}

		// get thread instance of given id
		thread_t& get(size_t i) noexcept {
			return threads[i];
		}

		// wait for new task with timeout specified by `milliseconds`
		// usually used in your customized thread procedures
		void delay(size_t millseconds) {
			if (!is_terminated()) {
				std::unique_lock<std::mutex> lock(mutex);
				// waiting_guard_t guard(this); // the external delay is not encounting waiting count
				// std::atomic_thread_fence(std::memory_order_release);

				if (fetch(threads.size()).first == ~size_t(0)) {
					condition.wait_for(lock, std::chrono::milliseconds(millseconds));
				}
			}
		}

		// guard for exceptions on polling
		struct poll_guard_t {
			poll_guard_t(task_allocator_t& alloc, task_t* t) noexcept : allocator(alloc), task(t) {}
			~poll_guard_t() {
				// do cleanup work
				task->~task_t();
				allocator.deallocate(task, 1);
			}

			task_allocator_t& allocator;
			task_t* task;
		};

		// poll any task from thread poll manually
		bool poll() {
			size_t priority_index = running_count.fetch_add(1, std::memory_order_acquire);
			running_guard_t guard(running_count);
			return poll_internal(threads.size() - std::min(priority_index, threads.size()));
		}

		// poll any task from thread poll manually with given priority
		bool poll(size_t priority_index) {
			running_count.fetch_add(1, std::memory_order_acquire);
			running_guard_t guard(running_count);
			return poll_internal(priority_index);
		}

		// guard for exception on running
		struct running_guard_t {
			std::atomic<size_t>& count;
			running_guard_t(std::atomic<size_t>& var) noexcept : count(var) {}
			~running_guard_t() { count.fetch_sub(1, std::memory_order_release); }
		};

		~grid_async_worker_t() {
			terminate();
			join();
		}

		// get current thread index, be-aware of dll-linkage!
		size_t get_current_thread_index() const noexcept { return get_current_thread_index_internal(); }

		// get the count of threads in worker, including customized threads
		size_t get_thread_count() const noexcept {
			return threads.size();
		}

		// get the count of waiting task
		size_t get_task_count() const noexcept {
			return task_count.load(std::memory_order_acquire);
		}

		// limit the count of running thread. e.g. 0 is not limited, 1 is to pause one thread from running, etc.
		void limit(size_t count) noexcept {
			limit_count = count;
		}

		// queue a task to worker with given priority [0, thread_count - 1], which 0 is the highest priority
		template <typename callable_t>
		void queue(callable_t&& func, size_t priority = 0) {
			if (!is_terminated()) {
				assert(!threads.empty());
				priority = std::min(priority, threads.size() - 1);
				task_t* task = task_allocator.allocate(1);
				new (task) task_t(std::forward<callable_t>(func), nullptr);
				task_count.fetch_add(1, std::memory_order_relaxed);

				// try empty slots first
				size_t index = 0;
				ptrdiff_t max_diff = std::numeric_limits<ptrdiff_t>::min();
				size_t thread_count = threads.size();
				for (size_t n = 0; n < task_head_duplicate_count; n++) {
					std::atomic<task_t*>& task_head = task_heads[priority + n * thread_count];
					task_t* expected = nullptr;
					if (task_head.compare_exchange_strong(expected, task, std::memory_order_release)) {
						// dispatch immediately
						if (waiting_thread_count > priority + limit_count) {
							wakeup_one();
						}

						return;
					} else {
						ptrdiff_t diff = task - expected;
						if (diff >= max_diff) {
							max_diff = diff;
							index = n;
						}
					}
				}

				// full, chain to farest one
				std::atomic<task_t*>& task_head = task_heads[priority + index * thread_count];

				// avoid legacy compiler bugs
				// see https://en.cppreference.com/w/cpp/atomic/atomic/compare_exchange
				task_t* node = task_head.load(std::memory_order_relaxed);
				do {
					task->next = node;
				} while (!task_head.compare_exchange_weak(node, task, std::memory_order_acq_rel, std::memory_order_relaxed));

				// dispatch immediately
				if (waiting_thread_count > priority + limit_count) {
					wakeup_one();
				}
			} else {
				// terminate requested, chain to default task_head at 0
				if (!task_heads.empty()) {
					task_t* task = task_allocator.allocate(1);
					new (task) task_t(std::forward<callable_t>(func), nullptr);
					task_count.fetch_add(1, std::memory_order_relaxed);

					std::atomic<task_t*>& task_head = task_heads[0];
					task_t* node = task_head.load(std::memory_order_relaxed);
					do {
						task->next = node;
					} while (!task_head.compare_exchange_weak(node, task, std::memory_order_acq_rel, std::memory_order_relaxed));
				} else {
					// terminate finished, just run at here
					assert(get_current_thread_index_internal() == ~size_t(0));
					func();
				}
			}
		}

		// mark as terminated
		void terminate() {
			terminated.store(1, std::memory_order_release);
			wakeup_all();
		}

		// is about to terminated
		bool is_terminated() const noexcept {
			return terminated.load(std::memory_order_acquire) != 0;
		}

		// wait for all threads in worker to be finished.
		void join() {
			if (!task_heads.empty()) {
				for (size_t i = 0; i < threads.size(); i++) {
					if (threads[i].joinable()) {
						threads[i].join();
					}
				}

				assert(running_count.load(std::memory_order_acquire) == 0);
				assert(waiting_thread_count == 0);
				while (!cleanup()) {}

				threads.clear();
				task_heads.clear();
				threads.resize(internal_thread_count);
			}
		}

		// notify threads in thread pool, usually used for customized threads
		void wakeup_one() {
			std::lock_guard<std::mutex> lock(mutex);
			condition.notify_one();
		}

		void wakeup_all() {
			std::lock_guard<std::mutex> lock(mutex);
			condition.notify_all();
		}

		// get_current worker instance be aware of multi-dll linkage!
		static grid_async_worker_t*& get_current() noexcept {
			static thread_local grid_async_worker_t* current_async_worker = nullptr;
			return current_async_worker;
		}

		// be aware of multi-dll linkage!
		static size_t& get_current_thread_index_internal() noexcept {
			static thread_local size_t current_thread_index = ~size_t(0);
			return current_thread_index;
		}

	protected:
		// blocked delay for any task
		void delay() {
			if (!is_terminated()) {
				std::unique_lock<std::mutex> lock(mutex);
				waiting_guard_t guard(this);

				if (fetch(threads.size()).first == ~size_t(0)) {
					if (!is_terminated()) {
						condition.wait(lock);
					}
				}
			}
		}

		// cleanup all pending tasks
		bool cleanup() noexcept {
			bool empty = true;

			for (size_t i = 0; i < task_heads.size(); i++) {
				std::atomic<task_t*>& task_head = task_heads[i];
				task_t* task = task_head.exchange(nullptr, std::memory_order_acquire);
				empty = empty && (task == nullptr);
				while (task != nullptr) {
					task_t* p = task;
					task = task->next;
					p->task();
					p->~task_t();
					task_allocator.deallocate(p, 1);
				}
			}

			return empty;
		}

		// try fetching a task with given priority
		std::pair<size_t, size_t> fetch(size_t priority_index) const noexcept {
			size_t thread_count = threads.size();
			for (size_t k = 0; k < task_head_duplicate_count; k++) {
				for (size_t n = 0; n < priority_index; n++) {
					size_t i = k * thread_count + n;
					if (task_heads[i].load(std::memory_order_acquire) != nullptr) {
						return std::make_pair(i, n);
					}
				}
			}

			return std::make_pair(~size_t(0), ~size_t(0));
		}

		// poll with given priority
		bool poll_internal(size_t priority_index) {
			std::pair<size_t, size_t> slot = fetch(priority_index);
			size_t index = slot.first;

			if (index != ~size_t(0)) {
				size_t priority = slot.second;
				std::atomic<task_t*>& task_head = task_heads[index];
				if (task_head.load(std::memory_order_acquire) != nullptr) {
					// fetch a task atomically
					task_t* task = task_head.exchange(nullptr, std::memory_order_acquire);
					if (task != nullptr) {
						task_t* org = task_head.exchange(task->next, std::memory_order_release);

						// return the remaining
						if (org != nullptr) {
							do {
								task_t* next = org->next;

								// avoid legacy compiler bugs
								// see https://en.cppreference.com/w/cpp/atomic/atomic/compare_exchange
								task_t* node = task_head.load(std::memory_order_relaxed);
								do {
									org->next = node;
								} while (!task_head.compare_exchange_weak(node, org, std::memory_order_relaxed, std::memory_order_relaxed));

								org = next;
							} while (org != nullptr);

							std::atomic_thread_fence(std::memory_order_acq_rel);
							if (waiting_thread_count > priority + limit_count) {
								wakeup_one();
							}
						}

						task_count.fetch_sub(1, std::memory_order_release);
						// in case task->task() throws exceptions
						poll_guard_t guard(task_allocator, task);
						task->task();
					}
				} else {
					std::this_thread::sleep_for(std::chrono::nanoseconds(1));
				}

				return true;
			} else {
				return false;
			}
		}

	protected:
		task_allocator_t task_allocator; // default task allocator
		std::vector<thread_t> threads; // worker
		std::atomic<size_t> running_count; // running_count
		std::vector<std::atomic<task_t*>> task_heads; // task pointer list
		std::mutex mutex; // mutex to protect condition
		std::condition_variable condition; // condition variable for idle wait
		std::atomic<size_t> terminated; // is to terminate
		size_t waiting_thread_count; // thread count of waiting on condition variable
		size_t limit_count; // limit the count of concurrently running thread
		size_t internal_thread_count; // the count of internal thread
		std::atomic<size_t> task_count; // the count of total waiting tasks 
	};

	template <typename async_worker_t>
	struct grid_async_balancer_t {
		grid_async_balancer_t(async_worker_t& worker, size_t size = 4u) : async_worker(worker), current_limit(0), window_size(static_cast<ptrdiff_t>(size)) {
			async_worker.limit(current_limit);
			balance.store(0, std::memory_order_release);
		}

		void down() noexcept {
			if (current_limit + 1 < async_worker.get_thread_count() && async_worker.get_task_count() == 0) {
				ptrdiff_t size = balance.load(std::memory_order_acquire);
				if (size + window_size < 0) {
					async_worker.limit(++current_limit);
					balance.fetch_add(window_size, std::memory_order_relaxed);
				} else {
					balance.fetch_sub(1, std::memory_order_relaxed);
				}
			}
		}

		void up() noexcept {
			if (current_limit != 0 && async_worker.get_task_count() > 0) {
				ptrdiff_t size = balance.load(std::memory_order_acquire);
				if (size > window_size) {
					async_worker.limit(--current_limit);
					balance.fetch_sub(window_size, std::memory_order_relaxed);
				} else {
					balance.fetch_add(1, std::memory_order_relaxed);
				}
			}
		}

	private:
		async_worker_t& async_worker;
		size_t current_limit;
		ptrdiff_t window_size;
		std::atomic<ptrdiff_t> balance;
	};
}

