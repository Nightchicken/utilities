# AABB.h

Dynamic AABB tree (bounding volume hierarchy) in 2D and 3D, backed by an
arena-allocated node pool. Classic Box2D-style tree: surface-area-heuristic
insertion, AVL rotation balancing, fat AABBs, overlap query and ray cast.

Depends on `vec.h`, `arena.h`, `<math.h>`, `<stddef.h>`.

## Change log

- Converted every comment to Doxygen (`/** ... */` with `@brief`, `@param`,
  `@return`, `@note`, `@warning`, `@p`, `@ref`) so clangd/ccls surface
  parameter meanings on hover and in signature help. Behaviour is unchanged --
  comments only, verified by diffing out every comment line and by a smoke
  test over insert/query/ray cast.
- Struct fields documented with `@var Struct::field`, so hovering a member of
  `AABB2`/`AABB3`, `AABB2Node`/`AABB3Node` or `AABB2Tree`/`AABB3Tree` explains
  what it holds and what its sentinel values mean.
- The loose intro paragraph became a proper `@file` block with a usage example,
  which LSPs show when hovering the `#include`.
- Internal helpers (`*_tree_allocate_node`, `*_tree_free_node`,
  `*_tree_balance`, `*_tree_refit`, `*_tree_insert_leaf`, `*_tree_remove_leaf`,
  `*_tree_query_node`, `*_tree_ray_node`) are marked as internal in their docs,
  pointing at the public call to use instead.
- **Breaking:** `userData` is now `void*` instead of `int`. This changes the
  signatures of `*_tree_insert` (last parameter), `*_tree_get_user_data`
  (return type), and both callback typedefs `AABB2/3QueryCallback` and
  `AABB2/3RayCallback` (first parameter). Internal nodes and freshly allocated
  nodes hold `NULL` rather than `0`. Callers storing an integer id can keep
  doing so through `(void*)(intptr_t)id` and back, but the natural port is to
  pass a pointer to your object directly. The tree never dereferences, copies
  through, or frees the pointer -- its lifetime is entirely yours.

## Usage

```c
#include "AABB.h"

Arena a = arena_create(1 << 20);

// capacity is in NODES, not proxies: N leaves need 2*N-1 nodes.
AABB2Tree t = aabb2_tree_create(&a, 2 * MAX_BODIES - 1, 0.1f /* fat margin */);

int proxy = aabb2_tree_insert(&t, aabb2_make(lo, hi), &entity);
if (proxy == AABB_NULL) { /* pool full */ }

// each frame; returns 0 when the box still fits its fat box (no work done)
aabb2_tree_move(&t, proxy, aabb2_make(newLo, newHi));

static int on_hit(void* userData, void* ctx){
    (void)ctx;
    Entity* e = (Entity*)userData;
    /* ... */
    return 1;              // non-zero keeps searching, 0 stops early
}
aabb2_tree_query(&t, queryBox, on_hit, &myState);

aabb2_tree_remove(&t, proxy);
arena_destroy(&a);         // frees the node pool with everything else
```

The 3D API is identical with `aabb3_`/`AABB3`/`Vec3f` substituted.

## API

Box maths (`aabb2_*` / `aabb3_*`):

| Function | Notes |
| --- | --- |
| `make(min, max)` | Sorts the corners; use it when order isn't guaranteed |
| `union(a, b)` | Bounding union, so it also covers the gap between disjoint boxes |
| `cost(b)` | Perimeter in 2D, surface area in 3D -- the SAH metric |
| `overlaps(a, b)` | Inclusive: touching faces count |
| `contains(a, b)` | "Does **a** contain **b**" -- order is easy to reverse |
| `fatten(b, margin)` | Grows all faces outward; negative shrinks and can invert |
| `ray_hit(b, origin, dir, tmax, tHit)` | Slab test; `tHit` may be NULL |

Tree (`aabb2_tree_*` / `aabb3_tree_*`):

| Function | Notes |
| --- | --- |
| `create(arena, capacity, margin)` | `capacity` is nodes; check `t.capacity != 0` |
| `reset_pool(t)` | Empties the tree; invalidates every proxy id |
| `insert(t, box, userData)` | `userData` is `void*`; returns proxy id or `AABB_NULL`; needs 2 free nodes |
| `remove(t, proxyId)` | Proxy id dangles afterwards |
| `move(t, proxyId, box)` | 0 = still fit, 1 = reinserted; id stays valid |
| `get_fat_aabb(t, proxyId)` | Returns the *fattened* box, not what you inserted |
| `get_user_data(t, proxyId)` | Returns the stored `void*`; not validated |
| `height(t)` | 0 when empty; should stay near log2(proxies) |
| `query(t, box, cb, ctx)` | `cb` returns non-zero to continue, 0 to stop |
| `ray_cast(t, origin, dir, maxFraction, cb, ctx)` | `cb` returns the new clip |

## Gotchas worth knowing

- **`capacity` is nodes, not proxies.** Sizing a pool for exactly N leaves fails
  partway through, because each insert into a non-empty tree also consumes an
  internal parent node. Use `2 * N - 1`.
- **Queries over-report.** Hits are tested against fattened boxes; re-test with
  your tight box inside the callback when you need exact answers.
- **`AABB_NULL` (-1)** is the universal sentinel: absent parent/child, empty
  root, exhausted free list, and the failure return of `insert`.
- **Ray direction need not be normalised.** `t` is measured in multiples of
  `dir`, so passing the full displacement with `maxFraction = 1.0f` casts
  exactly that segment.
- **Ray callback return value** is a three-way signal: positive clips the ray to
  that distance (return your hit distance for closest-hit), exactly `0` ends the
  cast, negative leaves the current bound alone (ignore this hit, keep going).
- **Ray traversal is left-then-right, not nearest-first**, so closest-hit
  bookkeeping is the callback's job.
- **Do not insert or remove inside a query or ray callback** -- the traversal
  holds node indices across the call.
- Proxy ids are node indices, not insertion order, and are recycled after removal.
- **`userData` is a borrowed `void*`.** The tree stores it and hands it back; it
  never dereferences or frees it. Keeping a proxy alive past the object it
  points at leaves a dangling pointer that queries will happily return.

## LSP setup

clangd picks the doc comments up with no configuration. For this header-only
project a minimal `compile_flags.txt` at the repo root gets include resolution
right:

```
-I./headers
-std=c11
```
