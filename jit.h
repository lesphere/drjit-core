#pragma once

#include "malloc.h"
#include "cuda.h"
#include <mutex>
#include <condition_variable>
#include <string.h>
#include <inttypes.h>

#define PTR "0x%" PRIxPTR
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#if defined(ENOKI_CUDA)
struct Device {
    uint32_t id;
    uint32_t block_count;
    uint32_t thread_count;
};

struct Stream {
    /// Enoki device index associated with this stream (*not* the CUDA device ID)
    uint32_t device = 0;

    /// Index of this stream
    uint32_t stream = 0;

    /// Associated CUDA stream handle
    cudaStream_t handle = nullptr;

    /// A CUDA event for synchronization purposes
    cudaEvent_t event = nullptr;

    /// Memory regions that will be unused once the running kernel finishes
    AllocInfoMap alloc_pending;
};

using StreamMap = tsl::robin_map<std::pair<uint32_t, uint32_t>, Stream *, pair_hash>;
#endif

enum EnokiType { Invalid = 0, Int8, UInt8, Int16, UInt16,
                 Int32, UInt32, Int64, UInt64, Float16,
                 Float32, Float64, Bool, Pointer };

/// Central variable data structure, which represents an assignment in SSA form
struct Variable {
    /// Intermediate language statement
    char *cmd = nullptr;

    /// Data type of this variable
    uint32_t type = (uint32_t) EnokiType::Invalid;

    /// Number of entries
    uint32_t size = 0;

    /// Dependencies of this instruction
    uint32_t dep[3] { 0 };

    /// Extra dependency (which is not directly used in arithmetic, e.g. scatter/gather)
    uint32_t extra_dep = 0;

    /// Associated label (for debugging)
    char *label = nullptr;

    /// Pointer to device memory
    void *data = nullptr;

    /// External reference count (by application using Enoki)
    uint32_t ref_count_ext = 0;

    /// Internal reference count (dependencies within computation ggraph)
    uint32_t ref_count_int = 0;

    /// Size of the instruction subtree (heuristic for instruction scheduling)
    uint32_t tsize = 0;

    /// Does the instruction have side effects (e.g. 'scatter')
    bool side_effect = false;

    /// A variable is 'dirty' if there are pending scatter operations to it
    bool dirty = false;

    /// Free 'data' after this variable is no longer referenced?
    bool free_variable = true;

    /// Optimization: is this a direct pointer (rather than an array which stores a pointer?)
    bool direct_pointer = false;
};

/// Abbreviated version of the Variable data structure
struct VariableKey {
    char *cmd;
    uint32_t type;
    uint32_t size;
    uint32_t dep[3];
    uint32_t extra_dep;

    VariableKey(const Variable &v) {
        memcpy(this, &v, sizeof(VariableKey));
    }

    bool operator==(const VariableKey &v) const {
        return memcmp(this, &v, sizeof(VariableKey)) == 0;
    }
};

struct VariableKeyHasher {
    size_t operator()(const VariableKey &k) const {
        size_t result = crc32((const uint8_t *) k.cmd, strlen(k.cmd));
        hash_combine(result, crc32((const uint8_t *) &k.type, sizeof(uint32_t) * 6));
        return result;
    }
};


/// Records the full JIT compiler state
struct State {
    /// Must be held to access members
    std::mutex mutex;

    /// Indicates whether the state is initialized by \ref jit_init()
    bool initialized = false;

    /// Log level
    uint32_t log_level = 0;

    /// Available devices and their CUDA IDs
    std::vector<Device> devices;

#if defined(ENOKI_CUDA)
    /// Maps Enoki (device index, stream index) pairs to a Stream data structure
    StreamMap streams;
#endif

    /// Map of currently allocated memory regions
    tsl::robin_map<void *, AllocInfo> alloc_used;

    /// Map of currently unused memory regions
    AllocInfoMap alloc_free;

    /// Keep track of current memory usage and a maximum watermark
    size_t alloc_usage    [(int) AllocType::Count] { 0 },
           alloc_watermark[(int) AllocType::Count] { 0 };

    /// Stores the mapping from variable indices to variables
    tsl::robin_map<uint32_t, Variable> variables;

    /// Maps from a key characterizing a variable to its index
    tsl::robin_map<VariableKey, uint32_t, VariableKeyHasher> variable_from_key;

    /// Maps from pointer addresses to variable indices
    tsl::robin_map<const void *, uint32_t> variable_from_ptr;

    /// Current variable index
    uint32_t variable_index = 1;

    /// Current operand for scatter/gather operations
    uint32_t scatter_gather_operand = 0;

    /// TODO: maybe a TLS variable?
    /// Enumerates "live" (externally referenced) variables and statements with side effects
    tsl::robin_set<uint32_t> live;

    /// Enumerates "dirty" variables (targets of 'scatter' operations that have not yet executed)
    std::vector<uint32_t> dirty;
};

/// RAII helper for locking a mutex (like std::lock_guard)
class lock_guard {
public:
    lock_guard(std::mutex &mutex) : m_mutex(mutex) { m_mutex.lock(); }
    ~lock_guard() { m_mutex.unlock(); }
    lock_guard(const lock_guard &) = delete;
    lock_guard &operator=(const lock_guard &) = delete;
private:
    std::mutex &m_mutex;
};

/// RAII helper for *unlocking* a mutex
class unlock_guard {
public:
    unlock_guard(std::mutex &mutex) : m_mutex(mutex) { m_mutex.unlock(); }
    ~unlock_guard() { m_mutex.lock(); }
    unlock_guard(const unlock_guard &) = delete;
    unlock_guard &operator=(const unlock_guard &) = delete;
private:
    std::mutex &m_mutex;
};

class wait_flag {
public:
    wait_flag() : m_flag(false) { }

    void set() {
        lock_guard g(m_mutex);
        m_flag = true;
        m_cond.notify_all();
    }

    void clear() {
        lock_guard g(m_mutex);
        m_flag = false;
    }

    void wait() {
        std::unique_lock lock(m_mutex);
        m_cond.wait(lock, [this]() { return m_flag; });
    }

private:
    bool m_flag;
    std::mutex m_mutex;
    std::condition_variable m_cond;
};

struct Buffer {
public:
    Buffer();

    // Disable copy/move constructor and assignment
    Buffer(const Buffer &) = delete;
    Buffer(Buffer &&) = delete;
    Buffer &operator=(const Buffer &) = delete;
    Buffer &operator=(Buffer &&) = delete;

    ~Buffer() {
        free(m_start);
    }

    const char *get() { return m_start; }

    void clear() {
        m_cur = m_start;
        m_start[0] = '\0';
    }

    void put(const char *str) {
        do {
            char *cur = (char *) memccpy(m_cur, str, '\0', m_end - m_cur);

            if (likely(cur)) {
                m_cur = cur - 1;
                break;
            }

            expand();
        } while (true);
    }

    size_t fmt(const char *format, ...) {
        size_t written;
        do {
            size_t size = m_end - m_cur;
            va_list args;
            va_start(args, format);
            written = (size_t) vsnprintf(m_cur, size, format, args);
            va_end(args);

            if (likely(written < size)) {
                m_cur += written;
                break;
            }

            expand();
        } while (true);
        return written;
    }

private:
    void expand();

private:
    char *m_start, *m_cur, *m_end;
};


/// Global state record shared by all threads
#if defined(ENOKI_CUDA)
  extern __thread Stream *active_stream;
#endif

extern State state;
extern Buffer buffer;

/// Initialize core data structures of the JIT compiler
extern void jit_init();

/// Release all resources used by the JIT compiler, and report reference leaks.
extern void jit_shutdown();

/// Set the currently active device & stream
extern void jit_device_set(uint32_t device, uint32_t stream);

/// Wait for all computation on the current stream to finish
extern void jit_stream_sync();

/// Wait for all computation on the current device to finish
extern void jit_device_sync();
