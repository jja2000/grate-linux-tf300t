/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2019 Intel Corporation
 */

#include "i915_drv.h"

#include "intel_engine.h"
#include "intel_engine_heartbeat.h"
#include "intel_engine_pm.h"
#include "intel_engine_pool.h"
#include "intel_gt.h"
#include "intel_gt_pm.h"
#include "intel_rc6.h"
#include "intel_ring.h"

static int __engine_unpark(struct intel_wakeref *wf)
{
	struct intel_engine_cs *engine =
		container_of(wf, typeof(*engine), wakeref);
	void *map;

	GEM_TRACE("%s\n", engine->name);

	intel_gt_pm_get(engine->gt);

	/* Pin the default state for fast resets from atomic context. */
	map = NULL;
	if (engine->default_state)
		map = i915_gem_object_pin_map(engine->default_state,
					      I915_MAP_WB);
	if (!IS_ERR_OR_NULL(map))
		engine->pinned_default_state = map;

	if (engine->unpark)
		engine->unpark(engine);

	intel_engine_unpark_heartbeat(engine);
	return 0;
}

#if IS_ENABLED(CONFIG_LOCKDEP)

static inline unsigned long __timeline_mark_lock(struct intel_context *ce)
{
	unsigned long flags;

	local_irq_save(flags);
	mutex_acquire(&ce->timeline->mutex.dep_map, 2, 0, _THIS_IP_);

	return flags;
}

static inline void __timeline_mark_unlock(struct intel_context *ce,
					  unsigned long flags)
{
	mutex_release(&ce->timeline->mutex.dep_map, _THIS_IP_);
	local_irq_restore(flags);
}

#else

static inline unsigned long __timeline_mark_lock(struct intel_context *ce)
{
	return 0;
}

static inline void __timeline_mark_unlock(struct intel_context *ce,
					  unsigned long flags)
{
}

#endif /* !IS_ENABLED(CONFIG_LOCKDEP) */

static void
__queue_and_release_pm(struct i915_request *rq,
		       struct intel_timeline *tl,
		       struct intel_engine_cs *engine)
{
	struct intel_gt_timelines *timelines = &engine->gt->timelines;

	GEM_TRACE("%s\n", engine->name);

	/*
	 * We have to serialise all potential retirement paths with our
	 * submission, as we don't want to underflow either the
	 * engine->wakeref.counter or our timeline->active_count.
	 *
	 * Equally, we cannot allow a new submission to start until
	 * after we finish queueing, nor could we allow that submitter
	 * to retire us before we are ready!
	 */
	spin_lock(&timelines->lock);

	/* Let intel_gt_retire_requests() retire us (acquired under lock) */
	if (!atomic_fetch_inc(&tl->active_count))
		list_add_tail(&tl->link, &timelines->active_list);

	/* Hand the request over to HW and so engine_retire() */
	__i915_request_queue(rq, NULL);

	/* Let new submissions commence (and maybe retire this timeline) */
	__intel_wakeref_defer_park(&engine->wakeref);

	spin_unlock(&timelines->lock);
}

static bool switch_to_kernel_context(struct intel_engine_cs *engine)
{
	struct intel_context *ce = engine->kernel_context;
	struct i915_request *rq;
	unsigned long flags;
	bool result = true;

	/* Already inside the kernel context, safe to power down. */
	if (engine->wakeref_serial == engine->serial)
		return true;

	/* GPU is pointing to the void, as good as in the kernel context. */
	if (intel_gt_is_wedged(engine->gt))
		return true;

	/*
	 * Note, we do this without taking the timeline->mutex. We cannot
	 * as we may be called while retiring the kernel context and so
	 * already underneath the timeline->mutex. Instead we rely on the
	 * exclusive property of the __engine_park that prevents anyone
	 * else from creating a request on this engine. This also requires
	 * that the ring is empty and we avoid any waits while constructing
	 * the context, as they assume protection by the timeline->mutex.
	 * This should hold true as we can only park the engine after
	 * retiring the last request, thus all rings should be empty and
	 * all timelines idle.
	 *
	 * For unlocking, there are 2 other parties and the GPU who have a
	 * stake here.
	 *
	 * A new gpu user will be waiting on the engine-pm to start their
	 * engine_unpark. New waiters are predicated on engine->wakeref.count
	 * and so intel_wakeref_defer_park() acts like a mutex_unlock of the
	 * engine->wakeref.
	 *
	 * The other party is intel_gt_retire_requests(), which is walking the
	 * list of active timelines looking for completions. Meanwhile as soon
	 * as we call __i915_request_queue(), the GPU may complete our request.
	 * Ergo, if we put ourselves on the timelines.active_list
	 * (se intel_timeline_enter()) before we increment the
	 * engine->wakeref.count, we may see the request completion and retire
	 * it causing an undeflow of the engine->wakeref.
	 */
	flags = __timeline_mark_lock(ce);
	GEM_BUG_ON(atomic_read(&ce->timeline->active_count) < 0);

	rq = __i915_request_create(ce, GFP_NOWAIT);
	if (IS_ERR(rq))
		/* Context switch failed, hope for the best! Maybe reset? */
		goto out_unlock;

	/* Check again on the next retirement. */
	engine->wakeref_serial = engine->serial + 1;
	i915_request_add_active_barriers(rq);

	/* Install ourselves as a preemption barrier */
	rq->sched.attr.priority = I915_PRIORITY_BARRIER;
	__i915_request_commit(rq);

	/* Expose ourselves to the world */
	__queue_and_release_pm(rq, ce->timeline, engine);

	result = false;
out_unlock:
	__timeline_mark_unlock(ce, flags);
	return result;
}

static void call_idle_barriers(struct intel_engine_cs *engine)
{
	struct llist_node *node, *next;

	llist_for_each_safe(node, next, llist_del_all(&engine->barrier_tasks)) {
		struct dma_fence_cb *cb =
			container_of((struct list_head *)node,
				     typeof(*cb), node);

		cb->func(ERR_PTR(-EAGAIN), cb);
	}
}

static int __engine_park(struct intel_wakeref *wf)
{
	struct intel_engine_cs *engine =
		container_of(wf, typeof(*engine), wakeref);

	engine->saturated = 0;

	/*
	 * If one and only one request is completed between pm events,
	 * we know that we are inside the kernel context and it is
	 * safe to power down. (We are paranoid in case that runtime
	 * suspend causes corruption to the active context image, and
	 * want to avoid that impacting userspace.)
	 */
	if (!switch_to_kernel_context(engine))
		return -EBUSY;

	GEM_TRACE("%s\n", engine->name);

	call_idle_barriers(engine); /* cleanup after wedging */

	intel_engine_park_heartbeat(engine);
	intel_engine_disarm_breadcrumbs(engine);
	intel_engine_pool_park(&engine->pool);

	/* Must be reset upon idling, or we may miss the busy wakeup. */
	GEM_BUG_ON(engine->execlists.queue_priority_hint != INT_MIN);

	if (engine->park)
		engine->park(engine);

	if (engine->pinned_default_state) {
		i915_gem_object_unpin_map(engine->default_state);
		engine->pinned_default_state = NULL;
	}

	engine->execlists.no_priolist = false;

	/* While gt calls i915_vma_parked(), we have to break the lock cycle */
	intel_gt_pm_put_async(engine->gt);
	return 0;
}

static const struct intel_wakeref_ops wf_ops = {
	.get = __engine_unpark,
	.put = __engine_park,
};

void intel_engine_init__pm(struct intel_engine_cs *engine)
{
	struct intel_runtime_pm *rpm = engine->uncore->rpm;

	intel_wakeref_init(&engine->wakeref, rpm, &wf_ops);
	intel_engine_init_heartbeat(engine);
}

#if IS_ENABLED(CONFIG_DRM_I915_SELFTEST)
#include "selftest_engine_pm.c"
#endif
