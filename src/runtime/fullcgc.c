/*
 * This software is part of the SBCL system. See the README file for
 * more information.
 *
 * This software is derived from the CMU CL system, which was
 * written at Carnegie Mellon University and released into the
 * public domain. The software is in the public domain and is
 * provided with absolutely no warranty. See the COPYING and CREDITS
 * files for more information.
 */

#include "gc.h"
#include "gc-internal.h"
#include "genesis/gc-tables.h"
#include "genesis/closure.h"
#include "genesis/layout.h"
#include "genesis/hash-table.h"
#include "queue.h"

#include <stdio.h>
#ifndef LISP_FEATURE_WIN32
#define HAVE_GETRUSAGE 1
#endif
#if HAVE_GETRUSAGE
#include <sys/resource.h> // for getrusage()
#endif


/* Most headered objects use MARK_BIT to record liveness.
 * Bignums always use the leftmost bit regardless of word size */
#define MARK_BIT ((uword_t)1 << 31)
#ifdef LISP_FEATURE_64_BIT
#define BIGNUM_MARK_BIT ((uword_t)1 << 63)
#else
#define BIGNUM_MARK_BIT MARK_BIT
#endif

#ifdef LISP_FEATURE_IMMOBILE_SPACE
#define interesting_pointer_p(x) \
  (find_page_index((void*)x) >= 0 || find_immobile_page_index((void*)x) >= 0)
#else
#define interesting_pointer_p(x) (find_page_index((void*)x) >= 0)
#endif

#ifdef DEBUG
#  define dprintf(arg) printf arg
FILE * logfile;
#else
#  define dprintf(arg)
#endif

struct unbounded_queue {
  struct Qblock* head_block;
  struct Qblock* tail_block;
  struct Qblock* recycler;
  long tot_count; // Not used
} scav_queue;

/* Initialized to number of pages in page table
 * and decremented before use. */
static page_index_t free_page;

/* The whole-page allocator works backwards from the end of dynamic space.
 * If it collides with 'last_free_page', then you lose. */
static void* get_free_page() {
    --free_page;
    if (free_page < last_free_page)
        lose("Needed more space to GC\n");
    char* mem = page_address(free_page);
    if (page_need_to_zero(free_page))
        memset(mem, 0, GENCGC_CARD_BYTES);
    set_page_need_to_zero(free_page, 1);
    return mem;
}

/* The suballocator doles out blocks of bits for marking conses live.
 * Example: If pages are 32768 bytes, and Lisp words are 8 bytes,
 * then one GC page can hold 2K cons cells.
 * One byte marks 8 conses (1 bit per cons), 256 bytes mark 2048 conses.
 * 128 blocks of 256 bytes fit on a 32K GC page. */
char *suballocator_free_ptr, *suballocator_end_ptr;

static void* allocate_cons_mark_bits() {
    int nbytes = GENCGC_CARD_BYTES / (2 * N_WORD_BYTES) / 8;
    if (suballocator_free_ptr + nbytes > suballocator_end_ptr) {
        suballocator_free_ptr = get_free_page();
        suballocator_end_ptr = suballocator_free_ptr + GENCGC_CARD_BYTES;
    }
    void* mem = suballocator_free_ptr;
    suballocator_free_ptr += nbytes;
    return mem;
}

static void gc_enqueue(lispobj object)
{
    gc_dcheck(is_lisp_pointer(object));
    struct Qblock* block = scav_queue.tail_block;
    if (block->count == QBLOCK_CAPACITY) {
        struct Qblock* next;
        next = scav_queue.recycler;
        if (next) {
            scav_queue.recycler = next->next;
            next->next = 0;
            dprintf(("Popped recycle list\n"));
        } else {
            next = (struct Qblock*)get_free_page();
            dprintf(("Alloc'd new block\n"));
        }
        block = block->next = next;
        scav_queue.tail_block = block;
    }
    block->elements[block->tail] = object;
    if (++block->tail == QBLOCK_CAPACITY) block->tail = 0;
    ++block->count;
}

static lispobj gc_dequeue()
{
    struct Qblock* block = scav_queue.head_block;
    gc_assert(block->count);
    int index = block->tail - block->count;
    lispobj object = block->elements[index + (index<0 ? QBLOCK_CAPACITY : 0)];
    if (--block->count == 0) {
        struct Qblock* next = block->next;
        if (next) {
            scav_queue.head_block = next;
            block->next = scav_queue.recycler;
            scav_queue.recycler = block;
            dprintf(("Qblock emptied - returned to recycle list\n"));
        } else {
            dprintf(("Qblock emptied - NOT returned to recycle list\n"));
        }
    }
    return object;
}

/* The 'mark_bits' hashtable maps a page address to a block of mark bits
 * for headerless objects (conses) */
struct hopscotch_table mark_bits;

static inline uword_t compute_page_key(lispobj cons) {
    return cons & ~(GENCGC_CARD_BYTES - 1);
}
static inline int compute_dword_number(lispobj cons) {
    return (cons & (GENCGC_CARD_BYTES - 1)) >> (1+WORD_SHIFT);
}

static inline int cons_markedp(lispobj pointer) {
    unsigned char* bits = (unsigned char*)
        hopscotch_get(&mark_bits, compute_page_key(pointer), 0);
    if (!bits) return 0;
    int index = compute_dword_number(pointer);
    return (bits[index / 8] >> (index % 8)) & 1;
}

/* Return true if OBJ has already survived the current GC. */
static inline int pointer_survived_gc_yet(lispobj pointer)
{
    if (!interesting_pointer_p(pointer))
        return 1;
    if (lowtag_of(pointer) == LIST_POINTER_LOWTAG)
        return cons_markedp(pointer);
    lispobj header = *native_pointer(pointer);
    if (widetag_of(header) == BIGNUM_WIDETAG)
        return (header & BIGNUM_MARK_BIT) != 0;
    if (widetag_of(header) == SIMPLE_FUN_WIDETAG)
        header = *fun_code_header(native_pointer(pointer));
    return (header & MARK_BIT) != 0;
}

void __mark_obj(lispobj pointer)
{
    gc_dcheck(is_lisp_pointer(pointer));
    if (!interesting_pointer_p(pointer))
        return;
    if (lowtag_of(pointer) != LIST_POINTER_LOWTAG) {
        lispobj* base = native_pointer(pointer);
        lispobj header = *base;
        if (widetag_of(header) == BIGNUM_WIDETAG) {
            *base |= BIGNUM_MARK_BIT;
            return; // don't enqueue - no pointers
        } else {
            if (widetag_of(header) == SIMPLE_FUN_WIDETAG) {
                base = fun_code_header(base);
                pointer = make_lispobj(base, OTHER_POINTER_LOWTAG);
                header = *base;
            }
            // Bit 31 of the header is the mark bit for all remaining object types.
            // This avoids clash with the layout pointer of instances and functions,
            // TLS index of symbols, and various other bits.
            // The mark bit occupies the same byte as the generation number
            // in immobile space, but doesn't conflict with that usage.
            if (header & MARK_BIT)
                return; // already marked
            *base |= MARK_BIT;
        }
        if (unboxed_obj_widetag_p(widetag_of(header)))
            return;
    } else {
        uword_t key = compute_page_key(pointer);
        int index = compute_dword_number(pointer);
        unsigned char* bits = (unsigned char*)hopscotch_get(&mark_bits, key, 0);
        if (!bits) {
            bits = allocate_cons_mark_bits();
            hopscotch_insert(&mark_bits, key, (sword_t)bits);
        } else if (bits[index / 8] & (1 << (index % 8))) {
            return;
        }
        // Mark the cons
        bits[index / 8] |= 1 << (index % 8);
    }
    gc_enqueue(pointer);
}

inline void gc_mark_obj(lispobj thing) {
    if (is_lisp_pointer(thing))
        __mark_obj(thing);
}

static inline void mark_pair(lispobj* where)
{
    gc_mark_obj(where[0]);
    gc_mark_obj(where[1]);
}

void gc_mark_range(lispobj* where, long count) {
    long i;
    for(i=0; i<count; ++i)
        gc_mark_obj(where[i]);
}

#define HT_ENTRY_LIVENESS_FUN_ARRAY_NAME alivep_funs
#include "weak-hash-pred.inc"

static void trace_object(lispobj* where)
{
    lispobj header = *where;
    int widetag = widetag_of(header);
    sword_t scan_from = 1;
    sword_t scan_to = sizetab[widetag](where);
    sword_t i;
    struct weak_pointer *weakptr;
    lispobj layout, bitmap;

    /* If the C compiler emits this switch as a jump table, order doesn't matter.
     * But if as consecutive tests, instance and vector should be tested first
     * as they are the most freequent */
    switch (widetag) {
    case INSTANCE_WIDETAG:
#ifdef LISP_FEATURE_COMPACT_INSTANCE_HEADER
    /* No need to deal with FINs for non-compact header, because the layout
       pointer isn't in the header word, the trampoline pointer can only point
       to readonly space, and all slots are tagged. */
    case FUNCALLABLE_INSTANCE_WIDETAG:
        layout = instance_layout(where);
        gc_mark_obj(layout);
#else
        layout = instance_layout(where); // will be marked as where[1]
#endif
        if (!layout) break; // fall into general case
        // mixed boxed/unboxed objects
        bitmap = ((struct layout*)native_pointer(layout))->bitmap;
        // If no raw slots, just scan without use of the bitmap.
        if (bitmap == make_fixnum(-1)) break;
        for(i=1; i<scan_to; ++i)
            if (layout_bitmap_logbitp(i-1, bitmap) && is_lisp_pointer(where[i]))
                __mark_obj(where[i]);
        return; // do not scan slots
    case SIMPLE_VECTOR_WIDETAG:
        if ((HeaderValue(header) & 0xFF) != subtype_VectorNormal) {
            lispobj lhash_table = where[2];
            gc_dcheck(is_lisp_pointer(lhash_table));
            __mark_obj(lhash_table);
            struct hash_table* hash_table
              = (struct hash_table *)native_pointer(lhash_table);
            if (!hash_table->_weakness) {
                scav_hash_table_entries(hash_table, alivep_funs, mark_pair);
            } else {
                // An object can only be removed from the queue once.
                // Therefore the 'next' pointer has got to be nil.
                gc_assert(hash_table->next_weak_hash_table == NIL);
                hash_table->next_weak_hash_table = (lispobj)weak_hash_tables;
                weak_hash_tables = hash_table;
            }
            return;
        }
        break;
    case CLOSURE_WIDETAG:
        gc_mark_obj(((struct closure*)where)->fun - FUN_RAW_ADDR_OFFSET);
        scan_from = 2;
        break; // scan slots normally
    case CODE_HEADER_WIDETAG:
        for_each_simple_fun(i, fun, (struct code*)where, 0, {
            gc_mark_range(SIMPLE_FUN_SCAV_START(fun),
                          SIMPLE_FUN_SCAV_NWORDS(fun));
        })
        scan_to = code_header_words(header);
        break;
    case FDEFN_WIDETAG:
        gc_mark_obj(fdefn_callee_lispobj((struct fdefn*)where));
        scan_to = 3;
        break;
    case WEAK_POINTER_WIDETAG:
        weakptr = (struct weak_pointer*)where;
        if (is_lisp_pointer(weakptr->value) && interesting_pointer_p(weakptr->value))
            add_to_weak_pointer_list(weakptr);
        return;
    default:
        if (unboxed_obj_widetag_p(widetag)) return;
    }
    for(i=scan_from; i<scan_to; ++i)
        gc_mark_obj(where[i]);
}

void prepare_for_full_mark_phase()
{
    // FIXME: Estimate how large to create mark_bits based on dynamic space size.
    // Guess 8 words per object, and X% of the objects are conses.
    // The problem is guessing how localized the conses are: guess that N conses
    // will reside on fraction*N different pages, which guides us as to how many
    // hash table entries are needed.
    hopscotch_create(&mark_bits, HOPSCOTCH_HASH_FUN_DEFAULT,
                     N_WORD_BYTES, /* table values are machine words */
                     65536, /* initial size */
                     0);

    free_page = page_table_pages;
    suballocator_free_ptr = suballocator_end_ptr = 0;
    struct Qblock* block = (struct Qblock*)get_free_page();
    dprintf(("Queue block holds %d objects\n", (int)QBLOCK_CAPACITY));
    scav_queue.head_block = block;
    scav_queue.tail_block = block;
    scav_queue.recycler   = 0;
    gc_assert(!scav_queue.head_block->count);
}

void execute_full_mark_phase()
{
#if HAVE_GETRUSAGE
    struct rusage before, after;
    getrusage(RUSAGE_SELF, &before);
#endif
    lispobj* where = (lispobj*)STATIC_SPACE_START;
    lispobj* end = static_space_free_pointer;
    while (where < end) {
        lispobj obj = compute_lispobj(where);
        gc_enqueue(obj);
        where += lowtag_of(obj) != LIST_POINTER_LOWTAG
                   ? sizetab[widetag_of(*where)](where) : 2;
    }
 again:
    while (scav_queue.head_block->count) {
        lispobj ptr = gc_dequeue();
        gc_dcheck(ptr != 0);
        if (lowtag_of(ptr) != LIST_POINTER_LOWTAG)
            trace_object(native_pointer(ptr));
        else
            mark_pair((lispobj*)(ptr - LIST_POINTER_LOWTAG));
    }
    if (weak_hash_tables) {
        scav_weak_hash_tables(alivep_funs, mark_pair);
        if (scav_queue.head_block->count) {
            dprintf(("looping due to weak objects\n"));
            goto again;
        }
    }
#if HAVE_GETRUSAGE
    getrusage(RUSAGE_SELF, &after);
#define timediff(b,a,field) \
    (double)((a.field.tv_sec-b.field.tv_sec)*1000000 + \
             (a.field.tv_usec-b.field.tv_usec)) / 1000000.0
    fprintf(stderr,
            "[Mark phase: %d pages used, HT-count=%d, ET=%f+%f sys+usr]\n",
            (int)(page_table_pages - free_page), mark_bits.count,
            timediff(before, after, ru_stime), timediff(before, after, ru_utime));
#endif
}

static void smash_weak_pointers()
{
    struct weak_pointer *wp, *next_wp;
    for (wp = weak_pointers, next_wp = NULL; wp != NULL; wp = next_wp) {
        gc_assert(widetag_of(wp->header)==WEAK_POINTER_WIDETAG);

        next_wp = wp->next;
        wp->next = NULL;
        if (next_wp == wp) /* gencgc uses a ref to self for end of list */
            next_wp = NULL;

        lispobj pointee = wp->value;
        gc_assert(is_lisp_pointer(pointee));
        if (!pointer_survived_gc_yet(pointee))
            wp->value = UNBOUND_MARKER_WIDETAG;
    }
}

__attribute__((unused)) static char *fillerp(lispobj* where)
{
    page_index_t page;
    if (where[0] | where[1])
        return "cons";
    if ((page = find_page_index(where)) >= 0 && page_table[page].large_object)
        return "cons (largeobj filler)";
    return "cons (filler)";
}

FILE *sweeplog;

#ifdef LOG_SWEEP_ACTIONS
# define NOTE_GARBAGE(gen,addr,nwords,tally) \
  { tally[gen] += nwords; \
    if (sweeplog) \
     fprintf(sweeplog, "%5d %d #x%"OBJ_FMTX": %"OBJ_FMTX" %"OBJ_FMTX"\n", \
             (int)nwords, gen, compute_lispobj(addr), \
             addr[0], addr[1]); }
#else
# define NOTE_GARBAGE(gen,addr,nwords,tally) tally[gen] += nwords
#endif

#ifndef LISP_FEATURE_IMMOBILE_SPACE
#define __immobile_obj_gen_bits(x) (lose("No page index?"),0)
#else
static void sweep_fixedobj_pages(long *zeroed)
{
    low_page_index_t page;

    for (page = find_immobile_page_index((void*)(immobile_fixedobj_free_pointer-1));
         page >= 0;
         --page) {
        int obj_spacing = fixedobj_page_obj_align(page);
        if (!obj_spacing)
            continue;
        char *page_base = (char*)IMMOBILE_SPACE_START + page * IMMOBILE_CARD_BYTES;
        int nwords = fixedobj_page_obj_size(page);
        lispobj *obj = (lispobj*)page_base;
        lispobj *limit = (lispobj*)(page_base + IMMOBILE_CARD_BYTES - obj_spacing);
        for ( ; obj <= limit ; obj = (lispobj*)((char*)obj + obj_spacing) ) {
            lispobj header = *obj;
            if (fixnump(header)) { // is a hole
            } else if (header & MARK_BIT) { // live object
                *obj = header ^ MARK_BIT;
            } else {
                NOTE_GARBAGE(__immobile_obj_gen_bits(obj), obj, nwords, zeroed);
                memset(obj, 0, nwords * N_WORD_BYTES);
            }
        }
    }
}
#endif

static uword_t sweep(lispobj* where, lispobj* end, uword_t arg)
{
    long *zeroed = (long*)arg; // one count per generation
    sword_t nwords;

    // TODO: consecutive dead objects on same page should be merged.
    for ( ; where < end ; where += nwords ) {
        lispobj header = *where;
        if (is_cons_half(header)) {
            nwords = 2;
            if (!cons_markedp((lispobj)where)) {
                if (where[0] | where[1]) {
               cons:
                    gc_dcheck(!immobile_space_p((lispobj)where));
                    NOTE_GARBAGE(page_table[find_page_index(where)].gen,
                                 where, 2, zeroed);
                    where[0] = where[1] = 0;
                }
            }
        } else {
            nwords = sizetab[widetag_of(header)](where);
            lispobj markbit =
              widetag_of(header) != BIGNUM_WIDETAG ? MARK_BIT : BIGNUM_MARK_BIT;
            if (header & markbit)
                *where = header ^ markbit;
            else {
                // Turn the object into either a (0 . 0) cons
                // or an unboxed filler depending on size.
                if (nwords <= 2) // could be SAP, SIMPLE-ARRAY-NIL, 1-word bignum, etc
                    goto cons;
                struct code* code  = (struct code*)where;
                lispobj header = 2<<N_WIDETAG_BITS | CODE_HEADER_WIDETAG;
                if (code->header != header) {
                    page_index_t page = find_page_index(where);
                    int gen = page >= 0 ? page_table[page].gen
                      : __immobile_obj_gen_bits(where);
                    NOTE_GARBAGE(gen, where, nwords, zeroed);
                    code->header = header;
                    code->code_size = make_fixnum((nwords - 2) * N_WORD_BYTES);
                    memset(where+2, 0, (nwords - 2) * N_WORD_BYTES);
                }
            }
        }
    }
    return 0;
}

void execute_full_sweep_phase()
{
    long words_zeroed[1+PSEUDO_STATIC_GENERATION]; // One count per generation

    scan_weak_hash_tables(alivep_funs);
    smash_weak_pointers();

#ifdef LOG_SWEEP_ACTIONS
    sweeplog = fopen("/tmp/sweep.log", "a");
    fprintf(sweeplog, "-- begin sweep --\n");
#endif

    memset(words_zeroed, 0, sizeof words_zeroed);
#ifdef LISP_FEATURE_IMMOBILE_SPACE
    sweep_fixedobj_pages(words_zeroed);
    if (sweeplog) fprintf(sweeplog, "-- varyobj pages --\n");
    sweep((lispobj*)IMMOBILE_VARYOBJ_SUBSPACE_START, immobile_space_free_pointer,
          (uword_t)words_zeroed);
#endif
    if (sweeplog) fprintf(sweeplog, "-- dynamic space --\n");
    walk_generation(sweep, -1, (uword_t)words_zeroed);
    fprintf(stderr, "[Sweep phase: ");
    int i;
    for(i=6;i>=0;--i)
        fprintf(stderr, "%ld%s", words_zeroed[i], i?"+":"");
    fprintf(stderr, " words zeroed]\n");
    hopscotch_destroy(&mark_bits);
#ifdef LOG_SWEEP_ACTIONS
    fclose(sweeplog);
    sweeplog = 0;
#endif
}