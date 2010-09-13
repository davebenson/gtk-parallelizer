#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include "parallelizer.h"

#define WINDOW_NAME                     "window1"

static GPtrArray *cmdline_inputs = NULL;
static int cmdline_max_parallel = -1;


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

static GOptionEntry op_entries[] =
{
  {"input", 'i', 0, G_OPTION_ARG_CALLBACK, handle_input, "script to run", "FILENAME"},
  {"max-parallel", 'n', 0, G_OPTION_ARG_INT, &cmdline_max_parallel, "max processes to run at once", "N"},
  {NULL,0,0,0,NULL,NULL,NULL}
};

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

static SystemTrapFuncs trap_funcs =
{
  syshandler__handle_data,
  syshandler__handle_line,
  syshandler__ended,
  syshandler__all_done
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

  System *system;
  system = system_new ();
  system_trap (system, &trap_funcs, NULL);
  if (cmdline_max_parallel > 0)
    system_set_max_running_tasks (system, cmdline_max_parallel);
  for (i = 0; i < cmdline_inputs->len; i++)
    {
      const char *filename = cmdline_inputs->pdata[i];
      if (strcmp (filename, "-") == 0)
        system_add_input_stdin (system);
      else
        {
          if (!system_add_input_script (system, filename, &error))
            g_error ("opening script: %s", error->message);
        }
    }
  GMainLoop *loop;
  loop = g_main_loop_new (g_main_context_default (), FALSE);
  g_main_loop_run (loop);
  return 0;
}
