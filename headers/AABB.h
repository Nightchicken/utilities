#ifndef AABB_H
#define AABB_H
#include "vec.h"
#include "arena.h"
#include <math.h>
#include <stddef.h>

/**
 * @file AABB.h
 * @brief Dynamic AABB tree (bounding volume hierarchy) in 2D and 3D.
 *
 * Comes in 2D (AABB2 / AABB2Tree, `aabb2_*` and `aabb2_tree_*` functions) and
 * 3D (AABB3 / AABB3Tree, `aabb3_*` and `aabb3_tree_*`) flavours with identical
 * APIs. Both are the classic Box2D-style tree: surface-area-heuristic
 * insertion, AVL-style rotation balancing, fat AABBs, overlap query and ray
 * cast.
 *
 * The node pool is carved once out of an Arena and addressed by index; free
 * slots form an intrusive linked list through the `next` field, so there is no
 * per-operation heap allocation. A tree of N leaves needs 2*N-1 nodes -- size
 * the pool accordingly.
 *
 * Objects are identified by an opaque *proxy id* (a node index) returned from
 * insert. Each leaf carries a list of caller-supplied `void*` items that
 * queries hand back; the item slabs are carved from the same Arena, so a leaf
 * holds up to `itemsPerLeaf` items and nothing is allocated per operation.
 *
 * An item is whatever you want -- an entity pointer, or another AABB tree, in
 * which case tag your payload so the callback can recognise it and query the
 * subtree itself. Traversal never descends into a nested tree on its own.
 *
 * Typical use:
 * @code
 *     Arena a = arena_create(1 << 20);
 *     AABB2Tree t = aabb2_tree_create(&a, 2 * MAX_BODIES - 1, 4, 0.1f);
 *
 *     int proxy = aabb2_tree_insert(&t, aabb2_make(lo, hi));
 *     if (proxy == AABB_NULL) { ... pool full ... }
 *     aabb2_tree_leaf_add(&t, proxy, entity);
 *
 *     aabb2_tree_move(&t, proxy, aabb2_make(newLo, newHi));  // each frame
 *     aabb2_tree_query(&t, queryBox, myCallback, &myState);
 *     arena_destroy(&a);
 * @endcode
 *
 * @note Queries test *fattened* boxes, so they over-report. Re-test against
 *       your tight box inside the callback when you need exact answers.
 */

/**
 * @brief Sentinel "no node" index, shared by both trees.
 *
 * Used for absent parents and children, an empty root, an exhausted free list,
 * and as the failure return of {aabb2,aabb3}_tree_insert(). Chosen as -1 so it
 * can never collide with a real node index.
 */
#define AABB_NULL (-1)

/**
 * @brief Integer max, used for recomputing node heights.
 *
 * @param a First value.
 * @param b Second value.
 * @return The larger of @p a and @p b; @p b when they are equal.
 */
static inline int aabb_max_int(int a, int b){ return a > b ? a : b; }

// ============================================================================
// 2D
// ============================================================================

/**
 * @brief 2D axis-aligned bounding box, stored as two opposite corners.
 *
 * @var AABB2::topLeft
 *   Low corner: the componentwise minimum over x and y. Callers building a
 *   box by hand must keep this <= @ref AABB2::bottomRight on every axis, or
 *   use aabb2_make(), which sorts the corners for you.
 * @var AABB2::bottomRight
 *   High corner: the componentwise maximum over x and y.
 */
typedef struct AABB2{
	Vec2f topLeft;
	Vec2f bottomRight;
}AABB2;

/**
 * @brief Build a box from two arbitrary corners.
 *
 * @param min One corner. Need not actually be the smaller one.
 * @param max The opposite corner. Need not actually be the larger one.
 * @return A box whose topLeft/bottomRight are the componentwise min/max of
 *         the two inputs, so the result is well formed on every axis.
 *
 * @note Use this rather than brace-initialising AABB2 when corner order is not
 *       already guaranteed -- an inverted box silently breaks every overlap
 *       and cost computation below.
 */
static inline AABB2 aabb2_make(Vec2f min, Vec2f max){
	AABB2 b;
	b.topLeft.x = fminf(min.x, max.x);
	b.topLeft.y = fminf(min.y, max.y);
	b.bottomRight.x = fmaxf(min.x, max.x);
	b.bottomRight.y = fmaxf(min.y, max.y);
	return b;
}

/**
 * @brief Smallest box enclosing both inputs.
 *
 * @param a First box. Must be well formed (topLeft <= bottomRight on every
 *          axis).
 * @param b Second box, same requirement.
 * @return The componentwise min of the low corners and max of the high
 *         corners. This is a bounding union, not a set union: it also covers
 *         the empty space between two disjoint boxes.
 */
static inline AABB2 aabb2_union(AABB2 a, AABB2 b){
	AABB2 r;
	r.topLeft.x = fminf(a.topLeft.x, b.topLeft.x);
	r.topLeft.y = fminf(a.topLeft.y, b.topLeft.y);
	r.bottomRight.x = fmaxf(a.bottomRight.x, b.bottomRight.x);
	r.bottomRight.y = fmaxf(a.bottomRight.y, b.bottomRight.y);
	return r;
}

/**
 * @brief Node cost used to steer tree insertion.
 *
 * @param b Box to measure. A well-formed box gives a non-negative result;
 *          an inverted one gives a meaningless (possibly negative) cost.
 * @return Perimeter of @p b, i.e. 2 * (width + height).
 *
 * @note This is the surface-area heuristic (perimeter, in 2D): insertion
 *       picks the descent that grows this quantity least, which keeps
 *       sibling boxes tight and queries cheap.
 */
static inline float aabb2_cost(AABB2 b){
	float wx = b.bottomRight.x - b.topLeft.x;
	float wy = b.bottomRight.y - b.topLeft.y;
	return 2.0f * (wx + wy);
}

/**
 * @brief Test whether two boxes intersect.
 *
 * @param a First box.
 * @param b Second box.
 * @return Non-zero if the boxes share any point, 0 if they are disjoint.
 *
 * @note The test is inclusive: boxes that merely touch on a face or edge
 *       count as overlapping.
 */
static inline int aabb2_overlaps(AABB2 a, AABB2 b){
	return !(a.bottomRight.x < b.topLeft.x || a.topLeft.x > b.bottomRight.x ||
	         a.bottomRight.y < b.topLeft.y || a.topLeft.y > b.bottomRight.y);
}

/**
 * @brief Test whether one box fully encloses another.
 *
 * @param a The outer box, the one that must do the containing.
 * @param b The inner box, tested for being inside @p a.
 * @return Non-zero if every point of @p b lies in @p a, 0 otherwise.
 *
 * @note Argument order matters and is easy to get backwards: this asks
 *       "does a contain b", not the reverse. Containment is inclusive, so a
 *       box contains itself.
 */
static inline int aabb2_contains(AABB2 a, AABB2 b){
	return a.topLeft.x <= b.topLeft.x && a.topLeft.y <= b.topLeft.y &&
	       b.bottomRight.x <= a.bottomRight.x && b.bottomRight.y <= a.bottomRight.y;
}

/**
 * @brief Expand a box outward on all sides.
 *
 * @param b      Box to grow.
 * @param margin Distance to push each face outward, in world units. Applied to
 *               all 2 axes. A negative margin shrinks the box instead, and
 *               can invert it if it exceeds half the box extent.
 * @return @p b with its low corner moved down by @p margin and its high corner
 *         up by @p margin on every axis.
 *
 * @note This is the "fat AABB" trick: leaves are stored slightly oversized so
 *       small movements stay inside their stored box and skip reinsertion.
 */
static inline AABB2 aabb2_fatten(AABB2 b, float margin){
	AABB2 r;
	r.topLeft.x = b.topLeft.x - margin;
	r.topLeft.y = b.topLeft.y - margin;
	r.bottomRight.x = b.bottomRight.x + margin;
	r.bottomRight.y = b.bottomRight.y + margin;
	return r;
}

/**
 * @brief One node of the tree pool -- either a leaf, an internal node, or free.
 *
 * @var AABB2Node::box
 *   For a leaf, the fattened box of the proxy. For an internal node, the union
 *   of both children's boxes. Undefined while the node is free.
 * @var AABB2Node::itemCount
 *   How many of this leaf's item slots are in use, from 0 to
 *   @ref AABB2Tree::itemsPerLeaf. The items themselves live in the tree's
 *   shared slab, reachable with aabb2_tree_leaf_items(). Meaningful on leaves
 *   only -- internal nodes hold 0.
 * @var AABB2Node::parent
 *   Index of the parent node, or ::AABB_NULL at the root.
 * @var AABB2Node::left
 *   Index of the left child, or ::AABB_NULL on a leaf.
 * @var AABB2Node::right
 *   Index of the right child, or ::AABB_NULL on a leaf.
 * @var AABB2Node::height
 *   0 for a leaf, -1 while the node is on the free list, otherwise
 *   1 + max(child heights). Doubles as the leaf/internal/free tag.
 * @var AABB2Node::next
 *   Free-list link to the next free slot while this node is free; unused, and
 *   not maintained, while the node is live.
 */
typedef struct AABB2Node{
	AABB2 box;
	int itemCount;
	int parent;
	int left;
	int right;
	int height;   // 0 for leaves, -1 when free, else 1 + max(child heights)
	int next;     // free-list link when free
}AABB2Node;

/**
 * @brief Dynamic bounding volume hierarchy over a fixed, arena-backed pool.
 *
 * @var AABB2Tree::nodes
 *   Node pool carved from an Arena by aabb2_tree_create(). Not owned: it dies
 *   with the arena, so never free() it. NULL if that allocation failed.
 * @var AABB2Tree::count
 *   Number of nodes currently in use, leaves and internal nodes together --
 *   not the number of proxies. A tree of N proxies uses 2*N-1 nodes.
 * @var AABB2Tree::capacity
 *   Total slots in @ref AABB2Tree::nodes, or 0 if the arena allocation failed.
 *   The hard ceiling on count.
 * @var AABB2Tree::root
 *   Index of the root node, or ::AABB_NULL while the tree is empty.
 * @var AABB2Tree::freeList
 *   Head of the intrusive free list, or ::AABB_NULL when the pool is full.
 * @var AABB2Tree::margin
 *   Fattening margin in world units, applied to every inserted box. Larger
 *   values mean fewer reinsertions on movement but looser, slower queries.
 * @var AABB2Tree::items
 *   Flat slab of `capacity * itemsPerLeaf` pointers carved from the same
 *   Arena, indexed as `items[nodeIndex * itemsPerLeaf + slot]`. Not owned, so
 *   never free() it. NULL if that allocation failed.
 * @var AABB2Tree::itemsPerLeaf
 *   Item slots reserved for every node, and so the most items one leaf can
 *   hold. Fixed at aabb2_tree_create() time; 0 if the allocation failed.
 */
typedef struct AABB2Tree{
	AABB2Node* nodes;
	void** items;
	int count;
	int capacity;
	int itemsPerLeaf;
	int root;
	int freeList;
	float margin;
}AABB2Tree;

/**
 * @brief Empty the tree and rebuild the free list over the whole pool.
 *
 * @param t Tree to clear. Its @ref AABB2Tree::nodes, @ref AABB2Tree::items
 *          and @ref AABB2Tree::capacity are left alone; only the links,
 *          heights, item counts, count and root are reset. Every leaf's item
 *          list is emptied.
 *
 * @warning Every proxy id previously handed out is invalid afterwards.
 * @note Called by aabb2_tree_create(); call it directly to reuse a tree for a
 *       fresh set of proxies without touching the arena.
 */
static inline void aabb2_tree_reset_pool(AABB2Tree* t){
	t->count = 0;
	t->root = AABB_NULL;
	for (int i = 0; i < t->capacity; i++){
		t->nodes[i].next = i + 1;
		t->nodes[i].height = -1;
		t->nodes[i].itemCount = 0;
	}
	if (t->capacity > 0) t->nodes[t->capacity - 1].next = AABB_NULL;
	t->freeList = t->capacity > 0 ? 0 : AABB_NULL;
}

/**
 * @brief Create a tree whose node pool is carved out of an arena.
 *
 * @param arena    Arena to allocate the node pool from. The pool is never
 *                 freed individually; it lives until the arena is reset or
 *                 destroyed, so the arena must outlive the tree.
 * @param capacity     Number of *nodes* to reserve, not proxies. Size it as
 *                     2*maxProxies-1, since each insert also consumes an
 *                     internal node. Values below 1 are clamped to 1.
 * @param itemsPerLeaf Item slots reserved per node, and so the most items one
 *                     leaf can hold. Values below 1 are clamped to 1. Slots
 *                     are reserved for every node, internal ones included --
 *                     they never use theirs, so roughly half the slab is
 *                     headroom for nodes that later become leaves.
 * @param margin       Fat-AABB margin in world units applied to every inserted
 *                     box; see aabb2_fatten(). 0 disables fattening, which
 *                     makes every move a reinsertion.
 * @return A tree by value. If either arena allocation failed, `nodes` is NULL
 *         and `capacity` is 0, and every aabb2_tree_insert() then returns
 *         ::AABB_NULL -- check that rather than the return of this function.
 *
 * @note Takes two bites out of the arena: `capacity` nodes and
 *       `capacity * itemsPerLeaf` pointers.
 */
static inline AABB2Tree aabb2_tree_create(Arena* arena, int capacity, int itemsPerLeaf, float margin){
	AABB2Tree t;
	if (capacity < 1) capacity = 1;
	if (itemsPerLeaf < 1) itemsPerLeaf = 1;
	t.nodes = (AABB2Node*)arena_alloc(arena, sizeof(AABB2Node) * (size_t)capacity);
	t.items = (void**)arena_alloc(arena, sizeof(void*) * (size_t)capacity * (size_t)itemsPerLeaf);
	int ok = t.nodes != NULL && t.items != NULL;
	t.capacity = ok ? capacity : 0;
	t.itemsPerLeaf = ok ? itemsPerLeaf : 0;
	t.margin = margin;
	aabb2_tree_reset_pool(&t);
	return t;
}

/**
 * @brief Pop a slot off the free list and initialise it as a detached leaf.
 *
 * @param t Tree to allocate from.
 * @return Index of the claimed node with parent/left/right set to ::AABB_NULL,
 *         height 0 and an empty item list, or ::AABB_NULL if the pool is
 *         full.
 *         @ref AABB2Tree::box is left as-is and must be set by the caller.
 *
 * @note Internal. Use aabb2_tree_insert() to add a proxy.
 */
static inline int aabb2_tree_allocate_node(AABB2Tree* t){
	if (t->freeList == AABB_NULL) return AABB_NULL;
	int id = t->freeList;
	t->freeList = t->nodes[id].next;
	t->nodes[id].parent = AABB_NULL;
	t->nodes[id].left = AABB_NULL;
	t->nodes[id].right = AABB_NULL;
	t->nodes[id].height = 0;
	t->nodes[id].itemCount = 0;
	t->count++;
	return id;
}

/**
 * @brief Push a node back onto the free list.
 *
 * @param t  Tree owning the node.
 * @param id Index of the node to release. Must already be unlinked from the
 *           tree -- this does not touch its parent or children.
 *
 * @note Internal. Use aabb2_tree_remove() to drop a proxy.
 */
static inline void aabb2_tree_free_node(AABB2Tree* t, int id){
	t->nodes[id].next = t->freeList;
	t->nodes[id].height = -1;
	t->freeList = id;
	t->count--;
}

/**
 * @brief Rotate one subtree at most once to restore AVL balance.
 *
 * @param t  Tree to rotate within. @ref AABB2Tree::root is updated if @p iA was
 *           the root and the rotation replaced it.
 * @param iA Index of the subtree root to balance.
 * @return Index of the subtree root after rotation -- @p iA itself when the
 *         node has height < 2 or the children differ by at most one level,
 *         otherwise the child promoted into its place. Callers must use this
 *         return value, since @p iA may no longer be the subtree root.
 *
 * @note Boxes and heights of the two nodes involved are recomputed here;
 *       ancestors are not, which is what aabb2_tree_refit() is for.
 */
static inline int aabb2_tree_balance(AABB2Tree* t, int iA){
	if (t->nodes[iA].height < 2) return iA;

	int iB = t->nodes[iA].left;
	int iC = t->nodes[iA].right;
	int balance = t->nodes[iC].height - t->nodes[iB].height;

	if (balance > 1){
		int iF = t->nodes[iC].left;
		int iG = t->nodes[iC].right;
		t->nodes[iC].left = iA;
		t->nodes[iC].parent = t->nodes[iA].parent;
		t->nodes[iA].parent = iC;
		if (t->nodes[iC].parent != AABB_NULL){
			if (t->nodes[t->nodes[iC].parent].left == iA)
				t->nodes[t->nodes[iC].parent].left = iC;
			else
				t->nodes[t->nodes[iC].parent].right = iC;
		}else{
			t->root = iC;
		}
		if (t->nodes[iF].height > t->nodes[iG].height){
			t->nodes[iC].right = iF;
			t->nodes[iA].right = iG;
			t->nodes[iG].parent = iA;
			t->nodes[iA].box = aabb2_union(t->nodes[iB].box, t->nodes[iG].box);
			t->nodes[iC].box = aabb2_union(t->nodes[iA].box, t->nodes[iF].box);
			t->nodes[iA].height = 1 + aabb_max_int(t->nodes[iB].height, t->nodes[iG].height);
			t->nodes[iC].height = 1 + aabb_max_int(t->nodes[iA].height, t->nodes[iF].height);
		}else{
			t->nodes[iC].right = iG;
			t->nodes[iA].right = iF;
			t->nodes[iF].parent = iA;
			t->nodes[iA].box = aabb2_union(t->nodes[iB].box, t->nodes[iF].box);
			t->nodes[iC].box = aabb2_union(t->nodes[iA].box, t->nodes[iG].box);
			t->nodes[iA].height = 1 + aabb_max_int(t->nodes[iB].height, t->nodes[iF].height);
			t->nodes[iC].height = 1 + aabb_max_int(t->nodes[iA].height, t->nodes[iG].height);
		}
		return iC;
	}

	if (balance < -1){
		int iD = t->nodes[iB].left;
		int iE = t->nodes[iB].right;
		t->nodes[iB].left = iA;
		t->nodes[iB].parent = t->nodes[iA].parent;
		t->nodes[iA].parent = iB;
		if (t->nodes[iB].parent != AABB_NULL){
			if (t->nodes[t->nodes[iB].parent].left == iA)
				t->nodes[t->nodes[iB].parent].left = iB;
			else
				t->nodes[t->nodes[iB].parent].right = iB;
		}else{
			t->root = iB;
		}
		if (t->nodes[iD].height > t->nodes[iE].height){
			t->nodes[iB].right = iD;
			t->nodes[iA].left = iE;
			t->nodes[iE].parent = iA;
			t->nodes[iA].box = aabb2_union(t->nodes[iC].box, t->nodes[iE].box);
			t->nodes[iB].box = aabb2_union(t->nodes[iA].box, t->nodes[iD].box);
			t->nodes[iA].height = 1 + aabb_max_int(t->nodes[iC].height, t->nodes[iE].height);
			t->nodes[iB].height = 1 + aabb_max_int(t->nodes[iA].height, t->nodes[iD].height);
		}else{
			t->nodes[iB].right = iE;
			t->nodes[iA].left = iD;
			t->nodes[iD].parent = iA;
			t->nodes[iA].box = aabb2_union(t->nodes[iC].box, t->nodes[iD].box);
			t->nodes[iB].box = aabb2_union(t->nodes[iA].box, t->nodes[iE].box);
			t->nodes[iA].height = 1 + aabb_max_int(t->nodes[iC].height, t->nodes[iD].height);
			t->nodes[iB].height = 1 + aabb_max_int(t->nodes[iA].height, t->nodes[iE].height);
		}
		return iB;
	}

	return iA;
}

/**
 * @brief Walk from a node to the root, rebalancing and re-unioning as it goes.
 *
 * @param t     Tree to fix up.
 * @param index Node to start from -- normally the parent of whatever just
 *              changed. ::AABB_NULL is a no-op, so passing the parent of the
 *              root is safe.
 *
 * @note Every node on the path gets a fresh height and a box re-unioned from
 *       its children, so structural edits only need to call this once on the
 *       lowest node they touched.
 */
static inline void aabb2_tree_refit(AABB2Tree* t, int index){
	while (index != AABB_NULL){
		index = aabb2_tree_balance(t, index);
		int left = t->nodes[index].left;
		int right = t->nodes[index].right;
		t->nodes[index].height = 1 + aabb_max_int(t->nodes[left].height, t->nodes[right].height);
		t->nodes[index].box = aabb2_union(t->nodes[left].box, t->nodes[right].box);
		index = t->nodes[index].parent;
	}
}

/**
 * @brief Link an already-allocated leaf into the tree at the cheapest spot.
 *
 * @param t    Tree to insert into.
 * @param leaf Index of a leaf node whose box is already set. Its parent/child
 *             links are overwritten.
 * @return 1 on success; 0 if the pool had no slot left for the new internal
 *         parent node, in which case the tree is unchanged and @p leaf is
 *         still unlinked.
 *
 * @note Descends by the surface-area heuristic (perimeter, in 2D), then
 *       refits the path back to the root. Inserting into a non-empty tree
 *       costs a second node.
 */
static inline int aabb2_tree_insert_leaf(AABB2Tree* t, int leaf){
	if (t->root == AABB_NULL){
		t->root = leaf;
		t->nodes[leaf].parent = AABB_NULL;
		return 1;
	}

	AABB2 leafBox = t->nodes[leaf].box;
	int index = t->root;
	while (t->nodes[index].height != 0){
		int left = t->nodes[index].left;
		int right = t->nodes[index].right;

		float area = aabb2_cost(t->nodes[index].box);
		AABB2 combined = aabb2_union(t->nodes[index].box, leafBox);
		float combinedArea = aabb2_cost(combined);

		float cost = 2.0f * combinedArea;
		float inheritanceCost = 2.0f * (combinedArea - area);

		float cost1, cost2;
		AABB2 c1 = aabb2_union(leafBox, t->nodes[left].box);
		if (t->nodes[left].height == 0)
			cost1 = aabb2_cost(c1) + inheritanceCost;
		else
			cost1 = (aabb2_cost(c1) - aabb2_cost(t->nodes[left].box)) + inheritanceCost;

		AABB2 c2 = aabb2_union(leafBox, t->nodes[right].box);
		if (t->nodes[right].height == 0)
			cost2 = aabb2_cost(c2) + inheritanceCost;
		else
			cost2 = (aabb2_cost(c2) - aabb2_cost(t->nodes[right].box)) + inheritanceCost;

		if (cost < cost1 && cost < cost2) break;
		index = (cost1 < cost2) ? left : right;
	}

	int sibling = index;
	int newParent = aabb2_tree_allocate_node(t);
	if (newParent == AABB_NULL) return 0;

	int oldParent = t->nodes[sibling].parent;
	t->nodes[newParent].parent = oldParent;
	t->nodes[newParent].itemCount = 0;
	t->nodes[newParent].box = aabb2_union(leafBox, t->nodes[sibling].box);
	t->nodes[newParent].height = t->nodes[sibling].height + 1;
	t->nodes[newParent].left = sibling;
	t->nodes[newParent].right = leaf;
	t->nodes[sibling].parent = newParent;
	t->nodes[leaf].parent = newParent;

	if (oldParent != AABB_NULL){
		if (t->nodes[oldParent].left == sibling)
			t->nodes[oldParent].left = newParent;
		else
			t->nodes[oldParent].right = newParent;
	}else{
		t->root = newParent;
	}

	aabb2_tree_refit(t, t->nodes[leaf].parent);
	return 1;
}

/**
 * @brief Unlink a leaf, collapsing its parent into its sibling.
 *
 * @param t    Tree to remove from.
 * @param leaf Index of the leaf to unlink. It is *not* returned to the free
 *             list -- the caller decides whether to free or reuse it, which is
 *             what lets aabb2_tree_move() recycle the same node.
 *
 * @note The leaf's now-redundant parent *is* freed, and the path above is
 *       refit.
 */
static inline void aabb2_tree_remove_leaf(AABB2Tree* t, int leaf){
	if (leaf == t->root){
		t->root = AABB_NULL;
		return;
	}

	int parent = t->nodes[leaf].parent;
	int grandParent = t->nodes[parent].parent;
	int sibling = (t->nodes[parent].left == leaf) ? t->nodes[parent].right
	                                              : t->nodes[parent].left;

	if (grandParent != AABB_NULL){
		if (t->nodes[grandParent].left == parent)
			t->nodes[grandParent].left = sibling;
		else
			t->nodes[grandParent].right = sibling;
		t->nodes[sibling].parent = grandParent;
		aabb2_tree_free_node(t, parent);
		aabb2_tree_refit(t, grandParent);
	}else{
		t->root = sibling;
		t->nodes[sibling].parent = AABB_NULL;
		aabb2_tree_free_node(t, parent);
	}
}

/**
 * @brief Add a box to the tree.
 *
 * @param t        Tree to insert into.
 * @param box      Tight box for the object. It is fattened by
 *                 @ref AABB2Tree::margin before being stored, so the box you
 *                 read back from aabb2_tree_get_fat_aabb() is larger than this.
 * @return A stable proxy id to pass to move/remove/leaf_*, or ::AABB_NULL if
 *         the pool is full. Ids stay valid until the proxy is removed or the
 *         pool is reset; they are node indices, not insertion order.
 *
 * @note The new proxy starts with an *empty* item list -- follow up with
 *       aabb2_tree_leaf_add() for each item it should carry. A leaf with no
 *       items is legal and is still reported by queries, with a count of 0.
 * @note Needs two free nodes when the tree is non-empty (the leaf plus an
 *       internal parent), so a pool sized for exactly N leaves will fail early.
 */
static inline int aabb2_tree_insert(AABB2Tree* t, AABB2 box){
	int id = aabb2_tree_allocate_node(t);
	if (id == AABB_NULL) return AABB_NULL;
	t->nodes[id].box = aabb2_fatten(box, t->margin);
	t->nodes[id].height = 0;
	if (!aabb2_tree_insert_leaf(t, id)){
		aabb2_tree_free_node(t, id);
		return AABB_NULL;
	}
	return id;
}

/**
 * @brief Remove a proxy and return its node to the pool.
 *
 * @param t       Tree to remove from.
 * @param proxyId Id returned by aabb2_tree_insert(). Must be a live proxy --
 *                removing twice, or passing an internal node's index,
 *                corrupts the tree. Not validated.
 *
 * @warning @p proxyId is dangling afterwards and may be handed out again by a
 *          later insert.
 */
static inline void aabb2_tree_remove(AABB2Tree* t, int proxyId){
	aabb2_tree_remove_leaf(t, proxyId);
	aabb2_tree_free_node(t, proxyId);
}

/**
 * @brief Update a proxy's box, reinserting only if it escaped its fat box.
 *
 * @param t       Tree holding the proxy.
 * @param proxyId Id returned by aabb2_tree_insert(). Stays valid either way --
 *                a reinsertion reuses the same node.
 * @param box     New tight box for the object. Refattened by
 *                @ref AABB2Tree::margin if a reinsertion happens.
 * @return 0 if @p box still fits inside the stored fat box, so nothing was
 *         done; 1 if the proxy was removed and reinserted.
 *
 * @note This is the per-frame update call for moving objects; the return value
 *       is useful as a cheap "this one actually moved" signal.
 */
static inline int aabb2_tree_move(AABB2Tree* t, int proxyId, AABB2 box){
	if (aabb2_contains(t->nodes[proxyId].box, box)) return 0;
	aabb2_tree_remove_leaf(t, proxyId);
	t->nodes[proxyId].box = aabb2_fatten(box, t->margin);
	t->nodes[proxyId].height = 0;
	t->nodes[proxyId].left = AABB_NULL;
	t->nodes[proxyId].right = AABB_NULL;
	aabb2_tree_insert_leaf(t, proxyId);
	return 1;
}

/**
 * @brief Read back a proxy's stored box.
 *
 * @param t       Tree holding the proxy.
 * @param proxyId Id returned by aabb2_tree_insert().
 * @return The *fattened* box, i.e. the tight box you inserted grown by
 *         @ref AABB2Tree::margin -- not the box you passed in.
 */
static inline AABB2 aabb2_tree_get_fat_aabb(const AABB2Tree* t, int proxyId){ return t->nodes[proxyId].box; }
/**
 * @brief Base of a proxy's item slots.
 *
 * @param t       Tree holding the proxy.
 * @param proxyId Id returned by aabb2_tree_insert().
 * @return Pointer to the leaf's first item slot, into the tree's shared slab.
 *         Only the first aabb2_tree_leaf_count() entries hold live items; the
 *         rest is uninitialised. Stays valid until the pool is reset, and is
 *         writable through the returned pointer.
 *
 * @note Not validated: asking an internal node or a freed slot returns a live
 *       pointer into the slab holding meaningless data.
 */
static inline void** aabb2_tree_leaf_items(const AABB2Tree* t, int proxyId){
	return t->items + (size_t)proxyId * (size_t)t->itemsPerLeaf;
}

/**
 * @brief How many items a proxy currently holds.
 *
 * @param t       Tree holding the proxy.
 * @param proxyId Id returned by aabb2_tree_insert().
 * @return Live item count, 0 to @ref AABB2Tree::itemsPerLeaf. Not validated.
 */
static inline int aabb2_tree_leaf_count(const AABB2Tree* t, int proxyId){ return t->nodes[proxyId].itemCount; }

/**
 * @brief Append one item to a proxy's list.
 *
 * @param t       Tree holding the proxy.
 * @param proxyId Id returned by aabb2_tree_insert().
 * @param item    Opaque pointer handed back by queries and ray casts. Never
 *                dereferenced or freed by the tree, so its lifetime is yours
 *                to manage. May be NULL, and may be another AABB tree.
 * @return 1 on success, 0 if the leaf already holds
 *         @ref AABB2Tree::itemsPerLeaf items. Duplicates are not detected.
 *
 * @note Does not touch the leaf's box. If the item widens what the proxy
 *       covers, follow with aabb2_tree_move().
 */
static inline int aabb2_tree_leaf_add(AABB2Tree* t, int proxyId, void* item){
	if (t->nodes[proxyId].itemCount >= t->itemsPerLeaf) return 0;
	aabb2_tree_leaf_items(t, proxyId)[t->nodes[proxyId].itemCount++] = item;
	return 1;
}

/**
 * @brief Drop the item at one slot, filling the hole with the last item.
 *
 * @param t       Tree holding the proxy.
 * @param proxyId Id returned by aabb2_tree_insert().
 * @param index   Slot to drop, 0 to aabb2_tree_leaf_count()-1. Out-of-range
 *                values are ignored.
 *
 * @warning Order is not preserved -- the last item is swapped into @p index.
 *          When erasing while iterating, do not advance the loop counter after
 *          a removal, or walk the list backwards.
 */
static inline void aabb2_tree_leaf_remove_at(AABB2Tree* t, int proxyId, int index){
	int n = t->nodes[proxyId].itemCount;
	if (index < 0 || index >= n) return;
	void** slots = aabb2_tree_leaf_items(t, proxyId);
	slots[index] = slots[n - 1];
	t->nodes[proxyId].itemCount = n - 1;
}

/**
 * @brief Drop the first slot holding a given pointer.
 *
 * @param t       Tree holding the proxy.
 * @param proxyId Id returned by aabb2_tree_insert().
 * @param item    Pointer to look for, compared by address, not by value.
 * @return 1 if an item was removed, 0 if @p item was not in the list.
 *
 * @warning Order is not preserved; see aabb2_tree_leaf_remove_at(). Only the
 *          first match goes, so a pointer added twice needs removing twice.
 */
static inline int aabb2_tree_leaf_remove(AABB2Tree* t, int proxyId, void* item){
	void** slots = aabb2_tree_leaf_items(t, proxyId);
	for (int i = 0; i < t->nodes[proxyId].itemCount; i++){
		if (slots[i] == item){ aabb2_tree_leaf_remove_at(t, proxyId, i); return 1; }
	}
	return 0;
}

/**
 * @brief Drop every item from a proxy, keeping the proxy itself.
 *
 * @param t       Tree holding the proxy.
 * @param proxyId Id returned by aabb2_tree_insert().
 *
 * @note The proxy stays in the tree and keeps its box; queries keep reporting
 *       it with a count of 0. Use aabb2_tree_remove() to drop the proxy too.
 */
static inline void aabb2_tree_leaf_clear(AABB2Tree* t, int proxyId){ t->nodes[proxyId].itemCount = 0; }
/**
 * @brief Height of the tree, as a rough quality metric.
 *
 * @param t Tree to measure.
 * @return 0 for an empty tree, otherwise the root's height (a single-leaf tree
 *         is also 0). Balancing keeps this near log2(proxies); a much larger
 *         value points at a degenerate distribution or too small a margin.
 */
static inline int aabb2_tree_height(const AABB2Tree* t){ return t->root == AABB_NULL ? 0 : t->nodes[t->root].height; }

/**
 * @brief Callback invoked once per leaf whose box overlaps the query box.
 *
 * @param items The matching proxy's item slots, as added with
 *              aabb2_tree_leaf_add(). Valid for @p count entries; do not
 *              hold on to it past the callback.
 * @param count How many items @p items holds. May be 0 for a proxy whose list
 *              is empty -- the leaf is still reported.
 * @param ctx   The opaque pointer passed to aabb2_tree_query(), untouched.
 * @return Non-zero to keep searching, 0 to stop the traversal early. Returning
 *         0 by accident (a plain `return;`-style 0) silently truncates results.
 *
 * @note Hits are reported against *fattened* boxes, so this over-reports:
 *       re-test with the tight box in the callback if you need exactness.
 * @note An item that is itself an AABB tree is not descended into: recognise
 *       it here and call aabb2_tree_query() on it yourself, forwarding the
 *       query box through @p ctx.
 */
typedef int (*AABB2QueryCallback)(void** items, int count, void* ctx);

/**
 * @brief Recursive half of the overlap query.
 *
 * @param t     Tree to search.
 * @param index Subtree root to descend from; ::AABB_NULL terminates.
 * @param box   Query box, tested against each node's stored box.
 * @param cb    Callback for matching leaves.
 * @param ctx   Opaque pointer forwarded to @p cb.
 * @return Non-zero if the traversal ran to completion, 0 if @p cb asked to
 *         stop. Propagated up so an early exit unwinds the whole recursion.
 *
 * @note Internal. Call aabb2_tree_query() instead.
 */
static inline int aabb2_tree_query_node(const AABB2Tree* t, int index, AABB2 box,
                                        AABB2QueryCallback cb, void* ctx){
	if (index == AABB_NULL) return 1;
	if (!aabb2_overlaps(t->nodes[index].box, box)) return 1;
	if (t->nodes[index].height == 0)
		return cb(aabb2_tree_leaf_items(t, index), t->nodes[index].itemCount, ctx);
	if (!aabb2_tree_query_node(t, t->nodes[index].left, box, cb, ctx)) return 0;
	return aabb2_tree_query_node(t, t->nodes[index].right, box, cb, ctx);
}

/**
 * @brief Report every proxy whose fat box overlaps a box.
 *
 * @param t   Tree to search.
 * @param box Query box in world space.
 * @param cb  Called once per overlapping proxy; return 0 from it to stop early.
 * @param ctx Opaque pointer forwarded to @p cb -- use it to reach your own
 *            state, since the callback gets nothing else.
 *
 * @warning Do not insert into or remove from @p t inside @p cb; the traversal
 *          holds node indices across the call.
 */
static inline void aabb2_tree_query(const AABB2Tree* t, AABB2 box,
                                    AABB2QueryCallback cb, void* ctx){
	aabb2_tree_query_node(t, t->root, box, cb, ctx);
}

/**
 * @brief Slab test of the segment origin + t*dir, t in [0, tmax], vs a box.
 *
 * @param b      Box to test.
 * @param origin Start point of the segment.
 * @param dir    Direction vector. Not required to be normalised -- t is
 *               measured in multiples of this vector, so passing the full
 *               displacement makes t a 0..1 fraction.
 * @param tmax   Upper bound on t, i.e. where the segment ends. Passed by value
 *               and only narrowed internally; the caller's value is unchanged.
 * @param tHit   Optional out-parameter for the entry t. May be NULL. Set only
 *               when the function returns non-zero, and set to 0 when @p origin
 *               is already inside @p b.
 * @return Non-zero on hit, 0 on miss.
 *
 * @note An axis whose direction component is under 1e-8 is treated as parallel:
 *       it contributes a containment test instead of a slab clip.
 */
static inline int aabb2_ray_hit(AABB2 b, Vec2f origin, Vec2f dir, float tmax, float* tHit){
	float tmin = 0.0f;
	for (int axis = 0; axis < 2; axis++){
		float o = axis == 0 ? origin.x : origin.y;
		float d = axis == 0 ? dir.x : dir.y;
		float lo = axis == 0 ? b.topLeft.x : b.topLeft.y;
		float hi = axis == 0 ? b.bottomRight.x : b.bottomRight.y;
		if (fabsf(d) < 1e-8f){
			if (o < lo || o > hi) return 0;
		}else{
			float inv = 1.0f / d;
			float t1 = (lo - o) * inv;
			float t2 = (hi - o) * inv;
			if (t1 > t2){ float tmp = t1; t1 = t2; t2 = tmp; }
			if (t1 > tmin) tmin = t1;
			if (t2 < tmax) tmax = t2;
			if (tmin > tmax) return 0;
		}
	}
	if (tHit) *tHit = tmin;
	return 1;
}

/**
 * @brief Callback invoked for each leaf the ray reaches, newest clip first.
 *
 * @param items       The hit proxy's item slots, as added with
 *                    aabb2_tree_leaf_add(). Valid for @p count entries; do
 *                    not hold on to it past the callback.
 * @param count       How many items @p items holds. May be 0.
 * @param origin      Ray origin, as passed to aabb2_tree_ray_cast().
 * @param dir         Ray direction, as passed to aabb2_tree_ray_cast().
 * @param maxFraction The current upper bound on t when this leaf was reached.
 * @param ctx         Opaque pointer passed to aabb2_tree_ray_cast(), untouched.
 * @return The new maximum fraction. A positive value clips the ray to that t
 *         (return your exact hit distance to find the closest hit); exactly 0
 *         terminates the cast; a negative value leaves the current bound
 *         unchanged, which ignores this hit and keeps going.
 */
typedef float (*AABB2RayCallback)(void** items, int count, Vec2f origin, Vec2f dir,
                                  float maxFraction, void* ctx);

/**
 * @brief Recursive half of the ray cast.
 *
 * @param t           Tree to traverse.
 * @param index       Subtree root to descend from; ::AABB_NULL terminates.
 * @param origin      Ray origin.
 * @param dir         Ray direction.
 * @param maxFraction In/out: the shared clip distance, narrowed as leaves
 *                    report hits. Traversal stops once it reaches 0. Pointer,
 *                    not a value, so sibling subtrees see earlier clips.
 * @param cb          Callback for each leaf reached.
 * @param ctx         Opaque pointer forwarded to @p cb.
 *
 * @note Internal. Call aabb2_tree_ray_cast() instead. Children are visited in
 *       fixed left-then-right order, not nearest-first, so the callback must
 *       do its own closest-hit bookkeeping.
 */
static inline void aabb2_tree_ray_node(const AABB2Tree* t, int index, Vec2f origin,
                                       Vec2f dir, float* maxFraction,
                                       AABB2RayCallback cb, void* ctx){
	if (index == AABB_NULL || *maxFraction <= 0.0f) return;
	float th;
	if (!aabb2_ray_hit(t->nodes[index].box, origin, dir, *maxFraction, &th)) return;
	if (t->nodes[index].height == 0){
		float r = cb(aabb2_tree_leaf_items(t, index), t->nodes[index].itemCount,
		             origin, dir, *maxFraction, ctx);
		if (r == 0.0f) *maxFraction = 0.0f;
		else if (r > 0.0f) *maxFraction = r;
		return;
	}
	aabb2_tree_ray_node(t, t->nodes[index].left, origin, dir, maxFraction, cb, ctx);
	aabb2_tree_ray_node(t, t->nodes[index].right, origin, dir, maxFraction, cb, ctx);
}

/**
 * @brief Cast a ray through the tree, reporting each proxy it reaches.
 *
 * @param t           Tree to cast against.
 * @param origin      Ray origin in world space.
 * @param dir         Ray direction. Not required to be normalised; fractions
 *                    below are measured in multiples of this vector.
 * @param maxFraction Initial upper bound on t. Pass 1.0f with @p dir set to the
 *                    full displacement to cast exactly that segment. Taken by
 *                    value -- the caller's variable is not modified.
 * @param cb          Called per hit leaf; its return value clips or ends the
 *                    cast (see ::AABB2RayCallback).
 * @param ctx         Opaque pointer forwarded to @p cb.
 *
 * @warning Do not modify @p t inside @p cb.
 */
static inline void aabb2_tree_ray_cast(const AABB2Tree* t, Vec2f origin, Vec2f dir,
                                       float maxFraction, AABB2RayCallback cb, void* ctx){
	float mf = maxFraction;
	aabb2_tree_ray_node(t, t->root, origin, dir, &mf, cb, ctx);
}

// ============================================================================
// 3D
// ============================================================================

/**
 * @brief 3D axis-aligned bounding box, stored as two opposite corners.
 *
 * @var AABB3::min
 *   Low corner: the componentwise minimum over x, y and z. Callers building
 *   a box by hand must keep this <= @ref AABB3::max on every axis, or use
 *   aabb3_make(), which sorts the corners for you.
 * @var AABB3::max
 *   High corner: the componentwise maximum over x, y and z.
 */
typedef struct AABB3{
	Vec3f min;
	Vec3f max;
}AABB3;

/**
 * @brief Build a box from two arbitrary corners.
 *
 * @param min One corner. Need not actually be the smaller one.
 * @param max The opposite corner. Need not actually be the larger one.
 * @return A box whose min/max are the componentwise min/max of the two
 *         inputs, so the result is well formed on every axis.
 *
 * @note Use this rather than brace-initialising AABB3 when corner order is not
 *       already guaranteed -- an inverted box silently breaks every overlap
 *       and cost computation below.
 */
static inline AABB3 aabb3_make(Vec3f min, Vec3f max){
	AABB3 b;
	b.min.x = fminf(min.x, max.x);
	b.min.y = fminf(min.y, max.y);
	b.min.z = fminf(min.z, max.z);
	b.max.x = fmaxf(min.x, max.x);
	b.max.y = fmaxf(min.y, max.y);
	b.max.z = fmaxf(min.z, max.z);
	return b;
}

/**
 * @brief Smallest box enclosing both inputs.
 *
 * @param a First box. Must be well formed (min <= max on every axis).
 * @param b Second box, same requirement.
 * @return The componentwise min of the low corners and max of the high
 *         corners. This is a bounding union, not a set union: it also covers
 *         the empty space between two disjoint boxes.
 */
static inline AABB3 aabb3_union(AABB3 a, AABB3 b){
	AABB3 r;
	r.min.x = fminf(a.min.x, b.min.x);
	r.min.y = fminf(a.min.y, b.min.y);
	r.min.z = fminf(a.min.z, b.min.z);
	r.max.x = fmaxf(a.max.x, b.max.x);
	r.max.y = fmaxf(a.max.y, b.max.y);
	r.max.z = fmaxf(a.max.z, b.max.z);
	return r;
}

/**
 * @brief Node cost used to steer tree insertion.
 *
 * @param b Box to measure. A well-formed box gives a non-negative result;
 *          an inverted one gives a meaningless (possibly negative) cost.
 * @return Surface area of @p b, i.e. 2 * (wx*wy + wy*wz + wz*wx).
 *
 * @note This is the surface-area heuristic: insertion picks the descent that
 *       grows this quantity least, which keeps sibling boxes tight and
 *       queries cheap.
 */
static inline float aabb3_cost(AABB3 b){
	float wx = b.max.x - b.min.x;
	float wy = b.max.y - b.min.y;
	float wz = b.max.z - b.min.z;
	return 2.0f * (wx * wy + wy * wz + wz * wx);
}

/**
 * @brief Test whether two boxes intersect.
 *
 * @param a First box.
 * @param b Second box.
 * @return Non-zero if the boxes share any point, 0 if they are disjoint.
 *
 * @note The test is inclusive: boxes that merely touch on a face or edge
 *       count as overlapping.
 */
static inline int aabb3_overlaps(AABB3 a, AABB3 b){
	return !(a.max.x < b.min.x || a.min.x > b.max.x ||
	         a.max.y < b.min.y || a.min.y > b.max.y ||
	         a.max.z < b.min.z || a.min.z > b.max.z);
}

/**
 * @brief Test whether one box fully encloses another.
 *
 * @param a The outer box, the one that must do the containing.
 * @param b The inner box, tested for being inside @p a.
 * @return Non-zero if every point of @p b lies in @p a, 0 otherwise.
 *
 * @note Argument order matters and is easy to get backwards: this asks
 *       "does a contain b", not the reverse. Containment is inclusive, so a
 *       box contains itself.
 */
static inline int aabb3_contains(AABB3 a, AABB3 b){
	return a.min.x <= b.min.x && a.min.y <= b.min.y && a.min.z <= b.min.z &&
	       b.max.x <= a.max.x && b.max.y <= a.max.y && b.max.z <= a.max.z;
}

/**
 * @brief Expand a box outward on all sides.
 *
 * @param b      Box to grow.
 * @param margin Distance to push each face outward, in world units. Applied to
 *               all 3 axes. A negative margin shrinks the box instead, and
 *               can invert it if it exceeds half the box extent.
 * @return @p b with its low corner moved down by @p margin and its high corner
 *         up by @p margin on every axis.
 *
 * @note This is the "fat AABB" trick: leaves are stored slightly oversized so
 *       small movements stay inside their stored box and skip reinsertion.
 */
static inline AABB3 aabb3_fatten(AABB3 b, float margin){
	AABB3 r;
	r.min.x = b.min.x - margin;
	r.min.y = b.min.y - margin;
	r.min.z = b.min.z - margin;
	r.max.x = b.max.x + margin;
	r.max.y = b.max.y + margin;
	r.max.z = b.max.z + margin;
	return r;
}

/**
 * @brief One node of the tree pool -- either a leaf, an internal node, or free.
 *
 * @var AABB3Node::box
 *   For a leaf, the fattened box of the proxy. For an internal node, the union
 *   of both children's boxes. Undefined while the node is free.
 * @var AABB3Node::itemCount
 *   How many of this leaf's item slots are in use, from 0 to
 *   @ref AABB3Tree::itemsPerLeaf. The items themselves live in the tree's
 *   shared slab, reachable with aabb3_tree_leaf_items(). Meaningful on leaves
 *   only -- internal nodes hold 0.
 * @var AABB3Node::parent
 *   Index of the parent node, or ::AABB_NULL at the root.
 * @var AABB3Node::left
 *   Index of the left child, or ::AABB_NULL on a leaf.
 * @var AABB3Node::right
 *   Index of the right child, or ::AABB_NULL on a leaf.
 * @var AABB3Node::height
 *   0 for a leaf, -1 while the node is on the free list, otherwise
 *   1 + max(child heights). Doubles as the leaf/internal/free tag.
 * @var AABB3Node::next
 *   Free-list link to the next free slot while this node is free; unused, and
 *   not maintained, while the node is live.
 */
typedef struct AABB3Node{
	AABB3 box;
	int itemCount;
	int parent;
	int left;
	int right;
	int height;
	int next;
}AABB3Node;

/**
 * @brief Dynamic bounding volume hierarchy over a fixed, arena-backed pool.
 *
 * @var AABB3Tree::nodes
 *   Node pool carved from an Arena by aabb3_tree_create(). Not owned: it dies
 *   with the arena, so never free() it. NULL if that allocation failed.
 * @var AABB3Tree::count
 *   Number of nodes currently in use, leaves and internal nodes together --
 *   not the number of proxies. A tree of N proxies uses 2*N-1 nodes.
 * @var AABB3Tree::capacity
 *   Total slots in @ref AABB3Tree::nodes, or 0 if the arena allocation failed.
 *   The hard ceiling on count.
 * @var AABB3Tree::root
 *   Index of the root node, or ::AABB_NULL while the tree is empty.
 * @var AABB3Tree::freeList
 *   Head of the intrusive free list, or ::AABB_NULL when the pool is full.
 * @var AABB3Tree::margin
 *   Fattening margin in world units, applied to every inserted box. Larger
 *   values mean fewer reinsertions on movement but looser, slower queries.
 * @var AABB3Tree::items
 *   Flat slab of `capacity * itemsPerLeaf` pointers carved from the same
 *   Arena, indexed as `items[nodeIndex * itemsPerLeaf + slot]`. Not owned, so
 *   never free() it. NULL if that allocation failed.
 * @var AABB3Tree::itemsPerLeaf
 *   Item slots reserved for every node, and so the most items one leaf can
 *   hold. Fixed at aabb3_tree_create() time; 0 if the allocation failed.
 */
typedef struct AABB3Tree{
	AABB3Node* nodes;
	void** items;
	int count;
	int capacity;
	int itemsPerLeaf;
	int root;
	int freeList;
	float margin;
}AABB3Tree;

/**
 * @brief Empty the tree and rebuild the free list over the whole pool.
 *
 * @param t Tree to clear. Its @ref AABB3Tree::nodes, @ref AABB3Tree::items
 *          and @ref AABB3Tree::capacity are left alone; only the links,
 *          heights, item counts, count and root are reset. Every leaf's item
 *          list is emptied.
 *
 * @warning Every proxy id previously handed out is invalid afterwards.
 * @note Called by aabb3_tree_create(); call it directly to reuse a tree for a
 *       fresh set of proxies without touching the arena.
 */
static inline void aabb3_tree_reset_pool(AABB3Tree* t){
	t->count = 0;
	t->root = AABB_NULL;
	for (int i = 0; i < t->capacity; i++){
		t->nodes[i].next = i + 1;
		t->nodes[i].height = -1;
		t->nodes[i].itemCount = 0;
	}
	if (t->capacity > 0) t->nodes[t->capacity - 1].next = AABB_NULL;
	t->freeList = t->capacity > 0 ? 0 : AABB_NULL;
}

/**
 * @brief Create a tree whose node pool is carved out of an arena.
 *
 * @param arena    Arena to allocate the node pool from. The pool is never
 *                 freed individually; it lives until the arena is reset or
 *                 destroyed, so the arena must outlive the tree.
 * @param capacity     Number of *nodes* to reserve, not proxies. Size it as
 *                     2*maxProxies-1, since each insert also consumes an
 *                     internal node. Values below 1 are clamped to 1.
 * @param itemsPerLeaf Item slots reserved per node, and so the most items one
 *                     leaf can hold. Values below 1 are clamped to 1. Slots
 *                     are reserved for every node, internal ones included --
 *                     they never use theirs, so roughly half the slab is
 *                     headroom for nodes that later become leaves.
 * @param margin       Fat-AABB margin in world units applied to every inserted
 *                     box; see aabb3_fatten(). 0 disables fattening, which
 *                     makes every move a reinsertion.
 * @return A tree by value. If either arena allocation failed, `nodes` is NULL
 *         and `capacity` is 0, and every aabb3_tree_insert() then returns
 *         ::AABB_NULL -- check that rather than the return of this function.
 *
 * @note Takes two bites out of the arena: `capacity` nodes and
 *       `capacity * itemsPerLeaf` pointers.
 */
static inline AABB3Tree aabb3_tree_create(Arena* arena, int capacity, int itemsPerLeaf, float margin){
	AABB3Tree t;
	if (capacity < 1) capacity = 1;
	if (itemsPerLeaf < 1) itemsPerLeaf = 1;
	t.nodes = (AABB3Node*)arena_alloc(arena, sizeof(AABB3Node) * (size_t)capacity);
	t.items = (void**)arena_alloc(arena, sizeof(void*) * (size_t)capacity * (size_t)itemsPerLeaf);
	int ok = t.nodes != NULL && t.items != NULL;
	t.capacity = ok ? capacity : 0;
	t.itemsPerLeaf = ok ? itemsPerLeaf : 0;
	t.margin = margin;
	aabb3_tree_reset_pool(&t);
	return t;
}

/**
 * @brief Pop a slot off the free list and initialise it as a detached leaf.
 *
 * @param t Tree to allocate from.
 * @return Index of the claimed node with parent/left/right set to ::AABB_NULL,
 *         height 0 and an empty item list, or ::AABB_NULL if the pool is
 *         full.
 *         @ref AABB3Tree::box is left as-is and must be set by the caller.
 *
 * @note Internal. Use aabb3_tree_insert() to add a proxy.
 */
static inline int aabb3_tree_allocate_node(AABB3Tree* t){
	if (t->freeList == AABB_NULL) return AABB_NULL;
	int id = t->freeList;
	t->freeList = t->nodes[id].next;
	t->nodes[id].parent = AABB_NULL;
	t->nodes[id].left = AABB_NULL;
	t->nodes[id].right = AABB_NULL;
	t->nodes[id].height = 0;
	t->nodes[id].itemCount = 0;
	t->count++;
	return id;
}

/**
 * @brief Push a node back onto the free list.
 *
 * @param t  Tree owning the node.
 * @param id Index of the node to release. Must already be unlinked from the
 *           tree -- this does not touch its parent or children.
 *
 * @note Internal. Use aabb3_tree_remove() to drop a proxy.
 */
static inline void aabb3_tree_free_node(AABB3Tree* t, int id){
	t->nodes[id].next = t->freeList;
	t->nodes[id].height = -1;
	t->freeList = id;
	t->count--;
}

/**
 * @brief Rotate one subtree at most once to restore AVL balance.
 *
 * @param t  Tree to rotate within. @ref AABB3Tree::root is updated if @p iA was
 *           the root and the rotation replaced it.
 * @param iA Index of the subtree root to balance.
 * @return Index of the subtree root after rotation -- @p iA itself when the
 *         node has height < 2 or the children differ by at most one level,
 *         otherwise the child promoted into its place. Callers must use this
 *         return value, since @p iA may no longer be the subtree root.
 *
 * @note Boxes and heights of the two nodes involved are recomputed here;
 *       ancestors are not, which is what aabb3_tree_refit() is for.
 */
static inline int aabb3_tree_balance(AABB3Tree* t, int iA){
	if (t->nodes[iA].height < 2) return iA;

	int iB = t->nodes[iA].left;
	int iC = t->nodes[iA].right;
	int balance = t->nodes[iC].height - t->nodes[iB].height;

	if (balance > 1){
		int iF = t->nodes[iC].left;
		int iG = t->nodes[iC].right;
		t->nodes[iC].left = iA;
		t->nodes[iC].parent = t->nodes[iA].parent;
		t->nodes[iA].parent = iC;
		if (t->nodes[iC].parent != AABB_NULL){
			if (t->nodes[t->nodes[iC].parent].left == iA)
				t->nodes[t->nodes[iC].parent].left = iC;
			else
				t->nodes[t->nodes[iC].parent].right = iC;
		}else{
			t->root = iC;
		}
		if (t->nodes[iF].height > t->nodes[iG].height){
			t->nodes[iC].right = iF;
			t->nodes[iA].right = iG;
			t->nodes[iG].parent = iA;
			t->nodes[iA].box = aabb3_union(t->nodes[iB].box, t->nodes[iG].box);
			t->nodes[iC].box = aabb3_union(t->nodes[iA].box, t->nodes[iF].box);
			t->nodes[iA].height = 1 + aabb_max_int(t->nodes[iB].height, t->nodes[iG].height);
			t->nodes[iC].height = 1 + aabb_max_int(t->nodes[iA].height, t->nodes[iF].height);
		}else{
			t->nodes[iC].right = iG;
			t->nodes[iA].right = iF;
			t->nodes[iF].parent = iA;
			t->nodes[iA].box = aabb3_union(t->nodes[iB].box, t->nodes[iF].box);
			t->nodes[iC].box = aabb3_union(t->nodes[iA].box, t->nodes[iG].box);
			t->nodes[iA].height = 1 + aabb_max_int(t->nodes[iB].height, t->nodes[iF].height);
			t->nodes[iC].height = 1 + aabb_max_int(t->nodes[iA].height, t->nodes[iG].height);
		}
		return iC;
	}

	if (balance < -1){
		int iD = t->nodes[iB].left;
		int iE = t->nodes[iB].right;
		t->nodes[iB].left = iA;
		t->nodes[iB].parent = t->nodes[iA].parent;
		t->nodes[iA].parent = iB;
		if (t->nodes[iB].parent != AABB_NULL){
			if (t->nodes[t->nodes[iB].parent].left == iA)
				t->nodes[t->nodes[iB].parent].left = iB;
			else
				t->nodes[t->nodes[iB].parent].right = iB;
		}else{
			t->root = iB;
		}
		if (t->nodes[iD].height > t->nodes[iE].height){
			t->nodes[iB].right = iD;
			t->nodes[iA].left = iE;
			t->nodes[iE].parent = iA;
			t->nodes[iA].box = aabb3_union(t->nodes[iC].box, t->nodes[iE].box);
			t->nodes[iB].box = aabb3_union(t->nodes[iA].box, t->nodes[iD].box);
			t->nodes[iA].height = 1 + aabb_max_int(t->nodes[iC].height, t->nodes[iE].height);
			t->nodes[iB].height = 1 + aabb_max_int(t->nodes[iA].height, t->nodes[iD].height);
		}else{
			t->nodes[iB].right = iE;
			t->nodes[iA].left = iD;
			t->nodes[iD].parent = iA;
			t->nodes[iA].box = aabb3_union(t->nodes[iC].box, t->nodes[iD].box);
			t->nodes[iB].box = aabb3_union(t->nodes[iA].box, t->nodes[iE].box);
			t->nodes[iA].height = 1 + aabb_max_int(t->nodes[iC].height, t->nodes[iD].height);
			t->nodes[iB].height = 1 + aabb_max_int(t->nodes[iA].height, t->nodes[iE].height);
		}
		return iB;
	}

	return iA;
}

/**
 * @brief Walk from a node to the root, rebalancing and re-unioning as it goes.
 *
 * @param t     Tree to fix up.
 * @param index Node to start from -- normally the parent of whatever just
 *              changed. ::AABB_NULL is a no-op, so passing the parent of the
 *              root is safe.
 *
 * @note Every node on the path gets a fresh height and a box re-unioned from
 *       its children, so structural edits only need to call this once on the
 *       lowest node they touched.
 */
static inline void aabb3_tree_refit(AABB3Tree* t, int index){
	while (index != AABB_NULL){
		index = aabb3_tree_balance(t, index);
		int left = t->nodes[index].left;
		int right = t->nodes[index].right;
		t->nodes[index].height = 1 + aabb_max_int(t->nodes[left].height, t->nodes[right].height);
		t->nodes[index].box = aabb3_union(t->nodes[left].box, t->nodes[right].box);
		index = t->nodes[index].parent;
	}
}

/**
 * @brief Link an already-allocated leaf into the tree at the cheapest spot.
 *
 * @param t    Tree to insert into.
 * @param leaf Index of a leaf node whose box is already set. Its parent/child
 *             links are overwritten.
 * @return 1 on success; 0 if the pool had no slot left for the new internal
 *         parent node, in which case the tree is unchanged and @p leaf is
 *         still unlinked.
 *
 * @note Descends by the surface-area heuristic, then refits the path back to
 *       the root. Inserting into a non-empty tree costs a second node.
 */
static inline int aabb3_tree_insert_leaf(AABB3Tree* t, int leaf){
	if (t->root == AABB_NULL){
		t->root = leaf;
		t->nodes[leaf].parent = AABB_NULL;
		return 1;
	}

	AABB3 leafBox = t->nodes[leaf].box;
	int index = t->root;
	while (t->nodes[index].height != 0){
		int left = t->nodes[index].left;
		int right = t->nodes[index].right;

		float area = aabb3_cost(t->nodes[index].box);
		AABB3 combined = aabb3_union(t->nodes[index].box, leafBox);
		float combinedArea = aabb3_cost(combined);

		float cost = 2.0f * combinedArea;
		float inheritanceCost = 2.0f * (combinedArea - area);

		float cost1, cost2;
		AABB3 c1 = aabb3_union(leafBox, t->nodes[left].box);
		if (t->nodes[left].height == 0)
			cost1 = aabb3_cost(c1) + inheritanceCost;
		else
			cost1 = (aabb3_cost(c1) - aabb3_cost(t->nodes[left].box)) + inheritanceCost;

		AABB3 c2 = aabb3_union(leafBox, t->nodes[right].box);
		if (t->nodes[right].height == 0)
			cost2 = aabb3_cost(c2) + inheritanceCost;
		else
			cost2 = (aabb3_cost(c2) - aabb3_cost(t->nodes[right].box)) + inheritanceCost;

		if (cost < cost1 && cost < cost2) break;
		index = (cost1 < cost2) ? left : right;
	}

	int sibling = index;
	int newParent = aabb3_tree_allocate_node(t);
	if (newParent == AABB_NULL) return 0;

	int oldParent = t->nodes[sibling].parent;
	t->nodes[newParent].parent = oldParent;
	t->nodes[newParent].itemCount = 0;
	t->nodes[newParent].box = aabb3_union(leafBox, t->nodes[sibling].box);
	t->nodes[newParent].height = t->nodes[sibling].height + 1;
	t->nodes[newParent].left = sibling;
	t->nodes[newParent].right = leaf;
	t->nodes[sibling].parent = newParent;
	t->nodes[leaf].parent = newParent;

	if (oldParent != AABB_NULL){
		if (t->nodes[oldParent].left == sibling)
			t->nodes[oldParent].left = newParent;
		else
			t->nodes[oldParent].right = newParent;
	}else{
		t->root = newParent;
	}

	aabb3_tree_refit(t, t->nodes[leaf].parent);
	return 1;
}

/**
 * @brief Unlink a leaf, collapsing its parent into its sibling.
 *
 * @param t    Tree to remove from.
 * @param leaf Index of the leaf to unlink. It is *not* returned to the free
 *             list -- the caller decides whether to free or reuse it, which is
 *             what lets aabb3_tree_move() recycle the same node.
 *
 * @note The leaf's now-redundant parent *is* freed, and the path above is
 *       refit.
 */
static inline void aabb3_tree_remove_leaf(AABB3Tree* t, int leaf){
	if (leaf == t->root){
		t->root = AABB_NULL;
		return;
	}

	int parent = t->nodes[leaf].parent;
	int grandParent = t->nodes[parent].parent;
	int sibling = (t->nodes[parent].left == leaf) ? t->nodes[parent].right
	                                              : t->nodes[parent].left;

	if (grandParent != AABB_NULL){
		if (t->nodes[grandParent].left == parent)
			t->nodes[grandParent].left = sibling;
		else
			t->nodes[grandParent].right = sibling;
		t->nodes[sibling].parent = grandParent;
		aabb3_tree_free_node(t, parent);
		aabb3_tree_refit(t, grandParent);
	}else{
		t->root = sibling;
		t->nodes[sibling].parent = AABB_NULL;
		aabb3_tree_free_node(t, parent);
	}
}

/**
 * @brief Add a box to the tree.
 *
 * @param t        Tree to insert into.
 * @param box      Tight box for the object. It is fattened by
 *                 @ref AABB3Tree::margin before being stored, so the box you
 *                 read back from aabb3_tree_get_fat_aabb() is larger than this.
 * @return A stable proxy id to pass to move/remove/leaf_*, or ::AABB_NULL if
 *         the pool is full. Ids stay valid until the proxy is removed or the
 *         pool is reset; they are node indices, not insertion order.
 *
 * @note The new proxy starts with an *empty* item list -- follow up with
 *       aabb3_tree_leaf_add() for each item it should carry. A leaf with no
 *       items is legal and is still reported by queries, with a count of 0.
 * @note Needs two free nodes when the tree is non-empty (the leaf plus an
 *       internal parent), so a pool sized for exactly N leaves will fail early.
 */
static inline int aabb3_tree_insert(AABB3Tree* t, AABB3 box){
	int id = aabb3_tree_allocate_node(t);
	if (id == AABB_NULL) return AABB_NULL;
	t->nodes[id].box = aabb3_fatten(box, t->margin);
	t->nodes[id].height = 0;
	if (!aabb3_tree_insert_leaf(t, id)){
		aabb3_tree_free_node(t, id);
		return AABB_NULL;
	}
	return id;
}

/**
 * @brief Remove a proxy and return its node to the pool.
 *
 * @param t       Tree to remove from.
 * @param proxyId Id returned by aabb3_tree_insert(). Must be a live proxy --
 *                removing twice, or passing an internal node's index,
 *                corrupts the tree. Not validated.
 *
 * @warning @p proxyId is dangling afterwards and may be handed out again by a
 *          later insert.
 */
static inline void aabb3_tree_remove(AABB3Tree* t, int proxyId){
	aabb3_tree_remove_leaf(t, proxyId);
	aabb3_tree_free_node(t, proxyId);
}

/**
 * @brief Update a proxy's box, reinserting only if it escaped its fat box.
 *
 * @param t       Tree holding the proxy.
 * @param proxyId Id returned by aabb3_tree_insert(). Stays valid either way --
 *                a reinsertion reuses the same node.
 * @param box     New tight box for the object. Refattened by
 *                @ref AABB3Tree::margin if a reinsertion happens.
 * @return 0 if @p box still fits inside the stored fat box, so nothing was
 *         done; 1 if the proxy was removed and reinserted.
 *
 * @note This is the per-frame update call for moving objects; the return value
 *       is useful as a cheap "this one actually moved" signal.
 */
static inline int aabb3_tree_move(AABB3Tree* t, int proxyId, AABB3 box){
	if (aabb3_contains(t->nodes[proxyId].box, box)) return 0;
	aabb3_tree_remove_leaf(t, proxyId);
	t->nodes[proxyId].box = aabb3_fatten(box, t->margin);
	t->nodes[proxyId].height = 0;
	t->nodes[proxyId].left = AABB_NULL;
	t->nodes[proxyId].right = AABB_NULL;
	aabb3_tree_insert_leaf(t, proxyId);
	return 1;
}

/**
 * @brief Read back a proxy's stored box.
 *
 * @param t       Tree holding the proxy.
 * @param proxyId Id returned by aabb3_tree_insert().
 * @return The *fattened* box, i.e. the tight box you inserted grown by
 *         @ref AABB3Tree::margin -- not the box you passed in.
 */
static inline AABB3 aabb3_tree_get_fat_aabb(const AABB3Tree* t, int proxyId){ return t->nodes[proxyId].box; }
/**
 * @brief Base of a proxy's item slots.
 *
 * @param t       Tree holding the proxy.
 * @param proxyId Id returned by aabb3_tree_insert().
 * @return Pointer to the leaf's first item slot, into the tree's shared slab.
 *         Only the first aabb3_tree_leaf_count() entries hold live items; the
 *         rest is uninitialised. Stays valid until the pool is reset, and is
 *         writable through the returned pointer.
 *
 * @note Not validated: asking an internal node or a freed slot returns a live
 *       pointer into the slab holding meaningless data.
 */
static inline void** aabb3_tree_leaf_items(const AABB3Tree* t, int proxyId){
	return t->items + (size_t)proxyId * (size_t)t->itemsPerLeaf;
}

/**
 * @brief How many items a proxy currently holds.
 *
 * @param t       Tree holding the proxy.
 * @param proxyId Id returned by aabb3_tree_insert().
 * @return Live item count, 0 to @ref AABB3Tree::itemsPerLeaf. Not validated.
 */
static inline int aabb3_tree_leaf_count(const AABB3Tree* t, int proxyId){ return t->nodes[proxyId].itemCount; }

/**
 * @brief Append one item to a proxy's list.
 *
 * @param t       Tree holding the proxy.
 * @param proxyId Id returned by aabb3_tree_insert().
 * @param item    Opaque pointer handed back by queries and ray casts. Never
 *                dereferenced or freed by the tree, so its lifetime is yours
 *                to manage. May be NULL, and may be another AABB tree.
 * @return 1 on success, 0 if the leaf already holds
 *         @ref AABB3Tree::itemsPerLeaf items. Duplicates are not detected.
 *
 * @note Does not touch the leaf's box. If the item widens what the proxy
 *       covers, follow with aabb3_tree_move().
 */
static inline int aabb3_tree_leaf_add(AABB3Tree* t, int proxyId, void* item){
	if (t->nodes[proxyId].itemCount >= t->itemsPerLeaf) return 0;
	aabb3_tree_leaf_items(t, proxyId)[t->nodes[proxyId].itemCount++] = item;
	return 1;
}

/**
 * @brief Drop the item at one slot, filling the hole with the last item.
 *
 * @param t       Tree holding the proxy.
 * @param proxyId Id returned by aabb3_tree_insert().
 * @param index   Slot to drop, 0 to aabb3_tree_leaf_count()-1. Out-of-range
 *                values are ignored.
 *
 * @warning Order is not preserved -- the last item is swapped into @p index.
 *          When erasing while iterating, do not advance the loop counter after
 *          a removal, or walk the list backwards.
 */
static inline void aabb3_tree_leaf_remove_at(AABB3Tree* t, int proxyId, int index){
	int n = t->nodes[proxyId].itemCount;
	if (index < 0 || index >= n) return;
	void** slots = aabb3_tree_leaf_items(t, proxyId);
	slots[index] = slots[n - 1];
	t->nodes[proxyId].itemCount = n - 1;
}

/**
 * @brief Drop the first slot holding a given pointer.
 *
 * @param t       Tree holding the proxy.
 * @param proxyId Id returned by aabb3_tree_insert().
 * @param item    Pointer to look for, compared by address, not by value.
 * @return 1 if an item was removed, 0 if @p item was not in the list.
 *
 * @warning Order is not preserved; see aabb3_tree_leaf_remove_at(). Only the
 *          first match goes, so a pointer added twice needs removing twice.
 */
static inline int aabb3_tree_leaf_remove(AABB3Tree* t, int proxyId, void* item){
	void** slots = aabb3_tree_leaf_items(t, proxyId);
	for (int i = 0; i < t->nodes[proxyId].itemCount; i++){
		if (slots[i] == item){ aabb3_tree_leaf_remove_at(t, proxyId, i); return 1; }
	}
	return 0;
}

/**
 * @brief Drop every item from a proxy, keeping the proxy itself.
 *
 * @param t       Tree holding the proxy.
 * @param proxyId Id returned by aabb3_tree_insert().
 *
 * @note The proxy stays in the tree and keeps its box; queries keep reporting
 *       it with a count of 0. Use aabb3_tree_remove() to drop the proxy too.
 */
static inline void aabb3_tree_leaf_clear(AABB3Tree* t, int proxyId){ t->nodes[proxyId].itemCount = 0; }
/**
 * @brief Height of the tree, as a rough quality metric.
 *
 * @param t Tree to measure.
 * @return 0 for an empty tree, otherwise the root's height (a single-leaf tree
 *         is also 0). Balancing keeps this near log2(proxies); a much larger
 *         value points at a degenerate distribution or too small a margin.
 */
static inline int aabb3_tree_height(const AABB3Tree* t){ return t->root == AABB_NULL ? 0 : t->nodes[t->root].height; }

/**
 * @brief Callback invoked once per leaf whose box overlaps the query box.
 *
 * @param items The matching proxy's item slots, as added with
 *              aabb3_tree_leaf_add(). Valid for @p count entries; do not
 *              hold on to it past the callback.
 * @param count How many items @p items holds. May be 0 for a proxy whose list
 *              is empty -- the leaf is still reported.
 * @param ctx   The opaque pointer passed to aabb3_tree_query(), untouched.
 * @return Non-zero to keep searching, 0 to stop the traversal early. Returning
 *         0 by accident (a plain `return;`-style 0) silently truncates results.
 *
 * @note Hits are reported against *fattened* boxes, so this over-reports:
 *       re-test with the tight box in the callback if you need exactness.
 * @note An item that is itself an AABB tree is not descended into: recognise
 *       it here and call aabb3_tree_query() on it yourself, forwarding the
 *       query box through @p ctx.
 */
typedef int (*AABB3QueryCallback)(void** items, int count, void* ctx);

/**
 * @brief Recursive half of the overlap query.
 *
 * @param t     Tree to search.
 * @param index Subtree root to descend from; ::AABB_NULL terminates.
 * @param box   Query box, tested against each node's stored box.
 * @param cb    Callback for matching leaves.
 * @param ctx   Opaque pointer forwarded to @p cb.
 * @return Non-zero if the traversal ran to completion, 0 if @p cb asked to
 *         stop. Propagated up so an early exit unwinds the whole recursion.
 *
 * @note Internal. Call aabb3_tree_query() instead.
 */
static inline int aabb3_tree_query_node(const AABB3Tree* t, int index, AABB3 box,
                                        AABB3QueryCallback cb, void* ctx){
	if (index == AABB_NULL) return 1;
	if (!aabb3_overlaps(t->nodes[index].box, box)) return 1;
	if (t->nodes[index].height == 0)
		return cb(aabb3_tree_leaf_items(t, index), t->nodes[index].itemCount, ctx);
	if (!aabb3_tree_query_node(t, t->nodes[index].left, box, cb, ctx)) return 0;
	return aabb3_tree_query_node(t, t->nodes[index].right, box, cb, ctx);
}

/**
 * @brief Report every proxy whose fat box overlaps a box.
 *
 * @param t   Tree to search.
 * @param box Query box in world space.
 * @param cb  Called once per overlapping proxy; return 0 from it to stop early.
 * @param ctx Opaque pointer forwarded to @p cb -- use it to reach your own
 *            state, since the callback gets nothing else.
 *
 * @warning Do not insert into or remove from @p t inside @p cb; the traversal
 *          holds node indices across the call.
 */
static inline void aabb3_tree_query(const AABB3Tree* t, AABB3 box,
                                    AABB3QueryCallback cb, void* ctx){
	aabb3_tree_query_node(t, t->root, box, cb, ctx);
}

/**
 * @brief Slab test of the segment origin + t*dir, t in [0, tmax], vs a box.
 *
 * @param b      Box to test.
 * @param origin Start point of the segment.
 * @param dir    Direction vector. Not required to be normalised -- t is
 *               measured in multiples of this vector, so passing the full
 *               displacement makes t a 0..1 fraction.
 * @param tmax   Upper bound on t, i.e. where the segment ends. Passed by value
 *               and only narrowed internally; the caller's value is unchanged.
 * @param tHit   Optional out-parameter for the entry t. May be NULL. Set only
 *               when the function returns non-zero, and set to 0 when @p origin
 *               is already inside @p b.
 * @return Non-zero on hit, 0 on miss.
 *
 * @note An axis whose direction component is under 1e-8 is treated as parallel:
 *       it contributes a containment test instead of a slab clip.
 */
static inline int aabb3_ray_hit(AABB3 b, Vec3f origin, Vec3f dir, float tmax, float* tHit){
	float tmin = 0.0f;
	for (int axis = 0; axis < 3; axis++){
		float o  = axis == 0 ? origin.x : (axis == 1 ? origin.y : origin.z);
		float d  = axis == 0 ? dir.x    : (axis == 1 ? dir.y    : dir.z);
		float lo = axis == 0 ? b.min.x  : (axis == 1 ? b.min.y  : b.min.z);
		float hi = axis == 0 ? b.max.x  : (axis == 1 ? b.max.y  : b.max.z);
		if (fabsf(d) < 1e-8f){
			if (o < lo || o > hi) return 0;
		}else{
			float inv = 1.0f / d;
			float t1 = (lo - o) * inv;
			float t2 = (hi - o) * inv;
			if (t1 > t2){ float tmp = t1; t1 = t2; t2 = tmp; }
			if (t1 > tmin) tmin = t1;
			if (t2 < tmax) tmax = t2;
			if (tmin > tmax) return 0;
		}
	}
	if (tHit) *tHit = tmin;
	return 1;
}

/**
 * @brief Callback invoked for each leaf the ray reaches, newest clip first.
 *
 * @param items       The hit proxy's item slots, as added with
 *                    aabb3_tree_leaf_add(). Valid for @p count entries; do
 *                    not hold on to it past the callback.
 * @param count       How many items @p items holds. May be 0.
 * @param origin      Ray origin, as passed to aabb3_tree_ray_cast().
 * @param dir         Ray direction, as passed to aabb3_tree_ray_cast().
 * @param maxFraction The current upper bound on t when this leaf was reached.
 * @param ctx         Opaque pointer passed to aabb3_tree_ray_cast(), untouched.
 * @return The new maximum fraction. A positive value clips the ray to that t
 *         (return your exact hit distance to find the closest hit); exactly 0
 *         terminates the cast; a negative value leaves the current bound
 *         unchanged, which ignores this hit and keeps going.
 */
typedef float (*AABB3RayCallback)(void** items, int count, Vec3f origin, Vec3f dir,
                                  float maxFraction, void* ctx);

/**
 * @brief Recursive half of the ray cast.
 *
 * @param t           Tree to traverse.
 * @param index       Subtree root to descend from; ::AABB_NULL terminates.
 * @param origin      Ray origin.
 * @param dir         Ray direction.
 * @param maxFraction In/out: the shared clip distance, narrowed as leaves
 *                    report hits. Traversal stops once it reaches 0. Pointer,
 *                    not a value, so sibling subtrees see earlier clips.
 * @param cb          Callback for each leaf reached.
 * @param ctx         Opaque pointer forwarded to @p cb.
 *
 * @note Internal. Call aabb3_tree_ray_cast() instead. Children are visited in
 *       fixed left-then-right order, not nearest-first, so the callback must
 *       do its own closest-hit bookkeeping.
 */
static inline void aabb3_tree_ray_node(const AABB3Tree* t, int index, Vec3f origin,
                                       Vec3f dir, float* maxFraction,
                                       AABB3RayCallback cb, void* ctx){
	if (index == AABB_NULL || *maxFraction <= 0.0f) return;
	float th;
	if (!aabb3_ray_hit(t->nodes[index].box, origin, dir, *maxFraction, &th)) return;
	if (t->nodes[index].height == 0){
		float r = cb(aabb3_tree_leaf_items(t, index), t->nodes[index].itemCount,
		             origin, dir, *maxFraction, ctx);
		if (r == 0.0f) *maxFraction = 0.0f;
		else if (r > 0.0f) *maxFraction = r;
		return;
	}
	aabb3_tree_ray_node(t, t->nodes[index].left, origin, dir, maxFraction, cb, ctx);
	aabb3_tree_ray_node(t, t->nodes[index].right, origin, dir, maxFraction, cb, ctx);
}

/**
 * @brief Cast a ray through the tree, reporting each proxy it reaches.
 *
 * @param t           Tree to cast against.
 * @param origin      Ray origin in world space.
 * @param dir         Ray direction. Not required to be normalised; fractions
 *                    below are measured in multiples of this vector.
 * @param maxFraction Initial upper bound on t. Pass 1.0f with @p dir set to the
 *                    full displacement to cast exactly that segment. Taken by
 *                    value -- the caller's variable is not modified.
 * @param cb          Called per hit leaf; its return value clips or ends the
 *                    cast (see ::AABB3RayCallback).
 * @param ctx         Opaque pointer forwarded to @p cb.
 *
 * @warning Do not modify @p t inside @p cb.
 */
static inline void aabb3_tree_ray_cast(const AABB3Tree* t, Vec3f origin, Vec3f dir,
                                       float maxFraction, AABB3RayCallback cb, void* ctx){
	float mf = maxFraction;
	aabb3_tree_ray_node(t, t->root, origin, dir, &mf, cb, ctx);
}

#endif

/*
 * MIT License
 * Copyright (c) 2026 Matthew Kabat
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
