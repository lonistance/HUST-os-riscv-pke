/*
 * implementing the scheduler
 */

#include "sched.h"
#include "spike_interface/spike_utils.h"

process* ready_queue_head = NULL;
process* block_queue_head = NULL;
void tem_show_ready_queue() {
  process* p = ready_queue_head;
  sprint( "ready queue: " );
  while ( p != NULL ) {
    sprint( "pid = %d,status = %d-> ", p->pid, p->status );
    p = p->queue_next;
  }
  sprint( "NULL\n" );
}
//
// insert a process, proc, into the END of ready queue.
//
void insert_to_ready_queue( process* proc ) {
  sprint( "going to insert process %d to ready queue.\n", proc->pid );
  // if the queue is empty in the beginning
  if( ready_queue_head == NULL ){
    proc->status = READY;
    proc->queue_next = NULL;
    ready_queue_head = proc;
    sprint( "ready queue is empty.\n" );
    tem_show_ready_queue();
    return;
  }

  // ready queue is not empty
  process *p;
  // browse the ready queue to see if proc is already in-queue
  for( p=ready_queue_head; p->queue_next!=NULL; p=p->queue_next )
    if( p == proc ) {
      return;  //already in queue
    }
  // p points to the last element of the ready queue
  if( p==proc ) return;
  p->queue_next = proc;
  proc->status = READY;
  proc->queue_next = NULL;
  return;
}

//
// insert a process, proc, into the END of block queue.
//
void insert_to_block_queue( process* proc ) {
  //sprint( "going to insert process %d to block queue.\n", proc->pid );
  // if the queue is empty in the beginning
  sprint( "process %d is blocked\n", proc->pid );
  if( block_queue_head == NULL ){
    proc->status = BLOCKED;
    proc->queue_next = NULL;
    ready_queue_head = proc;//when copy it, I spell it as ready_queue_head,bug here!It should be block_queue_head
    return;
  }

  // block queue is not empty
  process *p;
  // browse the ready queue to see if proc is already in-queue
  for( p=block_queue_head; p->queue_next!=NULL; p=p->queue_next )
    if( p == proc ) return;  //already in queue

  // p points to the last element of the block queue
  if( p==proc ) return;
  p->queue_next = proc;
  proc->status = BLOCKED;
  proc->queue_next = NULL;

  return;
}

//
// choose a proc from the ready queue, and put it to run.
// note: schedule() does not take care of previous current process. If the current
// process is still runnable, you should place it into the ready queue (by calling
// ready_queue_insert), and then call schedule().
//
extern process procs[NPROC];
void schedule() {
  if ( !ready_queue_head ){
    // by default, if there are no ready process, and all processes are in the status of
    // FREE and ZOMBIE, we should shutdown the emulated RISC-V machine.
    int should_shutdown = 1;

    for( int i=0; i<NPROC; i++ )
      if( (procs[i].status != FREE) && (procs[i].status != ZOMBIE) ){
        should_shutdown = 0;
        sprint( "ready queue empty, but process %d is not in free/zombie state:%d\n", 
          i, procs[i].status );
      }

    if( should_shutdown ){
      sprint( "no more ready processes, system shutdown now.\n" );
      shutdown( 0 );
    }else{
      panic( "Not handled: we should let system wait for unfinished processes.\n" );
    }
  }

  tem_show_ready_queue();
  current = ready_queue_head;
  assert( current->status == READY );
  ready_queue_head = ready_queue_head->queue_next;

  current->status = RUNNING;
  sprint( "going to schedule process %d to run.\n", current->pid );
  switch_to( current );
}

// when a process is done, wake up the parent process from block queue, and put it into ready queue.
void wake_up( process* proc ) {
  process* wake = block_queue_head;
  if ( wake == NULL ) {
    sprint( "block queue is empty, nothing to wake up.\n" );
    return;
  } else if ( wake == proc->parent) {
    block_queue_head = wake->queue_next;
    wake->status = READY;
    sprint( "going to wake up process %d from block queue.\n", proc->pid );
    insert_to_ready_queue( wake );
    return;
  }
  else {
    while ( wake->queue_next != NULL ) {
      if ( wake->queue_next == proc->parent ) {
        process* tmp = wake->queue_next;
        wake->queue_next = tmp->queue_next;
        tmp->status = READY;
        sprint( "going to wake up process %d from block queue.\n", proc->pid );
        insert_to_ready_queue( tmp );
        return;
      }
      wake = wake->queue_next;
    }
  }
}
