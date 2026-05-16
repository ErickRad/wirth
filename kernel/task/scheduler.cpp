#include "scheduler.hpp"

#include <stdint.h>

#include "../arch/x86/gdt.hpp"
#include "../mm/heap.hpp"
#include "../mm/vmm.hpp"

namespace {

constexpr uint32_t kMaxTasks = 16;
constexpr uint32_t kMaxProcesses = 8;
constexpr uint32_t kKernelCodeSelector = 0x08;
constexpr uint32_t kEflagsInterruptEnabled = 0x202;
constexpr uint32_t kQuantumTicks = 10;
constexpr uint32_t kNoWakeTick = 0xFFFFFFFFu;

enum class TaskState : uint8_t {
    kUnused = 0,
    kRunnable,
    kSleeping,
    kExited,
};

struct Task {
    TaskState state;
    uint32_t tid;
    uint32_t pid;
    uint32_t* saved_esp;
    void (*entry)();
    uint32_t wake_tick;
    uint32_t ring;               // 0 = kernel, 3 = user
    uint32_t user_entry;         // ring3: entry point
    uint32_t user_esp0;          // ring3: user stack pointer
    uint32_t kernel_stack_top;   // ring3: kernel stack top used by TSS.esp0
    bool user_started;           // true after first iret to ring3
};

struct Process {
    bool used;
    uint32_t pid;
    uint32_t parent_pid;
    uint32_t uid;
    uint32_t gid;
    char home[32];
    char name[24];
    uint32_t live_threads;
    kernel::mm::vmm::AddressSpace address_space;
};

Task g_tasks[kMaxTasks] = {};
Process g_processes[kMaxProcesses] = {};
uint32_t g_current_task = 0;
uint32_t g_next_pid = 1;
uint32_t g_next_tid = 1;
uint32_t g_process_count = 0;
uint32_t g_thread_count = 0;

void copy_text(char* dst, const char* src, uint32_t max_size) {
    uint32_t i = 0;
    if (src == nullptr || max_size == 0) {
        return;
    }
    while (i + 1 < max_size && src[i] != '\0') {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

void clear_task(Task& task) {
    task.state = TaskState::kUnused;
    task.tid = 0;
    task.pid = 0;
    task.saved_esp = nullptr;
    task.entry = nullptr;
    task.wake_tick = kNoWakeTick;
    task.ring = 0;
    task.user_entry = 0;
    task.user_esp0 = 0;
    task.kernel_stack_top = 0;
    task.user_started = false;
}

void clear_process(Process& process) {
    process.used = false;
    process.pid = 0;
    process.parent_pid = 0;
    process.uid = 0;
    process.gid = 0;
    process.home[0] = '\0';
    process.name[0] = '\0';
    process.live_threads = 0;
    process.address_space = nullptr;
}

Process* find_process(uint32_t pid) {
    for (uint32_t i = 0; i < kMaxProcesses; ++i) {
        if (g_processes[i].used && g_processes[i].pid == pid) {
            return &g_processes[i];
        }
    }
    return nullptr;
}

uint32_t create_process(const char* name, uint32_t parent_pid) {
    for (uint32_t i = 0; i < kMaxProcesses; ++i) {
        if (g_processes[i].used) {
            continue;
        }
        Process& process = g_processes[i];
        process.used = true;
        process.pid = g_next_pid++;
        process.parent_pid = parent_pid;
        process.live_threads = 0;
        if (g_process_count == 0 && parent_pid == 0) {
            process.uid = 0;
            process.gid = 0;
            copy_text(process.home, "/root", sizeof(process.home));
        } else {
            Process* const parent = find_process(parent_pid);
            process.uid = (parent == nullptr) ? 0 : parent->uid;
            process.gid = (parent == nullptr) ? 0 : parent->gid;
            copy_text(
                process.home,
                (parent == nullptr || parent->home[0] == '\0') ? "/root" : parent->home,
                sizeof(process.home));
        }
        if (g_process_count == 0 && parent_pid == 0) {
            process.address_space = kernel::mm::vmm::kernel_address_space();
        } else {
            process.address_space = kernel::mm::vmm::create_kernel_clone_address_space();
            if (process.address_space == nullptr) {
                clear_process(process);
                return 0;
            }
        }
        copy_text(process.name, (name == nullptr) ? "process" : name, sizeof(process.name));
        ++g_process_count;
        return process.pid;
    }
    return 0;
}

void setup_initial_task_stack(Task& task, uint32_t stack_base, uint32_t stack_size) {
    uint32_t* sp = reinterpret_cast<uint32_t*>(stack_base + stack_size);

    *(--sp) = kEflagsInterruptEnabled;
    *(--sp) = kKernelCodeSelector;
    *(--sp) = reinterpret_cast<uint32_t>(task_entry_trampoline);

    *(--sp) = 0;  // eax
    *(--sp) = 0;  // ecx
    *(--sp) = 0;  // edx
    *(--sp) = 0;  // ebx
    *(--sp) = 0;  // esp
    *(--sp) = 0;  // ebp
    *(--sp) = 0;  // esi
    *(--sp) = 0;  // edi

    task.saved_esp = sp;
}

int alloc_task_slot() {
    for (uint32_t i = 0; i < kMaxTasks; ++i) {
        if (g_tasks[i].state == TaskState::kUnused || g_tasks[i].state == TaskState::kExited) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int create_task_for_process(uint32_t pid, kernel::task::scheduler::TaskEntry entry, uint32_t stack_size) {
    if (entry == nullptr) {
        return -1;
    }
    Process* process = find_process(pid);
    if (process == nullptr) {
        return -1;
    }

    const int slot_index = alloc_task_slot();
    if (slot_index < 0) {
        return -1;
    }
    Task& slot = g_tasks[slot_index];
    clear_task(slot);

    const uint32_t stack_bytes = (stack_size < 4096u) ? 4096u : stack_size;
    void* const stack_mem = kernel::mm::heap::alloc(stack_bytes, 16);
    if (stack_mem == nullptr) {
        clear_task(slot);
        return -1;
    }

    slot.state = TaskState::kRunnable;
    slot.tid = g_next_tid++;
    slot.pid = pid;
    slot.entry = entry;
    slot.wake_tick = kNoWakeTick;
    setup_initial_task_stack(slot, reinterpret_cast<uint32_t>(stack_mem), stack_bytes);

    ++process->live_threads;
    ++g_thread_count;
    return static_cast<int>(slot.tid);
}

void wake_sleeping_tasks(uint32_t now_tick) {
    for (uint32_t i = 0; i < kMaxTasks; ++i) {
        Task& task = g_tasks[i];
        if (task.state == TaskState::kSleeping && now_tick >= task.wake_tick) {
            task.state = TaskState::kRunnable;
            task.wake_tick = kNoWakeTick;
        }
    }
}

uint32_t pick_next_runnable(uint32_t current_index) {
    for (uint32_t offset = 1; offset <= kMaxTasks; ++offset) {
        const uint32_t candidate = (current_index + offset) % kMaxTasks;
        if (g_tasks[candidate].state == TaskState::kRunnable) {
            return candidate;
        }
    }
    return current_index;
}

Task* current_task_ptr() {
    if (g_current_task >= kMaxTasks) {
        return nullptr;
    }
    if (g_tasks[g_current_task].state == TaskState::kUnused) {
        return nullptr;
    }
    return &g_tasks[g_current_task];
}

void mark_current_exited() {
    Task* task = current_task_ptr();
    if (task == nullptr || task->state == TaskState::kExited) {
        return;
    }
    Process* process = find_process(task->pid);
    if (process != nullptr && process->live_threads > 0) {
        --process->live_threads;
    }
    if (g_thread_count > 0) {
        --g_thread_count;
    }
    task->state = TaskState::kExited;
    task->wake_tick = kNoWakeTick;
}

}  // namespace

namespace {

// Helper: prepare IRET frame for ring3 entry on the stack
// Returns pointer to prepared frame for popa + iret
uint32_t* prepare_ring3_iret_frame(uint32_t* kernel_stack_top, uint32_t ring3_entry, uint32_t ring3_esp) {
    uint32_t* sp = kernel_stack_top;
    
    // Build frame that will be consumed by popa, then iret to ring3
    // After popa, stack has: [SS, ESP, EFLAGS, CS, EIP]
    // iret will pop them in reverse order
    
    // Layout for popa (restores edi, esi, ebp, esp, ebx, edx, ecx, eax):
    *(--sp) = 0;           // edi
    *(--sp) = 0;           // esi
    *(--sp) = 0;           // ebp
    *(--sp) = 0;           // esp (ignored, will be popped but not used)
    *(--sp) = 0;           // ebx
    *(--sp) = 0;           // edx
    *(--sp) = 0;           // ecx
    *(--sp) = 0;           // eax
    
    // After popa, stack has these for iret with ring change:
    *(--sp) = 0x23;        // SS (user data segment)
    *(--sp) = ring3_esp;   // ESP (user stack pointer)
    *(--sp) = 0x202;       // EFLAGS (IF enabled, no other flags)
    *(--sp) = 0x1B;        // CS (user code segment)
    *(--sp) = ring3_entry; // EIP (entry point)
    
    return sp;
}

}  // namespace

namespace kernel::task::scheduler {

void init(uint32_t kernel_stack_esp) {
    for (uint32_t i = 0; i < kMaxTasks; ++i) {
        clear_task(g_tasks[i]);
    }
    for (uint32_t i = 0; i < kMaxProcesses; ++i) {
        clear_process(g_processes[i]);
    }

    g_current_task = 0;
    g_next_pid = 1;
    g_next_tid = 1;
    g_process_count = 0;
    g_thread_count = 0;

    const uint32_t kernel_pid = create_process("kernel", 0);
    if (kernel_pid == 0) {
        return;
    }
    Process* kernel_process = find_process(kernel_pid);
    g_tasks[0].state = TaskState::kRunnable;
    g_tasks[0].tid = g_next_tid++;
    g_tasks[0].pid = kernel_pid;
    g_tasks[0].saved_esp = reinterpret_cast<uint32_t*>(kernel_stack_esp);
    g_tasks[0].entry = nullptr;
    g_tasks[0].wake_tick = kNoWakeTick;
    if (kernel_process != nullptr) {
        kernel_process->live_threads = 1;
    }
    g_thread_count = 1;
}

int create_kernel_task(TaskEntry entry, uint32_t stack_size) {
    return create_task_for_process(1, entry, stack_size);
}

int create_process_task(const char* process_name, TaskEntry entry, uint32_t stack_size) {
    const uint32_t pid = create_process(process_name, current_process_id());
    if (pid == 0) {
        return -1;
    }
    if (create_task_for_process(pid, entry, stack_size) < 0) {
        Process* process = find_process(pid);
        if (process != nullptr) {
            clear_process(*process);
            if (g_process_count > 0) {
                --g_process_count;
            }
        }
        return -1;
    }
    return static_cast<int>(pid);
}

int create_ring3_task(const char* process_name, uint32_t entry_point, uint32_t user_esp0) {
    const uint32_t pid = create_process(process_name, current_process_id());
    if (pid == 0) {
        return -1;
    }

    Process* process = find_process(pid);
    if (process == nullptr) {
        return -1;
    }

    const int slot_index = alloc_task_slot();
    if (slot_index < 0) {
        clear_process(*process);
        if (g_process_count > 0) {
            --g_process_count;
        }
        return -1;
    }

    Task& slot = g_tasks[slot_index];
    clear_task(slot);

    // Setup ring3 task (kernel stack still needed for syscalls/interrupts)
    const uint32_t stack_bytes = 4096;
    void* const stack_mem = kernel::mm::heap::alloc(stack_bytes, 16);
    if (stack_mem == nullptr) {
        clear_task(slot);
        clear_process(*process);
        if (g_process_count > 0) {
            --g_process_count;
        }
        return -1;
    }

    slot.state = TaskState::kRunnable;
    slot.tid = g_next_tid++;
    slot.pid = pid;
    slot.ring = 3;               // Ring3 flag
    slot.user_entry = entry_point;
    slot.user_esp0 = user_esp0;
    slot.wake_tick = kNoWakeTick;
    slot.user_started = false;

    // For ring3, saved_esp points to kernel stack (will be prepared by scheduler)
    uint32_t* sp = reinterpret_cast<uint32_t*>(reinterpret_cast<uint32_t>(stack_mem) + stack_bytes);
    slot.saved_esp = sp;
    slot.kernel_stack_top = reinterpret_cast<uint32_t>(sp);

    ++process->live_threads;
    ++g_thread_count;
    return static_cast<int>(pid);
}

uint32_t on_timer_interrupt(uint32_t current_esp, uint32_t tick_count) {
    Task* current = current_task_ptr();
    if (current == nullptr) {
        return current_esp;
    }

    current->saved_esp = reinterpret_cast<uint32_t*>(current_esp);
    wake_sleeping_tasks(tick_count);

    if ((tick_count % kQuantumTicks) != 0) {
        return current_esp;
    }

    const uint32_t next = pick_next_runnable(g_current_task);
    const uint32_t prev = g_current_task;
    g_current_task = next;
    if (g_tasks[prev].pid != g_tasks[g_current_task].pid) {
        Process* const next_process = find_process(g_tasks[g_current_task].pid);
        if (next_process != nullptr && next_process->address_space != nullptr) {
            kernel::mm::vmm::switch_address_space(next_process->address_space);
        }
    }
    if (g_tasks[g_current_task].saved_esp == nullptr) {
        return current_esp;
    }

    Task& next_task = g_tasks[g_current_task];

    if (next_task.ring == 3) {
        // Ensure ring3 transitions use the task-private kernel stack on next CPL3->CPL0 entry.
        if (next_task.kernel_stack_top != 0) {
            kernel::arch::x86::gdt::set_kernel_stack(next_task.kernel_stack_top);
        }

        // First dispatch to ring3 must synthesize a ring-change iret frame.
        // Subsequent preemptions resume from the saved interrupt frame.
        if (!next_task.user_started) {
            next_task.user_started = true;
            uint32_t* frame = prepare_ring3_iret_frame(
                next_task.saved_esp,
                next_task.user_entry,
                next_task.user_esp0
            );
            return reinterpret_cast<uint32_t>(frame);
        }
    }

    if (next_task.ring != 3 && prev != g_current_task) {
        // Kernel-only tasks keep using their own interrupt stack snapshots.
        kernel::arch::x86::gdt::set_kernel_stack(reinterpret_cast<uint32_t>(next_task.saved_esp));
    }

    // If next task is ring3 and already started, restore interrupted frame directly.
    if (next_task.ring == 3 && next_task.user_started) {
        return reinterpret_cast<uint32_t>(next_task.saved_esp);
    }

    return reinterpret_cast<uint32_t>(g_tasks[g_current_task].saved_esp);
}

uint32_t current_task_id() {
    const Task* current = current_task_ptr();
    return (current == nullptr) ? 0 : current->tid;
}

uint32_t current_thread_id() {
    return current_task_id();
}

uint32_t current_process_id() {
    const Task* current = current_task_ptr();
    return (current == nullptr) ? 0 : current->pid;
}

uint32_t current_ring_level() {
    const Task* current = current_task_ptr();
    return (current == nullptr) ? 0 : current->ring;
}

uint32_t process_count() {
    return g_process_count;
}

uint32_t current_user_id() {
    const Task* current = current_task_ptr();
    if (current == nullptr) {
        return 0;
    }
    const Process* process = find_process(current->pid);
    return (process == nullptr) ? 0 : process->uid;
}

uint32_t current_group_id() {
    const Task* current = current_task_ptr();
    if (current == nullptr) {
        return 0;
    }
    const Process* process = find_process(current->pid);
    return (process == nullptr) ? 0 : process->gid;
}

const char* current_home() {
    const Task* current = current_task_ptr();
    if (current == nullptr) {
        return "/root";
    }
    const Process* process = find_process(current->pid);
    if (process == nullptr || process->home[0] == '\0') {
        return "/root";
    }
    return process->home;
}

uint32_t thread_count() {
    return g_thread_count;
}

void exit_current() {
    mark_current_exited();
}

void sleep_current(uint32_t ticks, uint32_t now_tick) {
    if (ticks == 0) {
        return;
    }
    Task* current = current_task_ptr();
    if (current == nullptr) {
        return;
    }
    current->state = TaskState::kSleeping;
    current->wake_tick = now_tick + ticks;
}

bool is_current_sleeping(uint32_t now_tick) {
    Task* current = current_task_ptr();
    if (current == nullptr || current->state != TaskState::kSleeping) {
        return false;
    }
    return now_tick < current->wake_tick;
}

}  // namespace kernel::task::scheduler

extern "C" void task_entry_trampoline() {
    using namespace kernel::task::scheduler;
    const uint32_t current_tid = current_task_id();

    for (uint32_t i = 0; i < kMaxTasks; ++i) {
        if (g_tasks[i].tid == current_tid && g_tasks[i].entry != nullptr) {
            g_tasks[i].entry();
            mark_current_exited();
            break;
        }
    }

    while (true) {
        asm volatile("hlt");
    }
}
