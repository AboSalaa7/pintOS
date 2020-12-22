#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "devices/pit.h"
#include "threads/interrupt.h"
#include "threads/synch.h"
#include "threads/thread.h"
  
/* See [8254] for hardware details of the 8254 timer chip. */

#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

static bool comparator_by_ticks ( const struct list_elem* temp_1 , const struct list_elem* temp_2 , void* aux ){

    struct thread* first = list_entry( temp_1 , struct thread , elem );
    struct thread* second = list_entry( temp_2 , struct thread , elem );
    ASSERT( first != NULL && second != NULL );
    if( first->time_to_wake < second->time_to_wake )
        return true ;
    else if ( first->time_to_wake == second->time_to_wake && first->priority > second->priority )
        return true ;
    return false ;
}

struct list blocked_threads ;

/* Number of timer ticks since OS booted. */
static int64_t ticks;

/* Number of loops per timer tick.
   Initialized by timer_calibrate(). */
static unsigned loops_per_tick;

static intr_handler_func timer_interrupt;
static bool too_many_loops (unsigned loops);
static void busy_wait (int64_t loops);
static void real_time_sleep (int64_t num, int32_t denom);
static void real_time_delay (int64_t num, int32_t denom);

/* Sets up the timer to interrupt TIMER_FREQ times per second,
   and registers the corresponding interrupt. */
void
timer_init (void)
{
  pit_configure_channel (0, 2, TIMER_FREQ);
  intr_register_ext (0x20, timer_interrupt, "8254 Timer");
  list_init(&blocked_threads);
  
}

/* Calibrates loops_per_tick, used to implement brief delays. */
void
timer_calibrate (void)
{
  unsigned high_bit, test_bit;

  ASSERT (intr_get_level () == INTR_ON);
  printf ("Calibrating timer...  ");

  /* Approximate loops_per_tick as the largest power-of-two
     still less than one timer tick. */
  loops_per_tick = 1u << 10;
  while (!too_many_loops (loops_per_tick << 1))
    {
      loops_per_tick <<= 1;
      ASSERT (loops_per_tick != 0);
    }

  /* Refine the next 8 bits of loops_per_tick. */
  high_bit = loops_per_tick;
  for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
    if (!too_many_loops (loops_per_tick | test_bit))
      loops_per_tick |= test_bit;

  printf ("%'"PRIu64" loops/s.\n", (uint64_t) loops_per_tick * TIMER_FREQ);
}

/* Returns the number of timer ticks since the OS booted. */
int64_t
timer_ticks (void)
{
  enum intr_level old_level = intr_disable ();
  int64_t t = ticks;
  intr_set_level (old_level);
  return t;
}

/* Returns the number of timer ticks elapsed since THEN, which
   should be a value once returned by timer_ticks(). */
int64_t
timer_elapsed (int64_t then)
{
  return timer_ticks () - then;
}

/* Sleeps for approximately TICKS timer ticks.  Interrupts must
   be turned on. */
void
timer_sleep (int64_t ticks)
{
    enum intr_level x = intr_disable();

    thread_current()->time_to_wake = timer_ticks() + ticks ;

    list_insert_ordered( &blocked_threads , &thread_current()->elem , comparator_by_ticks , NULL );

    thread_block();

    intr_set_level(x);

    /*
  int64_t start = timer_ticks ();

  ASSERT (intr_get_level () == INTR_ON);
  while (timer_elapsed (start) < ticks)
    thread_yield ();
    */
}

/* Sleeps for approximately MS milliseconds.  Interrupts must be
   turned on. */
void
timer_msleep (int64_t ms)
{
  real_time_sleep (ms, 1000);
}

/* Sleeps for approximately US microseconds.  Interrupts must be
   turned on. */
void
timer_usleep (int64_t us)
{
  real_time_sleep (us, 1000 * 1000);
}

/* Sleeps for approximately NS nanoseconds.  Interrupts must be
   turned on. */
void
timer_nsleep (int64_t ns)
{
  real_time_sleep (ns, 1000 * 1000 * 1000);
}

/* Busy-waits for approximately MS milliseconds.  Interrupts need
   not be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_msleep()
   instead if interrupts are enabled. */
void
timer_mdelay (int64_t ms)
{
  real_time_delay (ms, 1000);
}

/* Sleeps for approximately US microseconds.  Interrupts need not
   be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_usleep()
   instead if interrupts are enabled. */
void
timer_udelay (int64_t us)
{
  real_time_delay (us, 1000 * 1000);
}

/* Sleeps execution for approximately NS nanoseconds.  Interrupts
   need not be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_nsleep()
   instead if interrupts are enabled.*/
void
timer_ndelay (int64_t ns)
{
  real_time_delay (ns, 1000 * 1000 * 1000);
}

/* Prints timer statistics. */
void
timer_print_stats (void)
{
  printf ("Timer: %"PRId64" ticks\n", timer_ticks ());
}

/* Timer interrupt handler. */
static void
timer_interrupt (struct intr_frame *args UNUSED)
{

  
  enum intr_level x = intr_disable() ;

  ticks++;
  thread_tick ();
  
  struct list_elem *e ;

  

  if( ! list_empty( &blocked_threads ) ){

    for ( e = list_begin ( &blocked_threads ); e != list_end( &blocked_threads ) ; e = list_begin(&blocked_threads)   ){
        struct thread* x = list_entry( e , struct thread , elem );
        if ( x->time_to_wake > timer_ticks() )
            break ;
        list_remove(e);
        thread_unblock( x );
        if( list_empty( &blocked_threads ) )
            break ;
    }
  }

  if( thread_mlfqs ){

     if( thread_current() != get_idle_thread() )
        thread_current()->recent_cpu = add_fixed_int( thread_current()->recent_cpu , 1 )  ;

    if(timer_ticks() % TIMER_FREQ ==0){
          
    real x1 = int_to_fixed(59);
    real x2 = div_fixed_int(x1,60);
    x2 = mul_two_fixed(x2,load_avg);
    real x3 = int_to_fixed(1);
    real x4 = div_fixed_int(x3,60);
    
    x4 = mul_fixed_int( x4 , list_size( get_ready_list() ) + thread_current() == get_idle_thread() ? 0 : 1 );
    load_avg = add_two_fixed( x2 , x4 );
    

    
    for( e = list_begin( get_all_list() ) ; e != list_end( get_all_list() ) ; e = list_next(e) ){
        struct thread* x = list_entry( e , struct thread , elem );
    
        if(x != get_idle_thread()){
        x1 = mul_fixed_int(load_avg,2);
        x2 = add_fixed_int(x1,1);
        x1 = div_two_fixed(x1,x2);
        x1 = mul_two_fixed( x1 , x->recent_cpu );
        x->recent_cpu = add_fixed_int(x1 , x->nice );
       
        
    }
    }

    if(timer_ticks() % 4 == 0){
     for( e = list_begin( get_all_list() ) ; e != list_end( get_all_list() ) ; e = list_next(e) ){
        struct thread* x = list_entry( e , struct thread , elem );
    
        if(x != get_idle_thread()){
        x1 = div_fixed_int( x->recent_cpu , 4  );
        x2 = int_to_fixed(64);
        x2 = sub_two_fixed(64,x1);
        x2 = sub_fixed_int(x2 , x->nice * 2 );
        x->priority = fixed_to_nearest_int(x2) ;
        if( x->priority > 63 )
            x->priority = 63 ;
        else if ( x->priority < 0  )
            x->priority = 0 ;
        }
        
    }
    }
  }
  }
  intr_yield_on_return();

  intr_set_level(x);

}

/* Returns true if LOOPS iterations waits for more than one timer
   tick, otherwise false. */
static bool
too_many_loops (unsigned loops)
{
  /* Wait for a timer tick. */
  int64_t start = ticks;
  while (ticks == start)
    barrier ();

  /* Run LOOPS loops. */
  start = ticks;
  busy_wait (loops);

  /* If the tick count changed, we iterated too long. */
  barrier ();
  return start != ticks;
}

/* Iterates through a simple loop LOOPS times, for implementing
   brief delays.

   Marked NO_INLINE because code alignment can significantly
   affect timings, so that if this function was inlined
   differently in different places the results would be difficult
   to predict. */
static void NO_INLINE
busy_wait (int64_t loops)
{
  while (loops-- > 0)
    barrier ();
}

/* Sleep for approximately NUM/DENOM seconds. */
static void
real_time_sleep (int64_t num, int32_t denom)
{
  /* Convert NUM/DENOM seconds into timer ticks, rounding down.

        (NUM / DENOM) s
     ---------------------- = NUM * TIMER_FREQ / DENOM ticks.
     1 s / TIMER_FREQ ticks
  */
  int64_t ticks = num * TIMER_FREQ / denom;

  ASSERT (intr_get_level () == INTR_ON);
  if (ticks > 0)
    {
      /* We're waiting for at least one full timer tick.  Use
         timer_sleep() because it will yield the CPU to other
         processes. */
      timer_sleep (ticks);
    }
  else
    {
      /* Otherwise, use a busy-wait loop for more accurate
         sub-tick timing. */
      real_time_delay (num, denom);
    }
}

/* Busy-wait for approximately NUM/DENOM seconds. */
static void
real_time_delay (int64_t num, int32_t denom)
{
  /* Scale the numerator and denominator down by 1000 to avoid
     the possibility of overflow. */
  ASSERT (denom % 1000 == 0);
  busy_wait (loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000));
}