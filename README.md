# High-Performance Dynamic Memory Allocator

A custom implementation of `malloc`, `free`, `realloc`, and `calloc` in C, achieving 98/100 performance score with 73% memory utilization and 7,700+ KOPS throughput.

## Overview

This project implements a 64-bit dynamic memory allocator using advanced techniques including segregated free lists, mini-block optimization, and immediate coalescing. The allocator is designed to balance memory utilization and throughput across diverse workload patterns.

## Key Features

### 🏗️ **Segregated Free Lists**
- 15 size classes organizing free blocks by size ranges (32 bytes → 262KB+)
- LIFO insertion for O(1) free list operations
- Bounded best-fit search strategy for optimal block selection

### ⚡ **Mini-Block Optimization**
- Specialized 16-byte blocks for small allocations (≤8 bytes)
- Singly-linked list for reduced overhead
- Forward-scanning coalescing algorithm for mini-to-mini merging
- 50% reduction in overhead for small allocations

### 🎯 **Footer Elimination**
- Allocated blocks store only 8-byte headers (no footers)
- 3-bit header encoding: `alloc | prev_alloc | prev_mini`
- Free regular blocks maintain headers + footers for bidirectional coalescing

### 🔄 **Immediate Coalescing**
- Merges adjacent free blocks on every `free()` call
- Supports mini-to-mini, mini-to-regular, and regular-to-regular coalescing
- Minimizes external fragmentation

## Performance

Tested on industry-standard malloc traces:

| Metric | Score |
|--------|-------|
| **Memory Utilization** | 73.4% |
| **Throughput** | 7,719 KOPS |
| **Overall Score** | 98.2/100 |

### Trace Results
```
Trace                  Util    Throughput
-----------------------------------------
bdd-aa4.rep           75.4%    36,665 KOPS
cbit-parity.rep       78.6%    33,697 KOPS
syn-array.rep         91.7%     3,734 KOPS
ngram-gulliver2.rep   58.3%    46,858 KOPS
```

## Implementation Details

### Block Structure

**Regular Allocated Block:**
```
┌────────────────┐
│ Header (8 B)   │  [size | alloc | prev_alloc | prev_mini]
├────────────────┤
│ Payload        │  (user data)
│    ...         │
└────────────────┘
```

**Regular Free Block:**
```
┌────────────────┐
│ Header (8 B)   │
├────────────────┤
│ Next ptr (8 B) │
├────────────────┤
│ Prev ptr (8 B) │
├────────────────┤
│ Unused space   │
├────────────────┤
│ Footer (8 B)   │
└────────────────┘
```

**Mini Block (16 bytes):**
```
┌────────────────┐
│ Header (8 B)   │
├────────────────┤
│ Payload/Next   │  (8 B - dual purpose)
└────────────────┘
```

### Size Classes

| Class | Size Range | Class | Size Range |
|-------|-----------|-------|-----------|
| 0 | 32 B | 8 | 4097-8192 B |
| 1 | 33-64 B | 9 | 8193-16384 B |
| 2 | 65-128 B | 10 | 16385-32768 B |
| 3 | 129-256 B | 11 | 32769-65536 B |
| 4 | 257-512 B | 12 | 65537-131072 B |
| 5 | 513-1024 B | 13 | 131073-262144 B |
| 6 | 1025-2048 B | 14 | 262145+ B |
| 7 | 2049-4096 B | | |

### Algorithms

**Allocation (`malloc`):**
1. Calculate aligned size (asize)
2. Check mini free list if asize ≤ 16 bytes
3. Search segregated lists with bounded best-fit
4. Extend heap if no suitable block found
5. Split block if remainder ≥ 16 bytes
6. Update boundary tags

**Deallocation (`free`):**
1. Mark block as free
2. Update next block's prev_alloc bit
3. Coalesce with adjacent free blocks
4. Insert into appropriate free list

**Coalescing:**
- Check prev_alloc and prev_mini bits to determine if previous block is free
- Check next block's allocation status
- Merge blocks and update size
- Remove merged blocks from free lists
- Insert coalesced block into appropriate size class

## Building and Testing

### Prerequisites
```bash
gcc (version 7.0+)
make
```

### Compilation
```bash
make                 # Build release version
make mdriver-dbg     # Build debug version with assertions
```

### Running Tests
```bash
./mdriver                                    # Run all traces
./mdriver -v                                 # Verbose output
./mdriver -f traces/bdd-aa4.rep             # Run specific trace
./mdriver-dbg -c traces/syn-array-short.rep # Debug mode
```

### Heap Checker
The implementation includes a comprehensive heap checker that validates:
- Block alignment (16-byte boundaries)
- Minimum block sizes
- Header/footer consistency for free blocks
- Coalescing (no consecutive free blocks)
- Free list consistency
- Boundary tag correctness

## File Structure
```
.
├── mm.c              # Main allocator implementation
├── mm.h              # Public interface
├── memlib.c          # Memory system simulator
├── memlib.h          # Memory system interface
├── mdriver.c         # Test driver
├── Makefile          # Build configuration
└── traces/           # Test workloads
    ├── syn-*.rep     # Synthetic traces
    ├── bdd-*.rep     # Binary decision diagram traces
    ├── cbit-*.rep    # Bit manipulation traces
    └── ngram-*.rep   # Natural language traces
```

## Technical Highlights

### Optimization Techniques
1. **Footer Elimination**: Reduces per-block overhead from 16 to 8 bytes for allocated blocks
2. **Mini-Block Specialization**: Handles small allocations efficiently without violating alignment
3. **Bounded Best-Fit**: Searches limited number of blocks to balance fit quality and speed
4. **Immediate Coalescing**: Reduces fragmentation at the cost of free() performance
5. **Bit-Packing**: Encodes 3 metadata bits in header's low bits (exploiting 16-byte alignment)

### Design Trade-offs
- **Immediate vs Deferred Coalescing**: Chose immediate to minimize fragmentation
- **LIFO vs Address-Ordered Lists**: LIFO for O(1) insertion, accepts slight fragmentation increase
- **Splitting Threshold**: Split blocks leaving 16+ byte remainders to balance utilization and performance

## Performance Analysis

### Strengths
- **Large allocations** (syn-array.rep): 91.7% utilization
- **Mixed workloads** (syn-mix.rep): 90.0% utilization
- **High throughput** on pointer-intensive traces: 40K+ KOPS

### Areas for Improvement
- Small allocation traces (ngram-*): 58-62% utilization
- Highly fragmented workloads show room for better coalescing strategies

## Development Process

This allocator was developed iteratively:
1. **Baseline**: Explicit free list → 67% utilization
2. **Footer Removal**: Eliminated footers for allocated blocks → 67% utilization, improved throughput
3. **Mini-Blocks**: Added 16-byte block specialization → 73% utilization
4. **Bounded Best-Fit**: Implemented limited search → 73% utilization, maintained throughput
5. **More Size Classes**: Expanded from 10 to 15 classes → 98/100 final score

## Author

**Joshua Smith**  
Carnegie Mellon University  
Electrical and Computer Engineering

## Course Context

**18-213: Introduction to Computer Systems**  
This project demonstrates low-level memory management, pointer manipulation, and performance optimization in systems programming.

## Acknowledgments

- CMU 18-213 course staff for the test infrastructure and traces
- *Computer Systems: A Programmer's Perspective* (Bryant & O'Hallaron) for foundational concepts

## License

This project is submitted as coursework for Carnegie Mellon University. Code is provided for educational reference only.

---

**Note:** This is an academic project. For production use, consider battle-tested allocators like jemalloc, tcmalloc, or mimalloc.
