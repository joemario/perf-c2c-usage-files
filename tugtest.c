//compile with 
// gcc -Wall -o tugtest tugtest.c 

/* 
** simple program to test memory read/write cacheline contention.
*/

#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <sched.h>
#include <errno.h>
#include <unistd.h>
#include <sys/time.h>
#include <inttypes.h>
#include <linux/mempolicy.h>
#include <sys/mman.h>
#include <pthread.h>
#include <numaif.h>

#define cpu_relax()    asm volatile("rep; nop")
#define MAX_CPUS       1024

enum { FALSE,   TRUE    };
enum { SUCCESS, FAILURE };

struct reader_thread_arg {
  int cpunum;
  int idx;
} _rdthdarg[MAX_CPUS];

struct writer_thread_arg {
  int cpunum;
  int idx;
} _wrthdarg[MAX_CPUS];

volatile uint64_t lockmem[8] __attribute__((aligned(64)));
static double time_diff(struct timeval x , struct timeval y);

int getopt(int argc, char * const argv[], const char *optstring);
extern char *optarg;
extern int   optind;

volatile int wait_to_start = TRUE;
struct timeval tv_start, tv_stop;

int    sleep_cnt = 5;
long   loop_cnt = 2000000;
int    debug    = FALSE;
int    test_thd_cnt  = 0;
int    readerIdx = 0;
int    writerIdx = 0;

#define CPU_BIND(cpu) \
   do { \
      cpu_set_t cs; \
      CPU_ZERO (&cs); \
      CPU_SET (cpu, &cs); \
                \
      if (sched_setaffinity(0, sizeof (cs), &cs) < 0) { \
         perror("sched_setaffinity"); \
         exit(EXIT_FAILURE); \
      }\
   } while (0)

/*
** spin attempting to get lockmem 
*/
void acquire_lock(volatile uint64_t *lock) {
  uint64_t expected = 0, new = 1;
  uint64_t result = 0;
  
  while ((result = __atomic_compare_exchange_n(lock, &expected, &new, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) == 0) {
	; // Do nothing 
  }
}


void release_lock(volatile uint64_t *lock)
{
  uint64_t expected = 1, new = 0;
  uint64_t result = 0;
  
  while ((result = __atomic_compare_exchange_n(lock, &expected, &new, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) == 0) {
	; // Do nothing 
  }
}

void *writer(void *arg)
{
   register long i,j;
   struct writer_thread_arg *wt = arg;
   int cpu = wt->cpunum;
   int idx = wt->idx;
   volatile uint64_t *p = (volatile uint64_t *)((char *)lockmem );

   // Bind this thread to the cpu passed in.
   CPU_BIND(cpu);
        
   if (debug) 
      printf("Starting writer thread %d on cpu %d, accessing data at 0x%p.\n", idx, cpu, lockmem);
   
   if (idx == 0) {
      gettimeofday(&tv_start, NULL);
   }

   // Wait for all the threads to have been kicked off in main().
   while (wait_to_start)
      ;

   for (i = 0; i < loop_cnt; i++) {
      acquire_lock(p);
      for (j = 0; j < sleep_cnt; j++) cpu_relax();
      release_lock(p);
   }

   if (idx == 0) {
      gettimeofday(&tv_stop, NULL);
   }

   if (debug) printf("Writer on cpu %d finished\n", cpu);
   return 0;
}


void *reader(void *arg)
{
   register long i,j;
   struct reader_thread_arg *rt = arg;
   int cpu = rt->cpunum;
   int idx = rt->idx;

   // Start readers at 32 offset in cacheline 
   volatile uint64_t *varptr = (volatile uint64_t*)(((char *)lockmem) + 32);

   CPU_BIND(cpu);
   if (debug) {
       printf("Starting reader thread %d on cpu %d, accessing data at 0x%p.\n", idx, cpu, varptr);
       fflush(stdout);
   }

   while (wait_to_start)
      ;

   for (i = 0; i < loop_cnt ; i++) {
      // Force a load - printf should never happen.
      // 
      if (*varptr < 0 ) 
         printf("varptr < 0\n");
      for (j = 0; j < sleep_cnt; j++) 
	 cpu_relax();
   }

   if (debug) {
      printf("Reader on cpu %d finished\n", cpu); 
      fflush(stdout);
   }

   return 0;
}


int main(int argc, char *argv[])
{
   int    opt;
   char *usage = " ./tugtest -r<reader1-cpu> -r<reader2-cpu> -w<writer1-cpu> -w<writer2-cpu> \n Example: \n   ./tugtest -r2 -w6 -w21 -r10 -r3 -r17 -w8 -w19 -d \n   The above will create reader threads on cpus 2,10,3,17 and writers on cpus 6,21,8,19.  It will also set debug. \n   The -S flag can be used to change sleep cycles between loops. Default is -S5 \n   The -L flag is for the number of loops for each reader and writer thread to execute, with a default of -L2000000";

   //
   // process the command line 
   //
   while ((opt = getopt(argc, argv, "hptdm:r:w:L:S:")) != -1) {

      switch (opt) {

         // Debug
         case 'D':
         case 'd':
            debug = TRUE;
            break;

         // Reader threads
         case 'r':
            _rdthdarg[readerIdx].cpunum = atoi(optarg);
            _rdthdarg[readerIdx].idx = readerIdx;
            if (debug) printf("Reader cpu: %d, idx: %d\n", _rdthdarg[readerIdx].cpunum,  _rdthdarg[readerIdx].idx);
            readerIdx++;
            test_thd_cnt++;
            break;

         // Writer threads
         case 'w':
            _wrthdarg[writerIdx].cpunum = atoi(optarg);
            _wrthdarg[writerIdx].idx = writerIdx;
            if (debug) printf("Writer cpu: %d\n", _wrthdarg[writerIdx].cpunum);
            writerIdx++;
            test_thd_cnt++;
            break;

         // Loop count
         case 'L':
            loop_cnt = atoi(optarg);
            if (debug) printf("loop count is %ld\n",loop_cnt);
            if (loop_cnt < 1) {
               printf("loop cnt must be >= 1\n");
               exit(FAILURE);
            }
            break;

         // Sleep loop count
         case 'S':
            sleep_cnt = atoi(optarg);
            if (sleep_cnt < 0) {
               printf("sleep cnt, between loops, must be >= 0\n");
               exit(FAILURE);
            }
            break;

         case 'h': /* -h for help */
            printf("usage: %s\n", usage);

            exit(0);
      }
   }

   if (debug) {
       printf("Loop cnt: %ld\n", loop_cnt);
       printf("Sleep loop cnt, between loops: %d\n", sleep_cnt);
   }

   //
   // create the threads that will ping pong back and forth
   //
   pthread_t thread[test_thd_cnt];
   int i, ti;

   for (i=0;i<readerIdx; i++) {
       int rc = pthread_create(&thread[i], NULL, reader, &_rdthdarg[i] );
       if (rc) {
          perror("Reader pthread_create");
          exit(1);
       }
   }

   // 
   // Kick off writer threads, but skip the in-use reader thread[idx] values
   // 
   for (i=0, ti = readerIdx; ti < (readerIdx + writerIdx); ti++, i++) {
       int rc = pthread_create(&thread[ti], NULL, writer, &_wrthdarg[i] );
       if (rc) {
          perror("Writer pthread_create");
          exit(1);
       }
   }

   //
   // bind parent to cpu 0
   //
   CPU_BIND(0);

   // Let threads all get ready. 
   usleep(1000);

   // Let all threads begin.
   wait_to_start = FALSE;

   for (i=readerIdx;i< readerIdx + writerIdx;i++ ) {
       pthread_join(thread[i], NULL);
   }

   double delta_time_sec = time_diff(tv_start, tv_stop);

   printf("Time for first writer to acquire+release lock %ld times: %.2lf sec.\n", loop_cnt, delta_time_sec);

   for (i=0;i<readerIdx;i++ ) {
       pthread_join(thread[i], NULL);
   }

   return 0;
}

static double time_diff(struct timeval x , struct timeval y)
{
    double x_ms , y_ms , diff;

    x_ms = (double)x.tv_sec*1000000 + (double)x.tv_usec;
    y_ms = (double)y.tv_sec*1000000 + (double)y.tv_usec;

    diff = (double)y_ms - (double)x_ms;
    return diff/1000000;
}

