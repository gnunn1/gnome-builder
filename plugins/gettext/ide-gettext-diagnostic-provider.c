/* ide-gettext-diagnostic-provider.c
 *
 * Copyright (C) 2016 Daiki Ueno <dueno@src.gnome.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define G_LOG_DOMAIN "ide-gettext-diagnostic-provider"

#include <glib/gi18n.h>
#include <stdlib.h>

#include "egg-task-cache.h"

#include "ide-context.h"
#include "ide-diagnostic.h"
#include "ide-diagnostics.h"
#include "ide-file.h"
#include "ide-gettext-diagnostic-provider.h"
#include "ide-source-location.h"
#include "ide-unsaved-file.h"
#include "ide-unsaved-files.h"

struct _IdeGettextDiagnostics
{
  GObject parent_instance;
  IdeDiagnostics *diagnostics;
  guint64 sequence;
};

G_DEFINE_TYPE (IdeGettextDiagnostics, ide_gettext_diagnostics, G_TYPE_OBJECT)

enum {
  PROP_0,
  PROP_DIAGNOSTICS,
  PROP_SEQUENCE,
  LAST_PROP
};

static GParamSpec *properties [LAST_PROP];

static void
ide_gettext_diagnostics_set_property (GObject      *object,
                                      guint         prop_id,
                                      const GValue *value,
                                      GParamSpec   *pspec)
{
  IdeGettextDiagnostics *self = IDE_GETTEXT_DIAGNOSTICS (object);

  switch (prop_id)
    {
    case PROP_DIAGNOSTICS:
      self->diagnostics = g_value_dup_boxed (value);
      break;

    case PROP_SEQUENCE:
      self->sequence = g_value_get_uint64 (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
ide_gettext_diagnostics_finalize (GObject *object)
{
  IdeGettextDiagnostics *self = IDE_GETTEXT_DIAGNOSTICS (object);

  g_clear_pointer (&self->diagnostics, ide_diagnostics_unref);

  G_OBJECT_CLASS (ide_gettext_diagnostics_parent_class)->finalize (object);
}

static void
ide_gettext_diagnostics_class_init (IdeGettextDiagnosticsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = ide_gettext_diagnostics_finalize;
  object_class->set_property = ide_gettext_diagnostics_set_property;

  properties [PROP_DIAGNOSTICS] =
    g_param_spec_boxed ("diagnostics",
                        "Diagnostics",
                        "Diagnostics",
                        IDE_TYPE_DIAGNOSTICS,
                        (G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE));

  properties [PROP_SEQUENCE] =
    g_param_spec_uint64 ("sequence",
                         "Sequence",
                         "The document sequence number",
                         0,
                         G_MAXUINT64,
                         0,
                         (G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE));

  g_object_class_install_properties (object_class, LAST_PROP, properties);
}

static void
ide_gettext_diagnostics_init (IdeGettextDiagnostics *self)
{
}

struct _IdeGettextDiagnosticProvider
{
  IdeObject     parent_instance;
  EggTaskCache *diagnostics_cache;
};

static void diagnostic_provider_iface_init (IdeDiagnosticProviderInterface *iface);

G_DEFINE_TYPE_EXTENDED (IdeGettextDiagnosticProvider,
                        ide_gettext_diagnostic_provider,
                        IDE_TYPE_OBJECT,
                        0,
                        G_IMPLEMENT_INTERFACE (IDE_TYPE_DIAGNOSTIC_PROVIDER,
                                               diagnostic_provider_iface_init))

typedef struct
{
  IdeFile *file;
  IdeUnsavedFile *unsaved_file;
} TranslationUnit;

static void
translation_unit_free (TranslationUnit *unit)
{
  if (unit != NULL)
    {
      g_clear_object (&unit->file);
      g_clear_pointer (&unit->unsaved_file, ide_unsaved_file_unref);
      g_slice_free (TranslationUnit, unit);
    }
}

static IdeUnsavedFile *
get_unsaved_file (IdeGettextDiagnosticProvider *self,
                  IdeFile                      *file)
{
  IdeUnsavedFiles *unsaved_files;
  IdeContext *context;
  g_autoptr(GPtrArray) array = NULL;
  guint index;

  context = ide_object_get_context (IDE_OBJECT (self));
  unsaved_files = ide_context_get_unsaved_files (context);
  array = ide_unsaved_files_to_array (unsaved_files);

  for (index = 0; index < array->len; index++)
    {
      IdeUnsavedFile *unsaved_file = g_ptr_array_index (array, index);
      if (g_file_equal (ide_unsaved_file_get_file (unsaved_file), ide_file_get_file (file)))
        return ide_unsaved_file_ref (unsaved_file);
    }

  return NULL;
}

static void
get_diagnostics_cb (GObject      *source_object,
                    GAsyncResult *res,
                    gpointer      user_data)
{
  EggTaskCache *cache = EGG_TASK_CACHE (source_object);
  g_autoptr(GTask) task = user_data;
  IdeGettextDiagnostics *diags;
  GError *error = NULL;

  diags = egg_task_cache_get_finish (cache, res, &error);
  if (!diags)
    g_task_return_error (task, error);
  else
    g_task_return_pointer (task, diags, g_object_unref);
}

static void
ide_gettext_diagnostic_provider_diagnose_async (IdeDiagnosticProvider *provider,
                                                IdeFile               *file,
                                                GCancellable          *cancellable,
                                                GAsyncReadyCallback    callback,
                                                gpointer               user_data)
{
  IdeGettextDiagnosticProvider *self = (IdeGettextDiagnosticProvider *)provider;
  g_autoptr(IdeUnsavedFile) unsaved_file = NULL;
  IdeGettextDiagnostics *cached;
  g_autoptr(GTask) task = NULL;

  g_return_if_fail (IDE_IS_GETTEXT_DIAGNOSTIC_PROVIDER (self));
  g_return_if_fail (!cancellable || G_IS_CANCELLABLE (cancellable));

  task = g_task_new (self, cancellable, callback, user_data);

  unsaved_file = get_unsaved_file (self, file);

  if ((cached = egg_task_cache_peek (self->diagnostics_cache, file)) &&
      (cached->sequence >= ide_unsaved_file_get_sequence (unsaved_file)))
    {
      g_task_return_pointer (task, g_object_ref (cached), g_object_unref);
      return;
    }

  egg_task_cache_get_async (self->diagnostics_cache,
                            file,
                            TRUE,
                            cancellable,
                            get_diagnostics_cb,
                            g_object_ref (task));
}

static IdeDiagnostics *
ide_gettext_diagnostic_provider_diagnose_finish (IdeDiagnosticProvider  *provider,
                                                 GAsyncResult           *result,
                                                 GError                **error)
{
  GTask *task = (GTask *)result;
  g_autoptr(IdeGettextDiagnostics) object = NULL;

  g_return_val_if_fail (IDE_IS_GETTEXT_DIAGNOSTIC_PROVIDER (provider), NULL);
  g_return_val_if_fail (G_IS_TASK (task), NULL);

  if (NULL == (object = g_task_propagate_pointer (task, error)))
    return NULL;

  return ide_diagnostics_ref (object->diagnostics);
}

static void
diagnostic_provider_iface_init (IdeDiagnosticProviderInterface *iface)
{
  iface->diagnose_async = ide_gettext_diagnostic_provider_diagnose_async;
  iface->diagnose_finish = ide_gettext_diagnostic_provider_diagnose_finish;
}

static void
ide_gettext_diagnostic_provider_finalize (GObject *object)
{
  IdeGettextDiagnosticProvider *self = IDE_GETTEXT_DIAGNOSTIC_PROVIDER (object);

  g_clear_object (&self->diagnostics_cache);

  G_OBJECT_CLASS (ide_gettext_diagnostic_provider_parent_class)->finalize (object);
}

static void
ide_gettext_diagnostic_provider_class_init (IdeGettextDiagnosticProviderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = ide_gettext_diagnostic_provider_finalize;
}

static void
subprocess_wait_cb (GObject      *source_object,
                    GAsyncResult *res,
                    gpointer      user_data)
{
  GSubprocess *subprocess = G_SUBPROCESS (source_object);
  g_autoptr(GTask) task = user_data;
  TranslationUnit *unit = g_task_get_task_data (task);
  GPtrArray *array = NULL;
  IdeGettextDiagnostics *diags;
  GInputStream *stderr_input;
  GDataInputStream *stderr_data_input;
  g_autofree gchar *input_prefix = NULL;
  GError *error = NULL;

  if (!g_subprocess_wait_finish (subprocess, res, &error))
    {
      g_task_return_error (task, error);
      return;
    }

  array = g_ptr_array_new_with_free_func ((GDestroyNotify)ide_diagnostic_unref);
  if (g_subprocess_get_exit_status (subprocess) == 0)
    goto out;

  stderr_input = g_subprocess_get_stderr_pipe (subprocess);
  stderr_data_input = g_data_input_stream_new (stderr_input);
  g_clear_object (&stderr_input);

  input_prefix = g_strdup_printf ("%s:", ide_unsaved_file_get_temp_path (unit->unsaved_file));

  for (;;)
    {
      g_autofree gchar *line = NULL;
      gsize length;

      line = g_data_input_stream_read_line (stderr_data_input,
                                            &length,
                                            g_task_get_cancellable (task),
                                            &error);
      if (line == NULL)
        break;

      if (g_str_has_prefix (line, input_prefix))
        {
          gchar *p = line + strlen (input_prefix);

          if (g_ascii_isdigit (*p))
            {
              gulong line_number = strtoul (p, &p, 10);
              IdeSourceLocation *loc;
              IdeDiagnostic *diag;

              loc = ide_source_location_new (unit->file,
                                             line_number - 1,
                                             0,
                                             0);
              diag = ide_diagnostic_new (IDE_DIAGNOSTIC_WARNING,
                                         g_strstrip (p + 1),
                                         loc);
              g_ptr_array_add (array, diag);
            }
        }
    }

 out:
  diags = g_object_new (IDE_TYPE_GETTEXT_DIAGNOSTICS,
                        "diagnostics", ide_diagnostics_new (array),
                        "sequence", ide_unsaved_file_get_sequence (unit->unsaved_file),
                        NULL);
  g_task_return_pointer (task, diags, g_object_unref);
}

static const gchar *
id_to_xgettext_language (const gchar *id)
{
  static const struct {
    const gchar *id;
    const gchar *lang;
  } id_to_lang[] = {
    { "awk", "awk" },
    { "c", "C" },
    { "chdr", "C" },
    { "cpp", "C++" },
    { "js", "JavaScript" },
    { "lisp", "Lisp" },
    { "objc", "ObjectiveC" },
    { "perl", "Perl" },
    { "php", "PHP" },
    { "python", "Python" },
    { "sh", "Shell" },
    { "tcl", "Tcl" },
    { "vala", "Vala" }
  };
  gsize i;

  if (id != NULL)
    {
      for (i = 0; i < G_N_ELEMENTS (id_to_lang); i++)
        if (strcmp (id, id_to_lang[i].id) == 0)
          return id_to_lang[i].lang;
    }

  return NULL;
}

static void
populate_cache (EggTaskCache  *cache,
                gconstpointer  key,
                GTask         *task,
                gpointer       user_data)
{
  IdeFile *file = (IdeFile *)key;
  IdeGettextDiagnosticProvider *self = user_data;
  g_autoptr(IdeUnsavedFile) unsaved_file = get_unsaved_file (self, file);
  GtkSourceLanguage *language = ide_file_get_language (file);
  const gchar *language_id = gtk_source_language_get_id (language);
  g_autoptr(GSubprocess) subprocess = NULL;
  TranslationUnit *unit;
  GError *error = NULL;

  if (!ide_unsaved_file_persist (unsaved_file,
                                 g_task_get_cancellable (task),
                                 &error))
    {
      g_task_return_error (task, error);
      return;
    }

  subprocess = g_subprocess_new (G_SUBPROCESS_FLAGS_STDIN_PIPE
                                 | G_SUBPROCESS_FLAGS_STDOUT_PIPE
                                 | G_SUBPROCESS_FLAGS_STDERR_PIPE,
                                 &error,
                                 "xgettext",
                                 "--check=ellipsis-unicode",
                                 "--check=quote-unicode",
                                 "--check=space-ellipsis",
                                 "-k_",
                                 "-kN_",
                                 "-L", id_to_xgettext_language (language_id),
                                 "-o" "-",
                                 ide_unsaved_file_get_temp_path (unsaved_file),
                                 NULL);
  if (!subprocess)
    {
      g_task_return_error (task, error);
      return;
    }

  unit = g_slice_new0 (TranslationUnit);
  unit->file = g_object_ref (file);
  unit->unsaved_file = ide_unsaved_file_ref (unsaved_file);
  g_task_set_task_data (task, unit, (GDestroyNotify) translation_unit_free);

  g_subprocess_wait_async (subprocess,
                           g_task_get_cancellable (task),
                           subprocess_wait_cb,
                           task);
}

static void
ide_gettext_diagnostic_provider_init (IdeGettextDiagnosticProvider *self)
{
  self->diagnostics_cache = egg_task_cache_new ((GHashFunc)ide_file_hash,
                                                (GEqualFunc)ide_file_equal,
                                                g_object_ref,
                                                g_object_unref,
                                                g_object_ref,
                                                g_object_unref,
                                                20 * 1000L,
                                                populate_cache,
                                                self,
                                                NULL);
}
