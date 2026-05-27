/**
 * @file Me.c
 * @brief 基于句柄的紧凑型堆内存管理器
 * 
 * @design_overview
 * 本系统实现了一个支持**内存压缩**和**碎片整理**的堆管理器。
 * 通过引入"句柄（Handle）"间接层，实现了逻辑引用与物理地址的解耦，
 * 使得内存块在堆中移动时，用户持有的句柄依然有效。
 * 
 * @architecture 三层架构模型
 * ```
 * User Code
 *    │
 *    ▼
 * Handle (MemHandle)
 *    │
 *    ▼
 * Slot Table
 *    │
 *    └─> slots[index].block_ptr
 *          │
 *          ▼
 *      BlockHeader
 *      [magic|data_size|is_free|slot*|next*|data...]
 * ```
 * 
 * @key_features
 * - **句柄抽象**: 用户持有整数句柄而非裸指针，避免悬空指针
 * - **首次适配**: First-Fit分配算法，平衡性能与碎片率
 * - **惰性合并**: free时合并相邻空闲块，降低复杂度
 * - **紧凑压缩**: 双指针扫描复制算法消除外部碎片
 * - **完整性校验**: Magic Number检测堆腐败
 * - **8字节对齐**: 优化CPU缓存性能和硬件兼容性
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* ==================== 常量与类型定义 ==================== */

/** @brief 内存句柄类型（本质是Slot表的索引） 别名 */
typedef uint32_t MemHandle;

#define INVALID_HANDLE 0xFFFFFFFF   /**< 无效句柄标识 */
#define MAX_SLOTS 1024              /**< Slot表容量：最多同时存在1024个内存块 */
#define HEAP_SIZE (1024 * 1024)     /**< 默认堆大小：1MB */

/**
 * @brief 8字节对齐宏
 * @param x 待对齐的数值
 * @return 向上对齐到8的倍数
 * @example ALIGN_8(5) = 8, ALIGN_8(13) = 16, ALIGN_8(16) = 16
 */
#define ALIGN_8(x) (((x) + 7) & ~7)

#define BLOCK_MAGIC 0xDEADBEEF      /**< 魔数：用于检测内存块合法性 */
#define MIN_SPLIT_SIZE 8            /**< 最小分割阈值：剩余空间小于此值不再切割 */

/* ==================== 数据结构定义 ==================== */

/**
 * @struct BlockHeader
 * @brief 内存块头部元数据（32位系统24字节，64位系统32字节，8字节对齐）
 * 
 * @memory_layout 内存块逻辑布局
 * ```
 * Header  [ magic | data_size | is_free | slot* | next* ]  <- 32字节(64位系统)
 * Payload [ data_size 字节数据 ]
 * ```
 * 
 * @field magic       魔数标识，验证块合法性，防止越界写入
 * @field data_size   用户数据区大小（不含头部）
 * @field is_free     空闲标志：true=空闲，false=已分配
 * @field slot        指向所属 Slot 的反向指针，释放后为 NULL
 * @field next        指向链表中的下一个块
 * @field data        用户数据区起点，柔性数组形式存放实际负载
 * 
 * @design_note
 * - 块链表由 next 指针显式维护，避免依赖隐式地址计算
 * - slot 指针保持块和 Slot 之间的反向引用关系
 * - data[] 是柔性数组，数据紧跟块头部，无需额外尾部指针或独立 payload 结构
 * - __attribute__((aligned(8))) 保证 data[] 起始地址 8 字节对齐
 */
typedef struct __attribute__((aligned(8))) BlockHeader
{
    uint32_t magic;       /**< 魔数校验字段 */
    uint32_t data_size;   /**< 用户数据区大小（payload size） */
    uint32_t is_free;     /**< 空闲状态标志 */
    struct Slot *slot;    /**< 指向对应Slot的反向指针 */
    struct BlockHeader *next; /**< 指向下一个块的链表指针 */
    uint8_t data[];       /**< 柔性数组：用户数据区起点 */
} BlockHeader;

/**
 * @struct Slot
 * @brief 句柄映射表项（间接层的核心）
 * 
 * @field is_active  槽位激活状态：true=已被分配，false=空闲
 * @field block_ptr  指向对应的BlockHeader（解引用的关键桥梁）
 * 
 * @design_pattern 间接层模式
 * ```
 * Handle (uint32_t) ──► Slot[index] ──► BlockHeader* ──► 实际内存
 * ```
 * - **分配时**: 建立 handle → slot → block 的映射链
 * - **释放时**: 仅标记is_active=false，保留block_ptr便于调试
 * - **整理时**: 更新block_ptr指向新位置，保持handle有效性
 * 
 * @advantage
 * 即使物理块在堆中移动（memmove），上层代码只需持有handle，
 * 通过mem_deref()即可获取最新的正确地址，无需修改任何指针。
 */
typedef struct Slot
{
    bool is_active;         /**< 槽位是否被占用 */
    BlockHeader *block_ptr; /**< 指向物理内存块的指针 */
} Slot;

/**
 * @struct HeapManager
 * @brief 堆管理器全局状态（单例模式）
 * 
 * @field heap_start  堆内存起始地址（由malloc分配的连续空间）
 * @field total_size  堆总大小（字节）
 * @field slots       Slot映射表（MAX_SLOTS个槽位）
 * 
 * @design_note
 * 采用静态全局变量g_manager，整个程序只有一个堆实例。
 * 这种设计简化了API调用（无需传递管理器指针），但不支持多堆并发。
 */
typedef struct
{
    uint8_t *heap_start;           /**< 堆基地址 */
    uint32_t total_size;           /**< 堆总容量 */
    Slot slots[MAX_SLOTS];         /**< 句柄映射表 */
} HeapManager;

/** @brief 全局堆管理器实例（单例） */
static HeapManager g_manager;

/* ==================== 内部辅助函数 ==================== */

/**
 * @brief 申请一个空闲Slot槽位
 * 
 * @param block 要关联的内存块指针
 * @return MemHandle 分配的句柄（即Slot索引），失败返回INVALID_HANDLE
 * 
 * @algorithm 线性扫描Slot表，找到第一个inactive的槽位
 * @complexity O(MAX_SLOTS)，最坏情况遍历所有1024个槽位
 * 
 * @critical_steps
 * 1. 设置 is_active = true（标记占用）
 * 2. 保存 block_ptr 建立映射关系
 * 3. 返回索引 i 作为句柄（对上层透明）
 * 
 * @note 这是O(n)操作，若频繁分配可考虑维护空闲链表优化
 */
static MemHandle _request_slot(BlockHeader *block)
{
    for (uint32_t i = 0; i < MAX_SLOTS; i++)
    {
        if (!g_manager.slots[i].is_active)
        {
            g_manager.slots[i].is_active = true;
            g_manager.slots[i].block_ptr = block;
            return i;
        }
    }
    return INVALID_HANDLE;
}

/**
 * @brief 释放Slot槽位
 * 
 * @param handle 要释放的句柄
 * 
 * @implementation
 * - 清除激活标志 is_active = false
 * - 清空 block_ptr = NULL（可选，保留可便于调试发现use-after-free）
 * 
 * @warning 调用前需确保handle < MAX_SLOTS，否则静默失败
 */
static void _release_slot(MemHandle handle)
{
    if (handle >= MAX_SLOTS)
        return;
    g_manager.slots[handle].is_active = false;
    g_manager.slots[handle].block_ptr = NULL;
}

/**
 * @brief 验证内存块的合法性
 * 
 * @param block 待验证的块指针
 * @return true 魔数匹配，块有效
 * @return false 魔数不匹配，可能已腐败或指针错误
 * 
 * @security 防御性编程的关键，每次访问块前都应校验
 * @usage 在alloc/free/deref/defrag等所有访问块的入口处调用
 */
static bool _is_valid_block(BlockHeader *block)
{
    return block->magic == BLOCK_MAGIC;
}

/**
 * @brief 合并相邻的空闲块（减少外部碎片）
 * 
 * @algorithm 单次扫描贪心合并
 * 1. 从头到尾遍历所有块
 * 2. 检测当前块和下一块是否都为空闲
 * 3. 若是，将下一块合并到当前块（扩展data_size）
 * 4. continue继续检查新的下一块（贪心策略）
 * 
 * @merge_formula 合并后的大小计算
 * ```
 * block->data_size += sizeof(BlockHeader) + next->data_size
 *                   = 原数据区 + 下一块头部 + 下一块数据区
 * ```
 * 
 * @critical_logic
 * - 合并后**不移动current指针**，因为continue会继续检查同一位置
 * - 保持魔数和slot指针不变（仅扩展大小）
 * - 被合并的next块不再独立存在，其空间成为block的一部分
 * 
 * @complexity O(N)，N为块的数量
 * @side_effect 修改块的data_size字段，但不改变物理布局
 * 
 * @example 合并前后对比
 * ```
 * 合并前: [A:free][B:free][C:used]
 * 合并后: [AB:free][C:used]  （A的data_size扩展包含B）
 * ```
 */
static void _coalesce()
{
    BlockHeader *block = (BlockHeader *)g_manager.heap_start;

    while (block)
    {
        /* 安全性检查：检测堆腐败 */
        if (!_is_valid_block(block))
        {
            fprintf(stderr, "Heap Corruption Detected.\n");
            exit(1);
        }

        BlockHeader *next = block->next;
        if (!next)
            break;

        /* 核心合并逻辑：两个相邻空闲块合并为一个 */
        if (block->is_free && next->is_free)
        {
            /* 关键：合并时需包含下一块的头部空间 */
            block->data_size += sizeof(BlockHeader) + next->data_size;
            block->next = next->next;
            continue; /* 继续尝试合并更后面的块（贪心） */
        }

        block = block->next;
    }
}

/* ==================== 公共API实现 ==================== */

/**
 * @brief 初始化堆管理器
 * 
 * @param size 请求的堆大小（字节）
 * 
 * @initialization_steps
 * 1. 对齐size到8字节边界
 * 2. 分配原始内存（malloc）
 * 3. 清零Slot表（memset）
 * 4. 创建初始空闲块（占据整个堆空间）
 * 
 * @design_principle "大空白纸"策略
 * 初始状态下，整个堆是一个大的空闲块，后续分配从中切割。
 * 这种设计简化了首次分配的逻辑，无需特殊处理。
 * 
 * @memory_layout 初始化后的堆布局
 * ```
 * [BlockHeader: 整个堆-24字节][未使用空间]
 *  ↑ is_free=true, data_size=HEAP_SIZE-24
 * ```
 * 
 * @error_handling 分配失败时直接终止程序（适合嵌入式/专用场景）
 * @warning 调用前不应有其他堆实例存在，否则造成内存泄漏
 */
void mem_init(uint32_t size)
{
    size = ALIGN_8(size);
    g_manager.total_size = size;

    g_manager.heap_start = (uint8_t *)malloc(size);
    if (!g_manager.heap_start)
    {
        fprintf(stderr, "Heap Init Failed.\n");
        exit(1);
    }

    // 清零Slot表
    // void *memset(void *ptr, int value, size_t num); 
    //  - ptr: 目标内存地址
    //  - value: 要设置的值（每个字节）
    //  - num: 要设置的字节数
    memset(g_manager.slots, 0, sizeof(g_manager.slots));
  


    /* 创建初始空闲块：占据整个堆空间 */
    BlockHeader *initial = (BlockHeader *)g_manager.heap_start;
    initial->magic = BLOCK_MAGIC;
    initial->data_size = size - sizeof(BlockHeader);
    initial->is_free = true;
    initial->slot = NULL;
    initial->next = NULL;

    printf("Heap Init Success. Total Size: %u bytes\n", size);
}

/**
 * @brief 销毁堆管理器，释放底层内存
 * 
 * @note 调用前应手动释放所有已分配的句柄，否则会造成内存泄漏
 * @warning 销毁后不应再使用任何之前分配的handle
 */
void mem_destroy()
{
    if (g_manager.heap_start)
    {
        free(g_manager.heap_start);
        g_manager.heap_start = NULL;
    }
}

/**
 * @brief 分配指定大小的内存块
 * 
 * @param size 请求的字节数
 * @return MemHandle 成功返回句柄，失败返回INVALID_HANDLE
 * 
 * @algorithm First-Fit（首次适配）
 * 1. 对齐size到8字节
 * 2. 线性扫描堆空间，寻找第一个足够大的空闲块
 * 3. 若找到，根据剩余空间决定是否分割
 * 4. 分配Slot并返回句柄
 * 
 * @split_strategy 分割条件
 * ```
 * 剩余空间 >= sizeof(BlockHeader) + MIN_SPLIT_SIZE 时才分割
 * 避免产生无法利用的微小碎片（<8字节）
 * ```
 * 
 * @complexity O(N)，N为现有块的数量（最坏遍历整个堆）
 * 
 * @critical_pointer_operations
 * - **新块位置**: `current_block + header_size + allocated_size`
 * - **剩余空间**: `original_data_size - requested_size - new_header_size`
 * - **类型转换**: uint8_t*用于字节级算术，BlockHeader*用于结构化访问
 * 
 * @example 分配流程示例
 * ```
 * 初始: [Free: 1000字节]
 * 请求: mem_alloc(100)
 * 结果: [Used: 100][Free: 860]  （24字节头部+100数据+24头部+860数据）
 * ```
 * 
 * @fragmentation 多次alloc/free后会产生外部碎片，需定期调用mem_defrag
 */
MemHandle mem_alloc(uint32_t size)
{
    if (size == 0)
        return INVALID_HANDLE;

    size = ALIGN_8(size);

    BlockHeader *block = (BlockHeader *)g_manager.heap_start;

    /* 线性扫描寻找合适的空闲块 */
    while (block)
    {
        if (!_is_valid_block(block))
        {
            fprintf(stderr, "Heap Corruption in alloc.\n");
            exit(1);
        }

        /* 找到第一个满足条件的空闲块 */
        if (block->is_free && block->data_size >= size)
        {
            uint32_t remain = block->data_size - size;

            /* 判断是否需要分割：剩余空间足够创建一个新块 */
            if (remain >= sizeof(BlockHeader) + MIN_SPLIT_SIZE)
            {
                /* 在剩余空间创建新的空闲块 */
                BlockHeader *new_block = (BlockHeader *)((uint8_t *)block + sizeof(BlockHeader) + size);

                new_block->magic = BLOCK_MAGIC;
                new_block->data_size = remain - sizeof(BlockHeader);
                new_block->is_free = true;
                new_block->slot = NULL;
                new_block->next = block->next;

                /* 缩小当前块的数据区大小 */
                block->data_size = size;
                block->next = new_block;
            }

            /* 标记为已分配 */
            block->is_free = false;

            /* 分配Slot并建立映射 */
            MemHandle handle = _request_slot(block);
            if (handle == INVALID_HANDLE)
                return INVALID_HANDLE;

            block->slot = &g_manager.slots[handle];
            return handle;
        }

        /* 移动到下一个块 */
        block = block->next;
    }

    return INVALID_HANDLE; /* 无足够空间 */
}

/**
 * @brief 通过句柄解引用获取数据指针
 * 
 * @param handle 内存句柄
 * @return void* 用户数据区指针，失败返回NULL
 * 
 * @abstraction_layer 句柄系统的核心价值
 * ```
 * 传统方式: char *ptr = malloc(100);  // 直接使用指针
 *          free(ptr);                  // ptr变成悬空指针！
 * 
 * 句柄方式: MemHandle h = mem_alloc(100);
 *          char *ptr = mem_deref(h);  // 间接获取指针
 *          mem_free(h);               // h仍然有效，但deref返回NULL
 * ```
 * 
 * @safety_checks 四层安全检查
 * 1. 句柄范围检查（handle < MAX_SLOTS）
 * 2. Slot激活状态检查（is_active == true）
 * 3. 块魔数校验（magic == BLOCK_MAGIC）
 * 4. 指针对齐验证（address & 7 == 0）
 * 
 * @performance O(1)操作，仅涉及数组访问和指针解引用
 * 
 * @warning 在mem_defrag之后，物理地址会改变，必须重新调用此函数获取最新地址
 * @usage_pattern 每次使用前都调用mem_deref，不要缓存返回的指针
 */
void *mem_deref(MemHandle handle)
{
    if (handle >= MAX_SLOTS)
        return NULL;

    if (!g_manager.slots[handle].is_active)
        return NULL;

    BlockHeader *block = g_manager.slots[handle].block_ptr;

    if (!_is_valid_block(block))
    {
        fprintf(stderr, "Invalid Block accessed via Handle %u.\n", handle);
        exit(1);
    }

    void *ptr = block->data;

    /* 对齐检查：确保返回的指针是8字节对齐的 */
    if (((uintptr_t)ptr & 7) != 0)
    {
        fprintf(stderr, "Unaligned Pointer Detected.\n");
        exit(1);
    }

    return ptr;
}

/**
 * @brief 释放内存块
 * 
 * @param handle 要释放的句柄
 * 
 * @release_workflow 释放流程
 * 1. 验证句柄和Slot的有效性
 * 2. 标记对应块为空闲（is_free = true）
 * 3. 清除块的slot指针（设为NULL）
 * 4. 释放Slot槽位（is_active = false）
 * 5. 触发相邻空闲块合并（_coalesce）
 * 
 * @design_philosophy "标记-合并"策略
 * - **标记阶段**: 快速O(1)操作，立即回收逻辑资源
 * - **合并阶段**: 延迟处理，减少频繁的小块合并开销
 * 
 * @fragmentation_impact
 * 释放中间块会产生外部碎片：
 * ```
 * 释放前: [A:used][B:used][C:used]
 * 释放B:  [A:used][B:FREE][C:used]  ← 产生空洞
 * 合并后: [A:used][BC:FREE]         ← 若C也空闲则合并
 * ```
 * 
 * @note 释放后handle失效，再次使用会返回NULL（安全）
 */
void mem_free(MemHandle handle)
{
    if (handle >= MAX_SLOTS)
        return;

    if (!g_manager.slots[handle].is_active)
        return;

    BlockHeader *block = g_manager.slots[handle].block_ptr;

    if (!_is_valid_block(block))
    {
        fprintf(stderr, "Heap Corruption in free.\n");
        exit(1);
    }

    block->is_free = true;
    block->slot = NULL;

    _release_slot(handle);

    /* 合并相邻空闲块，减少碎片 */
    _coalesce();
}

/**
 * @brief 碎片整理（紧凑化压缩算法）
 * 
 * @algorithm 双指针扫描复制算法（Two-Pointer Compaction）
 * 
 * @core_principle 使用read_ptr和write_ptr两个指针同步扫描
 * ```
 * read_ptr:  读取源块（遍历所有块，包括空闲块）
 * write_ptr: 写入目标位置（仅移动已分配块）
 * 空闲块:    被跳过，自然被淘汰
 * ```
 * 
 * @execution_steps 执行步骤
 * 1. 初始化read_ptr和write_ptr都指向堆起始位置
 * 2. read_ptr遍历每个块：
 *    a. 若为**已分配块**：
 *       - 使用memmove复制到write_ptr位置
 *       - **更新对应Slot的block_ptr**（关键！保持句柄有效）
 *       - write_ptr前进该块的大小
 *    b. 若为**空闲块**：跳过（自然被淘汰）
 * 3. 整理完成后，尾部剩余空间合并为一个大的空闲块
 * 
 * @critical_update Slot表更新的重要性
 * ```c
 * // 复制后必须更新对应Slot的block_ptr
 * moved_block->slot->block_ptr = moved_block;
 * ```
 * 这是保证句柄系统一致性的**核心操作**，缺少此步会导致deref返回旧地址！
 * 
 * @complexity O(N*M)，N为块数，M为平均块大小（memmove开销）
 * @side_effect 所有已分配块的物理地址都会改变，但句柄保持不变
 * 
 * @usage_pattern 应在以下场景调用：
 * - 大量alloc/free后（碎片率高）
 * - 游戏关卡切换时（批量清理）
 * - 检测到分配失败时（作为最后手段）
 * - 定期维护（如每100次分配后）
 * 
 * @example 整理前后对比
 * ```
 * 整理前: [A:used][FREE][C:used][FREE][E:used]
 * 整理后: [A:used][C:used][E:used][FREE:大片连续空间]
 * ```
 * 
 * @note memmove能正确处理重叠内存区域，比memcpy更安全
 */
void mem_defrag()
{
    printf("\n[Defrag Start]\n");

    uint8_t *write_ptr = g_manager.heap_start;
    uint8_t *heap_end = g_manager.heap_start + g_manager.total_size;
    BlockHeader *read_block = (BlockHeader *)g_manager.heap_start;
    BlockHeader *prev_moved = NULL;

    while (read_block)
    {
        BlockHeader *next_read = read_block->next;

        if (!_is_valid_block(read_block))
        {
            fprintf(stderr, "Heap Corruption during defrag.\n");
            exit(1);
        }

        uint32_t total_size = sizeof(BlockHeader) + read_block->data_size;

        if (!read_block->is_free)
        {
            BlockHeader *moved_block = (BlockHeader *)write_ptr;

            if ((uint8_t *)read_block != write_ptr)
            {
                memmove(write_ptr, read_block, total_size);
                moved_block = (BlockHeader *)write_ptr;
            }

            moved_block->next = NULL;

            if (prev_moved)
            {
                prev_moved->next = moved_block;
            }

            prev_moved = moved_block;
            if (moved_block->slot)
            {
                moved_block->slot->block_ptr = moved_block;
            }
            write_ptr += total_size;
        }

        read_block = next_read;
    }

    uint32_t remain = (uint32_t)(heap_end - write_ptr);
    BlockHeader *free_block = NULL;

    if (remain >= sizeof(BlockHeader) + MIN_SPLIT_SIZE)
    {
        free_block = (BlockHeader *)write_ptr;
        free_block->magic = BLOCK_MAGIC;
        free_block->data_size = remain - sizeof(BlockHeader);
        free_block->is_free = true;
        free_block->slot = NULL;
        free_block->next = NULL;
        write_ptr += remain;
    }

    if (prev_moved)
    {
        prev_moved->next = free_block;
    }

    printf("[Defrag Finished]\n\n");
}

/* ==================== 测试主程序 ==================== */

/**
 * @brief 测试程序：演示内存管理器的完整生命周期
 * 
 * @test_phases 四个测试阶段
 * - **Phase 1**: 分配测试 - 创建3个不同大小的块
 * - **Phase 2**: 碎片制造 - 释放中间块B，产生空洞
 * - **Phase 3**: 碎片整理 - 调用defrag压缩空间
 * - **Phase 4**: 验证 - 确认数据完整性和地址变化
 * 
 * @expected_behavior 预期行为
 * - C的地址在defrag后会变小（向低地址移动，填补B的空洞）
 * - A和C的数据在defrag前后保持一致（memmove保证）
 * - 所有句柄在整个过程中保持有效（Slot表更新保证）
 * 
 * @memory_layout_evolution 内存布局演变
 * ```
 * Phase 1: [A:5bytes][B:19bytes][C:1024bytes][FREE]
 * Phase 2: [A:5bytes][FREE][C:1024bytes][FREE]  ← B被释放
 * Phase 3: [A:5bytes][C:1024bytes][FREE:大片]   ← 整理后C前移
 * ```
 */
int main()
{
    mem_init(HEAP_SIZE);

    printf("\n=== 第1阶段：分配 ===\n");
    MemHandle A = mem_alloc(5);   // 实际占用8字节（对齐后）
    MemHandle B = mem_alloc(19);  // 实际占用24字节（对齐后）
    MemHandle C = mem_alloc(1024);

    strcpy((char *)mem_deref(A), "BlockA");
    strcpy((char *)mem_deref(C), "Handle-Based Compacting Heap");

    printf("地址 A: %p\n", mem_deref(A));
    printf("地址 B: %p\n", mem_deref(B));
    printf("地址 C: %p\n", mem_deref(C));

    printf("\n=== 第2阶段：制造碎片 ===\n");
    printf("正在释放 B（中间块）...\n");
    mem_free(B);

    printf("碎片整理前 C 的地址: %p\n", mem_deref(C));

    printf("\n=== 第3阶段：碎片整理 ===\n");
    mem_defrag();

    printf("碎片整理后 C 的地址: %p\n", mem_deref(C));

    printf("\n=== 第4阶段：验证 ===\n");
    printf("A 的数据: %s\n", (char *)mem_deref(A));
    printf("C 的数据: %s\n", (char *)mem_deref(C));

    mem_destroy();
    return 0;
}