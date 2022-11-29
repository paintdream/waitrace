#include "grid_coroutine.h"
#include "grid_buffer.h"
#include "grid_system.h"
#include <chrono>
#include <cstdio>
using namespace grid;

using worker_t = grid_async_worker_t<>;

struct engine_t {
	using warp_t = grid_warp_t<worker_t>;
	using frame_t = grid_frame_t<warp_t, worker_t>;
	using barrier_t = grid_barrier_t<warp_t, worker_t>;
	using event_t = grid_event_t<warp_t, worker_t>;
	using pipe_t = grid_pipe_t<int, warp_t, worker_t>;

	engine_t() : worker(std::thread::hardware_concurrency()), frame(worker), pipe(worker),
		prepare_barrier(worker, 2u), sync_event(worker),
		warp_audio(worker), warp_script(worker), warp_network(worker), warp_render(worker) {
		worker.start();

		coroutine_async().run();
		coroutine_tick().run();
	}

	~engine_t() {
		worker.terminate();
		worker.join();
	}

	grid_coroutine_t coroutine_async() {
		int index = 0;
		while (co_await frame) {
			// place a barrier here to assure the completion of sync_event.reset() in main coroutine happends before we use it
			printf("coroutine async prepare\n");
			co_await prepare_barrier;
			pipe.emplace(index++);

			// pretend to do something
			printf("coroutine async ticks\n");

			co_await grid_switch(&warp_audio);
			printf("coroutine async audio ticks\n");

			co_await sync_event;
			printf("coroutine async after event\n");

			co_await grid_switch(&warp_script);
			printf("coroutine async script ticks\n");

			// it will trigger a gcc 11 bug if you remove the following line but i didn't know why
			co_await grid_awaitable(&warp_render, std::function<void()>([]() { printf("coroutine async render ticks\n"); }));
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}

		printf("coroutine async exit\n");
	}

	template <typename element_t>
	using transient_vector = std::vector<element_t, grid_cache_allocator_t<element_t>>;
		
	template <typename element_t>
	using block_allocator_t = grid_block_allocator_t<element_t>;

	struct vector_t {
		float x, y, z;
	};

	struct position_t : vector_t {};
	struct velocity_t : vector_t {};

	struct attribute_t {
		float current_value;
		float max_value;
	};

	struct hitpoint_t : attribute_t {};
	struct manapoint_t : attribute_t {};
	struct parent_t {
		uint32_t id;
	};

	grid_coroutine_t coroutine_tick() {
		grid_cache_t<uint8_t> frame_cache;
		grid_system_t<uint32_t, block_allocator_t, parent_t, position_t, velocity_t> geo_system;
		grid_system_t<uint32_t, block_allocator_t, parent_t, hitpoint_t, manapoint_t> attribute_system;
		grid_systems_t<block_allocator_t, decltype(geo_system), decltype(attribute_system)> systems(geo_system, attribute_system);

		for (uint32_t i = 0; i < 32; i++) {
			geo_system.insert(i, parent_t{ i - 1 }, position_t{ 0.0f, 0.0f, float(i) }, velocity_t{ 0.0f, float(i), 0.0f });
			attribute_system.insert(i, parent_t{ i + 1 }, hitpoint_t{ 0.0f, float(i) }, manapoint_t{ 0.0f, float(i) });
		}

		while (co_await frame) {
			sync_event.reset();
			printf("coroutine main prepare\n");

			co_await prepare_barrier;

			int value = co_await pipe;
			printf("coroutine main pipe value %d\n", value);

			frame_cache.reset();
			printf("coroutine main ticks\n");
			transient_vector<std::function<void()>> callbacks(&frame_cache);
			callbacks.emplace_back([]() { printf("callback A\n"); });

			int count = 0;
			systems.components<parent_t>().for_each([&count](auto&& p) { count++; });
			printf("system component parent_t count: %d\n", count);

			co_await grid_switch(&warp_script);
			printf("coroutine main script ticks\n");
			callbacks.emplace_back([]() { printf("callback B\n"); });

			printf("coroutine main before event\n");
			sync_event.notify();

			co_await grid_awaitable_union(worker,
				grid_awaitable(&warp_render, std::function<void()>([]() { printf("coroutine parallel render ticks\n"); })),
				grid_awaitable(&warp_network, std::function<void()>([]() { printf("coroutine parallel network ticks\n"); })));
			
			for (auto&& callback : callbacks) {
				callback();
			}
		}

		printf("coroutine main exit\n");
	}

	bool dispatch(bool running) {
		printf("=================================\n");
		return frame.dispatch(running);
	}

protected:
	worker_t worker;
	frame_t frame;

	pipe_t pipe;
	barrier_t prepare_barrier;
	event_t sync_event;
	warp_t warp_audio;
	warp_t warp_script;
	warp_t warp_network;
	warp_t warp_render;
};

int main(void) {
	engine_t engine;

	while (engine.dispatch(rand() % 11 != 0)) {
		// simulate frame sync
		std::this_thread::sleep_for(std::chrono::milliseconds(rand() % 40));
	}

	return 0;
}
