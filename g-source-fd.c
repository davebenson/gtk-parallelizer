#include "g-source-fd.h"

#if 1
# define DEBUG_ONLY(x)
#else
# define DEBUG_ONLY(x) x
#endif

struct _GSourceFD
{
  GSource base;
  GPollFD poll_fd;
  gboolean has_poll;
};

static gboolean g_source_fd_prepare  (GSource    *source,
                                      gint       *timeout_)
{
  *timeout_ = -1;
  return FALSE;
}

static gboolean g_source_fd_check    (GSource    *source)
{
  GSourceFD *sfd = (GSourceFD *) source;
  DEBUG_ONLY (g_message ("g_source_fd_check: fd=%u, revents=%u", sfd->poll_fd.fd, sfd->poll_fd.revents));
  return sfd->poll_fd.revents != 0;
}

static gboolean g_source_fd_dispatch (GSource    *source,
                                      GSourceFunc callback,
                                      gpointer    user_data)
{
  GSourceFD *sfd = (GSourceFD *) source;
  if (callback != NULL
   && !callback (user_data))
    {
      g_source_remove_poll (source, &sfd->poll_fd);
      sfd->has_poll = FALSE;
      return FALSE;
    }
  return TRUE;
}

static void g_source_fd_finalize (GSource *source)
{
  GSourceFD *sfd = (GSourceFD *) source;
  if (sfd->has_poll)
    g_source_remove_poll (source, &sfd->poll_fd);
}

static GSourceFuncs source_fd_funcs =
{
  g_source_fd_prepare,
  g_source_fd_check,
  g_source_fd_dispatch,
  g_source_fd_finalize
};

GSourceFD    *g_source_fd_new         (int          fd,
                                       GIOCondition events,
                                       GSourceFunc  func,
                                       void        *data)
{
  GSource *source = g_source_new (&source_fd_funcs, sizeof (GSourceFD));
  GSourceFD *sfd = (GSourceFD *) source;
  sfd->poll_fd.fd = fd;
  sfd->poll_fd.events = events;
  g_source_add_poll (source, &sfd->poll_fd);
  g_source_attach (source, g_main_context_default ());
  g_source_set_callback (source, func, data, NULL);
  sfd->has_poll = TRUE;
  return sfd;
}


GIOCondition  g_source_fd_get_revents (GSourceFD   *source)
{
  return ((GSourceFD *) source)->poll_fd.revents;
}
