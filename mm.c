/**
 * @file mm.c
 * @brief A 64-bit segregated free list memory allocator
 *
 * This allocator uses segregated free lists to manage heap memory efficiently.
 * The implementation provides malloc, free, realloc, and calloc functionality
 * with coalescing to minimize fragmentation.
 *
 * Allocated Block:
 *   - Header (8 bytes): [size | allocated bit]
 *   - Payload (variable size): user data
 *
 * Free Block:
 *   - Header (8 bytes): [size | allocated bit = 0]
 *   - Next pointer (8 bytes): pointer to next free block in size class
 *   - Prev pointer (8 bytes): pointer to previous free block in size class
 *   - Unused space (if block is larger than minimum)
 *
 * Minimum block size is 32 bytes (header + next + prev + footer).
 *
 * SEGREGATED FREE LIST ORGANIZATION:
 * ===================================
 * Free blocks are organized into NUM_CLASSES (10) size classes, where each
 * class maintains a doubly-linked list of free blocks within a size range:
 *
 *   Class 0: 32 bytes
 *   Class 1: 33-64 bytes
 *   Class 2: 65-128 bytes
 *   Class 3: 129-256 bytes
 *   Class 4: 257-512 bytes
 *   Class 5: 513-1024 bytes
 *   Class 6: 1025-2048 bytes
 *   Class 7: 2049-4096 bytes
 *   Class 8: 4097-8192 bytes
 *   Class 9: 8193+ bytes (all larger blocks)
 *
 * Each size class uses LIFO insertion.
 *
 * HEAP STRUCTURE:
 * ===============
 * The heap begins with a prologue (8-byte footer, size=0, allocated=1) and
 * ends with an epilogue (8-byte header, size=0, allocated=1). These boundary
 * tags simplify coalescing by eliminating edge cases.
 *
 * [Prologue Footer] [Block 1] [Block 2] ... [Block N] [Epilogue Header]
 *
 * @author Joshua Smith <joshuas3@andrew.cmu.edu>
 */

#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "memlib.h"
#include "mm.h"

/* Do not change the following! */

#ifdef DRIVER
/* create aliases for driver tests */
#define malloc mm_malloc
#define free mm_free
#define realloc mm_realloc
#define calloc mm_calloc
#define memset mem_memset
#define memcpy mem_memcpy
#endif /* def DRIVER */

/* You can change anything from here onward */

/*
 *****************************************************************************
 * If DEBUG is defined (such as when running mdriver-dbg), these macros      *
 * are enabled. You can use them to print debugging output and to check      *
 * contracts only in debug mode.                                             *
 *                                                                           *
 * Only debugging macros with names beginning "dbg_" are allowed.            *
 * You may not define any other macros having arguments.                     *
 *****************************************************************************
 */
#ifdef DEBUG
/* When DEBUG is defined, these form aliases to useful functions */
#define dbg_requires(expr) assert(expr)
#define dbg_assert(expr) assert(expr)
#define dbg_ensures(expr) assert(expr)
#define dbg_printf(...) ((void)printf(__VA_ARGS__))
#define dbg_printheap(...) print_heap(__VA_ARGS__)
#else
/* When DEBUG is not defined, these should emit no code whatsoever,
 * not even from evaluation of argument expressions.  However,
 * argument expressions should still be syntax-checked and should
 * count as uses of any variables involved.  This used to use a
 * straightforward hack involving sizeof(), but that can sometimes
 * provoke warnings about misuse of sizeof().  I _hope_ that this
 * newer, less straightforward hack will be more robust.
 * Hat tip to Stack Overflow poster chqrlie (see
 * https://stackoverflow.com/questions/72647780).
 */
#define dbg_discard_expr_(...) ((void)((0) && printf(__VA_ARGS__)))
#define dbg_requires(expr) dbg_discard_expr_("%d", !(expr))
#define dbg_assert(expr) dbg_discard_expr_("%d", !(expr))
#define dbg_ensures(expr) dbg_discard_expr_("%d", !(expr))
#define dbg_printf(...) dbg_discard_expr_(__VA_ARGS__)
#define dbg_printheap(...) ((void)((0) && print_heap(__VA_ARGS__)))
#endif

/* Basic constants */

typedef uint64_t word_t;

/** @brief Word and header size (bytes) */
static const size_t wsize = sizeof(word_t);

/** @brief Double word size (bytes) */
static const size_t dsize = 2 * wsize;

/** @brief Minimum block size (bytes) */
static const size_t min_block_size = 2 * dsize;

/**
 * TODO: explain what chunksize is
 * (Must be divisible by dsize)
 */
static const size_t chunksize = (1 << 12);

/**
 * TODO: explain what alloc_mask is
 */
static const word_t alloc_mask = 0x1;
static const word_t prev_alloc_mask = 0x2;
static const word_t prev_mini_mask = 0x4;

/**
 * TODO: explain what size_mask is
 */
static const word_t size_mask = ~(word_t)0xF;

/** @brief Represents the header and payload of one block in the heap */
typedef struct block {
    word_t header;
    union {
        char payload[0]; 
        struct {
            struct block *next;
            struct block *prev;
        };
    };
} block_t;

static word_t *header_to_footer(block_t *block);
static bool get_alloc(block_t *block);
static bool get_mini(block_t *block);

/** @brief Pointer to first block in the heap */
static block_t *heap_start = NULL;
//static block_t *free_list_head = NULL;

#define NUM_CLASSES 15
static block_t *size_class[NUM_CLASSES];

static const size_t mb_block_size = 16;
static const size_t mb_dsize = 8;

static block_t *mini_block_head = NULL;
/*
 *****************************************************************************
 * The functions below are short wrapper functions to perform                *
 * bit manipulation, pointer arithmetic, and other helper operations.        *
 *                                                                           *
 * We've given you the function header comments for the functions below      *
 * to help you understand how this baseline code works.                      *
 *                                                                           *
 * Note that these function header comments are short since the functions    *
 * they are describing are short as well; you will need to provide           *
 * adequate details for the functions that you write yourself!               *
 *****************************************************************************
 */

/*
 * ---------------------------------------------------------------------------
 *                        BEGIN SHORT HELPER FUNCTIONS
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Returns the maximum of two integers.
 * @param[in] x
 * @param[in] y
 * @return `x` if `x > y`, and `y` otherwise.
 */
static size_t max(size_t x, size_t y) {
    return (x > y) ? x : y;
}

/**
 * @brief Rounds `size` up to next multiple of n
 * @param[in] size
 * @param[in] n
 * @return The size after rounding up
 */
static size_t round_up(size_t size, size_t n) {
    return n * ((size + (n - 1)) / n);
}

/**
 * @brief Returns the allocation status of a given header value.
 *
 * This is based on the lowest bit of the header value.
 *
 * @param[in] word
 * @return The allocation status correpsonding to the word
 */
static bool extract_alloc(word_t word) {
    return (bool)(word & alloc_mask);
}

/**
 * @brief Returns the allocation status of a block, based on its header.
 * @param[in] block
 * @return The allocation status of the block
 */
static bool get_alloc(block_t *block) {
    return extract_alloc(block->header);
}

/**
 * @brief Returns the allocation status of the previous block
 * 
 * This reads bit 1 of the current block's header to determine
 * if the previous block in memory is allocated.
 *
 * @param[in] block
 * @return True if previous block is allocated, false if free
 */
static bool get_prev_alloc(block_t *block) {
    return (bool)((block->header >> 1) & 0x1);
}

/**
 * @brief Clears the prev_alloc bit to mark previous block as free
 * 
 * This clears bit 1 of the current block's header to indicate
 * the previous block in memory is free.
 *
 * @param[in] block
 */
static void clear_prev_alloc(block_t *block){
    block->header &= ~prev_alloc_mask;
}

/**
 * @brief Sets the prev_alloc bit to mark previous block as allocated
 * 
 * This sets bit 1 of the current block's header to indicate
 * the previous block in memory is allocated.
 *
 * @param[in] block
 */
static void set_prev_alloc(block_t *block){
    block->header |= prev_alloc_mask;
}

/**
 * @brief Extracts the size represented in a packed word.
 *
 * This function simply clears the lowest 4 bits of the word, as the heap
 * is 16-byte aligned.
 *
 * @param[in] word
 * @return The size of the block represented by the word
 */
static size_t extract_size(word_t word) {
    return (word & size_mask);
}

/**
 * @brief Extracts the size of a block from its header.
 * @param[in] block
 * @return The size of the block
 */
static size_t get_size(block_t *block) {
    return extract_size(block->header);
}

/**
 * @brief Returns whether a block is a mini block
 * 
 * A mini block is 16 bytes (8-byte header + 8-byte payload/next pointer).
 * This function checks if the block's size equals the mini block size.
 *
 * @param[in] block
 * @return True if block is mini (16 bytes), false otherwise
 */
static bool get_mini(block_t *block){
    return (bool)(get_size(block) == mb_block_size);
}

/**
 * @brief Returns whether the previous block is a mini block
 * 
 * This reads bit 3 of the current block's header.
 *
 * @param[in] block
 * @return True if previous block is mini, false otherwise
 */
static bool get_prev_mini(block_t *block) {
    return (bool)((block->header >> 2) & 0x1);
}

/**
 * @brief Sets the prev_is_mini bit to indicate previous block is mini
 * 
 * Updates both header and footer (if block is free and regular).
 *
 * @param[in] block
 */
static void set_prev_mini(block_t *block) {
    block->header |= prev_mini_mask;
    
    if (!get_alloc(block) && !get_mini(block)) {
        word_t *footer = header_to_footer(block);
        *footer |= prev_mini_mask;
    }
}

/**
 * @brief Clears the prev_is_mini bit to indicate previous block is not mini
 * 
 * Updates both header and footer (if block is free and regular).
 *
 * @param[in] block
 */
static void clear_prev_mini(block_t *block) {
    block->header &= ~prev_mini_mask;
    
    if (!get_alloc(block) && !get_mini(block)) {
        word_t *footer = header_to_footer(block);
        *footer &= ~prev_mini_mask;
    }
}

/**
 * @brief Packs the `size` and `alloc` of a block into a word suitable for
 *        use as a packed value.
 *
 * Packed values are used for both headers and footers.
 *
 * The allocation status is packed into the lowest bit of the word.
 *
 * @param[in] size The size of the block being represented
 * @param[in] alloc True if the block is allocated
 * @return The packed value
 */
static word_t pack(size_t size, bool alloc, bool prev_alloc, bool prev_mini) {
    word_t word = size;
    if (alloc) {
        word |= alloc_mask;
    }
    if (prev_alloc) {
        word |= (alloc_mask << 1);
    }
    if (prev_mini){
        word |= (alloc_mask << 2);
    }
    return word;
}

/**
 * @brief Given a payload pointer, returns a pointer to the corresponding
 *        block.
 * @param[in] bp A pointer to a block's payload
 * @return The corresponding block
 */
static block_t *payload_to_header(void *bp) {
    return (block_t *)((char *)bp - offsetof(block_t, payload));
}

/**
 * @brief Given a block pointer, returns a pointer to the corresponding
 *        payload.
 * @param[in] block
 * @return A pointer to the block's payload
 * @pre The block must be a valid block, not a boundary tag.
 */
static void *header_to_payload(block_t *block) {
    dbg_requires(get_size(block) != 0);
    return (void *)(block->payload);
}

/**
 * @brief Given a block pointer, returns a pointer to the corresponding
 *        footer.
 * @param[in] block
 * @return A pointer to the block's footer
 * @pre The block must be a valid block, not a boundary tag.
 */
static word_t *header_to_footer(block_t *block) {
    dbg_requires(get_size(block) != 0 &&
                 "Called header_to_footer on the epilogue block");
    return (word_t *)(block->payload + get_size(block) - dsize);
}

/**
 * @brief Given a block footer, returns a pointer to the corresponding
 *        header.
 * @param[in] footer A pointer to the block's footer
 * @return A pointer to the start of the block
 * @pre The footer must be the footer of a valid block, not a boundary tag.
 */
static block_t *footer_to_header(word_t *footer) {
    size_t size = extract_size(*footer);
    dbg_assert(size != 0 && "Called footer_to_header on the prologue block");
    return (block_t *)((char *)footer + wsize - size);
}

/**
 * @brief Returns the payload size of a given block.
 *
 * The payload size is equal to the entire block size minus the sizes of the
 * block's header and footer.
 *
 * @param[in] block
 * @return The size of the block's payload
 */
static size_t get_payload_size(block_t *block) {
    size_t asize = get_size(block);
    if(!get_alloc(block)) return asize - dsize;
    else                  return asize - wsize;
}

/**
 * @brief Writes an epilogue header at the given address.
 *
 * The epilogue header has size 0, and is marked as allocated.
 *
 * @param[out] block The location to write the epilogue header
 */
static void write_epilogue(block_t *block) {
    dbg_requires(block != NULL);
    dbg_requires((char *)block == (char *)mem_heap_hi() - 7);
    block->header = pack(0, true, false, false);
}

/**
 * @brief Writes a block starting at the given address.
 *
 * This function writes both a header and footer, where the location of the
 * footer is computed in relation to the header.
 *
 * TODO: Are there any preconditions or postconditions?
 *
 * @param[out] block The location to begin writing the block header
 * @param[in] size The size of the new block
 * @param[in] alloc The allocation status of the new block
 */
static void write_block(block_t *block, size_t size, bool alloc, bool prev_alloc, bool prev_mini) {
    dbg_requires(block != NULL);
    dbg_requires(size > 0);
    block->header = pack(size, alloc, prev_alloc, prev_mini);
    if(!alloc && (size != mb_block_size)){
        word_t *footerp = header_to_footer(block);
        *footerp = pack(size, alloc, prev_alloc, prev_mini);
    }
}

/**
 * @brief Finds the next consecutive block on the heap.
 *
 * This function accesses the next block in the "implicit list" of the heap
 * by adding the size of the block.
 *
 * @param[in] block A block in the heap
 * @return The next consecutive block on the heap
 * @pre The block is not the epilogue
 */
static block_t *find_next(block_t *block) {
    dbg_requires(block != NULL);
    dbg_requires(get_size(block) != 0 &&
                 "Called find_next on the last block in the heap");
    return (block_t *)((char *)block + get_size(block));
}

/**
 * @brief Finds the footer of the previous block on the heap.
 * @param[in] block A block in the heap
 * @return The location of the previous block's footer
 */
static word_t *find_prev_footer(block_t *block) {
    // Compute previous footer position as one word before the header
    return &(block->header) - 1;
}

/**
 * @brief Finds the previous consecutive block on the heap.
 *
 * This is the previous block in the "implicit list" of the heap.
 *
 * If the function is called on the first block in the heap, NULL will be
 * returned, since the first block in the heap has no previous block!
 *
 * The position of the previous block is found by reading the previous
 * block's footer to determine its size, then calculating the start of the
 * previous block based on its size.
 *
 * @param[in] block A block in the heap
 * @return The previous consecutive block in the heap.
 */
static block_t *find_prev(block_t *block) {
    dbg_requires(block != NULL);
    dbg_requires(!get_prev_alloc(block));

    // Check if previous block is a mini block
    if (get_prev_mini(block)) {
        // Previous is a FREE MINI block - no footer, just go back 16 bytes
        block_t *prev = (block_t *)((char *)block - mb_block_size);
        
        dbg_assert(get_size(prev) == mb_block_size);
        dbg_assert(!get_alloc(prev));
        
        return prev;
    } else {
        // Previous is a FREE REGULAR block - has footer
        word_t *footerp = find_prev_footer(block);
        
        // Return NULL if called on first block in the heap
        if (extract_size(*footerp) == 0) {
            return NULL;
        }
        
        block_t *prev = footer_to_header(footerp);
        
        dbg_assert(get_size(prev) >= min_block_size);
        dbg_assert(!get_alloc(prev));
        
        return prev;
    }
}

/*
 * ---------------------------------------------------------------------------
 *                        END SHORT HELPER FUNCTIONS
 * ---------------------------------------------------------------------------
 */

/******** The remaining content below are helper and debug routines ********/

/**
 * @brief Maps block size to size class index using power-of-2 ranges
 *
 * @param[in] size The size of the block in bytes
 * @return The size class index (0 to NUM_CLASSES-1)
 */
static int size_to_class(size_t size){
    if (size <= 32)      return 0;
    if (size <= 64)      return 1;
    if (size <= 128)     return 2;
    if (size <= 256)     return 3;
    if (size <= 512)     return 4;
    if (size <= 1024)    return 5;
    if (size <= 2048)    return 6;
    if (size <= 4096)    return 7;
    if (size <= 8192)    return 8;
    if (size <= 16384)   return 9;
    if (size <= 32768)   return 10;
    if (size <= 65536)   return 11;
    if (size <= 131072)  return 12;
    if (size <= 262144)  return 13;
    else                 return 14;
}

/**
 * @brief Inserts a free block at head of its size class list (LIFO)
 *
 * @param[in] block Pointer to the free block to insert
 */
static void add_to_free_list(block_t *block){
    dbg_requires(block != NULL);
    dbg_requires(!get_alloc(block));

    int class = size_to_class(get_size(block));

    block->next = size_class[class];
    block->prev = NULL;

    if (size_class[class] != NULL) {
        size_class[class]->prev = block;
    }
    
    size_class[class] = block;
}

/**
 * @brief Removes a free block from its size class list
 *
 * @param[in] block Pointer to the free block to remove
 */
static void rem_from_free_list(block_t *block){
    dbg_requires(block != NULL);
    dbg_requires(!get_alloc(block));

    block_t *old_prev = block -> prev;
    block_t *old_next = block -> next;

    int class = size_to_class(get_size(block));    

    if(old_prev == NULL && old_next == NULL){
        // NULL <-> __block__ <-> NULL
        size_class[class] = NULL;
    } else if(old_prev != NULL && old_next != NULL){
        // block <-> __block__ <-> block
        old_prev -> next = old_next;
        old_next -> prev = old_prev;
    } else if(old_prev != NULL && old_next == NULL){
        // block <-> __block__ <-> NULL
        old_prev -> next = NULL;
    } else if(old_prev == NULL && old_next != NULL){
        // NULL <-> __block__ <-> block
        old_next -> prev = NULL;
        size_class[class] = old_next;
    }
}

/**
 * @brief Removes a free mini block from the mini block free list
 * 
 * This function searches for and unlinks the specified block from the
 * singly-linked mini block free list. If the block is at the head,
 * it updates the head pointer. Otherwise, it searches the list and
 * unlinks the block from its predecessor.
 *
 * @param[in] block Pointer to the mini block to remove
 * @pre block must be free, mini, and in the mini_block_head list
 */
static void rem_from_mini_list(block_t *block){
    dbg_requires(block != NULL);
    dbg_requires(mini_block_head != NULL);
    dbg_requires(!get_alloc(block));
    dbg_requires(get_mini(block));

    if (block == mini_block_head) {
        mini_block_head = mini_block_head->next;
        return;
    }

    block_t *current = mini_block_head;
    while (current != NULL && current->next != block) {
        current = current->next;
    }
    
    if (current != NULL && current->next == block) {
        current->next = block->next;
    }

}

/**
 * @brief Inserts a free mini block at head of mini block free list (LIFO)
 * 
 * This function adds the specified mini block to the front of the
 * singly-linked mini block free list.
 *
 * @param[in] block Pointer to the free mini block to insert
 * @pre block must be free and mini (16 bytes)
 */
static void add_to_mini_list(block_t *block){
    dbg_requires(block != NULL);
    dbg_requires(!get_alloc(block));
    dbg_requires(get_mini(block));

    block -> next = mini_block_head;
    mini_block_head = block;
}

/**
 * @brief Merges a free block with adjacent free blocks to reduce fragmentation
 *
 * @param[in] block Pointer to the newly freed block
 * @return Pointer to the coalesced block
 */
static block_t *coalesce_block(block_t *block) {
    dbg_requires(block != NULL);
    dbg_requires(!get_alloc(block));

    block_t *next = find_next(block);
    block_t *prev = NULL;
    if(!get_prev_alloc(block)){
        prev = find_prev(block);
    }

    bool prev_alloced = get_prev_alloc(block) || (prev == NULL);
    bool next_alloced = (next == NULL) || get_alloc(next);

    bool free_free_free = !prev_alloced && !next_alloced;
    bool free_free_alloced = !prev_alloced && next_alloced;
    bool alloced_free_free = prev_alloced && !next_alloced;
    bool alloced_free_alloced = prev_alloced && next_alloced;

    size_t size = get_size(block);

    if(free_free_free){
        size += get_size(prev);
        size += get_size(next);

        if(get_mini(prev)) rem_from_mini_list(prev);
        else               rem_from_free_list(prev);

        if(get_mini(next)) rem_from_mini_list(next);
        else               rem_from_free_list(next);

        write_block(prev, size, false, get_prev_alloc(prev), get_prev_mini(prev));

        clear_prev_alloc(find_next(prev));
        clear_prev_mini(find_next(prev));

        return prev;
    } else if(free_free_alloced){
        size += get_size(prev);
        if(get_mini(prev)) rem_from_mini_list(prev);
        else               rem_from_free_list(prev);

        write_block(prev, size, false, get_prev_alloc(prev), get_prev_mini(prev));

        clear_prev_alloc(find_next(prev));
        clear_prev_mini(find_next(prev));

        return prev;
    } else if(alloced_free_free){
        size += get_size(next);
        if(get_mini(next)) rem_from_mini_list(next);
        else               rem_from_free_list(next);

        write_block(block, size, false, get_prev_alloc(block), get_prev_mini(block));

        clear_prev_alloc(find_next(block));
        clear_prev_mini(find_next(block));
        
        return block;
    } else if(alloced_free_alloced){
        clear_prev_alloc(next);
        if(get_mini(block)) set_prev_mini(next);
        else                clear_prev_mini(next);
        return block;
    }
    return block;
}

/**
 * @brief Extends heap when no suitable free block exists
 *
 * @param[in] size Number of bytes to request (rounded to alignment)
 * @return Pointer to the new free block, or NULL on failure
 */
static block_t *extend_heap(size_t size) {
    void *bp;

    // Allocate an even number of words to maintain alignment
    size = round_up(size, dsize);
    if ((bp = mem_sbrk((intptr_t)size)) == (void *)-1) {
        return NULL;
    }

    // Initialize free block header/footer
    block_t *block = payload_to_header(bp);
    write_block(block, size, false, get_prev_alloc(block), get_prev_mini(block));

    // Create new epilogue header
    block_t *block_next = find_next(block);
    write_epilogue(block_next);

    // Coalesce in case the previous block was free
    block = coalesce_block(block);
    add_to_free_list(block);

    return block;
}

// done
/**
 * @brief Splits a block if remainder is large enough to be useful
 *
 * @param[in] block Pointer to block to split
 * @param[in] asize Desired allocated size (aligned)
 */
static void split_block(block_t *block, size_t asize) {
    dbg_requires(!get_alloc(block));
    dbg_requires(!get_mini(block));
    dbg_requires(asize >= mb_block_size);
    dbg_requires(asize % dsize == 0);

    size_t block_size = get_size(block);
    size_t remainder = block_size - asize;

    // block_t *block, size_t size, bool alloc, bool prev_alloc, bool prev_mini
    if (asize == mb_block_size) {
        // ALLOCATING first part as MINI
        if (remainder >= min_block_size) {
            // CASE: alloc mini | free regular
            write_block(block, mb_block_size, true, get_prev_alloc(block), get_prev_mini(block));

            block_t *block_next = find_next(block);
            write_block(block_next, remainder, false, true, true);
            add_to_free_list(block_next);

            clear_prev_alloc(find_next(block_next));
            clear_prev_mini(find_next(block_next));
        } 
        else if (remainder == mb_block_size) {
            // CASE: alloc mini | free mini
            write_block(block, mb_block_size, true, get_prev_alloc(block), get_prev_mini(block));

            block_t *block_next = find_next(block);
            write_block(block_next, mb_block_size, false, true, true);
            add_to_mini_list(block_next);
            
            clear_prev_alloc(find_next(block_next));
            set_prev_mini(find_next(block_next));
        } 
        else {
            write_block(block, block_size, true, get_prev_alloc(block), get_prev_mini(block));
            set_prev_alloc(find_next(block));
            set_prev_mini(find_next(block));
        }
        
    } else {
        // ALLOCATING first part as REGULAR
        if (remainder >= min_block_size) {
            // CASE: alloc regular | free reg
            write_block(block, asize, true, get_prev_alloc(block), get_prev_mini(block));

            block_t *block_next = find_next(block);
            write_block(block_next, remainder, false, true, false);
            add_to_free_list(block_next);

            clear_prev_alloc(find_next(block_next));
            clear_prev_mini(find_next(block_next));
        } 
        else if (remainder == mb_block_size) {
            // CASE: alloc regular | Create 16 mini free
            write_block(block, asize, true, get_prev_alloc(block), get_prev_mini(block));

            block_t *block_next = find_next(block);
            write_block(block_next, mb_block_size, false, true, false);
            add_to_mini_list(block_next);
            
            clear_prev_alloc(find_next(block_next));
            set_prev_mini(find_next(block_next));
        } 
        else {
            write_block(block, block_size, true, get_prev_alloc(block), get_prev_mini(block));
            set_prev_alloc(find_next(block));
            clear_prev_mini(find_next(block));
        }
    }
    dbg_ensures(get_alloc(block));
}

// done
/**
 * @brief Searches size classes for a block large enough for request
 *
 * @param[in] asize Required size (aligned)
 * @return Pointer to suitable free block, or NULL if none found
 */
static block_t *find_fit(size_t asize) {
    block_t *block;
    
    if(asize <= mb_block_size){
        if(mini_block_head != NULL){
            return mini_block_head;
        }
    }

    int class = size_to_class(asize);
    
    if (size_class[class] != NULL) {
        for(block = size_class[class]; block != NULL; block = block->next){
            if(asize <= get_size(block)){
                return block;
            }
        }
    }

    for(int i = class + 1; i < NUM_CLASSES; i++){
        block_t *best = NULL;
        size_t best_size = SIZE_MAX;
        int search_count = 0;
        const int MAX_SEARCH = 10;
        
        for(block = size_class[i]; block != NULL && search_count < MAX_SEARCH; block = block->next){
            if(asize <= get_size(block)){
                if(get_size(block) < best_size){
                    best = block;
                    best_size = get_size(block);
                }
            }
            search_count++;
        }
        
        if(best != NULL) return best;
    }
    
    return NULL;
}

// done
/**
 * @brief Validates heap invariants and free list consistency
 *
 * @param[in] line Source line number for debugging
 * @return true if heap is valid, false if corruption detected
 */
bool mm_checkheap(int line) {
    
    word_t *prologue = (word_t *)heap_start - 1;
    if (*prologue != pack(0, true, true, false)) {
        printf("ERROR (line %d): Prologue corrupted\n", line);
        return false;
    }

    int totalFreed = 0;
    int trackedFreed = 0;
    int totalAllocated = 0;

    block_t *block;
    block_t *prev_block = NULL;
    for(block = heap_start; get_size(block) > 0; block = find_next(block)){
        // [ASSERT] block size is multiple of 16
        if(get_size(block) % 16 != 0){
            printf("ERROR (line %d): Block %p not aligned\n", line, (void*)block);
            return false;
        } 
        
        // [ASSERT] block is not too small
        if(get_size(block) < mb_block_size){
            printf("ERROR (line %d): Block %p size is too small\n", line, (void*)block);
            return false;
        } 
        
        // [ASSERT] block in bounds
        if((char*)block < (char*)mem_heap_lo() || 
                  (char*)block > (char*)mem_heap_hi()){
            printf("ERROR (line %d): Block %p outside heap\n", line, (void*)block);
            return false;
        } 
        
        // [ASSERT] get_alloc(prev) matches get_prev_alloc(curr)
        if(prev_block != NULL ){
            if(get_alloc(prev_block) && !get_prev_alloc(block)){
                printf("ERROR (line %d): get_alloc(prev) = 1 but get_prev_alloc(curr) = 0!\n", line);
                return false;
            }
            if(!get_alloc(prev_block) && get_prev_alloc(block)){
                printf("ERROR (line %d): get_alloc(prev) = 0 but get_prev_alloc(curr) = 1!\n", line);
                return false;
            }
        }
        
        // [ASSERT] no consecutive free blocks in heap
        if(!get_alloc(block)){
            block_t *next = find_next(block);
            if(!get_alloc(next) && get_size(next) > 0){
                printf("ERROR (line %d): Block %p has not been coalesced\n", line, (void*)block);
                return false;
            }
        }
        
        // [ASSERT] free block footer matches prev header
        if(!get_alloc(block) && !get_mini(block)){
            if(block->header != *header_to_footer(block)){
                printf("ERROR (line %d): Header/footer mismatch at %p\n", 
                    line, (void*)block);
                return false;
            }
        }
        

        if(!get_alloc(block)) totalFreed++;
        else totalAllocated++;
        prev_block = block;
    }

    // [ASSERT] epilogue is ok
    block_t *epi = block;
    if( !(get_size(epi) == 0 && get_alloc(epi) == 1) ){
        printf("ERROR epilogue wrong");
    }

    for(int i = 0; i < NUM_CLASSES; i++){
        for(block_t *block = size_class[i]; block != NULL; block = block -> next){
            trackedFreed++;
        }
    }
    for(block_t *block = mini_block_head; block != NULL; block = block -> next){
        trackedFreed++;
    }
    
    // [ASSERT] blocks in free list match free blocks in heap
    if(trackedFreed != totalFreed){
        dbg_printf("ERROR (line %d): free list blocks and total free blocks mismatch \n", line);
        dbg_printf("tracked free: %d\n", trackedFreed);
        dbg_printf("actual free: %d\n", totalFreed);
        dbg_printf("actual allocated: %d\n", totalAllocated);
        return false;
    }
    
    return true;
}

// done
/**
 * @brief Initializes the allocator with empty heap and free lists
 *
 * @return true if successful, false if mem_sbrk fails
 */
bool mm_init(void) {
    // Create the initial empty heap
    word_t *start = (word_t *)(mem_sbrk(2 * wsize));

    if (start == (void *)-1) {
        return false;
    }

    start[0] = pack(0, true, true, false); // Heap prologue (block footer)
    start[1] = pack(0, true, true, false); // Heap epilogue (block header)

    for(int i = 0; i < NUM_CLASSES; i++){
        size_class[i] = NULL;
    }

    mini_block_head = NULL;
    // Heap starts with first "block header", currently the epilogue
    heap_start = (block_t *)&(start[1]);
    //free_list_head = NULL;
    // Extend the empty heap with a free block of chunksize bytes
    if (extend_heap(chunksize) == NULL) {
        return false;
    }

    return true;
}

/**
 * @brief Allocates a block of at least the requested size
 *
 * @param[in] size Number of bytes requested
 * @return Pointer to allocated payload, or NULL on failure
 */
void *malloc(size_t size) {
    dbg_requires(mm_checkheap(__LINE__));

    size_t asize;
    size_t extendsize;
    block_t *block;
    void *bp = NULL;

    // Initialize heap if it isn't initialized
    if (heap_start == NULL) {
        if (!(mm_init())) {
            dbg_printf("Problem initializing heap. Likely due to sbrk");
            return NULL;
        }
    }

    // Ignore spurious request
    if (size == 0) {
        dbg_ensures(mm_checkheap(__LINE__));
        return bp;
    }

    // Adjust block size to include overhead and to meet alignment requirements
    if(size <= mb_dsize){
        asize = mb_block_size;
    } else{
        asize = max(round_up(size + wsize, dsize), min_block_size);
    }
    
    block = find_fit(asize);
    // CASE: allocate a mini-block and there is space in the mini-list
    if(asize == mb_block_size && block != NULL && get_mini(block)){
        rem_from_mini_list(block);

        write_block(block, mb_block_size, true, get_prev_alloc(block), get_prev_mini(block));
        
        set_prev_alloc(find_next(block));
        set_prev_mini(find_next(block));
        
        bp = header_to_payload(block);
        dbg_ensures(mm_checkheap(__LINE__));
        return bp;
    }

    // If no fit is found, request more memory, and then and place the block
    if (block == NULL) {
        // Always request at least chunksize
        extendsize = max(asize, chunksize);
        block = extend_heap(extendsize);
        // extend_heap returns an error
        if (block == NULL) {
            return bp;
        }
    }

    // The block should be marked as free
    dbg_assert(!get_alloc(block));

    // Try to split the block if too large
    rem_from_free_list(block);
    split_block(block, asize);

    bp = header_to_payload(block);

    dbg_ensures(mm_checkheap(__LINE__));
    return bp;
}

/**
 * @brief Frees a block, coalesces neighbors, and adds to free list
 *
 * @param[in] bp Pointer to payload (from malloc/realloc/calloc)
 */
void free(void *bp) {
    dbg_requires(mm_checkheap(__LINE__));

    if (bp == NULL) {
        return;
    }

    block_t *block = payload_to_header(bp);
    size_t size = get_size(block);

    // The block should be marked as allocated
    dbg_assert(get_alloc(block));

    // Mark the block as free
    write_block(block, size, false, get_prev_alloc(block), get_prev_mini(block));

    // Try to coalesce the block with its neighbors
    block = coalesce_block(block);
    if(get_mini(block)) add_to_mini_list(block);
    else                add_to_free_list(block);

    dbg_ensures(mm_checkheap(__LINE__));
}

/**
 * @brief Reallocates a block to a new size
 *
 * @param[in] ptr Pointer to old block
 * @param[in] size New size in bytes
 * @return Pointer to new block, or NULL on failure
 */
void *realloc(void *ptr, size_t size) {
    block_t *block = payload_to_header(ptr);
    size_t copysize;
    void *newptr;

    // If size == 0, then free block and return NULL
    if (size == 0) {
        free(ptr);
        return NULL;
    }

    // If ptr is NULL, then equivalent to malloc
    if (ptr == NULL) {
        return malloc(size);
    }

    // Otherwise, proceed with reallocation
    newptr = malloc(size);

    // If malloc fails, the original block is left untouched
    if (newptr == NULL) {
        return NULL;
    }

    // Copy the old data
    copysize = get_payload_size(block); // gets size of old payload
    if (size < copysize) {
        copysize = size;
    }
    memcpy(newptr, ptr, copysize);

    // Free the old block
    free(ptr);

    return newptr;
}

/**
 * @brief Allocates and zero-initializes an array
 *
 * @param[in] elements Number of elements
 * @param[in] size Size of each element in bytes
 * @return Pointer to zeroed memory, or NULL on failure
 */
void *calloc(size_t elements, size_t size) {
    void *bp;
    size_t asize = elements * size;

    if (elements == 0) {
        return NULL;
    }
    if (asize / elements != size) {
        // Multiplication overflowed
        return NULL;
    }

    bp = malloc(asize);
    if (bp == NULL) {
        return NULL;
    }

    // Initialize all bits to 0
    memset(bp, 0, asize);

    return bp;
}

/*
 *****************************************************************************
 * Do not delete the following super-secret(tm) lines!                       *
 *                                                                           *
 * 53 6f 20 79 6f 75 27 72 65 20 74 72 79 69 6e 67 20 74 6f 20               *
 *                                                                           *
 * 66 69 67 75 72 65 20 6f 75 74 20 77 68 61 74 20 74 68 65 20               *
 * 68 65 78 61 64 65 63 69 6d 61 6c 20 64 69 67 69 74 73 20 64               *
 * 6f 2e 2e 2e 20 68 61 68 61 68 61 21 20 41 53 43 49 49 20 69               *
 *                                                                           *
 * 73 6e 27 74 20 74 68 65 20 72 69 67 68 74 20 65 6e 63 6f 64               *
 * 69 6e 67 21 20 4e 69 63 65 20 74 72 79 2c 20 74 68 6f 75 67               *
 * 68 21 20 2d 44 72 2e 20 45 76 69 6c 0a c5 7c fc 80 6e 57 0a               *
 *                                                                           *
 *****************************************************************************
*/
