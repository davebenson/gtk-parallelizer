#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include "parallelizer.h"

#define WINDOW_NAME                     "window1"

static GPtrArray *cmdline_inputs = NULL;
static int cmdline_max_parallel = -1;

  static System *the_system;

static gboolean
handle_input (const gchar    *option_name,
              const gchar    *value,
              gpointer        data,
              GError        **error)
{
  (void) option_name;
  (void) data;
  (void) error;
  g_ptr_array_add (cmdline_inputs, g_strdup (value));
  return TRUE;
}

static gulong last_time_secs = 0;
static char   last_time_str[64];

static void
maybe_uptime_last_time_secs (gulong tim)
{
  if (last_time_secs != tim)
    {
      struct tm tm;
      time_t t = tim;
      last_time_secs = tim;
      tm = *localtime(&t);
      strftime (last_time_str, sizeof (last_time_str),
                "%Y-%m-%d %H:%M:%S",
                &tm);
    }
}
static void
syshandler__handle_task_started (Task *task,
                         const GTimeVal *current_time,
                         const char *cmdline,
                         void *handler_data)
{
  maybe_uptime_last_time_secs (current_time->tv_sec);
  fprintf (stderr,
           "%s.%03u [%6u] started: %s\n",
           last_time_str,
           current_time->tv_usec/1000,
           task->task_index,
           cmdline);
}

static void
syshandler__handle_data (Task *task,
                         const GTimeVal *current_time,
                         gboolean is_stderr, /* else is stdout */
                         unsigned len,
                         const guint8 *data,
                         gpointer handler_data)
{
}
static void
syshandler__handle_line (Task *task,
                         const GTimeVal *current_time,
                         gboolean is_stderr, /* else is stdout */
                         const char *text,
                         gpointer handler_data)
{
  maybe_uptime_last_time_secs (current_time->tv_sec);
  fprintf (is_stderr ? stderr : stdout,
           "%s.%03u [%6u]%c %s\n",
           last_time_str,
           current_time->tv_usec/1000,
           task->task_index,
           is_stderr ? '!' : ':',
           text);
}

static void
syshandler__ended       (Task *task,
                         const GTimeVal *current_time,
                         TaskTerminationType termination_type,
                         int termination_info,
                         gpointer handler_data)
{
  //g_message ("task %u ended [type=%u, info=%u]", task->task_index,termination_type,termination_info);
  maybe_uptime_last_time_secs (current_time->tv_sec);
  switch (termination_type)
    {
    case TASK_TERMINATION_EXIT:
      if (termination_info == 0)
        fprintf (stderr, "%s.%03u: Task %u exitted with status 0: success.\n",
                 last_time_str, current_time->tv_usec/1000, task->task_index);
      else
        fprintf (stderr, "%s.%03u! Task %u exitted with status %d!\n",
                 last_time_str, current_time->tv_usec/1000, task->task_index,
                 termination_info);
      break;
    case TASK_TERMINATION_SIGNAL:
      fprintf (stderr, "%s.%03u! Task %u killed by signal %u (%s)!\n",
               last_time_str, current_time->tv_usec/1000, task->task_index,
               termination_info, g_strsignal (termination_info));
      break;
    }
}

static void
syshandler__all_done    (System *system,
                         const GTimeVal *current_time,
                         gpointer handler_data)
{
  /* fsync() log file??? */

  exit (0);
}

static GPtrArray *chunked_per_process_data;
static unsigned chunked_next_to_end = 0;
static gboolean chunked_failed = FALSE;
static void
chunked__init (void)
{
  chunked_per_process_data = g_ptr_array_new ();
}

static void
chunked__handle_task_started (Task *task,
                         const GTimeVal *current_time,
                         const char *cmdline,
                         void *handler_data)
{
  g_ptr_array_add (chunked_per_process_data, g_byte_array_new ());
}
static void
chunked__handle_data (Task *task,
                         const GTimeVal *current_time,
                         gboolean is_stderr, /* else is stdout */
                         unsigned len,
                         const guint8 *data,
                         gpointer handler_data)
{
  if (is_stderr)
    return;
  if (task->task_index == chunked_next_to_end)
    {
      if (fwrite (data, len, 1, stdout) != 1)
        g_error ("error writing to standard-output");
    }
  else
    {
      g_byte_array_append (chunked_per_process_data->pdata[task->task_index],
                           data, len);
    }
}
static void
chunked__handle_line (Task *task,
                         const GTimeVal *current_time,
                         gboolean is_stderr, /* else is stdout */
                         const char *text,
                         gpointer handler_data)
{
  if (!is_stderr)
    return;
  syshandler__handle_line (task, current_time, TRUE, text, NULL);
}

static void
chunked__ended       (Task *task,
                         const GTimeVal *current_time,
                         TaskTerminationType termination_type,
                         int termination_info,
                         gpointer handler_data)
{
  if (termination_type != TASK_TERMINATION_EXIT
  ||  termination_info != 0)
    chunked_failed = TRUE;
  maybe_uptime_last_time_secs (current_time->tv_sec);

  switch (termination_type)
    {
    case TASK_TERMINATION_EXIT:
      if (termination_info != 0)
        fprintf (stderr, "%s.%03u! Task %u exitted with status %d!\n",
                 last_time_str, current_time->tv_usec/1000, task->task_index,
                 termination_info);
      break;
    case TASK_TERMINATION_SIGNAL:
      fprintf (stderr, "%s.%03u! Task %u killed by signal %u (%s)!\n",
               last_time_str, current_time->tv_usec/1000, task->task_index,
               termination_info, g_strsignal (termination_info));
      break;
    }

  if (task->task_index == chunked_next_to_end)
    {
      chunked_next_to_end++;
      while (chunked_next_to_end < chunked_per_process_data->len)
        {
          Task *task = the_system->tasks->pdata[chunked_next_to_end];

          /* dump any output from task */
          GByteArray *o = chunked_per_process_data->pdata[chunked_next_to_end];
          if (o->len > 0 && fwrite (o->data, o->len, 1, stdout) != 1)
            g_error ("error writing to standard-output");

          if (task->state != TASK_DONE)
            break;

          g_byte_array_free (chunked_per_process_data->pdata[chunked_next_to_end], TRUE);
          chunked_per_process_data->pdata[chunked_next_to_end] = NULL;
          chunked_next_to_end++;
        }
    }
}

static void
chunked__all_done    (System *system,
                         const GTimeVal *current_time,
                         gpointer handler_data)
{
  /* fsync() log file??? */

  exit (chunked_failed ? 1 : 0);
}


static struct {
  const char *mode;
  const char *mode_desc_short;
  void (*init) (void);
  SystemTrapFuncs funcs;
} modes[] = {
  {
    /* NOTE: the default mode must always be first!! */
    "default",
    "display line-by-line stdout and stderr with timestamps and other info",
    NULL,
    {
      syshandler__handle_task_started,
      syshandler__handle_data,
      syshandler__handle_line,
      syshandler__ended,
      syshandler__all_done
    }
  },
  {
    "chunked",
    "group each processes outputs together",
    chunked__init,
    {
      chunked__handle_task_started,
      chunked__handle_data,
      chunked__handle_line,
      chunked__ended,
      chunked__all_done
    }
  },
};

static SystemTrapFuncs *trap_funcs = &modes[0].funcs;

static gboolean
handle_mode  (const gchar    *option_name,
              const gchar    *value,
              gpointer        data,
              GError        **error)
{
  unsigned i;
  for (i = 0; i < G_N_ELEMENTS (modes); i++)
    if (strcmp (modes[i].mode, value) == 0)
      break;
  if (i == G_N_ELEMENTS (modes))
    {
      g_set_error (error, PARALLELIZER_ERROR_DOMAIN_QUARK,
                   PARALLELIZER_ERROR_CMDLINE_ARG,
                   "bad mode %s: try --list-modes",
                   value);
      return FALSE;
    }
  trap_funcs = &modes[i].funcs;
  if (modes[i].init)
    modes[i].init ();
  return TRUE;
}

static gboolean
handle_list_modes  (const gchar    *option_name,
                    const gchar    *value,
                    gpointer        data,
                    GError        **error)
{
  fprintf (stderr, "modes:\n");
  unsigned i;
  (void) option_name;
  (void) data;
  (void) error;
  for (i = 0; i < G_N_ELEMENTS (modes); i++)
    fprintf (stderr, "  --mode=%s\n"
                     "      %s\n\n",
                     modes[i].mode,
                     modes[i].mode_desc_short);
  exit (1);
  return FALSE;
}


static GOptionEntry op_entries[] =
{
  {"input", 'i', 0, G_OPTION_ARG_CALLBACK, handle_input, "script to run", "FILENAME"},
  {"max-parallel", 'n', 0, G_OPTION_ARG_INT, &cmdline_max_parallel, "max processes to run at once", "N"},
  {"mode", 'm', 0, G_OPTION_ARG_CALLBACK, handle_mode, "specify mode of operation", "MODE"},
  {"list-modes", 0, G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_CALLBACK, handle_list_modes,
   "list all modes of operation", NULL },
  {NULL,0,0,0,NULL,NULL,NULL}
};


int main(int argc, char **argv)
{
  GError *error = NULL;
  GOptionContext *op_context;
  unsigned i;

  cmdline_inputs = g_ptr_array_new ();
  op_context = g_option_context_new (NULL);
  g_option_context_set_summary (op_context, "run several programs in parallel");
  g_option_context_add_main_entries (op_context, op_entries, NULL);
  if (!g_option_context_parse (op_context, &argc, &argv, &error))
    {
      g_message ("error parsing command-line options: %s", error->message);
      return 1;
    }
  g_option_context_free (op_context);

  /* ignore sigpipe */
  signal (SIGPIPE, SIG_IGN);

  unsigned n_input_sources = 0;
  the_system = system_new ();
  system_trap (the_system, trap_funcs, NULL);
  if (cmdline_max_parallel > 0)
    system_set_max_running_tasks (the_system, cmdline_max_parallel);
  for (i = 0; i < cmdline_inputs->len; i++)
    {
      const char *filename = cmdline_inputs->pdata[i];
      if (strcmp (filename, "-") == 0)
        {
          n_input_sources++;
          system_add_input_stdin (the_system);
        }
      else
        {
          if (!system_add_input_script (the_system, filename, &error))
            g_error ("opening script: %s", error->message);
          n_input_sources++;
        }
    }
  if (n_input_sources == 0)
    {
      fprintf (stderr,
               "%s: no inputs given, nothing to do.  try --help\n",
               argv[0]);
      return 0;
    }
  GMainLoop *loop;
  loop = g_main_loop_new (g_main_context_default (), FALSE);
  g_main_loop_run (loop);
  return 0;
}
