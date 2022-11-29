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

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cassert>
#include <vector>

namespace grid {
	// meta operand for overlap rules with std::pair<begin, end> like bounding box
	template <typename element_t, typename prim_type = typename element_t::first_type, size_t element_count = prim_type::size * 2>
	struct grid_overlap_t {
		using scalar_t = typename prim_type::type;
		using index_t = size_t;
		using bound_t = element_t;
		static constexpr size_t size = element_count;

		static constexpr scalar_t get(const element_t& v, index_t index) noexcept {
			return reinterpret_cast<const scalar_t*>(&v)[index];
		}

		static constexpr scalar_t& get(element_t& v, index_t index) noexcept {
			return reinterpret_cast<scalar_t*>(&v)[index];
		}

		// compare element on given dimension (index)
		static constexpr bool compare(const element_t& lhs, const element_t& rhs, index_t index) noexcept {
			return get(rhs, index) < get(lhs, index);
		}

		// check if element range overlaps the left partition on given dimension (index)
		static constexpr bool overlap_left(const element_t& lhs, const element_t& rhs, index_t index) noexcept {
			return index < size / 2 || !(get(lhs, index) < get(rhs, index - size / 2));
		}

		// check if element range overlaps the right partition on given dimension (index)
		static constexpr bool overlap_right(const element_t& lhs, const element_t& rhs, index_t index) noexcept {
			return index >= size / 2 || !(get(rhs, index + size / 2) < get(lhs, index));
		}

		// hard to understand, do not waste time here.
		// split by given index
		template <bool right_skew>
		static scalar_t split_push(element_t& value, const element_t& reference, index_t index) noexcept {
			if (index < size / 2 != right_skew) {
				scalar_t& target = get(value, index);
				scalar_t save = target;
				target = get(reference, index);
				return save;
			} else {
				return get(value, index);
			}
		}

		// recover split
		static void split_pop(element_t& value, index_t index, scalar_t save) noexcept {
			get(value, index) = save;
		}

		static constexpr element_t bound(const element_t& lhs) {
			return lhs;
		}

		static void merge(element_t& lhs, const element_t& rhs) {
			for (size_t i = 0; i < size / 2; i++) {
				get(lhs, i) = std::min(get(lhs, i), get(rhs, i));
			}

			for (size_t i = size / 2; i < size; i++) {
				get(lhs, i) = std::max(get(lhs, i), get(rhs, i));
			}
		}

		static size_t interleave(const long* data, size_t len) {
			long level = sizeof(size_t) * 8 / (long)len;
			long bit_mask = 1 << level;
			size_t code = 0;

			for (long n = 0; n < level; n++) {
				for (size_t k = 0; k < len; k++) {
					code = (code << 1) | static_cast<size_t>(!!(data[k] & bit_mask));
				}

				bit_mask >>= 1;
			}

			return code;
		}

		static constexpr size_t next_index(size_t index) {
			return (index + 1) % size;
		}

		static size_t encode(const element_t& box, const element_t& value) {
			long level = sizeof(size_t) * 8 / (long)size;
			long range = (1 << (level + 1)) - 1;
			long quantized_values[size];

			for (size_t i = 0; i < size / 2; i++) {
				quantized_values[i] = std::max((long)0, std::min(range, (long)(range * (get(value, i) - get(box, i)) / (get(box, i + size / 2) - get(box, i)))));
			}

			for (size_t i = size / 2; i < size; i++) {
				quantized_values[i] = std::max((long)0, std::min(range, (long)((get(value, i) - get(box, i - size / 2)) / (get(box, i) - get(box, i - size / 2)))));
			}

			return interleave(quantized_values, size);
		}
	};

	// kd-tree with custom spatial structure
	// grid_tree_t is both a tree or a tree node
	// each grid_tree_t tags with an integer value `key_index` which specifies the comparing dimension on its kd-tree layer
	template <typename tree_key_t, typename meta = grid_overlap_t<tree_key_t>>
	struct grid_tree_t {
		using key_t = tree_key_t;
		using index_t = typename meta::index_t;

		explicit grid_tree_t(const key_t& k = key_t(), index_t i = 0) noexcept : key(k), key_index(i), left_node(nullptr), right_node(nullptr), parent_node(nullptr) {}

		// attach tree `t` to `this`
		void attach(grid_tree_t* t) noexcept {
			assert(t != nullptr && t != this);
			assert(t->left_node == nullptr && t->right_node == nullptr && t->get_parent() == nullptr);
			merge(t);
		}

		// detach `this` from its parent
		// selector is to determine which children (left/right) is selected to replace `this` on tree
		template <typename selector_t>
		grid_tree_t* detach(selector_t& selector) noexcept {
			grid_tree_t* new_root = nullptr;

			// try light detach first
			if (light_detach(new_root)) {
				return new_root;
			}

			assert(left_node != nullptr && right_node != nullptr);

			// find replacer tree node
			grid_tree_t* p = selector(left_node, right_node) ? right_node->find_minimal(get_index()) : left_node->find_maximal(get_index());
			assert(p != nullptr);
			new_root = p;

			// detach cascaded
			p->detach(selector);
			assert(p->get_parent() == nullptr);
			assert(p->left_node == nullptr && p->right_node == nullptr);

			// assign new topology relationship
			if (get_parent() != nullptr) {
				grid_tree_t** pp = get_parent()->left_node == this ? &get_parent()->left_node : &get_parent()->right_node;
				*pp = new_root;
				new_root = nullptr;
			}

			if (left_node != nullptr) {
				left_node->set_parent(p);
			}

			if (right_node != nullptr) {
				right_node->set_parent(p);
			}

			// replace links
			std::swap(key_index, p->key_index);
			std::swap(links, p->links);
			return new_root;
		}

		template <bool right_skew, typename query_key_t, typename queryer_t>
		bool query(query_key_t&& target_key, queryer_t&& queryer) noexcept(noexcept(queryer(std::declval<grid_tree_t&>()))) {
			for (grid_tree_t* p = this; p != nullptr; p = (right_skew ? p->right_node : p->left_node)) {
				if (!queryer(*p)) {
					return false;
				}

				if /* constexpr */ (right_skew) {
					if (p->left_node != nullptr && meta::overlap_left(p->key, target_key, p->get_index()) && !p->left_node->template query<right_skew>(target_key, queryer)) {
						return false;
					}

					if (!meta::overlap_right(p->key, target_key, p->get_index()))
						break;
				} else {
					if (p->right_node != nullptr && meta::overlap_right(p->key, target_key, p->get_index()) && !p->right_node->template query<right_skew>(target_key, queryer)) {
						return false;
					}

					if (!meta::overlap_left(p->key, target_key, p->get_index()))
						break;
				}
			}

			return true;
		}

		template <bool right_skew, typename query_key_t, typename queryer_t>
		bool query(query_key_t&& target_key, queryer_t&& queryer) const noexcept(noexcept(queryer(std::declval<const grid_tree_t&>()))) {
			return const_cast<grid_tree_t*>(this)->template query<right_skew>(std::forward<query_key_t>(target_key), std::forward<queryer_t>(queryer));
		}

		template <bool right_skew, typename query_key_t, typename queryer_t, typename culler_t>
		bool query(query_key_t&& target_key, queryer_t&& queryer, culler_t&& culler) noexcept(noexcept(queryer(std::declval<grid_tree_t&>())) && noexcept(culler(std::declval<const key_t&>()))) {
			for (grid_tree_t* p = this; p != nullptr; p = (right_skew ? p->right_node : p->left_node)) {
				if (!culler(target_key))
					break;

				// found the object wanted, break
				if (culler(p->get_key()) && !queryer(*p)) {
					return false;
				}

				// culling
				auto save = meta::template split_push<right_skew>(target_key, p->get_key(), p->get_index());

				if /* constexpr */ (right_skew) {
					// cull right in left node
					if (p->left_node != nullptr && !p->left_node->template query<right_skew>(target_key, queryer, culler)) {
						return false;
					}
				} else {
					// cull left in right node
					if (p->right_node != nullptr && !p->right_node->template query<right_skew>(target_key, queryer, culler)) {
						return false;
					}
				}

				meta::split_pop(target_key, p->get_index(), save);
			}

			return true;
		}

		template <bool right_skew, typename query_key_t, typename queryer_t, typename culler_t>
		bool query(query_key_t&& target_key, queryer_t&& queryer, culler_t&& culler) const noexcept(noexcept(queryer(std::declval<const grid_tree_t&>())) && noexcept(culler(std::declval<const key_t&>()))) {
			return const_cast<grid_tree_t*>(this)->template query<right_skew>(std::forward<query_key_t>(target_key), std::forward<queryer_t>(queryer), std::forward<culler_t>(culler));
		}

		const key_t& get_key() const noexcept {
			return key;
		}

		void set_key(const key_t& k) noexcept {
			key = k;
		}

		index_t get_index() const noexcept {
			return key_index;
		}

		grid_tree_t* get_parent() const noexcept {
			return parent_node;
		}

		void set_parent(grid_tree_t* t) noexcept {
			parent_node = t;
		}

		struct tree_code_t {
			explicit tree_code_t(grid_tree_t* t = nullptr, size_t c = 0) noexcept : code(c), tree(t) {}
			bool operator < (const tree_code_t& rhs) const noexcept { return code < rhs.code; }
			size_t code;
			grid_tree_t* tree;
		};

		grid_tree_t* optimize() {
			// collect all nodes over the tree
			std::vector<tree_code_t> all_nodes;
			all_nodes.emplace_back(tree_code_t(this));
			size_t n = 0;
			typename meta::bound_t box = meta::bound(key);

			while (n < all_nodes.size()) {
				grid_tree_t* tree = all_nodes[n++].tree;
				meta::merge(box, tree->key);

				if (tree->left_node != nullptr) {
					all_nodes.emplace_back(tree_code_t(tree->left_node));
				}

				if (tree->right_node != nullptr) {
					all_nodes.emplace_back(tree_code_t(tree->right_node));
				}

				memset(&tree->links, 0, sizeof(tree->links));
			}

			// zip encoding
			for (size_t i = 0; i < n; i++) {
				tree_code_t& treeCode = all_nodes[i];
				treeCode.code = meta::encode(box, treeCode.tree->key);
			}

			std::sort(all_nodes.begin(), all_nodes.end());

			// reconstruct tree
			tree_code_t* root = &all_nodes[all_nodes.size() / 2];
			root->tree->key_index = 0;
			build(root->tree, &all_nodes[0], root, 1);
			build(root->tree, root + 1, &all_nodes[0] + all_nodes.size(), 1);

			return root->tree;
		}

	protected:
		void build(grid_tree_t* root, tree_code_t* begin, tree_code_t* end, size_t index) {
			if (begin < end) {
				tree_code_t* mid = begin + (end - begin) / 2;
				mid->tree->key_index = index;
				root->attach(mid->tree);

				index = meta::next_index(index);
				build(root, begin, mid, index);
				build(root, mid + 1, end, index);
			}
		}

		// if left_node == nullptr or right_node == nullptr, then we perform light_detach (without adjust children tree
		bool light_detach(grid_tree_t*& new_root) noexcept {
			new_root = nullptr;
			if (left_node != nullptr) {
				if (right_node != nullptr) {
					// not ok, go complicated way
					return false;
				} else {
					// only left_node
					left_node->set_parent(get_parent());
					new_root = left_node;
					left_node = nullptr;
				}
			} else if (right_node != nullptr) {
				// only right_node
				right_node->set_parent(get_parent());
				new_root = right_node;
				right_node = nullptr;
			}

			// detach from parent
			if (get_parent() != nullptr) {
				grid_tree_t** pp = get_parent()->left_node == this ? &get_parent()->left_node : &get_parent()->right_node;
				*pp = new_root;

				new_root = nullptr;
				set_parent(nullptr);
			}

			return true;
		}

		// find minimal value on dimension (index) over subtrees
		grid_tree_t* find_minimal(index_t index) noexcept {
			grid_tree_t* p = this;
			if (left_node != nullptr) {
				grid_tree_t* compare = left_node->find_minimal(index);
				if (meta::compare(p->key, compare->key, index)) {
					p = compare;
				}
			}

			// if index == get_index(), then right_node must be greater than this, skip
			if (index != get_index() && right_node != nullptr) {
				grid_tree_t* compare = right_node->find_minimal(index);
				if (meta::compare(p->key, compare->key, index)) {
					p = compare;
				}
			}

			return p;
		}

		// find maximal value on dimension (index) over subtrees
		grid_tree_t* find_maximal(index_t index) noexcept {
			grid_tree_t* p = this;
			// if index == get_index(), then left_node must be less than this, skip
			if (index != get_index() && left_node != nullptr) {
				grid_tree_t* compare = left_node->find_maximal(index);
				if (!meta::compare(p->key, compare->key, index)) {
					p = compare;
				}
			}

			if (right_node != nullptr) {
				grid_tree_t* compare = right_node->find_maximal(index);
				if (!meta::compare(p->key, compare->key, index)) {
					p = compare;
				}
			}

			return p;
		}

		// merge t to this
		void merge(grid_tree_t* t) noexcept {
			assert(t->get_parent() == nullptr);
			// which branch should be selected?
			bool left = meta::compare(key, t->key, get_index());

			grid_tree_t** ptr = left ? &left_node : &right_node;
			if (*ptr == nullptr) {
				*ptr = t;
				t->set_parent(this);
			} else {
				// merge recursively
				(*ptr)->merge(t);
			}
		}

		key_t key;
		index_t key_index;
		union {
			struct {
				grid_tree_t* parent_node;
				grid_tree_t* left_node;
				grid_tree_t* right_node;
			};
			struct {
				grid_tree_t* links[3];
			} links;
		};
	};
}

