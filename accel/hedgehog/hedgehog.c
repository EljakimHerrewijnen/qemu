/*
 * Minimal Hedgehog-like embedding backend
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/accel.h"
#include "qemu/atomic.h"
#include "qemu/config-file.h"
#include "qemu/cutils.h"
#include "qemu/main-loop.h"
#include "qemu/option.h"
#include "qemu/module.h"
#include "qemu/rcu.h"
#include "system/replay.h"
#include "qemu/target-info.h"
#include "qemu/target-info-qom.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include "qapi/error.h"
#include "chardev/char.h"
#include "hw/core/boards.h"
#include "hw/core/qdev.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/cpu.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "accel/tcg/cpu-loop.h"
#include "exec/cpu-common.h"
#include "exec/cpu-interrupt.h"
#include "system/cpus.h"
#include "system/cpu-timers.h"
#include "system/hedgehog-exec-hooks.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "system/memory.h"
#include "system/tcg.h"
#include "system/hedgehog-backend.h"
#include "tcg/startup.h"
#include "accel/tcg/internal-common.h"
#include "system/memory-internal.h"
#include "hedgehog-mmio-device.h"
#include "hedgehog-internal.h"

typedef struct HedgehogInitState {
    bool runtime_initialized;
    bool opts_initialized;
    bool property_binding_notifier_registered;
    bool initialized;
    bool board_initialized;
    bool board_backend_active;
    /* Protected by BQL; QEMU's virtual clock is shared across backends. */
    unsigned standalone_active_runs;
    GThread *standalone_dispatcher;
    bool standalone_dispatcher_stop;
    char *machine_type;
} HedgehogInitState;

static GMutex hedgehog_init_lock;
static GMutex hedgehog_direct_run_lock;
static HedgehogInitState hedgehog_init_state;

typedef struct HedgehogChardev {
    char *id;
    char *label;
    Chardev *chr;
} HedgehogChardev;

static GPtrArray *hedgehog_chardevs;

typedef struct HedgehogPropertyBinding {
    char *object_path;
    char *property;
    char *value;
} HedgehogPropertyBinding;

static GPtrArray *hedgehog_property_bindings;
static NotifierWithReturn hedgehog_property_binding_notifier;

typedef struct HedgehogRAMRegion {
    MemoryRegion mr;
    hwaddr base;
    uint64_t size;
} HedgehogRAMRegion;

typedef struct HedgehogMMIOMapping {
    HedgehogMMIODevice *dev;
    hwaddr base;
    uint64_t size;
} HedgehogMMIOMapping;

static bool hedgehog_ranges_overlap(hwaddr lhs_base, uint64_t lhs_size,
                                   hwaddr rhs_base, uint64_t rhs_size)
{
    uint64_t lhs_start = lhs_base;
    uint64_t rhs_start = rhs_base;
    uint64_t lhs_end;
    uint64_t rhs_end;

    if (!lhs_size || !rhs_size) {
        return false;
    }

    lhs_end = lhs_start + lhs_size;
    rhs_end = rhs_start + rhs_size;
    if (lhs_end < lhs_start || rhs_end < rhs_start) {
        return true;
    }

    return lhs_start < rhs_end && rhs_start < lhs_end;
}

typedef struct HedgehogCPUOutputLink {
    HedgehogBackend *backend;
    unsigned index;
    int level;
    qemu_irq irq;
    HedgehogCPUOutputFunc callback;
    void *opaque;
} HedgehogCPUOutputLink;

typedef struct HedgehogCPUOutputEvent {
    HedgehogCPUOutputLink *link;
    bool level;
} HedgehogCPUOutputEvent;

#define HEDGEHOG_SYSTEM_CALL_OBSERVER_QUEUE_LIMIT 4096
#define HEDGEHOG_CPU_WAIT_OBSERVER_QUEUE_LIMIT 4096

typedef struct HedgehogSystemCallPending {
    HedgehogSystemCallEvent request;
    bool routed;
} HedgehogSystemCallPending;

typedef struct HedgehogCPUWaitEventNode {
    HedgehogCPUWaitEvent event;
} HedgehogCPUWaitEventNode;

static void hedgehog_backend_free_system_call_pending(void *opaque)
{
    g_free(opaque);
}

struct HedgehogBackend {
    CPUState *cpu;
    AddressSpace *active_as;
    bool board_backed;
    bool owns_cpu;
    bool owns_address_space;
    bool owns_memory_root;
    MemoryRegion root;
    AddressSpace as;
    GPtrArray *ram_regions;
    GPtrArray *mmio_mappings;
    GPtrArray *cpu_output_links;
    GQueue cpu_output_events;
    GQueue system_call_events;
    GQueue system_call_pending;
    HedgehogSystemCallObserverFunc system_call_observer;
    void *system_call_observer_opaque;
    uint64_t system_call_observer_sequence;
    uint64_t system_call_observer_dropped_events;
    uint64_t system_call_observer_dropped_pending;
    GQueue cpu_wait_events;
    HedgehogCPUWaitObserverFunc cpu_wait_observer;
    void *cpu_wait_observer_opaque;
    uint64_t cpu_wait_observer_dropped_events;
    HedgehogCPUResetObserverFunc cpu_reset_observer;
    void *cpu_reset_observer_opaque;
    bool cpu_reset_requested;
    QEMUTimer *host_timer;
    HedgehogVirtualTimerFunc host_timer_callback;
    void *host_timer_opaque;
    bool host_timer_pending;
    bool arm64_reset_state_configured;
    unsigned arm64_reset_el;
    bool arm64_reset_secure;
    bool execution_started;
    bool stop_requested;
    bool invalid_mem_seen;
    HedgehogInvalidMemInfo invalid_mem_info;
};

static void hedgehog_backend_capture_virtual_timer(void *opaque)
{
    HedgehogBackend *uc = opaque;
    BQL_LOCK_GUARD();

    uc->host_timer_pending = true;
    cpu_exit(uc->cpu);
    qemu_cond_broadcast(uc->cpu->halt_cond);
}

bool hedgehog_backend_connect_virtual_timer(HedgehogBackend *uc,
                                             HedgehogVirtualTimerFunc callback,
                                             void *opaque, Error **errp)
{
    BQL_LOCK_GUARD();

    if (!uc || uc->board_backed || (callback && uc->execution_started)) {
        error_setg(errp, "Virtual timer connection requires a fresh standalone backend");
        return false;
    }
    if (uc->host_timer) {
        timer_del(uc->host_timer);
    } else if (callback) {
        uc->host_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                     hedgehog_backend_capture_virtual_timer, uc);
    }
    uc->host_timer_callback = callback;
    uc->host_timer_opaque = opaque;
    uc->host_timer_pending = false;
    return true;
}

bool hedgehog_backend_arm_virtual_timer(HedgehogBackend *uc, int64_t deadline_ns)
{
    BQL_LOCK_GUARD();

    if (!uc || !uc->host_timer || !uc->host_timer_callback) {
        return false;
    }
    uc->host_timer_pending = false;
    if (deadline_ns < 0) {
        timer_del(uc->host_timer);
    } else {
        timer_mod_ns(uc->host_timer, deadline_ns);
    }
    qemu_notify_event();
    return true;
}

static void hedgehog_backend_capture_cpu_output(void *opaque, int index, int level)
{
    HedgehogCPUOutputLink *link = opaque;
    HedgehogBackend *uc = link->backend;
    HedgehogCPUOutputEvent *event;
    BQL_LOCK_GUARD();

    if (link->level == !!level) {
        return;
    }
    link->level = !!level;
    if (!link->callback) {
        return;
    }
    event = g_new(HedgehogCPUOutputEvent, 1);
    event->link = link;
    event->level = !!level;
    g_queue_push_tail(&uc->cpu_output_events, event);
    cpu_exit(uc->cpu);
    qemu_cond_broadcast(uc->cpu->halt_cond);
}

bool hedgehog_backend_connect_cpu_output(HedgehogBackend *uc, unsigned index,
                                         HedgehogCPUOutputFunc callback,
                                         void *opaque, Error **errp)
{
    NamedGPIOList *gpios;
    HedgehogCPUOutputLink *link;
    unsigned i;
    int outputs = 0;
    BQL_LOCK_GUARD();

    if (!uc || uc->board_backed || (callback && uc->execution_started)) {
        error_setg(errp, "CPU output connection requires a fresh standalone backend");
        return false;
    }
    for (i = 0; i < uc->cpu_output_links->len; i++) {
        link = g_ptr_array_index(uc->cpu_output_links, i);
        if (link->index == index) {
            link->callback = callback;
            link->opaque = opaque;
            return true;
        }
    }
    QLIST_FOREACH(gpios, &DEVICE(uc->cpu)->gpios, node) {
        if (!gpios->name) {
            outputs = gpios->num_out;
            break;
        }
    }
    if (!callback || index >= outputs) {
        error_setg(errp, "CPU anonymous output index is not available");
        return false;
    }
    link = g_new0(HedgehogCPUOutputLink, 1);
    link->backend = uc;
    link->index = index;
    link->callback = callback;
    link->opaque = opaque;
    link->irq = qemu_allocate_irq(hedgehog_backend_capture_cpu_output, link, index);
    qdev_connect_gpio_out(DEVICE(uc->cpu), index, link->irq);
    g_ptr_array_add(uc->cpu_output_links, link);
    return true;
}

static void hedgehog_backend_dispatch_cpu_outputs(HedgehogBackend *uc)
{
    GQueue events = G_QUEUE_INIT;
    HedgehogCPUOutputEvent *event;
    bool timer_pending;

    /* Only the direct-run caller invokes host callbacks, outside BQL. Native
     * timer dispatch may hold BQL and must never acquire a Python GIL there. */
    g_assert(!bql_locked());
    {
        BQL_LOCK_GUARD();
        events = uc->cpu_output_events;
        g_queue_init(&uc->cpu_output_events);
        timer_pending = uc->host_timer_pending;
        uc->host_timer_pending = false;
    }
    if (timer_pending && uc->host_timer_callback) {
        uc->host_timer_callback(uc->host_timer_opaque);
    }
    while ((event = g_queue_pop_head(&events))) {
        HedgehogCPUOutputLink *link = event->link;
        if (link->callback) {
            link->callback(link->opaque, link->index, event->level);
        }
        g_free(event);
    }
}

static void hedgehog_backend_clear_system_call_events_locked(HedgehogBackend *uc)
{
    g_queue_clear_full(&uc->system_call_events, g_free);
    g_queue_clear_full(&uc->system_call_pending,
                       hedgehog_backend_free_system_call_pending);
}

bool hedgehog_backend_connect_system_call_observer(
    HedgehogBackend *uc, HedgehogSystemCallObserverFunc callback,
    void *opaque, Error **errp)
{
    BQL_LOCK_GUARD();

    if (!uc || uc->board_backed || (callback && uc->execution_started)) {
        error_setg(errp,
                   "System-call observation requires a fresh standalone backend");
        return false;
    }
    hedgehog_backend_clear_system_call_events_locked(uc);
    uc->system_call_observer = callback;
    uc->system_call_observer_opaque = opaque;
    uc->system_call_observer_sequence = 0;
    uc->system_call_observer_dropped_events = 0;
    uc->system_call_observer_dropped_pending = 0;
    return true;
}

static void hedgehog_backend_queue_system_call_event_locked(
    HedgehogBackend *uc, const HedgehogSystemCallEvent *event)
{
    HedgehogSystemCallEvent *copy = g_new(HedgehogSystemCallEvent, 1);

    if (g_queue_get_length(&uc->system_call_events) >=
        HEDGEHOG_SYSTEM_CALL_OBSERVER_QUEUE_LIMIT) {
        g_free(g_queue_pop_head(&uc->system_call_events));
        uc->system_call_observer_dropped_events++;
    }
    *copy = *event;
    g_queue_push_tail(&uc->system_call_events, copy);
    /* Yield at an instruction boundary so an unbounded direct run can deliver
     * the observer event on its caller thread before resuming guest work. */
    cpu_exit(uc->cpu);
    qemu_cond_broadcast(uc->cpu->halt_cond);
}

void hedgehog_backend_report_system_call_request(
    HedgehogBackend *uc, HedgehogSystemCallKind kind, vaddr pc,
    unsigned source_el, unsigned immediate, const uint64_t x[8])
{
    HedgehogSystemCallEvent event = { 0 };
    HedgehogSystemCallPending *pending;

    if (!uc || !x) {
        return;
    }
    BQL_LOCK_GUARD();
    if (!uc->system_call_observer) {
        return;
    }
    event.sequence = ++uc->system_call_observer_sequence;
    event.pc = pc;
    event.return_pc = pc + 4;
    memcpy(event.x, x, sizeof(event.x));
    event.source_el = source_el;
    event.target_el = UINT32_MAX;
    event.return_el = UINT32_MAX;
    event.immediate = immediate & 0xffff;
    event.kind = kind;
    event.phase = HEDGEHOG_SYSTEM_CALL_EVENT_REQUEST;
    pending = g_new0(HedgehogSystemCallPending, 1);
    pending->request = event;
    if (g_queue_get_length(&uc->system_call_pending) >=
        HEDGEHOG_SYSTEM_CALL_OBSERVER_QUEUE_LIMIT) {
        hedgehog_backend_free_system_call_pending(
            g_queue_pop_head(&uc->system_call_pending));
        uc->system_call_observer_dropped_pending++;
    }
    g_queue_push_tail(&uc->system_call_pending, pending);
    hedgehog_backend_queue_system_call_event_locked(uc, &event);
}

void hedgehog_backend_report_system_call_route(
    HedgehogBackend *uc, HedgehogSystemCallKind kind, vaddr return_pc,
    unsigned target_el)
{
    GList *link;

    if (!uc) {
        return;
    }
    BQL_LOCK_GUARD();
    if (!uc->system_call_observer) {
        return;
    }
    for (link = uc->system_call_pending.tail; link; link = link->prev) {
        HedgehogSystemCallPending *candidate = link->data;

        if (candidate->request.kind == kind &&
            candidate->request.return_pc == return_pc && !candidate->routed) {
            candidate->request.target_el = target_el;
            candidate->routed = true;
            return;
        }
    }
}

void hedgehog_backend_report_system_call_complete(
    HedgehogBackend *uc, vaddr return_pc, unsigned exception_el,
    unsigned return_el, const uint64_t x[8])
{
    HedgehogSystemCallPending *pending = NULL;
    HedgehogSystemCallEvent event;
    GList *link;

    if (!uc || !x) {
        return;
    }
    BQL_LOCK_GUARD();
    if (!uc->system_call_observer) {
        return;
    }
    for (link = uc->system_call_pending.tail; link; link = link->prev) {
        HedgehogSystemCallPending *candidate = link->data;

        if (candidate->routed && candidate->request.return_pc == return_pc &&
            candidate->request.target_el == exception_el) {
            pending = candidate;
            g_queue_delete_link(&uc->system_call_pending, link);
            break;
        }
    }
    if (!pending) {
        /* Keep a truthful request-only row for host-handled, non-returning,
         * evicted, and ERET-from-the-wrong-EL calls. */
        return;
    }
    event = pending->request;
    g_free(pending);
    memcpy(event.x, x, sizeof(event.x));
    event.return_el = return_el;
    event.phase = HEDGEHOG_SYSTEM_CALL_EVENT_COMPLETE;
    hedgehog_backend_queue_system_call_event_locked(uc, &event);
}

static void hedgehog_backend_dispatch_system_call_events(HedgehogBackend *uc)
{
    GQueue events = G_QUEUE_INIT;
    HedgehogSystemCallObserverFunc callback;
    void *opaque;
    HedgehogSystemCallEvent *event;

    /* Python/ctypes callbacks must never run while QEMU owns BQL. */
    g_assert(!bql_locked());
    {
        BQL_LOCK_GUARD();
        events = uc->system_call_events;
        g_queue_init(&uc->system_call_events);
        callback = uc->system_call_observer;
        opaque = uc->system_call_observer_opaque;
    }
    while ((event = g_queue_pop_head(&events))) {
        if (callback) {
            callback(opaque, event);
        }
        g_free(event);
    }
}

static void hedgehog_backend_clear_cpu_wait_events_locked(HedgehogBackend *uc)
{
    g_queue_clear_full(&uc->cpu_wait_events, g_free);
}

bool hedgehog_backend_connect_cpu_wait_observer(
    HedgehogBackend *uc, HedgehogCPUWaitObserverFunc callback,
    void *opaque, Error **errp)
{
    BQL_LOCK_GUARD();

    if (!uc || uc->board_backed || (callback && uc->execution_started)) {
        error_setg(errp,
                   "CPU-wait observation requires a fresh standalone backend");
        return false;
    }
    hedgehog_backend_clear_cpu_wait_events_locked(uc);
    uc->cpu_wait_observer = callback;
    uc->cpu_wait_observer_opaque = opaque;
    uc->cpu_wait_observer_dropped_events = 0;
    return true;
}

void hedgehog_backend_report_cpu_wait(
    HedgehogBackend *uc, HedgehogCPUWaitKind kind, vaddr pc,
    unsigned current_el)
{
    HedgehogCPUWaitEventNode *node;

    if (!uc) {
        return;
    }
    BQL_LOCK_GUARD();
    if (!uc->cpu_wait_observer) {
        return;
    }
    if (g_queue_get_length(&uc->cpu_wait_events) >=
        HEDGEHOG_CPU_WAIT_OBSERVER_QUEUE_LIMIT) {
        g_free(g_queue_pop_head(&uc->cpu_wait_events));
        uc->cpu_wait_observer_dropped_events++;
    }
    node = g_new0(HedgehogCPUWaitEventNode, 1);
    node->event.pc = pc;
    node->event.current_el = current_el;
    node->event.kind = kind;
    g_queue_push_tail(&uc->cpu_wait_events, node);
    /* Deliver the observation before standalone idle wait blocks. */
    cpu_exit(uc->cpu);
    qemu_cond_broadcast(uc->cpu->halt_cond);
}

static void hedgehog_backend_dispatch_cpu_wait_events(HedgehogBackend *uc)
{
    GQueue events = G_QUEUE_INIT;
    HedgehogCPUWaitObserverFunc callback;
    void *opaque;
    HedgehogCPUWaitEventNode *node;

    /* Host callbacks execute only on the direct-run caller, outside BQL. */
    g_assert(!bql_locked());
    {
        BQL_LOCK_GUARD();
        events = uc->cpu_wait_events;
        g_queue_init(&uc->cpu_wait_events);
        callback = uc->cpu_wait_observer;
        opaque = uc->cpu_wait_observer_opaque;
    }
    while ((node = g_queue_pop_head(&events))) {
        if (callback) {
            callback(opaque, &node->event);
        }
        g_free(node);
    }
}

bool hedgehog_backend_connect_cpu_reset_observer(
    HedgehogBackend *uc, HedgehogCPUResetObserverFunc callback,
    void *opaque, Error **errp)
{
    BQL_LOCK_GUARD();

    if (!uc || uc->board_backed || (callback && uc->execution_started)) {
        error_setg(errp,
                   "CPU-reset observation requires a fresh standalone backend");
        return false;
    }
    uc->cpu_reset_observer = callback;
    uc->cpu_reset_observer_opaque = opaque;
    return true;
}

static void hedgehog_backend_dispatch_cpu_reset_observer(HedgehogBackend *uc)
{
    HedgehogCPUResetObserverFunc callback;
    void *opaque;

    /* The caller applies the queued reset before this notification and is
     * outside BQL, so an embedding can replay its own retained input level. */
    g_assert(!bql_locked());
    {
        BQL_LOCK_GUARD();
        callback = uc->cpu_reset_observer;
        opaque = uc->cpu_reset_observer_opaque;
    }
    if (callback) {
        callback(opaque);
    }
}

static void hedgehog_backend_process_queued_cpu_work(CPUState *cpu)
{
    BQL_LOCK_GUARD();

    /*
     * Mirror qemu_process_cpu_events(): cpu_exit() is also QEMU's wakeup
     * mechanism for queued CPU work, so its exit_request is consumed before
     * that work runs.  Leaving it set makes every later cpu_exec() return
     * EXCP_INTERRUPT; this is observable after broadcast TLB maintenance,
     * which queues work even for a standalone CPU.  Hedgehog's asynchronous
     * stop remains authoritative in the separate stop_requested flag.
     */
    qatomic_set(&cpu->exit_request, false);
    process_queued_cpu_work(cpu);
}

static void hedgehog_backend_keep_standalone_cpu_stopped(HedgehogBackend *uc)
{
    if (!uc || uc->board_backed || !uc->cpu || !uc->cpu->created ||
        qemu_cpu_is_self(uc->cpu)) {
        return;
    }

    /*
     * Standalone Hedgehog owns execution by calling cpu_exec() directly. Keep
     * QEMU's scheduler thread parked so queued CPU work cannot race the direct
     * runner and observe this CPU as already running.
     */
    uc->cpu->stopped = true;
    if (qatomic_read(&uc->cpu->running)) {
        uc->cpu->stop = true;
        cpu_exit(uc->cpu);
        while (qatomic_read(&uc->cpu->running)) {
            g_usleep(1000);
        }
        uc->cpu->stop = false;
        uc->cpu->stopped = true;
    }
}

static gpointer hedgehog_standalone_dispatch_events(gpointer unused)
{
    rcu_register_thread();
    replay_mutex_lock();
    bql_lock();
    while (!hedgehog_init_state.standalone_dispatcher_stop) {
        main_loop_wait(false);
    }
    bql_unlock();
    replay_mutex_unlock();
    rcu_unregister_thread();
    return NULL;
}

static void hedgehog_backend_begin_direct_run(HedgehogBackend *uc)
{
    if (uc->board_backed) {
        return;
    }
    /* Wake an explicit blocking event poll before taking loop ownership. */
    qemu_notify_event();
    g_mutex_lock(&hedgehog_direct_run_lock);
    bql_lock();
    if (hedgehog_init_state.standalone_active_runs++ == 0) {
        cpu_enable_ticks();
        qemu_clock_enable(QEMU_CLOCK_VIRTUAL, true);
        hedgehog_init_state.standalone_dispatcher_stop = false;
        hedgehog_init_state.standalone_dispatcher = g_thread_new(
            "hedgehog-events", hedgehog_standalone_dispatch_events, NULL);
    }
    hedgehog_exec_hook_set_direct_run(uc->cpu, true);
    bql_unlock();
    g_mutex_unlock(&hedgehog_direct_run_lock);
}

static void hedgehog_backend_end_direct_run(HedgehogBackend *uc)
{
    GThread *dispatcher = NULL;

    if (uc->board_backed) {
        return;
    }
    g_mutex_lock(&hedgehog_direct_run_lock);
    bql_lock();
    hedgehog_exec_hook_set_direct_run(uc->cpu, false);
    g_assert(hedgehog_init_state.standalone_active_runs > 0);
    if (--hedgehog_init_state.standalone_active_runs == 0) {
        dispatcher = hedgehog_init_state.standalone_dispatcher;
        hedgehog_init_state.standalone_dispatcher_stop = true;
        qemu_notify_event();
    }
    bql_unlock();
    /* Never join while holding BQL: the dispatcher needs it to exit. */
    if (dispatcher) {
        g_thread_join(dispatcher);
        bql_lock();
        hedgehog_init_state.standalone_dispatcher = NULL;
        cpu_disable_ticks();
        qemu_clock_enable(QEMU_CLOCK_VIRTUAL, false);
        bql_unlock();
    }
    g_mutex_unlock(&hedgehog_direct_run_lock);
}

int64_t hedgehog_backend_virtual_clock_ns(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static void hedgehog_backend_wait_for_interrupt(HedgehogBackend *uc)
{
    BQL_LOCK_GUARD();

    while (!qatomic_read(&uc->stop_requested) && uc->cpu->halted &&
           !cpu_has_work(uc->cpu) && !uc->host_timer_pending &&
           g_queue_is_empty(&uc->cpu_output_events)) {
        qemu_cond_wait_bql(uc->cpu->halt_cond);
    }
}


static bool hedgehog_backend_apply_arm64_reset_state(HedgehogBackend *uc,
                                                     Error **errp)
{
    const HedgehogArchOps *ops = hedgehog_arch_ops();

    if (!uc->arm64_reset_state_configured) {
        return true;
    }
    if (!ops || !ops->apply_aarch64_reset_state) {
        error_setg(errp, "AArch64 reset state is unavailable for this target");
        return false;
    }
    return ops->apply_aarch64_reset_state(uc->cpu, uc->arm64_reset_el,
                                          uc->arm64_reset_secure, errp);
}

static void hedgehog_backend_free_property_binding(void *opaque)
{
    HedgehogPropertyBinding *binding = opaque;

    if (!binding) {
        return;
    }

    g_free(binding->object_path);
    g_free(binding->property);
    g_free(binding->value);
    g_free(binding);
}

static char *hedgehog_backend_normalize_object_path(const char *object_path)
{
    return object_path[0] == '/' ?
        g_strdup(object_path) : g_strconcat("/", object_path, NULL);
}

static HedgehogChardev *hedgehog_backend_find_chardev(const char *id)
{
    guint i;

    if (!hedgehog_chardevs || !id) {
        return NULL;
    }

    for (i = 0; i < hedgehog_chardevs->len; i++) {
        HedgehogChardev *entry = g_ptr_array_index(hedgehog_chardevs, i);

        if (g_strcmp0(entry->id, id) == 0) {
            return entry;
        }
    }

    return NULL;
}

static HedgehogPropertyBinding *hedgehog_backend_find_property_binding(
    const char *object_path,
    const char *property)
{
    guint i;

    if (!hedgehog_property_bindings) {
        return NULL;
    }

    for (i = 0; i < hedgehog_property_bindings->len; i++) {
        HedgehogPropertyBinding *binding =
            g_ptr_array_index(hedgehog_property_bindings, i);

        if (g_strcmp0(binding->object_path, object_path) == 0 &&
            g_strcmp0(binding->property, property) == 0) {
            return binding;
        }
    }

    return NULL;
}

static bool hedgehog_backend_set_property_string_locked(const char *object_path,
                                                        const char *property,
                                                        const char *value,
                                                        Error **errp)
{
    Object *obj;
    bool ambiguous = false;

    obj = object_resolve_path(object_path, &ambiguous);
    if (!obj) {
        if (ambiguous) {
            error_setg(errp, "object path '%s' is ambiguous", object_path);
        } else {
            error_setg(errp, "object path '%s' was not found", object_path);
        }
        return false;
    }

    if (!object_property_find_err(obj, property, errp)) {
        return false;
    }

    /* Try string first; fall back to bool for boolean properties. */
    {
        Error *local_err = NULL;
        if (object_property_set_str(obj, property, value, &local_err)) {
            return true;
        }
        error_free(local_err);
    }
    if (g_strcmp0(value, "true") == 0 || g_strcmp0(value, "on") == 0) {
        return object_property_set_bool(obj, property, true, errp);
    }
    if (g_strcmp0(value, "false") == 0 || g_strcmp0(value, "off") == 0) {
        return object_property_set_bool(obj, property, false, errp);
    }
    {
        uint64_t uint_value;

        if (parse_uint_full(value, 0, &uint_value) == 0 &&
            uint_value <= INT64_MAX) {
            Error *local_err = NULL;

            if (object_property_set_int(obj, property, (int64_t)uint_value,
                                        &local_err)) {
                return true;
            }
            error_free(local_err);
        }
    }
    return object_property_set_str(obj, property, value, errp);
}

static bool hedgehog_backend_apply_available_property_bindings(Error **errp)
{
    gint i;

    if (!hedgehog_property_bindings || hedgehog_property_bindings->len == 0) {
        return true;
    }

    for (i = hedgehog_property_bindings->len - 1; i >= 0; i--) {
        HedgehogPropertyBinding *binding =
            g_ptr_array_index(hedgehog_property_bindings, i);
        bool ambiguous = false;

        if (!object_resolve_path(binding->object_path, &ambiguous)) {
            if (ambiguous) {
                error_setg(errp, "object path '%s' is ambiguous",
                           binding->object_path);
                return false;
            }
            continue;
        }

        if (!hedgehog_backend_set_property_string_locked(binding->object_path,
                                                         binding->property,
                                                         binding->value,
                                                         errp)) {
            error_prepend(errp, "failed to bind property %s:%s=%s: ",
                          binding->object_path, binding->property,
                          binding->value);
            return false;
        }

        g_ptr_array_remove_index(hedgehog_property_bindings, i);
    }

    return true;
}

static bool hedgehog_backend_require_all_property_bindings_resolved(Error **errp)
{
    HedgehogPropertyBinding *binding;

    if (!hedgehog_property_bindings || hedgehog_property_bindings->len == 0) {
        return true;
    }

    binding = g_ptr_array_index(hedgehog_property_bindings, 0);
    error_setg(errp,
               "property binding %s:%s=%s was not resolved during machine initialization",
               binding->object_path, binding->property, binding->value);
    return false;
}

static int hedgehog_backend_apply_bindings_to_object(Object *obj, void *opaque)
{
    Error **errp = opaque;
    g_autofree char *path = object_get_canonical_path(obj);
    gint i;

    if (!path || !hedgehog_property_bindings || hedgehog_property_bindings->len == 0) {
        return 0;
    }

    for (i = hedgehog_property_bindings->len - 1; i >= 0; i--) {
        HedgehogPropertyBinding *binding =
            g_ptr_array_index(hedgehog_property_bindings, i);

        if (g_strcmp0(binding->object_path, path) != 0) {
            continue;
        }

        if (!object_property_find_err(obj, binding->property, errp)) {
            error_prepend(errp, "failed to bind property %s:%s=%s: ",
                          binding->object_path, binding->property,
                          binding->value);
            return -1;
        }

        if (!object_property_set_str(obj, binding->property,
                                     binding->value, errp)) {
            error_prepend(errp, "failed to bind property %s:%s=%s: ",
                          binding->object_path, binding->property,
                          binding->value);
            return -1;
        }

        g_ptr_array_remove_index(hedgehog_property_bindings, i);
    }

    return 0;
}

static int hedgehog_backend_on_object_initialized(NotifierWithReturn *notifier,
                                                  void *data,
                                                  Error **errp)
{
    ObjectInitializeChildEvent *event = data;

    if (!hedgehog_property_bindings || hedgehog_property_bindings->len == 0) {
        return 0;
    }

    (void)notifier;

    if (hedgehog_backend_apply_bindings_to_object(event->child, errp) != 0) {
        return -1;
    }

    if (object_child_foreach_recursive(event->child,
                                       hedgehog_backend_apply_bindings_to_object,
                                       errp) != 0) {
        return -1;
    }

    if (!hedgehog_backend_apply_available_property_bindings(errp)) {
        return -1;
    }

    return 0;
}

static bool hedgehog_backend_ensure_runtime_initialized(Error **errp)
{
    Error *local_err = NULL;

    g_mutex_lock(&hedgehog_init_lock);
    if (hedgehog_init_state.runtime_initialized) {
        g_mutex_unlock(&hedgehog_init_lock);
        return true;
    }

    module_call_init(MODULE_INIT_TARGET_INFO);
    target_info_qom_set_target();
    qemu_init_subsystems();
    if (!hedgehog_init_state.opts_initialized) {
        qemu_add_opts(&qemu_chardev_opts);
        qemu_add_opts(&qemu_device_opts);
        qemu_add_opts(&qemu_global_opts);
        qemu_add_opts(&qemu_semihosting_config_opts);
        hedgehog_init_state.opts_initialized = true;
    }
    if (!hedgehog_init_state.property_binding_notifier_registered) {
        hedgehog_property_binding_notifier.notify =
            hedgehog_backend_on_object_initialized;
        object_add_initialize_child_notifier(&hedgehog_property_binding_notifier);
        hedgehog_init_state.property_binding_notifier_registered = true;
    }
    if (qemu_init_main_loop(&local_err) < 0) {
        bql_unlock();
        g_mutex_unlock(&hedgehog_init_lock);
        if (local_err) {
            error_propagate(errp, local_err);
        } else {
            error_setg(errp, "failed to initialize qemu main loop");
        }
        return false;
    }
    bql_unlock();

    hedgehog_init_state.runtime_initialized = true;
    g_mutex_unlock(&hedgehog_init_lock);
    return true;
}

static void hedgehog_backend_request_stop_from_hook(void *opaque,
                                                   HedgehogExecStopReason reason,
                                                   const HedgehogInvalidMemInfo *info)
{
    HedgehogBackend *uc = opaque;

    if (reason == HEDGEHOG_EXEC_STOP_INVALID_MEMORY) {
        uc->invalid_mem_seen = true;
        if (info) {
            uc->invalid_mem_info = *info;
        }
    }

    hedgehog_backend_stop(uc);
}

static void hedgehog_backend_create_machine_containers(Object *machine)
{
    static const char *const containers[] = {
        "unattached",
        "peripheral",
        "peripheral-anon",
    };
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(containers); i++) {
        if (!object_resolve_path_component(machine, containers[i])) {
            object_property_add_new_container(machine, containers[i]);
        }
    }

    if (!object_resolve_path_component(machine_get_container("unattached"),
                                       "sysbus")) {
        object_property_add_child(machine_get_container("unattached"),
                                  "sysbus", OBJECT(sysbus_get_default()));
    }
}

static char *hedgehog_backend_canonicalize_machine_type(const char *machine_type)
{
    const char *type = machine_type && machine_type[0] ? machine_type : "none";
    size_t suffix_len = strlen(TYPE_MACHINE_SUFFIX);
    size_t type_len = strlen(type);

    if (type_len > suffix_len &&
        g_str_has_suffix(type, TYPE_MACHINE_SUFFIX)) {
        return g_strndup(type, type_len - suffix_len);
    }

    return g_strdup(type);
}

static char *hedgehog_backend_machine_type_from_obj(Object *obj)
{
    const char *qom_type = object_get_typename(obj);

    return hedgehog_backend_canonicalize_machine_type(qom_type);
}

static MachineClass *hedgehog_backend_find_machine_class(const char *name)
{
    g_autoptr(GSList) machines =
        object_class_get_list(TYPE_MACHINE, false);
    GSList *el;

    for (el = machines; el; el = el->next) {
        MachineClass *mc = el->data;

        if (!strcmp(mc->name, name) || !g_strcmp0(mc->alias, name)) {
            return mc;
        }
    }

    return NULL;
}

static bool hedgehog_backend_create_machine(const char *machine_type,
                                            Error **errp)
{
    g_autofree char *requested = NULL;
    g_autofree char *existing = NULL;
    Object *machine;
    MachineClass *machine_class;

    requested = hedgehog_backend_canonicalize_machine_type(machine_type);
    machine = object_resolve_path_component(object_get_root(), "machine");

    if (machine) {
        if (!object_dynamic_cast(machine, TYPE_MACHINE)) {
            error_setg(errp, "existing /machine object is not a MachineState");
            return false;
        }

        if (!current_machine) {
            current_machine = MACHINE(machine);
        }

        hedgehog_backend_create_machine_containers(machine);

        existing = hedgehog_backend_machine_type_from_obj(machine);
        if (g_strcmp0(existing, requested) != 0) {
            error_setg(errp,
                       "requested machine '%s' but existing machine is '%s'",
                       requested, existing);
            return false;
        }
        return true;
    }

    machine_class = hedgehog_backend_find_machine_class(requested);
    if (!machine_class) {
        error_setg(errp, "unknown machine type '%s'", requested);
        return false;
    }

    object_set_machine_compat_props(machine_class->compat_props);

    current_machine =
        MACHINE(object_new_with_class(OBJECT_CLASS(machine_class)));
    object_property_add_child(object_get_root(), "machine", OBJECT(current_machine));
    hedgehog_backend_create_machine_containers(OBJECT(current_machine));

    return true;
}

static void hedgehog_backend_init_tcg_accel(void)
{
    AccelClass *ac;
    AccelState *accel;
    int ret;

    if (current_machine && current_machine->accelerator) {
        return;
    }

    if (!current_machine) {
        Object *machine = object_resolve_path_component(object_get_root(), "machine");

        if (machine && object_dynamic_cast(machine, TYPE_MACHINE)) {
            current_machine = MACHINE(machine);
        }
    }

    g_assert(current_machine);

    ac = accel_find("tcg");
    g_assert(ac);

    accel = ACCEL(object_new_with_class(OBJECT_CLASS(ac)));
    ret = accel_init_machine(accel, current_machine);
    g_assert(ret == 0);

    accel_init_interfaces(ACCEL_GET_CLASS(current_machine->accelerator));
}

static void hedgehog_backend_advance_machine_phase(MachineInitPhase phase)
{
    if (!phase_check(phase)) {
        phase_advance(phase);
    }
}

static void hedgehog_backend_advance_machine_phases(void)
{
    hedgehog_backend_advance_machine_phase(PHASE_MACHINE_CREATED);
    hedgehog_backend_advance_machine_phase(PHASE_ACCEL_CREATED);
    hedgehog_backend_advance_machine_phase(PHASE_LATE_BACKENDS_CREATED);
}

static bool hedgehog_backend_is_board_machine(const char *machine_type)
{
    g_autofree char *canonical = hedgehog_backend_canonicalize_machine_type(machine_type);

    return strcmp(canonical, "none") != 0;
}

static CPUState *hedgehog_backend_first_realized_cpu(void)
{
    CPUState *cpu;

    CPU_FOREACH(cpu) {
        if (DEVICE(cpu)->realized) {
            return cpu;
        }
    }

    return NULL;
}

static ObjectClass *hedgehog_backend_lookup_cpu_class(const char *cpu_type,
                                                      Error **errp)
{
    ObjectClass *cpu_class;

    cpu_class = module_object_class_by_name(cpu_type);
    if (!cpu_class) {
        cpu_class = cpu_class_by_name(target_cpu_type(), cpu_type);
    }
    if (!cpu_class) {
        error_setg(errp, "unknown cpu type '%s' for target '%s'",
                   cpu_type, target_cpu_type());
        return NULL;
    }
    if (object_class_is_abstract(cpu_class)) {
        error_setg(errp, "cpu type '%s' is abstract; use a concrete model",
                   cpu_type);
        return NULL;
    }

    return cpu_class;
}

static bool hedgehog_backend_realize_board_machine(const char *cpu_type,
                                                   Error **errp)
{
    const char *default_cpu_type;
    CPUState *cpu;
    ObjectClass *cpu_class;
    Error *local_err = NULL;

    if (!current_machine) {
        error_setg(errp, "failed to realize board machine: no current machine");
        return false;
    }

    if (!current_machine->cpu_type) {
        if (cpu_type && cpu_type[0]) {
            /* Prefer the user-provided CPU type over the machine default. */
            cpu_class = hedgehog_backend_lookup_cpu_class(cpu_type, errp);
            if (!cpu_class) {
                return false;
            }
            current_machine->cpu_type = object_class_get_name(cpu_class);
        } else {
            default_cpu_type = machine_default_cpu_type(current_machine);
            if (default_cpu_type) {
                current_machine->cpu_type = default_cpu_type;
            } else {
                error_setg(errp,
                           "machine '%s' does not provide a default CPU type",
                           object_get_typename(OBJECT(current_machine)));
                return false;
            }
        }
    }

    /*
     * Apply CPU properties as QEMU globals so they take effect before
     * the board init realizes the CPU.
     */
    if (hedgehog_property_bindings) {
        for (guint bi = hedgehog_property_bindings->len; bi > 0; bi--) {
            HedgehogPropertyBinding *binding =
                g_ptr_array_index(hedgehog_property_bindings, bi - 1);

            if (current_machine->cpu_type &&
                g_strcmp0(binding->object_path,
                          "/machine/unattached/cpu") == 0) {
                /*
                 * Set the property directly on the MachineState object.
                 * The virt board reads these from the machine at init.
                 */
                Error *perr = NULL;
                if (object_property_find(OBJECT(current_machine),
                                          binding->property)) {
                    object_property_set_str(OBJECT(current_machine),
                                            binding->property,
                                            binding->value, &perr);
                    if (perr) {
                        error_free(perr);
                        perr = NULL;
                    }
                }
                /* Also register as a global for the CPU type. */
                GlobalProperty *prop = g_new0(GlobalProperty, 1);
                prop->driver = g_strdup(current_machine->cpu_type);
                prop->property = g_strdup(binding->property);
                prop->value = g_strdup(binding->value);
                qdev_prop_register_global(prop);
                g_ptr_array_remove_index(hedgehog_property_bindings, bi - 1);
            }
        }
    }

    machine_run_board_init(current_machine, NULL, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return false;
    }

    if (!hedgehog_backend_require_all_property_bindings_resolved(errp)) {
        return false;
    }

    cpu = hedgehog_backend_first_realized_cpu();
    if (!cpu) {
        error_setg(errp, "machine '%s' realized without a CPU",
                   object_get_typename(OBJECT(current_machine)));
        return false;
    }

    return true;
}

bool hedgehog_backend_initialize(Error **errp)
{
    return hedgehog_backend_initialize_for_machine(NULL, errp);
}

bool hedgehog_backend_initialize_for_machine(const char *machine_type,
                                             Error **errp)
{
    g_autofree char *requested = hedgehog_backend_canonicalize_machine_type(machine_type);

    if (!hedgehog_backend_ensure_runtime_initialized(errp)) {
        return false;
    }

    g_mutex_lock(&hedgehog_init_lock);

    if (hedgehog_init_state.initialized) {
        if (g_strcmp0(requested, hedgehog_init_state.machine_type) != 0) {
            error_setg(errp,
                       "hedgehog backend is already initialized for machine '%s' "
                       "and cannot switch to '%s'",
                       hedgehog_init_state.machine_type, requested);
            g_mutex_unlock(&hedgehog_init_lock);
            return false;
        }
        g_mutex_unlock(&hedgehog_init_lock);
        return true;
    }

    if (!hedgehog_backend_create_machine(requested, errp)) {
        g_mutex_unlock(&hedgehog_init_lock);
        return false;
    }

    if (!hedgehog_backend_apply_available_property_bindings(errp)) {
        g_mutex_unlock(&hedgehog_init_lock);
        return false;
    }

    machine_memory_init();
    hedgehog_backend_init_tcg_accel();
    hedgehog_backend_advance_machine_phases();

    if (!tcg_enabled()) {
        error_setg(errp, "the hedgehog backend requires initialized TCG support");
        g_mutex_unlock(&hedgehog_init_lock);
        return false;
    }

    hedgehog_init_state.initialized = true;
    hedgehog_init_state.machine_type = g_steal_pointer(&requested);
    g_mutex_unlock(&hedgehog_init_lock);

    return true;
}

bool hedgehog_backend_chardev_add(const char *id, const char *uri,
                                  Error **errp)
{
    HedgehogChardev *entry;
    Chardev *chr;

    if (!id || !id[0]) {
        error_setg(errp, "chardev id is required");
        return false;
    }

    if (!uri || !uri[0]) {
        error_setg(errp, "chardev uri is required");
        return false;
    }

    if (!hedgehog_backend_ensure_runtime_initialized(errp)) {
        return false;
    }

    BQL_LOCK_GUARD();

    if (hedgehog_backend_find_chardev(id)) {
        error_setg(errp, "hedgehog chardev '%s' already exists", id);
        return false;
    }

    if (!hedgehog_chardevs) {
        hedgehog_chardevs = g_ptr_array_new();
    }

    chr = qemu_chr_new_noreplay(id, uri, false, NULL);
    if (!chr) {
        error_setg(errp, "failed to create hedgehog chardev '%s' from '%s'",
                   id, uri);
        return false;
    }

    entry = g_new0(HedgehogChardev, 1);
    entry->id = g_strdup(id);
    entry->label = g_strdup(id);
    entry->chr = chr;
    g_ptr_array_add(hedgehog_chardevs, entry);
    return true;
}

bool hedgehog_backend_bind_property(const char *object_path,
                                    const char *property,
                                    const char *value,
                                    Error **errp)
{
    g_autofree char *normalized_path = NULL;
    HedgehogPropertyBinding *binding;
    bool ambiguous = false;

    if (!object_path || !object_path[0]) {
        error_setg(errp, "object_path is required");
        return false;
    }

    if (!property || !property[0]) {
        error_setg(errp, "property is required");
        return false;
    }

    if (!value || !value[0]) {
        error_setg(errp, "value is required");
        return false;
    }

    if (!hedgehog_backend_ensure_runtime_initialized(errp)) {
        return false;
    }

    normalized_path = hedgehog_backend_normalize_object_path(object_path);

    BQL_LOCK_GUARD();

    if (object_resolve_path(normalized_path, &ambiguous)) {
        return hedgehog_backend_set_property_string_locked(normalized_path,
                                                           property,
                                                           value,
                                                           errp);
    }
    if (ambiguous) {
        error_setg(errp, "object path '%s' is ambiguous", normalized_path);
        return false;
    }

    g_mutex_lock(&hedgehog_init_lock);
    if (!hedgehog_init_state.board_initialized) {
        if (!hedgehog_property_bindings) {
            hedgehog_property_bindings =
                g_ptr_array_new_with_free_func(hedgehog_backend_free_property_binding);
        }

        binding = hedgehog_backend_find_property_binding(normalized_path, property);
        if (binding) {
            g_mutex_unlock(&hedgehog_init_lock);
            error_setg(errp, "property binding '%s:%s' already exists",
                       normalized_path, property);
            return false;
        }

        binding = g_new0(HedgehogPropertyBinding, 1);
        binding->object_path = g_strdup(normalized_path);
        binding->property = g_strdup(property);
        binding->value = g_strdup(value);
        g_ptr_array_add(hedgehog_property_bindings, binding);
        g_mutex_unlock(&hedgehog_init_lock);
        return true;
    }
    g_mutex_unlock(&hedgehog_init_lock);

    return hedgehog_backend_set_property_string_locked(normalized_path, property,
                                                       value, errp);
}

bool hedgehog_backend_chardev_attach_serial(int index, const char *id,
                                            Error **errp)
{
    HedgehogChardev *entry;

    if (index < 0) {
        error_setg(errp, "serial index must be >= 0");
        return false;
    }

    if (!id || !id[0]) {
        error_setg(errp, "chardev id is required");
        return false;
    }

    if (!hedgehog_backend_ensure_runtime_initialized(errp)) {
        return false;
    }

    BQL_LOCK_GUARD();

    entry = hedgehog_backend_find_chardev(id);
    if (!entry) {
        error_setg(errp, "unknown hedgehog chardev '%s'", id);
        return false;
    }

    g_mutex_lock(&hedgehog_init_lock);
    if (hedgehog_init_state.board_initialized) {
        g_mutex_unlock(&hedgehog_init_lock);
        error_setg(errp,
                   "serial backends must be attached before the board machine is realized");
        return false;
    }
    g_mutex_unlock(&hedgehog_init_lock);

    serial_hd_set(index, entry->chr);
    return true;
}

int hedgehog_backend_chardev_get_endpoint(const char *id, char *buf,
                                          size_t buf_size, Error **errp)
{
    g_autofree char *endpoint = NULL;
    HedgehogChardev *entry;
    size_t required;

    if (!id || !id[0]) {
        error_setg(errp, "chardev id is required");
        return -1;
    }

    if (!hedgehog_backend_ensure_runtime_initialized(errp)) {
        return -1;
    }

    BQL_LOCK_GUARD();

    entry = hedgehog_backend_find_chardev(id);
    if (!entry) {
        error_setg(errp, "unknown hedgehog chardev '%s'", id);
        return -1;
    }

    endpoint = qemu_chr_get_pty_name(entry->chr);
    if (!endpoint) {
        endpoint = qemu_chr_get_filename(entry->chr);
    }
    if (!endpoint) {
        error_setg(errp, "hedgehog chardev '%s' has no endpoint information", id);
        return -1;
    }

    required = strlen(endpoint) + 1;
    if (buf && buf_size >= required) {
        g_strlcpy(buf, endpoint, buf_size);
    }
    return required;
}

int hedgehog_backend_poll_events(bool blocking, Error **errp)
{
    int count = 0;

    if (!hedgehog_backend_ensure_runtime_initialized(errp)) {
        return -1;
    }

    g_mutex_lock(&hedgehog_direct_run_lock);
    if (hedgehog_init_state.standalone_active_runs) {
        g_mutex_unlock(&hedgehog_direct_run_lock);
        return 0; /* The standalone dispatcher owns this event loop. */
    }
    if (blocking && g_main_context_iteration(NULL, true)) {
        count++;
    }
    while (g_main_context_iteration(NULL, false)) {
        count++;
    }

    {
        BQL_LOCK_GUARD();
        qemu_clock_run_all_timers();
    }
    g_mutex_unlock(&hedgehog_direct_run_lock);
    return count;
}

HedgehogBackend *hedgehog_backend_new(const char *cpu_type, Error **errp)
{
    return hedgehog_backend_new_with_machine(cpu_type, NULL, errp);
}

HedgehogBackend *hedgehog_backend_new_with_machine(const char *cpu_type,
                                                   const char *machine_type,
                                                   Error **errp)
{
    Error *local_err = NULL;
    g_autofree char *requested_machine =
        hedgehog_backend_canonicalize_machine_type(machine_type);
    bool board_backed = hedgehog_backend_is_board_machine(requested_machine);
    bool board_initialized = false;
    CPUState *cpu = NULL;
    HedgehogBackend *uc;
    Object *cpuobj;
    ObjectClass *cpu_class;

    BQL_LOCK_GUARD();

    if (!cpu_type && !board_backed) {
        error_setg(errp, "a concrete CPU type is required");
        return NULL;
    }

    if (!hedgehog_backend_initialize_for_machine(machine_type, &local_err)) {
        error_propagate(errp, local_err);
        return NULL;
    }

    if (board_backed) {
        g_mutex_lock(&hedgehog_init_lock);
        if (hedgehog_init_state.board_backend_active) {
            error_setg(errp,
                       "only one board-backed hedgehog backend can be active at a time");
            g_mutex_unlock(&hedgehog_init_lock);
            return NULL;
        }
        board_initialized = hedgehog_init_state.board_initialized;
        g_mutex_unlock(&hedgehog_init_lock);

        if (!board_initialized) {
            if (!hedgehog_backend_realize_board_machine(cpu_type, &local_err)) {
                error_propagate(errp, local_err);
                return NULL;
            }

            g_mutex_lock(&hedgehog_init_lock);
            hedgehog_init_state.board_initialized = true;
            g_mutex_unlock(&hedgehog_init_lock);
        }

        cpu = hedgehog_backend_first_realized_cpu();
        if (!cpu) {
            error_setg(errp, "failed to find a realized board CPU");
            return NULL;
        }
    }

    uc = g_new0(HedgehogBackend, 1);
    uc->board_backed = board_backed;
    uc->ram_regions = g_ptr_array_new();
    uc->mmio_mappings = g_ptr_array_new();
    uc->cpu_output_links = g_ptr_array_new_with_free_func(g_free);
    g_queue_init(&uc->cpu_output_events);
    g_queue_init(&uc->system_call_events);
    g_queue_init(&uc->system_call_pending);
    g_queue_init(&uc->cpu_wait_events);

    if (board_backed) {
        uc->cpu = cpu;
        uc->active_as = &address_space_memory;
    } else {
        memory_region_init(&uc->root, NULL, "hedgehog-memory", UINT64_MAX);
        uc->owns_memory_root = true;

        address_space_init(&uc->as, &uc->root, "hedgehog-memory");
        uc->owns_address_space = true;
        uc->active_as = &uc->as;

        cpu_class = hedgehog_backend_lookup_cpu_class(cpu_type, errp);
        if (!cpu_class) {
            hedgehog_backend_free(uc);
            return NULL;
        }

        cpuobj = object_new_with_class(cpu_class);
        if (object_property_find(cpuobj, "apic-id")) {
            object_property_set_int(cpuobj, "apic-id", 0, &local_err);
            if (local_err) {
                error_propagate(errp, local_err);
                object_unref(cpuobj);
                hedgehog_backend_free(uc);
                return NULL;
            }
        }

        object_property_set_link(cpuobj, "memory", OBJECT(&uc->root), &local_err);
        if (local_err) {
            error_propagate(errp, local_err);
            object_unref(cpuobj);
            hedgehog_backend_free(uc);
            return NULL;
        }

        if (!qdev_realize(DEVICE(cpuobj), NULL, &local_err)) {
            error_propagate(errp, local_err);
            object_unref(cpuobj);
            hedgehog_backend_free(uc);
            return NULL;
        }

        uc->cpu = CPU(cpuobj);
        uc->owns_cpu = true;
    }

    {
        const HedgehogArchOps *ops = hedgehog_arch_ops();

        hedgehog_exec_hook_register_backend(
            uc->cpu, uc, hedgehog_backend_request_stop_from_hook, uc,
            ops ? ops->get_invalid_insn : NULL);
    }

    if (board_backed) {
        g_mutex_lock(&hedgehog_init_lock);
        hedgehog_init_state.board_backend_active = true;
        g_mutex_unlock(&hedgehog_init_lock);
    }

    hedgehog_backend_reset(uc);
    hedgehog_backend_keep_standalone_cpu_stopped(uc);
    /* No asynchronous caller can exist before the handle is returned. */
    hedgehog_backend_reset_stop(uc);
    /* Standalone clocks run only during direct execution. Board-backed
     * sessions retain their existing VM-clock lifecycle. */
    if (board_backed) {
        cpu_enable_ticks();
        qemu_clock_enable(QEMU_CLOCK_VIRTUAL, true);
    }
    return uc;
}

void hedgehog_backend_free(HedgehogBackend *uc)
{
    guint i;
    bool wait_for_rcu = false;

    BQL_LOCK_GUARD();

    if (!uc) {
        return;
    }

    if (uc->host_timer) {
        timer_free(uc->host_timer);
    }
    if (uc->cpu) {
        for (i = 0; i < uc->cpu_output_links->len; i++) {
            HedgehogCPUOutputLink *link = g_ptr_array_index(uc->cpu_output_links, i);
            qdev_connect_gpio_out(DEVICE(uc->cpu), link->index, NULL);
            qemu_free_irq(link->irq);
        }
        g_queue_clear_full(&uc->cpu_output_events, g_free);
        hedgehog_backend_clear_cpu_wait_events_locked(uc);
        hedgehog_exec_hook_unregister_backend(uc->cpu);
        if (uc->owns_cpu && qemu_tcg_mttcg_enabled()) {
            cpu_remove_sync(uc->cpu);
        } else {
            cpu_exit(uc->cpu);
        }
        if (uc->owns_cpu && DEVICE(uc->cpu)->realized) {
            qdev_unrealize(DEVICE(uc->cpu));
        }
        {
            const HedgehogArchOps *ops = hedgehog_arch_ops();

            if (ops && ops->release_cpu) {
                ops->release_cpu(uc->cpu);
            }
        }
        if (uc->owns_cpu) {
            object_unref(OBJECT(uc->cpu));
        }
        uc->cpu = NULL;
    }

    for (i = 0; i < uc->mmio_mappings->len; i++) {
        HedgehogMMIOMapping *mapping = g_ptr_array_index(uc->mmio_mappings, i);

        if (uc->owns_memory_root) {
            memory_region_del_subregion(&uc->root,
                                        hedgehog_mmio_device_region(mapping->dev));
            wait_for_rcu = true;
        } else if (uc->board_backed) {
            MemoryRegion *sysmem = get_system_memory();
            memory_region_del_subregion(
                sysmem, hedgehog_mmio_device_region(mapping->dev));
            wait_for_rcu = true;
        }
    }

    for (i = 0; i < uc->ram_regions->len; i++) {
        HedgehogRAMRegion *region = g_ptr_array_index(uc->ram_regions, i);

        if (uc->owns_memory_root) {
            memory_region_del_subregion(&uc->root, &region->mr);
        }
    }

    if (uc->owns_address_space) {
        address_space_destroy(&uc->as);
        wait_for_rcu = true;
    }

    if (wait_for_rcu) {
        /*
         * address_space_destroy() and subregion removals defer FlatView cleanup
         * to RCU callbacks. Keep all MemoryRegion-backed QOM objects alive
         * until those callbacks complete.
         */
        drain_call_rcu();
    }

    for (i = 0; i < uc->mmio_mappings->len; i++) {
        HedgehogMMIOMapping *mapping = g_ptr_array_index(uc->mmio_mappings, i);

        if (DEVICE(mapping->dev)->realized) {
            qdev_unrealize(DEVICE(mapping->dev));
        }
        object_unref(OBJECT(mapping->dev));
        g_free(mapping);
    }

    for (i = 0; i < uc->ram_regions->len; i++) {
        HedgehogRAMRegion *region = g_ptr_array_index(uc->ram_regions, i);

        object_unparent(OBJECT(&region->mr));
        g_free(region);
    }

    if (uc->owns_memory_root) {
        object_unparent(OBJECT(&uc->root));
    }

    g_ptr_array_free(uc->cpu_output_links, true);
    g_ptr_array_free(uc->mmio_mappings, true);
    hedgehog_backend_clear_system_call_events_locked(uc);
    g_ptr_array_free(uc->ram_regions, true);

    if (uc->board_backed) {
        g_mutex_lock(&hedgehog_init_lock);
        hedgehog_init_state.board_backend_active = false;
        g_mutex_unlock(&hedgehog_init_lock);
    }

    g_free(uc);
}

static bool hedgehog_backend_map_ram_common(HedgehogBackend *uc,
                                            const char *name,
                                            hwaddr addr, uint64_t size,
                                            void *ptr, Error **errp)
{
    HedgehogRAMRegion *region;
    guint i;

    BQL_LOCK_GUARD();

    if (!uc || !size) {
        error_setg(errp, "RAM mapping requires a backend and non-zero size");
        return false;
    }

    if (uc->board_backed) {
        error_setg(errp,
                   "RAM mapping is not supported for board-backed hedgehog backends");
        return false;
    }

    for (i = 0; i < uc->ram_regions->len; i++) {
        HedgehogRAMRegion *existing = g_ptr_array_index(uc->ram_regions, i);

        if (hedgehog_ranges_overlap(existing->base, existing->size, addr, size)) {
            error_setg(errp, "RAM mapping overlaps an existing region");
            return false;
        }
    }

    for (i = 0; i < uc->mmio_mappings->len; i++) {
        HedgehogMMIOMapping *existing = g_ptr_array_index(uc->mmio_mappings, i);

        if (hedgehog_ranges_overlap(existing->base, existing->size, addr, size)) {
            error_setg(errp, "RAM mapping overlaps an existing MMIO mapping");
            return false;
        }
    }

    region = g_new0(HedgehogRAMRegion, 1);
    if (ptr) {
        memory_region_init_ram_ptr(&region->mr, NULL,
                                   name ?: "hedgehog-ram-ptr", size, ptr);
    } else {
        if (!memory_region_init_ram_flags_nomigrate(&region->mr, NULL,
                                                    name ?: "hedgehog-ram",
                                                    size, 0, errp)) {
            g_free(region);
            return false;
        }
    }

    region->base = addr;
    region->size = size;
    memory_region_add_subregion(&uc->root, addr, &region->mr);
    g_ptr_array_add(uc->ram_regions, region);
    return true;
}

bool hedgehog_backend_map_ram(HedgehogBackend *uc, const char *name,
                             hwaddr addr, uint64_t size, Error **errp)
{
    return hedgehog_backend_map_ram_common(uc, name, addr, size, NULL, errp);
}

bool hedgehog_backend_map_ram_ptr(HedgehogBackend *uc, const char *name,
                                 hwaddr addr, uint64_t size, void *ptr,
                                 Error **errp)
{
    if (!ptr) {
        error_setg(errp, "host-backed RAM mapping requires a non-NULL pointer");
        return false;
    }
    return hedgehog_backend_map_ram_common(uc, name, addr, size, ptr, errp);
}

bool hedgehog_backend_map_mmio(HedgehogBackend *uc, const char *name,
                              hwaddr addr, uint64_t size,
                              HedgehogMMIOReadFunc read_fn,
                              HedgehogMMIOWriteFunc write_fn,
                              void *opaque, Error **errp)
{
    HedgehogMMIOMapping *mapping;
    Object *obj;
    guint i;

    BQL_LOCK_GUARD();

    if (!uc || !size) {
        error_setg(errp, "MMIO mapping requires a backend and non-zero size");
        return false;
    }

    if (!uc->board_backed) {
        for (i = 0; i < uc->ram_regions->len; i++) {
            HedgehogRAMRegion *existing = g_ptr_array_index(uc->ram_regions, i);

            if (hedgehog_ranges_overlap(existing->base, existing->size,
                                         addr, size)) {
                error_setg(errp,
                           "MMIO mapping overlaps an existing RAM region");
                return false;
            }
        }
    }

    for (i = 0; i < uc->mmio_mappings->len; i++) {
        HedgehogMMIOMapping *existing = g_ptr_array_index(uc->mmio_mappings, i);

        if (hedgehog_ranges_overlap(existing->base, existing->size, addr, size)) {
            error_setg(errp, "MMIO mapping overlaps an existing MMIO mapping");
            return false;
        }
    }

    obj = object_new(TYPE_HEDGEHOG_MMIO_DEVICE);
    hedgehog_mmio_device_configure(HEDGEHOG_MMIO_DEVICE(obj),
                                  name ?: "hedgehog-mmio",
                                  size, read_fn, write_fn, opaque);

    if (!sysbus_realize(SYS_BUS_DEVICE(obj), errp)) {
        object_unref(obj);
        return false;
    }

    mapping = g_new0(HedgehogMMIOMapping, 1);
    mapping->dev = HEDGEHOG_MMIO_DEVICE(obj);
    mapping->base = addr;
    mapping->size = size;

    /*
     * For board-backed machines, add MMIO sub-regions to the system memory
     * root with priority 1 so they override the board's default flat view.
     * For standalone backends, use the hedgehog-owned root at priority 0.
     */
    if (uc->board_backed) {
        MemoryRegion *sysmem = get_system_memory();
        memory_region_add_subregion_overlap(
            sysmem, addr, hedgehog_mmio_device_region(mapping->dev), 1);
    } else {
        memory_region_add_subregion(&uc->root, addr,
                                    hedgehog_mmio_device_region(mapping->dev));
    }
    g_ptr_array_add(uc->mmio_mappings, mapping);
    return true;
}

bool hedgehog_backend_mem_unmap(HedgehogBackend *uc, hwaddr addr,
                               uint64_t size, Error **errp)
{
    g_autoptr(GPtrArray) removed_ram = g_ptr_array_new();
    g_autoptr(GPtrArray) removed_mmio = g_ptr_array_new();
    guint i;
    uint64_t end;

    BQL_LOCK_GUARD();

    if (!uc || !size) {
        error_setg(errp, "memory unmap requires a backend and non-zero size");
        return false;
    }

    end = addr + size;
    if (end < addr) {
        error_setg(errp, "memory unmap range overflow");
        return false;
    }

    /* Validate the complete operation before mutating the address space. */
    for (i = 0; i < uc->ram_regions->len; i++) {
        HedgehogRAMRegion *region = g_ptr_array_index(uc->ram_regions, i);
        uint64_t region_start = region->base;
        uint64_t region_end = region_start + region->size;

        if (region_end < region_start) {
            error_setg(errp, "invalid RAM mapping range bookkeeping");
            return false;
        }

        if (!(region_start >= addr && region_end <= end) &&
            hedgehog_ranges_overlap(region_start, region->size, addr, size)) {
            error_setg(errp, "partial RAM unmap is unsupported");
            return false;
        }
    }

    for (i = 0; i < uc->mmio_mappings->len; i++) {
        HedgehogMMIOMapping *mapping = g_ptr_array_index(uc->mmio_mappings, i);
        uint64_t mapping_start = mapping->base;
        uint64_t mapping_end = mapping_start + mapping->size;

        if (mapping_end < mapping_start) {
            error_setg(errp, "invalid MMIO mapping range bookkeeping");
            return false;
        }

        if (!(mapping_start >= addr && mapping_end <= end) &&
            hedgehog_ranges_overlap(mapping_start, mapping->size, addr, size)) {
            error_setg(errp, "partial MMIO unmap is unsupported");
            return false;
        }
    }

    for (i = uc->ram_regions->len; i > 0; i--) {
        HedgehogRAMRegion *region = g_ptr_array_index(uc->ram_regions, i - 1);

        if (region->base >= addr && region->base + region->size <= end) {
            memory_region_del_subregion(&uc->root, &region->mr);
            g_ptr_array_add(removed_ram, region);
            g_ptr_array_remove_index(uc->ram_regions, i - 1);
        }
    }

    for (i = uc->mmio_mappings->len; i > 0; i--) {
        HedgehogMMIOMapping *mapping = g_ptr_array_index(uc->mmio_mappings, i - 1);

        if (mapping->base >= addr && mapping->base + mapping->size <= end) {
            MemoryRegion *parent = uc->board_backed ? get_system_memory() : &uc->root;

            memory_region_del_subregion(
                parent, hedgehog_mmio_device_region(mapping->dev));
            g_ptr_array_add(removed_mmio, mapping);
            g_ptr_array_remove_index(uc->mmio_mappings, i - 1);
        }
    }

    if (removed_ram->len == 0 && removed_mmio->len == 0) {
        error_setg(errp, "unmap range does not match any mapping");
        return false;
    }

    /*
     * FlatViews are reclaimed by deferred RCU callbacks.  A grace period alone
     * does not guarantee that those callbacks have run, so drain them before
     * releasing MemoryRegions, MMIO callbacks, or caller-owned map_ram_ptr()
     * backing.
     */
    drain_call_rcu();

    for (i = 0; i < removed_ram->len; i++) {
        HedgehogRAMRegion *region = g_ptr_array_index(removed_ram, i);

        object_unparent(OBJECT(&region->mr));
        g_free(region);
    }
    for (i = 0; i < removed_mmio->len; i++) {
        HedgehogMMIOMapping *mapping = g_ptr_array_index(removed_mmio, i);

        if (DEVICE(mapping->dev)->realized) {
            qdev_unrealize(DEVICE(mapping->dev));
        }
        object_unref(OBJECT(mapping->dev));
        g_free(mapping);
    }

    return true;
}

MemTxResult hedgehog_backend_mem_read(HedgehogBackend *uc, hwaddr addr,
                                     void *buf, hwaddr len)
{
    g_assert(uc);
    g_assert(uc->active_as);
    return address_space_read(uc->active_as, addr, MEMTXATTRS_UNSPECIFIED,
                              buf, len);
}

MemTxResult hedgehog_backend_mem_write(HedgehogBackend *uc, hwaddr addr,
                                      const void *buf, hwaddr len)
{
    g_assert(uc);
    g_assert(uc->active_as);
    return address_space_write(uc->active_as, addr, MEMTXATTRS_UNSPECIFIED,
                               buf, len);
}

int hedgehog_backend_reg_read(HedgehogBackend *uc, int regno,
                             uint8_t *buf, size_t buf_size,
                             Error **errp)
{
    const CPUClass *cc;
    g_autoptr(GByteArray) reg = NULL;
    int reg_len;

    if (!uc || !buf) {
        error_setg(errp, "register read requires backend and output buffer");
        return -1;
    }
    if (regno < 0) {
        error_setg(errp, "register read requires non-negative register id");
        return -1;
    }

    cc = uc->cpu->cc;
    if (!cc->gdb_read_register) {
        error_setg(errp, "target does not expose gdb register read callback");
        return -1;
    }

    reg = g_byte_array_new();
    reg_len = cc->gdb_read_register(uc->cpu, reg, regno);
    if (reg_len <= 0 || reg->len < reg_len) {
        error_setg(errp, "failed to read register %d", regno);
        return -1;
    }
    if ((size_t)reg_len > buf_size) {
        error_setg(errp, "buffer too small for register %d (%d bytes needed)",
                   regno, reg_len);
        return -1;
    }

    memcpy(buf, reg->data, reg_len);
    return reg_len;
}

int hedgehog_backend_reg_write(HedgehogBackend *uc, int regno,
                              const uint8_t *buf, size_t buf_size,
                              Error **errp)
{
    const CPUClass *cc;
    g_autofree uint8_t *tmp = NULL;
    size_t tmp_size;
    int reg_len;

    if (!uc || !buf) {
        error_setg(errp, "register write requires backend and input buffer");
        return -1;
    }
    if (regno < 0) {
        error_setg(errp, "register write requires non-negative register id");
        return -1;
    }

    cc = uc->cpu->cc;
    if (!cc->gdb_write_register) {
        error_setg(errp, "target does not expose gdb register write callback");
        return -1;
    }

    tmp_size = MAX(buf_size, sizeof(uint64_t));
    tmp = g_malloc0(tmp_size);
    memcpy(tmp, buf, buf_size);
    reg_len = cc->gdb_write_register(uc->cpu, tmp, regno);
    if (reg_len <= 0 || (size_t)reg_len > buf_size) {
        error_setg(errp, "failed to write register %d", regno);
        return -1;
    }

    return reg_len;
}

void hedgehog_backend_set_tb_hook(HedgehogBackend *uc,
                                 HedgehogExecHookFunc hook_fn,
                                 void *opaque)
{
    if (!uc) {
        return;
    }

    hedgehog_exec_hook_set_tb(uc, hook_fn, opaque);
}

void hedgehog_backend_set_insn_hook(HedgehogBackend *uc,
                                   HedgehogExecHookFunc hook_fn,
                                   void *opaque)
{
    if (!uc) {
        return;
    }

    hedgehog_exec_hook_set_insn(uc, hook_fn, opaque);
}

void hedgehog_backend_set_invalid_insn_hook(
    HedgehogBackend *uc,
    HedgehogInvalidInsnHookFunc hook_fn,
    void *opaque)
{
    if (!uc) {
        return;
    }

    hedgehog_exec_hook_set_invalid_insn(uc, hook_fn, opaque);
}

void hedgehog_backend_set_invalid_mem_hook(HedgehogBackend *uc,
                                          HedgehogInvalidMemHookFunc hook_fn,
                                          void *opaque)
{
    if (!uc) {
        return;
    }

    hedgehog_exec_hook_set_invalid(uc, hook_fn, opaque);
}

bool hedgehog_backend_set_invalid_mem_diagnostic_capture(
    HedgehogBackend *uc, bool enabled, Error **errp)
{
    BQL_LOCK_GUARD();

    if (!uc) {
        error_setg(errp, "invalid-memory diagnostic capture requires backend");
        return false;
    }
    if (uc->execution_started) {
        error_setg(errp,
                   "invalid-memory diagnostic capture must be configured before execution");
        return false;
    }

    hedgehog_exec_hook_set_invalid_mem_diagnostic_capture(uc, enabled);
    return true;
}

bool hedgehog_backend_get_last_invalid_mem_diagnostic(
    HedgehogBackend *uc, HedgehogInvalidMemInfo *info)
{
    return hedgehog_exec_hook_get_last_invalid_mem_diagnostic(uc, info);
}

void hedgehog_backend_reset(HedgehogBackend *uc)
{
    BQL_LOCK_GUARD();

    g_assert(uc);
    hedgehog_backend_keep_standalone_cpu_stopped(uc);
    cpu_reset(uc->cpu);
    hedgehog_backend_apply_arm64_reset_state(uc, &error_abort);
    /* A reset cannot complete a call issued before it. Keep any delivery queue
     * intact for the direct runner, but discard stale correlation candidates. */
    g_queue_clear_full(&uc->system_call_pending,
                       hedgehog_backend_free_system_call_pending);
    hedgehog_backend_keep_standalone_cpu_stopped(uc);
    hedgehog_backend_reset_stop(uc);
    uc->invalid_mem_seen = false;
    hedgehog_exec_hook_clear_invalid_mem_diagnostic(uc);
}

bool hedgehog_backend_request_cpu_reset(HedgehogBackend *uc, Error **errp)
{
    BQL_LOCK_GUARD();

    if (!uc || !uc->cpu || uc->board_backed) {
        error_setg(errp,
                   "architectural CPU reset requires a standalone backend");
        return false;
    }
    if (!uc->execution_started) {
        error_setg(errp,
                   "architectural CPU reset requires an executing backend");
        return false;
    }

    /* Apply the reset only at the direct-run event boundary.  Callers may use
     * this from an observer, but cannot mutate guest architectural state. */
    uc->cpu_reset_requested = true;
    cpu_exit(uc->cpu);
    qemu_cond_broadcast(uc->cpu->halt_cond);
    return true;
}

static bool hedgehog_backend_apply_pending_cpu_reset(HedgehogBackend *uc)
{
    bool requested;

    {
        BQL_LOCK_GUARD();
        requested = uc->cpu_reset_requested;
        uc->cpu_reset_requested = false;
    }
    if (!requested) {
        return false;
    }
    hedgehog_backend_reset(uc);
    hedgehog_backend_dispatch_cpu_reset_observer(uc);
    return true;
}

void hedgehog_backend_reset_stop(HedgehogBackend *uc)
{
    g_assert(uc);
    qatomic_set(&uc->stop_requested, false);
    qatomic_set(&uc->cpu->exit_request, false);
}

void hedgehog_backend_set_pc(HedgehogBackend *uc, vaddr addr)
{
    g_assert(uc);
    cpu_set_pc(uc->cpu, addr);
}

vaddr hedgehog_backend_get_pc(HedgehogBackend *uc)
{
    g_assert(uc);
    return uc->cpu->cc->get_pc(uc->cpu);
}

static HedgehogRunResult hedgehog_backend_translate_run_result(int cpu_exit)
{
    switch (cpu_exit) {
    case EXCP_HLT:
    case EXCP_HALTED:
        return HEDGEHOG_RUN_HALTED;
    case EXCP_DEBUG:
        return HEDGEHOG_RUN_BUDGET_EXHAUSTED;
    default:
        return HEDGEHOG_RUN_EXCEPTION;
    }
}

HedgehogRunResult hedgehog_backend_run(HedgehogBackend *uc,
                                     uint64_t max_instructions,
                                     int *cpu_exit)
{
    int ret = EXCP_DEBUG;
    uint64_t i;

    g_assert(uc);
    hedgehog_backend_keep_standalone_cpu_stopped(uc);
    uc->execution_started = true;
    uc->invalid_mem_seen = false;
    hedgehog_exec_hook_clear_invalid_mem_diagnostic(uc);

    /*
     * Stop state is reset explicitly by the caller before it arms any
     * asynchronous timeout.  Never clear it here: a stop may arrive after
     * the caller's loop check but before this function begins executing.
     */
    if (qatomic_read(&uc->stop_requested)) {
        if (cpu_exit) {
            *cpu_exit = EXCP_INTERRUPT;
        }
        return HEDGEHOG_RUN_STOP_REQUESTED;
    }

    if (max_instructions == 0) {
        hedgehog_backend_begin_direct_run(uc);
        do {
            hedgehog_backend_dispatch_cpu_outputs(uc);
            hedgehog_backend_dispatch_system_call_events(uc);
            hedgehog_backend_dispatch_cpu_wait_events(uc);
            if (hedgehog_backend_apply_pending_cpu_reset(uc)) {
                ret = EXCP_INTERRUPT;
                continue;
            }
            cpu_exec_start(uc->cpu);
            ret = cpu_exec(uc->cpu);
            cpu_exec_end(uc->cpu);
            hedgehog_backend_process_queued_cpu_work(uc->cpu);
            hedgehog_backend_dispatch_cpu_outputs(uc);
            hedgehog_backend_dispatch_system_call_events(uc);
            hedgehog_backend_dispatch_cpu_wait_events(uc);
            if (hedgehog_backend_apply_pending_cpu_reset(uc)) {
                ret = EXCP_INTERRUPT;
                continue;
            }
            if (!uc->board_backed && (ret == EXCP_HLT || ret == EXCP_HALTED) &&
                !qatomic_read(&uc->stop_requested)) {
                hedgehog_backend_wait_for_interrupt(uc);
                ret = EXCP_INTERRUPT;
            }
            if (ret == EXCP_ATOMIC && !qatomic_read(&uc->stop_requested) &&
                !uc->invalid_mem_seen) {
                cpu_exec_step_atomic(uc->cpu);
            }
        } while (!qatomic_read(&uc->stop_requested) && !uc->invalid_mem_seen &&
                 (ret == EXCP_INTERRUPT || ret == EXCP_ATOMIC ||
                  ret == EXCP_YIELD));
        hedgehog_backend_end_direct_run(uc);
        hedgehog_backend_dispatch_cpu_outputs(uc);
        hedgehog_backend_dispatch_system_call_events(uc);
        hedgehog_backend_dispatch_cpu_wait_events(uc);
        if (hedgehog_backend_apply_pending_cpu_reset(uc)) {
            ret = EXCP_INTERRUPT;
        }
        if (cpu_exit) {
            *cpu_exit = ret;
        }
        if (uc->invalid_mem_seen) {
            return HEDGEHOG_RUN_INVALID_MEMORY;
        }
        if (qatomic_read(&uc->stop_requested)) {
            return HEDGEHOG_RUN_STOP_REQUESTED;
        }
        return hedgehog_backend_translate_run_result(ret);
    }

    /*
     * Finite Hedgehog runs use single-step mode to make the instruction budget
     * exact. Keep QEMU's virtual clock enabled while stepping so architectural
     * timer reads such as CNT{P,V}CT_EL0 progress during firmware delay loops.
     */
    hedgehog_backend_begin_direct_run(uc);
    cpu_single_step(uc->cpu, SSTEP_ENABLE | SSTEP_NOIRQ);
    for (i = 0; i < max_instructions; i++) {
        cpu_exec_start(uc->cpu);
        ret = cpu_exec(uc->cpu);
        cpu_exec_end(uc->cpu);
        hedgehog_backend_process_queued_cpu_work(uc->cpu);
        hedgehog_backend_dispatch_cpu_outputs(uc);
        hedgehog_backend_dispatch_system_call_events(uc);
        hedgehog_backend_dispatch_cpu_wait_events(uc);
        if (hedgehog_backend_apply_pending_cpu_reset(uc)) {
            ret = EXCP_INTERRUPT;
            continue;
        }

        if (ret == EXCP_ATOMIC && !qatomic_read(&uc->stop_requested) &&
            !uc->invalid_mem_seen) {
            cpu_exec_step_atomic(uc->cpu);
            continue;
        }

        if (ret == EXCP_INTERRUPT && !qatomic_read(&uc->stop_requested) &&
            !uc->invalid_mem_seen) {
            continue;
        }

        if (ret == EXCP_YIELD && !qatomic_read(&uc->stop_requested) &&
            !uc->invalid_mem_seen) {
            continue;
        }

        if (qatomic_read(&uc->stop_requested) || ret != EXCP_DEBUG) {
            break;
        }
    }
    cpu_single_step(uc->cpu, 0);
    hedgehog_backend_end_direct_run(uc);
    hedgehog_backend_dispatch_cpu_outputs(uc);
    hedgehog_backend_dispatch_system_call_events(uc);
    hedgehog_backend_dispatch_cpu_wait_events(uc);
    if (hedgehog_backend_apply_pending_cpu_reset(uc)) {
        ret = EXCP_INTERRUPT;
    }

    if (cpu_exit) {
        *cpu_exit = ret;
    }

    if (uc->invalid_mem_seen) {
        return HEDGEHOG_RUN_INVALID_MEMORY;
    }
    if (qatomic_read(&uc->stop_requested)) {
        return HEDGEHOG_RUN_STOP_REQUESTED;
    }
    if ((ret == EXCP_DEBUG || ret == EXCP_INTERRUPT || ret == EXCP_ATOMIC ||
         ret == EXCP_YIELD) &&
        i == max_instructions) {
        return HEDGEHOG_RUN_BUDGET_EXHAUSTED;
    }
    return hedgehog_backend_translate_run_result(ret);
}

void hedgehog_backend_stop(HedgehogBackend *uc)
{
    g_assert(uc);
    qatomic_set(&uc->stop_requested, true);
    cpu_exit(uc->cpu);
    {
        /* Match the idle predicate lock: a stop cannot be lost between the
         * predicate check and the condition wait releasing BQL. */
        BQL_LOCK_GUARD();
        qemu_cond_broadcast(uc->cpu->halt_cond);
    }
    qemu_notify_event();
}

bool hedgehog_backend_set_hard_interrupt(HedgehogBackend *uc,
                                         bool asserted,
                                         Error **errp)
{
    BQL_LOCK_GUARD();

    if (!uc || !uc->cpu) {
        error_setg(errp, "backend or CPU is not initialized");
        return false;
    }
    if (uc->board_backed) {
        error_setg(errp,
                   "hard interrupt input is only supported for standalone backends");
        return false;
    }

    if (asserted) {
        /*
         * Let the accelerator perform its normal wakeup/exit handling.  The
         * target CPU remains responsible for interrupt masks, routing, and
         * architectural exception entry.
         */
        cpu_interrupt(uc->cpu, CPU_INTERRUPT_HARD);
    } else {
        cpu_reset_interrupt(uc->cpu, CPU_INTERRUPT_HARD);
    }
    return true;
}

bool hedgehog_backend_set_aarch64_reset_state(HedgehogBackend *uc,
                                              unsigned current_el,
                                              bool secure,
                                              Error **errp)
{
    bool previous_configured;
    unsigned previous_el;
    bool previous_secure;

    if (!uc || !uc->cpu) {
        error_setg(errp, "backend or CPU is not initialized");
        return false;
    }
    if (current_el < 1 || current_el > 3) {
        error_setg(errp, "AArch64 reset current_el must be 1, 2, or 3");
        return false;
    }
    if (uc->board_backed) {
        error_setg(errp,
                   "AArch64 reset state is only supported for standalone backends");
        return false;
    }

    BQL_LOCK_GUARD();

    previous_configured = uc->arm64_reset_state_configured;
    previous_el = uc->arm64_reset_el;
    previous_secure = uc->arm64_reset_secure;

    uc->arm64_reset_state_configured = true;
    uc->arm64_reset_el = current_el;
    uc->arm64_reset_secure = secure;

    if (!hedgehog_backend_apply_arm64_reset_state(uc, errp)) {
        uc->arm64_reset_state_configured = previous_configured;
        uc->arm64_reset_el = previous_el;
        uc->arm64_reset_secure = previous_secure;
        return false;
    }

    return true;
}

static bool hedgehog_backend_aarch64_cpreg_configurable(
    HedgehogBackend *uc, Error **errp)
{
    if (!uc || !uc->cpu) {
        error_setg(errp, "backend or CPU is not initialized");
        return false;
    }
    if (uc->board_backed) {
        error_setg(errp,
                   "AArch64 CPREG configuration is only supported for standalone backends");
        return false;
    }
    if (uc->execution_started) {
        error_setg(errp,
                   "AArch64 CPREG configuration must happen before guest execution");
        return false;
    }
    return true;
}

bool hedgehog_backend_add_aarch64_cp_reg(HedgehogBackend *uc,
                                         unsigned opc0,
                                         unsigned opc1,
                                         unsigned crn,
                                         unsigned crm,
                                         unsigned opc2,
                                         unsigned min_el,
                                         bool readable,
                                         bool writable,
                                         uint64_t reset_value,
                                         Error **errp)
{
    const HedgehogArchOps *ops = hedgehog_arch_ops();

    if (!hedgehog_backend_aarch64_cpreg_configurable(uc, errp)) {
        return false;
    }
    if (!ops || !ops->add_aarch64_cp_reg) {
        error_setg(errp, "AArch64 CPREG overlays are unavailable in this backend");
        return false;
    }

    BQL_LOCK_GUARD();
    return ops->add_aarch64_cp_reg(uc->cpu, opc0, opc1, crn, crm, opc2,
                                   min_el, readable, writable, reset_value,
                                   errp);
}

bool hedgehog_backend_set_aarch64_cp_reg_value(HedgehogBackend *uc,
                                               unsigned opc0,
                                               unsigned opc1,
                                               unsigned crn,
                                               unsigned crm,
                                               unsigned opc2,
                                               uint64_t reset_value,
                                               uint64_t value,
                                               Error **errp)
{
    const HedgehogArchOps *ops = hedgehog_arch_ops();

    if (!hedgehog_backend_aarch64_cpreg_configurable(uc, errp)) {
        return false;
    }
    if (!ops || !ops->set_aarch64_cp_reg_value) {
        error_setg(errp,
                   "AArch64 CPREG live/reset values are unavailable in this backend");
        return false;
    }

    BQL_LOCK_GUARD();
    return ops->set_aarch64_cp_reg_value(
        uc->cpu, opc0, opc1, crn, crm, opc2, reset_value, value, errp);
}

bool hedgehog_backend_arm_diagnostics(HedgehogBackend *uc,
                                      HedgehogArmDiagnostics *diagnostics,
                                      Error **errp)
{
    const HedgehogArchOps *ops = hedgehog_arch_ops();

    if (!uc || !uc->cpu || !diagnostics) {
        error_setg(errp,
                   "backend, CPU, or diagnostics buffer is not initialized");
        return false;
    }
    if (!ops || !ops->arm_diagnostics) {
        error_setg(errp, "ARM diagnostics are unavailable in this backend");
        return false;
    }

    BQL_LOCK_GUARD();
    memset(diagnostics, 0, sizeof(*diagnostics));
    return ops->arm_diagnostics(uc->cpu, diagnostics, errp);
}

bool hedgehog_backend_restore_arm_generic_timers(
    HedgehogBackend *uc, const HedgehogArmGenericTimerState *state,
    Error **errp)
{
    const HedgehogArchOps *ops = hedgehog_arch_ops();

    if (!uc || !uc->cpu || !state || uc->board_backed) {
        error_setg(errp,
                   "generic-timer restore requires a stopped standalone backend");
        return false;
    }
    if (!ops || !ops->restore_arm_generic_timers) {
        error_setg(errp, "ARM generic-timer restore is unavailable in this backend");
        return false;
    }

    BQL_LOCK_GUARD();
    if (qatomic_read(&uc->cpu->running)) {
        error_setg(errp,
                   "generic-timer restore requires a stopped standalone backend");
        return false;
    }
    return ops->restore_arm_generic_timers(uc->cpu, state, errp);
}

CPUState *hedgehog_backend_cpu(HedgehogBackend *uc)
{
    return uc ? uc->cpu : NULL;
}

AddressSpace *hedgehog_backend_address_space(HedgehogBackend *uc)
{
    return uc ? uc->active_as : NULL;
}
