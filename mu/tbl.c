/*
 * Mu tbls, key-value lookup tables
 *
 * Copyright (c) 2016 Christopher Haster
 * Distributed under the MIT license in mu.h
 */
#include "tbl.h"
#include "mu.h"


// Table access
mu_inline struct mtbl *mtbl(mu_t t) {
    return (struct mtbl *)((muint_t)t & ~7);
}

mu_inline bool mu_isrtbl(mu_t t) {
    return (MTTBL^MTRTBL) & (muint_t)t;
}


// General purpose hash for mu types
mu_inline muint_t mu_tbl_hash(mu_t t, mu_t m) {
    // Mu types have bitwise equality but aren't distributed very well.
    //
    // We can kinda fix this with Knuth's multiplicitive hash
    // (2^32 / golden ratio), but multiplication only propogates entropy
    // upwards, and later masking will lose most of the integer's variation.
    // So instead of masking, we just shift the integer downwards, keeping the
    // most impacted bits.
#ifdef MU64
    return ((muint_t)m * 11400714819323198485UL) >> (64 - mtbl(t)->npw2);
#else
    return ((muint_t)m * 2654435761UL) >> (32 - mtbl(t)->npw2);
#endif
}

// Find smallest integer size for table length
static muintq_t mu_tbl_isize(mlen_t len) {
    if (len > 3221225472) {
        return 8;
    } else if (len > 57344) {
        return 4;
    } else if (len > 240) {
        return 2;
    } else {
        return 1;
    }
}

// Find next power of 2 needed for list or table
static muintq_t mu_tbl_listnpw2(mlen_t len) {
    if (len < MU_MINALLOC/sizeof(mu_t)) {
        len = MU_MINALLOC/sizeof(mu_t);
    }

    return mu_npw2(len);
}

static muintq_t mu_tbl_pairsnpw2(mlen_t len, muintq_t *pisize) {
    const muintq_t psize = 2*sizeof(mu_t);

    // Calculate space for indices, order is important for correct rounding
    muintq_t isize = mu_tbl_isize(len);
    muint_t indices = ((muint_t)psize*len + psize-1) / (psize - isize);
    if (indices < MU_MINALLOC/psize) {
        indices = MU_MINALLOC/psize;
    }

    *pisize = isize;
    return mu_npw2(indices);
}

// Other calculated attributes of tables
mu_inline bool mu_tbl_islist(mu_t t) {
    return mtbl(t)->isize == 0;
}

mu_inline muint_t mu_tbl_count(mu_t t) {
    return mtbl(t)->len + mtbl(t)->nils;
}

mu_inline muint_t mu_tbl_size(mu_t t) {
    return (1 << mtbl(t)->npw2);
}

mu_inline muint_t mu_tbl_off(mu_t t) {
    // This looks complicated but most of these are constants and powers of 2
    const muintq_t psize = 2*sizeof(mu_t);
    return (mtbl(t)->isize*mu_tbl_size(t) + psize-1) / psize;
}

// Indirect entry access
static mu_t *mu_tbl_getpair(mu_t t, muint_t i) {
    muint_t off = 0;
    if (mtbl(t)->isize == 1) {
        off = ((uint8_t*)mtbl(t)->array)[i];
    } else if (mtbl(t)->isize == 2) {
        off = ((uint16_t*)mtbl(t)->array)[i];
    } else if (mtbl(t)->isize == 4) {
        off = ((uint32_t*)mtbl(t)->array)[i];
    } else if (mtbl(t)->isize == 8) {
        off = ((uint64_t*)mtbl(t)->array)[i];
    }

    return off ? &mtbl(t)->array[2*off] : 0;
}

static void mu_tbl_setpair(mu_t t, muint_t i, mu_t *p) {
    muint_t j = (p - mtbl(t)->array)/2;
    if (mtbl(t)->isize == 1) {
        ((uint8_t*)mtbl(t)->array)[i] = j;
    } else if (mtbl(t)->isize == 2) {
        ((uint16_t*)mtbl(t)->array)[i] = j;
    } else if (mtbl(t)->isize == 4) {
        ((uint32_t*)mtbl(t)->array)[i] = j;
    } else if (mtbl(t)->isize == 8) {
        ((uint64_t*)mtbl(t)->array)[i] = j;
    }
}


// Functions for managing tables
mu_t mu_tbl_create(muint_t len) {
    struct mtbl *t = mu_alloc(sizeof(struct mtbl));
    t->ref = 1;
    t->npw2 = mu_tbl_listnpw2(len);
    t->isize = 0;
    t->len = 0;
    t->nils = 0;
    t->tail = 0;

    muint_t size = 1 << t->npw2;
    t->array = mu_alloc(size * sizeof(mu_t));
    memset(t->array, 0, size * sizeof(mu_t));

    return (mu_t)((muint_t)t + MTTBL);
}

mu_t mu_tbl_createtail(muint_t len, mu_t tail) {
    mu_t t = mu_tbl_create(len);
    mu_tbl_settail(t, tail);
    return t;
}

void mu_tbl_settail(mu_t t, mu_t tail) {
    mu_assert(!tail || mu_istbl(tail) || mu_isbuf(tail));
    mu_checkconst(!mu_isrtbl(t), "table");

    if (mu_isbuf(tail)) {
        mu_t b = tail;
        tail = mu_buf_gettail(b);
        mu_dec(b);
    }

    mtbl(t)->tail = tail;
}

void mu_tbl_destroy(mu_t t) {
    muint_t i    = (mu_tbl_islist(t) ? 1 : 2) * mu_tbl_off(t);
    muint_t len  = (mu_tbl_islist(t) ? 1 : 2) * mu_tbl_count(t);
    muint_t size = (mu_tbl_islist(t) ? 1 : 2) * mu_tbl_size(t);
    for (; i < len; i++) {
        mu_dec(mtbl(t)->array[i]);
    }

    mu_dealloc(mtbl(t)->array, size*sizeof(mu_t));
    mu_dec(mtbl(t)->tail);
    mu_dealloc(mtbl(t), sizeof(struct mtbl));
}


// Recursively looks up a key in the table
// returns either that value or nil
mu_t mu_tbl_lookup(mu_t t, mu_t k) {
    mu_assert(mu_istbl(t));
    if (!k) {
        return 0;
    }

    for (; t; t = mtbl(t)->tail) {
        muint_t mask = (1 << mtbl(t)->npw2) - 1;

        if (mu_tbl_islist(t)) {
            muint_t i = mu_num_getuint(k) & mask;

            if (k == mu_num_fromuint(i)) {
                return mu_inc(mtbl(t)->array[i]);
            }
        } else {
            for (muint_t i = mu_tbl_hash(t, k);; i++) {
                mu_t *p = mu_tbl_getpair(t, i & mask);

                if (p && p[0] == k) {
                    mu_dec(k);
                    return mu_inc(p[1]);
                } else if (!p) {
                    break;
                }
            }
        }
    }

    mu_dec(k);
    return 0;
}


// Expands into list/table, possibly converting from a list
static void mu_tbl_listexpand(mu_t t, mlen_t len) {
    mu_t *oldarray = mtbl(t)->array;
    muint_t oldcount = mu_tbl_count(t);
    muint_t oldsize  = mu_tbl_size(t);

    mtbl(t)->npw2 = mu_tbl_listnpw2(len);
    mtbl(t)->array = mu_alloc(mu_tbl_size(t)*sizeof(mu_t));
    memset(mtbl(t)->array, 0, mu_tbl_size(t)*sizeof(mu_t));

    memcpy(mtbl(t)->array, oldarray, oldcount*sizeof(mu_t));
    mu_dealloc(oldarray, oldsize*sizeof(mu_t));
}

static void mu_tbl_pairsexpand(mu_t t, mlen_t len) {
    bool waslist = mu_tbl_islist(t);
    mu_t *oldarray = mtbl(t)->array;
    muint_t oldoff   = mu_tbl_off(t);
    muint_t oldcount = mu_tbl_count(t);
    muint_t oldsize  = mu_tbl_size(t);

    mtbl(t)->npw2 = mu_tbl_pairsnpw2(len, &mtbl(t)->isize);
    mtbl(t)->len = 0;
    mtbl(t)->nils = 0;
    mtbl(t)->array = mu_alloc(2*mu_tbl_size(t)*sizeof(mu_t));
    memset(mtbl(t)->array, 0, 2*mu_tbl_off(t)*sizeof(mu_t));

    for (muint_t i = 0; i < oldcount; i++) {
        if (waslist) {
            mu_tbl_insert(t, mu_num_fromuint(i), oldarray[i]);
        } else {
            mu_tbl_insert(t, oldarray[2*(i+oldoff)+0],
                    oldarray[2*(i+oldoff)+1]);
        }
    }

    mu_dealloc(oldarray, (waslist ? 1 : 2)*oldsize*sizeof(mu_t));
}

// Inserts a value in the table with the given key
// without decending down the tail chain
void mu_tbl_insert(mu_t t, mu_t k, mu_t v) {
    mu_assert(mu_istbl(t));
    mu_checkconst(!mu_isrtbl(t), "table");
    if (!k) {
        mu_dec(v);
        return;
    }

    muint_t mask = (1 << mtbl(t)->npw2) - 1;

    if (mu_tbl_islist(t)) {
        muint_t i = mu_num_getuint(k) & mask;

        if (k == mu_num_fromuint(i) && mtbl(t)->array[i]) {
            // replace old value
            mu_t oldv = mtbl(t)->array[i];
            mtbl(t)->array[i] = v;
            mtbl(t)->len += (v ? 1 : 0) - 1;
            mtbl(t)->nils += (!v ? 1 : 0);
            mu_dec(oldv);
            return;
        } else if (!v) {
            // nothing to remove
            return;
        } else {
            mu_checklen((muint_t)mtbl(t)->len + 1 <= (mlen_t)-1, "table");

            if (k != mu_num_fromuint(i)) {
                muint_t i = mu_num_getuint(k) & (2*mask+1);
                if (k == mu_num_fromuint(i)) {
                    // just needs bigger list
                    mu_tbl_listexpand(t, i+1);
                    mu_tbl_insert(t, k, v);
                    return;
                } else {
                    // needs table
                    mu_tbl_pairsexpand(t, mtbl(t)->len+1);
                    mu_tbl_insert(t, k, v);
                    return;
                }
            }

            // new value fits
            mtbl(t)->array[i] = v;
            mtbl(t)->len += 1;
            mtbl(t)->nils += i - (mtbl(t)->len-1);
            return;
        }
    } else {
        for (muint_t i = mu_tbl_hash(t, k);; i++) {
            mu_t *p = mu_tbl_getpair(t, i & mask);

            if (p && p[0] == k) {
                // replace old value
                mu_t oldv = p[1];
                p[1] = v;
                mtbl(t)->len += (v ? 1 : 0) - (oldv ? 1 : 0);
                mtbl(t)->nils += (!v ? 1 : 0) - (!oldv ? 1 : 0);
                mu_dec(k);
                mu_dec(oldv);
                return;
            } else if (!p && !v) {
                // nothing to remove
                return;
            } else if (!p) {
                mu_checklen((muint_t)mtbl(t)->len + 1 <= (mlen_t)-1, "table");

                muint_t j = mu_tbl_off(t) + mu_tbl_count(t);
                if (j >= mu_tbl_size(t)) {
                    // needs bigger table
                    mu_tbl_pairsexpand(t, mtbl(t)->len+1);
                    mu_tbl_insert(t, k, v);
                    return;
                }

                mtbl(t)->array[2*j+0] = k;
                mtbl(t)->array[2*j+1] = v;
                mtbl(t)->len += 1;
                mu_tbl_setpair(t, i & mask, &mtbl(t)->array[2*j]);
                return;
            }
        }
    }
}

// Recursively assigns a value in the table with the given key
// decends down the tail chain until its found
void mu_tbl_assign(mu_t head, mu_t k, mu_t v) {
    mu_assert(mu_istbl(head));
    bool ro = false;
    if (!k) {
        mu_dec(k);
        return;
    }

    for (mu_t t = head; t; t = mtbl(t)->tail) {
        ro = ro || mu_isrtbl(t);
        muint_t mask = (1 << mtbl(t)->npw2) - 1;

        if (mu_tbl_islist(t)) {
            muint_t i = mu_num_getuint(k) & mask;

            if (k == mu_num_fromuint(i) && mtbl(t)->array[i]) {
                mu_checkconst(!ro, "table");

                // replace old value
                mu_t oldv = mtbl(t)->array[i];
                mtbl(t)->array[i] = v;
                mtbl(t)->len += (v ? 1 : 0) - 1;
                mtbl(t)->nils += (!v ? 1 : 0);
                mu_dec(oldv);
                return;
            }
        } else {
            for (muint_t i = mu_tbl_hash(t, k);; i++) {
                mu_t *p = mu_tbl_getpair(t, i & mask);

                if (p && p[0] == k && p[1]) {
                    mu_checkconst(!ro, "table");

                    // replace old value
                    mu_t oldv = p[1];
                    p[1] = v;
                    mtbl(t)->len  += (v ? 1 : 0) - (oldv ? 1 : 0);
                    mtbl(t)->nils -= (v ? 1 : 0) - (oldv ? 1 : 0);
                    mu_dec(k);
                    mu_dec(oldv);
                    return;
                } else if (!p) {
                    break;
                }
            }
        }
    }

    if (!v) {
        mu_dec(k);
        return;
    }

    mu_tbl_insert(head, k, v);
}


// Performs iteration on a table
bool mu_tbl_next(mu_t t, muint_t *ip, mu_t *kp, mu_t *vp) {
    mu_assert(mu_istbl(t));
    muint_t off = mu_tbl_off(t);
    muint_t count = mu_tbl_count(t);
    muint_t i = *ip;
    mu_t k, v;

    do {
        if (i >= count) {
            return false;
        }

        if (mu_tbl_islist(t)) {
            k = mu_num_fromuint(i);
            v = mtbl(t)->array[i];
        } else {
            k = mtbl(t)->array[2*(i+off)+0];
            v = mtbl(t)->array[2*(i+off)+1];
        }

        i++;
    } while (!v);

    if (kp) *kp = mu_inc(k);
    if (vp) *vp = mu_inc(v);
    *ip = i;
    return true;
}

static mcnt_t mu_tbl_iter_step(mu_t scope, mu_t *frame) {
    mu_t t = mu_tbl_lookup(scope, mu_num_fromuint(0));
    muint_t i = mu_num_getuint(mu_tbl_lookup(scope, mu_num_fromuint(1)));

    bool next = mu_tbl_next(t, &i, 0, &frame[0]);
    mu_dec(t);
    mu_tbl_insert(scope, mu_num_fromuint(1), mu_num_fromuint(i));
    return next ? 1 : 0;
}

mu_t mu_tbl_iter(mu_t t) {
    mu_assert(mu_istbl(t));
    return mu_fn_fromsbfn(0x0, mu_tbl_iter_step,
            mu_tbl_fromlist((mu_t[]){mu_inc(t), mu_num_fromuint(0)}, 2));
}

static mcnt_t mu_tbl_pairs_step(mu_t scope, mu_t *frame) {
    mu_t t = mu_tbl_lookup(scope, mu_num_fromuint(0));
    muint_t i = mu_num_getuint(mu_tbl_lookup(scope, mu_num_fromuint(1)));

    bool next = mu_tbl_next(t, &i, &frame[0], &frame[1]);
    mu_dec(t);
    mu_tbl_insert(scope, mu_num_fromuint(1), mu_num_fromuint(i));
    return next ? 2 : 0;
}

mu_t mu_tbl_pairs(mu_t t) {
    mu_assert(mu_istbl(t));
    return mu_fn_fromsbfn(0x0, mu_tbl_pairs_step,
        mu_tbl_fromlist((mu_t[]){mu_inc(t), mu_num_fromuint(0)}, 2));
}


// Table creating functions
mu_t mu_tbl_initlist(struct mtbl *t, mu_t (*const *def)(void), muint_t n) {
    mu_t m = (mu_t)((muint_t)t + MTTBL);

    for (muint_t i = 0; i < n; i++) {
        if (def[i]) {
            mu_tbl_insert(m, mu_num_fromuint(i), def[i]());
        }
    }

    return (mu_t)((muint_t)t + MTRTBL);
}

mu_t mu_tbl_initpairs(struct mtbl *t, mu_t (*tail)(void),
            mu_t (*const (*def)[2])(void), muint_t n) {
    mu_t m = (mu_t)((muint_t)t + MTTBL);
    if (tail) {
        mu_tbl_settail(m, tail());
    }

    for (muint_t i = 0; i < n; i++) {
        if (def[i][0] && def[i][1]) {
            mu_tbl_insert(m, def[i][0](), def[i][1]());
        }
    }

    return (mu_t)((muint_t)t + MTRTBL);
}

mu_t mu_tbl_fromlist(mu_t *list, muint_t n) {
    mu_t t = mu_tbl_create(n);

    for (muint_t i = 0; i < n; i++) {
        mu_tbl_insert(t, mu_num_fromuint(i), list[i]);
    }

    return t;
}

mu_t mu_tbl_frompairs(mu_t (*pairs)[2], muint_t n) {
    mu_t t = mu_tbl_create(n);

    for (muint_t i = 0; i < n; i++) {
        mu_tbl_insert(t, pairs[i][0], pairs[i][1]);
    }

    return t;
}

static mu_t mu_tbl_fromiter(mu_t i) {
    mu_t frame[MU_FRAME];
    mu_t t = mu_tbl_create(0);
    muint_t index = 0;

    while (mu_fn_next(i, 0x2, frame)) {
        if (frame[1]) {
            mu_tbl_insert(t, frame[0], frame[1]);
        } else {
            mu_tbl_insert(t, mu_num_fromuint(index++), frame[0]);
        }
    }

    mu_dec(i);
    return t;
}

mu_t mu_tbl_frommu(mu_t m) {
    switch (mu_gettype(m)) {
        case MTNIL:
            return mu_tbl_create(0);

        case MTNUM:
            return mu_tbl_create(mu_num_getuint(m));

        case MTSTR:
            return mu_tbl_fromiter(mu_fn_call(MU_ITER, 0x11, m));

        case MTTBL:
        case MTRTBL:
            return mu_tbl_fromiter(mu_fn_call(MU_PAIRS, 0x11, m));

        case MTFN:
            return mu_tbl_fromiter(m);

        default:
            mu_dec(m);
            return 0;
    }
}

// Table operations
void mu_tbl_push(mu_t t, mu_t p, mint_t i) {
    mu_assert(mu_istbl(t));
    mu_checkconst(!mu_isrtbl(t), "table");
    i = (i >= 0) ? i : i + mtbl(t)->len;
    i = (i > mtbl(t)->len) ? mtbl(t)->len : (i < 0) ? 0 : i;

    if (mu_tbl_count(t) + 1 >= mu_tbl_size(t)) {
        mu_checklen((muint_t)mtbl(t)->len + 1 <= (mlen_t)-1, "table");

        if (mu_tbl_islist(t)) {
            mu_tbl_listexpand(t, mu_tbl_count(t)+1);
        } else {
            mu_tbl_pairsexpand(t, mu_tbl_count(t)+1);
        }
    }

    if (mu_tbl_islist(t)) {
        memmove(&mtbl(t)->array[i+1], &mtbl(t)->array[i],
                (mu_tbl_size(t)-(i+1))*sizeof(mu_t));
        mtbl(t)->array[i] = p;
        mtbl(t)->len += (p ? 1 : 0);
    } else {
        muint_t off = mu_tbl_off(t);
        muint_t count = mu_tbl_count(t);
        mtbl(t)->len = 0;
        mtbl(t)->nils = 0;
        memset(mtbl(t)->array, 0, mu_tbl_size(t));

        for (muint_t j = 0; j < i; j++) {
            if (!mtbl(t)->array[2*(j+off)+1]) {
                i++;
            } else {
                mu_tbl_insert(t, mtbl(t)->array[2*(j+off)+0],
                        mtbl(t)->array[2*(j+off)+1]);
            }
        }

        memmove(&mtbl(t)->array[2*(i+1+off)], &mtbl(t)->array[2*(i+off)],
                2*(mu_tbl_size(t)-(i+1+off))*sizeof(mu_t));
        mu_tbl_insert(t, mu_num_fromuint(i), p);

        for (muint_t j = i; j < count; j++) {
            mu_t k = mtbl(t)->array[2*(j+1+off)+0];
            if (mu_isnum(k)) {
                k = mu_num_add(k, mu_num_fromuint(1));
            }

            mu_tbl_insert(t, k, mtbl(t)->array[2*(j+1+off)+1]);
        }
    }
}

mu_t mu_tbl_pop(mu_t t, mint_t i) {
    mu_assert(mu_istbl(t));
    mu_checkconst(!mu_isrtbl(t), "table");
    i = (i >= 0) ? i : i + mtbl(t)->len;
    i = (i > mtbl(t)->len) ? mtbl(t)->len : (i < 0) ? 0 : i;

    if (mu_tbl_islist(t)) {
        mu_t p = mtbl(t)->array[i];
        memmove(&mtbl(t)->array[i], &mtbl(t)->array[i+1],
                (mu_tbl_size(t)-(i+1))*sizeof(mu_t));
        mtbl(t)->len -= (p ? 1 : 0);
        return p;
    } else {
        muint_t off = mu_tbl_off(t);
        muint_t count = mu_tbl_count(t);
        mtbl(t)->len = 0;
        mtbl(t)->nils = 0;
        memset(mtbl(t)->array, 0, mu_tbl_size(t));

        for (muint_t j = 0; j < i; j++) {
            if (!mtbl(t)->array[2*(j+off)+1]) {
                i++;
            } else {
                mu_tbl_insert(t, mtbl(t)->array[2*(j+off)+0],
                        mtbl(t)->array[2*(j+off)+1]);
            }
        }

        mu_dec(mtbl(t)->array[2*(i+off)+0]);
        mu_t p = mtbl(t)->array[2*(i+off)+1];

        for (muint_t j = i+1; j < count; j++) {
            mu_t k = mtbl(t)->array[2*(j+off)+0];
            if (mu_isnum(k)) {
                k = mu_num_sub(k, mu_num_fromuint(1));
            }

            mu_tbl_insert(t, k, mtbl(t)->array[2*(j+off)+1]);
        }

        return p;
    }
}

mu_t mu_tbl_concat(mu_t a, mu_t b, mu_t offset) {
    mu_assert(mu_istbl(a) && mu_istbl(b)
              && (!offset || mu_isnum(offset)));

    if (!offset) {
        offset = mu_num_fromuint(mu_tbl_getlen(a));
    } else if (mu_num_cmp(offset, mu_num_fromuint(0)) < 0) {
        offset = mu_num_add(offset, mu_num_fromuint(mu_tbl_getlen(a)));
    }

    mu_t d = mu_tbl_create(mu_tbl_getlen(a) + mu_tbl_getlen(b));
    mu_t k, v;

    for (muint_t i = 0; mu_tbl_next(a, &i, &k, &v);) {
        mu_tbl_insert(d, k, v);
    }

    for (muint_t i = 0; mu_tbl_next(b, &i, &k, &v);) {
        if (mu_isnum(k)) {
            mu_tbl_insert(d, mu_num_add(k, offset), v);
        } else {
            mu_tbl_insert(d, k, v);
        }
    }

    mu_dec(b);
    return d;
}

mu_t mu_tbl_subset(mu_t t, mint_t lower, mint_t upper) {
    mu_assert(mu_istbl(t));
    lower = (lower >= 0) ? lower : lower + mu_tbl_getlen(t);
    upper = (upper >= 0) ? upper : upper + mu_tbl_getlen(t);

    if (lower < 0) {
        lower = 0;
    }

    if (upper > mtbl(t)->len) {
        upper = mtbl(t)->len;
    }

    if (lower >= upper) {
        mu_dec(t);
        return mu_tbl_create(0);
    }

    mu_t d = mu_tbl_create(upper - lower);

    muint_t i = 0;
    for (muint_t j = 0; j < lower; j++) {
        mu_tbl_next(t, &i, 0, 0);
    }

    for (muint_t j = lower; j < upper; j++) {
        mu_t k, v;
        mu_tbl_next(t, &i, &k, &v);
        if (mu_isnum(k)) {
            k = mu_num_sub(k, mu_num_fromuint(lower));
        }

        mu_tbl_insert(d, k, v);
    }

    return d;
}

// Set operations
mu_t mu_tbl_and(mu_t a, mu_t b) {
    mu_assert(mu_istbl(a) && mu_istbl(b));
    mlen_t alen = mu_tbl_getlen(a);
    mlen_t blen = mu_tbl_getlen(b);
    mu_t d = mu_tbl_create(alen < blen ? alen : blen);
    mu_t k, v;

    for (muint_t i = 0; mu_tbl_next(a, &i, &k, &v);) {
        mu_t w = mu_tbl_lookup(b, mu_inc(k));
        if (w) {
            mu_dec(w);
            mu_tbl_insert(d, k, v);
        } else {
            mu_dec(k);
            mu_dec(v);
        }
    }

    mu_dec(b);
    return d;
}

mu_t mu_tbl_or(mu_t a, mu_t b) {
    mu_assert(mu_istbl(a) && mu_istbl(b));
    mu_t d = mu_tbl_create(mu_tbl_getlen(a) + mu_tbl_getlen(b));
    mu_t k, v;

    for (muint_t i = 0; mu_tbl_next(b, &i, &k, &v);) {
        mu_tbl_insert(d, k, v);
    }

    for (muint_t i = 0; mu_tbl_next(a, &i, &k, &v);) {
        mu_tbl_insert(d, k, v);
    }

    mu_dec(b);
    return d;
}

mu_t mu_tbl_xor(mu_t a, mu_t b) {
    mu_assert(mu_istbl(a) && mu_istbl(b));
    mlen_t alen = mu_tbl_getlen(a);
    mlen_t blen = mu_tbl_getlen(b);
    mu_t d = mu_tbl_create(alen > blen ? alen : blen);
    mu_t k, v;

    for (muint_t i = 0; mu_tbl_next(a, &i, &k, &v);) {
        mu_t w = mu_tbl_lookup(b, mu_inc(k));
        if (!w) {
            mu_tbl_insert(d, k, v);
        } else {
            mu_dec(k);
            mu_dec(v);
            mu_dec(w);
        }
    }

    for (muint_t i = 0; mu_tbl_next(b, &i, &k, &v);) {
        mu_t w = mu_tbl_lookup(a, mu_inc(k));
        if (!w) {
            mu_tbl_insert(d, k, v);
        } else {
            mu_dec(k);
            mu_dec(v);
            mu_dec(w);
        }
    }

    mu_dec(b);
    return d;
}

mu_t mu_tbl_diff(mu_t a, mu_t b) {
    mu_assert(mu_istbl(a) && mu_istbl(b));
    mu_t d = mu_tbl_create(mu_tbl_getlen(a));
    mu_t k, v;

    for (muint_t i = 0; mu_tbl_next(a, &i, &k, &v);) {
        mu_t w = mu_tbl_lookup(b, mu_inc(k));
        if (!w) {
            mu_tbl_insert(d, k, v);
        } else {
            mu_dec(k);
            mu_dec(v);
            mu_dec(w);
        }
    }

    mu_dec(b);
    return d;
}


// String representation
mu_t mu_tbl_parsen(const mbyte_t **ppos, const mbyte_t *end) {
    const mbyte_t *pos = *ppos;
    mu_t t = mu_tbl_create(0);
    mu_t i = mu_num_fromuint(0);

    if (*pos++ != '[') {
        return 0;
    }

    while (pos < end && *pos != ']') {
        mu_t k = mu_parsen(&pos, end);
        if (!k) {
            mu_dec(t);
            return 0;
        }

        if (pos < end && *pos == ':') {
            pos++;
            mu_t v = mu_parsen(&pos, end);
            if (!v) {
                mu_dec(k);
                mu_dec(t);
                return 0;
            }
            mu_tbl_insert(t, k, v);
        } else {
            mu_tbl_insert(t, i, k);
            i = mu_num_add(i, mu_num_fromuint(1));
        }

        if (pos == end || *pos != ',') {
            break;
        }

        pos++;
    }

    if (pos == end || *pos++ != ']') {
        // unterminated table
        mu_dec(t);
        return 0;
    }

    *ppos = pos;
    return t;
}

mu_t mu_tbl_parse(const char *s, muint_t n) {
    const mbyte_t *pos = (const mbyte_t *)s;
    const mbyte_t *end = (const mbyte_t *)pos + n;

    mu_t t = mu_tbl_parsen(&pos, end);

    if (pos != end) {
        mu_dec(t);
        return 0;
    }

    return t;
}

static void mu_tbl_repr_nested(mu_t t, mu_t *s, muint_t *n, mu_t depth) {
    if (mu_num_cmp(depth, mu_num_fromuint(0)) <= 0) {
        mu_buf_pushf(s, n, "%t", t);
        return;
    }

    bool linear = mu_tbl_islist(t);
    for (muint_t i = 0; linear && i < mu_tbl_getlen(t); i++) {
        if (!mtbl(t)->array[i]) {
            linear = false;
        }
    }

    mu_buf_pushc(s, n, '[');

    mu_t k, v;
    for (muint_t i = 0; mu_tbl_next(t, &i, &k, &v);) {
        if (!linear) {
            mu_buf_pushf(s, n, "%r: ", k);
        } else {
            mu_dec(k);
        }

        if (mu_istbl(v)) {
            mu_tbl_repr_nested(v, s, n, mu_num_sub(depth, mu_num_fromuint(1)));
            mu_dec(v);
        } else {
            mu_buf_pushf(s, n, "%r", v);
        }
        mu_buf_pushcstr(s, n, ", ");
    }

    if (mu_tbl_getlen(t) > 0) {
        *n -= 2;
    }

    mu_buf_pushc(s, n, ']');
}

mu_t mu_tbl_repr(mu_t t, mu_t depth) {
    mu_assert(mu_istbl(t) && (!depth || mu_isnum(depth)));
    if (!depth) {
        depth = mu_num_fromuint(1);
    } else if (depth == mu_num_fromuint(0)) {
        return mu_str_format("<tbl 0x%wx>", (muint_t)t & ~7);
    }

    mu_t s = mu_buf_create(0);
    muint_t n = 0;

    mu_tbl_repr_nested(t, &s, &n, depth);

    return mu_str_intern(s, n);
}


// Table related Mu functions
static mcnt_t mu_tbl_bfn(mu_t *frame) {
    mu_t tail = frame[1];
    mu_checkargs(!tail || mu_istbl(tail) || mu_isbuf(tail),
            MU_TBL_KEY, 0x2, frame);

    mu_t m = mu_tbl_frommu(mu_inc(frame[0]));
    mu_checkargs(m, MU_TBL_KEY, 0x2, frame);
    mu_dec(frame[0]);
    frame[0] = m;

    if (mu_istbl(tail)) {
        mu_tbl_settail(frame[0], tail);
    } else {
        mu_tbl_settail(frame[0], mu_buf_gettail(tail));
        mu_dec(tail);
    }

    return 1;
}

MU_DEF_STR(mu_tbl_key_def, "tbl")
MU_DEF_BFN(mu_tbl_def, 0x2, mu_tbl_bfn)
