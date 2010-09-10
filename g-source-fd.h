
#include <glib.h>

typedef struct _GSourceFD GSourceFD;

GSourceFD    *g_source_fd_new         (int          fd,
                                       GIOCondition events,
                                       GSourceFunc  func,
                                       void        *data);
GIOCondition  g_source_fd_get_revents (GSourceFD   *source);
