#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include "parallelizer.h"

static void do_input_source_trap (System *system);
static void do_input_source_untrap (System *system);
static void start_next_task (System *system);

#define DEFAULT_MAX_UNSTARTED_TASKS     500
#define DEFAULT_MAX_RUNNING_TASKS       32

System *
system_new (void)
{
  System *system = g_slice_new (System);
  system->tasks = g_ptr_array_new ();
  system->next_unstarted_task = 0;
  system->input_sources = g_ptr_array_new ();
  system->cur_input_source = 0;
  system->is_input_source_trapped = FALSE;
  system->first_message = system->last_message = NULL;
  system->log_fd = -1;
  system->max_unstarted_tasks = DEFAULT_MAX_UNSTARTED_TASKS;
  system->max_running_tasks = DEFAULT_MAX_RUNNING_TASKS;
  return system;
}

static void
set_close_on_exec (int fd)
{
  fcntl (fd, F_SETFL, fcntl (fd, F_GETFL, 0) | FD_CLOEXEC);
}

static void
do_pipe (int *pipe_fds)
{
retry_pipe:
  if (pipe (pipe_fds) < 0)
    {
      if (errno == EINTR)
        goto retry_pipe;
      g_error ("error creating pipe: %s", g_strerror (errno));
    }
  set_close_on_exec (pipe_fds[0]);
  set_close_on_exec (pipe_fds[1]);
}

static void
start_next_task (System *system)
{
  unsigned task_index = system->n_unstarted_tasks + system->n_running_tasks;
  int stderr_pipe[2], stdout_pipe[2], stdin_pipe[2];
  int pid;
  Task *task = system->tasks->pdata[task_index];
  g_assert (task->state == TASK_WAITING);

  do_pipe (stdin_pipe);
  do_pipe (stdout_pipe);
  do_pipe (stderr_pipe);

retry_fork:
  pid = fork ();
  if (pid < 0)
    {
      if (errno == EINTR)
        goto retry_fork;
      else
        g_error ("error forking");
    }
  else if (pid == 0)
    {
      /* child process */
      dup2 (stdin_pipe[0], STDIN_FILENO);
      dup2 (stdout_pipe[1], STDOUT_FILENO);
      dup2 (stderr_pipe[1], STDERR_FILENO);
      do_exec (task->str);
      _exit (127);
    }

  /* parent process */
  close (stdin_pipe[0]);
  close (stdout_pipe[1]);
  close (stderr_pipe[1]);
  task->state = TASK_RUNNING;
  system->n_unstarted_tasks--;
  system->n_running_tasks++;
  task->info.running.pid = pid;
  task->info.running.stdin_poll.fd = stdin_pipe[1];
  task->info.running.stdin_poll.events = G_IO_OUT;
  task->info.running.has_stdin_poll = FALSE;
  task->info.running.stdout_poll.fd = stdout_pipe[0];
  task->info.running.stdout_poll.events = G_IO_IN;
  task->info.running.stderr_poll.fd = stderr_pipe[0];
  task->info.running.stderr_poll.events = G_IO_IN;
  g_main_context_add_poll (g_main_context_default (), &task->info.running.stdout_poll, 0);
  g_main_context_add_poll (g_main_context_default (), &task->info.running.stderr_poll, 0);
}

static void
handle_source (Source *source, const char *str, void *trap_data)
{
  System *system = trap_data;
  Task *task = g_slice_new (Task);
  task->system = system;
  task->task_index = system->tasks->len;
  task->str = g_strdup (str);
  task->first_message = task->last_message = NULL;
  task->state = TASK_WAITING;

  g_ptr_array_add (system->tasks, task);
  system->n_unstarted_tasks += 1;

  if (system->n_running_tasks < system->max_running_tasks)
    start_next_task (system);
  else if (system->n_unstarted_tasks >= system->max_unstarted_tasks)
    do_input_source_untrap (system);
}

static void
do_input_source_trap (System *system)
{
  Source *source = system->input_sources->pdata[system->cur_input_source];
  g_assert (!system->is_input_source_trapped);
  source_trap (source, handle_source, system);
  system->is_input_source_trapped = TRUE;
}

static void
do_input_source_untrap (System *system)
{
  Source *source = system->input_sources->pdata[system->cur_input_source];
  g_assert (system->is_input_source_trapped);
  source_untrap (source);
  system->is_input_source_trapped = FALSE;
}


void    system_add_input_source        (System *system,
                                        Source *source)
{
  g_ptr_array_add (system->input_sources, source);
  if (system->cur_input_source == system->input_sources->len - 1
   && system->n_unstarted_tasks < system->max_unstarted_tasks)
    {
      /* trap input source */
      do_input_source_trap (system);
    }
}

gboolean system_add_input_script        (System     *system,
                                         const char *filename,
                                         GError    **error)
{
  int fd = open (filename, O_RDONLY);
  if (fd < 0)
    {
      g_set_error (error, PARALLELIZER_ERROR_DOMAIN_QUARK,
                   PARALLELIZER_ERROR_OPEN,
                   "could not open %s: %s", filename, g_strerror (errno));
      return FALSE;
    }
  system_add_input_fd (system, fd, TRUE);
  return TRUE;
}

void    system_add_input_stdin         (System *system)
{
  system_add_input_fd (system, 0, FALSE);
}

typedef struct _SourceFD SourceFD;
struct _SourceFD
{
  Source base;
  GPollFD fd;
  gboolean is_pollable;
  gboolean should_close;
  GByteArray *buffer;
};

static gboolean
do_idle_input_source (gpointer data)
{
  SourceFD *sfd = data;
  while (memchr (sfd->buffer->data, '\n', sfd->buffer->len) == NULL
    && sfd->fd.fd >= 0)
    {
      unsigned old_len = sfd->buffer->len;
      g_byte_array_set_size (sfd->buffer, old_len + 4096);
      read_rv = read (sfd->fd.fd, sfd->buffer->data + old_len,
                      sfd->buffer->len - old_len);
      if (read_rv < 0)
        {
          ...
        }
      else if (read_rv == 0)
        {
          /* eof */
          if (sfd->buffer->len > 0)
            {
              /* complain about partial line */
              ...
            }
          if (sfd->should_close)
            close (sfd->fd.fd);
          sfd->fd.fd = -1;
        }
    }
  while (trapped)
    {
      ....do callbacks...
    }
  return TRUE;
}

static void 
source_fd_trap (Source *source)
{
  SourceFD *sfd = (SourceFD *) source;
  if (sfd->is_pollable)
    g_idle_add (do_idle_input_source, source);
  else
    g_main_context_add_poll (g_main_context_default (), &task->fd, 0);
}

static void 
source_fd_untrap (Source *source)
{
  SourceFD *sfd = (SourceFD *) source;
  if (sfd->is_pollable)
    g_idle_remove_by_data (source);
  else
    g_main_context_remove_poll (g_main_context_default (), &task->fd);
}

static void 
source_fd_destroy (Source *source)
{
  SourceFD *sfd = (SourceFD *) source;
  if (sfd->should_close)
    close (sfd->fd);
  g_slice_free (sfd, SourceFD);
}

static gboolean
get_fd_is_pollable (int fd)
{
  struct stat stat_buf;
  if (fstat (fd, &stat_buf) < 0)
    g_error ("error calling fstat: %s", g_strerror (errno));
  return S_ISFIFO (stat_buf.st_mode)
      || S_ISSOCK (stat_buf.st_mode)
      || S_ISCHR (stat_buf.st_mode)
      || isatty (fd);
}

void    system_add_input_fd            (System *system,
                                        int     fd,
                                        gboolean should_close)
{
  SourceFD *source = g_slice_new (SourceFD);
  source->base.trap = source_fd_trap;
  source->base.destroy = source_fd_destroy;
  source->fd.fd = fd;
  source->fd.events = G_IO_IN;
  source->is_pollable = get_fd_is_pollable (fd);
  source->should_close = should_close;
  source->callback = NULL;
  source->trap_data = NULL;
  source->buffer = g_byte_array_new ();
  system_add_input_source (system, source);
}

void    system_set_max_unstarted_tasks (System *system,
                                        unsigned n)
{
  system->max_unstarted_tasks = n;
  if (n < system->n_unstarted_tasks)
    {
      if (!system->is_input_source_trapped)
        do_input_source_trap (system);
    }
  else
    {
      if (system->is_input_source_trapped)
        do_input_source_untrap (system);
    }
}

void    system_set_max_running_tasks   (System *system,
                                        unsigned n)
{
  system->max_running_tasks = n;
  while (system->n_running_tasks < system->max_running_tasks
      && system->n_unstarted_tasks > 0)
    start_next_task (system);
}

struct _SystemTrap
{
  System *system;
  SystemTrap *prev, *next;
  SystemTrapFuncs *funcs;
  void *trap_data;
};

SystemTrap *system_trap                (System *system,
                                        SystemTrapFuncs *funcs,
					void            *trap_data)
{
  SystemTrap *rv = g_slice_new (SystemTrap);
  rv->system = system;
  rv->prev = NULL;
  rv->next = system->trap_list;
  if (rv->next)
    rv->next->prev = rv;
  rv->funcs = funcs;
  rv->trap_data = trap_data;
  return rv;
}
