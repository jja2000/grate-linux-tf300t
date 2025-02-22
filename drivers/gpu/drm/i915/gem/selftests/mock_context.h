/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2016 Intel Corporation
 */

#ifndef __MOCK_CONTEXT_H
#define __MOCK_CONTEXT_H

struct drm_file;
struct drm_i915_private;

void mock_init_contexts(struct drm_i915_private *i915);

struct i915_gem_context *
mock_context(struct drm_i915_private *i915,
	     const char *name);

void mock_context_close(struct i915_gem_context *ctx);

struct i915_gem_context *
live_context(struct drm_i915_private *i915, struct file *file);

struct i915_gem_context *kernel_context(struct drm_i915_private *i915);
void kernel_context_close(struct i915_gem_context *ctx);

#endif /* !__MOCK_CONTEXT_H */
