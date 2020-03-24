/*
 * Copyright (C) 2009-2019 Ole André Vadla Ravnås <oleavr@nowsecure.com>
 *
 * Licence: wxWindows Library Licence, Version 3.1
 */

#include "gumstalker.h"

struct _GumStalker
{
  GObject parent;
};

G_DEFINE_TYPE (GumStalker, gum_stalker, G_TYPE_OBJECT)

gboolean
gum_stalker_is_supported (void)
{
  return TRUE;
}

static void
gum_stalker_class_init (GumStalkerClass * klass)
{
}

static void
gum_stalker_init (GumStalker * self)
{
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

void
gum_stalker_follow_me (GumStalker * self,
                       GumStalkerTransformer * transformer,
                       GumEventSink * sink)
{

  GumEventType mask = gum_event_sink_query_mask(sink);
  if (mask & GUM_EXEC)
  {
    g_warning("Exec events unsupported");
  }
  if (mask & GUM_COMPILE)
  {
    g_warning("Compile events unsupported");
  }
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

gboolean
gum_stalker_iterator_next (GumStalkerIterator * self,
                           const cs_insn ** insn)
{
  return FALSE;
}

void
gum_stalker_iterator_keep (GumStalkerIterator * self)
{
}

void
gum_stalker_iterator_put_callout (GumStalkerIterator * self,
                                  GumStalkerCallout callout,
                                  gpointer data,
                                  GDestroyNotify data_destroy)
{
}
