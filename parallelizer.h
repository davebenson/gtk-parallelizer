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
  pid_t pid;		/* if running */
  TaskTerminationType termination_type;
  int termination_info;	/* exit status or signal */
};

struct _System
{
  GPtrArray *tasks;
  unsigned next_unstarted_task;

  GPtrArray *input_scripts;
  unsigned cur_input_script;

  TaskMessage *first_message, *last_message;
  
  int log_fd;

  unsigned max_unstarted_tasks;
  unsigned max_running_tasks;
};

System *system_new                     (void);
void    system_add_input_script        (System *system,
                                        const char *filename);
void    system_set_max_unstarted_tasks (System *system,
                                        unsigned n);
void    system_set_max_running_tasks   (System *system,
                                        unsigned n);

void    system_set_handlers            (System *system,
                                        TaskDataHandler data_handler, 
					TaskEndedNotify ended,
					void            handler_data);

