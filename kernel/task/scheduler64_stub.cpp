#include "scheduler.hpp"

using namespace kernel::task::scheduler;

namespace kernel::task::scheduler {

int create_kernel_task(TaskEntry entry, uint32_t stack_size) { (void)entry; (void)stack_size; return -1; }

int create_process_task(const char* process_name, TaskEntry entry, uint32_t stack_size) { (void)process_name; (void)entry; (void)stack_size; return -1; }

int create_ring3_task(const char* process_name, uint32_t entry_point, uint32_t user_esp0) { (void)process_name; (void)entry_point; (void)user_esp0; return -1; }

uint32_t on_timer_interrupt(uint32_t current_esp, uint32_t tick_count) { (void)tick_count; return current_esp; }
uint32_t current_task_id() { return 0; }
uint32_t current_thread_id() { return 0; }
uint32_t current_process_id() { return 0; }
uint32_t current_ring_level() { return 0; }
uint32_t current_user_id() { return 0; }
uint32_t current_group_id() { return 0; }
const char* current_home() { return ""; }
uint32_t process_count() { return 0; }
uint32_t thread_count() { return 0; }
void exit_current() {}

void sleep_current(uint32_t ticks, uint32_t now_tick) { (void)ticks; (void)now_tick; }
bool is_current_sleeping(uint32_t now_tick) { (void)now_tick; return false; }

} // namespace kernel::task::scheduler

extern "C" void task_entry_trampoline() {}
