/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "uv.h"
#include "internal.h"
#include "logical-callback-node.h"
#include "unified_callback.h"
#include "list.h"
#include "scheduler.h"

#define UV_LOOP_WATCHER_DEFINE(_name, _type)                                  \
  int uv_##_name##_init(uv_loop_t* loop, uv_##_name##_t* handle) {            \
    uv__handle_init(loop, (uv_handle_t*)handle, UV_##_type);                  \
    handle->_name##_cb = NULL;                                                \
    return 0;                                                                 \
  }                                                                           \
                                                                              \
  int uv_##_name##_start(uv_##_name##_t* handle, uv_##_name##_cb cb) {        \
    if (uv__is_active(handle)) return 0;                                      \
    if (cb == NULL) return -EINVAL;                                           \
    uv__register_callback(handle, (any_func) cb, UV_##_type##_CB);            \
    /* JD: FIFO order. Was LIFO order. No docs requiring this, and FIFO needed to make setImmediate-as-check work. I think LIFO order was so that uv__run_X could start at front and iterate without an infinite loop? */ \
    QUEUE_INSERT_TAIL(&handle->loop->_name##_handles, &handle->queue);        \
    handle->_name##_cb = cb;                                                  \
    uv__handle_start(handle);                                                 \
    handle->self_parent = 0;                                                  \
    return 0;                                                                 \
  }                                                                           \
                                                                              \
  int uv_##_name##_stop(uv_##_name##_t* handle) {                             \
    if (!uv__is_active(handle)) return 0;                                     \
    QUEUE_REMOVE(&handle->queue);                                             \
    handle->self_parent = 0;                                                  \
    uv__handle_stop(handle);                                                  \
    return 0;                                                                 \
  }                                                                           \
                                                                              \
/* Returns a list of sched_context_t's describing the ready timers.           \
   Callers are responsible for cleaning up the list, perhaps like this:       \
     list_destroy_full(ready_handles, sched_context_destroy_func, NULL) */    \
  static struct list * uv__ready_##_name##s(uv_loop_t *loop, enum execution_context exec_context) { \
    uv_##_name##_t* handle = NULL;                                            \
    struct list *ready_handles = NULL;                                        \
    sched_context_t *sched_context = NULL;                                    \
    QUEUE* q = NULL;                                                          \
                                                                              \
    ready_handles = list_create();                                            \
                                                                              \
    /* All registered handles are always ready. */                            \
    QUEUE_FOREACH(q, &loop->_name##_handles) {                                \
      handle = QUEUE_DATA(q, uv_##_name##_t, queue);                          \
      sched_context = sched_context_create(exec_context, CALLBACK_CONTEXT_HANDLE, handle);  \
      list_push_back(ready_handles, &sched_context->elem);                    \
    }                                                                         \
                                                                              \
    return ready_handles;                                                     \
  }                                                                           \
                                                                              \
  struct list * uv__ready_##_name##_lcbns(void *h, enum execution_context exec_context) { \
    uv_##_name##_t *handle;                                                   \
    lcbn_t *lcbn;                                                             \
    struct list *ready_lcbns;                                                 \
                                                                              \
    handle = (uv_##_name##_t *) h;                                            \
    assert(handle);                                                           \
    assert(handle->type == UV_##_type);                                       \
                                                                              \
    ready_lcbns = list_create();                                              \
    switch (exec_context)                                                     \
    {                                                                         \
      case EXEC_CONTEXT_UV__RUN_##_type:                                      \
        lcbn = lcbn_get(handle->cb_type_to_lcbn, UV_##_type##_CB);            \
        assert(lcbn && lcbn->cb == (any_func) handle->_name##_cb);            \
        assert(lcbn->cb);                                                     \
        list_push_back(ready_lcbns, &sched_lcbn_create(lcbn)->elem);          \
        break;                                                                \
      case EXEC_CONTEXT_UV__RUN_CLOSING_HANDLES:                              \
        lcbn = lcbn_get(handle->cb_type_to_lcbn, UV_CLOSE_CB);                \
        assert(lcbn && lcbn->cb == (any_func) handle->close_cb);              \
        if (lcbn->cb)                                                         \
          list_push_back(ready_lcbns, &sched_lcbn_create(lcbn)->elem);        \
        break;                                                                \
      default:                                                                \
        assert(!"uv__ready_##_name##_lcbns: Error, unexpected context");      \
    }                                                                         \
                                                                              \
    return ready_lcbns;                                                       \
  }                                                                           \
                                                                              \
  void uv__run_##_name(uv_loop_t* loop) {                                     \
    struct list *ready_handles = NULL;                                        \
    sched_context_t *next_sched_context = NULL;                               \
    sched_lcbn_t *next_sched_lcbn = NULL;                                     \
    uv_##_name##_t *next_handle = NULL;                                       \
                                                                              \
    ENTRY_EXIT_LOG((LOG_MAIN, 9, "uv__run_" #_name ": begin: loop %p\n", loop)); \
                                                                              \
    if (QUEUE_EMPTY(&loop->_name##_handles))                                  \
      goto DONE;                                                              \
                                                                              \
    ready_handles = uv__ready_##_name##s(loop, EXEC_CONTEXT_UV__RUN_##_type); \
    while (ready_handles)                                                     \
    {                                                                         \
      next_sched_context = scheduler_next_context(ready_handles);             \
      if (list_empty(ready_handles) || !next_sched_context)                   \
        break;                                                                \
                                                                              \
      /* Run the next handle. */                                              \
      next_sched_lcbn = scheduler_next_lcbn(next_sched_context);              \
      next_handle = (uv_##_name##_t *) next_sched_context->wrapper;           \
                                                                              \
      assert(next_sched_lcbn->lcbn == lcbn_get(next_handle->cb_type_to_lcbn, UV_##_type##_CB)); \
                                                                              \
      invoke_callback_wrap((any_func) next_handle->_name##_cb, UV_##_type##_CB, (long) next_handle); \
                                                                              \
      /* Each handle is a candidate once per loop iter. */                    \
      list_remove(ready_handles, &next_sched_context->elem);                  \
      sched_context_destroy(next_sched_context);                              \
    }                                                                         \
                                                                              \
    if (ready_handles)                                                        \
      list_destroy_full(ready_handles, sched_context_list_destroy_func, NULL); \
                                                                              \
    DONE:                                                                     \
      ENTRY_EXIT_LOG((LOG_MAIN, 9, "uv__run_" #_name ": returning\n"));       \
  }                                                                           \
                                                                              \
  void uv__##_name##_close(uv_##_name##_t* handle) {                          \
    uv_##_name##_stop(handle);                                                \
  }

UV_LOOP_WATCHER_DEFINE(prepare, PREPARE)
UV_LOOP_WATCHER_DEFINE(check, CHECK)
UV_LOOP_WATCHER_DEFINE(idle, IDLE)
