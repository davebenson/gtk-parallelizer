#include <gtk/gtk.h>

#define WINDOW_NAME                     "window1"

static const char *cmdline_filename = NULL;
static int cmdline_max_parallel = -1;

static GOptionEntry op_entries[] =
{
  {"input", 'i', 0, G_OPTION_ARG_FILENAME, &cmdline_filename, "script to run", "FILENAME"},
  {"max-parallel", 'n', 0, G_OPTION_ARG_INT, &cmdline_max_parallel, "max processes to run at once", "N"},
  {NULL,0,0,0,NULL,NULL,NULL}
};


int main(int argc, char **argv)
{

  GtkBuilder *builder;
  GtkWindow *window;
  GError *error = NULL;
  GOptionContext *op_context;

  gtk_init (&argc, &argv);

  op_context = g_option_context_new (NULL);
  g_option_context_set_summary (op_context, "run several programs in parallel");
  g_option_context_add_main_entries (op_context, op_entries, NULL);
  if (!g_option_context_parse (op_context, &argc, &argv, &error))
    {
      g_message ("error parsing command-line options: %s", error->message);
      return 1;
    }
  g_option_context_free (op_context);

  builder = gtk_builder_new ();
  if (!gtk_builder_add_from_file (builder, "parallelizer.glade", &error))
    g_error ("error loading parallelizer.glade: %s", error->message);
  window = GTK_WINDOW (gtk_builder_get_object (builder, WINDOW_NAME));
  gtk_widget_show_all (GTK_WIDGET (window));
  gtk_main ();
  return 0;
}
