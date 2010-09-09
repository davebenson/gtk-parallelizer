
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

System *
system_new (void)
{
  System *system = g_slice_new (System);
  system->tasks = g_ptr_array_new ();
  system->next_unstarted_task = 0;
  system->input_scripts = g_ptr_array_new ();
  system->cur_input_script = 0;
  system->first_message = system->last_message = NULL;
  system->log_fd = -1;
  system->max_unstarted_tasks = DEFAULT_MAX_UNSTARTED_TASKS;
  system->max_running_tasks = DEFAULT_MAX_RUNNING_TASKS;
  return system;
}

void    system_add_input_source        (System *system,
                                        Source *source)
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

void    system_set_max_unstarted_tasks (System *system,
                                        unsigned n);
void    system_set_max_running_tasks   (System *system,
                                        unsigned n);

void    system_set_handlers            (System *system,
                                        TaskDataHandler data_handler, 
					TaskEndedNotify ended,
					void            handler_data);

