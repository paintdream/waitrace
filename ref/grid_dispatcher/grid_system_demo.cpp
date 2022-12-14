#include "grid_system.h"
using namespace grid;

using warp_t = grid_warp_t<grid_async_worker_t<>>;
using entity_t = uint32_t;
using entity_allocator = grid_entity_allocator_t<entity_t>;

struct grid_component_matrix_t {
	float values[4][4];
};

template <typename element_t>
using block_allocator_t = grid_block_allocator_t<element_t>;

int main(void) {
	static constexpr size_t thread_count = 8;
	static constexpr size_t warp_count = 16;
	grid_async_worker_t<> worker(thread_count);
	worker.start();

	std::vector<warp_t> warps;
	warps.reserve(warp_count);
	for (size_t i = 0; i < warp_count; i++) {
		warps.emplace_back(worker);
	}

	grid_system_t<entity_t, block_allocator_t, grid_component_matrix_t, size_t> matrix_system;
	std::vector<entity_t> entities;
	grid_entity_allocator_t<entity_t> allocator;

	for (size_t k = 0; k < 128; k++) {
		entity_t entity = allocator.allocate();
		matrix_system.insert(entity, grid_component_matrix_t(), k);
		entities.emplace_back(entity);
	}

	for (size_t m = 0; m < 32; m++) {
		entity_t entity = entities[m] * 4;
		matrix_system.remove(entity);
		allocator.free(entity);
	}

	for (size_t m = 0; m < 64; m++) {
		matrix_system.insert(allocator.allocate(), grid_component_matrix_t(), m);
	}

	float sum = 0;
	for (auto&& item : matrix_system.component<grid_component_matrix_t>()) {
		sum += item.values[0][0];
	}

	allocator.reset();
	printf("Sum should be zero: %f\n", sum);
	
	// test for running example from thread pool
	std::atomic<size_t> counter;
	counter.store(0, std::memory_order_release);

	warps[0].queue_routine_external([&worker, &matrix_system, &counter]() {
		counter.fetch_add(matrix_system.size(), std::memory_order_release);
		matrix_system.for_each_parallel<grid_component_matrix_t, warp_t>([&worker, &counter](grid_component_matrix_t& matrix) {
			// initialize with identity matrix
			printf("[%d] Initialize matrix: %p\n", (int)worker.get_current_thread_index(), &matrix);

			matrix.values[0][0] = 1; matrix.values[0][1] = 0; matrix.values[0][2] = 0; matrix.values[0][3] = 0;
			matrix.values[1][0] = 0; matrix.values[1][1] = 1; matrix.values[1][2] = 0; matrix.values[1][3] = 0;
			matrix.values[2][0] = 0; matrix.values[2][1] = 0; matrix.values[2][2] = 1; matrix.values[2][3] = 0;
			matrix.values[3][0] = 0; matrix.values[3][1] = 0; matrix.values[3][2] = 0; matrix.values[3][3] = 1;

			if (counter.fetch_sub(1, std::memory_order_acquire) == 1) {
				worker.terminate();
			}
		}, 64);
	});

	grid_system_t<entity_t, block_allocator_t, float, size_t> other_system;
	for (size_t k = 0; k < 5; k++) {
		other_system.insert(grid::grid_verify_cast<entity_t>(k), 0.1f, k);
	}

	using sys_t = grid_systems_t<block_allocator_t, decltype(matrix_system), decltype(other_system)>;
	sys_t systems(matrix_system, other_system);
	size_t count = 0;
	for (auto&& v : systems.components<size_t>()) {
		count++;
	}

	size_t batch_count = 0;
	systems.components<size_t>().for_each([&](const size_t* v, size_t n) { batch_count += n; }); // much faster
	assert(count == batch_count);

	{
		auto& w = systems;
		for (auto&& v : w.components<float>()) {
			float& f = v;
			count++;
		}

		w.components<float, size_t>().for_each([&](float, size_t) {});
		
		for (auto&& v : w.components<float, size_t>()) {
			float& f = std::get<0>(v);
			f = 1.0f;
			size_t& s = std::get<1>(v);
			s = 2;
			count++;
		}

		w.components<float>().for_each([&count](float) {
			count++;
		});

		w.components<size_t, float>().for_each([&count](size_t, float) {
			count++;
		});

		w.components<size_t>().for_each_system([&count](sys_t::component_queue_t<size_t>& s) {
			count++;
		});
	}
	{
		const auto& w = systems;
		for (auto&& v : w.components<float>()) {
			const float& f = v;
			count++;
		}

		for (auto&& v : w.components<float, size_t>()) {
			const float& f = std::get<0>(v);
			const size_t& s = std::get<1>(v);
			count++;
		}

		w.components<float>().for_each([&count](float) {
			count++;
		});

		w.components<size_t, float>().for_each([&count](size_t, float) {
			count++;
		});

		w.components<size_t>().for_each_system([&count](const sys_t::component_queue_t<size_t>& s) {
			count++;
		});
	}

	worker.join();

	// finished!
	while (!warp_t::join(warps.begin(), warps.end())) {}
	return 0;
}

