/*
    src/init.cpp -- Initialization and shutdown of Enoki-JIT

    Copyright (c) 2020 Wenzel Jakob <wenzel.jakob@epfl.ch>

    All rights reserved. Use of this source code is governed by a BSD-style
    license that can be found in the LICENSE file.
*/

#include "internal.h"
#include "malloc.h"
#include "internal.h"
#include "log.h"
#include "registry.h"
#include "var.h"
#include "profiler.h"
#include <sys/stat.h>

#if defined(_WIN32)
#  include <windows.h>
#  include <direct.h>
#else
#  include <glob.h>
#  include <dlfcn.h>
#endif

State state;
Buffer buffer{1024};

#if !defined(_WIN32)
  char* jit_temp_path = nullptr;
#else
  wchar_t* jit_temp_path = nullptr;
#endif

#if defined(_MSC_VER)
  __declspec(thread) ThreadState* thread_state_cuda = nullptr;
  __declspec(thread) ThreadState* thread_state_llvm = nullptr;
#else
  __thread ThreadState* thread_state_cuda = nullptr;
  __thread ThreadState* thread_state_llvm = nullptr;
#endif

#if defined(ENOKI_ENABLE_ITTNOTIFY)
__itt_domain *enoki_domain = __itt_domain_create("enoki");
#endif

static_assert(
    sizeof(VariableKey) == 8 * sizeof(uint32_t),
    "VariableKey: incorrect size, likely an issue with padding/packing!");

static_assert(
    sizeof(tsl::detail_robin_hash::bucket_entry<VariableMap::value_type, false>) == 64,
    "VariableMap: incorrect bucket size, likely an issue with padding/packing!");

static ProfilerRegion profiler_region_init("jit_init");

/// Initialize core data structures of the JIT compiler
void jit_init(int llvm, int cuda) {
    ProfilerPhase profiler(profiler_region_init);

#if defined(__APPLE__)
    cuda = 0;
#endif

    if (state.has_llvm != 0 || state.has_cuda != 0 || (llvm == 0 && cuda == 0))
        return;

#if !defined(_WIN32)
    char temp_path[512];
    snprintf(temp_path, sizeof(temp_path), "%s/.enoki", getenv("HOME"));
    struct stat st = {};
    int rv = stat(temp_path, &st);
    size_t temp_path_size = (strlen(temp_path) + 1) * sizeof(char);
    jit_temp_path = (char*) malloc(temp_path_size);
    memcpy(jit_temp_path, temp_path, temp_path_size);
#else
    wchar_t temp_path_w[512];
    char temp_path[512];
    if (GetTempPathW(sizeof(temp_path_w) / sizeof(wchar_t), temp_path_w) == 0)
        jit_fail("jit_init(): could not obtain path to temporary directory!");
    wcsncat(temp_path_w, L"enoki", sizeof(temp_path) / sizeof(wchar_t));
    struct _stat st = {};
    int rv = _wstat(temp_path_w, &st);
    size_t temp_path_size = (wcslen(temp_path_w) + 1) * sizeof(wchar_t);
    jit_temp_path = (wchar_t*) malloc(temp_path_size);
    memcpy(jit_temp_path, temp_path_w, temp_path_size);
    wcstombs(temp_path, temp_path_w, sizeof(temp_path));
#endif

    if (rv == -1) {
        jit_log(Info, "jit_init(): creating directory \"%s\" ..", temp_path);
#if !defined(_WIN32)
        if (mkdir(temp_path, 0700) == -1)
#else
        if (_wmkdir(temp_path_w, 0700) == -1)
#endif
            jit_fail("jit_init(): creation of directory \"%s\" failed: %s",
                temp_path, strerror(errno));
    }

    // Enumerate CUDA devices and collect suitable ones
    jit_log(Info, "jit_init(): detecting devices ..");

    state.has_llvm = llvm && jit_llvm_init();
    state.has_cuda = cuda && jit_cuda_init();

    for (int i = 0; cuda && i < jit_cuda_devices; ++i) {
        int pci_bus_id = 0, pci_dom_id = 0, pci_dev_id = 0, num_sm = 0,
            unified_addr = 0, managed = 0, shared_memory_bytes = 0,
            cc_minor = 0, cc_major = 0;
        size_t mem_total = 0;
        char name[256];

        cuda_check(cuDeviceTotalMem(&mem_total, i));
        cuda_check(cuDeviceGetName(name, sizeof(name), i));
        cuda_check(cuDeviceGetAttribute(&pci_bus_id, CU_DEVICE_ATTRIBUTE_PCI_BUS_ID, i));
        cuda_check(cuDeviceGetAttribute(&pci_dev_id, CU_DEVICE_ATTRIBUTE_PCI_DEVICE_ID, i));
        cuda_check(cuDeviceGetAttribute(&pci_dom_id, CU_DEVICE_ATTRIBUTE_PCI_DOMAIN_ID, i));
        cuda_check(cuDeviceGetAttribute(&num_sm, CU_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT, i));
        cuda_check(cuDeviceGetAttribute(&unified_addr, CU_DEVICE_ATTRIBUTE_UNIFIED_ADDRESSING, i));
        cuda_check(cuDeviceGetAttribute(&managed, CU_DEVICE_ATTRIBUTE_MANAGED_MEMORY, i));
        cuda_check(cuDeviceGetAttribute(&shared_memory_bytes, CU_DEVICE_ATTRIBUTE_MAX_SHARED_MEMORY_PER_BLOCK_OPTIN, i));
        cuda_check(cuDeviceGetAttribute(&cc_minor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, i));
        cuda_check(cuDeviceGetAttribute(&cc_major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, i));

        jit_log(Info,
                " - Found CUDA device %i: \"%s\" "
                "(PCI ID %02x:%02x.%i, compute cap. %i.%i, %i SMs w/%s shared mem., %s global mem.)",
                i, name, pci_bus_id, pci_dev_id, pci_dom_id, cc_major, cc_minor, num_sm,
                std::string(jit_mem_string(shared_memory_bytes)).c_str(),
                std::string(jit_mem_string(mem_total)).c_str());

        if (unified_addr == 0) {
            jit_log(Warn, " - Warning: device does *not* support unified addressing, skipping ..");
            continue;
        } else if (managed == 0) {
            jit_log(Warn, " - Warning: device does *not* support managed memory, skipping ..");
            continue;
        }

        Device device;
        device.id = i;
        device.compute_capability = cc_major * 10 + cc_minor;
        device.shared_memory_bytes = (uint32_t) shared_memory_bytes;
        device.num_sm = (uint32_t) num_sm;
        cuda_check(cuDevicePrimaryCtxRetain(&device.context, i));

        scoped_set_context guard(device.context);
        for (int i = 0; i < ENOKI_SUB_STREAMS; ++i) {
            cuda_check(cuStreamCreate(&device.sub_streams[i], CU_STREAM_NON_BLOCKING));
            cuda_check(cuEventCreate(&device.sub_events[i], CU_EVENT_DISABLE_TIMING));
        }

        state.devices.push_back(device);
    }

    // Enable P2P communication if possible
    for (auto &a : state.devices) {
        for (auto &b : state.devices) {
            if (a.id == b.id)
                continue;

            int peer_ok = 0;
            scoped_set_context guard(a.context);
            cuda_check(cuDeviceCanAccessPeer(&peer_ok, a.id, b.id));
            if (peer_ok) {
                jit_log(Debug, " - Enabling peer access from device %i -> %i",
                        a.id, b.id);
                CUresult rv = cuCtxEnablePeerAccess(b.context, 0);
                if (rv == CUDA_ERROR_PEER_ACCESS_ALREADY_ENABLED)
                    continue;
                cuda_check(rv);
            }
        }
    }

    state.variable_index = 1;

    state.kernel_hard_misses = state.kernel_soft_misses = 0;
    state.kernel_hits = state.kernel_launches = 0;
}

void* jit_cuda_stream() {
    return (void*) thread_state(true)->stream;
}

void* jit_cuda_context() {
    return (void*) thread_state(true)->context;
}

/// Release all resources used by the JIT compiler, and report reference leaks.
void jit_shutdown(int light) {
    if (!state.tss.empty()) {
        jit_log(Info, "jit_shutdown(): releasing %zu thread state%s ..",
                state.tss.size(), state.tss.size() > 1 ? "s" : "");

        for (ThreadState *ts : state.tss) {
            jit_free_flush(ts);
            if (ts->cuda) {
                scoped_set_context guard(ts->context);
                cuda_check(cuStreamSynchronize(ts->stream));
                cuda_check(cuEventDestroy(ts->event));
                cuda_check(cuStreamDestroy(ts->stream));
            } else {
                task_wait_and_release(ts->task);
                if (!ts->active_mask.empty())
                    jit_log(Warn, "jit_shutdown(): leaked %zu active masks!",
                            ts->active_mask.size());
            }
            delete ts->release_chain;
            delete ts;
        }
        pool_destroy();
        state.tss.clear();
    }

    thread_state_llvm = nullptr;
    thread_state_cuda = nullptr;

    if (!state.kernel_cache.empty()) {
        jit_log(Info, "jit_shutdown(): releasing %zu kernel%s ..",
                state.kernel_cache.size(),
                state.kernel_cache.size() > 1 ? "s" : "");

        for (auto &v : state.kernel_cache) {
            jit_kernel_free(v.first.device, v.second);
            free(v.first.str);
        }

        state.kernel_cache.clear();
    }

    if (std::max(state.log_level_stderr, state.log_level_callback) >= LogLevel::Warn) {
        uint32_t n_leaked = 0;
        std::vector<uint32_t> leaked_scatters;
        for (auto &kv : state.variables) {
            const Variable &v = kv.second;
            if (v.scatter && v.ref_count_ext == 1 && v.ref_count_int == 0)
                leaked_scatters.push_back(kv.first);
        }
        for (uint32_t i : leaked_scatters)
            jit_var_dec_ref_ext(i);
        for (auto &var : state.variables) {
            if (n_leaked == 0)
                jit_log(Warn, "jit_shutdown(): detected variable leaks:");
            if (n_leaked < 10)
                jit_log(Warn,
                        " - variable %u is still being referenced! (internal "
                        "references=%u, external references=%u)",
                        var.first, var.second.ref_count_int,
                        var.second.ref_count_ext);
            else if (n_leaked == 10)
                jit_log(Warn, " - (skipping remainder)");
            ++n_leaked;
        }

        if (n_leaked > 0)
            jit_log(Warn, "jit_shutdown(): %u variables are still referenced!", n_leaked);

        if (state.variables.empty() && !state.extra.empty())
            jit_log(Warn,
                    "jit_shutdown(): %zu empty records were not cleaned up!",
                    state.extra.size());
    }

    if (state.variables.empty() && !state.cse_cache.empty()) {
        for (auto &kv: state.cse_cache)
            jit_log(Warn, " - %u: %u, %u, %u, %u", kv.second, kv.first.dep[0],
                    kv.first.dep[1], kv.first.dep[2], kv.first.dep[3]);
        jit_fail("jit_shutdown(): detected a common subexpression elimination cache leak!");
    }

    if (state.variables.empty() && !state.variable_from_ptr.empty())
        jit_fail("jit_shutdown(): detected a pointer-literal leak!");

    jit_registry_shutdown();
    jit_malloc_shutdown();

    if (state.has_cuda) {
        for (auto &v : state.devices) {
            {
                scoped_set_context guard(v.context);
                for (int i = 0; i < ENOKI_SUB_STREAMS; ++i) {
                    cuda_check(cuEventDestroy(v.sub_events[i]));
                    cuda_check(cuStreamDestroy(v.sub_streams[i]));
                }
            }
            cuda_check(cuDevicePrimaryCtxRelease(v.id));
        }
        state.devices.clear();
    }

    jit_log(Info, "jit_shutdown(): done");

    if (light == 0) {
        jit_llvm_shutdown();
        jit_cuda_shutdown();
    }

    free(jit_temp_path);
    jit_temp_path = nullptr;

    state.has_cuda = false;
    state.has_llvm = false;
}


ThreadState *jit_init_thread_state(bool cuda) {
    ThreadState *ts = new ThreadState();

    if (cuda) {
        if (!state.has_cuda) {
            #if defined(_WIN32)
                const char *cuda_fname = "nvcuda.dll";
            #elif defined(__linux__)
                const char *cuda_fname  = "libcuda.so";
            #else
                const char *cuda_fname  = "libcuda.dylib";
            #endif

            delete ts;
            jit_raise("jit_init_thread_state(): the CUDA backend is inactive "
                      "because the CUDA driver library (\"%s\") could not be "
                      "found! Set the ENOKI_LIBCUDA_PATH environment "
                      "variable to specify its path.",
                      cuda_fname);
        }

        if (state.devices.empty()) {
            delete ts;
            jit_raise("jit_init_thread_state(): the CUDA backend is inactive "
                      "because no compatible CUDA devices were found on your "
                      "system.");
        }

        ts->device = 0;
        ts->context = state.devices[ts->device].context;
        scoped_set_context guard(ts->context);
        cuda_check(cuStreamCreate(&ts->stream, CU_STREAM_NON_BLOCKING));
        cuda_check(cuEventCreate(&ts->event, CU_EVENT_DISABLE_TIMING));
        thread_state_cuda = ts;
    } else {
        if (!state.has_llvm) {
            delete ts;
            #if defined(_WIN32)
                const char *llvm_fname = "LLVM-C.dll";
            #elif defined(__linux__)
                const char *llvm_fname  = "libLLVM.so";
            #else
                const char *llvm_fname  = "libLLVM.dylib";
            #endif

            jit_raise("jit_init_thread_state(): the LLVM backend is inactive "
                      "because the LLVM shared library (\"%s\") could not be "
                      "found! Set the ENOKI_LIBLLVM_PATH environment "
                      "variable to specify its path.",
                      llvm_fname);
        }
        thread_state_llvm = ts;
        ts->device = -1;
    }

    ts->cuda = cuda;
    state.tss.push_back(ts);
    return ts;
}

void jit_cuda_set_device(int device) {
    ThreadState *ts = thread_state(true);
    if (ts->device == device)
        return;

    if ((size_t) device >= state.devices.size())
        jit_raise("jit_cuda_set_device(%i): must be in the range 0..%i!",
                  device, (int) state.devices.size() - 1);

    jit_log(Info, "jit_cuda_set_device(%i)", device);

    CUcontext new_context = state.devices[device].context;

    /* Disassociate from old context */ {
        scoped_set_context guard(ts->context);
        cuda_check(cuStreamSynchronize(ts->stream));
        cuda_check(cuEventDestroy(ts->event));
        cuda_check(cuStreamDestroy(ts->stream));
    }

    /* Associate with new context */ {
        ts->context = new_context;
        ts->device = device;
        scoped_set_context guard(ts->context);

        cuda_check(cuStreamCreate(&ts->stream, CU_STREAM_NON_BLOCKING));
        cuda_check(cuEventCreate(&ts->event, CU_EVENT_DISABLE_TIMING));
    }
}

void jit_sync_thread(ThreadState *ts) {
    if (!ts)
        return;
    if (ts->cuda) {
        scoped_set_context guard(ts->context);
        cuda_check(cuStreamSynchronize(ts->stream));
    } else {
        task_wait_and_release(ts->task);
        ts->task = nullptr;
    }
}

/// Wait for all computation on the current stream to finish
void jit_sync_thread() {
    unlock_guard guard(state.mutex);
    jit_sync_thread(thread_state_cuda);
    jit_sync_thread(thread_state_llvm);
}

/// Wait for all computation on the current device to finish
void jit_sync_device() {
    ThreadState *ts = thread_state_cuda;
    if (ts) {
        /* Release mutex while synchronizing */ {
            unlock_guard guard(state.mutex);
            scoped_set_context guard2(ts->context);
            cuda_check(cuCtxSynchronize());
        }
    }

    if (thread_state_llvm) {
        std::vector<ThreadState *> tss = state.tss;
        // Release mutex while synchronizing */
        unlock_guard guard(state.mutex);
        for (ThreadState *ts : tss) {
            if (!ts->cuda)
                jit_sync_thread(ts);
        }
    }
}

/// Wait for all computation on *all devices* to finish
void jit_sync_all_devices() {
    std::vector<ThreadState *> tss = state.tss;
    unlock_guard guard(state.mutex);
    for (ThreadState *ts : tss)
        jit_sync_thread(ts);
}

/// Glob for a shared library and try to load the most recent version
void *jit_find_library(const char *fname, const char *glob_pat,
                       const char *env_var) {
#if !defined(_WIN32)
    const char* env_var_val = env_var ? getenv(env_var) : nullptr;
    if (env_var_val != nullptr && strlen(env_var_val) == 0)
        env_var_val = nullptr;

    void* handle = dlopen(env_var_val ? env_var_val : fname, RTLD_LAZY);

    if (!handle) {
        if (env_var_val) {
            jit_log(Warn, "jit_find_library(): Unable to load \"%s\": %s!",
                    env_var_val, dlerror());
            return nullptr;
        }

        glob_t g;
        if (glob(glob_pat, GLOB_BRACE, nullptr, &g) == 0) {
            const char *chosen = nullptr;
            if (g.gl_pathc > 1) {
                jit_log(Info, "jit_find_library(): Multiple versions of "
                              "%s were found on your system!\n", fname);
                std::sort(g.gl_pathv, g.gl_pathv + g.gl_pathc,
                          [](const char *a, const char *b) {
                              while (a != nullptr && b != nullptr) {
                                  while (*a == *b && *a != '\0' && !isdigit(*a)) {
                                      ++a; ++b;
                                  }
                                  if (isdigit(*a) && isdigit(*b)) {
                                      char *ap, *bp;
                                      int ai = strtol(a, &ap, 10);
                                      int bi = strtol(b, &bp, 10);
                                      if (ai != bi)
                                          return ai < bi;
                                      a = ap;
                                      b = bp;
                                  } else {
                                      return strcmp(a, b) < 0;
                                  }
                              }
                              return false;
                          });
                uint32_t counter = 1;
                for (int j = 0; j < 2; ++j) {
                    for (size_t i = 0; i < g.gl_pathc; ++i) {
                        struct stat buf;
                        // Skip symbolic links at first
                        if (j == 0 && (lstat(g.gl_pathv[i], &buf) || S_ISLNK(buf.st_mode)))
                            continue;
                        jit_log(Info, " %u. \"%s\"", counter++, g.gl_pathv[i]);
                        chosen = g.gl_pathv[i];
                    }
                    if (chosen)
                        break;
                }
                jit_log(Info,
                        "\nChoosing the last one. Specify a path manually "
                        "using the environment\nvariable '%s' to override this "
                        "behavior.\n", env_var);
            } else if (g.gl_pathc == 1) {
                chosen = g.gl_pathv[0];
            }
            if (chosen)
                handle = dlopen(chosen, RTLD_LAZY);
            globfree(&g);
        }
    }
#else
    wchar_t buffer[1024];
    mbstowcs(buffer, env_var, sizeof(buffer) / sizeof(wchar_t));

    const wchar_t* env_var_val = env_var ? _wgetenv(buffer) : nullptr;
    if (env_var_val != nullptr && wcslen(env_var_val) == 0)
        env_var_val = nullptr;

    mbstowcs(buffer, fname, sizeof(buffer) / sizeof(wchar_t));
    void* handle = (void *) LoadLibraryW(env_var_val ? env_var_val : buffer);
#endif

    return handle;
}
