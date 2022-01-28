/*
 * virthreadpool.c: a generic thread pool implementation
 *
 * Copyright (C) 2014 Red Hat, Inc.
 * Copyright (C) 2010 Hu Tao
 * Copyright (C) 2010 Daniel P. Berrange
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include "virthreadpool.h"
#include "viralloc.h"
#include "virthread.h"
#include "virerror.h"

#define VIR_FROM_THIS VIR_FROM_NONE

typedef struct _virThreadPoolJob virThreadPoolJob;
typedef virThreadPoolJob *virThreadPoolJobPtr;

struct _virThreadPoolJob {
    virThreadPoolJobPtr prev;
    virThreadPoolJobPtr next;
    unsigned int priority;

    void *data;
};

typedef struct _virThreadPoolJobList virThreadPoolJobList;
typedef virThreadPoolJobList *virThreadPoolJobListPtr;

struct _virThreadPoolJobList {
    virThreadPoolJobPtr head;
    virThreadPoolJobPtr tail;
    virThreadPoolJobPtr firstPrio;
};


struct _virThreadPool {
    /*标记此pool需要整体退出*/
    bool quit;

    /*此线程池下每个线程的工作函数*/
    virThreadPoolJobFunc jobFunc;
    /*线程池名称*/
    const char *jobName;
    /*线程工作函数参数*/
    void *jobOpaque;
    /*用于串连job*/
    virThreadPoolJobList jobList;
    /*在queue中的job总数*/
    size_t jobQueueDepth;

    virMutex mutex;
    /*非prio类型对应的条件变量*/
    virCond cond;
    /*条件变量，用于检测pool退出*/
    virCond quit_cond;

    size_t maxWorkers;
    size_t minWorkers;
    /*记录当前pool中空闲的worker数量*/
    size_t freeWorkers;
    size_t nWorkers;
    virThreadPtr workers;

    size_t maxPrioWorkers;
    size_t nPrioWorkers;
    virThreadPtr prioWorkers;
    virCond prioCond;/*prio类型对应的条件变量*/
};

struct virThreadPoolWorkerData {
    virThreadPoolPtr pool;/*所属线程池*/
    virCondPtr cond;/*条件变量指针*/
    bool priority;/*是否priority*/
};

/* Test whether the worker needs to quit if the current number of workers @count
 * is greater than @limit actually allows.
 */
static inline bool virThreadPoolWorkerQuitHelper(size_t count, size_t limit)
{
    return count > limit;
}

/*线程池worker*/
static void virThreadPoolWorker(void *opaque)
{
    struct virThreadPoolWorkerData *data = opaque;
    /*取此线程worker从属于哪个thread pool*/
    virThreadPoolPtr pool = data->pool;
    virCondPtr cond = data->cond;
    bool priority = data->priority;

    /*如果此线程priority为真，则取相应的Worker*/
    size_t *curWorkers = priority ? &pool->nPrioWorkers : &pool->nWorkers;
    size_t *maxLimit = priority ? &pool->maxPrioWorkers : &pool->maxWorkers;
    virThreadPoolJobPtr job = NULL;

    VIR_FREE(data);

    /*防止同一线程池中其它线程进入*/
    virMutexLock(&pool->mutex);

    while (1) {
        /* In order to support async worker termination, we need ensure that
         * both busy and free workers know if they need to terminated. Thus,
         * busy workers need to check for this fact before they start waiting for
         * another job (and before taking another one from the queue); and
         * free workers need to check for this right after waking up.
         */
        if (virThreadPoolWorkerQuitHelper(*curWorkers, *maxLimit))
            goto out;
        while (!pool->quit &&
               ((!priority && !pool->jobList.head) ||
                (priority && !pool->jobList.firstPrio))) {
            /*pool未要求退出，但队列为空，增加freeworker数量*/
            if (!priority)
                pool->freeWorkers++;

            /*等待job*/
            if (virCondWait(cond, &pool->mutex) < 0) {
                if (!priority)
                    pool->freeWorkers--;
                goto out;
            }
            if (!priority)
                pool->freeWorkers--;

            if (virThreadPoolWorkerQuitHelper(*curWorkers, *maxLimit))
                goto out;
        }

        if (pool->quit)
            /*pool需要退出，跳出*/
            break;

        /*取首个job*/
        if (priority) {
            job = pool->jobList.firstPrio;
        } else {
            job = pool->jobList.head;
        }

        /*选择下一个priority类型的job*/
        if (job == pool->jobList.firstPrio) {
            virThreadPoolJobPtr tmp = job->next;
            while (tmp) {
                if (tmp->priority)
                    /*必须为标记为priority的job*/
                    break;
                tmp = tmp->next;
            }
            pool->jobList.firstPrio = tmp;
        }

        /*将job自jobList中移除*/
        if (job->prev)
            job->prev->next = job->next;
        else
            pool->jobList.head = job->next;
        if (job->next)
            job->next->prev = job->prev;
        else
            pool->jobList.tail = job->prev;

        /*job数减一*/
        pool->jobQueueDepth--;

        virMutexUnlock(&pool->mutex);
        /*执行job回调*/
        (pool->jobFunc)(job->data, pool->jobOpaque);
        VIR_FREE(job);
        virMutexLock(&pool->mutex);
    }

 out:
    if (priority)
        pool->nPrioWorkers--;
    else
        pool->nWorkers--;
    if (pool->nWorkers == 0 && pool->nPrioWorkers == 0)
        /*pool中所有worker均已退出，触发条件变量，知会等待者*/
        virCondSignal(&pool->quit_cond);
    virMutexUnlock(&pool->mutex);
}

static int
virThreadPoolExpand(virThreadPoolPtr pool, size_t gain, bool priority)
{
    virThreadPtr *workers = priority ? &pool->prioWorkers : &pool->workers;
    size_t *curWorkers = priority ? &pool->nPrioWorkers : &pool->nWorkers;
    size_t i = 0;
    struct virThreadPoolWorkerData *data = NULL;

    if (VIR_EXPAND_N(*workers, *curWorkers, gain) < 0)
        return -1;

    /*开启gain个线程*/
    for (i = 0; i < gain; i++) {
        g_autofree char *name = NULL;
        /*为线程申请私有数据*/
        if (VIR_ALLOC(data) < 0)
            goto error;

        data->pool = pool;
        data->cond = priority ? &pool->prioCond : &pool->cond;
        data->priority = priority;

        /*如果线程为priority,则名称添加相应前缀*/
        if (priority)
            name = g_strdup_printf("prio-%s", pool->jobName);
        else
            name = g_strdup(pool->jobName);

        /*创建i号线程*/
        if (virThreadCreateFull(&(*workers)[i],
                                false,
                                virThreadPoolWorker,/*线程工作函数*/
                                name/*线程名称*/,
                                true,
                                data) < 0) {
            VIR_FREE(data);
            virReportSystemError(errno, "%s", _("Failed to create thread"));
            goto error;
        }
    }

    return 0;

 error:
    *curWorkers -= gain - i;
    return -1;
}

/*新建线程池*/
virThreadPoolPtr
virThreadPoolNewFull(size_t minWorkers/*最小的worker数*/,
                     size_t maxWorkers/*最大的worker数*/,
                     size_t prioWorkers,
                     virThreadPoolJobFunc func,/*job工作函数，pool会共用此函数*/
                     const char *name/*线程池名称*/,
                     void *opaque)
{
    virThreadPoolPtr pool;

    if (minWorkers > maxWorkers)
        minWorkers = maxWorkers;

    if (VIR_ALLOC(pool) < 0)
        return NULL;

    pool->jobList.tail = pool->jobList.head = NULL;

    pool->jobFunc = func;
    pool->jobName = name;
    pool->jobOpaque = opaque;

    if (virMutexInit(&pool->mutex) < 0)
        goto error;
    if (virCondInit(&pool->cond) < 0)
        goto error;
    if (virCondInit(&pool->quit_cond) < 0)
        goto error;

    pool->minWorkers = minWorkers;
    pool->maxWorkers = maxWorkers;
    pool->maxPrioWorkers = prioWorkers;

    if (virThreadPoolExpand(pool, minWorkers, false) < 0)
        goto error;

    if (prioWorkers) {
        if (virCondInit(&pool->prioCond) < 0)
            goto error;

        if (virThreadPoolExpand(pool, prioWorkers, true) < 0)
            goto error;
    }

    return pool;

 error:
    virThreadPoolFree(pool);
    return NULL;

}

void virThreadPoolFree(virThreadPoolPtr pool)
{
    virThreadPoolJobPtr job;
    bool priority = false;

    if (!pool)
        return;

    virMutexLock(&pool->mutex);
    /*标记此pool需要退出，此标记被打后，各worker会检查并进行退出*/
    pool->quit = true;
    if (pool->nWorkers > 0)
        virCondBroadcast(&pool->cond);
    if (pool->nPrioWorkers > 0) {
        priority = true;
        virCondBroadcast(&pool->prioCond);
    }

    /*等待pool中所有worker退出*/
    while (pool->nWorkers > 0 || pool->nPrioWorkers > 0)
        ignore_value(virCondWait(&pool->quit_cond, &pool->mutex));

    while ((job = pool->jobList.head)) {
        pool->jobList.head = pool->jobList.head->next;
        VIR_FREE(job);
    }

    VIR_FREE(pool->workers);
    virMutexUnlock(&pool->mutex);
    virMutexDestroy(&pool->mutex);
    virCondDestroy(&pool->quit_cond);
    virCondDestroy(&pool->cond);
    if (priority) {
        VIR_FREE(pool->prioWorkers);
        virCondDestroy(&pool->prioCond);
    }
    VIR_FREE(pool);
}


size_t virThreadPoolGetMinWorkers(virThreadPoolPtr pool)
{
    size_t ret;

    virMutexLock(&pool->mutex);
    ret = pool->minWorkers;
    virMutexUnlock(&pool->mutex);

    return ret;
}

size_t virThreadPoolGetMaxWorkers(virThreadPoolPtr pool)
{
    size_t ret;

    virMutexLock(&pool->mutex);
    ret = pool->maxWorkers;
    virMutexUnlock(&pool->mutex);

    return ret;
}

size_t virThreadPoolGetPriorityWorkers(virThreadPoolPtr pool)
{
    size_t ret;

    virMutexLock(&pool->mutex);
    ret = pool->nPrioWorkers;
    virMutexUnlock(&pool->mutex);

    return ret;
}

size_t virThreadPoolGetCurrentWorkers(virThreadPoolPtr pool)
{
    size_t ret;

    virMutexLock(&pool->mutex);
    ret = pool->nWorkers;
    virMutexUnlock(&pool->mutex);

    return ret;
}

size_t virThreadPoolGetFreeWorkers(virThreadPoolPtr pool)
{
    size_t ret;

    virMutexLock(&pool->mutex);
    ret = pool->freeWorkers;
    virMutexUnlock(&pool->mutex);

    return ret;
}

size_t virThreadPoolGetJobQueueDepth(virThreadPoolPtr pool)
{
    size_t ret;

    virMutexLock(&pool->mutex);
    ret = pool->jobQueueDepth;
    virMutexUnlock(&pool->mutex);

    return ret;
}

/*
 * @priority - job priority
 * Return: 0 on success, -1 otherwise
 */
int virThreadPoolSendJob(virThreadPoolPtr pool,
                         unsigned int priority/*是否优先job*/,
                         void *jobData)
{
    /*添加sendJob*/
    virThreadPoolJobPtr job;

    virMutexLock(&pool->mutex);
    if (pool->quit)
        goto error;

    if (pool->freeWorkers - pool->jobQueueDepth <= 0 &&
        pool->nWorkers < pool->maxWorkers &&
        virThreadPoolExpand(pool, 1, false) < 0)
        goto error;

    if (VIR_ALLOC(job) < 0)
        goto error;

    job->data = jobData;
    job->priority = priority;

    job->prev = pool->jobList.tail;
    if (pool->jobList.tail)
        pool->jobList.tail->next = job;
    pool->jobList.tail = job;

    if (!pool->jobList.head)
        pool->jobList.head = job;

    if (priority && !pool->jobList.firstPrio)
        pool->jobList.firstPrio = job;

    /*pool中job数量增加*/
    pool->jobQueueDepth++;

    /*知会pool中线程，防止有线程在等待*/
    virCondSignal(&pool->cond);
    if (priority)
        virCondSignal(&pool->prioCond);

    virMutexUnlock(&pool->mutex);
    return 0;

 error:
    virMutexUnlock(&pool->mutex);
    return -1;
}

int
virThreadPoolSetParameters(virThreadPoolPtr pool,
                           long long int minWorkers,
                           long long int maxWorkers,
                           long long int prioWorkers)
{
    size_t max;
    size_t min;

    virMutexLock(&pool->mutex);

    max = maxWorkers >= 0 ? maxWorkers : pool->maxWorkers;
    min = minWorkers >= 0 ? minWorkers : pool->minWorkers;
    if (min > max) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("minWorkers cannot be larger than maxWorkers"));
        goto error;
    }

    if ((maxWorkers == 0 && pool->maxWorkers > 0) ||
        (maxWorkers > 0 && pool->maxWorkers == 0)) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("maxWorkers must not be switched from zero to non-zero"
                         " and vice versa"));
        goto error;
    }

    if (minWorkers >= 0) {
        if ((size_t) minWorkers > pool->nWorkers &&
            virThreadPoolExpand(pool, minWorkers - pool->nWorkers,
                                false) < 0)
            goto error;
        pool->minWorkers = minWorkers;
    }

    if (maxWorkers >= 0) {
        pool->maxWorkers = maxWorkers;
        virCondBroadcast(&pool->cond);
    }

    if (prioWorkers >= 0) {
        if (prioWorkers < pool->nPrioWorkers) {
            virCondBroadcast(&pool->prioCond);
        } else if ((size_t) prioWorkers > pool->nPrioWorkers &&
                   virThreadPoolExpand(pool, prioWorkers - pool->nPrioWorkers,
                                       true) < 0) {
            goto error;
        }
        pool->maxPrioWorkers = prioWorkers;
    }

    virMutexUnlock(&pool->mutex);
    return 0;

 error:
    virMutexUnlock(&pool->mutex);
    return -1;
}
