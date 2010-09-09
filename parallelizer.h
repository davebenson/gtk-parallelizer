
typedef struct _TaskMessage TaskMessage;
typedef struct _Task Task;
typedef struct _System System;
typedef struct _Source Source;

#include <glib.h>

#define PARALLELIZER_ERROR_DOMAIN_QUARK   g_quark_from_static_string("Parallelizer")
typedef enum
{
  PARALLELIZER_ERROR_OPEN
} ParallelizerErrorCode;

/* On most systems, a process can only terminate in these two ways:
 *   - called exit() or _exit()
 *   - killed by a signal (from the kernel, or via kill() or raise())
 */
typedef enum
{
  TASK_TERMINATION_EXIT,
  TASK_TERMINATION_SIGNAL
} TaskTerminationType;



typedef enum
{
  TASK_WAITING,
  TASK_RUNNING,
  TASK_DONE
} TaskState;

struct _TaskMessage
{
  GTimeVal timestamp;
  unsigned len;
  guint8 *data;
  gboolean is_stderr;

  TaskMessage *next_in_system;
  TaskMessage *next_in_task;
};

struct _Task
{
  System *system;
  unsigned task_index;
  char *str;		/* a command-line */
  TaskState state;
  TaskMessage *first_message, *last_message;
  union {
    struct {
      pid_t pid;
      GPollFD stdin_poll;
      GPollFD stdout_poll;
      GPollFD stderr_poll;
      gboolean has_stdin_poll;
    } running;
    struct {
      TaskTerminationType termination_type;
      int termination_info;
    } terminated;
  } info;
};

typedef void (*SourceCommandlineCallback)(Source *source,
                                          const char *str,
                                          void *trap_data);
struct _Source
{
  /* implementation only needs to support one trap;
     if callback==NULL, untrap.  */
  void (*trap)(Source *source);
  void (*untrap)(Source *source);
  void (*destroy)(Source *source);

  SourceCommandlineCallback callback;
  void *trap_data;
};

void source_trap   (Source *source,
                    SourceCommandlineCallback callback,
                    void *trap_data);
void source_untrap (Source *source);

typedef struct _SystemTrap SystemTrap;
typedef struct _SystemTrapFuncs SystemTrapFuncs;
struct _SystemTrapFuncs
{
  void (*handle_data) (Task *task,
                       const GTimeVal *current_time,
                       gboolean is_stderr, /* else is stdout */
                       unsigned len,
                       const guint8 *data,
                       gpointer handler_data);
  void (*handle_line) (Task *task,
                       const GTimeVal *current_time,
                       gboolean is_stderr, /* else is stdout */
                       const char *text,
                       gpointer handler_data);
  void (*ended)       (Task *task,
                       const GTimeVal *current_time,
                       TaskTerminationType termination_type,
                       int termination_info,
                       gpointer handler_data);
  void (*all_done)    (System *system,
                       const GTimeVal *current_time,
                       gpointer handler_data);
};



struct _System
{
  /* invariants: next_unstarted_task <= tasks->len 
          AND    n_unstarted_tasks+n_running_tasks+n_finished_tasks = tasks->len
   */
  GPtrArray *tasks;
  unsigned next_unstarted_task;
  unsigned n_unstarted_tasks;
  unsigned n_running_tasks;
  unsigned n_finished_tasks;

  GPtrArray *input_sources;
  unsigned cur_input_source;
  gboolean is_input_source_trapped;

  TaskMessage *first_message, *last_message;
  
  int log_fd;

  unsigned max_unstarted_tasks;
  unsigned max_running_tasks;

  SystemTrap *trap_list;
};

System *system_new                     (void);
void    system_add_input_source        (System *system,
                                        Source *source);
gboolean system_add_input_script       (System     *system,
                                        const char *filename,
                                        GError    **error);
void    system_add_input_stdin         (System *system);
void    system_add_input_fd            (System *system,
                                        int     fd,
                                        gboolean should_close);
void    system_set_max_unstarted_tasks (System *system,
                                        unsigned n);
void    system_set_max_running_tasks   (System *system,
                                        unsigned n);

SystemTrap *system_trap                (System *system,
                                        SystemTrapFuncs *funcs,
                                        void            *trap_data);

