// SPDX-License-Identifier: GPL-2.0
/*
 * Moving/copying garbage collector
 *
 * Copyright 2012 Google, Inc.
 */

#include "bcache.h"
#include "btree.h"
#include "debug.h"
#include "request.h"

#include <trace/events/bcache.h>

struct moving_io {
	struct closure		cl;
	struct keybuf_key	*w;
	struct data_insert_op	op;
	struct bbio		bio;
};

static bool moving_pred(struct keybuf *buf, struct bkey *k)
{
	struct cache_set *c = container_of(buf, struct cache_set,
					   moving_gc_keys);
	unsigned int i;

	for (i = 0; i < KEY_PTRS(k); i++)
		if (ptr_available(c, k, i) &&
		    GC_MOVE(PTR_BUCKET(c, k, i))) //如果key对应的bucket被标记为move，在bch_moving_gc中被设置
			return true;

	return false;
}

/* Moving GC - IO loop */

static void moving_io_destructor(struct closure *cl)
{
	struct moving_io *io = container_of(cl, struct moving_io, cl);

	kfree(io);
}

static void write_moving_finish(struct closure *cl)
{
	struct moving_io *io = container_of(cl, struct moving_io, cl);
	struct bio *bio = &io->bio.bio;

	bio_free_pages(bio);

	if (io->op.replace_collision)
		trace_bcache_gc_copy_collision(&io->w->key);

	bch_keybuf_del(&io->op.c->moving_gc_keys, io->w);

	up(&io->op.c->moving_in_flight);

	closure_return_with_destructor(cl, moving_io_destructor);
}

static void read_moving_endio(struct bio *bio)
{
	struct bbio *b = container_of(bio, struct bbio, bio);
	struct moving_io *io = container_of(bio->bi_private,
					    struct moving_io, cl);

	if (bio->bi_status)
		io->op.status = bio->bi_status;
	else if (!KEY_DIRTY(&b->key) &&
		 ptr_stale(io->op.c, &b->key, 0)) {
		io->op.status = BLK_STS_IOERR;
	}

	bch_bbio_endio(io->op.c, bio, bio->bi_status, "reading data to move");
}

static void moving_init(struct moving_io *io)
{
	struct bio *bio = &io->bio.bio;

	bio_init(bio, bio->bi_inline_vecs,
		 DIV_ROUND_UP(KEY_SIZE(&io->w->key), PAGE_SECTORS));
	bio_get(bio);
	bio_set_prio(bio, IOPRIO_PRIO_VALUE(IOPRIO_CLASS_IDLE, 0));

	bio->bi_iter.bi_size	= KEY_SIZE(&io->w->key) << 9;
	bio->bi_private		= &io->cl;
	bch_bio_map(bio, NULL);
}

static void write_moving(struct closure *cl)
{
	struct moving_io *io = container_of(cl, struct moving_io, cl);
	struct data_insert_op *op = &io->op;

	if (!op->status) {
		moving_init(io);

		io->bio.bio.bi_iter.bi_sector = KEY_START(&io->w->key);
		op->write_prio		= 1;
		op->bio			= &io->bio.bio;

		op->writeback		= KEY_DIRTY(&io->w->key);
		op->csum		= KEY_CSUM(&io->w->key);

		bkey_copy(&op->replace_key, &io->w->key);
		op->replace		= true;

		closure_call(&op->cl, bch_data_insert, NULL, cl); //更新内存b+tree
	}

	continue_at(cl, write_moving_finish, op->wq);
}

static void read_moving_submit(struct closure *cl)
{
	struct moving_io *io = container_of(cl, struct moving_io, cl);
	struct bio *bio = &io->bio.bio;
    //该bio的bi_sector由bch_moving_gc中keybuf * w 中获得
	bch_submit_bbio(bio, io->op.c, &io->w->key, 0);

	continue_at(cl, write_moving, io->op.wq); //从缓存设备读取数据后，写入缓存设备
}

static void read_moving(struct cache_set *c)
{
	struct keybuf_key *w;
	struct moving_io *io;
	struct bio *bio;
	struct closure cl;

	closure_init_stack(&cl);

	/* XXX: if we error, background writeback could stall indefinitely */

	while (!test_bit(CACHE_SET_STOPPING, &c->flags)) {
		w = bch_keybuf_next_rescan(c, &c->moving_gc_keys,  //bch_keybuf_next_rescan每次从红黑树返回一个struct keybuf_key
					   &MAX_KEY, moving_pred);             //填充moving_gc_keys
		if (!w)
			break;

		if (ptr_stale(c, &w->key, 0)) {
			bch_keybuf_del(&c->moving_gc_keys, w);
			continue;
		}

		io = kzalloc(sizeof(struct moving_io) + sizeof(struct bio_vec)
			     * DIV_ROUND_UP(KEY_SIZE(&w->key), PAGE_SECTORS),
			     GFP_KERNEL);
		if (!io)
			goto err;

		w->private	= io;
		io->w		= w;
		io->op.inode	= KEY_INODE(&w->key);
		io->op.c	= c;
		io->op.wq	= c->moving_gc_wq;

		moving_init(io);  //根据io生成&io->bio.bio
		bio = &io->bio.bio;

		bio_set_op_attrs(bio, REQ_OP_READ, 0); //从缓存设备读取
		bio->bi_end_io	= read_moving_endio;

		if (bch_bio_alloc_pages(bio, GFP_KERNEL))
			goto err;

		trace_bcache_gc_copy(&w->key);

		down(&c->moving_in_flight);
		closure_call(&io->cl, read_moving_submit, NULL, &cl);  //延迟
	}

	if (0) {
err:		if (!IS_ERR_OR_NULL(w->private))
			kfree(w->private);

		bch_keybuf_del(&c->moving_gc_keys, w);
	}

	closure_sync(&cl);
}

static bool bucket_cmp(struct bucket *l, struct bucket *r)
{
	return GC_SECTORS_USED(l) < GC_SECTORS_USED(r);
}

static unsigned int bucket_heap_top(struct cache *ca)
{
	struct bucket *b;

	return (b = heap_peek(&ca->heap)) ? GC_SECTORS_USED(b) : 0;
}

void bch_moving_gc(struct cache_set *c)
{
	struct cache *ca;
	struct bucket *b;
	unsigned int i;

	if (!c->copy_gc_enabled)
		return;

	mutex_lock(&c->bucket_lock);

	for_each_cache(ca, c, i) {  //遍历cache set中的所有cache设备
		unsigned int sectors_to_move = 0;
		unsigned int reserve_sectors = ca->sb.bucket_size *
			     fifo_used(&ca->free[RESERVE_MOVINGGC]);

		ca->heap.used = 0;

		for_each_bucket(b, ca) {  //遍历cache设备中的bucket
			if (GC_MARK(b) == GC_MARK_METADATA ||  //元数据bucket不能回收
			    !GC_SECTORS_USED(b) ||             //bucket的使用量为0
			    GC_SECTORS_USED(b) == ca->sb.bucket_size ||  //bucket使用量为满
			    atomic_read(&b->pin))              //bucket的pin等于0表示不能回收
				continue;  //跳过不能回收的bucket

			if (!heap_full(&ca->heap)) {
				sectors_to_move += GC_SECTORS_USED(b);
				heap_add(&ca->heap, b, bucket_cmp);
			} else if (bucket_cmp(b, heap_peek(&ca->heap))) {
				sectors_to_move -= bucket_heap_top(ca);
				sectors_to_move += GC_SECTORS_USED(b);

				ca->heap.data[0] = b;
				heap_sift(&ca->heap, 0, bucket_cmp);
			}
		}

		while (sectors_to_move > reserve_sectors) {
			heap_pop(&ca->heap, b, bucket_cmp);
			sectors_to_move -= GC_SECTORS_USED(b);
		}

		while (heap_pop(&ca->heap, b, bucket_cmp))
			SET_GC_MOVE(b, 1);
	}

	mutex_unlock(&c->bucket_lock);

	c->moving_gc_keys.last_scanned = ZERO_KEY;

	read_moving(c);
}

void bch_moving_init_cache_set(struct cache_set *c)
{
	bch_keybuf_init(&c->moving_gc_keys);
	sema_init(&c->moving_in_flight, 64);
}
