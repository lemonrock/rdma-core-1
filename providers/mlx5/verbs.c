/*
 * Copyright (c) 2012 Mellanox Technologies, Inc.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <config.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#include <util/compiler.h>
#include <util/mmio.h>

#include "mlx5.h"
#include "mlx5-abi.h"
#include "wqe.h"

int mlx5_single_threaded = 0;

static inline int is_xrc_tgt(int type)
{
	return type == IBV_QPT_XRC_RECV;
}

int mlx5_query_device(struct ibv_context *context, struct ibv_device_attr *attr)
{
	struct ibv_query_device cmd;
	uint64_t raw_fw_ver;
	unsigned major, minor, sub_minor;
	int ret;

	ret = ibv_cmd_query_device(context, attr, &raw_fw_ver, &cmd, sizeof cmd);
	if (ret)
		return ret;

	major     = (raw_fw_ver >> 32) & 0xffff;
	minor     = (raw_fw_ver >> 16) & 0xffff;
	sub_minor = raw_fw_ver & 0xffff;

	snprintf(attr->fw_ver, sizeof attr->fw_ver,
		 "%d.%d.%04d", major, minor, sub_minor);

	return 0;
}

static int mlx5_read_clock(struct ibv_context *context, uint64_t *cycles)
{
	unsigned int clockhi, clocklo, clockhi1;
	int i;
	struct mlx5_context *ctx = to_mctx(context);

	if (!ctx->hca_core_clock)
		return -EOPNOTSUPP;

	/* Handle wraparound */
	for (i = 0; i < 2; i++) {
		clockhi = be32toh(mmio_read32_be(ctx->hca_core_clock));
		clocklo = be32toh(mmio_read32_be(ctx->hca_core_clock + 4));
		clockhi1 = be32toh(mmio_read32_be(ctx->hca_core_clock));
		if (clockhi == clockhi1)
			break;
	}

	*cycles = (uint64_t)clockhi << 32 | (uint64_t)clocklo;

	return 0;
}

int mlx5_query_rt_values(struct ibv_context *context,
			 struct ibv_values_ex *values)
{
	uint32_t comp_mask = 0;
	int err = 0;

	if (!check_comp_mask(values->comp_mask, IBV_VALUES_MASK_RAW_CLOCK))
		return EINVAL;

	if (values->comp_mask & IBV_VALUES_MASK_RAW_CLOCK) {
		uint64_t cycles;

		err = mlx5_read_clock(context, &cycles);
		if (!err) {
			values->raw_clock.tv_sec = 0;
			values->raw_clock.tv_nsec = cycles;
			comp_mask |= IBV_VALUES_MASK_RAW_CLOCK;
		}
	}

	values->comp_mask = comp_mask;

	return err;
}

int mlx5_query_port(struct ibv_context *context, uint8_t port,
		     struct ibv_port_attr *attr)
{
	struct ibv_query_port cmd;

	return ibv_cmd_query_port(context, port, attr, &cmd, sizeof cmd);
}

struct ibv_pd *mlx5_alloc_pd(struct ibv_context *context)
{
	struct ibv_alloc_pd       cmd;
	struct mlx5_alloc_pd_resp resp;
	struct mlx5_pd		 *pd;

	pd = calloc(1, sizeof *pd);
	if (!pd)
		return NULL;

	if (ibv_cmd_alloc_pd(context, &pd->ibv_pd, &cmd, sizeof cmd,
			     &resp.ibv_resp, sizeof resp)) {
		free(pd);
		return NULL;
	}

	atomic_init(&pd->refcount, 1);
	pd->pdn = resp.pdn;

	return &pd->ibv_pd;
}

static void mlx5_put_bfreg_index(struct mlx5_context *ctx, uint32_t bfreg_dyn_index)
{
	pthread_mutex_lock(&ctx->dyn_bfregs_mutex);
	ctx->count_dyn_bfregs[bfreg_dyn_index]--;
	pthread_mutex_unlock(&ctx->dyn_bfregs_mutex);
}

static int mlx5_get_bfreg_index(struct mlx5_context *ctx)
{
	int i;

	pthread_mutex_lock(&ctx->dyn_bfregs_mutex);
	for (i = 0; i < ctx->num_dyn_bfregs; i++) {
		if (!ctx->count_dyn_bfregs[i]) {
			ctx->count_dyn_bfregs[i]++;
			pthread_mutex_unlock(&ctx->dyn_bfregs_mutex);
			return i;
		}
	}

	pthread_mutex_unlock(&ctx->dyn_bfregs_mutex);

	return -1;
}

/* Returns a dedicated BF to be used by a thread domain */
static struct mlx5_bf *mlx5_attach_dedicated_bf(struct ibv_context *context)
{
	struct mlx5_uar_info uar;
	struct mlx5_context *ctx = to_mctx(context);
	struct mlx5_device *dev = to_mdev(context->device);
	int bfreg_dyn_index;
	uint32_t bfreg_total_index;
	uint32_t uar_page_index;
	int index_in_uar, index_uar_in_page;
	int mmap_bf_index;
	int num_bfregs_per_page;

	bfreg_dyn_index = mlx5_get_bfreg_index(ctx);
	if (bfreg_dyn_index < 0) {
		errno = ENOENT;
		return NULL;
	}

	bfreg_total_index = ctx->start_dyn_bfregs_index + bfreg_dyn_index;
	/* Check whether this bfreg index was already mapped and ready to be used */
	if (ctx->bfs[bfreg_total_index].reg)
		return &(ctx->bfs[bfreg_total_index]);

	num_bfregs_per_page = ctx->num_uars_per_page * MLX5_NUM_NON_FP_BFREGS_PER_UAR;
	uar_page_index = bfreg_dyn_index / num_bfregs_per_page;

	/* The first bf index of each page will hold the mapped area address of the UAR */
	mmap_bf_index = ctx->start_dyn_bfregs_index + (uar_page_index * num_bfregs_per_page);

	pthread_mutex_lock(&ctx->dyn_bfregs_mutex);
	if (ctx->bfs[mmap_bf_index].uar) {
		/* UAR was already mapped, set its matching bfreg */
		goto set_reg;
	}

	ctx->bfs[mmap_bf_index].uar = mlx5_mmap(&uar, uar_page_index, context->cmd_fd, dev->page_size,
				  MLX5_UAR_TYPE_REGULAR_DYN);
	if (ctx->bfs[mmap_bf_index].uar == MAP_FAILED) {
		ctx->bfs[mmap_bf_index].uar = NULL;
		pthread_mutex_unlock(&ctx->dyn_bfregs_mutex);
		goto out;
	}

set_reg:
	pthread_mutex_unlock(&ctx->dyn_bfregs_mutex);
	/* Find the uar index in the system page, may be different than 1 when 4K UAR is used in 64K system page */
	index_uar_in_page = (bfreg_dyn_index % num_bfregs_per_page) /
			    MLX5_NUM_NON_FP_BFREGS_PER_UAR;
	index_in_uar = bfreg_dyn_index % MLX5_NUM_NON_FP_BFREGS_PER_UAR;
	/* set the global index so that this entry will be detected as a valid BF entry as part of post_send */
	ctx->bfs[bfreg_total_index].uuarn = bfreg_total_index;
	ctx->bfs[bfreg_total_index].reg = ctx->bfs[mmap_bf_index].uar + (index_uar_in_page * MLX5_ADAPTER_PAGE_SIZE) +
					MLX5_BF_OFFSET + (index_in_uar * ctx->bf_reg_size);
	ctx->bfs[bfreg_total_index].buf_size = ctx->bf_reg_size / 2;
	ctx->bfs[bfreg_total_index].bfreg_dyn_index = bfreg_dyn_index;
	/* This mmap command can't be repeated by secondary processes, no option to re-allocate same UAR */
	ctx->bfs[bfreg_total_index].uar_mmap_offset = 0;
	ctx->bfs[bfreg_total_index].need_lock = 0;

	return &ctx->bfs[bfreg_total_index];
out:
	mlx5_put_bfreg_index(ctx, bfreg_dyn_index);
	return NULL;
}

static void mlx5_detach_dedicated_bf(struct ibv_context *context, struct mlx5_bf *bf)
{
	struct mlx5_context *ctx = to_mctx(context);

	mlx5_put_bfreg_index(ctx, bf->bfreg_dyn_index);
}

struct ibv_td *mlx5_alloc_td(struct ibv_context *context, struct ibv_td_init_attr *init_attr)
{
	struct mlx5_td	*td;

	if (init_attr->comp_mask) {
		errno = EINVAL;
		return NULL;
	}

	td = calloc(1, sizeof(*td));
	if (!td) {
		errno = ENOMEM;
		return NULL;
	}

	td->bf = mlx5_attach_dedicated_bf(context);
	if (!td->bf) {
		free(td);
		return NULL;
	}

	td->ibv_td.context = context;
	atomic_init(&td->refcount, 1);

	return &td->ibv_td;
}

int mlx5_dealloc_td(struct ibv_td *ib_td)
{
	struct mlx5_td	*td;

	td = to_mtd(ib_td);
	if (atomic_load(&td->refcount) > 1)
		return EBUSY;

	mlx5_detach_dedicated_bf(ib_td->context, td->bf);
	free(td);

	return 0;
}

struct ibv_pd *
mlx5_alloc_parent_domain(struct ibv_context *context,
			 struct ibv_parent_domain_init_attr *attr)
{
	struct mlx5_parent_domain *mparent_domain;

	if (ibv_check_alloc_parent_domain(attr))
		return NULL;

	if (attr->comp_mask) {
		errno = EINVAL;
		return NULL;
	}

	mparent_domain = calloc(1, sizeof(*mparent_domain));
	if (!mparent_domain) {
		errno = ENOMEM;
		return NULL;
	}

	if (attr->td) {
		mparent_domain->mtd = to_mtd(attr->td);
		atomic_fetch_add(&mparent_domain->mtd->refcount, 1);
	}

	mparent_domain->mpd.mprotection_domain = to_mpd(attr->pd);
	atomic_fetch_add(&mparent_domain->mpd.mprotection_domain->refcount, 1);
	atomic_init(&mparent_domain->mpd.refcount, 1);

	ibv_initialize_parent_domain(
	    &mparent_domain->mpd.ibv_pd,
	    &mparent_domain->mpd.mprotection_domain->ibv_pd);

	return &mparent_domain->mpd.ibv_pd;
}

static int mlx5_dealloc_parent_domain(struct mlx5_parent_domain *mparent_domain)
{
	if (atomic_load(&mparent_domain->mpd.refcount) > 1)
		return EBUSY;

	atomic_fetch_sub(&mparent_domain->mpd.mprotection_domain->refcount, 1);

	if (mparent_domain->mtd)
		atomic_fetch_sub(&mparent_domain->mtd->refcount, 1);

	free(mparent_domain);
	return 0;
}

int mlx5_free_pd(struct ibv_pd *pd)
{
	int ret;
	struct mlx5_parent_domain *mparent_domain = to_mparent_domain(pd);
	struct mlx5_pd *mpd = to_mpd(pd);

	if (mparent_domain)
		return mlx5_dealloc_parent_domain(mparent_domain);

	if (atomic_load(&mpd->refcount) > 1)
		return EBUSY;

	ret = ibv_cmd_dealloc_pd(pd);
	if (ret)
		return ret;

	free(mpd);
	return 0;
}

struct ibv_mr *mlx5_reg_mr(struct ibv_pd *pd, void *addr, size_t length,
			   int acc)
{
	struct mlx5_mr *mr;
	struct ibv_reg_mr cmd;
	int ret;
	enum ibv_access_flags access = (enum ibv_access_flags)acc;
	struct ib_uverbs_reg_mr_resp resp;

	mr = calloc(1, sizeof(*mr));
	if (!mr)
		return NULL;

	ret = ibv_cmd_reg_mr(pd, addr, length, (uintptr_t)addr, access,
			     &(mr->ibv_mr), &cmd, sizeof(cmd), &resp,
			     sizeof resp);
	if (ret) {
		mlx5_free_buf(&(mr->buf));
		free(mr);
		return NULL;
	}
	mr->alloc_flags = acc;

	return &mr->ibv_mr;
}

int mlx5_rereg_mr(struct ibv_mr *ibmr, int flags, struct ibv_pd *pd, void *addr,
		  size_t length, int access)
{
	struct ibv_rereg_mr cmd;
	struct ib_uverbs_rereg_mr_resp resp;

	if (flags & IBV_REREG_MR_KEEP_VALID)
		return ENOTSUP;

	return ibv_cmd_rereg_mr(ibmr, flags, addr, length, (uintptr_t)addr,
				access, pd, &cmd, sizeof(cmd), &resp,
				sizeof(resp));
}

int mlx5_dereg_mr(struct ibv_mr *ibmr)
{
	int ret;
	struct mlx5_mr *mr = to_mmr(ibmr);

	ret = ibv_cmd_dereg_mr(ibmr);
	if (ret)
		return ret;

	free(mr);
	return 0;
}

struct ibv_mw *mlx5_alloc_mw(struct ibv_pd *pd, enum ibv_mw_type type)
{
	struct ibv_mw *mw;
	struct ibv_alloc_mw cmd;
	struct ib_uverbs_alloc_mw_resp resp;
	int ret;

	mw = malloc(sizeof(*mw));
	if (!mw)
		return NULL;

	memset(mw, 0, sizeof(*mw));

	ret = ibv_cmd_alloc_mw(pd, type, mw, &cmd, sizeof(cmd), &resp,
			       sizeof(resp));
	if (ret) {
		free(mw);
		return NULL;
	}

	return mw;
}

int mlx5_dealloc_mw(struct ibv_mw *mw)
{
	int ret;
	struct ibv_dealloc_mw cmd;

	ret = ibv_cmd_dealloc_mw(mw, &cmd, sizeof(cmd));
	if (ret)
		return ret;

	free(mw);
	return 0;
}

int mlx5_round_up_power_of_two(long long sz)
{
	long long ret;

	for (ret = 1; ret < sz; ret <<= 1)
		; /* nothing */

	if (ret > INT_MAX) {
		fprintf(stderr, "%s: roundup overflow\n", __func__);
		return -ENOMEM;
	}

	return (int)ret;
}

static int align_queue_size(long long req)
{
	return mlx5_round_up_power_of_two(req);
}

static int get_cqe_size(void)
{
	char *env;
	int size = 64;

	env = getenv("MLX5_CQE_SIZE");
	if (env)
		size = atoi(env);

	switch (size) {
	case 64:
	case 128:
		return size;

	default:
		return -EINVAL;
	}
}

static int use_scatter_to_cqe(void)
{
	char *env;

	env = getenv("MLX5_SCATTER_TO_CQE");
	if (env && !strcmp(env, "0"))
		return 0;

	return 1;
}

static int srq_sig_enabled(void)
{
	char *env;

	env = getenv("MLX5_SRQ_SIGNATURE");
	if (env)
		return 1;

	return 0;
}

static int qp_sig_enabled(void)
{
	char *env;

	env = getenv("MLX5_QP_SIGNATURE");
	if (env)
		return 1;

	return 0;
}

enum {
	CREATE_CQ_SUPPORTED_WC_FLAGS = IBV_WC_STANDARD_FLAGS	|
				       IBV_WC_EX_WITH_COMPLETION_TIMESTAMP |
				       IBV_WC_EX_WITH_CVLAN |
				       IBV_WC_EX_WITH_FLOW_TAG |
				       IBV_WC_EX_WITH_TM_INFO |
				       IBV_WC_EX_WITH_COMPLETION_TIMESTAMP_WALLCLOCK
};

enum {
	CREATE_CQ_SUPPORTED_COMP_MASK = IBV_CQ_INIT_ATTR_MASK_FLAGS
};

enum {
	CREATE_CQ_SUPPORTED_FLAGS = IBV_CREATE_CQ_ATTR_SINGLE_THREADED
};

static struct ibv_cq_ex *create_cq(struct ibv_context *context,
				   const struct ibv_cq_init_attr_ex *cq_attr,
				   int cq_alloc_flags,
				   struct mlx5dv_cq_init_attr *mlx5cq_attr)
{
	struct mlx5_create_cq		cmd;
	struct mlx5_create_cq_resp	resp;
	struct mlx5_cq		       *cq;
	int				cqe_sz;
	int				ret;
	int				ncqe;
	int				rc;
	struct mlx5_context *mctx = to_mctx(context);
	FILE *fp = to_mctx(context)->dbg_fp;

	if (!cq_attr->cqe) {
		mlx5_dbg(fp, MLX5_DBG_CQ, "CQE invalid\n");
		errno = EINVAL;
		return NULL;
	}

	if (cq_attr->comp_mask & ~CREATE_CQ_SUPPORTED_COMP_MASK) {
		mlx5_dbg(fp, MLX5_DBG_CQ,
			 "Unsupported comp_mask for create_cq\n");
		errno = EINVAL;
		return NULL;
	}

	if (cq_attr->comp_mask & IBV_CQ_INIT_ATTR_MASK_FLAGS &&
	    cq_attr->flags & ~CREATE_CQ_SUPPORTED_FLAGS) {
		mlx5_dbg(fp, MLX5_DBG_CQ,
			 "Unsupported creation flags requested for create_cq\n");
		errno = EINVAL;
		return NULL;
	}

	if (cq_attr->wc_flags & ~CREATE_CQ_SUPPORTED_WC_FLAGS) {
		mlx5_dbg(fp, MLX5_DBG_CQ, "\n");
		errno = ENOTSUP;
		return NULL;
	}

	cq =  calloc(1, sizeof *cq);
	if (!cq) {
		mlx5_dbg(fp, MLX5_DBG_CQ, "\n");
		return NULL;
	}

	if (cq_alloc_flags & MLX5_CQ_FLAGS_EXTENDED) {
		rc = mlx5_cq_fill_pfns(cq, cq_attr, mctx);
		if (rc) {
			errno = rc;
			goto err;
		}
	}

	memset(&cmd, 0, sizeof cmd);
	cq->cons_index = 0;

	if (mlx5_spinlock_init(&cq->lock))
		goto err;

	ncqe = align_queue_size(cq_attr->cqe + 1);
	if ((ncqe > (1 << 24)) || (ncqe < (cq_attr->cqe + 1))) {
		mlx5_dbg(fp, MLX5_DBG_CQ, "ncqe %d\n", ncqe);
		errno = EINVAL;
		goto err_spl;
	}

	cqe_sz = get_cqe_size();
	if (cqe_sz < 0) {
		mlx5_dbg(fp, MLX5_DBG_CQ, "\n");
		errno = -cqe_sz;
		goto err_spl;
	}

	if (mlx5_alloc_cq_buf(to_mctx(context), cq, &cq->buf_a, ncqe, cqe_sz)) {
		mlx5_dbg(fp, MLX5_DBG_CQ, "\n");
		goto err_spl;
	}

	cq->dbrec  = mlx5_alloc_dbrec(to_mctx(context));
	if (!cq->dbrec) {
		mlx5_dbg(fp, MLX5_DBG_CQ, "\n");
		goto err_buf;
	}

	cq->dbrec[MLX5_CQ_SET_CI]	= 0;
	cq->dbrec[MLX5_CQ_ARM_DB]	= 0;
	cq->arm_sn			= 0;
	cq->cqe_sz			= cqe_sz;
	cq->flags			= cq_alloc_flags;

	if (cq_attr->comp_mask & IBV_CQ_INIT_ATTR_MASK_FLAGS &&
	    cq_attr->flags & IBV_CREATE_CQ_ATTR_SINGLE_THREADED)
		cq->flags |= MLX5_CQ_FLAGS_SINGLE_THREADED;
	cmd.buf_addr = (uintptr_t) cq->buf_a.buf;
	cmd.db_addr  = (uintptr_t) cq->dbrec;
	cmd.cqe_size = cqe_sz;

	if (mlx5cq_attr) {
		if (!check_comp_mask(mlx5cq_attr->comp_mask,
				     MLX5DV_CQ_INIT_ATTR_MASK_RESERVED - 1)) {
			mlx5_dbg(fp, MLX5_DBG_CQ,
				   "Unsupported vendor comp_mask for create_cq\n");
			errno = EINVAL;
			goto err_db;
		}

		if (mlx5cq_attr->comp_mask & MLX5DV_CQ_INIT_ATTR_MASK_COMPRESSED_CQE) {
			if (mctx->cqe_comp_caps.max_num &&
			    (mlx5cq_attr->cqe_comp_res_format &
			     mctx->cqe_comp_caps.supported_format)) {
				cmd.cqe_comp_en = 1;
				cmd.cqe_comp_res_format = mlx5cq_attr->cqe_comp_res_format;
			} else {
				mlx5_dbg(fp, MLX5_DBG_CQ, "CQE Compression is not supported\n");
				errno = EINVAL;
				goto err_db;
			}
		}

		if (mlx5cq_attr->comp_mask & MLX5DV_CQ_INIT_ATTR_MASK_FLAGS) {
			if (mlx5cq_attr->flags & ~(MLX5DV_CQ_INIT_ATTR_FLAGS_RESERVED - 1)) {
				mlx5_dbg(fp, MLX5_DBG_CQ,
					 "Unsupported vendor flags for create_cq\n");
				errno = EINVAL;
				goto err_db;
			}

			if (mlx5cq_attr->flags & MLX5DV_CQ_INIT_ATTR_FLAGS_CQE_PAD) {
				if (!(mctx->vendor_cap_flags &
				      MLX5_VENDOR_CAP_FLAGS_CQE_128B_PAD) ||
				    (cqe_sz != 128)) {
					mlx5_dbg(fp, MLX5_DBG_CQ,
						 "%dB CQE paddind is not supported\n",
						 cqe_sz);
					errno = EINVAL;
					goto err_db;
				}

				cmd.flags |= MLX5_CREATE_CQ_FLAGS_CQE_128B_PAD;
			}
		}
	}

	ret = ibv_cmd_create_cq(context, ncqe - 1, cq_attr->channel,
				cq_attr->comp_vector,
				ibv_cq_ex_to_cq(&cq->ibv_cq), &cmd.ibv_cmd,
				sizeof(cmd), &resp.ibv_resp, sizeof(resp));
	if (ret) {
		mlx5_dbg(fp, MLX5_DBG_CQ, "ret %d\n", ret);
		goto err_db;
	}

	cq->active_buf = &cq->buf_a;
	cq->resize_buf = NULL;
	cq->cqn = resp.cqn;
	cq->stall_enable = to_mctx(context)->stall_enable;
	cq->stall_adaptive_enable = to_mctx(context)->stall_adaptive_enable;
	cq->stall_cycles = to_mctx(context)->stall_cycles;

	return &cq->ibv_cq;

err_db:
	mlx5_free_db(to_mctx(context), cq->dbrec);

err_buf:
	mlx5_free_cq_buf(to_mctx(context), &cq->buf_a);

err_spl:
	mlx5_spinlock_destroy(&cq->lock);

err:
	free(cq);

	return NULL;
}

struct ibv_cq *mlx5_create_cq(struct ibv_context *context, int cqe,
			      struct ibv_comp_channel *channel,
			      int comp_vector)
{
	struct ibv_cq_ex *cq;
	struct ibv_cq_init_attr_ex cq_attr = {.cqe = cqe, .channel = channel,
						.comp_vector = comp_vector,
						.wc_flags = IBV_WC_STANDARD_FLAGS};

	if (cqe <= 0) {
		errno = EINVAL;
		return NULL;
	}

	cq = create_cq(context, &cq_attr, 0, NULL);
	return cq ? ibv_cq_ex_to_cq(cq) : NULL;
}

struct ibv_cq_ex *mlx5_create_cq_ex(struct ibv_context *context,
				    struct ibv_cq_init_attr_ex *cq_attr)
{
	return create_cq(context, cq_attr, MLX5_CQ_FLAGS_EXTENDED, NULL);
}

struct ibv_cq_ex *mlx5dv_create_cq(struct ibv_context *context,
				      struct ibv_cq_init_attr_ex *cq_attr,
				      struct mlx5dv_cq_init_attr *mlx5_cq_attr)
{
	struct ibv_cq_ex *cq;

	cq = create_cq(context, cq_attr, MLX5_CQ_FLAGS_EXTENDED, mlx5_cq_attr);
	if (!cq)
		return NULL;

	verbs_init_cq(ibv_cq_ex_to_cq(cq), context,
		      cq_attr->channel, cq_attr->cq_context);
	return cq;
}

int mlx5_resize_cq(struct ibv_cq *ibcq, int cqe)
{
	struct mlx5_cq *cq = to_mcq(ibcq);
	struct mlx5_resize_cq_resp resp;
	struct mlx5_resize_cq cmd;
	struct mlx5_context *mctx = to_mctx(ibcq->context);
	int err;

	if (cqe < 0) {
		errno = EINVAL;
		return errno;
	}

	memset(&cmd, 0, sizeof(cmd));
	memset(&resp, 0, sizeof(resp));

	if (((long long)cqe * 64) > INT_MAX)
		return EINVAL;

	mlx5_spin_lock(&cq->lock);
	cq->active_cqes = cq->ibv_cq.cqe;
	if (cq->active_buf == &cq->buf_a)
		cq->resize_buf = &cq->buf_b;
	else
		cq->resize_buf = &cq->buf_a;

	cqe = align_queue_size(cqe + 1);
	if (cqe == ibcq->cqe + 1) {
		cq->resize_buf = NULL;
		err = 0;
		goto out;
	}

	/* currently we don't change cqe size */
	cq->resize_cqe_sz = cq->cqe_sz;
	cq->resize_cqes = cqe;
	err = mlx5_alloc_cq_buf(mctx, cq, cq->resize_buf, cq->resize_cqes, cq->resize_cqe_sz);
	if (err) {
		cq->resize_buf = NULL;
		errno = ENOMEM;
		goto out;
	}

	cmd.buf_addr = (uintptr_t)cq->resize_buf->buf;
	cmd.cqe_size = cq->resize_cqe_sz;

	err = ibv_cmd_resize_cq(ibcq, cqe - 1, &cmd.ibv_cmd, sizeof(cmd),
				&resp.ibv_resp, sizeof(resp));
	if (err)
		goto out_buf;

	mlx5_cq_resize_copy_cqes(cq);
	mlx5_free_cq_buf(mctx, cq->active_buf);
	cq->active_buf = cq->resize_buf;
	cq->ibv_cq.cqe = cqe - 1;
	mlx5_spin_unlock(&cq->lock);
	cq->resize_buf = NULL;
	return 0;

out_buf:
	mlx5_free_cq_buf(mctx, cq->resize_buf);
	cq->resize_buf = NULL;

out:
	mlx5_spin_unlock(&cq->lock);
	return err;
}

int mlx5_destroy_cq(struct ibv_cq *cq)
{
	int ret;

	ret = ibv_cmd_destroy_cq(cq);
	if (ret)
		return ret;

	mlx5_free_db(to_mctx(cq->context), to_mcq(cq)->dbrec);
	mlx5_free_cq_buf(to_mctx(cq->context), to_mcq(cq)->active_buf);
	free(to_mcq(cq));

	return 0;
}

struct ibv_srq *mlx5_create_srq(struct ibv_pd *pd,
				struct ibv_srq_init_attr *attr)
{
	struct mlx5_create_srq      cmd;
	struct mlx5_create_srq_resp resp;
	struct mlx5_srq		   *srq;
	int			    ret;
	struct mlx5_context	   *ctx;
	int			    max_sge;
	struct ibv_srq		   *ibsrq;

	ctx = to_mctx(pd->context);
	srq = calloc(1, sizeof *srq);
	if (!srq) {
		fprintf(stderr, "%s-%d:\n", __func__, __LINE__);
		return NULL;
	}
	ibsrq = &srq->vsrq.srq;

	memset(&cmd, 0, sizeof cmd);
	if (mlx5_spinlock_init(&srq->lock)) {
		fprintf(stderr, "%s-%d:\n", __func__, __LINE__);
		goto err;
	}

	if (attr->attr.max_wr > ctx->max_srq_recv_wr) {
		fprintf(stderr, "%s-%d:max_wr %d, max_srq_recv_wr %d\n", __func__, __LINE__,
			attr->attr.max_wr, ctx->max_srq_recv_wr);
		errno = EINVAL;
		goto err;
	}

	/*
	 * this calculation does not consider required control segments. The
	 * final calculation is done again later. This is done so to avoid
	 * overflows of variables
	 */
	max_sge = ctx->max_rq_desc_sz / sizeof(struct mlx5_wqe_data_seg);
	if (attr->attr.max_sge > max_sge) {
		fprintf(stderr, "%s-%d:max_wr %d, max_srq_recv_wr %d\n", __func__, __LINE__,
			attr->attr.max_wr, ctx->max_srq_recv_wr);
		errno = EINVAL;
		goto err;
	}

	srq->max     = align_queue_size(attr->attr.max_wr + 1);
	srq->max_gs  = attr->attr.max_sge;
	srq->counter = 0;

	if (mlx5_alloc_srq_buf(pd->context, srq)) {
		fprintf(stderr, "%s-%d:\n", __func__, __LINE__);
		goto err;
	}

	srq->db = mlx5_alloc_dbrec(to_mctx(pd->context));
	if (!srq->db) {
		fprintf(stderr, "%s-%d:\n", __func__, __LINE__);
		goto err_free;
	}

	*srq->db = 0;

	cmd.buf_addr = (uintptr_t) srq->buf.buf;
	cmd.db_addr  = (uintptr_t) srq->db;
	srq->wq_sig = srq_sig_enabled();
	if (srq->wq_sig)
		cmd.flags = MLX5_SRQ_FLAG_SIGNATURE;

	attr->attr.max_sge = srq->max_gs;
	pthread_mutex_lock(&ctx->srq_table_mutex);
	ret = ibv_cmd_create_srq(pd, ibsrq, attr, &cmd.ibv_cmd, sizeof(cmd),
				 &resp.ibv_resp, sizeof(resp));
	if (ret)
		goto err_db;

	ret = mlx5_store_srq(ctx, resp.srqn, srq);
	if (ret)
		goto err_destroy;

	pthread_mutex_unlock(&ctx->srq_table_mutex);

	srq->srqn = resp.srqn;
	srq->rsc.rsn = resp.srqn;
	srq->rsc.type = MLX5_RSC_TYPE_SRQ;

	return ibsrq;

err_destroy:
	ibv_cmd_destroy_srq(ibsrq);

err_db:
	pthread_mutex_unlock(&ctx->srq_table_mutex);
	mlx5_free_db(to_mctx(pd->context), srq->db);

err_free:
	free(srq->wrid);
	mlx5_free_buf(&srq->buf);

err:
	free(srq);

	return NULL;
}

int mlx5_modify_srq(struct ibv_srq *srq,
		    struct ibv_srq_attr *attr,
		    int attr_mask)
{
	struct ibv_modify_srq cmd;

	return ibv_cmd_modify_srq(srq, attr, attr_mask, &cmd, sizeof cmd);
}

int mlx5_query_srq(struct ibv_srq *srq,
		    struct ibv_srq_attr *attr)
{
	struct ibv_query_srq cmd;

	return ibv_cmd_query_srq(srq, attr, &cmd, sizeof cmd);
}

int mlx5_destroy_srq(struct ibv_srq *srq)
{
	int ret;
	struct mlx5_srq *msrq = to_msrq(srq);
	struct mlx5_context *ctx = to_mctx(srq->context);

	if (msrq->cmd_qp) {
		ret = mlx5_destroy_qp(msrq->cmd_qp);
		if (ret)
			return ret;
		msrq->cmd_qp = NULL;
	}

	ret = ibv_cmd_destroy_srq(srq);
	if (ret)
		return ret;

	if (ctx->cqe_version && msrq->rsc.type == MLX5_RSC_TYPE_XSRQ)
		mlx5_clear_uidx(ctx, msrq->rsc.rsn);
	else
		mlx5_clear_srq(ctx, msrq->srqn);

	mlx5_free_db(ctx, msrq->db);
	mlx5_free_buf(&msrq->buf);
	free(msrq->tm_list);
	free(msrq->wrid);
	free(msrq->op);
	free(msrq);

	return 0;
}

static int sq_overhead(struct mlx5_qp *qp, enum ibv_qp_type qp_type)
{
	size_t size = 0;
	size_t mw_bind_size =
	    sizeof(struct mlx5_wqe_umr_ctrl_seg) +
	    sizeof(struct mlx5_wqe_mkey_context_seg) +
	    max_t(size_t, sizeof(struct mlx5_wqe_umr_klm_seg), 64);

	switch (qp_type) {
	case IBV_QPT_DRIVER:
		if (qp->dc_type != MLX5DV_DCTYPE_DCI)
			return -EINVAL;
		size += sizeof(struct mlx5_wqe_datagram_seg);
		SWITCH_FALLTHROUGH;

	case IBV_QPT_RC:
		size += sizeof(struct mlx5_wqe_ctrl_seg) +
			max(sizeof(struct mlx5_wqe_atomic_seg) +
			    sizeof(struct mlx5_wqe_raddr_seg),
			    mw_bind_size);
		break;

	case IBV_QPT_UC:
		size = sizeof(struct mlx5_wqe_ctrl_seg) +
			max(sizeof(struct mlx5_wqe_raddr_seg),
			    mw_bind_size);
		break;

	case IBV_QPT_UD:
		size = sizeof(struct mlx5_wqe_ctrl_seg) +
			sizeof(struct mlx5_wqe_datagram_seg);

		if (qp->flags & MLX5_QP_FLAGS_USE_UNDERLAY)
			size += (sizeof(struct mlx5_wqe_eth_seg) + sizeof(struct mlx5_wqe_eth_pad));

		break;

	case IBV_QPT_XRC_SEND:
		size = sizeof(struct mlx5_wqe_ctrl_seg) + mw_bind_size;
		SWITCH_FALLTHROUGH;

	case IBV_QPT_XRC_RECV:
		size = max(size, sizeof(struct mlx5_wqe_ctrl_seg) +
			   sizeof(struct mlx5_wqe_xrc_seg) +
			   sizeof(struct mlx5_wqe_raddr_seg));
		break;

	case IBV_QPT_RAW_PACKET:
		size = sizeof(struct mlx5_wqe_ctrl_seg) +
			sizeof(struct mlx5_wqe_eth_seg);
		break;

	default:
		return -EINVAL;
	}

	return size;
}

static int mlx5_calc_send_wqe(struct mlx5_context *ctx,
			      struct ibv_qp_init_attr_ex *attr,
			      struct mlx5_qp *qp)
{
	int size;
	int inl_size = 0;
	int max_gather;
	int tot_size;

	size = sq_overhead(qp, attr->qp_type);
	if (size < 0)
		return size;

	if (attr->cap.max_inline_data) {
		inl_size = size + align(sizeof(struct mlx5_wqe_inl_data_seg) +
			attr->cap.max_inline_data, 16);
	}

	if (attr->comp_mask & IBV_QP_INIT_ATTR_MAX_TSO_HEADER) {
		size += align(attr->max_tso_header, 16);
		qp->max_tso_header = attr->max_tso_header;
	}

	max_gather = (ctx->max_sq_desc_sz - size) /
		sizeof(struct mlx5_wqe_data_seg);
	if (attr->cap.max_send_sge > max_gather)
		return -EINVAL;

	size += attr->cap.max_send_sge * sizeof(struct mlx5_wqe_data_seg);
	tot_size = max_int(size, inl_size);

	if (tot_size > ctx->max_sq_desc_sz)
		return -EINVAL;

	return align(tot_size, MLX5_SEND_WQE_BB);
}

static int mlx5_calc_rcv_wqe(struct mlx5_context *ctx,
			     struct ibv_qp_init_attr_ex *attr,
			     struct mlx5_qp *qp)
{
	uint32_t size;
	int num_scatter;

	if (attr->srq)
		return 0;

	num_scatter = max_t(uint32_t, attr->cap.max_recv_sge, 1);
	size = sizeof(struct mlx5_wqe_data_seg) * num_scatter;
	if (qp->wq_sig)
		size += sizeof(struct mlx5_rwqe_sig);

	if (size > ctx->max_rq_desc_sz)
		return -EINVAL;

	size = mlx5_round_up_power_of_two(size);

	return size;
}

static int mlx5_calc_sq_size(struct mlx5_context *ctx,
			     struct ibv_qp_init_attr_ex *attr,
			     struct mlx5_qp *qp)
{
	int wqe_size;
	int wq_size;
	FILE *fp = ctx->dbg_fp;

	if (!attr->cap.max_send_wr)
		return 0;

	wqe_size = mlx5_calc_send_wqe(ctx, attr, qp);
	if (wqe_size < 0) {
		mlx5_dbg(fp, MLX5_DBG_QP, "\n");
		return wqe_size;
	}

	if (wqe_size > ctx->max_sq_desc_sz) {
		mlx5_dbg(fp, MLX5_DBG_QP, "\n");
		return -EINVAL;
	}

	qp->max_inline_data = wqe_size - sq_overhead(qp, attr->qp_type) -
		sizeof(struct mlx5_wqe_inl_data_seg);
	attr->cap.max_inline_data = qp->max_inline_data;

	/*
	 * to avoid overflow, we limit max_send_wr so
	 * that the multiplication will fit in int
	 */
	if (attr->cap.max_send_wr > 0x7fffffff / ctx->max_sq_desc_sz) {
		mlx5_dbg(fp, MLX5_DBG_QP, "\n");
		return -EINVAL;
	}

	wq_size = mlx5_round_up_power_of_two(attr->cap.max_send_wr * wqe_size);
	qp->sq.wqe_cnt = wq_size / MLX5_SEND_WQE_BB;
	if (qp->sq.wqe_cnt > ctx->max_send_wqebb) {
		mlx5_dbg(fp, MLX5_DBG_QP, "\n");
		return -EINVAL;
	}

	qp->sq.wqe_shift = mlx5_ilog2(MLX5_SEND_WQE_BB);
	qp->sq.max_gs = attr->cap.max_send_sge;
	qp->sq.max_post = wq_size / wqe_size;

	return wq_size;
}

enum {
	DV_CREATE_WQ_SUPPORTED_COMP_MASK = MLX5DV_WQ_INIT_ATTR_MASK_STRIDING_RQ
};

static int mlx5_calc_rwq_size(struct mlx5_context *ctx,
			      struct mlx5_rwq *rwq,
			      struct ibv_wq_init_attr *attr,
			      struct mlx5dv_wq_init_attr *mlx5wq_attr)
{
	size_t wqe_size;
	int wq_size;
	uint32_t num_scatter;
	int is_mprq = 0;
	int scat_spc;

	if (!attr->max_wr)
		return -EINVAL;
	if (mlx5wq_attr) {
		if (!check_comp_mask(mlx5wq_attr->comp_mask,
				     DV_CREATE_WQ_SUPPORTED_COMP_MASK))
			return -EINVAL;

		is_mprq = !!(mlx5wq_attr->comp_mask &
			     MLX5DV_WQ_INIT_ATTR_MASK_STRIDING_RQ);
	}

	/* TBD: check caps for RQ */
	num_scatter = max_t(uint32_t, attr->max_sge, 1);
	wqe_size = sizeof(struct mlx5_wqe_data_seg) * num_scatter +
		sizeof(struct mlx5_wqe_srq_next_seg) * is_mprq;

	if (rwq->wq_sig)
		wqe_size += sizeof(struct mlx5_rwqe_sig);

	if (wqe_size <= 0 || wqe_size > ctx->max_rq_desc_sz)
		return -EINVAL;

	wqe_size = mlx5_round_up_power_of_two(wqe_size);
	wq_size = mlx5_round_up_power_of_two(attr->max_wr) * wqe_size;
	wq_size = max(wq_size, MLX5_SEND_WQE_BB);
	rwq->rq.wqe_cnt = wq_size / wqe_size;
	rwq->rq.wqe_shift = mlx5_ilog2(wqe_size);
	rwq->rq.max_post = 1 << mlx5_ilog2(wq_size / wqe_size);
	scat_spc = wqe_size -
		((rwq->wq_sig) ? sizeof(struct mlx5_rwqe_sig) : 0) -
		is_mprq * sizeof(struct mlx5_wqe_srq_next_seg);
	rwq->rq.max_gs = scat_spc / sizeof(struct mlx5_wqe_data_seg);
	return wq_size;
}

static int mlx5_calc_rq_size(struct mlx5_context *ctx,
			     struct ibv_qp_init_attr_ex *attr,
			     struct mlx5_qp *qp)
{
	int wqe_size;
	int wq_size;
	int scat_spc;
	FILE *fp = ctx->dbg_fp;

	if (!attr->cap.max_recv_wr)
		return 0;

	if (attr->cap.max_recv_wr > ctx->max_recv_wr) {
		mlx5_dbg(fp, MLX5_DBG_QP, "\n");
		return -EINVAL;
	}

	wqe_size = mlx5_calc_rcv_wqe(ctx, attr, qp);
	if (wqe_size < 0 || wqe_size > ctx->max_rq_desc_sz) {
		mlx5_dbg(fp, MLX5_DBG_QP, "\n");
		return -EINVAL;
	}

	wq_size = mlx5_round_up_power_of_two(attr->cap.max_recv_wr) * wqe_size;
	if (wqe_size) {
		wq_size = max(wq_size, MLX5_SEND_WQE_BB);
		qp->rq.wqe_cnt = wq_size / wqe_size;
		qp->rq.wqe_shift = mlx5_ilog2(wqe_size);
		qp->rq.max_post = 1 << mlx5_ilog2(wq_size / wqe_size);
		scat_spc = wqe_size -
			(qp->wq_sig ? sizeof(struct mlx5_rwqe_sig) : 0);
		qp->rq.max_gs = scat_spc / sizeof(struct mlx5_wqe_data_seg);
	} else {
		qp->rq.wqe_cnt = 0;
		qp->rq.wqe_shift = 0;
		qp->rq.max_post = 0;
		qp->rq.max_gs = 0;
	}
	return wq_size;
}

static int mlx5_calc_wq_size(struct mlx5_context *ctx,
			     struct ibv_qp_init_attr_ex *attr,
			     struct mlx5_qp *qp)
{
	int ret;
	int result;

	ret = mlx5_calc_sq_size(ctx, attr, qp);
	if (ret < 0)
		return ret;

	result = ret;
	ret = mlx5_calc_rq_size(ctx, attr, qp);
	if (ret < 0)
		return ret;

	result += ret;

	qp->sq.offset = ret;
	qp->rq.offset = 0;

	return result;
}

static void map_uuar(struct ibv_context *context, struct mlx5_qp *qp,
		     int uuar_index, struct mlx5_bf *dyn_bf)
{
	struct mlx5_context *ctx = to_mctx(context);

	if (!dyn_bf)
		qp->bf = &ctx->bfs[uuar_index];
	else
		qp->bf = dyn_bf;
}

static const char *qptype2key(enum ibv_qp_type type)
{
	switch (type) {
	case IBV_QPT_RC: return "HUGE_RC";
	case IBV_QPT_UC: return "HUGE_UC";
	case IBV_QPT_UD: return "HUGE_UD";
	case IBV_QPT_RAW_PACKET: return "HUGE_RAW_ETH";
	default: return "HUGE_NA";
	}
}

static int mlx5_alloc_qp_buf(struct ibv_context *context,
			     struct ibv_qp_init_attr_ex *attr,
			     struct mlx5_qp *qp,
			     int size)
{
	int err;
	enum mlx5_alloc_type alloc_type;
	enum mlx5_alloc_type default_alloc_type = MLX5_ALLOC_TYPE_ANON;
	const char *qp_huge_key;

	if (qp->sq.wqe_cnt) {
		qp->sq.wrid = malloc(qp->sq.wqe_cnt * sizeof(*qp->sq.wrid));
		if (!qp->sq.wrid) {
			errno = ENOMEM;
			err = -1;
			return err;
		}

		qp->sq.wr_data = malloc(qp->sq.wqe_cnt * sizeof(*qp->sq.wr_data));
		if (!qp->sq.wr_data) {
			errno = ENOMEM;
			err = -1;
			goto ex_wrid;
		}
	}

	qp->sq.wqe_head = malloc(qp->sq.wqe_cnt * sizeof(*qp->sq.wqe_head));
	if (!qp->sq.wqe_head) {
		errno = ENOMEM;
		err = -1;
			goto ex_wrid;
	}

	if (qp->rq.wqe_cnt) {
		qp->rq.wrid = malloc(qp->rq.wqe_cnt * sizeof(uint64_t));
		if (!qp->rq.wrid) {
			errno = ENOMEM;
			err = -1;
			goto ex_wrid;
		}
	}

	/* compatibility support */
	qp_huge_key  = qptype2key(qp->ibv_qp->qp_type);
	if (mlx5_use_huge(qp_huge_key))
		default_alloc_type = MLX5_ALLOC_TYPE_HUGE;

	mlx5_get_alloc_type(to_mctx(context), MLX5_QP_PREFIX, &alloc_type,
			    default_alloc_type);

	err = mlx5_alloc_prefered_buf(to_mctx(context), &qp->buf,
				      align(qp->buf_size, to_mdev
				      (context->device)->page_size),
				      to_mdev(context->device)->page_size,
				      alloc_type,
				      MLX5_QP_PREFIX);

	if (err) {
		err = -ENOMEM;
		goto ex_wrid;
	}

	memset(qp->buf.buf, 0, qp->buf_size);

	if (attr->qp_type == IBV_QPT_RAW_PACKET ||
	    qp->flags & MLX5_QP_FLAGS_USE_UNDERLAY) {
		size_t aligned_sq_buf_size = align(qp->sq_buf_size,
						   to_mdev(context->device)->page_size);
		/* For Raw Packet QP, allocate a separate buffer for the SQ */
		err = mlx5_alloc_prefered_buf(to_mctx(context), &qp->sq_buf,
					      aligned_sq_buf_size,
					      to_mdev(context->device)->page_size,
					      alloc_type,
					      MLX5_QP_PREFIX);
		if (err) {
			err = -ENOMEM;
			goto rq_buf;
		}

		memset(qp->sq_buf.buf, 0, aligned_sq_buf_size);
	}

	return 0;
rq_buf:
	mlx5_free_actual_buf(to_mctx(context), &qp->buf);
ex_wrid:
	if (qp->rq.wrid)
		free(qp->rq.wrid);

	if (qp->sq.wqe_head)
		free(qp->sq.wqe_head);

	if (qp->sq.wr_data)
		free(qp->sq.wr_data);
	if (qp->sq.wrid)
		free(qp->sq.wrid);

	return err;
}

static void mlx5_free_qp_buf(struct mlx5_context *ctx, struct mlx5_qp *qp)
{
	mlx5_free_actual_buf(ctx, &qp->buf);

	if (qp->sq_buf.buf)
		mlx5_free_actual_buf(ctx, &qp->sq_buf);

	if (qp->rq.wrid)
		free(qp->rq.wrid);

	if (qp->sq.wqe_head)
		free(qp->sq.wqe_head);

	if (qp->sq.wrid)
		free(qp->sq.wrid);

	if (qp->sq.wr_data)
		free(qp->sq.wr_data);
}

static int mlx5_cmd_create_rss_qp(struct ibv_context *context,
				 struct ibv_qp_init_attr_ex *attr,
				 struct mlx5_qp *qp,
				 uint32_t mlx5_create_flags)
{
	struct mlx5_create_qp_ex_rss cmd_ex_rss = {};
	struct mlx5_create_qp_resp_ex resp = {};
	int ret;

	if (attr->rx_hash_conf.rx_hash_key_len > sizeof(cmd_ex_rss.rx_hash_key)) {
		errno = EINVAL;
		return errno;
	}

	cmd_ex_rss.rx_hash_fields_mask = attr->rx_hash_conf.rx_hash_fields_mask;
	cmd_ex_rss.rx_hash_function = attr->rx_hash_conf.rx_hash_function;
	cmd_ex_rss.rx_key_len = attr->rx_hash_conf.rx_hash_key_len;
	cmd_ex_rss.create_flags = mlx5_create_flags;
	memcpy(cmd_ex_rss.rx_hash_key, attr->rx_hash_conf.rx_hash_key,
			attr->rx_hash_conf.rx_hash_key_len);

	ret = ibv_cmd_create_qp_ex2(context, &qp->verbs_qp,
					    sizeof(qp->verbs_qp), attr,
					    &cmd_ex_rss.ibv_cmd, sizeof(cmd_ex_rss.ibv_cmd),
					    sizeof(cmd_ex_rss), &resp.ibv_resp,
					    sizeof(resp.ibv_resp), sizeof(resp));
	if (ret)
		return ret;

	qp->rss_qp = 1;
	return 0;
}

static int mlx5_cmd_create_qp_ex(struct ibv_context *context,
				 struct ibv_qp_init_attr_ex *attr,
				 struct mlx5_create_qp *cmd,
				 struct mlx5_qp *qp,
				 struct mlx5_create_qp_resp_ex *resp)
{
	struct mlx5_create_qp_ex cmd_ex;
	int ret;

	memset(&cmd_ex, 0, sizeof(cmd_ex));
	*ibv_create_cq_ex_to_reg(&cmd_ex.ibv_cmd) = cmd->ibv_cmd.core_payload;

	memcpy(&cmd_ex.drv_ex, &cmd->buf_addr,
	       offsetof(typeof(*cmd), sq_buf_addr) +
	       sizeof(cmd->sq_buf_addr) - sizeof(cmd->ibv_cmd));

	ret = ibv_cmd_create_qp_ex2(context, &qp->verbs_qp,
				    sizeof(qp->verbs_qp), attr,
				    &cmd_ex.ibv_cmd, sizeof(cmd_ex.ibv_cmd),
				    sizeof(cmd_ex), &resp->ibv_resp,
				    sizeof(resp->ibv_resp), sizeof(*resp));

	return ret;
}

enum {
	MLX5_CREATE_QP_SUP_COMP_MASK = (IBV_QP_INIT_ATTR_PD |
					IBV_QP_INIT_ATTR_XRCD |
					IBV_QP_INIT_ATTR_CREATE_FLAGS |
					IBV_QP_INIT_ATTR_MAX_TSO_HEADER |
					IBV_QP_INIT_ATTR_IND_TABLE |
					IBV_QP_INIT_ATTR_RX_HASH),
};

enum {
	MLX5_DV_CREATE_QP_SUP_COMP_MASK = MLX5DV_QP_INIT_ATTR_MASK_QP_CREATE_FLAGS |
					  MLX5DV_QP_INIT_ATTR_MASK_DC
};

enum {
	MLX5_CREATE_QP_EX2_COMP_MASK = (IBV_QP_INIT_ATTR_CREATE_FLAGS |
					IBV_QP_INIT_ATTR_MAX_TSO_HEADER |
					IBV_QP_INIT_ATTR_IND_TABLE |
					IBV_QP_INIT_ATTR_RX_HASH),
};

static int create_dct(struct ibv_context *context,
		      struct ibv_qp_init_attr_ex *attr,
		      struct mlx5dv_qp_init_attr *mlx5_qp_attr,
		      struct mlx5_qp		       *qp)
{
	struct mlx5_create_qp		cmd = {};
	struct mlx5_create_qp_resp	resp = {};
	int				ret;
	struct mlx5_context	       *ctx = to_mctx(context);
	int32_t				usr_idx = 0xffffff;
	FILE *fp = ctx->dbg_fp;

	if (!check_comp_mask(attr->comp_mask, IBV_QP_INIT_ATTR_PD)) {
		mlx5_dbg(fp, MLX5_DBG_QP,
			 "Unsupported comp_mask for %s\n", __func__);
		errno = EINVAL;
		return errno;
	}

	if (!check_comp_mask(mlx5_qp_attr->comp_mask, MLX5DV_QP_INIT_ATTR_MASK_DC)) {
		mlx5_dbg(fp, MLX5_DBG_QP,
			 "Unsupported vendor comp_mask for %s\n", __func__);
		errno = EINVAL;
		return errno;
	}

	cmd.flags = MLX5_QP_FLAG_TYPE_DCT;
	cmd.access_key = mlx5_qp_attr->dc_init_attr.dct_access_key;

	if (ctx->cqe_version) {
		usr_idx = mlx5_store_uidx(ctx, qp);
		if (usr_idx < 0) {
			mlx5_dbg(fp, MLX5_DBG_QP, "Couldn't find free user index\n");
			errno = ENOMEM;
			return errno;
		}
	}
	cmd.uidx = usr_idx;

	ret = ibv_cmd_create_qp_ex(context, &qp->verbs_qp, sizeof(qp->verbs_qp),
				   attr, &cmd.ibv_cmd, sizeof(cmd),
				   &resp.ibv_resp, sizeof(resp));
	if (ret) {
		mlx5_dbg(fp, MLX5_DBG_QP, "Couldn't create dct, ret %d\n", ret);
		if (ctx->cqe_version)
			mlx5_clear_uidx(ctx, cmd.uidx);
		return ret;
	}

	qp->dc_type = MLX5DV_DCTYPE_DCT;
	qp->rsc.type = MLX5_RSC_TYPE_QP;
	if (ctx->cqe_version)
		qp->rsc.rsn = usr_idx;
	return 0;
}

static struct ibv_qp *create_qp(struct ibv_context *context,
				struct ibv_qp_init_attr_ex *attr,
				struct mlx5dv_qp_init_attr *mlx5_qp_attr)
{
	struct mlx5_create_qp		cmd;
	struct mlx5_create_qp_resp	resp;
	struct mlx5_create_qp_resp_ex resp_ex;
	struct mlx5_qp		       *qp;
	int				ret;
	struct mlx5_context	       *ctx = to_mctx(context);
	struct ibv_qp		       *ibqp;
	int32_t				usr_idx = 0;
	uint32_t			uuar_index;
	uint32_t			mlx5_create_flags = 0;
	struct mlx5_bf			*bf = NULL;
	FILE *fp = ctx->dbg_fp;
	struct mlx5_parent_domain *mparent_domain;

	if (attr->comp_mask & ~MLX5_CREATE_QP_SUP_COMP_MASK)
		return NULL;

	if ((attr->comp_mask & IBV_QP_INIT_ATTR_MAX_TSO_HEADER) &&
	    (attr->qp_type != IBV_QPT_RAW_PACKET))
		return NULL;

	qp = calloc(1, sizeof(*qp));
	if (!qp) {
		mlx5_dbg(fp, MLX5_DBG_QP, "\n");
		return NULL;
	}

	ibqp = (struct ibv_qp *)&qp->verbs_qp;
	qp->ibv_qp = ibqp;

	if ((attr->comp_mask & IBV_QP_INIT_ATTR_CREATE_FLAGS) &&
		(attr->create_flags & IBV_QP_CREATE_SOURCE_QPN)) {

		if (attr->qp_type != IBV_QPT_UD) {
			errno = EINVAL;
			goto err;
		}

		qp->flags |= MLX5_QP_FLAGS_USE_UNDERLAY;
	}

	memset(&cmd, 0, sizeof(cmd));
	memset(&resp, 0, sizeof(resp));
	memset(&resp_ex, 0, sizeof(resp_ex));

	if (mlx5_qp_attr) {
		if (!check_comp_mask(mlx5_qp_attr->comp_mask,
				     MLX5_DV_CREATE_QP_SUP_COMP_MASK)) {
			mlx5_dbg(fp, MLX5_DBG_QP,
				 "Unsupported vendor comp_mask for create_qp\n");
			errno = EINVAL;
			goto err;
		}

		if ((mlx5_qp_attr->comp_mask & MLX5DV_QP_INIT_ATTR_MASK_DC) &&
		    (attr->qp_type != IBV_QPT_DRIVER)) {
			mlx5_dbg(fp, MLX5_DBG_QP, "DC QP must be of type IBV_QPT_DRIVER\n");
			errno = EINVAL;
			goto err;
		}
		if (mlx5_qp_attr->comp_mask &
		    MLX5DV_QP_INIT_ATTR_MASK_QP_CREATE_FLAGS) {
			if (mlx5_qp_attr->create_flags &
			    MLX5DV_QP_CREATE_TUNNEL_OFFLOADS) {
				mlx5_create_flags = MLX5_QP_FLAG_TUNNEL_OFFLOADS;
			} else {
				mlx5_dbg(fp, MLX5_DBG_QP,
					 "Unsupported creation flags requested for create_qp\n");
				errno = EINVAL;
				goto err;
			}
		}

		if (attr->qp_type == IBV_QPT_DRIVER) {
			if (mlx5_qp_attr->comp_mask & MLX5DV_QP_INIT_ATTR_MASK_DC) {
				if (mlx5_qp_attr->dc_init_attr.dc_type == MLX5DV_DCTYPE_DCT) {
					ret = create_dct(context, attr, mlx5_qp_attr, qp);
					if (ret)
						goto err;
					return ibqp;
				} else if (mlx5_qp_attr->dc_init_attr.dc_type == MLX5DV_DCTYPE_DCI) {
					mlx5_create_flags |= MLX5_QP_FLAG_TYPE_DCI;
					qp->dc_type = MLX5DV_DCTYPE_DCI;
				} else {
					errno = EINVAL;
					goto err;
				}
			} else {
				errno = EINVAL;
				goto err;
			}
		}

	} else {
		if (attr->qp_type == IBV_QPT_DRIVER)
			goto err;
	}

	if (attr->comp_mask & IBV_QP_INIT_ATTR_RX_HASH) {
		ret = mlx5_cmd_create_rss_qp(context, attr, qp,
					     mlx5_create_flags);
		if (ret)
			goto err;

		return ibqp;
	}

	cmd.flags = mlx5_create_flags;
	qp->wq_sig = qp_sig_enabled();
	if (qp->wq_sig)
		cmd.flags |= MLX5_QP_FLAG_SIGNATURE;

	if (use_scatter_to_cqe())
		cmd.flags |= MLX5_QP_FLAG_SCATTER_CQE;

	ret = mlx5_calc_wq_size(ctx, attr, qp);
	if (ret < 0) {
		errno = -ret;
		goto err;
	}

	if (attr->qp_type == IBV_QPT_RAW_PACKET ||
	    qp->flags & MLX5_QP_FLAGS_USE_UNDERLAY) {
		qp->buf_size = qp->sq.offset;
		qp->sq_buf_size = ret - qp->buf_size;
		qp->sq.offset = 0;
	} else {
		qp->buf_size = ret;
		qp->sq_buf_size = 0;
	}

	if (mlx5_alloc_qp_buf(context, attr, qp, ret)) {
		mlx5_dbg(fp, MLX5_DBG_QP, "\n");
		goto err;
	}

	if (attr->qp_type == IBV_QPT_RAW_PACKET ||
	    qp->flags & MLX5_QP_FLAGS_USE_UNDERLAY) {
		qp->sq_start = qp->sq_buf.buf;
		qp->sq.qend = qp->sq_buf.buf +
				(qp->sq.wqe_cnt << qp->sq.wqe_shift);
	} else {
		qp->sq_start = qp->buf.buf + qp->sq.offset;
		qp->sq.qend = qp->buf.buf + qp->sq.offset +
				(qp->sq.wqe_cnt << qp->sq.wqe_shift);
	}

	mlx5_init_qp_indices(qp);

	if (mlx5_spinlock_init(&qp->sq.lock) ||
	    mlx5_spinlock_init(&qp->rq.lock))
		goto err_free_qp_buf;

	qp->db = mlx5_alloc_dbrec(ctx);
	if (!qp->db) {
		mlx5_dbg(fp, MLX5_DBG_QP, "\n");
		goto err_free_qp_buf;
	}

	qp->db[MLX5_RCV_DBR] = 0;
	qp->db[MLX5_SND_DBR] = 0;

	cmd.buf_addr = (uintptr_t) qp->buf.buf;
	cmd.sq_buf_addr = (attr->qp_type == IBV_QPT_RAW_PACKET ||
			   qp->flags & MLX5_QP_FLAGS_USE_UNDERLAY) ?
			  (uintptr_t) qp->sq_buf.buf : 0;
	cmd.db_addr  = (uintptr_t) qp->db;
	cmd.sq_wqe_count = qp->sq.wqe_cnt;
	cmd.rq_wqe_count = qp->rq.wqe_cnt;
	cmd.rq_wqe_shift = qp->rq.wqe_shift;

	if (ctx->atomic_cap == IBV_ATOMIC_HCA)
		qp->atomics_enabled = 1;

	if (!ctx->cqe_version) {
		cmd.uidx = 0xffffff;
		pthread_mutex_lock(&ctx->qp_table_mutex);
	} else if (!is_xrc_tgt(attr->qp_type)) {
		usr_idx = mlx5_store_uidx(ctx, qp);
		if (usr_idx < 0) {
			mlx5_dbg(fp, MLX5_DBG_QP, "Couldn't find free user index\n");
			goto err_rq_db;
		}

		cmd.uidx = usr_idx;
	}

	mparent_domain = to_mparent_domain(attr->pd);
	if (mparent_domain && mparent_domain->mtd)
		bf = mparent_domain->mtd->bf;

	if (bf) {
		cmd.bfreg_index = bf->bfreg_dyn_index;
		cmd.flags |= MLX5_QP_FLAG_BFREG_INDEX;
	}

	if (attr->comp_mask & MLX5_CREATE_QP_EX2_COMP_MASK)
		ret = mlx5_cmd_create_qp_ex(context, attr, &cmd, qp, &resp_ex);
	else
		ret = ibv_cmd_create_qp_ex(context, &qp->verbs_qp, sizeof(qp->verbs_qp),
					   attr, &cmd.ibv_cmd, sizeof(cmd),
					   &resp.ibv_resp, sizeof(resp));
	if (ret) {
		mlx5_dbg(fp, MLX5_DBG_QP, "ret %d\n", ret);
		goto err_free_uidx;
	}

	uuar_index = (attr->comp_mask & MLX5_CREATE_QP_EX2_COMP_MASK) ?
			resp_ex.uuar_index : resp.uuar_index;
	if (!ctx->cqe_version) {
		if (qp->sq.wqe_cnt || qp->rq.wqe_cnt) {
			ret = mlx5_store_qp(ctx, ibqp->qp_num, qp);
			if (ret) {
				mlx5_dbg(fp, MLX5_DBG_QP, "ret %d\n", ret);
				goto err_destroy;
			}
		}

		pthread_mutex_unlock(&ctx->qp_table_mutex);
	}

	map_uuar(context, qp, uuar_index, bf);

	qp->rq.max_post = qp->rq.wqe_cnt;
	if (attr->sq_sig_all)
		qp->sq_signal_bits = MLX5_WQE_CTRL_CQ_UPDATE;
	else
		qp->sq_signal_bits = 0;

	attr->cap.max_send_wr = qp->sq.max_post;
	attr->cap.max_recv_wr = qp->rq.max_post;
	attr->cap.max_recv_sge = qp->rq.max_gs;

	qp->rsc.type = MLX5_RSC_TYPE_QP;
	qp->rsc.rsn = (ctx->cqe_version && !is_xrc_tgt(attr->qp_type)) ?
		      usr_idx : ibqp->qp_num;

	if (mparent_domain)
		atomic_fetch_add(&mparent_domain->mpd.refcount, 1);
	return ibqp;

err_destroy:
	ibv_cmd_destroy_qp(ibqp);

err_free_uidx:
	if (!ctx->cqe_version)
		pthread_mutex_unlock(&to_mctx(context)->qp_table_mutex);
	else if (!is_xrc_tgt(attr->qp_type))
		mlx5_clear_uidx(ctx, usr_idx);

err_rq_db:
	mlx5_free_db(to_mctx(context), qp->db);

err_free_qp_buf:
	mlx5_free_qp_buf(ctx, qp);

err:
	free(qp);

	return NULL;
}

struct ibv_qp *mlx5_create_qp(struct ibv_pd *pd,
			      struct ibv_qp_init_attr *attr)
{
	struct ibv_qp *qp;
	struct ibv_qp_init_attr_ex attrx;

	memset(&attrx, 0, sizeof(attrx));
	memcpy(&attrx, attr, sizeof(*attr));
	attrx.comp_mask = IBV_QP_INIT_ATTR_PD;
	attrx.pd = pd;
	qp = create_qp(pd->context, &attrx, NULL);
	if (qp)
		memcpy(attr, &attrx, sizeof(*attr));

	return qp;
}

static void mlx5_lock_cqs(struct ibv_qp *qp)
{
	struct mlx5_cq *send_cq = to_mcq(qp->send_cq);
	struct mlx5_cq *recv_cq = to_mcq(qp->recv_cq);

	if (send_cq && recv_cq) {
		if (send_cq == recv_cq) {
			mlx5_spin_lock(&send_cq->lock);
		} else if (send_cq->cqn < recv_cq->cqn) {
			mlx5_spin_lock(&send_cq->lock);
			mlx5_spin_lock(&recv_cq->lock);
		} else {
			mlx5_spin_lock(&recv_cq->lock);
			mlx5_spin_lock(&send_cq->lock);
		}
	} else if (send_cq) {
		mlx5_spin_lock(&send_cq->lock);
	} else if (recv_cq) {
		mlx5_spin_lock(&recv_cq->lock);
	}
}

static void mlx5_unlock_cqs(struct ibv_qp *qp)
{
	struct mlx5_cq *send_cq = to_mcq(qp->send_cq);
	struct mlx5_cq *recv_cq = to_mcq(qp->recv_cq);

	if (send_cq && recv_cq) {
		if (send_cq == recv_cq) {
			mlx5_spin_unlock(&send_cq->lock);
		} else if (send_cq->cqn < recv_cq->cqn) {
			mlx5_spin_unlock(&recv_cq->lock);
			mlx5_spin_unlock(&send_cq->lock);
		} else {
			mlx5_spin_unlock(&send_cq->lock);
			mlx5_spin_unlock(&recv_cq->lock);
		}
	} else if (send_cq) {
		mlx5_spin_unlock(&send_cq->lock);
	} else if (recv_cq) {
		mlx5_spin_unlock(&recv_cq->lock);
	}
}

int mlx5_destroy_qp(struct ibv_qp *ibqp)
{
	struct mlx5_qp *qp = to_mqp(ibqp);
	struct mlx5_context *ctx = to_mctx(ibqp->context);
	int ret;
	struct mlx5_parent_domain *mparent_domain = to_mparent_domain(ibqp->pd);

	if (qp->rss_qp) {
		ret = ibv_cmd_destroy_qp(ibqp);
		if (ret)
			return ret;
		goto free;
	}

	if (!ctx->cqe_version)
		pthread_mutex_lock(&ctx->qp_table_mutex);

	ret = ibv_cmd_destroy_qp(ibqp);
	if (ret) {
		if (!ctx->cqe_version)
			pthread_mutex_unlock(&ctx->qp_table_mutex);
		return ret;
	}

	mlx5_lock_cqs(ibqp);

	__mlx5_cq_clean(to_mcq(ibqp->recv_cq), qp->rsc.rsn,
			ibqp->srq ? to_msrq(ibqp->srq) : NULL);
	if (ibqp->send_cq != ibqp->recv_cq)
		__mlx5_cq_clean(to_mcq(ibqp->send_cq), qp->rsc.rsn, NULL);

	if (!ctx->cqe_version) {
		if (qp->dc_type == MLX5DV_DCTYPE_DCT) {
			/* The QP was inserted to the tracking table only after
			 * that it was modifed to RTR
			 */
			if (ibqp->state == IBV_QPS_RTR)
				mlx5_clear_qp(ctx, ibqp->qp_num);
		} else {
			if (qp->sq.wqe_cnt || qp->rq.wqe_cnt)
				mlx5_clear_qp(ctx, ibqp->qp_num);
		}
	}

	mlx5_unlock_cqs(ibqp);
	if (!ctx->cqe_version)
		pthread_mutex_unlock(&ctx->qp_table_mutex);
	else if (!is_xrc_tgt(ibqp->qp_type))
		mlx5_clear_uidx(ctx, qp->rsc.rsn);

	if (qp->dc_type != MLX5DV_DCTYPE_DCT) {
		mlx5_free_db(ctx, qp->db);
		mlx5_free_qp_buf(ctx, qp);
	}
free:
	if (mparent_domain)
		atomic_fetch_sub(&mparent_domain->mpd.refcount, 1);

	free(qp);

	return 0;
}

int mlx5_query_qp(struct ibv_qp *ibqp, struct ibv_qp_attr *attr,
		  int attr_mask, struct ibv_qp_init_attr *init_attr)
{
	struct ibv_query_qp cmd;
	struct mlx5_qp *qp = to_mqp(ibqp);
	int ret;

	if (qp->rss_qp)
		return ENOSYS;

	ret = ibv_cmd_query_qp(ibqp, attr, attr_mask, init_attr, &cmd, sizeof(cmd));
	if (ret)
		return ret;

	init_attr->cap.max_send_wr     = qp->sq.max_post;
	init_attr->cap.max_send_sge    = qp->sq.max_gs;
	init_attr->cap.max_inline_data = qp->max_inline_data;

	attr->cap = init_attr->cap;

	return 0;
}

enum {
	MLX5_MODIFY_QP_EX_ATTR_MASK = IBV_QP_RATE_LIMIT,
};

static int modify_dct(struct ibv_qp *qp, struct ibv_qp_attr *attr,
		      int attr_mask)
{
	struct ibv_modify_qp_ex cmd_ex = {};
	struct mlx5_modify_qp_resp_ex resp = {};
	struct mlx5_qp *mqp = to_mqp(qp);
	struct mlx5_context *context = to_mctx(qp->context);
	int min_resp_size;
	bool dct_create;
	int ret;

	ret = ibv_cmd_modify_qp_ex(qp, attr, attr_mask,
				   &cmd_ex,
				   sizeof(cmd_ex), sizeof(cmd_ex),
				   &resp.base,
				   sizeof(resp.base), sizeof(resp));
	if (ret)
		return ret;

	/* dct is created in hardware and gets unique qp number when QP
	 * is modified to RTR so operations that require QP number need
	 * to be delayed to this time
	 */
	dct_create =
		(attr_mask & IBV_QP_STATE) &&
		(attr->qp_state == IBV_QPS_RTR);

	if (!dct_create)
		return 0;

	min_resp_size =
		offsetof(typeof(resp), dctn) +
		sizeof(resp.dctn) -
		sizeof(resp.base);

	if (resp.response_length < min_resp_size) {
		errno = EINVAL;
		return errno;
	}

	qp->qp_num = resp.dctn;

	if (!context->cqe_version) {
		pthread_mutex_lock(&context->qp_table_mutex);
		ret = mlx5_store_qp(context, qp->qp_num, mqp);
		if (!ret)
			mqp->rsc.rsn = qp->qp_num;
		else
			errno = ENOMEM;
		pthread_mutex_unlock(&context->qp_table_mutex);
		return ret ? errno : 0;
	}
	return 0;
}

int mlx5_modify_qp(struct ibv_qp *qp, struct ibv_qp_attr *attr,
		   int attr_mask)
{
	struct ibv_modify_qp cmd = {};
	struct ibv_modify_qp_ex cmd_ex = {};
	struct ib_uverbs_ex_modify_qp_resp resp = {};
	struct mlx5_qp *mqp = to_mqp(qp);
	struct mlx5_context *context = to_mctx(qp->context);
	int ret;
	__be32 *db;

	if (mqp->dc_type == MLX5DV_DCTYPE_DCT)
		return modify_dct(qp, attr, attr_mask);

	if (mqp->rss_qp)
		return ENOSYS;

	if (mqp->flags & MLX5_QP_FLAGS_USE_UNDERLAY) {
		if (attr_mask & ~(IBV_QP_STATE | IBV_QP_CUR_STATE))
			return EINVAL;

		/* Underlay QP is UD over infiniband */
		if (context->cached_device_cap_flags & IBV_DEVICE_UD_IP_CSUM)
			mqp->qp_cap_cache |= MLX5_CSUM_SUPPORT_UNDERLAY_UD |
					     MLX5_RX_CSUM_VALID;
	}

	if (attr_mask & IBV_QP_PORT) {
		switch (qp->qp_type) {
		case IBV_QPT_RAW_PACKET:
			if (context->cached_link_layer[attr->port_num - 1] ==
			     IBV_LINK_LAYER_ETHERNET) {
				if (context->cached_device_cap_flags &
				    IBV_DEVICE_RAW_IP_CSUM)
					mqp->qp_cap_cache |=
						MLX5_CSUM_SUPPORT_RAW_OVER_ETH |
						MLX5_RX_CSUM_VALID;

				if (ibv_is_qpt_supported(
				 context->cached_tso_caps.supported_qpts,
				 IBV_QPT_RAW_PACKET))
					mqp->max_tso =
					     context->cached_tso_caps.max_tso;
			}
			break;
		default:
			break;
		}
	}

	if (attr_mask & MLX5_MODIFY_QP_EX_ATTR_MASK)
		ret = ibv_cmd_modify_qp_ex(qp, attr, attr_mask,
					   &cmd_ex,
					   sizeof(cmd_ex), sizeof(cmd_ex),
					   &resp,
					   sizeof(resp), sizeof(resp));
	else
		ret = ibv_cmd_modify_qp(qp, attr, attr_mask,
					&cmd, sizeof(cmd));

	if (!ret		       &&
	    (attr_mask & IBV_QP_STATE) &&
	    attr->qp_state == IBV_QPS_RESET) {
		if (qp->recv_cq) {
			mlx5_cq_clean(to_mcq(qp->recv_cq), mqp->rsc.rsn,
				      qp->srq ? to_msrq(qp->srq) : NULL);
		}
		if (qp->send_cq != qp->recv_cq && qp->send_cq)
			mlx5_cq_clean(to_mcq(qp->send_cq),
				      to_mqp(qp)->rsc.rsn, NULL);

		mlx5_init_qp_indices(mqp);
		db = mqp->db;
		db[MLX5_RCV_DBR] = 0;
		db[MLX5_SND_DBR] = 0;
	}

	/*
	 * When the Raw Packet QP is in INIT state, its RQ
	 * underneath is already in RDY, which means it can
	 * receive packets. According to the IB spec, a QP can't
	 * receive packets until moved to RTR state. To achieve this,
	 * for Raw Packet QPs, we update the doorbell record
	 * once the QP is moved to RTR.
	 */
	if (!ret &&
	    (attr_mask & IBV_QP_STATE) &&
	    attr->qp_state == IBV_QPS_RTR &&
	    (qp->qp_type == IBV_QPT_RAW_PACKET ||
	     mqp->flags & MLX5_QP_FLAGS_USE_UNDERLAY)) {
		mlx5_spin_lock(&mqp->rq.lock);
		mqp->db[MLX5_RCV_DBR] = htobe32(mqp->rq.head & 0xffff);
		mlx5_spin_unlock(&mqp->rq.lock);
	}

	return ret;
}

int mlx5_modify_qp_rate_limit(struct ibv_qp *qp,
			      struct ibv_qp_rate_limit_attr *attr)
{
	struct ibv_qp_attr qp_attr = {};
	struct ib_uverbs_ex_modify_qp_resp resp = {};
	struct mlx5_ib_modify_qp cmd = {};
	struct mlx5_context *mctx = to_mctx(qp->context);
	int ret;

	if (attr->comp_mask)
		return EINVAL;

	if ((attr->max_burst_sz ||
	     attr->typical_pkt_sz) &&
	    (!attr->rate_limit ||
	     !(mctx->packet_pacing_caps.cap_flags &
	       MLX5_IB_PP_SUPPORT_BURST)))
		return EINVAL;

	cmd.burst_info.max_burst_sz = attr->max_burst_sz;
	cmd.burst_info.typical_pkt_sz = attr->typical_pkt_sz;
	qp_attr.rate_limit = attr->rate_limit;

	ret = ibv_cmd_modify_qp_ex(qp, &qp_attr, IBV_QP_RATE_LIMIT,
				   &cmd.ibv_cmd,
				   sizeof(cmd.ibv_cmd), sizeof(cmd),
				   &resp,
				   sizeof(resp), sizeof(resp));

	return ret;
}

#define RROCE_UDP_SPORT_MIN 0xC000
#define RROCE_UDP_SPORT_MAX 0xFFFF
struct ibv_ah *mlx5_create_ah(struct ibv_pd *pd, struct ibv_ah_attr *attr)
{
	struct mlx5_context *ctx = to_mctx(pd->context);
	struct ibv_port_attr port_attr;
	struct mlx5_ah *ah;
	uint32_t gid_type;
	__be32 tmp;
	uint8_t grh;
	int is_eth;

	if (attr->port_num < 1 || attr->port_num > ctx->num_ports)
		return NULL;

	if (ctx->cached_link_layer[attr->port_num - 1]) {
		is_eth = ctx->cached_link_layer[attr->port_num - 1] ==
			IBV_LINK_LAYER_ETHERNET;
	} else {
		if (ibv_query_port(pd->context, attr->port_num, &port_attr))
			return NULL;

		is_eth = (port_attr.link_layer == IBV_LINK_LAYER_ETHERNET);
	}

	if (unlikely((!attr->is_global) && is_eth)) {
		errno = EINVAL;
		return NULL;
	}

	ah = calloc(1, sizeof *ah);
	if (!ah)
		return NULL;

	if (is_eth) {
		if (ibv_query_gid_type(pd->context, attr->port_num,
				       attr->grh.sgid_index, &gid_type))
			goto err;

		if (gid_type == IBV_GID_TYPE_ROCE_V2)
			ah->av.rlid = htobe16(rand() % (RROCE_UDP_SPORT_MAX + 1
						      - RROCE_UDP_SPORT_MIN)
					    + RROCE_UDP_SPORT_MIN);
		/* Since RoCE packets must contain GRH, this bit is reserved
		 * for RoCE and shouldn't be set.
		 */
		grh = 0;
	} else {
		ah->av.fl_mlid = attr->src_path_bits & 0x7f;
		ah->av.rlid = htobe16(attr->dlid);
		grh = 1;
	}
	ah->av.stat_rate_sl = (attr->static_rate << 4) | attr->sl;
	if (attr->is_global) {
		ah->av.tclass = attr->grh.traffic_class;
		ah->av.hop_limit = attr->grh.hop_limit;
		tmp = htobe32((grh << 30) |
			    ((attr->grh.sgid_index & 0xff) << 20) |
			    (attr->grh.flow_label & 0xfffff));
		ah->av.grh_gid_fl = tmp;
		memcpy(ah->av.rgid, attr->grh.dgid.raw, 16);
	}

	if (is_eth) {
		if (ctx->cmds_supp_uhw & MLX5_USER_CMDS_SUPP_UHW_CREATE_AH) {
			struct mlx5_create_ah_resp resp = {};

			if (ibv_cmd_create_ah(pd, &ah->ibv_ah, attr, &resp.ibv_resp, sizeof(resp)))
				goto err;

			ah->kern_ah = true;
			memcpy(ah->av.rmac, resp.dmac, ETHERNET_LL_SIZE);
		} else {
			uint16_t vid;

			if (ibv_resolve_eth_l2_from_gid(pd->context, attr,
							ah->av.rmac, &vid))
				goto err;
		}
	}

	return &ah->ibv_ah;
err:
	free(ah);
	return NULL;
}

int mlx5_destroy_ah(struct ibv_ah *ah)
{
	struct mlx5_ah *mah = to_mah(ah);
	int err;

	if (mah->kern_ah) {
		err = ibv_cmd_destroy_ah(ah);
		if (err)
			return err;
	}

	free(mah);
	return 0;
}

int mlx5_attach_mcast(struct ibv_qp *qp, const union ibv_gid *gid, uint16_t lid)
{
	return ibv_cmd_attach_mcast(qp, gid, lid);
}

int mlx5_detach_mcast(struct ibv_qp *qp, const union ibv_gid *gid, uint16_t lid)
{
	return ibv_cmd_detach_mcast(qp, gid, lid);
}

struct ibv_qp *mlx5_create_qp_ex(struct ibv_context *context,
				 struct ibv_qp_init_attr_ex *attr)
{
	return create_qp(context, attr, NULL);
}

struct ibv_qp *mlx5dv_create_qp(struct ibv_context *context,
				struct ibv_qp_init_attr_ex *qp_attr,
				struct mlx5dv_qp_init_attr *mlx5_qp_attr)
{
	return create_qp(context, qp_attr, mlx5_qp_attr);
}

int mlx5_get_srq_num(struct ibv_srq *srq, uint32_t *srq_num)
{
	struct mlx5_srq *msrq = to_msrq(srq);

	*srq_num = msrq->srqn;

	return 0;
}

struct ibv_xrcd *
mlx5_open_xrcd(struct ibv_context *context,
	       struct ibv_xrcd_init_attr *xrcd_init_attr)
{
	int err;
	struct verbs_xrcd *xrcd;
	struct ibv_open_xrcd cmd = {};
	struct ib_uverbs_open_xrcd_resp resp = {};

	xrcd = calloc(1, sizeof(*xrcd));
	if (!xrcd)
		return NULL;

	err = ibv_cmd_open_xrcd(context, xrcd, sizeof(*xrcd), xrcd_init_attr,
				&cmd, sizeof(cmd), &resp, sizeof(resp));
	if (err) {
		free(xrcd);
		return NULL;
	}

	return &xrcd->xrcd;
}

int mlx5_close_xrcd(struct ibv_xrcd *ib_xrcd)
{
	struct verbs_xrcd *xrcd = container_of(ib_xrcd, struct verbs_xrcd, xrcd);
	int ret;

	ret = ibv_cmd_close_xrcd(xrcd);
	if (!ret)
		free(xrcd);

	return ret;
}

static struct ibv_qp *
create_cmd_qp(struct ibv_context *context,
	      struct ibv_srq_init_attr_ex *srq_attr,
	      struct ibv_srq *srq)
{
	struct ibv_qp_init_attr_ex init_attr = {};
	FILE *fp = to_mctx(context)->dbg_fp;
	struct ibv_port_attr port_attr;
	struct ibv_modify_qp qcmd = {};
	struct ibv_qp_attr attr = {};
	struct ibv_query_port pcmd;
	struct ibv_qp *qp;
	int attr_mask;
	int port = 1;
	int ret;

	ret = ibv_cmd_query_port(context, port, &port_attr,
				 &pcmd, sizeof(pcmd));
	if (ret) {
		mlx5_dbg(fp, MLX5_DBG_QP, "ret %d\n", ret);
		return NULL;
	}

	init_attr.qp_type = IBV_QPT_RC;
	init_attr.srq = srq;
	/* Command QP will be used to pass MLX5_OPCODE_TAG_MATCHING messages
	 * to add/remove tag matching list entries.
	 * WQ size is based on max_ops parameter holding max number of
	 * outstanding list operations.
	 */
	init_attr.cap.max_send_wr = srq_attr->tm_cap.max_ops;
	/* Tag matching list entry will point to a single sge buffer */
	init_attr.cap.max_send_sge = 1;
	init_attr.comp_mask = IBV_QP_INIT_ATTR_PD;
	init_attr.pd = srq_attr->pd;
	init_attr.send_cq = srq_attr->cq;
	init_attr.recv_cq = srq_attr->cq;

	qp = create_qp(context, &init_attr, NULL);
	if (!qp)
		return NULL;

	attr.qp_state = IBV_QPS_INIT;
	attr.port_num = port;
	attr_mask = IBV_QP_STATE | IBV_QP_PKEY_INDEX
		  | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;

	ret = ibv_cmd_modify_qp(qp, &attr, attr_mask, &qcmd, sizeof(qcmd));
	if (ret) {
		mlx5_dbg(fp, MLX5_DBG_QP, "ret %d\n", ret);
		goto err;
	}

	attr.qp_state = IBV_QPS_RTR;
	attr.path_mtu = IBV_MTU_256;
	attr.dest_qp_num = qp->qp_num; /* Loopback */
	attr.ah_attr.dlid = port_attr.lid;
	attr.ah_attr.port_num = port;
	attr_mask = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU
		  | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN
		  | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;

	ret = ibv_cmd_modify_qp(qp, &attr, attr_mask, &qcmd, sizeof(qcmd));
	if (ret) {
		mlx5_dbg(fp, MLX5_DBG_QP, "ret %d\n", ret);
		goto err;
	}

	attr.qp_state = IBV_QPS_RTS;
	attr_mask = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT
		  | IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN
		  | IBV_QP_MAX_QP_RD_ATOMIC;

	ret = ibv_cmd_modify_qp(qp, &attr, attr_mask, &qcmd, sizeof(qcmd));
	if (ret) {
		mlx5_dbg(fp, MLX5_DBG_QP, "ret %d\n", ret);
		goto err;
	}

	return qp;

err:
	mlx5_destroy_qp(qp);
	return NULL;
}

struct ibv_srq *mlx5_create_srq_ex(struct ibv_context *context,
				   struct ibv_srq_init_attr_ex *attr)
{
	int err;
	struct mlx5_create_srq_ex cmd;
	struct mlx5_create_srq_resp resp;
	struct mlx5_srq *msrq;
	struct mlx5_context *ctx = to_mctx(context);
	int max_sge;
	struct ibv_srq *ibsrq;
	int uidx;
	FILE *fp = ctx->dbg_fp;

	if (!(attr->comp_mask & IBV_SRQ_INIT_ATTR_TYPE) ||
	    (attr->srq_type == IBV_SRQT_BASIC))
		return mlx5_create_srq(attr->pd,
				       (struct ibv_srq_init_attr *)attr);

	if (attr->srq_type != IBV_SRQT_XRC &&
	    attr->srq_type != IBV_SRQT_TM) {
		errno = EINVAL;
		return NULL;
	}

	/* An extended CQ is required to read TM information from */
	if (attr->srq_type == IBV_SRQT_TM &&
	    !(attr->cq && (to_mcq(attr->cq)->flags & MLX5_CQ_FLAGS_EXTENDED))) {
		errno = EINVAL;
		return NULL;
	}

	msrq = calloc(1, sizeof(*msrq));
	if (!msrq)
		return NULL;

	ibsrq = (struct ibv_srq *)&msrq->vsrq;

	memset(&cmd, 0, sizeof(cmd));
	memset(&resp, 0, sizeof(resp));

	if (mlx5_spinlock_init(&msrq->lock)) {
		fprintf(stderr, "%s-%d:\n", __func__, __LINE__);
		goto err;
	}

	if (attr->attr.max_wr > ctx->max_srq_recv_wr) {
		fprintf(stderr, "%s-%d:max_wr %d, max_srq_recv_wr %d\n",
			__func__, __LINE__, attr->attr.max_wr,
			ctx->max_srq_recv_wr);
		errno = EINVAL;
		goto err;
	}

	/*
	 * this calculation does not consider required control segments. The
	 * final calculation is done again later. This is done so to avoid
	 * overflows of variables
	 */
	max_sge = ctx->max_recv_wr / sizeof(struct mlx5_wqe_data_seg);
	if (attr->attr.max_sge > max_sge) {
		fprintf(stderr, "%s-%d:max_wr %d, max_srq_recv_wr %d\n",
			__func__, __LINE__, attr->attr.max_wr,
			ctx->max_srq_recv_wr);
		errno = EINVAL;
		goto err;
	}

	msrq->max     = align_queue_size(attr->attr.max_wr + 1);
	msrq->max_gs  = attr->attr.max_sge;
	msrq->counter = 0;

	if (mlx5_alloc_srq_buf(context, msrq)) {
		fprintf(stderr, "%s-%d:\n", __func__, __LINE__);
		goto err;
	}

	msrq->db = mlx5_alloc_dbrec(ctx);
	if (!msrq->db) {
		fprintf(stderr, "%s-%d:\n", __func__, __LINE__);
		goto err_free;
	}

	*msrq->db = 0;

	cmd.buf_addr = (uintptr_t)msrq->buf.buf;
	cmd.db_addr  = (uintptr_t)msrq->db;
	msrq->wq_sig = srq_sig_enabled();
	if (msrq->wq_sig)
		cmd.flags = MLX5_SRQ_FLAG_SIGNATURE;

	attr->attr.max_sge = msrq->max_gs;
	if (ctx->cqe_version) {
		uidx = mlx5_store_uidx(ctx, msrq);
		if (uidx < 0) {
			mlx5_dbg(fp, MLX5_DBG_QP, "Couldn't find free user index\n");
			goto err_free_db;
		}
		cmd.uidx = uidx;
	} else {
		cmd.uidx = 0xffffff;
		pthread_mutex_lock(&ctx->srq_table_mutex);
	}

	err = ibv_cmd_create_srq_ex(context, &msrq->vsrq, sizeof(msrq->vsrq),
				    attr, &cmd.ibv_cmd, sizeof(cmd),
				    &resp.ibv_resp, sizeof(resp));
	if (err)
		goto err_free_uidx;

	if (attr->srq_type == IBV_SRQT_TM) {
		int i;

		msrq->cmd_qp = create_cmd_qp(context, attr, ibsrq);
		if (!msrq->cmd_qp)
			goto err_destroy;

		msrq->tm_list = calloc(attr->tm_cap.max_num_tags + 1,
				       sizeof(struct mlx5_tag_entry));
		if (!msrq->tm_list)
			goto err_free_cmd;
		for (i = 0; i < attr->tm_cap.max_num_tags; i++)
			msrq->tm_list[i].next = &msrq->tm_list[i + 1];
		msrq->tm_head = &msrq->tm_list[0];
		msrq->tm_tail = &msrq->tm_list[attr->tm_cap.max_num_tags];

		msrq->op = calloc(to_mqp(msrq->cmd_qp)->sq.wqe_cnt,
				  sizeof(struct mlx5_srq_op));
		if (!msrq->op)
			goto err_free_tm;
		msrq->op_head = 0;
		msrq->op_tail = 0;
	}

	if (!ctx->cqe_version) {
		err = mlx5_store_srq(to_mctx(context), resp.srqn, msrq);
		if (err)
			goto err_free_tm;

		pthread_mutex_unlock(&ctx->srq_table_mutex);
	}

	msrq->srqn = resp.srqn;
	msrq->rsc.type = MLX5_RSC_TYPE_XSRQ;
	msrq->rsc.rsn = ctx->cqe_version ? cmd.uidx : resp.srqn;

	return ibsrq;

err_free_tm:
	free(msrq->tm_list);
	free(msrq->op);
err_free_cmd:
	if (msrq->cmd_qp)
		mlx5_destroy_qp(msrq->cmd_qp);
err_destroy:
	ibv_cmd_destroy_srq(ibsrq);

err_free_uidx:
	if (ctx->cqe_version)
		mlx5_clear_uidx(ctx, cmd.uidx);
	else
		pthread_mutex_unlock(&ctx->srq_table_mutex);

err_free_db:
	mlx5_free_db(ctx, msrq->db);

err_free:
	free(msrq->wrid);
	mlx5_free_buf(&msrq->buf);

err:
	free(msrq);

	return NULL;
}

int mlx5_query_device_ex(struct ibv_context *context,
			 const struct ibv_query_device_ex_input *input,
			 struct ibv_device_attr_ex *attr,
			 size_t attr_size)
{
	struct mlx5_context *mctx = to_mctx(context);
	struct mlx5_query_device_ex_resp resp;
	struct mlx5_query_device_ex cmd;
	struct ibv_device_attr *a;
	uint64_t raw_fw_ver;
	unsigned sub_minor;
	unsigned major;
	unsigned minor;
	int err;
	int cmd_supp_uhw = mctx->cmds_supp_uhw &
		MLX5_USER_CMDS_SUPP_UHW_QUERY_DEVICE;

	memset(&cmd, 0, sizeof(cmd));
	memset(&resp, 0, sizeof(resp));
	err = ibv_cmd_query_device_ex(context, input, attr, attr_size,
				      &raw_fw_ver,
				      &cmd.ibv_cmd, sizeof(cmd.ibv_cmd), sizeof(cmd),
				      &resp.ibv_resp, sizeof(resp.ibv_resp),
				      cmd_supp_uhw ? sizeof(resp) : sizeof(resp.ibv_resp));
	if (err)
		return err;

	attr->tso_caps = resp.tso_caps;
	attr->rss_caps.rx_hash_fields_mask = resp.rss_caps.rx_hash_fields_mask;
	attr->rss_caps.rx_hash_function = resp.rss_caps.rx_hash_function;
	attr->packet_pacing_caps = resp.packet_pacing_caps.caps;

	if (resp.support_multi_pkt_send_wqe & MLX5_ALLOW_MPW)
		mctx->vendor_cap_flags |= MLX5_VENDOR_CAP_FLAGS_MPW_ALLOWED;

	if (resp.support_multi_pkt_send_wqe & MLX5_SUPPORT_EMPW)
		mctx->vendor_cap_flags |= MLX5_VENDOR_CAP_FLAGS_ENHANCED_MPW;

	mctx->cqe_comp_caps = resp.cqe_comp_caps;
	mctx->sw_parsing_caps = resp.sw_parsing_caps;
	mctx->striding_rq_caps = resp.striding_rq_caps.caps;
	mctx->tunnel_offloads_caps = resp.tunnel_offloads_caps;
	mctx->packet_pacing_caps = resp.packet_pacing_caps;

	if (resp.flags & MLX5_QUERY_DEV_RESP_FLAGS_CQE_128B_COMP)
		mctx->vendor_cap_flags |= MLX5_VENDOR_CAP_FLAGS_CQE_128B_COMP;

	if (resp.flags & MLX5_QUERY_DEV_RESP_FLAGS_CQE_128B_PAD)
		mctx->vendor_cap_flags |= MLX5_VENDOR_CAP_FLAGS_CQE_128B_PAD;

	major     = (raw_fw_ver >> 32) & 0xffff;
	minor     = (raw_fw_ver >> 16) & 0xffff;
	sub_minor = raw_fw_ver & 0xffff;
	a = &attr->orig_attr;
	snprintf(a->fw_ver, sizeof(a->fw_ver), "%d.%d.%04d",
		 major, minor, sub_minor);

	return 0;
}

static int rwq_sig_enabled(struct ibv_context *context)
{
	char *env;

	env = getenv("MLX5_RWQ_SIGNATURE");
	if (env)
		return 1;

	return 0;
}

static void mlx5_free_rwq_buf(struct mlx5_rwq *rwq, struct ibv_context *context)
{
	struct mlx5_context *ctx = to_mctx(context);

	mlx5_free_actual_buf(ctx, &rwq->buf);
	free(rwq->rq.wrid);
}

static int mlx5_alloc_rwq_buf(struct ibv_context *context,
			      struct mlx5_rwq *rwq,
			      int size)
{
	int err;
	enum mlx5_alloc_type alloc_type;

	mlx5_get_alloc_type(to_mctx(context), MLX5_RWQ_PREFIX,
			    &alloc_type, MLX5_ALLOC_TYPE_ANON);

	rwq->rq.wrid = malloc(rwq->rq.wqe_cnt * sizeof(uint64_t));
	if (!rwq->rq.wrid) {
		errno = ENOMEM;
		return -1;
	}

	err = mlx5_alloc_prefered_buf(to_mctx(context), &rwq->buf,
				      align(rwq->buf_size, to_mdev
				      (context->device)->page_size),
				      to_mdev(context->device)->page_size,
				      alloc_type,
				      MLX5_RWQ_PREFIX);

	if (err) {
		free(rwq->rq.wrid);
		errno = ENOMEM;
		return -1;
	}

	return 0;
}

static struct ibv_wq *create_wq(struct ibv_context *context,
			 struct ibv_wq_init_attr *attr,
			 struct mlx5dv_wq_init_attr *mlx5wq_attr)
{
	struct mlx5_create_wq		cmd;
	struct mlx5_create_wq_resp		resp;
	int				err;
	struct mlx5_rwq			*rwq;
	struct mlx5_context	*ctx = to_mctx(context);
	int ret;
	int32_t				usr_idx = 0;
	FILE *fp = ctx->dbg_fp;

	if (attr->wq_type != IBV_WQT_RQ)
		return NULL;

	memset(&cmd, 0, sizeof(cmd));
	memset(&resp, 0, sizeof(resp));

	rwq = calloc(1, sizeof(*rwq));
	if (!rwq)
		return NULL;

	rwq->wq_sig = rwq_sig_enabled(context);
	if (rwq->wq_sig)
		cmd.drv.flags = MLX5_RWQ_FLAG_SIGNATURE;

	ret = mlx5_calc_rwq_size(ctx, rwq, attr, mlx5wq_attr);
	if (ret < 0) {
		errno = -ret;
		goto err;
	}

	rwq->buf_size = ret;
	if (mlx5_alloc_rwq_buf(context, rwq, ret))
		goto err;

	mlx5_init_rwq_indices(rwq);

	if (mlx5_spinlock_init(&rwq->rq.lock))
		goto err_free_rwq_buf;

	rwq->db = mlx5_alloc_dbrec(ctx);
	if (!rwq->db)
		goto err_free_rwq_buf;

	rwq->db[MLX5_RCV_DBR] = 0;
	rwq->db[MLX5_SND_DBR] = 0;
	rwq->pbuff = rwq->buf.buf + rwq->rq.offset;
	rwq->recv_db =  &rwq->db[MLX5_RCV_DBR];
	cmd.drv.buf_addr = (uintptr_t)rwq->buf.buf;
	cmd.drv.db_addr  = (uintptr_t)rwq->db;
	cmd.drv.rq_wqe_count = rwq->rq.wqe_cnt;
	cmd.drv.rq_wqe_shift = rwq->rq.wqe_shift;
	usr_idx = mlx5_store_uidx(ctx, rwq);
	if (usr_idx < 0) {
		mlx5_dbg(fp, MLX5_DBG_QP, "Couldn't find free user index\n");
		goto err_free_db_rec;
	}

	cmd.drv.user_index = usr_idx;

	if (mlx5wq_attr) {
		if (mlx5wq_attr->comp_mask & MLX5DV_WQ_INIT_ATTR_MASK_STRIDING_RQ) {
			if ((mlx5wq_attr->striding_rq_attrs.single_stride_log_num_of_bytes <
			    ctx->striding_rq_caps.min_single_stride_log_num_of_bytes) ||
			    (mlx5wq_attr->striding_rq_attrs.single_stride_log_num_of_bytes >
			     ctx->striding_rq_caps.max_single_stride_log_num_of_bytes)) {
				errno = EINVAL;
				goto err_create;
			}

			if ((mlx5wq_attr->striding_rq_attrs.single_wqe_log_num_of_strides <
			     ctx->striding_rq_caps.min_single_wqe_log_num_of_strides) ||
			    (mlx5wq_attr->striding_rq_attrs.single_wqe_log_num_of_strides >
			     ctx->striding_rq_caps.max_single_wqe_log_num_of_strides)) {
				errno = EINVAL;
				goto err_create;
			}

			cmd.drv.single_stride_log_num_of_bytes =
				mlx5wq_attr->striding_rq_attrs.single_stride_log_num_of_bytes;
			cmd.drv.single_wqe_log_num_of_strides =
				mlx5wq_attr->striding_rq_attrs.single_wqe_log_num_of_strides;
			cmd.drv.two_byte_shift_en =
				mlx5wq_attr->striding_rq_attrs.two_byte_shift_en;
			cmd.drv.comp_mask |= MLX5_IB_CREATE_WQ_STRIDING_RQ;
		}
	}

	err = ibv_cmd_create_wq(context, attr, &rwq->wq, &cmd.ibv_cmd,
				sizeof(cmd.ibv_cmd),
				sizeof(cmd),
				&resp.ibv_resp, sizeof(resp.ibv_resp),
				sizeof(resp));
	if (err)
		goto err_create;

	rwq->rsc.type = MLX5_RSC_TYPE_RWQ;
	rwq->rsc.rsn =  cmd.drv.user_index;

	rwq->wq.post_recv = mlx5_post_wq_recv;
	return &rwq->wq;

err_create:
	mlx5_clear_uidx(ctx, cmd.drv.user_index);
err_free_db_rec:
	mlx5_free_db(to_mctx(context), rwq->db);
err_free_rwq_buf:
	mlx5_free_rwq_buf(rwq, context);
err:
	free(rwq);
	return NULL;
}

struct ibv_wq *mlx5_create_wq(struct ibv_context *context,
			      struct ibv_wq_init_attr *attr)
{
	return create_wq(context, attr, NULL);
}

struct ibv_wq *mlx5dv_create_wq(struct ibv_context *context,
				struct ibv_wq_init_attr *attr,
				struct mlx5dv_wq_init_attr *mlx5_wq_attr)
{
	return create_wq(context, attr, mlx5_wq_attr);
}

int mlx5_modify_wq(struct ibv_wq *wq, struct ibv_wq_attr *attr)
{
	struct mlx5_modify_wq	cmd = {};
	struct mlx5_rwq *rwq = to_mrwq(wq);

	if ((attr->attr_mask & IBV_WQ_ATTR_STATE) &&
	    attr->wq_state == IBV_WQS_RDY) {
		if ((attr->attr_mask & IBV_WQ_ATTR_CURR_STATE) &&
		    attr->curr_wq_state != wq->state)
			return -EINVAL;

		if (wq->state == IBV_WQS_RESET) {
			mlx5_spin_lock(&to_mcq(wq->cq)->lock);
			__mlx5_cq_clean(to_mcq(wq->cq),
					rwq->rsc.rsn, NULL);
			mlx5_spin_unlock(&to_mcq(wq->cq)->lock);
			mlx5_init_rwq_indices(rwq);
			rwq->db[MLX5_RCV_DBR] = 0;
			rwq->db[MLX5_SND_DBR] = 0;
		}
	}

	return ibv_cmd_modify_wq(wq, attr, &cmd.ibv_cmd,  sizeof(cmd.ibv_cmd), sizeof(cmd));
}

int mlx5_destroy_wq(struct ibv_wq *wq)
{
	struct mlx5_rwq *rwq = to_mrwq(wq);
	int ret;

	ret = ibv_cmd_destroy_wq(wq);
	if (ret)
		return ret;

	mlx5_spin_lock(&to_mcq(wq->cq)->lock);
	__mlx5_cq_clean(to_mcq(wq->cq), rwq->rsc.rsn, NULL);
	mlx5_spin_unlock(&to_mcq(wq->cq)->lock);
	mlx5_clear_uidx(to_mctx(wq->context), rwq->rsc.rsn);
	mlx5_free_db(to_mctx(wq->context), rwq->db);
	mlx5_free_rwq_buf(rwq, wq->context);
	free(rwq);

	return 0;
}

struct ibv_flow *mlx5_create_flow(struct ibv_qp *qp, struct ibv_flow_attr *flow_attr)
{
	struct ibv_flow *flow_id;
	int ret;

	flow_id = calloc(1, sizeof *flow_id);
	if (!flow_id)
		return NULL;

	ret = ibv_cmd_create_flow(qp, flow_id, flow_attr);
	if (!ret)
		return flow_id;

	free(flow_id);
	return NULL;
}

int mlx5_destroy_flow(struct ibv_flow *flow_id)
{
	int ret;

	ret = ibv_cmd_destroy_flow(flow_id);
	if (ret)
		return ret;

	free(flow_id);
	return 0;
}

struct ibv_rwq_ind_table *mlx5_create_rwq_ind_table(struct ibv_context *context,
						    struct ibv_rwq_ind_table_init_attr *init_attr)
{
	struct ibv_create_rwq_ind_table *cmd;
	struct mlx5_create_rwq_ind_table_resp resp;
	struct ibv_rwq_ind_table *ind_table;
	uint32_t required_tbl_size;
	int num_tbl_entries;
	int cmd_size;
	int err;

	num_tbl_entries = 1 << init_attr->log_ind_tbl_size;
	/* Data must be u64 aligned */
	required_tbl_size = (num_tbl_entries * sizeof(uint32_t)) < sizeof(uint64_t) ?
			sizeof(uint64_t) : (num_tbl_entries * sizeof(uint32_t));

	cmd_size = required_tbl_size + sizeof(*cmd);
	cmd = calloc(1, cmd_size);
	if (!cmd)
		return NULL;

	memset(&resp, 0, sizeof(resp));
	ind_table = calloc(1, sizeof(*ind_table));
	if (!ind_table)
		goto free_cmd;

	err = ibv_cmd_create_rwq_ind_table(context, init_attr, ind_table, cmd,
					   cmd_size, cmd_size, &resp.ibv_resp, sizeof(resp.ibv_resp),
					   sizeof(resp));
	if (err)
		goto err;

	free(cmd);
	return ind_table;

err:
	free(ind_table);
free_cmd:
	free(cmd);
	return NULL;
}

int mlx5_destroy_rwq_ind_table(struct ibv_rwq_ind_table *rwq_ind_table)
{
	int ret;

	ret = ibv_cmd_destroy_rwq_ind_table(rwq_ind_table);

	if (ret)
		return ret;

	free(rwq_ind_table);
	return 0;
}

int mlx5_modify_cq(struct ibv_cq *cq, struct ibv_modify_cq_attr *attr)
{
	struct ibv_modify_cq cmd = {};

	return ibv_cmd_modify_cq(cq, attr, &cmd, sizeof(cmd));
}
