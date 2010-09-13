#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include "parallelizer.h"

static void do_input_source_trap (System *system);
static void do_input_source_untrap (System *system);
static void start_next_task (System *system);
static void check_if_task_done (Task *task);

#define DEFAULT_MAX_UNSTARTED_TASKS     500
#define DEFAULT_MAX_RUNNING_TASKS       32

#if 1
# define DEBUG_ONLY(x)
#else
# define DEBUG_ONLY(x) x
#endif

struct _SystemTrap
{
  System *system;
  SystemTrap *prev, *next;
  SystemTrapFuncs *funcs;
  void *trap_data;
};

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
  system->n_unstarted_tasks = 0;
  system->n_running_tasks = 0;
  system->n_finished_tasks = 0;
  system->trap_list = NULL;
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
do_exec (const char *cmdline)
{
  char *args[4];
  args[0] = "sh";
  args[1] = "-c";
  args[2] = (char *) cmdline;
  args[3] = NULL;
  execv ("/bin/sh", args);
}

static gboolean
handle_stdouterr_readable (Task       *task,
                           int         fd,
                           GByteArray *buffer,
                           gboolean    is_stderr)
{
  char *newline = memchr (buffer->data, '\n', buffer->len);
  gboolean got_eof = FALSE;
  GTimeVal cur_time;
  g_get_current_time (&cur_time);
  if (newline == NULL)
    {
      unsigned old_len = buffer->len;
      ssize_t read_rv;
      g_byte_array_set_size (buffer, old_len + 4096);
      read_rv = read (fd, buffer->data + old_len, buffer->len - old_len);
      if (read_rv < 0)
        {
          g_error ("error reading from process %s file-descriptor: %s",
                   is_stderr ? "stderr" : "stdin", g_strerror (errno));
        }
      else if (read_rv == 0)
        {
          got_eof = TRUE;
          g_byte_array_set_size (buffer, old_len);
        }
      else
        {
          /* invoke traps */
          SystemTrap *trap;
          g_byte_array_set_size (buffer, old_len + read_rv);
          for (trap = task->system->trap_list; trap; trap = trap->next)
            {
              if (trap->funcs->handle_data)
                trap->funcs->handle_data (task, &cur_time,
                                          is_stderr,
                                          buffer->len - old_len,
                                          buffer->data + old_len,
                                          trap->trap_data);
            }
        }
    }

  if (newline == NULL)
    newline = memchr (buffer->data, '\n', buffer->len);

  /* invoke traps */
  while (newline != NULL)
    {
      SystemTrap *trap;
      *newline = 0;

      for (trap = task->system->trap_list; trap; trap = trap->next)
        {
          if (trap->funcs->handle_line)
            trap->funcs->handle_line (task, &cur_time,
                                      is_stderr,
                                      (char*) buffer->data,
                                      trap->trap_data);
        }

      g_byte_array_remove_range (buffer, 0, (newline+1) - (char*)buffer->data);
      newline = memchr (buffer->data, '\n', buffer->len);
    }

  if (got_eof)
    {
      return FALSE;
    }
  return TRUE;
}

static gboolean
handle_stdout_readable (void *data)
{
  Task *task = data;
  if (!handle_stdouterr_readable (task, task->info.running.stdout_fd,
                                  task->info.running.stdout_input_buffer,
                                  FALSE))
    {
      task->info.running.stdout_source = NULL;
      close (task->info.running.stdout_fd);
      task->info.running.stdout_fd = -1;
      check_if_task_done (task);
      return FALSE;
    }
  return TRUE;
}

static gboolean
handle_stderr_readable (void *data)
{
  Task *task = data;
  if (!handle_stdouterr_readable (task, task->info.running.stderr_fd,
                                  task->info.running.stderr_input_buffer,
                                  TRUE))
    {
      task->info.running.stderr_source = NULL;
      close (task->info.running.stderr_fd);
      task->info.running.stderr_fd = -1;
      check_if_task_done (task);
      return FALSE;
    }
  return TRUE;
}
static void check_if_all_done (System *system)
{
  DEBUG_ONLY (g_message ("check_if_all_done: n_running_tasks=%u, n_unstarted_tasks=%u, cur_input_source=%u, n_input_sources=%u", system->n_running_tasks, system->n_unstarted_tasks, system->cur_input_source, system->input_sources->len));
  if (system->n_running_tasks == 0
   && system->n_unstarted_tasks == 0
   && system->cur_input_source >= system->input_sources->len)
    {
      DEBUG_ONLY (g_message ("all done (system trap=%p)", system->trap_list));
      SystemTrap *trap;
      GTimeVal cur_time;
      g_get_current_time (&cur_time);
      for (trap = system->trap_list; trap; trap = trap->next)
        if (trap->funcs->all_done)
          trap->funcs->all_done (system, &cur_time, trap->trap_data);
    }
}

static void
check_if_task_done (Task *task)
{
  g_assert (task->state == TASK_RUNNING);
  if (task->info.running.pid < 0
   && task->info.running.stdout_source == NULL
   && task->info.running.stderr_source == NULL)
    {
      TaskTerminationType type = task->info.running.termination_type;
      int info = task->info.running.termination_info;
      GTimeVal cur_time;

      if (task->info.running.stdin_source)
        g_source_destroy ((GSource *) task->info.running.stdin_source);
      if (task->info.running.stdin_fd >= 0)
        close (task->info.running.stdin_fd);
      g_byte_array_free (task->info.running.stdout_input_buffer, TRUE);
      g_byte_array_free (task->info.running.stderr_input_buffer, TRUE);
      g_byte_array_free (task->info.running.stdin_output_buffer, TRUE);
      
      task->state = TASK_DONE;
      task->info.terminated.termination_type = type;
      task->info.terminated.termination_info = info;
      task->system->n_running_tasks--;
      task->system->n_finished_tasks++;

      g_get_current_time (&cur_time);
      SystemTrap *trap;
      for (trap = task->system->trap_list; trap; trap = trap->next)
        if (trap->funcs->ended)
          trap->funcs->ended (task, &cur_time, type, info, trap->trap_data);

      DEBUG_ONLY (g_message ("n_unstarted,running,finished=%u,%u,%u",
                             task->system->n_unstarted_tasks,
                             task->system->n_running_tasks,
                             task->system->n_finished_tasks));

      check_if_all_done (task->system);
    }
}

static void
handle_child_watch_terminated (GPid     pid,
                               gint     status,
                               gpointer data)
{
  Task *task = data;
  if (status & 0xff)
    {
      /* signal */
      task->info.running.termination_type = TASK_TERMINATION_SIGNAL;
      task->info.running.termination_info = status;
    }
  else
    {
      /* exitted */
      task->info.running.termination_type = TASK_TERMINATION_EXIT;
      task->info.running.termination_info = status >> 8;
    }
  task->info.running.pid = -1;
  check_if_task_done (task);
}

static void
start_next_task (System *system)
{
  unsigned task_index = system->n_finished_tasks + system->n_running_tasks;
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
  task->info.running.stdin_fd = stdin_pipe[1];
  task->info.running.stdin_source = NULL;
  task->info.running.stdout_fd = stdout_pipe[0];
  task->info.running.stdout_source = NULL;
  task->info.running.stderr_fd = stderr_pipe[0];
  task->info.running.stderr_source = NULL;
  task->info.running.stdin_output_buffer = g_byte_array_new ();
  task->info.running.stdout_input_buffer = g_byte_array_new ();
  task->info.running.stderr_input_buffer = g_byte_array_new ();
  task->info.running.stdout_source = g_source_fd_new (task->info.running.stdout_fd, G_IO_IN, handle_stdout_readable, task);
  task->info.running.stderr_source = g_source_fd_new (task->info.running.stderr_fd, G_IO_IN, handle_stderr_readable, task);
  g_child_watch_add (pid, handle_child_watch_terminated, task);
  GTimeVal cur_time;
  g_get_current_time (&cur_time);
  SystemTrap *trap;
  for (trap = task->system->trap_list; trap; trap = trap->next)
    if (trap->funcs->handle_started)
      trap->funcs->handle_started (task, &cur_time, task->str, trap->trap_data);
}

static void
handle_source (Source *source, const char *str, void *trap_data)
{
  System *system = trap_data;
  if (str == NULL)
    {
      /* next source */
      DEBUG_ONLY (g_message ("handle_source: str NULL"));
      do_input_source_untrap (system);
      system->cur_input_source++;
      if (system->cur_input_source < system->input_sources->len)
        do_input_source_trap (system);
      else
        check_if_all_done (system);
    }
  else
    {
      Task *task = g_slice_new (Task);
      task->system = system;
      task->task_index = system->tasks->len;
      task->str = g_strdup (str);
      task->first_message = task->last_message = NULL;
      task->state = TASK_WAITING;

      g_ptr_array_add (system->tasks, task);
      system->n_unstarted_tasks += 1;

      DEBUG_ONLY(
      g_message ("handle_source: command=%s; unstarted: %u/%u; running:%u/%u",
                 str, system->n_unstarted_tasks, system->max_unstarted_tasks,
                 system->n_running_tasks, system->max_running_tasks)
      );

      if (system->n_running_tasks < system->max_running_tasks)
        start_next_task (system);
      else if (system->n_unstarted_tasks >= system->max_unstarted_tasks)
        do_input_source_untrap (system);
    }
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

void source_trap   (Source *source,
                    SourceCommandlineCallback callback,
                    void *trap_data)
{
  g_assert (source->callback == NULL);
  source->callback = callback;
  source->trap_data = trap_data;
  source->trap (source);
}

void source_untrap (Source *source)
{
  g_assert (source->callback != NULL);
  source->callback = NULL;
  source->trap_data = NULL;
  source->untrap (source);
}

typedef struct _SourceFD SourceFD;
struct _SourceFD
{
  Source base;
  int fd;
  GSourceFD *source;
  gboolean is_pollable;
  gboolean should_close;
  GByteArray *buffer;
  char separator_char;
};

static void
maybe_do_input_source_fd_read (SourceFD *sfd,
                               gboolean  at_most_one_read)
{
  while (memchr (sfd->buffer->data, sfd->separator_char, sfd->buffer->len) == NULL
    && sfd->fd >= 0)
    {
      unsigned old_len = sfd->buffer->len;
      ssize_t read_rv;
      g_byte_array_set_size (sfd->buffer, old_len + 4096);
      read_rv = read (sfd->fd, sfd->buffer->data + old_len,
                      sfd->buffer->len - old_len);
      if (read_rv < 0)
        {
          g_error ("error reading from file-descriptor: %s",
                   g_strerror (errno));
        }
      else if (read_rv == 0)
        {
          /* eof */
          DEBUG_ONLY (g_message ("input source: got eof"));
          g_byte_array_set_size (sfd->buffer, old_len);
          if (sfd->buffer->len > 0)
            {
              /* complain about partial line */
              g_warning ("partial line encountered at end of input file");
            }
          if (sfd->should_close)
            close (sfd->fd);
          sfd->fd = -1;
          return;
        }
      else
        {
          g_byte_array_set_size (sfd->buffer, old_len + read_rv);
          if (at_most_one_read)
            return;
        }
    }
}
static void
run_input_source_fd_callbacks (SourceFD *sfd)
{
  while (sfd->base.callback)
    {
      char *start = (char *) sfd->buffer->data;
      char *nl = memchr (start, sfd->separator_char, sfd->buffer->len);
      if (nl == NULL)
        break;
      *nl = 0;
      DEBUG_ONLY (g_message ("calling back %s", (char*)sfd->buffer->data));
      sfd->base.callback (&sfd->base, start, sfd->base.trap_data);
      g_byte_array_remove_range (sfd->buffer, 0, (nl + 1) - start);
    }
}

static gboolean
do_idle_input_source (gpointer data)
{
  SourceFD *sfd = data;
  maybe_do_input_source_fd_read (sfd, FALSE);
  run_input_source_fd_callbacks (sfd);
  if (sfd->fd < 0 && sfd->base.callback)
    sfd->base.callback (&sfd->base, NULL, sfd->base.trap_data);
  return TRUE;
}

static gboolean
handle_source_fd_readable (void *data)
{
  SourceFD *sfd = data;
  maybe_do_input_source_fd_read (sfd, TRUE);
  run_input_source_fd_callbacks (sfd);
  if (sfd->fd < 0 && sfd->base.callback)
    sfd->base.callback (&sfd->base, NULL, sfd->base.trap_data);
  return TRUE;
}

static void 
source_fd_trap (Source *source)
{
  SourceFD *sfd = (SourceFD *) source;
  if (sfd->fd < 0)
    {
      (*source->callback) (source, NULL, source->trap_data);
    }
  else if (sfd->is_pollable)
    sfd->source = g_source_fd_new (sfd->fd, G_IO_IN,
                                   handle_source_fd_readable, sfd);
  else
    {
      g_idle_add (do_idle_input_source, source);
    }
}

static void 
source_fd_untrap (Source *source)
{
  SourceFD *sfd = (SourceFD *) source;
  if (sfd->is_pollable)
    {
      if (sfd->source)
        {
          g_source_destroy ((GSource *) sfd->source);
          sfd->source = NULL;
        }
    }
  else
    g_idle_remove_by_data (source);
}

static void 
source_fd_destroy (Source *source)
{
  SourceFD *sfd = (SourceFD *) source;
  if (sfd->should_close)
    close (sfd->fd);
  g_slice_free (SourceFD, sfd);
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
  source->base.untrap = source_fd_untrap;
  source->base.destroy = source_fd_destroy;
  source->fd = fd;
  source->source = NULL;
  source->is_pollable = get_fd_is_pollable (fd);
  source->should_close = should_close;
  source->base.callback = NULL;
  source->base.trap_data = NULL;
  source->buffer = g_byte_array_new ();
  source->separator_char = '\n';                /* TODO: someday support NUL */
  system_add_input_source (system, &source->base);
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
  system->trap_list = rv;
  return rv;
}
