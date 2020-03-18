#include "llvm_api.h"
#include "internal.h"
#include "log.h"
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

/// Version number for cache files
#define ENOKI_LLVM_CACHE_VERSION 1

/// LLVM API
using LLVMBool = int;
using LLVMDisasmContextRef = void *;
using LLVMExecutionEngineRef = void *;
using LLVMModuleRef = void *;
using LLVMMemoryBufferRef = void *;
using LLVMContextRef = void *;
using LLVMMCJITMemoryManagerRef = void *;

struct LLVMMCJITCompilerOptions {
  unsigned OptLevel;
  int CodeModel;
  LLVMBool NoFramePointerElim;
  LLVMBool EnableFastISel;
  void *MCJMM;
};

static void (*LLVMLinkInMCJIT)() = nullptr;
static void (*LLVMInitializeX86AsmPrinter)() = nullptr;
static void (*LLVMInitializeX86Disassembler)() = nullptr;
static void (*LLVMInitializeX86Target)() = nullptr;
static void (*LLVMInitializeX86TargetInfo)() = nullptr;
static void (*LLVMInitializeX86TargetMC)() = nullptr;
static void (*LLVMDisposeMessage)(char *) = nullptr;
static char *(*LLVMGetDefaultTargetTriple)() = nullptr;
static char *(*LLVMGetHostCPUName)() = nullptr;
static char *(*LLVMGetHostCPUFeatures)() = nullptr;
static LLVMContextRef (*LLVMGetGlobalContext)() = nullptr;
static LLVMDisasmContextRef (*LLVMCreateDisasm)(const char *, void *, int,
                                                void *, void *) = nullptr;
static void (*LLVMDisasmDispose)(LLVMDisasmContextRef) = nullptr;
static int (*LLVMSetDisasmOptions)(LLVMDisasmContextRef, uint64_t) = nullptr;
static LLVMModuleRef (*LLVMModuleCreateWithName)(const char *) = nullptr;
static LLVMBool (*LLVMCreateMCJITCompilerForModule)(LLVMExecutionEngineRef *,
                                                    LLVMModuleRef,
                                                    LLVMMCJITCompilerOptions *,
                                                    size_t, char **) = nullptr;
static LLVMMCJITMemoryManagerRef (*LLVMCreateSimpleMCJITMemoryManager)(
    void *, void *, void *, void *, void *) = nullptr;
static void (*LLVMDisposeExecutionEngine)(LLVMExecutionEngineRef) = nullptr;
static void (*LLVMAddModule)(LLVMExecutionEngineRef, LLVMModuleRef) = nullptr;
static void (*LLVMDisposeModule)(LLVMModuleRef) = nullptr;
static LLVMMemoryBufferRef (*LLVMCreateMemoryBufferWithMemoryRange)(
    const char *, size_t, const char *, LLVMBool) = nullptr;
static LLVMBool (*LLVMParseIRInContext)(LLVMContextRef, LLVMMemoryBufferRef,
                                        LLVMModuleRef *, char **) = nullptr;
static char *(*LLVMPrintModuleToString)(LLVMModuleRef) = nullptr;
static uint64_t (*LLVMGetFunctionAddress)(LLVMExecutionEngineRef, const char *);
static LLVMBool (*LLVMRemoveModule)(LLVMExecutionEngineRef, LLVMModuleRef,
                                    LLVMModuleRef *, char **) = nullptr;
static size_t (*LLVMDisasmInstruction)(LLVMDisasmContextRef, uint8_t *,
                                       uint64_t, uint64_t, char *,
                                       size_t) = nullptr;

#define LLVMDisassembler_Option_PrintImmHex       2
#define LLVMDisassembler_Option_AsmPrinterVariant 4
#define LLVMCodeModelSmall 3

/// Enoki API
static void *jit_llvm_handle                  = nullptr;
static LLVMDisasmContextRef jit_llvm_disasm   = nullptr;
static LLVMExecutionEngineRef jit_llvm_engine = nullptr;
static LLVMContextRef jit_llvm_context        = nullptr;

char *jit_llvm_target_cpu                     = nullptr;
char *jit_llvm_target_features                = nullptr;
int   jit_llvm_vector_width                   = 0;
uint32_t jit_llvm_kernel_id                   = 0;

static bool     jit_llvm_init_attempted = false;
static bool     jit_llvm_init_success   = false;

static uint8_t *jit_llvm_mem        = nullptr;
static size_t   jit_llvm_mem_size   = 0;
static size_t   jit_llvm_mem_offset = 0;

extern "C" {

static uint8_t *jit_llvm_mem_allocate(void * /* opaque */, uintptr_t size,
                                      unsigned align, unsigned /* id */,
                                      const char *name) {
    if (align == 0)
        align = 16;

    jit_trace("jit_llvm_mem_allocate(section=%s, size=%zu, align=%u);", name,
              size, (uint32_t) align);

    size_t offset_align = (jit_llvm_mem_offset + (align - 1)) / align * align;

    // Zero-fill padding region for alignment
    memset(jit_llvm_mem + jit_llvm_mem_offset, 0,
           offset_align - jit_llvm_mem_offset);

    jit_llvm_mem_offset = offset_align + size;

    if (jit_llvm_mem_offset > jit_llvm_mem_size)
        return nullptr;

    return jit_llvm_mem + offset_align;
}

static uint8_t *jit_llvm_mem_allocate_data(void *opaque, uintptr_t size,
                                           unsigned align, unsigned id,
                                           const char *name,
                                           LLVMBool /* read_only */) {
    return jit_llvm_mem_allocate(opaque, size, align, id, name);
}

static LLVMBool jit_llvm_mem_finalize(void * /* opaque */, char ** /* err */) {
    return 0;
}

static void jit_llvm_mem_destroy(void * /* opaque */) { }

} /* extern "C" */ ;

bool jit_llvm_load(Kernel &kernel, const char *buffer, size_t buffer_size, uint32_t hash) {
    uint8_t version_number;
    uint32_t ir_size, payload_size, func_offset;
    char scratch[1024];
    snprintf(scratch, sizeof(scratch), "%s/.enoki/%08x.bin", getenv("HOME"), hash);

    int fd = open(scratch, O_RDONLY | O_EXCL);
    if (fd == -1)
        return false;

    ssize_t rv_1 = read(fd, &version_number, sizeof(uint8_t));
    ssize_t rv_2 = read(fd, &ir_size, sizeof(uint32_t));
    ssize_t rv_3 = read(fd, &payload_size, sizeof(uint32_t));
    ssize_t rv_4 = read(fd, &func_offset, sizeof(uint32_t));

    if (rv_1 != sizeof(uint8_t) || rv_2 != sizeof(uint32_t) ||
        rv_3 != sizeof(uint32_t) || rv_4 != sizeof(uint32_t) ||
        ir_size != buffer_size || func_offset >= payload_size ||
        version_number != ENOKI_LLVM_CACHE_VERSION) {
        close(fd);
        return false;
    }

    size_t remain = buffer_size;
    const char *ptr = buffer;

    while (remain > 0) {
        ssize_t n_read = read(fd, scratch, std::min(remain, sizeof(scratch)));

        if (n_read <= 0) {
            if (errno == EINTR) {
                continue;
            } else {
                close(fd);
                return false;
            }
        }

        if (memcmp(ptr, scratch, (size_t) n_read) != 0) {
            close(fd);
            return false;
        }

        remain -= n_read;
        ptr += n_read;
    }

    void *payload =
        mmap(nullptr, payload_size, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (payload == MAP_FAILED)
        jit_fail("jit_llvm_load(): could not mmap() memory: %s",
                 strerror(errno));

    uint8_t *payload_ptr = (uint8_t *) payload;
    remain = payload_size;

    while (remain > 0) {
        ssize_t n_read = read(fd, payload_ptr, remain);

        if (n_read <= 0) {
            if (errno == EINTR) {
                continue;
            } else {
                close(fd);
                munmap(payload, payload_size);
                return false;
            }
        }
        remain -= n_read;
        payload_ptr += n_read;
    }

    close(fd);

    if (mprotect(payload, payload_size, PROT_READ | PROT_EXEC) == -1)
        jit_fail("jit_llvm_load(): mprotect() failed: %s", strerror(errno));

    kernel.llvm.buffer = payload;
    kernel.llvm.func = (LLVMKernelFunction) ((uint8_t *) payload + func_offset);
    kernel.llvm.size = payload_size;
    kernel.type = KernelType::LLVM;

    return true;
}

void jit_llvm_write(const char *buffer, size_t buffer_size, uint32_t hash, uint32_t func_offset) {
    char scratch[1024];
    snprintf(scratch, sizeof(scratch), "%s/.enoki/%08x.bin", getenv("HOME"), hash);
    mode_t mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
    int fd = open(scratch, O_CREAT | O_WRONLY | O_EXCL, mode);
    if (fd == -1 && errno != EEXIST)
        jit_fail("jit_llvm_compile(): could not write compiled kernel to cache "
                 "file \"%s/.enoki/%08x.bin\": %s",
                 getenv("HOME"), hash, strerror(errno));

    auto write_retry = [&](const uint8_t *data, size_t data_size) {
        while (data_size > 0) {
            ssize_t n_written = write(fd, data, data_size);
            if (n_written <= 0) {
                if (errno == EINTR) {
                    continue;
                } else {
                    jit_fail("jit_llvm_compile(): could not write compiled kernel to cache "
                             "file \"%s/.enoki/%08x.bin\": %s",
                             getenv("HOME"), hash, strerror(errno));
                }
            }
            data += n_written;
            data_size -= n_written;
        }
    };

    if (fd != -1) {
        uint8_t version = ENOKI_LLVM_CACHE_VERSION;
        uint32_t data_size = (uint32_t) jit_llvm_mem_offset;
        write_retry((const uint8_t *) &version, sizeof(uint8_t));
        write_retry((const uint8_t *) &buffer_size, sizeof(uint32_t));
        write_retry((const uint8_t *) &data_size, sizeof(uint32_t));
        write_retry((const uint8_t *) &func_offset, sizeof(uint32_t));
        write_retry((const uint8_t *) buffer, buffer_size);
        write_retry((const uint8_t *) jit_llvm_mem, jit_llvm_mem_offset);
        close(fd);
    }
}

Kernel jit_llvm_compile(const char *buffer, size_t buffer_size, uint32_t hash, bool &cache_hit) {
    Kernel result;
    if (jit_llvm_load(result, buffer, buffer_size, hash)) {
        cache_hit = true;
        return result;
    }

    if (jit_llvm_mem_size <= buffer_size) {
        // Central assumption: LLVM text IR is much larger than the resulting generated code.
        free(jit_llvm_mem);
        if (posix_memalign((void **) &jit_llvm_mem, 64, buffer_size))
            jit_raise("jit_llvm_compile(): could not allocate %zu bytes of memory!", buffer_size);
        jit_llvm_mem_size = buffer_size;
    }
    jit_llvm_mem_offset = 0;

    // Temporarily change the kernel name
    char kernel_name_old[15], kernel_name_new[15];
    snprintf(kernel_name_new, 15, "enoki_%08x", (uint32_t) jit_llvm_kernel_id++);
    char *kernel_name_offset = (char *) strstr(buffer, "enoki_");
    memcpy(kernel_name_old, kernel_name_offset, 14);
    memcpy(kernel_name_offset, kernel_name_new, 14);

    LLVMMemoryBufferRef buf = LLVMCreateMemoryBufferWithMemoryRange(
        buffer, buffer_size, kernel_name_new, 0);
    if (unlikely(!buf))
        jit_fail("jit_run_compile(): could not create memory buffer!");

    // 'buf' is consumed by this function.
    LLVMModuleRef module = nullptr;
    char *error = nullptr;
    LLVMParseIRInContext(jit_llvm_context, buf, &module, &error);
    if (unlikely(error))
        jit_fail("jit_llvm_compile(): could not parse IR: %s.\n", error);

    if (false) {
        char *llvm_ir = LLVMPrintModuleToString(module);
        jit_trace("jit_llvm_compile(): Parsed LLVM IR:\n%s", llvm_ir);
        LLVMDisposeMessage(llvm_ir);
    }

    LLVMAddModule(jit_llvm_engine, module);

    uint8_t *func =
        (uint8_t *) LLVMGetFunctionAddress(jit_llvm_engine, kernel_name_new);
    if (unlikely(!func))
        jit_fail("jit_llvm_compile(): internal error: could not fetch function "
                 "address of kernel \"%s\"!\n", kernel_name_new);
    else if (unlikely(func < jit_llvm_mem))
        jit_fail("jit_llvm_compile(): internal error: invalid address: "
                 "%p < %p!\n", func, jit_llvm_mem);

    uint32_t func_offset = (uint32_t) (func - jit_llvm_mem);

    /// Dump assembly representation
    if (std::max(state.log_level_stderr, state.log_level_callback) >=
        LogLevel::Trace) {
        uint8_t *ptr = func;
        char ins_buf[256];
        do {
            size_t cur_offset = ptr - jit_llvm_mem;
            if (cur_offset >= jit_llvm_mem_offset)
                break;
            size_t size = LLVMDisasmInstruction(
                jit_llvm_disasm, ptr, jit_llvm_mem_offset - cur_offset,
                (uintptr_t) ptr, ins_buf, sizeof(ins_buf));
            if (size == 0)
                break;
            char *start = ins_buf;
            while (*start == ' ' || *start == '\t')
                ++start;
            jit_trace("jit_llvm_compile(): 0x%08x   %s", (uint32_t) cur_offset, start);
            if (strncmp(start, "ret", 3) == 0)
                break;
            ptr += size;
        } while (true);
    }

    void *ptr_result =
        mmap(nullptr, jit_llvm_mem_offset, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr_result == MAP_FAILED)
        jit_fail("jit_llvm_compile(): could not mmap() memory: %s",
                 strerror(errno));
    memcpy(ptr_result, jit_llvm_mem, jit_llvm_mem_offset);

    if (mprotect(ptr_result, jit_llvm_mem_offset, PROT_READ | PROT_EXEC) == -1)
        jit_fail("jit_llvm_compile(): mprotect() failed: %s", strerror(errno));

    LLVMRemoveModule(jit_llvm_engine, module, &module, &error);
    if (unlikely(error))
        jit_fail("jit_llvm_compile(): could remove module: %s.\n", error);
    LLVMDisposeModule(module);

    // Change the kernel name back
    memcpy(kernel_name_offset, kernel_name_old, 14);

    jit_llvm_write(buffer, buffer_size, hash, func_offset);

    result.llvm.buffer = ptr_result;
    result.llvm.func = (LLVMKernelFunction) ((uint8_t *) ptr_result + func_offset);
    result.llvm.size = jit_llvm_mem_offset;
    result.type = KernelType::LLVM;

    cache_hit = false;
    return result;
}

void jit_llvm_free(Kernel kernel) {
    if (munmap((void *) kernel.llvm.buffer, kernel.llvm.size) == -1)
        jit_fail("jit_llvm_compile(): munmap() failed!");
}

#define LOAD(name)                                                             \
    symbol = #name;                                                            \
    name = decltype(name)(dlsym(jit_llvm_handle, symbol));                     \
    if (!name)                                                                 \
        break;                                                                 \
    symbol = nullptr

#define Z(x) x = nullptr

bool jit_llvm_init() {
    if (jit_llvm_init_attempted)
        return jit_llvm_init_success;
    jit_llvm_init_attempted = true;


    char scratch[1024];
    struct stat st = {};
    snprintf(scratch, sizeof(scratch), "%s/.enoki", getenv("HOME"));
    if (stat(scratch, &st) == -1) {
        jit_log(Info, "jit_llvm_init(): creating directory \"%s\" ..", scratch);
        if (mkdir(scratch, 0700) == -1)
            jit_fail("jit_llvm_init(): creation of directory \"%s\" failed: %s",
                     scratch, strerror(errno));
    }

#if defined(__linux__)
    const char *llvm_fname  = "libLLVM.so",
               *llvm_glob   = "/usr/lib/x86_64-linux-gnu/libLLVM*.so.*";
#else
    const char *llvm_fname  = "libLLVM.dylib",
               *llvm_glob   = "/usr/local/Cellar/llvm/*/lib/libLLVM.dylib";
#endif

    jit_llvm_handle = jit_find_library(llvm_fname, llvm_glob, "ENOKI_LIBLLVM_PATH");

    if (!jit_llvm_handle) {
        jit_log(Warn, "jit_llvm_init(): %s could not be loaded -- "
                      "disabling LLVM backend! Set the 'ENOKI_LIBLLVM_PATH' "
                      "environment variable to specify its path.", llvm_fname);
        return false;
    }

    const char *symbol = nullptr;
    do {
        LOAD(LLVMLinkInMCJIT);
        LOAD(LLVMInitializeX86Target);
        LOAD(LLVMInitializeX86TargetInfo);
        LOAD(LLVMInitializeX86TargetMC);
        LOAD(LLVMInitializeX86AsmPrinter);
        LOAD(LLVMInitializeX86Disassembler);
        LOAD(LLVMGetGlobalContext);
        LOAD(LLVMGetDefaultTargetTriple);
        LOAD(LLVMGetHostCPUName);
        LOAD(LLVMGetHostCPUFeatures);
        LOAD(LLVMDisposeMessage);
        LOAD(LLVMCreateDisasm);
        LOAD(LLVMDisasmDispose);
        LOAD(LLVMSetDisasmOptions);
        LOAD(LLVMModuleCreateWithName);
        LOAD(LLVMCreateMCJITCompilerForModule);
        LOAD(LLVMCreateSimpleMCJITMemoryManager);
        LOAD(LLVMDisposeExecutionEngine);
        LOAD(LLVMAddModule);
        LOAD(LLVMDisposeModule);
        LOAD(LLVMCreateMemoryBufferWithMemoryRange);
        LOAD(LLVMParseIRInContext);
        LOAD(LLVMPrintModuleToString);
        LOAD(LLVMGetFunctionAddress);
        LOAD(LLVMRemoveModule);
        LOAD(LLVMDisasmInstruction);
    } while (false);

    if (symbol) {
        jit_log(Warn,
                "jit_llvm_init(): could not find symbol \"%s\" -- disabling "
                "LLVM backend!", symbol);
        return false;
    }

    LLVMLinkInMCJIT();
    LLVMInitializeX86TargetInfo();
    LLVMInitializeX86Target();
    LLVMInitializeX86TargetMC();
    LLVMInitializeX86AsmPrinter();
    LLVMInitializeX86Disassembler();

    jit_llvm_context = LLVMGetGlobalContext();
    if (!jit_llvm_context) {
        jit_log(Warn, "jit_llvm_init(): could not obtain context!");
        return false;
    }

    char* triple = LLVMGetDefaultTargetTriple();
    jit_llvm_disasm = LLVMCreateDisasm(triple, nullptr, 0, nullptr, nullptr);

    if (!jit_llvm_disasm) {
        jit_log(Warn, "jit_llvm_init(): could not create a disassembler!");
        LLVMDisposeMessage(triple);
        return false;
    }

    if (LLVMSetDisasmOptions(jit_llvm_disasm,
                             LLVMDisassembler_Option_PrintImmHex |
                             LLVMDisassembler_Option_AsmPrinterVariant) == 0) {
        jit_log(Warn, "jit_llvm_init(): could not configure disassembler!");
        LLVMDisasmDispose(jit_llvm_disasm);
        LLVMDisposeMessage(triple);
        return false;
    }

    LLVMMCJITCompilerOptions options;
    options.OptLevel = 3;
    options.CodeModel = LLVMCodeModelSmall;
    options.NoFramePointerElim = false;
    options.EnableFastISel = false;
    options.MCJMM = LLVMCreateSimpleMCJITMemoryManager(
        nullptr,
        (void *) jit_llvm_mem_allocate,
        (void *) jit_llvm_mem_allocate_data,
        (void *) jit_llvm_mem_finalize,
        (void *) jit_llvm_mem_destroy);

    LLVMModuleRef enoki_module = LLVMModuleCreateWithName("enoki");
    char *error = nullptr;
    if (LLVMCreateMCJITCompilerForModule(&jit_llvm_engine, enoki_module,
                                         &options, sizeof(options), &error)) {
        jit_log(Warn, "jit_llvm_init(): could not create MCJIT: %s", error);
        LLVMDisposeModule(enoki_module);
        LLVMDisasmDispose(jit_llvm_disasm);
        LLVMDisposeMessage(triple);
        return -1;
    }

    jit_llvm_target_cpu = LLVMGetHostCPUName();
    jit_llvm_target_features = LLVMGetHostCPUFeatures();
    jit_llvm_vector_width = 1;

    if (strstr(jit_llvm_target_features, "+sse4.2"))
        jit_llvm_vector_width = 4;
    if (strstr(jit_llvm_target_features, "+avx"))
        jit_llvm_vector_width = 8;
    if (strstr(jit_llvm_target_features, "+avx512f"))
        jit_llvm_vector_width = 16;

    jit_log(Info, "jit_llvm_init(): found %s, cpu=%s, vector width=%i.", triple,
            jit_llvm_target_cpu, jit_llvm_vector_width);

    LLVMDisposeMessage(triple);

    jit_llvm_init_success = jit_llvm_vector_width > 1;

    if (!jit_llvm_init_success) {
        jit_log(Warn, "jit_llvm_init(): no suitable vector ISA found, shutting "
                      "down LLVM backend..");
        jit_llvm_shutdown();
    }

    return jit_llvm_init_success;
}

void jit_llvm_shutdown() {
    if (!jit_llvm_init_success)
        return;

    jit_log(Info, "jit_llvm_shutdown()");

    LLVMDisasmDispose(jit_llvm_disasm);
    LLVMDisposeExecutionEngine(jit_llvm_engine);
    LLVMDisposeMessage(jit_llvm_target_cpu);
    LLVMDisposeMessage(jit_llvm_target_features);
    dlclose(jit_llvm_handle);

    jit_llvm_engine = nullptr;
    jit_llvm_disasm = nullptr;
    jit_llvm_context = nullptr;
    jit_llvm_target_cpu = nullptr;
    jit_llvm_target_features = nullptr;
    jit_llvm_handle = nullptr;
    jit_llvm_vector_width = 0;

    free(jit_llvm_mem);
    jit_llvm_mem        = nullptr;
    jit_llvm_mem_size   = 0;
    jit_llvm_mem_offset = 0;
    jit_llvm_kernel_id = 0;

    Z(LLVMLinkInMCJIT); Z(LLVMInitializeX86Target);
    Z(LLVMInitializeX86TargetInfo); Z(LLVMInitializeX86TargetMC);
    Z(LLVMInitializeX86AsmPrinter); Z(LLVMInitializeX86Disassembler);
    Z(LLVMGetGlobalContext); Z(LLVMGetDefaultTargetTriple);
    Z(LLVMGetHostCPUName); Z(LLVMGetHostCPUFeatures); Z(LLVMDisposeMessage);
    Z(LLVMCreateDisasm); Z(LLVMDisasmDispose); Z(LLVMSetDisasmOptions);
    Z(LLVMModuleCreateWithName); Z(LLVMCreateMCJITCompilerForModule);
    Z(LLVMCreateSimpleMCJITMemoryManager); Z(LLVMDisposeExecutionEngine);
    Z(LLVMAddModule); Z(LLVMDisposeModule);
    Z(LLVMCreateMemoryBufferWithMemoryRange); Z(LLVMParseIRInContext);
    Z(LLVMPrintModuleToString); Z(LLVMGetFunctionAddress); Z(LLVMRemoveModule);
    Z(LLVMDisasmInstruction);

    jit_llvm_init_success = false;
    jit_llvm_init_attempted = false;
}
