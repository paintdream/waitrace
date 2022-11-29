#include "grid_buffer.h"
#include <vector>
using namespace grid;

int main(void) {
	grid_bytes_t bytes;
	grid_buffer_t<uint8_t> buffer;
	grid_cache_t<uint8_t> cache;
	grid_cache_allocator_t<double, uint8_t> allocator(&cache);

	char var[] = "12345";
	bytes = grid_bytes_t::make_view(reinterpret_cast<const uint8_t*>(var), 5);
	bytes.test(15);
	bytes.set(16); // breaks const rule ...
	buffer = grid_bytes_t::make_view(reinterpret_cast<const uint8_t*>("1234568901234567890"), 20);
	cache.link(bytes, buffer);
	grid_bytes_t combined;
	combined.resize(bytes.get_view_size());
	combined.copy(0, bytes);

	// todo: more tests
	std::vector<double, grid_cache_allocator_t<double, uint8_t>> vec(allocator);
	vec.push_back(1234.0f);
	vec.resize(777);

	std::vector<double> dbl_vec;
	grid::grid_binary_insert(dbl_vec, 1234.0f);
	auto it = grid::grid_binary_find(dbl_vec.begin(), dbl_vec.end(), 1234.0f);
	assert(it != dbl_vec.end());
	grid::grid_binary_erase(dbl_vec, 1234.0f);

	std::vector<grid::grid_key_value_t<int, const char*>> str_vec;
	grid::grid_binary_insert(str_vec, grid::grid_make_key_value(1234, "asdf"));
	grid::grid_binary_insert(str_vec, grid::grid_make_key_value(2345, "defa"));
	auto it2 = grid::grid_binary_find(str_vec.begin(), str_vec.end(), 1234);
	assert(it2 != str_vec.end());
	assert(grid::grid_binary_find(str_vec.begin(), str_vec.end(), 1236) == str_vec.end());
	grid::grid_binary_erase(str_vec, 1234);
	grid::grid_binary_erase(str_vec, grid::grid_make_key_value(1234, ""));

	return 0;
}

