/*
 * Penguin integration hooks.
 *
 * These hooks are intentionally small C ABI surfaces for embedding QEMU in
 * Penguin without depending on PANDA internals.
 */
#ifndef QEMU_SYSTEM_PENGUIN_H
#define QEMU_SYSTEM_PENGUIN_H

#include "qemu/typedefs.h"

typedef int (*penguin_guest_hypercall_cb_t)(CPUState *cs, uint64_t nr,
                                            uint64_t a0, uint64_t a1,
                                            uint64_t a2, uint64_t a3,
                                            uint64_t a4, uint64_t a5,
                                            uint64_t *ret, void *opaque);

typedef int (*kvm_penguin_hypercall_cb_t)(CPUState *cs, uint64_t nr,
                                          uint64_t a0, uint64_t a1,
                                          uint64_t a2, uint64_t a3,
                                          uint64_t a4, uint64_t a5,
                                          uint64_t *ret);

typedef uint64_t (*penguin_mmio_read_cb_t)(uint64_t addr, unsigned size,
                                           void *opaque);
typedef void (*penguin_mmio_write_cb_t)(uint64_t addr, uint64_t data,
                                        unsigned size, void *opaque);

void set_penguin_guest_hypercall_callback(penguin_guest_hypercall_cb_t cb,
                                          void *opaque);
void penguin_register_guest_hypercall(uint64_t nr);
void penguin_unregister_guest_hypercall(uint64_t nr);
void penguin_clear_guest_hypercalls(void);
bool penguin_guest_hypercall_registered(uint64_t nr);
void set_kvm_penguin_hypercall_callback(kvm_penguin_hypercall_cb_t cb);

bool penguin_handle_guest_hypercall(CPUState *cs, uint64_t nr,
                                    uint64_t a0, uint64_t a1,
                                    uint64_t a2, uint64_t a3,
                                    uint64_t a4, uint64_t a5,
                                    uint64_t *ret);

int penguin_qemu_add_mmio_region(uint64_t base, uint64_t size,
                                 const char *name,
                                 penguin_mmio_read_cb_t read_cb,
                                 penguin_mmio_write_cb_t write_cb,
                                 void *opaque);

/*
 * Guest register access by GDB core-feature register number. Reads append
 * the register bytes (target byte order) into @buf and return the register
 * width; writes consume exactly the register width from @buf. Both return
 * a negative value on failure.
 */
int penguin_read_guest_reg(CPUState *cs, int regnum, uint8_t *buf,
                           int buf_len);
int penguin_write_guest_reg(CPUState *cs, int regnum, const uint8_t *buf,
                            int len);

/*
 * Direct CPUArchState access. penguin_cpu_env returns the env pointer for
 * a CPU (the layout contract validated in cpu-target.c); callers decode it
 * with the build-generated CPUArchState CFFI header. penguin_sync_cpu_state
 * must be called before env reads (and to make env writes stick) under
 * hardware accelerators; it is a no-op under TCG.
 */
void *penguin_cpu_env(CPUState *cs);
void penguin_sync_cpu_state(CPUState *cs);

/*
 * Save/load an internal VM snapshot by name. Minimal C ABI wrappers around
 * save_snapshot()/load_snapshot() for Penguin's CFFI layer. Must be called
 * with the BQL held from the main loop context. Return true on success.
 */
bool penguin_save_snapshot(const char *name);
bool penguin_load_snapshot(const char *name);

/*
 * Schedule a save (load=false) or load (load=true) snapshot to run on the
 * main loop. Safe to call from a vCPU thread; fire-and-forget.
 */
void penguin_schedule_snapshot(const char *name, bool load);

/*
 * Reset request callback. Called synchronously from qemu_system_reset_request()
 * before the reset executes, when a guest-initiated reset is detected. Allows
 * Penguin to tear down and reinitialize plugins before QEMU resets the VM.
 */
typedef void (*penguin_reset_request_cb_t)(int reason, void *opaque);
void set_penguin_reset_request_callback(penguin_reset_request_cb_t cb,
                                         void *opaque);

/*
 * QMP command callback. Called from qmp_dispatch() when an incoming QMP
 * command is not a built-in QEMU command, allowing Penguin to service custom
 * QMP commands. @command is the command name, @args is the JSON-encoded
 * arguments object, and on success the handler sets *@result to a malloc'd
 * JSON string (ownership transferred to the caller) or leaves it NULL for an
 * empty response. Returns true if Penguin handled the command.
 */
typedef bool (*penguin_qmp_cb_t)(const char *command, const char *args,
                                 char **result, void *opaque);
void set_penguin_qmp_callback(penguin_qmp_cb_t cb, void *opaque);

/*
 * Dispatch a QMP command to the Penguin QMP callback if one is registered.
 * Defined in system/penguin.c and referenced as a weak symbol from
 * qapi/qmp-dispatch.c (which is also linked into tools like qemu-nbd where
 * penguin.c is absent). Returns true if the command was handled.
 */
bool penguin_handle_qmp(const char *command, const char *args, char **result);

/*
 * Emit an arbitrary QMP event to every connected QMP monitor. This is the
 * outbound counterpart to the QMP command callback above: the callback lets a
 * plugin answer commands, this lets a plugin push an unsolicited notification.
 *
 * @name becomes the QMP "event" member verbatim, so a plugin can emit any event
 * name without a QAPI schema entry. Because there is no schema entry there is
 * no QAPIEvent ordinal, so these events bypass the rate limiting in
 * monitor_qapi_event_conf[] and are never throttled or coalesced.
 *
 * @data_json is an optional JSON-encoded payload, or NULL/"" for none. It must
 * decode to a JSON *object*: the QMP spec defines the "data" member as a
 * json-object, so a scalar or array is rejected. The timestamp is added
 * automatically, as for any QMP event.
 *
 * Safe to call from any thread, including a vCPU thread inside a guest
 * hypercall handler: the work is deferred to a main-loop bottom half, so the
 * caller does not need the BQL and does not block on JSON parsing or the
 * monitor write. Fire-and-forget; an empty @name or a malformed or non-object
 * @data_json is reported to QEMU stderr and the event is dropped.
 *
 * Note that an arbitrary @name can collide with a built-in QEMU event (e.g.
 * "STOP"); choosing a distinctive name is the caller's responsibility.
 */
void penguin_qmp_emit_event(const char *name, const char *data_json);

#endif /* QEMU_SYSTEM_PENGUIN_H */
