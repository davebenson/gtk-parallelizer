
typedef struct _TaskMessage TaskMessage;
typedef struct _Task Task;
typedef struct _System System;
typedef struct _Source Source;

/* On most systems, a process can only terminate in these two ways:
 *   - called exit() or _exit()
 *   - killed by a signal (from the kernel, or via kill() or raise())
 */
typedef enum
{
  TASK_TERMINATION_EXIT,
  TASK_TERMINATION_SIGNAL
} TaskTerminationType;


/* --- Task Handlers --- */
typedef void (*TaskDataHandler) (Task *task,
				 GTimeVal current_time,
                                 gboolean is_stderr, /* else is stdout */
                                 unsigned len,
                                 const guint8 *data,
				 gpointer handler_data);
/* termination_info is a signal number or exit-status */
typedef void (*TaskEndedNotify) (Task *task,
				 GTimeVal current_time,
                                 TaskTerminationType termination_type,
                                 int termination_info,
				 gpointer handler_data);


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
  unsigned cur_input_sources;

  TaskMessage *first_message, *last_message;
  
  int log_fd;

  unsigned max_unstarted_tasks;
  unsigned max_running_tasks;
};

System *system_new                     (void);
void    system_add_input_source        (System *system,
                                        Source *source);
void    system_add_input_script        (System *system,
                                        const char *filename);
void    system_add_input_stdin         (System *system);
void    system_add_input_fd            (System *system,
                                        int     fd,
                                        gboolean should_close);
void    system_set_max_unstarted_tasks (System *system,
                                        unsigned n);
void    system_set_max_running_tasks   (System *system,
                                        unsigned n);

void    system_set_handlers            (System *system,
                                        TaskDataHandler data_handler, 
					TaskEndedNotify ended,
					void            handler_data);

