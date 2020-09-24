#include "common.h"
#ifdef MODSECTION
#undef MODSECTION
#define MODSECTION "-blk_descr"
#endif

#include "blk_descr_file.h"

static inline void list_assign(struct list_head *dst, struct list_head *src)
{
	dst->next = src->next;
	dst->prev = src->prev;

	src->next->prev = dst;
	src->prev->next = dst;
}

static inline void blk_descr_file_init(struct blk_descr_file *blk_descr,
				       struct list_head *rangelist)
{
	list_assign(&blk_descr->rangelist, rangelist);
}

static inline void blk_descr_file_done(struct blk_descr_file *blk_descr)
{
	while (!list_empty(&blk_descr->rangelist)) {
		blk_range_link_t *range_link =
			list_entry(blk_descr->rangelist.next, blk_range_link_t, link);

		list_del(&range_link->link);
		kfree(range_link);
	}
}

void blk_descr_file_pool_init(blk_descr_pool_t *pool)
{
	blk_descr_pool_init(pool, 0);
}

void _blk_descr_file_cleanup(void *descr_array, size_t count)
{
	size_t inx;
	struct blk_descr_file *file_blocks = descr_array;

	for (inx = 0; inx < count; ++inx)
		blk_descr_file_done(file_blocks + inx);
}

void blk_descr_file_pool_done(blk_descr_pool_t *pool)
{
	blk_descr_pool_done(pool, _blk_descr_file_cleanup);
}

static union blk_descr_unify _blk_descr_file_allocate(void *descr_array, size_t index, void *arg)
{
	union blk_descr_unify blk_descr;
	struct blk_descr_file *file_blocks = descr_array;

	blk_descr.file = &file_blocks[index];

	blk_descr_file_init(blk_descr.file, (struct list_head *)arg);

	return blk_descr;
}

int blk_descr_file_pool_add(blk_descr_pool_t *pool, struct list_head *rangelist)
{
	union blk_descr_unify blk_descr = blk_descr_pool_alloc(
		pool, sizeof(struct blk_descr_file), _blk_descr_file_allocate, (void *)rangelist);

	if (NULL == blk_descr.ptr) {
		pr_err("Failed to allocate block descriptor\n");
		return -ENOMEM;
	}

	return SUCCESS;
}

union blk_descr_unify blk_descr_file_pool_take(blk_descr_pool_t *pool)
{
	return blk_descr_pool_take(pool, sizeof(struct blk_descr_file));
}
