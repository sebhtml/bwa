#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <math.h>
#ifdef HAVE_PTHREAD
#include <pthread.h>
#endif

#include "kstring.h"
#include "bwamem.h"
#include "bntseq.h"
#include "ksw.h"
#include "kvec.h"
#include "ksort.h"
#include "utils.h"

/* Theory on probability and scoring *ungapped* alignment
 *
 * s'(a,b) = log[P(b|a)/P(b)] = log[4P(b|a)], assuming uniform base distribution
 * s'(a,a) = log(4), s'(a,b) = log(4e/3), where e is the error rate
 *
 * Scale s'(a,b) to s(a,a) s.t. s(a,a)=x. Then s(a,b) = x*s'(a,b)/log(4), or conversely: s'(a,b)=s(a,b)*log(4)/x
 *
 * If the matching score is x and mismatch penalty is -y, we can compute error rate e:
 *   e = .75 * exp[-log(4) * y/x]
 *
 * log P(seq) = \sum_i log P(b_i|a_i) = \sum_i {s'(a,b) - log(4)}
 *   = \sum_i { s(a,b)*log(4)/x - log(4) } = log(4) * (S/x - l)
 *
 * where S=\sum_i s(a,b) is the alignment score. Converting to the phred scale:
 *   Q(seq) = -10/log(10) * log P(seq) = 10*log(4)/log(10) * (l - S/x) = 6.02 * (l - S/x)
 *
 *
 * Gap open (zero gap): q' = log[P(gap-open)], r' = log[P(gap-ext)] (see Durbin et al. (1998) Section 4.1)
 * Then q = x*log[P(gap-open)]/log(4), r = x*log[P(gap-ext)]/log(4)
 *
 * When there are gaps, l should be the length of alignment matches (i.e. the M operator in CIGAR)
 */

mem_opt_t *mem_opt_init()
{
	mem_opt_t *o;
	o = calloc(1, sizeof(mem_opt_t));
	o->flag = 0;
	o->a = 1; o->b = 4; o->q = 6; o->r = 1; o->w = 100;
	o->T = 30;
	o->zdrop = 100;
	o->pen_unpaired = 17;
	o->pen_clip = 5;
	o->min_seed_len = 19;
	o->split_width = 10;
	o->max_occ = 10000;
	o->max_chain_gap = 10000;
	o->max_ins = 10000;
	o->mask_level = 0.50;
	o->chain_drop_ratio = 0.50;
	o->split_factor = 1.5;
	o->chunk_size = 10000000;
	o->n_threads = 1;
	o->max_matesw = 100;
	bwa_fill_scmat(o->a, o->b, o->mat);
	return o;
}

/***************************
 * SMEM iterator interface *
 ***************************/

struct __smem_i {
	const bwt_t *bwt;
	const uint8_t *query;
	int start, len;
	bwtintv_v *matches; // matches; to be returned by smem_next()
	bwtintv_v *sub;     // sub-matches inside the longest match; temporary
	bwtintv_v *tmpvec[2]; // temporary arrays
};

smem_i *smem_itr_init(const bwt_t *bwt)
{
	smem_i *itr;
	itr = calloc(1, sizeof(smem_i));
	itr->bwt = bwt;
	itr->tmpvec[0] = calloc(1, sizeof(bwtintv_v));
	itr->tmpvec[1] = calloc(1, sizeof(bwtintv_v));
	itr->matches   = calloc(1, sizeof(bwtintv_v));
	itr->sub       = calloc(1, sizeof(bwtintv_v));
	return itr;
}

void smem_itr_destroy(smem_i *itr)
{
	free(itr->tmpvec[0]->a); free(itr->tmpvec[0]);
	free(itr->tmpvec[1]->a); free(itr->tmpvec[1]);
	free(itr->matches->a);   free(itr->matches);
	free(itr->sub->a);       free(itr->sub);
	free(itr);
}

void smem_set_query(smem_i *itr, int len, const uint8_t *query)
{
	itr->query = query;
	itr->start = 0;
	itr->len = len;
}

const bwtintv_v *smem_next(smem_i *itr, int split_len, int split_width)
{
	int i, max, max_i, ori_start;
	itr->tmpvec[0]->n = itr->tmpvec[1]->n = itr->matches->n = itr->sub->n = 0;
	if (itr->start >= itr->len || itr->start < 0) return 0;
	while (itr->start < itr->len && itr->query[itr->start] > 3) ++itr->start; // skip ambiguous bases
	if (itr->start == itr->len) return 0;
	ori_start = itr->start;
	itr->start = bwt_smem1(itr->bwt, itr->len, itr->query, ori_start, 1, itr->matches, itr->tmpvec); // search for SMEM
	if (itr->matches->n == 0) return itr->matches; // well, in theory, we should never come here
	for (i = max = 0, max_i = 0; i < itr->matches->n; ++i) { // look for the longest match
		bwtintv_t *p = &itr->matches->a[i];
		int len = (uint32_t)p->info - (p->info>>32);
		if (max < len) max = len, max_i = i;
	}
	if (split_len > 0 && max >= split_len && itr->matches->a[max_i].x[2] <= split_width) { // if the longest SMEM is unique and long
		int j;
		bwtintv_v *a = itr->tmpvec[0]; // reuse tmpvec[0] for merging
		bwtintv_t *p = &itr->matches->a[max_i];
		bwt_smem1(itr->bwt, itr->len, itr->query, ((uint32_t)p->info + (p->info>>32))>>1, itr->matches->a[max_i].x[2]+1, itr->sub, itr->tmpvec); // starting from the middle of the longest MEM
		i = j = 0; a->n = 0;
		while (i < itr->matches->n && j < itr->sub->n) { // ordered merge
			int64_t xi = itr->matches->a[i].info>>32<<32 | (itr->len - (uint32_t)itr->matches->a[i].info);
			int64_t xj = itr->sub->a[j].info>>32<<32 | (itr->len - (uint32_t)itr->sub->a[j].info);
			if (xi < xj) {
				kv_push(bwtintv_t, *a, itr->matches->a[i]);
				++i;
			} else if ((uint32_t)itr->sub->a[j].info - (itr->sub->a[j].info>>32) >= max>>1 && (uint32_t)itr->sub->a[j].info > ori_start) {
				kv_push(bwtintv_t, *a, itr->sub->a[j]);
				++j;
			} else ++j;
		}
		for (; i < itr->matches->n; ++i) kv_push(bwtintv_t, *a, itr->matches->a[i]);
		for (; j < itr->sub->n; ++j)
			if ((uint32_t)itr->sub->a[j].info - (itr->sub->a[j].info>>32) >= max>>1 && (uint32_t)itr->sub->a[j].info > ori_start)
				kv_push(bwtintv_t, *a, itr->sub->a[j]);
		kv_copy(bwtintv_t, *itr->matches, *a);
	}
	return itr->matches;
}

/********************************
 * Chaining while finding SMEMs *
 ********************************/

typedef struct {
	int64_t rbeg;
	int32_t qbeg, len;
} mem_seed_t;

typedef struct {
	int n, m;
	int64_t pos;
	mem_seed_t *seeds;
} mem_chain_t;

typedef struct { size_t n, m; mem_chain_t *a;  } mem_chain_v;

#include "kbtree.h"

#define chain_cmp(a, b) (((b).pos < (a).pos) - ((a).pos < (b).pos))
KBTREE_INIT(chn, mem_chain_t, chain_cmp)

static int test_and_merge(const mem_opt_t *opt, int64_t l_pac, mem_chain_t *c, const mem_seed_t *p)
{
	int64_t qend, rend, x, y;
	const mem_seed_t *last = &c->seeds[c->n-1];
	qend = last->qbeg + last->len;
	rend = last->rbeg + last->len;
	if (p->qbeg >= c->seeds[0].qbeg && p->qbeg + p->len <= qend && p->rbeg >= c->seeds[0].rbeg && p->rbeg + p->len <= rend)
		return 1; // contained seed; do nothing
	if ((last->rbeg < l_pac || c->seeds[0].rbeg < l_pac) && p->rbeg >= l_pac) return 0; // don't chain if on different strand
	x = p->qbeg - last->qbeg; // always non-negtive
	y = p->rbeg - last->rbeg;
	if (y >= 0 && x - y <= opt->w && y - x <= opt->w && x - last->len < opt->max_chain_gap && y - last->len < opt->max_chain_gap) { // grow the chain
		if (c->n == c->m) {
			c->m <<= 1;
			c->seeds = realloc(c->seeds, c->m * sizeof(mem_seed_t));
		}
		c->seeds[c->n++] = *p;
		return 1;
	}
	return 0; // request to add a new chain
}

static void mem_insert_seed(const mem_opt_t *opt, int64_t l_pac, kbtree_t(chn) *tree, smem_i *itr)
{
	const bwtintv_v *a;
	int split_len = (int)(opt->min_seed_len * opt->split_factor + .499);
	split_len = split_len < itr->len? split_len : itr->len;
	while ((a = smem_next(itr, split_len, opt->split_width)) != 0) { // to find all SMEM and some internal MEM
		int i;
		for (i = 0; i < a->n; ++i) { // go through each SMEM/MEM up to itr->start
			bwtintv_t *p = &a->a[i];
			int slen = (uint32_t)p->info - (p->info>>32); // seed length
			int64_t k;
			if (slen < opt->min_seed_len || p->x[2] > opt->max_occ) continue; // ignore if too short or too repetitive
			for (k = 0; k < p->x[2]; ++k) {
				mem_chain_t tmp, *lower, *upper;
				mem_seed_t s;
				int to_add = 0;
				s.rbeg = tmp.pos = bwt_sa(itr->bwt, p->x[0] + k); // this is the base coordinate in the forward-reverse reference
				s.qbeg = p->info>>32;
				s.len  = slen;
				if (s.rbeg < l_pac && l_pac < s.rbeg + s.len) continue; // bridging forward-reverse boundary; skip
				if (kb_size(tree)) {
					kb_intervalp(chn, tree, &tmp, &lower, &upper); // find the closest chain
					if (!lower || !test_and_merge(opt, l_pac, lower, &s)) to_add = 1;
				} else to_add = 1;
				if (to_add) { // add the seed as a new chain
					tmp.n = 1; tmp.m = 4;
					tmp.seeds = calloc(tmp.m, sizeof(mem_seed_t));
					tmp.seeds[0] = s;
					kb_putp(chn, tree, &tmp);
				}
			}
		}
	}
}

void mem_print_chain(const bntseq_t *bns, mem_chain_v *chn)
{
	int i, j;
	for (i = 0; i < chn->n; ++i) {
		mem_chain_t *p = &chn->a[i];
		printf("%d", p->n);
		for (j = 0; j < p->n; ++j) {
			bwtint_t pos;
			int is_rev, ref_id;
			pos = bns_depos(bns, p->seeds[j].rbeg, &is_rev);
			if (is_rev) pos -= p->seeds[j].len - 1;
			bns_cnt_ambi(bns, pos, p->seeds[j].len, &ref_id);
			printf("\t%d,%d,%ld(%s:%c%ld)", p->seeds[j].len, p->seeds[j].qbeg, (long)p->seeds[j].rbeg, bns->anns[ref_id].name, "+-"[is_rev], (long)(pos - bns->anns[ref_id].offset) + 1);
		}
		putchar('\n');
	}
}

mem_chain_v mem_chain(const mem_opt_t *opt, const bwt_t *bwt, int64_t l_pac, int len, const uint8_t *seq)
{
	mem_chain_v chain;
	smem_i *itr;
	kbtree_t(chn) *tree;

	kv_init(chain);
	if (len < opt->min_seed_len) return chain; // if the query is shorter than the seed length, no match
	tree = kb_init(chn, KB_DEFAULT_SIZE);
	itr = smem_itr_init(bwt);
	smem_set_query(itr, len, seq);
	mem_insert_seed(opt, l_pac, tree, itr);

	kv_resize(mem_chain_t, chain, kb_size(tree));

	#define traverse_func(p_) (chain.a[chain.n++] = *(p_))
	__kb_traverse(mem_chain_t, tree, traverse_func);
	#undef traverse_func

	smem_itr_destroy(itr);
	kb_destroy(chn, tree);
	return chain;
}

/********************
 * Filtering chains *
 ********************/

typedef struct {
	int beg, end, w;
	void *p, *p2;
} flt_aux_t;

#define flt_lt(a, b) ((a).w > (b).w)
KSORT_INIT(mem_flt, flt_aux_t, flt_lt)

int mem_chain_flt(const mem_opt_t *opt, int n_chn, mem_chain_t *chains)
{
	flt_aux_t *a;
	int i, j, n;
	if (n_chn <= 1) return n_chn; // no need to filter
	a = malloc(sizeof(flt_aux_t) * n_chn);
	for (i = 0; i < n_chn; ++i) {
		mem_chain_t *c = &chains[i];
		int64_t end;
		int w = 0, tmp;
		for (j = 0, end = 0; j < c->n; ++j) {
			const mem_seed_t *s = &c->seeds[j];
			if (s->qbeg >= end) w += s->len;
			else if (s->qbeg + s->len > end) w += s->qbeg + s->len - end;
			end = end > s->qbeg + s->len? end : s->qbeg + s->len;
		}
		tmp = w;
		for (j = 0, end = 0; j < c->n; ++j) {
			const mem_seed_t *s = &c->seeds[j];
			if (s->rbeg >= end) w += s->len;
			else if (s->rbeg + s->len > end) w += s->rbeg + s->len - end;
			end = end > s->qbeg + s->len? end : s->qbeg + s->len;
		}
		w = w < tmp? w : tmp;
		a[i].beg = c->seeds[0].qbeg;
		a[i].end = c->seeds[c->n-1].qbeg + c->seeds[c->n-1].len;
		a[i].w = w; a[i].p = c; a[i].p2 = 0;
	}
	ks_introsort(mem_flt, n_chn, a);
	{ // reorder chains such that the best chain appears first
		mem_chain_t *swap;
		swap = malloc(sizeof(mem_chain_t) * n_chn);
		for (i = 0; i < n_chn; ++i) {
			swap[i] = *((mem_chain_t*)a[i].p);
			a[i].p = &chains[i]; // as we will memcpy() below, a[i].p is changed
		}
		memcpy(chains, swap, sizeof(mem_chain_t) * n_chn);
		free(swap);
	}
	for (i = 1, n = 1; i < n_chn; ++i) {
		for (j = 0; j < n; ++j) {
			int b_max = a[j].beg > a[i].beg? a[j].beg : a[i].beg;
			int e_min = a[j].end < a[i].end? a[j].end : a[i].end;
			if (e_min > b_max) { // have overlap
				int min_l = a[i].end - a[i].beg < a[j].end - a[j].beg? a[i].end - a[i].beg : a[j].end - a[j].beg;
				if (e_min - b_max >= min_l * opt->mask_level) { // significant overlap
					if (a[j].p2 == 0) a[j].p2 = a[i].p;
					if (a[i].w < a[j].w * opt->chain_drop_ratio && a[j].w - a[i].w >= opt->min_seed_len<<1)
						break;
				}
			}
		}
		if (j == n) a[n++] = a[i]; // if have no significant overlap with better chains, keep it.
	}
	for (i = 0; i < n; ++i) { // mark chains to be kept
		mem_chain_t *c = (mem_chain_t*)a[i].p;
		if (c->n > 0) c->n = -c->n;
		c = (mem_chain_t*)a[i].p2;
		if (c && c->n > 0) c->n = -c->n;
	}
	free(a);
	for (i = 0; i < n_chn; ++i) { // free discarded chains
		mem_chain_t *c = &chains[i];
		if (c->n >= 0) {
			free(c->seeds);
			c->n = c->m = 0;
		} else c->n = -c->n;
	}
	for (i = n = 0; i < n_chn; ++i) { // squeeze out discarded chains
		if (chains[i].n > 0) {
			if (n != i) chains[n++] = chains[i];
			else ++n;
		}
	}
	return n;
}

/******************************
 * De-overlap single-end hits *
 ******************************/

#define alnreg_slt(a, b) ((a).score > (b).score || ((a).score == (b).score && ((a).rb < (b).rb || ((a).rb == (b).rb && (a).qb < (b).qb))))
KSORT_INIT(mem_ars, mem_alnreg_t, alnreg_slt)

int mem_sort_and_dedup(int n, mem_alnreg_t *a)
{
	int m, i;
	if (n <= 1) return n;
	ks_introsort(mem_ars, n, a);
	for (i = 1; i < n; ++i) { // mark identical hits
		if (a[i].score == a[i-1].score && a[i].rb == a[i-1].rb && a[i].qb == a[i-1].qb)
			a[i].qe = a[i].qb;
	}
	for (i = 1, m = 1; i < n; ++i) // exclude identical hits
		if (a[i].qe > a[i].qb) {
			if (m != i) a[m++] = a[i];
			else ++m;
		}
	return m;
}

void mem_mark_primary_se(const mem_opt_t *opt, int n, mem_alnreg_t *a) // IMPORTANT: must run mem_sort_and_dedup() before calling this function
{ // similar to the loop in mem_chain_flt()
	int i, k, tmp;
	kvec_t(int) z;
	if (n == 0) return;
	kv_init(z);
	for (i = 0; i < n; ++i) a[i].sub = 0, a[i].secondary = -1;
	tmp = opt->a + opt->b > opt->q + opt->r? opt->a + opt->b : opt->q + opt->r;
	kv_push(int, z, 0);
	for (i = 1; i < n; ++i) {
		for (k = 0; k < z.n; ++k) {
			int j = z.a[k];
			int b_max = a[j].qb > a[i].qb? a[j].qb : a[i].qb;
			int e_min = a[j].qe < a[i].qe? a[j].qe : a[i].qe;
			if (e_min > b_max) { // have overlap
				int min_l = a[i].qe - a[i].qb < a[j].qe - a[j].qb? a[i].qe - a[i].qb : a[j].qe - a[j].qb;
				if (e_min - b_max >= min_l * opt->mask_level) { // significant overlap
					if (a[j].sub == 0) a[j].sub = a[i].score;
					if (a[j].score - a[i].score <= tmp) ++a[j].sub_n;
					break;
				}
			}
		}
		if (k == z.n) kv_push(int, z, i);
		else a[i].secondary = z.a[k];
	}
	free(z.a);
}

/****************************************
 * Construct the alignment from a chain *
 ****************************************/

/* mem_chain2aln() vs mem_chain2aln_short()
 *
 * mem_chain2aln() covers all the functionality of mem_chain2aln_short().
 * However, it may waste time on extracting the reference sequences given a
 * very long query. mem_chain2aln_short() is faster for very short chains in a
 * long query. It may fail when the matches are long or reach the end of the
 * query. In this case, mem_chain2aln() will be called again.
 * mem_chain2aln_short() is almost never used for short-read alignment.
 */

#define MEM_SHORT_EXT 50
#define MEM_SHORT_LEN 200
#define MAX_BAND_TRY  2

int mem_chain2aln_short(const mem_opt_t *opt, int64_t l_pac, const uint8_t *pac, int l_query, const uint8_t *query, const mem_chain_t *c, mem_alnreg_v *av)
{
	int i, qb, qe, xtra;
	int64_t rb, re, rlen;
	uint8_t *rseq = 0;
	mem_alnreg_t a;
	kswr_t x;

	if (c->n == 0) return -1;
	qb = l_query;  qe = 0;
	rb = l_pac<<1; re = 0;
	memset(&a, 0, sizeof(mem_alnreg_t));
	for (i = 0; i < c->n; ++i) {
		const mem_seed_t *s = &c->seeds[i];
		qb = qb < s->qbeg? qb : s->qbeg;
		qe = qe > s->qbeg + s->len? qe : s->qbeg + s->len;
		rb = rb < s->rbeg? rb : s->rbeg;
		re = re > s->rbeg + s->len? re : s->rbeg + s->len;
		a.seedcov += s->len;
	}
	qb -= MEM_SHORT_EXT; qe += MEM_SHORT_EXT;
	if (qb <= 10 || qe >= l_query - 10) return 1; // because ksw_align() does not support end-to-end alignment
	rb -= MEM_SHORT_EXT; re += MEM_SHORT_EXT;
	rb = rb > 0? rb : 0;
	re = re < l_pac<<1? re : l_pac<<1;
	if (rb < l_pac && l_pac < re) {
		if (c->seeds[0].rbeg < l_pac) re = l_pac;
		else rb = l_pac;
	}
	if ((re - rb) - (qe - qb) > MEM_SHORT_EXT || (qe - qb) - (re - rb) > MEM_SHORT_EXT) return 1;
	if (qe - qb >= opt->w * 4 || re - rb >= opt->w * 4) return 1;
	if (qe - qb >= MEM_SHORT_LEN || re - rb >= MEM_SHORT_LEN) return 1;

	rseq = bns_get_seq(l_pac, pac, rb, re, &rlen);
	assert(rlen == re - rb);
	xtra = KSW_XSUBO | KSW_XSTART | ((qe - qb) * opt->a < 250? KSW_XBYTE : 0) | (opt->min_seed_len * opt->a);
	x = ksw_align(qe - qb, (uint8_t*)query + qb, re - rb, rseq, 5, opt->mat, opt->q, opt->r, xtra, 0);
	free(rseq);
	if (x.tb < MEM_SHORT_EXT>>1 || x.te > re - rb - (MEM_SHORT_EXT>>1)) return 1;

	a.rb = rb + x.tb; a.re = rb + x.te + 1;
	a.qb = qb + x.qb; a.qe = qb + x.qe + 1;
	a.score = x.score;
	a.csub = x.score2;
	kv_push(mem_alnreg_t, *av, a);
	if (bwa_verbose >= 4) printf("SHORT: [%d,%d) <=> [%ld,%ld)\n", a.qb, a.qe, (long)a.rb, (long)a.re);
	return 0;
}

static inline int cal_max_gap(const mem_opt_t *opt, int qlen)
{
	int l = (int)((double)(qlen * opt->a - opt->q) / opt->r + 1.);
	l = l > 1? l : 1;
	return l < opt->w<<1? l : opt->w<<1;
}

void mem_chain2aln(const mem_opt_t *opt, int64_t l_pac, const uint8_t *pac, int l_query, const uint8_t *query, const mem_chain_t *c, mem_alnreg_v *av)
{
	int i, k, max_off[2], aw[2]; // aw: actual bandwidth used in extension
	int64_t rlen, rmax[2], tmp, max = 0;
	const mem_seed_t *s;
	uint8_t *rseq = 0;
	uint64_t *srt;

	if (c->n == 0) return;
	// get the max possible span
	rmax[0] = l_pac<<1; rmax[1] = 0;
	for (i = 0; i < c->n; ++i) {
		int64_t b, e;
		const mem_seed_t *t = &c->seeds[i];
		b = t->rbeg - (t->qbeg + cal_max_gap(opt, t->qbeg));
		e = t->rbeg + t->len + ((l_query - t->qbeg - t->len) + cal_max_gap(opt, l_query - t->qbeg - t->len));
		rmax[0] = rmax[0] < b? rmax[0] : b;
		rmax[1] = rmax[1] > e? rmax[1] : e;
		if (t->len > max) max = t->len;
	}
	rmax[0] = rmax[0] > 0? rmax[0] : 0;
	rmax[1] = rmax[1] < l_pac<<1? rmax[1] : l_pac<<1;
	if (rmax[0] < l_pac && l_pac < rmax[1]) { // crossing the forward-reverse boundary; then choose one side
		if (c->seeds[0].rbeg < l_pac) rmax[1] = l_pac; // this works because all seeds are guaranteed to be on the same strand
		else rmax[0] = l_pac;
	}
	// retrieve the reference sequence
	rseq = bns_get_seq(l_pac, pac, rmax[0], rmax[1], &rlen);
	assert(rlen == rmax[1] - rmax[0]);

	srt = malloc(c->n * 8);
	for (i = 0; i < c->n; ++i)
		srt[i] = (uint64_t)c->seeds[i].len<<32 | i;
	ks_introsort_64(c->n, srt);

	for (k = c->n - 1; k >= 0; --k) {
		mem_alnreg_t *a;
		s = &c->seeds[(uint32_t)srt[k]];

		for (i = 0; i < av->n; ++i) { // test whether extension has been made before
			mem_alnreg_t *p = &av->a[i];
			int64_t rd;
			int qd, w, max_gap;
			if (s->rbeg < p->rb || s->rbeg + s->len > p->re || s->qbeg < p->qb || s->qbeg + s->len > p->qe) continue; // not fully contained
			// qd: distance ahead of the seed on query; rd: on reference
			qd = s->qbeg - p->qb; rd = s->rbeg - p->rb;
			max_gap = cal_max_gap(opt, qd < rd? qd : rd); // the maximal gap allowed in regions ahead of the seed
			w = max_gap < opt->w? max_gap : opt->w; // bounded by the band width
			if (qd - rd < w && rd - qd < w) break; // the seed is "around" a previous hit
			// similar to the previous four lines, but this time we look at the region behind
			qd = p->qe - (s->qbeg + s->len); rd = p->re - (s->rbeg + s->len);
			max_gap = cal_max_gap(opt, qd < rd? qd : rd);
			w = max_gap < opt->w? max_gap : opt->w;
			if (qd - rd < w && rd - qd < w) break;
		}
		if (i < av->n) { // the seed is (almost) contained in an existing alignment
			for (i = k + 1; i < c->n; ++i) { // check overlapping seeds in the same chain
				const mem_seed_t *t;
				if (srt[i] == 0) continue;
				t = &c->seeds[(uint32_t)srt[i]];
				if (t->len < s->len * .95) continue; // only check overlapping if t is long enough; TODO: more efficient by early stopping
				if (s->qbeg <= t->qbeg && s->qbeg + s->len - t->qbeg >= s->len>>2 && t->qbeg - s->qbeg != t->rbeg - s->rbeg) break;
				if (t->qbeg <= s->qbeg && t->qbeg + t->len - s->qbeg >= s->len>>2 && s->qbeg - t->qbeg != s->rbeg - t->rbeg) break;
			}
			if (i == c->n) { // no overlapping seeds; then skip extension
				srt[k] = 0; // mark that seed extension has not been performed
				continue;
			}
		}

		a = kv_pushp(mem_alnreg_t, *av);
		memset(a, 0, sizeof(mem_alnreg_t));
		a->w = aw[0] = aw[1] = opt->w;
		a->score = a->truesc = -1;

		if (s->qbeg) { // left extension
			uint8_t *rs, *qs;
			int qle, tle, gtle, gscore;
			qs = malloc(s->qbeg);
			for (i = 0; i < s->qbeg; ++i) qs[i] = query[s->qbeg - 1 - i];
			tmp = s->rbeg - rmax[0];
			rs = malloc(tmp);
			for (i = 0; i < tmp; ++i) rs[i] = rseq[tmp - 1 - i];
			for (i = 0; i < MAX_BAND_TRY; ++i) {
				int prev = a->score;
				aw[0] = opt->w << i;
				a->score = ksw_extend(s->qbeg, qs, tmp, rs, 5, opt->mat, opt->q, opt->r, aw[0], opt->pen_clip, opt->zdrop, s->len * opt->a, &qle, &tle, &gtle, &gscore, &max_off[0]);
				if (bwa_verbose >= 4) { printf("L\t%d < %d; w=%d; max_off=%d\n", prev, a->score, aw[0], max_off[0]); fflush(stdout); }
				if (a->score == prev || max_off[0] < (aw[0]>>1) + (aw[0]>>2)) break;
			}
			// check whether we prefer to reach the end of the query
			if (gscore <= 0 || gscore <= a->score - opt->pen_clip) { // local extension
				a->qb = s->qbeg - qle, a->rb = s->rbeg - tle;
				a->truesc = a->score;
			} else { // to-end extension
				a->qb = 0, a->rb = s->rbeg - gtle;
				a->truesc = gscore;
			}
			free(qs); free(rs);
		} else a->score = a->truesc = s->len * opt->a, a->qb = 0, a->rb = s->rbeg;

		if (s->qbeg + s->len != l_query) { // right extension
			int qle, tle, qe, re, gtle, gscore, sc0 = a->score;
			qe = s->qbeg + s->len;
			re = s->rbeg + s->len - rmax[0];
			assert(re >= 0);
			for (i = 0; i < MAX_BAND_TRY; ++i) {
				int prev = a->score;
				aw[1] = opt->w << i;
				a->score = ksw_extend(l_query - qe, query + qe, rmax[1] - rmax[0] - re, rseq + re, 5, opt->mat, opt->q, opt->r, aw[1], opt->pen_clip, opt->zdrop, sc0, &qle, &tle, &gtle, &gscore, &max_off[1]);
				if (bwa_verbose >= 4) { printf("R\t%d < %d; w=%d; max_off=%d\n", prev, a->score, aw[1], max_off[1]); fflush(stdout); }
				if (a->score == prev || max_off[1] < (aw[1]>>1) + (aw[1]>>2)) break;
			}
			// similar to the above
			if (gscore <= 0 || gscore <= a->score - opt->pen_clip) { // local extension
				a->qe = qe + qle, a->re = rmax[0] + re + tle;
				a->truesc += a->score - sc0;
			} else { // to-end extension
				a->qe = l_query, a->re = rmax[0] + re + gtle;
				a->truesc += gscore - sc0;
			}
		} else a->qe = l_query, a->re = s->rbeg + s->len;
		if (bwa_verbose >= 4) { printf("[%d]\taw={%d,%d}\tscore=%d\t[%d,%d) <=> [%ld,%ld)\n", k, aw[0], aw[1], a->score, a->qb, a->qe, (long)a->rb, (long)a->re); fflush(stdout); }

		// compute seedcov
		for (i = 0, a->seedcov = 0; i < c->n; ++i) {
			const mem_seed_t *t = &c->seeds[i];
			if (t->qbeg >= a->qb && t->qbeg + t->len <= a->qe && t->rbeg >= a->rb && t->rbeg + t->len <= a->re) // seed fully contained
				a->seedcov += t->len; // this is not very accurate, but for approx. mapQ, this is good enough
		}
		a->w = aw[0] > aw[1]? aw[0] : aw[1];
	}
	free(srt); free(rseq);
}

/*****************************
 * Basic hit->SAM conversion *
 *****************************/

static inline int infer_bw(int l1, int l2, int score, int a, int q, int r)
{
	int w;
	if (l1 == l2 && l1 * a - score < (q + r - a)<<1) return 0; // to get equal alignment length, we need at least two gaps
	w = ((double)((l1 < l2? l1 : l2) * a - score - q) / r + 1.);
	if (w < abs(l1 - l2)) w = abs(l1 - l2);
	return w;
}

static inline int get_rlen(int n_cigar, const uint32_t *cigar)
{
	int k, l;
	for (k = l = 0; k < n_cigar; ++k) {
		int op = cigar[k]&0xf;
		if (op == 0 || op == 2)
			l += cigar[k]>>4;
	}
	return l;
}

void mem_aln2sam(const bntseq_t *bns, kstring_t *str, bseq1_t *s, int n, const mem_aln_t *list, int which, const mem_aln_t *m_)
{
	int i;
	mem_aln_t ptmp = list[which], *p = &ptmp, mtmp, *m = 0; // make a copy of the alignment to convert

	if (m_) mtmp = *m_, m = &mtmp;
	// set flag
	p->flag |= m? 0x1 : 0; // is paired in sequencing
	p->flag |= p->rid < 0? 0x4 : 0; // is mapped
	p->flag |= m && m->rid < 0? 0x8 : 0; // is mate mapped
	if (p->rid < 0 && m && m->rid >= 0) // copy mate to alignment
		p->rid = m->rid, p->pos = m->pos, p->is_rev = m->is_rev, p->n_cigar = 0;
	if (m && m->rid < 0 && p->rid >= 0) // copy alignment to mate
		m->rid = p->rid, m->pos = p->pos, m->is_rev = p->is_rev, m->n_cigar = 0;
	p->flag |= p->is_rev? 0x10 : 0; // is on the reverse strand
	p->flag |= m && m->is_rev? 0x20 : 0; // is mate on the reverse strand

	// print up to CIGAR
	kputs(s->name, str); kputc('\t', str); // QNAME
	kputw((p->flag&0xffff) | (p->flag&0x10000? 0x100 : 0), str); kputc('\t', str); // FLAG
	if (p->rid >= 0) { // with coordinate
		kputs(bns->anns[p->rid].name, str); kputc('\t', str); // RNAME
		kputl(p->pos + 1, str); kputc('\t', str); // POS
		kputw(p->mapq, str); kputc('\t', str); // MAPQ
		if (p->n_cigar) { // aligned
			for (i = 0; i < p->n_cigar; ++i) {
				kputw(p->cigar[i]>>4, str); kputc("MIDSH"[p->cigar[i]&0xf], str);
			}
		} else kputc('*', str); // having a coordinate but unaligned (e.g. when copy_mate is true)
	} else kputsn("*\t0\t0\t*", 7, str); // without coordinte
	kputc('\t', str);

	// print the mate position if applicable
	if (m && m->rid >= 0) {
		if (p->rid == m->rid) kputc('=', str);
		else kputs(bns->anns[m->rid].name, str);
		kputc('\t', str);
		kputl(m->pos + 1, str); kputc('\t', str);
		if (p->rid == m->rid) {
			int64_t p0 = p->pos + (p->is_rev? get_rlen(p->n_cigar, p->cigar) - 1 : 0);
			int64_t p1 = m->pos + (m->is_rev? get_rlen(m->n_cigar, m->cigar) - 1 : 0);
			if (m->n_cigar == 0 || p->n_cigar == 0) kputc('0', str);
			else kputl(-(p0 - p1 + (p0 > p1? 1 : p0 < p1? -1 : 0)), str);
		} else kputc('0', str);
	} else kputsn("*\t0\t0", 5, str);
	kputc('\t', str);

	// print SEQ and QUAL
	if (p->flag & 0x100) { // for secondary alignments, don't write SEQ and QUAL
		kputsn("*\t*", 3, str);
	} else if (!p->is_rev) { // the forward strand
		int i, qb = 0, qe = s->l_seq;
		if (p->n_cigar) {
			if ((p->cigar[0]&0xf) == 4) qb += p->cigar[0]>>4;
			if ((p->cigar[p->n_cigar-1]&0xf) == 4) qe -= p->cigar[p->n_cigar-1]>>4;
		}
		ks_resize(str, str->l + (qe - qb) + 1);
		for (i = qb; i < qe; ++i) str->s[str->l++] = "ACGTN"[(int)s->seq[i]];
		kputc('\t', str);
		if (s->qual) { // printf qual
			ks_resize(str, str->l + (qe - qb) + 1);
			for (i = qb; i < qe; ++i) str->s[str->l++] = s->qual[i];
			str->s[str->l] = 0;
		} else kputc('*', str);
	} else { // the reverse strand
		int i, qb = 0, qe = s->l_seq;
		if (p->n_cigar) {
			if ((p->cigar[0]&0xf) == 4) qe -= p->cigar[0]>>4;
			if ((p->cigar[p->n_cigar-1]&0xf) == 4) qb += p->cigar[p->n_cigar-1]>>4;
		}
		ks_resize(str, str->l + (qe - qb) + 1);
		for (i = qe-1; i >= qb; --i) str->s[str->l++] = "TGCAN"[(int)s->seq[i]];
		kputc('\t', str);
		if (s->qual) { // printf qual
			ks_resize(str, str->l + (qe - qb) + 1);
			for (i = qe-1; i >= qb; --i) str->s[str->l++] = s->qual[i];
			str->s[str->l] = 0;
		} else kputc('*', str);
	}

	// print optional tags
	if (p->n_cigar) { kputsn("\tNM:i:", 6, str); kputw(p->NM, str); }
	if (p->score >= 0) { kputsn("\tAS:i:", 6, str); kputw(p->score, str); }
	if (p->sub >= 0) { kputsn("\tXS:i:", 6, str); kputw(p->sub, str); }
	if (bwa_rg_id[0]) { kputsn("\tRG:Z:", 6, str); kputs(bwa_rg_id, str); }
	for (i = 0; i < n; ++i)
		if (i != which && !(list[i].flag&0x20000)) break; // 0x20000: shadowed multi hit
	if (i < n) { // there are other primary hits; output them
		kputsn("\tXP:Z:", 6, str);
		for (i = 0; i < n; ++i) {
			const mem_aln_t *r = &list[i];
			int k;
			if (i == which || (list[i].flag&0x20000)) continue; // proceed if: 1) different from the current; 2) not shadowed multi hit
			kputs(bns->anns[r->rid].name, str); kputc(',', str);
			kputc("+-"[r->is_rev], str);
			kputl(r->pos+1, str); kputc(',', str);
			for (k = 0; k < r->n_cigar; ++k) {
				kputw(r->cigar[k]>>4, str); kputc("MIDSH"[r->cigar[k]&0xf], str);
			}
			kputc(',', str); kputw(r->mapq, str);
			kputc(',', str); kputw(r->NM, str);
			kputc(';', str);
		}
	}
	if (s->comment) { kputc('\t', str); kputs(s->comment, str); }
	kputc('\n', str);
}

/************************
 * Integrated interface *
 ************************/

int mem_approx_mapq_se(const mem_opt_t *opt, const mem_alnreg_t *a)
{
	int mapq, l, sub = a->sub? a->sub : opt->min_seed_len * opt->a;
	double identity;
	sub = a->csub > sub? a->csub : sub;
	if (sub >= a->score) return 0;
	l = a->qe - a->qb > a->re - a->rb? a->qe - a->qb : a->re - a->rb;
	mapq = a->score? (int)(MEM_MAPQ_COEF * (1. - (double)sub / a->score) * log(a->seedcov) + .499) : 0;
	identity = 1. - (double)(l * opt->a - a->score) / (opt->a + opt->b) / l;
	mapq = identity < 0.95? (int)(mapq * identity * identity + .499) : mapq;
	if (a->sub_n > 0) mapq -= (int)(4.343 * log(a->sub_n+1) + .499);
	if (mapq > 60) mapq = 60;
	if (mapq < 0) mapq = 0;
	return mapq;
}

void mem_reg2sam_se(const mem_opt_t *opt, const bntseq_t *bns, const uint8_t *pac, bseq1_t *s, mem_alnreg_v *a, int extra_flag, const mem_aln_t *m)
{
	kstring_t str;
	kvec_t(mem_aln_t) aa;
	int k;

	kv_init(aa);
	str.l = str.m = 0; str.s = 0;
	for (k = 0; k < a->n; ++k) {
		mem_alnreg_t *p = &a->a[k];
		mem_aln_t *q;
		if (p->score < opt->T) continue;
		if (p->secondary >= 0 && !(opt->flag&MEM_F_ALL)) continue;
		if (p->secondary >= 0 && p->score < a->a[p->secondary].score * .5) continue;
		q = kv_pushp(mem_aln_t, aa);
		*q = mem_reg2aln(opt, bns, pac, s->l_seq, s->seq, p);
		if (q->rid < 0) { // unfixable cross-reference alignment
			--aa.n;
			continue;
		}
		q->flag |= extra_flag | (p->secondary >= 0? 0x100 : 0); // flag secondary
		if (p->secondary >= 0) q->sub = -1; // don't output sub-optimal score
		if ((opt->flag&MEM_F_NO_MULTI) && k && p->secondary < 0) q->flag |= 0x10000;
		if (k && q->mapq > aa.a[0].mapq) q->mapq = aa.a[0].mapq;
	}
	if (aa.n == 0) { // no alignments good enough; then write an unaligned record
		mem_aln_t t;
		t = mem_reg2aln(opt, bns, pac, s->l_seq, s->seq, 0);
		t.flag |= extra_flag;
		mem_aln2sam(bns, &str, s, 1, &t, 0, m);
	} else {
		for (k = 0; k < aa.n; ++k)
			mem_aln2sam(bns, &str, s, aa.n, aa.a, k, m);
		for (k = 0; k < aa.n; ++k) free(aa.a[k].cigar);
		free(aa.a);
	}
	s->sam = str.s;
}

mem_alnreg_v mem_align1_core(const mem_opt_t *opt, const bwt_t *bwt, const bntseq_t *bns, const uint8_t *pac, int l_seq, char *seq)
{
	int i;
	mem_chain_v chn;
	mem_alnreg_v regs;

	for (i = 0; i < l_seq; ++i) // convert to 2-bit encoding if we have not done so
		seq[i] = seq[i] < 4? seq[i] : nst_nt4_table[(int)seq[i]];

	chn = mem_chain(opt, bwt, bns->l_pac, l_seq, (uint8_t*)seq);
	chn.n = mem_chain_flt(opt, chn.n, chn.a);
	if (bwa_verbose >= 4) mem_print_chain(bns, &chn);

	kv_init(regs);
	for (i = 0; i < chn.n; ++i) {
		mem_chain_t *p = &chn.a[i];
		int ret;
		ret = mem_chain2aln_short(opt, bns->l_pac, pac, l_seq, (uint8_t*)seq, p, &regs);
		if (ret > 0) mem_chain2aln(opt, bns->l_pac, pac, l_seq, (uint8_t*)seq, p, &regs);
		free(chn.a[i].seeds);
	}
	free(chn.a);
	regs.n = mem_sort_and_dedup(regs.n, regs.a);
	return regs;
}

mem_alnreg_v mem_align1(const mem_opt_t *opt, const bwt_t *bwt, const bntseq_t *bns, const uint8_t *pac, int l_seq, const char *seq_)
{ // the difference from mem_align1_core() is that this routine: 1) calls mem_mark_primary_se(); 2) does not modify the input sequence
	mem_alnreg_v ar;
	char *seq;
	seq = malloc(l_seq);
	memcpy(seq, seq_, l_seq); // makes a copy of seq_
	ar = mem_align1_core(opt, bwt, bns, pac, l_seq, seq);
	mem_mark_primary_se(opt, ar.n, ar.a);
	free(seq);
	return ar;
}

// This routine is only used for the API purpose
mem_aln_t mem_reg2aln(const mem_opt_t *opt, const bntseq_t *bns, const uint8_t *pac, int l_query, const char *query_, const mem_alnreg_t *ar)
{
	mem_aln_t a;
	int i, w2, qb, qe, NM, score, is_rev;
	int64_t pos, rb, re;
	uint8_t *query;

	memset(&a, 0, sizeof(mem_aln_t));
	if (ar == 0 || ar->rb < 0 || ar->re < 0) { // generate an unmapped record
		a.rid = -1; a.pos = -1; a.flag |= 0x4;
		return a;
	}
	qb = ar->qb, qe = ar->qe;
	rb = ar->rb, re = ar->re;
	query = malloc(l_query);
	for (i = 0; i < l_query; ++i) // convert to the nt4 encoding
		query[i] = query_[i] < 5? query_[i] : nst_nt4_table[(int)query_[i]];
	a.mapq = ar->secondary < 0? mem_approx_mapq_se(opt, ar) : 0;
	if (ar->secondary >= 0) a.flag |= 0x20000;
	if (bwa_fix_xref(opt->mat, opt->q, opt->r, opt->w, bns, pac, (uint8_t*)query, &qb, &qe, &rb, &re) < 0) { // unfixable cross-reference alignment
		a.rid = -1; a.pos = -1; a.flag |= 0x4;
		return a;
	}
	w2 = infer_bw(qe - qb, re - rb, ar->truesc, opt->a, opt->q, opt->r);
	w2 = w2 < opt->w? w2 : opt->w;
	a.cigar = bwa_gen_cigar(opt->mat, opt->q, opt->r, w2, bns->l_pac, pac, qe - qb, (uint8_t*)&query[qb], rb, re, &score, &a.n_cigar, &NM);
	a.NM = NM;
	pos = bns_depos(bns, rb < bns->l_pac? rb : re - 1, &is_rev);
	a.is_rev = is_rev;
	if (a.n_cigar > 0) {
		if ((a.cigar[0]&0xf) == 2) {
			pos += a.cigar[0]>>4;
			--a.n_cigar;
			memmove(a.cigar, a.cigar + 1, a.n_cigar * 4);
		} else if ((a.cigar[a.n_cigar-1]&0xf) == 2) --a.n_cigar;
	}
	if (qb != 0 || qe != l_query) { // add clipping to CIGAR
		int clip5, clip3;
		clip5 = is_rev? l_query - qe : qb;
		clip3 = is_rev? qb : l_query - qe;
		a.cigar = realloc(a.cigar, 4 * (a.n_cigar + 2));
		if (clip5) {
			memmove(a.cigar+1, a.cigar, a.n_cigar * 4);
			a.cigar[0] = clip5<<4 | (opt->flag&MEM_F_HARDCLIP? 4 : 3);
			++a.n_cigar;
		}
		if (clip3) a.cigar[a.n_cigar++] = clip3<<4 | (opt->flag&MEM_F_HARDCLIP? 4 : 3);
	}
	a.rid = bns_pos2rid(bns, pos);
	a.pos = pos - bns->anns[a.rid].offset;
	a.score = ar->score; a.sub = ar->sub > ar->csub? ar->sub : ar->csub;
	free(query);
	return a;
}

typedef struct {
	int start, step, n;
	const mem_opt_t *opt;
	const bwt_t *bwt;
	const bntseq_t *bns;
	const uint8_t *pac;
	const mem_pestat_t *pes;
	bseq1_t *seqs;
	mem_alnreg_v *regs;
} worker_t;

static void *worker1(void *data)
{
	worker_t *w = (worker_t*)data;
	int i;
	if (!(w->opt->flag&MEM_F_PE)) {
		for (i = w->start; i < w->n; i += w->step)
			w->regs[i] = mem_align1_core(w->opt, w->bwt, w->bns, w->pac, w->seqs[i].l_seq, w->seqs[i].seq);
	} else { // for PE we align the two ends in the same thread in case the 2nd read is of worse quality, in which case some threads may be faster/slower
		for (i = w->start; i < w->n>>1; i += w->step) {
			w->regs[i<<1|0] = mem_align1_core(w->opt, w->bwt, w->bns, w->pac, w->seqs[i<<1|0].l_seq, w->seqs[i<<1|0].seq);
			w->regs[i<<1|1] = mem_align1_core(w->opt, w->bwt, w->bns, w->pac, w->seqs[i<<1|1].l_seq, w->seqs[i<<1|1].seq);
		}
	}
	return 0;
}

static void *worker2(void *data)
{
	extern int mem_sam_pe(const mem_opt_t *opt, const bntseq_t *bns, const uint8_t *pac, const mem_pestat_t pes[4], uint64_t id, bseq1_t s[2], mem_alnreg_v a[2]);
	worker_t *w = (worker_t*)data;
	int i;
	if (!(w->opt->flag&MEM_F_PE)) {
		for (i = w->start; i < w->n; i += w->step) {
			mem_mark_primary_se(w->opt, w->regs[i].n, w->regs[i].a);
			mem_reg2sam_se(w->opt, w->bns, w->pac, &w->seqs[i], &w->regs[i], 0, 0);
			free(w->regs[i].a);
		}
	} else {
		int n = 0;
		for (i = w->start; i < w->n>>1; i += w->step) { // not implemented yet
			n += mem_sam_pe(w->opt, w->bns, w->pac, w->pes, i, &w->seqs[i<<1], &w->regs[i<<1]);
			free(w->regs[i<<1|0].a); free(w->regs[i<<1|1].a);
		}
		fprintf(stderr, "[M::%s@%d] performed mate-SW for %d reads\n", __func__, w->start, n);
	}
	return 0;
}

void mem_process_seqs(const mem_opt_t *opt, const bwt_t *bwt, const bntseq_t *bns, const uint8_t *pac, int n, bseq1_t *seqs)
{
	int i;
	worker_t *w;
	mem_alnreg_v *regs;
	mem_pestat_t pes[4];

	w = calloc(opt->n_threads, sizeof(worker_t));
	regs = malloc(n * sizeof(mem_alnreg_v));
	for (i = 0; i < opt->n_threads; ++i) {
		worker_t *p = &w[i];
		p->start = i; p->step = opt->n_threads; p->n = n;
		p->opt = opt; p->bwt = bwt; p->bns = bns; p->pac = pac;
		p->seqs = seqs; p->regs = regs;
		p->pes = &pes[0];
	}
#ifdef HAVE_PTHREAD
	if (opt->n_threads == 1) {
		worker1(w);
		if (opt->flag&MEM_F_PE) mem_pestat(opt, bns->l_pac, n, regs, pes);
		worker2(w);
	} else {
		pthread_t *tid;
		tid = (pthread_t*)calloc(opt->n_threads, sizeof(pthread_t));
		for (i = 0; i < opt->n_threads; ++i) pthread_create(&tid[i], 0, worker1, &w[i]);
		for (i = 0; i < opt->n_threads; ++i) pthread_join(tid[i], 0);
		if (opt->flag&MEM_F_PE) mem_pestat(opt, bns->l_pac, n, regs, pes);
		for (i = 0; i < opt->n_threads; ++i) pthread_create(&tid[i], 0, worker2, &w[i]);
		for (i = 0; i < opt->n_threads; ++i) pthread_join(tid[i], 0);
		free(tid);
	}
#else
	worker1(w);
	if (opt->flag&MEM_F_PE) mem_pestat(opt, bns->l_pac, n, regs, pes);
	worker2(w);
#endif
	for (i = 0; i < n; ++i) {
		fputs(seqs[i].sam, stdout);
		free(seqs[i].name); free(seqs[i].comment); free(seqs[i].seq); free(seqs[i].qual); free(seqs[i].sam);
	}
	free(regs); free(w);
}
