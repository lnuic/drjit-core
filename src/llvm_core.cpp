/*
    src/llvm_core.cpp -- Low-level interface to LLVM driver API

    Copyright (c) 2021 Wenzel Jakob <wenzel.jakob@epfl.ch>

    All rights reserved. Use of this source code is governed by a BSD-style
    license that can be found in the LICENSE file.
*/

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <dlfcn.h>
#  include <sys/mman.h>
#endif

#include "llvm.h"
#include "llvm_api.h"
#include "llvm_memmgr.h"
#include "internal.h"
#include "log.h"
#include "var.h"
#include "eval.h"
#include "profile.h"

static bool jitc_llvm_init_attempted  = false;
static bool jitc_llvm_init_success    = false;
static bool jitc_llvm_use_orcv2       = false;

static LLVMDisasmContextRef jitc_llvm_disasm_ctx = nullptr;
static LLVMContextRef jitc_llvm_context = nullptr;

/// String describing the LLVM target
char *jitc_llvm_target_triple = nullptr;

/// Target CPU string used by the LLVM backend
char *jitc_llvm_target_cpu = nullptr;

/// Target feature string used by the LLVM backend
char *jitc_llvm_target_features = nullptr;

/// Vector width of code generated by the LLVM backend
uint32_t jitc_llvm_vector_width = 0;
uint32_t jitc_llvm_max_align = 0;

/// Should the LLVM IR use typed (e.g., "i8*") or untyped ("ptr") pointers?
bool jitc_llvm_opaque_pointers = false;

/// Strings related to the vector width, used by template engine
char **jitc_llvm_ones_str = nullptr;

/// Current top-level task in the task queue
Task *jitc_task = nullptr;

/// Reference to the target machine used for compilation
LLVMTargetMachineRef jitc_llvm_tm = nullptr;

/// Number of work items per block handed to nanothread
uint32_t jitc_llvm_block_size = 16384;

void jitc_llvm_update_strings();

bool jitc_llvm_init() {
    if (jitc_llvm_init_attempted)
        return jitc_llvm_init_success;
    jitc_llvm_init_attempted = true;

    if (!jitc_llvm_api_init())
        return false;

    if (!jitc_llvm_api_has_core()) {
        jitc_log(Warn, "jit_llvm_init(): detected LLVM version lacks core API "
                       "used by Dr.Jit, shutting down LLVM backend ..");
        jitc_llvm_api_shutdown();
        return false;
    }

    if (!jitc_llvm_api_has_pb_new() && !jitc_llvm_api_has_pb_legacy()) {
        jitc_log(Warn, "jit_llvm_init(): detected LLVM version lacks pass "
                       "manager API used by Dr.Jit, shutting down LLVM backend ..");
        jitc_llvm_api_shutdown();
        return false;
    }


    LLVMLinkInMCJIT();
    LLVMInitializeDrJitTargetInfo();
    LLVMInitializeDrJitTarget();
    LLVMInitializeDrJitTargetMC();
    LLVMInitializeDrJitAsmPrinter();
    LLVMInitializeDrJitDisassembler();

    jitc_llvm_target_triple = LLVMGetDefaultTargetTriple();
    jitc_llvm_target_cpu = LLVMGetHostCPUName();
    jitc_llvm_target_features = LLVMGetHostCPUFeatures();
    jitc_llvm_context = LLVMGetGlobalContext();

    jitc_llvm_disasm_ctx =
        LLVMCreateDisasm(jitc_llvm_target_triple, nullptr, 0, nullptr, nullptr);

    if (jitc_llvm_disasm_ctx) {
        if (LLVMSetDisasmOptions(jitc_llvm_disasm_ctx,
                                 LLVMDisassembler_Option_PrintImmHex |
                                 LLVMDisassembler_Option_AsmPrinterVariant) == 0) {
            LLVMDisasmDispose(jitc_llvm_disasm_ctx);
            jitc_llvm_disasm_ctx = nullptr;
        }
    }

#if !defined(__aarch64__)
    if (!strstr(jitc_llvm_target_features, "+fma")) {
        jitc_log(Warn, "jit_llvm_init(): your CPU does not support the `fma` "
                       "instruction set, shutting down the LLVM "
                       "backend...");
        jitc_llvm_shutdown();
        return false;
    }
#endif

    jitc_llvm_vector_width = 1;

    if (strstr(jitc_llvm_target_features, "+sse4.2"))
        jitc_llvm_vector_width = 4;
    if (strstr(jitc_llvm_target_features, "+avx"))
        jitc_llvm_vector_width = 8;
    if (strstr(jitc_llvm_target_features, "+avx512vl"))
        jitc_llvm_vector_width = 16;

#if defined(__APPLE__) && defined(__aarch64__)
    jitc_llvm_vector_width = 4;
    LLVMDisposeMessage(jitc_llvm_target_cpu);
    const char *machine_name = "apple-a14";
    if (jitc_llvm_version_major > 15)
        machine_name = "apple-m1";
    jitc_llvm_target_cpu = LLVMCreateMessage(machine_name);
#endif

    jitc_llvm_init_success = jitc_llvm_vector_width > 1;
    jitc_llvm_max_align = jitc_llvm_vector_width * 4;

    if (!jitc_llvm_init_success) {
        jitc_log(Warn,
                 "jit_llvm_init(): no suitable vector ISA found, shutting "
                 "down LLVM backend..");
        jitc_llvm_shutdown();
    }

    if (jitc_llvm_api_has_orcv2() && jitc_llvm_orcv2_init()) {
        jitc_llvm_use_orcv2 = true;
    } else if (jitc_llvm_api_has_mcjit() && jitc_llvm_mcjit_init()) {
        jitc_llvm_use_orcv2 = false;
    } else {
        jitc_log(Warn, "jit_llvm_init(): ORCv2/MCJIT could not be initialized, "
                       "shutting down LLVM backend..");
        jitc_llvm_shutdown();
        return false;
    }

    jitc_llvm_opaque_pointers = jitc_llvm_version_major >= 15;

    jitc_llvm_update_strings();

    char major_str[5] = "?", minor_str[5] = "?", patch_str[5] = "?";

    if (jitc_llvm_version_major >= 0)
        snprintf(major_str, sizeof(major_str), "%i", jitc_llvm_version_major);
    if (jitc_llvm_version_minor >= 0)
        snprintf(minor_str, sizeof(minor_str), "%i", jitc_llvm_version_minor);
    if (jitc_llvm_version_patch >= 0)
        snprintf(patch_str, sizeof(patch_str), "%i", jitc_llvm_version_patch);

    jitc_log(Info,
             "jit_llvm_init(): found LLVM %s.%s.%s (%s), target=%s, cpu=%s, %s pointers, width=%u.",
             major_str, minor_str, patch_str,
             jitc_llvm_use_orcv2 ? "ORCv2" : "MCJIT",
             jitc_llvm_target_triple, jitc_llvm_target_cpu,
             jitc_llvm_opaque_pointers ? "opaque" : "typed",
             jitc_llvm_vector_width);

    return jitc_llvm_init_success;
}

void jitc_llvm_shutdown() {
    if (!jitc_llvm_init_success)
        return;

    jitc_log(Info, "jit_llvm_shutdown()");

    jitc_llvm_memmgr_shutdown();
    jitc_llvm_orcv2_shutdown();
    jitc_llvm_mcjit_shutdown();

    LLVMDisposeMessage(jitc_llvm_target_triple);
    LLVMDisposeMessage(jitc_llvm_target_cpu);
    LLVMDisposeMessage(jitc_llvm_target_features);
    if (jitc_llvm_disasm_ctx) {
        LLVMDisasmDispose(jitc_llvm_disasm_ctx);
        jitc_llvm_disasm_ctx = nullptr;
    }

    jitc_llvm_target_cpu = nullptr;
    jitc_llvm_target_features = nullptr;
    jitc_llvm_vector_width = 0;
    jitc_llvm_context = nullptr;

    if (jitc_llvm_ones_str) {
        for (uint32_t i = 0; i < (uint32_t) VarType::Count; ++i)
            free(jitc_llvm_ones_str[i]);
        free(jitc_llvm_ones_str);
    }
    jitc_llvm_ones_str = nullptr;

    jitc_llvm_init_success = false;
    jitc_llvm_init_attempted = false;

    jitc_llvm_api_shutdown();
}

void jitc_llvm_update_strings() {
    StringBuffer buf;
    uint32_t width = jitc_llvm_vector_width;

    buf.clear();
    if (jitc_llvm_ones_str) {
        for (uint32_t i = 0; i < (uint32_t) VarType::Count; ++i)
            free(jitc_llvm_ones_str[i]);
        free(jitc_llvm_ones_str);
    }

    jitc_llvm_ones_str =
        (char **) malloc(sizeof(char *) * (uint32_t) VarType::Count);

    for (uint32_t i = 0; i < (uint32_t) VarType::Count; ++i) {
        VarType vt = (VarType) i;

        buf.clear();
        buf.put('<');
        for (uint32_t j = 0; j < width; ++j) {
            buf.put(type_name_llvm[i], strlen(type_name_llvm[i]));
            buf.put(' ');

            if (vt == VarType::Bool)
                buf.put('1');
            else if (vt == VarType::Float16 || vt == VarType::Float32 ||
                     vt == VarType::Float64){
                buf.put("0x");
                for (uint32_t k = 0; k < 16; ++k)
                    buf.put(k < 2 * type_size[i] ? 'F' : '0');
            } else {
                buf.put("-1");
            }

            if (j + 1 < width)
                buf.put(", ");
        }
        buf.put('>');
        jitc_llvm_ones_str[i] = strdup(buf.get());
    }
}

void jitc_llvm_set_target(const char *target_cpu,
                          const char *target_features,
                          uint32_t vector_width) {
    if (!jitc_llvm_init_success)
        return;

    if (jitc_llvm_target_cpu)
        LLVMDisposeMessage(jitc_llvm_target_cpu);

    if (jitc_llvm_target_features) {
        LLVMDisposeMessage(jitc_llvm_target_features);
        jitc_llvm_target_features = nullptr;
    }

    jitc_llvm_vector_width = vector_width;
    jitc_llvm_target_cpu = LLVMCreateMessage((char *) target_cpu);
    if (target_features)
        jitc_llvm_target_features = LLVMCreateMessage((char *) target_features);

    jitc_llvm_update_strings();
}

/// Dump assembly representation
void jitc_llvm_disasm(const Kernel &kernel) {
    if (std::max(state.log_level_stderr, state.log_level_callback) <
        LogLevel::Trace)
        return;

    for (uint32_t i = 0; i < kernel.llvm.n_reloc; ++i) {
        uint8_t *func_base = (uint8_t *) kernel.llvm.reloc[i],
                *ptr = func_base;
        if (i == 1)
            continue;
        char ins_buf[256];
        bool last_nop = false;
        jitc_log(Debug, "jit_llvm_disasm(): ========== %u ==========", i);
        do {
            size_t offset      = ptr - (uint8_t *) kernel.data,
                   func_offset = ptr - func_base;
            if (offset >= kernel.size)
                break;
            size_t size =
                LLVMDisasmInstruction(jitc_llvm_disasm_ctx, ptr, kernel.size - offset,
                                      (uintptr_t) ptr, ins_buf, sizeof(ins_buf));
            if (size == 0)
                break;
            char *start = ins_buf;
            while (*start == ' ' || *start == '\t')
                ++start;
            if (strcmp(start, "nop") == 0) {
                if (!last_nop)
                    jitc_log(Debug, "jit_llvm_disasm(): ...");
                last_nop = true;
                ptr += size;
                continue;
            }
            last_nop = false;
            jitc_log(Debug, "jit_llvm_disasm(): 0x%08x   %s", (uint32_t) func_offset, start);
            if (strncmp(start, "ret", 3) == 0)
                break;
            ptr += size;
        } while (true);
    }
}

static ProfilerRegion profiler_region_llvm_compile("jit_llvm_compile");

void jitc_llvm_compile(Kernel &kernel) {
    ProfilerPhase phase(profiler_region_llvm_compile);

    jitc_llvm_memmgr_prepare(buffer.size());

    LLVMMemoryBufferRef llvm_buf = LLVMCreateMemoryBufferWithMemoryRange(
        buffer.get(), buffer.size(), kernel_name, 0);
    if (unlikely(!llvm_buf))
        jitc_fail("jit_run_compile(): could not create memory buffer!");

    // 'buf' is consumed by this function.
    LLVMModuleRef llvm_module = nullptr;
    char *error = nullptr;
    LLVMParseIRInContext(jitc_llvm_context, llvm_buf, &llvm_module, &error);
    if (unlikely(error))
        jitc_fail("jit_llvm_compile(): parsing failed. Please see the LLVM "
                  "IR and error message below:\n\n%s\n\n%s", buffer.get(), error);
    LLVMDisposeMessage(error);

// Always validate for now -- at least, until this Dr.Jit version has stabilized a bit more
// #if !defined(NDEBUG)
    bool status = LLVMVerifyModule(llvm_module, LLVMReturnStatusAction, &error);
    if (unlikely(status))
        jitc_fail("jit_llvm_compile(): module could not be verified! Please "
                  "see the LLVM IR and error message below:\n\n%s\n\n%s",
                  buffer.get(), error);
// #endif
    LLVMDisposeMessage(error);

    #define DRJIT_RUN_LEGACY_PASS_MANAGER()                                   \
        LLVMPassManagerRef jitc_llvm_pass_manager = LLVMCreatePassManager();  \
        LLVMAddLICMPass(jitc_llvm_pass_manager);                              \
        LLVMRunPassManager(jitc_llvm_pass_manager, llvm_module);              \
        LLVMDisposePassManager(jitc_llvm_pass_manager);

    #define DRJIT_RUN_NEW_PASS_MANAGER()                                      \
        LLVMPassBuilderOptionsRef pb_opt = LLVMCreatePassBuilderOptions();    \
        /* Disable some things we won't need for typical Dr.Jit programs */   \
        /* (they are already vectorized, and we don't want to make the */     \
        /* generated code even larger by unrolling it */                      \
        LLVMPassBuilderOptionsSetLoopUnrolling(pb_opt, 0);                    \
        LLVMPassBuilderOptionsSetLoopVectorization(pb_opt, 0);                \
        LLVMPassBuilderOptionsSetSLPVectorization(pb_opt, 0);                 \
        LLVMErrorRef error_ref =                                              \
            LLVMRunPasses(llvm_module, "default<O2>", jitc_llvm_tm, pb_opt);  \
        if (error_ref)                                                        \
            jitc_fail(                                                        \
                "jit_llvm_compile(): failed to run optimization passes: %s!", \
                LLVMGetErrorMessage(error_ref));                              \
        LLVMDisposePassBuilderOptions(pb_opt);

#if defined(LLVM_VERSION_MAJOR) && LLVM_VERSION_MAJOR < 15
    // Legacy pass manager, static interface to LLVM
    DRJIT_RUN_LEGACY_PASS_MANAGER();
#elif !defined(LLVM_VERSION_MAJOR)
    // Try resolving the legacy pass manager when dynamically resolving LLVM
    if (jitc_llvm_api_has_pb_legacy() && !jitc_llvm_api_has_pb_new()) {
        DRJIT_RUN_LEGACY_PASS_MANAGER();
    }
#endif

#if defined(LLVM_VERSION_MAJOR) && LLVM_VERSION_MAJOR >= 15
    // New pass manager, static interface to LLVM
    DRJIT_RUN_NEW_PASS_MANAGER();
#elif !defined(LLVM_VERSION_MAJOR)
    if (jitc_llvm_api_has_pb_new()) {
        DRJIT_RUN_NEW_PASS_MANAGER();
    }
#endif

    std::vector<uint8_t *> reloc(
        callable_count_unique ? (callable_count_unique + 2) : 1);

    if (jitc_llvm_use_orcv2)
        jitc_llvm_orcv2_compile(llvm_module, reloc);
    else
        jitc_llvm_mcjit_compile(llvm_module, reloc);

    if (jitc_llvm_memmgr_got)
        jitc_fail(
            "jit_llvm_compile(): a global offset table was generated by LLVM, "
            "which typically means that a compiler intrinsic was not supported "
            "by the target architecture. DrJit cannot handle this case "
            "and will terminate the application now. For reference, the "
            "following kernel code was responsible for this problem:\n\n%s",
            buffer.get());

#if !defined(_WIN32)
    void *ptr = mmap(nullptr, jitc_llvm_memmgr_offset, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED)
        jitc_fail("jit_llvm_compile(): could not mmap() memory: %s",
                  strerror(errno));
#else
    void *ptr = VirtualAlloc(nullptr, jitc_llvm_memmgr_offset,
                             MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!ptr)
        jitc_fail("jit_llvm_compile(): could not VirtualAlloc() memory: %u", GetLastError());
#endif
    memcpy(ptr, jitc_llvm_memmgr_data, jitc_llvm_memmgr_offset);

    kernel.data = ptr;
    kernel.size = (uint32_t) jitc_llvm_memmgr_offset;
    kernel.llvm.n_reloc = (uint32_t) reloc.size();
    kernel.llvm.reloc = (void **) malloc_check(sizeof(void *) * reloc.size());

    // Relocate function pointers
    for (size_t i = 0; i < reloc.size(); ++i)
        kernel.llvm.reloc[i] = (uint8_t *) ptr + (reloc[i] - jitc_llvm_memmgr_data);

    // Write address of @callables
    if (kernel.llvm.n_reloc > 1)
        *((void **) kernel.llvm.reloc[1]) = kernel.llvm.reloc + 1;

#if defined(DRJIT_ENABLE_ITTNOTIFY)
    kernel.llvm.itt = __itt_string_handle_create(kernel_name);
#endif

#if !defined(_WIN32)
    if (mprotect(ptr, jitc_llvm_memmgr_offset, PROT_READ | PROT_EXEC) == -1)
        jitc_fail("jit_llvm_compile(): mprotect() failed: %s", strerror(errno));
#else
    DWORD unused;
    if (VirtualProtect(ptr, jitc_llvm_memmgr_offset, PAGE_EXECUTE_READ, &unused) == 0)
        jitc_fail("jit_llvm_compile(): VirtualProtect() failed: %u", GetLastError());
#endif
}
