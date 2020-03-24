/*
 * Copyright (C) 2009-2019 Ole André Vadla Ravnås <oleavr@nowsecure.com>
 *
 * Licence: wxWindows Library Licence, Version 3.1
 */

#include "gumstalker.h"

#include "gumarmreader.h"
#include "gumarmrelocator.h"
#include "gumarmwriter.h"
#include "gummemory.h"
#include "gummetalhash.h"
#include "gumspinlock.h"
#include "gumtls.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

#define GUM_CODE_SLAB_MAX_SIZE  (4 * 1024 * 1024)
#define GUM_EXEC_BLOCK_MIN_SIZE 1024

#define GUM_STALKER_LOCK(o) g_mutex_lock (&(o)->mutex)
#define GUM_STALKER_UNLOCK(o) g_mutex_unlock (&(o)->mutex)

struct _GumStalker
{
  GObject parent;

  guint page_size;
  guint slab_size;
  guint slab_header_size;
  guint slab_max_blocks;
  gboolean is_rwx_supported;

  GMutex mutex;
  GSList * contexts;
  GumTlsKey exec_ctx;

  GArray * exclusions;
};

G_DEFINE_TYPE (GumStalker, gum_stalker, G_TYPE_OBJECT)
typedef struct _GumExecBlock GumExecBlock;
typedef struct _GumSlab GumSlab;
typedef struct _GumExecCtx GumExecCtx;
typedef struct _GumExecFrame GumExecFrame;
typedef struct _GumGeneratorContext GumGeneratorContext;
typedef struct _GumInstruction GumInstruction;
typedef guint GumVirtualizationRequirements;
typedef struct _GumBranchTarget GumBranchTarget;

struct _GumExecBlock
{
  GumExecCtx * ctx;
  GumSlab * slab;

  guint8 * real_begin;
  guint8 * real_end;

  guint8 * code_begin;
  guint8 * code_end;
};

struct _GumSlab
{
  guint8 * data;
  guint offset;
  guint size;
  GumSlab * next;

  guint num_blocks;
  GumExecBlock blocks[];
};

struct _GumExecFrame
{
  gpointer real_address;
  gpointer code_address;
};

static void gum_stalker_finalize (GObject * object);

struct _GumExecCtx
{
  volatile gint state;
  gint64 destroy_pending_since;

  GumStalker * stalker;
  GumThreadId thread_id;

  GumArmWriter code_writer;
  GumArmRelocator relocator;

  GumStalkerTransformer * transformer;
  void (* transform_block_impl) (GumStalkerTransformer * self,
      GumStalkerIterator * iterator, GumStalkerWriter * output);
  GumEventSink * sink;
  gboolean sink_started;
  GumEventType sink_mask;
  void (* sink_process_impl) (GumEventSink * self, const GumEvent * ev);

  gboolean unfollow_called_while_still_following;
  GumExecBlock * current_block;
  guint pending_calls;
  GumExecFrame * current_frame;
  GumExecFrame * first_frame;
  GumExecFrame * frames;

  gpointer resume_at;

  GumSlab * code_slab;
  GumMetalHashTable * mappings;
};

struct _GumGeneratorContext
{
  GumInstruction * instruction;
  GumArmRelocator * relocator;
  GumArmWriter * code_writer;
  gpointer continuation_real_address;
};

struct _GumInstruction
{
  const cs_insn * ci;
  guint8 * begin;
  guint8 * end;
};


struct _GumStalkerIterator
{
  GumExecCtx * exec_context;
  GumExecBlock * exec_block;
  GumGeneratorContext * generator_context;

  GumInstruction instruction;
  GumVirtualizationRequirements requirements;
};

enum _GumExecCtxState
{
  GUM_EXEC_CTX_ACTIVE,
  GUM_EXEC_CTX_UNFOLLOW_PENDING,
  GUM_EXEC_CTX_DESTROY_PENDING
};

enum _GumVirtualizationRequirements
{
  GUM_REQUIRE_NOTHING          = 0,
  GUM_REQUIRE_RELOCATION       = 1 << 0,
  GUM_REQUIRE_EXCLUSIVE_STORE  = 1 << 1,
};

struct _GumBranchTarget
{
  gpointer origin_ip;

  gpointer absolute_address;
  gssize relative_offset;

  arm64_reg reg;
};

gboolean
gum_stalker_is_supported (void)
{
  return TRUE;
}

static gpointer gum_unfollow_me_address;


static GumExecBlock *
gum_exec_block_obtain (GumExecCtx * ctx,
                       gpointer real_address,
                       gpointer * code_address_ptr)
{
  GumExecBlock * block;

  block = gum_metal_hash_table_lookup (ctx->mappings, real_address);
  if (block != NULL)
    *code_address_ptr = block->code_begin;

  return block;
}

static void
gum_stalker_class_init (GumStalkerClass * klass)
{
  GObjectClass * object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = gum_stalker_finalize;

  gum_unfollow_me_address = gum_strip_code_pointer (gum_stalker_unfollow_me);
}

static void
gum_stalker_finalize (GObject * object)
{
  GumStalker * self = GUM_STALKER (object);

  g_array_free (self->exclusions, TRUE);

  g_assert (self->contexts == NULL);
  gum_tls_key_free (self->exec_ctx);
  g_mutex_clear (&self->mutex);

  G_OBJECT_CLASS (gum_stalker_parent_class)->finalize (object);
}

static void
gum_stalker_init (GumStalker * self)
{
  self->exclusions = g_array_new (FALSE, FALSE, sizeof (GumMemoryRange));

  self->page_size = gum_query_page_size ();
  self->slab_size =
      GUM_ALIGN_SIZE (GUM_CODE_SLAB_MAX_SIZE, self->page_size);
  self->slab_header_size =
      GUM_ALIGN_SIZE (GUM_CODE_SLAB_MAX_SIZE / 12, self->page_size);
  self->slab_max_blocks = (self->slab_header_size -
      G_STRUCT_OFFSET (GumSlab, blocks)) / sizeof (GumExecBlock);
  self->is_rwx_supported = gum_query_rwx_support () != GUM_RWX_NONE;

  g_mutex_init (&self->mutex);
  self->contexts = NULL;
  self->exec_ctx = gum_tls_key_new ();
}

GumStalker *
gum_stalker_new (void)
{
  return g_object_new (GUM_TYPE_STALKER, NULL);
}

void
gum_stalker_exclude (GumStalker * self,
                     const GumMemoryRange * range)
{
}

gint
gum_stalker_get_trust_threshold (GumStalker * self)
{
  return 0;
}

void
gum_stalker_set_trust_threshold (GumStalker * self,
                                 gint trust_threshold)
{
  g_warning("Trust threshold unsupported");
}

void
gum_stalker_flush (GumStalker * self)
{
}

void
gum_stalker_stop (GumStalker * self)
{
}

gboolean
gum_stalker_garbage_collect (GumStalker * self)
{
  return FALSE;
}

static GumSlab *
gum_exec_ctx_add_slab (GumExecCtx * ctx)
{
  GumSlab * slab;
  GumStalker * stalker = ctx->stalker;

  slab = gum_memory_allocate (NULL, stalker->slab_size, stalker->page_size,
      stalker->is_rwx_supported ? GUM_PAGE_RWX : GUM_PAGE_RW);

  slab->data = (guint8 *) slab + stalker->slab_header_size;
  slab->offset = 0;
  slab->size = stalker->slab_size - stalker->slab_header_size;
  slab->next = ctx->code_slab;

  slab->num_blocks = 0;

  ctx->code_slab = slab;

  return slab;
}


static void
gum_stalker_thaw (GumStalker * self,
                  gpointer code,
                  gsize size)
{
  if (!self->is_rwx_supported)
    gum_mprotect (code, size, GUM_PAGE_RW);
}

static void
gum_stalker_freeze (GumStalker * self,
                    gpointer code,
                    gsize size)
{
  if (!self->is_rwx_supported)
    gum_memory_mark_code (code, size);

  gum_clear_cache (code, size);
}

static void
gum_exec_block_commit (GumExecBlock * block)
{
  gsize code_size, real_size;

  code_size = block->code_end - block->code_begin;
  block->slab->offset += code_size;

  real_size = block->real_end - block->real_begin;
  block->slab->offset += real_size;

  gum_stalker_freeze (block->ctx->stalker, block->code_begin, code_size);
}

static GumExecBlock *
gum_exec_block_new (GumExecCtx * ctx)
{
  GumStalker * stalker = ctx->stalker;
  GumSlab * slab = ctx->code_slab;
  gsize available;

  available = (slab != NULL) ? slab->size - slab->offset : 0;
  if (available >= GUM_EXEC_BLOCK_MIN_SIZE &&
      slab->num_blocks != stalker->slab_max_blocks)
  {
    GumExecBlock * block = slab->blocks + slab->num_blocks;

    block->ctx = ctx;
    block->slab = slab;

    block->code_begin = slab->data + slab->offset;
    block->code_end = block->code_begin;

    gum_stalker_thaw (stalker, block->code_begin, available);
    slab->num_blocks++;

    return block;
  }

  gum_exec_ctx_add_slab (ctx);

  return gum_exec_block_new (ctx);
}

static GumExecBlock *
gum_exec_ctx_obtain_block_for (GumExecCtx * ctx,
                               gpointer real_address,
                               gpointer * code_address_ptr)
{
  GumExecBlock * block;
  GumArmWriter * cw;
  GumArmRelocator * rl;
  GumGeneratorContext gc;
  GumStalkerIterator iterator;
  gboolean all_labels_resolved;


  block = gum_exec_block_obtain (ctx, real_address, code_address_ptr);
  if (block != NULL)
  {
    return block;
  }

  block = gum_exec_block_new (ctx);
  block->real_begin = real_address;
  *code_address_ptr = block->code_begin;

  gum_metal_hash_table_insert (ctx->mappings, real_address, block);

  cw = &ctx->code_writer;
  rl = &ctx->relocator;

  gum_arm_writer_reset (cw, block->code_begin);
  gum_arm_relocator_reset (rl, real_address, cw);

  gum_ensure_code_readable (real_address, ctx->stalker->page_size);

  gc.instruction = NULL;
  gc.relocator = rl;
  gc.code_writer = cw;
  gc.continuation_real_address = NULL;

  iterator.exec_context = ctx;
  iterator.exec_block = block;
  iterator.generator_context = &gc;

  iterator.instruction.ci = NULL;
  iterator.instruction.begin = NULL;
  iterator.instruction.end = NULL;
  iterator.requirements = GUM_REQUIRE_NOTHING;

  ctx->pending_calls++;

  ctx->transform_block_impl (ctx->transformer, &iterator,
      (GumStalkerWriter *) cw);

  ctx->pending_calls--;

  if (gc.continuation_real_address != NULL)
  {
    // GumBranchTarget continue_target = { 0, };

    // continue_target.absolute_address = gc.continuation_real_address;
    // continue_target.reg = ARM64_REG_INVALID;
    g_error("Need to implement this!!!");
  }

  gum_arm_writer_put_brk_imm (cw, 14);

  all_labels_resolved = gum_arm_writer_flush (cw);
  if (!all_labels_resolved)
    g_error ("Failed to resolve labels");

  block->code_end = (guint8 *) gum_arm_writer_cur (cw);
  block->real_end = (guint8 *) rl->input_cur;

  gum_exec_block_commit (block);

  return block;
}

static void
gum_exec_ctx_unfollow (GumExecCtx * ctx,
                       gpointer resume_at)
{
  ctx->current_block = NULL;

  ctx->resume_at = resume_at;

  gum_tls_key_set_value (ctx->stalker->exec_ctx, NULL);

  ctx->destroy_pending_since = g_get_monotonic_time ();
  g_atomic_int_set (&ctx->state, GUM_EXEC_CTX_DESTROY_PENDING);
}

static gboolean
gum_exec_ctx_maybe_unfollow (GumExecCtx * ctx,
                             gpointer resume_at)
{
  if (g_atomic_int_get (&ctx->state) != GUM_EXEC_CTX_UNFOLLOW_PENDING)
    return FALSE;

  if (ctx->pending_calls > 0)
    return FALSE;

  gum_exec_ctx_unfollow (ctx, resume_at);

  return TRUE;
}

static GumExecCtx *
gum_stalker_create_exec_ctx (GumStalker * self,
                             GumThreadId thread_id,
                             GumStalkerTransformer * transformer,
                             GumEventSink * sink)
{
  GumExecCtx * ctx;

  ctx = g_slice_new0 (GumExecCtx);

  ctx->state = GUM_EXEC_CTX_ACTIVE;

  ctx->stalker = g_object_ref (self);
  ctx->thread_id = thread_id;

  gum_arm_writer_init (&ctx->code_writer, NULL);
  gum_arm_relocator_init (&ctx->relocator, NULL, &ctx->code_writer);

  if (transformer != NULL)
    ctx->transformer = g_object_ref (transformer);
  else
    ctx->transformer = gum_stalker_transformer_make_default ();
  ctx->transform_block_impl =
      GUM_STALKER_TRANSFORMER_GET_IFACE (ctx->transformer)->transform_block;

  ctx->sink = g_object_ref (sink);
  ctx->sink_mask = gum_event_sink_query_mask (sink);
  ctx->sink_process_impl = GUM_EVENT_SINK_GET_IFACE (sink)->process;

  ctx->frames =
      gum_memory_allocate (NULL, self->page_size, self->page_size, GUM_PAGE_RW);
  ctx->first_frame = (GumExecFrame *) ((guint8 *) ctx->frames +
      self->page_size - sizeof (GumExecFrame));
  ctx->current_frame = ctx->first_frame;

  ctx->mappings = gum_metal_hash_table_new (NULL, NULL);

  GUM_STALKER_LOCK (self);
  self->contexts = g_slist_prepend (self->contexts, ctx);
  GUM_STALKER_UNLOCK (self);

  gum_exec_ctx_add_slab (ctx);
  return ctx;
}


static void
gum_exec_ctx_free (GumExecCtx * ctx)
{
  GumStalker * stalker = ctx->stalker;
  GumSlab * slab;

  gum_metal_hash_table_unref (ctx->mappings);

  slab = ctx->code_slab;
  while (slab != NULL)
  {
    GumSlab * next = slab->next;
    gum_memory_free (slab, stalker->slab_size);
    slab = next;
  }

  gum_memory_free (ctx->frames, stalker->page_size);

  g_object_unref (ctx->sink);
  g_object_unref (ctx->transformer);

  gum_arm_relocator_clear (&ctx->relocator);
  gum_arm_writer_clear (&ctx->code_writer);

  g_object_unref (stalker);

  g_slice_free (GumExecCtx, ctx);
}

static void
gum_stalker_destroy_exec_ctx (GumStalker * self,
                              GumExecCtx * ctx)
{
  GSList * entry;

  GUM_STALKER_LOCK (self);
  entry = g_slist_find (self->contexts, ctx);
  if (entry != NULL)
    self->contexts = g_slist_delete_link (self->contexts, entry);
  GUM_STALKER_UNLOCK (self);

  /* Racy due to garbage-collection. */
  if (entry == NULL)
    return;

  if (ctx->sink_started)
  {
    gum_event_sink_stop (ctx->sink);

    ctx->sink_started = FALSE;
  }

  gum_exec_ctx_free (ctx);
}


gpointer
_gum_stalker_do_follow_me (GumStalker * self,
                           GumStalkerTransformer * transformer,
                           GumEventSink * sink,
                           gpointer ret_addr)
{

  GumEventType mask = gum_event_sink_query_mask(sink);
  if (mask & GUM_COMPILE)
  {
    g_warning("Compile events unsupported");
  }

  GumExecCtx * ctx;
  gpointer code_address;

  ctx = gum_stalker_create_exec_ctx (self, gum_process_get_current_thread_id (),
      transformer, sink);
  gum_tls_key_set_value (self->exec_ctx, ctx);

  ctx->current_block = gum_exec_ctx_obtain_block_for (ctx, ret_addr,
      &code_address);

  if (gum_exec_ctx_maybe_unfollow (ctx, ret_addr))
  {
    gum_stalker_destroy_exec_ctx (self, ctx);
    return ret_addr;
  }

  gum_event_sink_start (sink);
  ctx->sink_started = TRUE;

  return code_address;
}


void
gum_stalker_follow_me (GumStalker * self,
                       GumStalkerTransformer * transformer,
                       GumEventSink * sink)
{
  _gum_stalker_do_follow_me(self, transformer, sink, NULL);
}

void
gum_stalker_unfollow_me (GumStalker * self)
{
}

gboolean
gum_stalker_is_following_me (GumStalker * self)
{
  return FALSE;
}

void
gum_stalker_follow (GumStalker * self,
                    GumThreadId thread_id,
                    GumStalkerTransformer * transformer,
                    GumEventSink * sink)
{
  g_warning("Follow unsupported");
}

void
gum_stalker_unfollow (GumStalker * self,
                      GumThreadId thread_id)
{
  g_warning("Unfollow unsupported");
}

void
gum_stalker_activate (GumStalker * self,
                      gconstpointer target)
{
  g_warning("Activate/deactivate unsupported");
}

void
gum_stalker_deactivate (GumStalker * self)
{
  g_warning("Activate/deactivate unsupported");
}

GumProbeId
gum_stalker_add_call_probe (GumStalker * self,
                            gpointer target_address,
                            GumCallProbeCallback callback,
                            gpointer data,
                            GDestroyNotify notify)
{
  g_warning("Call probes unsupported");
  return 0;
}

void
gum_stalker_remove_call_probe (GumStalker * self,
                               GumProbeId id)
{
  g_warning("Call probes unsupported");
}

static void
gum_exec_ctx_emit_exec_event (GumExecCtx * ctx,
                              gpointer location)
{
  GumEvent ev;
  GumExecEvent * exec = &ev.exec;

  ev.type = GUM_EXEC;

  exec->location = location;

  ctx->sink_process_impl (ctx->sink, &ev);
}

static void
gum_exec_ctx_emit_block_event (GumExecCtx * ctx,
                               gpointer begin,
                               gpointer end)
{
  GumEvent ev;
  GumBlockEvent * block = &ev.block;

  ev.type = GUM_BLOCK;

  block->begin = begin;
  block->end = end;

  ctx->sink_process_impl (ctx->sink, &ev);
}

static void
gum_exec_ctx_write_prolog (GumExecCtx * ctx,
                           GumArmWriter * cw)
{

}

static void
gum_exec_ctx_write_epilog (GumExecCtx * ctx,
                           GumArmWriter * cw)
{
}

static void
gum_exec_block_open_prolog (GumExecBlock * block,
                            GumGeneratorContext * gc)
{
  gum_exec_ctx_write_prolog (block->ctx, gc->code_writer);
}

static void
gum_exec_block_close_prolog (GumExecBlock * block,
                             GumGeneratorContext * gc)
{
  gum_exec_ctx_write_epilog (block->ctx, gc->code_writer);
}

static void
gum_exec_block_write_exec_event_code (GumExecBlock * block,
                                      GumGeneratorContext * gc)
{
  gum_exec_block_open_prolog (block, gc);

  gum_arm_writer_put_call_address_with_arguments (gc->code_writer,
      GUM_ADDRESS (gum_exec_ctx_emit_exec_event), 2,
      GUM_ARG_ADDRESS, GUM_ADDRESS (block->ctx),
      GUM_ARG_ADDRESS, GUM_ADDRESS (gc->instruction->begin));

  gum_exec_block_close_prolog(block, gc);
}

static void
gum_exec_block_write_block_event_code (GumExecBlock * block,
                                       GumGeneratorContext * gc)
{
  gum_exec_block_open_prolog (block, gc);

  gum_arm_writer_put_call_address_with_arguments (gc->code_writer,
      GUM_ADDRESS (gum_exec_ctx_emit_block_event), 3,
      GUM_ARG_ADDRESS, GUM_ADDRESS (block->ctx),
      GUM_ARG_ADDRESS, GUM_ADDRESS (gc->relocator->input_start),
      GUM_ARG_ADDRESS, GUM_ADDRESS (gc->relocator->input_cur));

  gum_exec_block_close_prolog(block, gc);
}

static gboolean
gum_exec_block_is_full (GumExecBlock * block)
{
  guint8 * slab_end = block->slab->data + block->slab->size;

  return slab_end - block->code_end < GUM_EXEC_BLOCK_MIN_SIZE;
}

gboolean
gum_stalker_iterator_next (GumStalkerIterator * self,
                           const cs_insn ** insn)
{
  GumGeneratorContext * gc = self->generator_context;
  GumArmRelocator * rl = gc->relocator;
  GumInstruction * instruction;
  guint n_read;

  instruction = self->generator_context->instruction;
  if (instruction != NULL)
  {
    GumExecBlock * block = self->exec_block;
    gboolean skip_implicitly_requested;

    skip_implicitly_requested = rl->outpos != rl->inpos;
    if (skip_implicitly_requested)
    {
      gum_arm_relocator_skip_one (rl);
    }

    block->code_end = gum_arm_writer_cur (gc->code_writer);

    if (gum_exec_block_is_full (block))
    {
      gc->continuation_real_address = instruction->end;
      return FALSE;
    }
  }

  instruction = &self->instruction;

  n_read = gum_arm_relocator_read_one (rl, &instruction->ci);
  if (n_read == 0)
    return FALSE;

  instruction->begin = GSIZE_TO_POINTER (instruction->ci->address);
  instruction->end = instruction->begin + instruction->ci->size;

  self->generator_context->instruction = instruction;

  if (insn != NULL)
    *insn = instruction->ci;

  return TRUE;
}

void
gum_stalker_iterator_keep (GumStalkerIterator * self)
{
  GumExecCtx * ec = self->exec_context;
  GumExecBlock * block = self->exec_block;
  GumGeneratorContext * gc = self->generator_context;
  GumArmRelocator * rl = gc->relocator;
  const cs_insn * insn = gc->instruction->ci;
  GumVirtualizationRequirements requirements;

  requirements = GUM_REQUIRE_NOTHING;

  if ((ec->sink_mask & GUM_EXEC) != 0)
  {
    gum_exec_block_write_exec_event_code (block, gc);
  }

  if ((ec->sink_mask & GUM_BLOCK) != 0 &&
      gum_arm_relocator_eob (rl))
  {
    switch (insn->id)
    {
      // TODO: All the call instructions here
      case ARM_INS_BL:
        break;
      default:
        gum_exec_block_write_block_event_code (block, gc);
        break;
    }
  }

  switch (insn->id)
  {
    // case ARM_INS_B:
    //   requirements = gum_exec_block_virtualize_sysenter_insn (block, gc);
    //   break;
    case ARM_INS_SMC:
    case ARM_INS_HVC:
      g_assert ("" == "not implemented");
      break;
    default:
      requirements = GUM_REQUIRE_RELOCATION;
  }

  // gum_exec_block_close_prolog (block, gc);

  if ((requirements & GUM_REQUIRE_RELOCATION) != 0)
    gum_arm_relocator_write_one (rl);

  self->requirements = requirements;
}

void
gum_stalker_iterator_put_callout (GumStalkerIterator * self,
                                  GumStalkerCallout callout,
                                  gpointer data,
                                  GDestroyNotify data_destroy)
{
}
