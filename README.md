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

### Trace Results
