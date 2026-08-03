#ifndef AABB_H
#define AABB_H
#include "vec.h"
#include "arena.h"
#include <math.h>

// Sentinel "no node" index, shared by both trees.
#define AABB_NULL (-1)

static inline int aabb_max_int(int a, int b){ return a > b ? a : b; }

// The dynamic AABB tree below comes in 2D (AABB2 / AABB2Tree, aabb2_* and
// aabb2_tree_* functions) and 3D (AABB3 / AABB3Tree, aabb3_* and aabb3_tree_*)
// flavours. Both are the classic Box2D-style tree: surface-area-heuristic
// insertion, AVL-style rotation balancing, fat AABBs, overlap query and ray
// cast. The node pool is carved once out of an Arena and addressed by index;
// free slots form an intrusive linked list through the `next` field, so there
// is no per-operation heap allocation. A tree of N leaves needs 2*N-1 nodes.

// ============================================================================
// 2D
// ============================================================================

// 2D axis-aligned box. topLeft is the min corner, bottomRight the max.
typedef struct AABB2{
	Vec2f topLeft;
	Vec2f bottomRight;
}AABB2;

static inline AABB2 aabb2_make(Vec2f min, Vec2f max){
	AABB2 b;
	b.topLeft.x = fminf(min.x, max.x);
	b.topLeft.y = fminf(min.y, max.y);
	b.bottomRight.x = fmaxf(min.x, max.x);
	b.bottomRight.y = fmaxf(min.y, max.y);
	return b;
}

static inline AABB2 aabb2_union(AABB2 a, AABB2 b){
	AABB2 r;
	r.topLeft.x = fminf(a.topLeft.x, b.topLeft.x);
	r.topLeft.y = fminf(a.topLeft.y, b.topLeft.y);
	r.bottomRight.x = fmaxf(a.bottomRight.x, b.bottomRight.x);
	r.bottomRight.y = fmaxf(a.bottomRight.y, b.bottomRight.y);
	return r;
}

// Perimeter, used as the 2D surface-area heuristic cost.
static inline float aabb2_cost(AABB2 b){
	float wx = b.bottomRight.x - b.topLeft.x;
	float wy = b.bottomRight.y - b.topLeft.y;
	return 2.0f * (wx + wy);
}

static inline int aabb2_overlaps(AABB2 a, AABB2 b){
	return !(a.bottomRight.x < b.topLeft.x || a.topLeft.x > b.bottomRight.x ||
	         a.bottomRight.y < b.topLeft.y || a.topLeft.y > b.bottomRight.y);
}

static inline int aabb2_contains(AABB2 a, AABB2 b){
	return a.topLeft.x <= b.topLeft.x && a.topLeft.y <= b.topLeft.y &&
	       b.bottomRight.x <= a.bottomRight.x && b.bottomRight.y <= a.bottomRight.y;
}

static inline AABB2 aabb2_fatten(AABB2 b, float margin){
	AABB2 r;
	r.topLeft.x = b.topLeft.x - margin;
	r.topLeft.y = b.topLeft.y - margin;
	r.bottomRight.x = b.bottomRight.x + margin;
	r.bottomRight.y = b.bottomRight.y + margin;
	return r;
}

typedef struct AABB2Node{
	AABB2 box;
	int userData;
	int parent;
	int left;
	int right;
	int height;   // 0 for leaves, -1 when free, else 1 + max(child heights)
	int next;     // free-list link when free
}AABB2Node;

typedef struct AABB2Tree{
	AABB2Node* nodes;
	int count;
	int capacity;
	int root;
	int freeList;
	float margin;
}AABB2Tree;

static inline void aabb2_tree_reset_pool(AABB2Tree* t){
	t->count = 0;
	t->root = AABB_NULL;
	for (int i = 0; i < t->capacity; i++){
		t->nodes[i].next = i + 1;
		t->nodes[i].height = -1;
	}
	if (t->capacity > 0) t->nodes[t->capacity - 1].next = AABB_NULL;
	t->freeList = t->capacity > 0 ? 0 : AABB_NULL;
}

static inline AABB2Tree aabb2_tree_create(Arena* arena, int capacity, float margin){
	AABB2Tree t;
	if (capacity < 1) capacity = 1;
	t.nodes = (AABB2Node*)arena_alloc(arena, sizeof(AABB2Node) * (size_t)capacity);
	t.capacity = t.nodes ? capacity : 0;
	t.margin = margin;
	aabb2_tree_reset_pool(&t);
	return t;
}

static inline int aabb2_tree_allocate_node(AABB2Tree* t){
	if (t->freeList == AABB_NULL) return AABB_NULL;
	int id = t->freeList;
	t->freeList = t->nodes[id].next;
	t->nodes[id].parent = AABB_NULL;
	t->nodes[id].left = AABB_NULL;
	t->nodes[id].right = AABB_NULL;
	t->nodes[id].height = 0;
	t->nodes[id].userData = 0;
	t->count++;
	return id;
}

static inline void aabb2_tree_free_node(AABB2Tree* t, int id){
	t->nodes[id].next = t->freeList;
	t->nodes[id].height = -1;
	t->freeList = id;
	t->count--;
}

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
	t->nodes[newParent].userData = 0;
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

// Insert a box; returns a stable proxy id, or AABB_NULL if the pool is full.
static inline int aabb2_tree_insert(AABB2Tree* t, AABB2 box, int userData){
	int id = aabb2_tree_allocate_node(t);
	if (id == AABB_NULL) return AABB_NULL;
	t->nodes[id].box = aabb2_fatten(box, t->margin);
	t->nodes[id].userData = userData;
	t->nodes[id].height = 0;
	if (!aabb2_tree_insert_leaf(t, id)){
		aabb2_tree_free_node(t, id);
		return AABB_NULL;
	}
	return id;
}

static inline void aabb2_tree_remove(AABB2Tree* t, int proxyId){
	aabb2_tree_remove_leaf(t, proxyId);
	aabb2_tree_free_node(t, proxyId);
}

// Returns 0 if the box still fits its fat box (no work), 1 if reinserted.
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

static inline AABB2 aabb2_tree_get_fat_aabb(const AABB2Tree* t, int proxyId){ return t->nodes[proxyId].box; }
static inline int aabb2_tree_get_user_data(const AABB2Tree* t, int proxyId){ return t->nodes[proxyId].userData; }
static inline int aabb2_tree_height(const AABB2Tree* t){ return t->root == AABB_NULL ? 0 : t->nodes[t->root].height; }

// Callback returns non-zero to keep searching, 0 to stop early.
typedef int (*AABB2QueryCallback)(int userData, void* ctx);

static inline int aabb2_tree_query_node(const AABB2Tree* t, int index, AABB2 box,
                                        AABB2QueryCallback cb, void* ctx){
	if (index == AABB_NULL) return 1;
	if (!aabb2_overlaps(t->nodes[index].box, box)) return 1;
	if (t->nodes[index].height == 0)
		return cb(t->nodes[index].userData, ctx);
	if (!aabb2_tree_query_node(t, t->nodes[index].left, box, cb, ctx)) return 0;
	return aabb2_tree_query_node(t, t->nodes[index].right, box, cb, ctx);
}

static inline void aabb2_tree_query(const AABB2Tree* t, AABB2 box,
                                    AABB2QueryCallback cb, void* ctx){
	aabb2_tree_query_node(t, t->root, box, cb, ctx);
}

// Slab test of segment origin + t*dir, t in [0, tmax], against box b.
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

// Callback returns the new max fraction: positive clips the ray, 0 terminates,
// negative leaves the current max unchanged.
typedef float (*AABB2RayCallback)(int userData, Vec2f origin, Vec2f dir,
                                  float maxFraction, void* ctx);

static inline void aabb2_tree_ray_node(const AABB2Tree* t, int index, Vec2f origin,
                                       Vec2f dir, float* maxFraction,
                                       AABB2RayCallback cb, void* ctx){
	if (index == AABB_NULL || *maxFraction <= 0.0f) return;
	float th;
	if (!aabb2_ray_hit(t->nodes[index].box, origin, dir, *maxFraction, &th)) return;
	if (t->nodes[index].height == 0){
		float r = cb(t->nodes[index].userData, origin, dir, *maxFraction, ctx);
		if (r == 0.0f) *maxFraction = 0.0f;
		else if (r > 0.0f) *maxFraction = r;
		return;
	}
	aabb2_tree_ray_node(t, t->nodes[index].left, origin, dir, maxFraction, cb, ctx);
	aabb2_tree_ray_node(t, t->nodes[index].right, origin, dir, maxFraction, cb, ctx);
}

static inline void aabb2_tree_ray_cast(const AABB2Tree* t, Vec2f origin, Vec2f dir,
                                       float maxFraction, AABB2RayCallback cb, void* ctx){
	float mf = maxFraction;
	aabb2_tree_ray_node(t, t->root, origin, dir, &mf, cb, ctx);
}

// ============================================================================
// 3D
// ============================================================================

// 3D axis-aligned box. min is the low corner, max the high corner.
typedef struct AABB3{
	Vec3f min;
	Vec3f max;
}AABB3;

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

// Surface area, used as the 3D surface-area heuristic cost.
static inline float aabb3_cost(AABB3 b){
	float wx = b.max.x - b.min.x;
	float wy = b.max.y - b.min.y;
	float wz = b.max.z - b.min.z;
	return 2.0f * (wx * wy + wy * wz + wz * wx);
}

static inline int aabb3_overlaps(AABB3 a, AABB3 b){
	return !(a.max.x < b.min.x || a.min.x > b.max.x ||
	         a.max.y < b.min.y || a.min.y > b.max.y ||
	         a.max.z < b.min.z || a.min.z > b.max.z);
}

static inline int aabb3_contains(AABB3 a, AABB3 b){
	return a.min.x <= b.min.x && a.min.y <= b.min.y && a.min.z <= b.min.z &&
	       b.max.x <= a.max.x && b.max.y <= a.max.y && b.max.z <= a.max.z;
}

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

typedef struct AABB3Node{
	AABB3 box;
	int userData;
	int parent;
	int left;
	int right;
	int height;
	int next;
}AABB3Node;

typedef struct AABB3Tree{
	AABB3Node* nodes;
	int count;
	int capacity;
	int root;
	int freeList;
	float margin;
}AABB3Tree;

static inline void aabb3_tree_reset_pool(AABB3Tree* t){
	t->count = 0;
	t->root = AABB_NULL;
	for (int i = 0; i < t->capacity; i++){
		t->nodes[i].next = i + 1;
		t->nodes[i].height = -1;
	}
	if (t->capacity > 0) t->nodes[t->capacity - 1].next = AABB_NULL;
	t->freeList = t->capacity > 0 ? 0 : AABB_NULL;
}

static inline AABB3Tree aabb3_tree_create(Arena* arena, int capacity, float margin){
	AABB3Tree t;
	if (capacity < 1) capacity = 1;
	t.nodes = (AABB3Node*)arena_alloc(arena, sizeof(AABB3Node) * (size_t)capacity);
	t.capacity = t.nodes ? capacity : 0;
	t.margin = margin;
	aabb3_tree_reset_pool(&t);
	return t;
}

static inline int aabb3_tree_allocate_node(AABB3Tree* t){
	if (t->freeList == AABB_NULL) return AABB_NULL;
	int id = t->freeList;
	t->freeList = t->nodes[id].next;
	t->nodes[id].parent = AABB_NULL;
	t->nodes[id].left = AABB_NULL;
	t->nodes[id].right = AABB_NULL;
	t->nodes[id].height = 0;
	t->nodes[id].userData = 0;
	t->count++;
	return id;
}

static inline void aabb3_tree_free_node(AABB3Tree* t, int id){
	t->nodes[id].next = t->freeList;
	t->nodes[id].height = -1;
	t->freeList = id;
	t->count--;
}

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
	t->nodes[newParent].userData = 0;
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

static inline int aabb3_tree_insert(AABB3Tree* t, AABB3 box, int userData){
	int id = aabb3_tree_allocate_node(t);
	if (id == AABB_NULL) return AABB_NULL;
	t->nodes[id].box = aabb3_fatten(box, t->margin);
	t->nodes[id].userData = userData;
	t->nodes[id].height = 0;
	if (!aabb3_tree_insert_leaf(t, id)){
		aabb3_tree_free_node(t, id);
		return AABB_NULL;
	}
	return id;
}

static inline void aabb3_tree_remove(AABB3Tree* t, int proxyId){
	aabb3_tree_remove_leaf(t, proxyId);
	aabb3_tree_free_node(t, proxyId);
}

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

static inline AABB3 aabb3_tree_get_fat_aabb(const AABB3Tree* t, int proxyId){ return t->nodes[proxyId].box; }
static inline int aabb3_tree_get_user_data(const AABB3Tree* t, int proxyId){ return t->nodes[proxyId].userData; }
static inline int aabb3_tree_height(const AABB3Tree* t){ return t->root == AABB_NULL ? 0 : t->nodes[t->root].height; }

typedef int (*AABB3QueryCallback)(int userData, void* ctx);

static inline int aabb3_tree_query_node(const AABB3Tree* t, int index, AABB3 box,
                                        AABB3QueryCallback cb, void* ctx){
	if (index == AABB_NULL) return 1;
	if (!aabb3_overlaps(t->nodes[index].box, box)) return 1;
	if (t->nodes[index].height == 0)
		return cb(t->nodes[index].userData, ctx);
	if (!aabb3_tree_query_node(t, t->nodes[index].left, box, cb, ctx)) return 0;
	return aabb3_tree_query_node(t, t->nodes[index].right, box, cb, ctx);
}

static inline void aabb3_tree_query(const AABB3Tree* t, AABB3 box,
                                    AABB3QueryCallback cb, void* ctx){
	aabb3_tree_query_node(t, t->root, box, cb, ctx);
}

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

typedef float (*AABB3RayCallback)(int userData, Vec3f origin, Vec3f dir,
                                  float maxFraction, void* ctx);

static inline void aabb3_tree_ray_node(const AABB3Tree* t, int index, Vec3f origin,
                                       Vec3f dir, float* maxFraction,
                                       AABB3RayCallback cb, void* ctx){
	if (index == AABB_NULL || *maxFraction <= 0.0f) return;
	float th;
	if (!aabb3_ray_hit(t->nodes[index].box, origin, dir, *maxFraction, &th)) return;
	if (t->nodes[index].height == 0){
		float r = cb(t->nodes[index].userData, origin, dir, *maxFraction, ctx);
		if (r == 0.0f) *maxFraction = 0.0f;
		else if (r > 0.0f) *maxFraction = r;
		return;
	}
	aabb3_tree_ray_node(t, t->nodes[index].left, origin, dir, maxFraction, cb, ctx);
	aabb3_tree_ray_node(t, t->nodes[index].right, origin, dir, maxFraction, cb, ctx);
}

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
