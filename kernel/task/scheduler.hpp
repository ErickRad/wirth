#pragma once

#include <stdint.h>

namespace kernel::task::scheduler {

using TaskEntry = void (*)();

void init(uint32_t kernel_stack_esp);
int create_kernel_task(TaskEntry entry, uint32_t stack_size);
int create_process_task(const char* process_name, TaskEntry entry, uint32_t stack_size);
int create_ring3_task(const char* process_name, uint32_t entry_point, uint32_t user_esp0);

uint32_t on_timer_interrupt(uint32_t current_esp, uint32_t tick_count);
uint32_t current_task_id();
uint32_t current_thread_id();
uint32_t current_process_id();
uint32_t current_ring_level();
uint32_t current_user_id();
uint32_t current_group_id();
const char* current_home();
uint32_t process_count();
uint32_t thread_count();
void exit_current();

void sleep_current(uint32_t ticks, uint32_t now_tick);
bool is_current_sleeping(uint32_t now_tick);

}  // namespace kernel::task::scheduler

extern "C" void task_entry_trampoline();
