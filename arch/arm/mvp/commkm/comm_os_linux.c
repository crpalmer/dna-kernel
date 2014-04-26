/*
 * Linux 2.6.32 and later Kernel module for VMware MVP Guest Communications
 *
 * Copyright (C) 2010-2012 VMware, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; see the file COPYING.  If not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */
#line 5


#include "comm_os.h"

#define DISPATCH_MAX_CYCLES 8192


typedef struct workqueue_struct CommOSWorkQueue;



static volatile int running;
static int numCpus;
static CommOSWorkQueue *dispatchWQ;
static CommOSDispatchFunc dispatch;
static CommOSWork dispatchWorksNow[NR_CPUS];
static CommOSWork dispatchWorks[NR_CPUS];
static unsigned int dispatchInterval = 1;
static unsigned int dispatchMaxCycles = 2048;
static CommOSWorkQueue *aioWQ;



static inline CommOSWorkQueue *
CreateWorkqueue(const char *name)
{
   return create_workqueue(name);
}



static inline void
DestroyWorkqueue(CommOSWorkQueue *wq)
{
   destroy_workqueue(wq);
}



static inline void
FlushDelayedWork(CommOSWork *work)
{
   flush_delayed_work(work);
}



static inline int
QueueDelayedWorkOn(int cpu,
                   CommOSWorkQueue *wq,
                   CommOSWork *work,
                   unsigned long jif)
{
   if (cpu < 0) {
      return !queue_delayed_work(wq, work, jif) ? -1 : 0;
   } else {
      return !queue_delayed_work_on(cpu, wq, work, jif) ? -1 : 0;
   }
}



static inline int
QueueDelayedWork(CommOSWorkQueue *wq,
                 CommOSWork *work,
                 unsigned long jif)
{
   return QueueDelayedWorkOn(-1, wq, work, jif);
}



static inline void
WaitForDelayedWork(CommOSWork *work)
{
   cancel_delayed_work_sync(work);
}



static inline void
FlushWorkqueue(CommOSWorkQueue *wq)
{
   flush_workqueue(wq);
}



void
CommOS_ScheduleDisp(void)
{
   CommOSWork *work = &dispatchWorksNow[get_cpu()];

   put_cpu();
   if (running) {
      QueueDelayedWork(dispatchWQ, work, 0);
   }
}



static void
DispatchWrapper(CommOSWork *work)
{
   unsigned int misses;

   for (misses = 0; running && (misses < dispatchMaxCycles); ) {
      

      if (!dispatch()) {
         

         misses++;
         if ((misses % 32) == 0) {
            CommOS_Yield();
         }
      } else {
         misses = 0;
      }
   }

   if (running &&
       (work >= &dispatchWorks[0]) &&
       (work <= &dispatchWorks[NR_CPUS - 1])) {

      QueueDelayedWork(dispatchWQ, work, dispatchInterval);
   }
}



void
CommOS_InitWork(CommOSWork *work,
                CommOSWorkFunc func)
{
   INIT_DELAYED_WORK(work, (work_func_t)func);
}


void
CommOS_FlushAIOWork(CommOSWork *work)
{
   if (aioWQ && work) {
      FlushDelayedWork(work);
   }
}



int
CommOS_ScheduleAIOWork(CommOSWork *work)
{
   if (running && aioWQ && work) {
      return QueueDelayedWork(aioWQ, work, 0);
   }
   return -1;
}



int
CommOS_StartIO(const char *dispatchTaskName,    
               CommOSDispatchFunc dispatchFunc, 
               unsigned int intervalMillis,     
               unsigned int maxCycles,          
               const char *aioTaskName)         
{
   int rc;
   int cpu;

   if (running) {
      CommOS_Debug(("%s: I/O tasks already running.\n", __FUNCTION__));
      return 0;
   }


   if (!dispatchFunc) {
      CommOS_Log(("%s: a NULL Dispatch handler was passed.\n", __FUNCTION__));
      return -1;
   }
   dispatch = dispatchFunc;

   if (intervalMillis == 0) {
      intervalMillis = 4;
   }
   if ((dispatchInterval = msecs_to_jiffies(intervalMillis)) < 1) {
      dispatchInterval = 1;
   }
   if (maxCycles > DISPATCH_MAX_CYCLES) {
      dispatchMaxCycles = DISPATCH_MAX_CYCLES;
   } else if (maxCycles > 0) {
      dispatchMaxCycles = maxCycles;
   }
   CommOS_Debug(("%s: Interval millis %u (jif:%u).\n", __FUNCTION__,
                 intervalMillis, dispatchInterval));
   CommOS_Debug(("%s: Max cycles %u.\n", __FUNCTION__, dispatchMaxCycles));

   numCpus = num_present_cpus();
   dispatchWQ = CreateWorkqueue(dispatchTaskName);
   if (!dispatchWQ) {
      CommOS_Log(("%s: Couldn't create %s task(s).\n", __FUNCTION__,
                  dispatchTaskName));
      return -1;
   }

   if (aioTaskName) {
      aioWQ = CreateWorkqueue(aioTaskName);
      if (!aioWQ) {
         CommOS_Log(("%s: Couldn't create %s task(s).\n", __FUNCTION__,
                     aioTaskName));
         DestroyWorkqueue(dispatchWQ);
         return -1;
      }
   } else {
      aioWQ = NULL;
   }

   running = 1;
   for (cpu = 0; cpu < numCpus; cpu++) {
      CommOS_InitWork(&dispatchWorksNow[cpu], DispatchWrapper);
      CommOS_InitWork(&dispatchWorks[cpu], DispatchWrapper);
      rc = QueueDelayedWorkOn(cpu, dispatchWQ,
                              &dispatchWorks[cpu],
                              dispatchInterval);
      if (rc != 0) {
         CommOS_StopIO();
         return -1;
      }
   }
   CommOS_Log(("%s: Created I/O task(s) successfully.\n", __FUNCTION__));
   return 0;
}



void
CommOS_StopIO(void)
{
   int cpu;

   if (running) {
      running = 0;
      if (aioWQ) {
         FlushWorkqueue(aioWQ);
         DestroyWorkqueue(aioWQ);
         aioWQ = NULL;
      }
      FlushWorkqueue(dispatchWQ);
      for (cpu = 0; cpu < numCpus; cpu++) {
         WaitForDelayedWork(&dispatchWorksNow[cpu]);
         WaitForDelayedWork(&dispatchWorks[cpu]);
      }
      DestroyWorkqueue(dispatchWQ);
      dispatchWQ = NULL;
      CommOS_Log(("%s: I/O tasks stopped.\n", __FUNCTION__));
   }
}

