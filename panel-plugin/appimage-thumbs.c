/* appimage-thumbs.c - Async icon loading for Type 2 AppImages
 *
 * Strategy:
 *   1. Derive the thumbnail cache path from the MD5 of the file:// URI
 *      per the freedesktop thumbnail spec.
 *   2. If a valid cached .png exists (mtime matches), use it immediately.
 *   3. Otherwise spawn `{appimage} --appimage-extract .DirIcon` in a
 *      temporary directory on a GLib thread-pool thread, scale the
 *      resulting image to 16 px, save it into the cache with the required
 *      spec metadata, then apply it to the GtkImage on the main thread.
 *
 * Only Type 2 AppImages are supported.  Type 1 images pre-date the
 * --appimage-extract flag and are not worth supporting.
 */

#include "appimage-thumbs.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#include <string.h>
#include <sys/stat.h>

/* ── Freedesktop thumbnail cache helpers ────────────────────────────────── */

/* Return the expected cache path for a given file:// URI.
 * The spec mandates: MD5(uri) + ".png" under ~/.cache/thumbnails/normal/.
 * Caller must g_free() the result. */
static gchar *
thumb_cache_path(const gchar *uri)
{
    gchar       *checksum;
    gchar       *filename;
    gchar       *path;

    checksum = g_compute_checksum_for_string(G_CHECKSUM_MD5, uri, -1);
    filename = g_strconcat(checksum, ".png", NULL);
    path     = g_build_filename(
            g_get_user_cache_dir(), "thumbnails", "normal", filename, NULL
        );

    g_free(checksum);
    g_free(filename);
    return path;
}

/* Check whether a cached thumbnail is still valid for the given file.
 * The spec requires that the PNG carries a "Thumb::MTime" text chunk
 * matching the source file's mtime.  Returns the loaded pixbuf (scaled
 * to size px) on success, NULL otherwise.  Caller owns the pixbuf. */
static GdkPixbuf *
load_valid_thumbnail(const gchar *cache_path,
                     const gchar *uri,
                     gint         size)
{
    GdkPixbuf   *pixbuf;
    GdkPixbuf   *scaled;
    const gchar *thumb_mtime;
    struct stat  st;
    gchar       *source_path;

    pixbuf = gdk_pixbuf_new_from_file(cache_path, NULL);
    if (pixbuf == NULL) {
        return NULL;
    }

    /* Validate mtime */
    thumb_mtime = gdk_pixbuf_get_option(pixbuf, "tEXt::Thumb::MTime");
    source_path = g_filename_from_uri(uri, NULL, NULL);

    if (thumb_mtime == NULL || source_path == NULL
            || g_stat(source_path, &st) != 0
            || atol(thumb_mtime) != (long)st.st_mtime) {
        g_object_unref(pixbuf);
        g_free(source_path);
        return NULL;
    }

    g_free(source_path);

    /* Scale to requested size */
    if (gdk_pixbuf_get_width(pixbuf)  == size
            && gdk_pixbuf_get_height(pixbuf) == size) {
        return pixbuf;
    }

    scaled = gdk_pixbuf_scale_simple(pixbuf, size, size,
                                     GDK_INTERP_BILINEAR);
    g_object_unref(pixbuf);
    return scaled;
}

/* Save a pixbuf into the thumbnail cache with the required spec metadata.
 * Creates ~/.cache/thumbnails/normal/ if it doesn't exist. */
static void
save_thumbnail(GdkPixbuf   *pixbuf,
               const gchar *cache_path,
               const gchar *uri,
               glong        mtime)
{
    gchar *mtime_str;
    gchar *dir;

    /* Ensure the cache directory exists */
    dir = g_path_get_dirname(cache_path);
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);

    mtime_str = g_strdup_printf("%ld", mtime);

    /* The spec requires Thumb::URI and Thumb::MTime text chunks */
    gdk_pixbuf_save(pixbuf, cache_path, "png", NULL,
                    "tEXt::Thumb::URI",   uri,
                    "tEXt::Thumb::MTime", mtime_str,
                    NULL);

    /* Thumbnails must be mode 0600 */
    g_chmod(cache_path, 0600);

    g_free(mtime_str);
}

/* ── Background extraction ──────────────────────────────────────────────── */

typedef struct {
    GWeakRef *image_ref;  /* weak reference; never keeps a menu alive */
    gchar    *path;       /* source AppImage path */
    gchar    *uri;        /* file:// URI of source */
    gchar    *cache_path; /* where to write/read the thumbnail */
    gint      size;       /* target icon size in pixels */
} ThumbTask;

static void
thumb_task_free(ThumbTask *task)
{
    g_free(task->path);
    g_free(task->uri);
    g_free(task->cache_path);
    g_free(task);
}

/* Data passed back to the main thread once extraction is done */
typedef struct {
    GWeakRef  *image_ref; /* ownership transferred from worker task */
    GdkPixbuf *pixbuf;   /* may be NULL on failure */
} ApplyData;

/* Main-thread callback: apply the pixbuf to the GtkImage */
static gboolean
apply_pixbuf_idle(gpointer user_data)
{
    ApplyData *data = (ApplyData *)user_data;

    /* Obtain a strong reference safely; the menu may already be gone. */
    GtkImage *image = g_weak_ref_get(data->image_ref);
    if (image != NULL) {
        if (data->pixbuf != NULL)
            gtk_image_set_from_pixbuf(image, data->pixbuf);
        g_object_unref(image);
    }
    g_weak_ref_clear(data->image_ref);
    g_free(data->image_ref);

    if (data->pixbuf != NULL) {
        g_object_unref(data->pixbuf);
    }

    g_free(data);
    return G_SOURCE_REMOVE;
}

/* Worker thread: extract .DirIcon from the AppImage, cache it, and
 * schedule application on the main thread. */
static void
extract_icon_thread(gpointer data, gpointer user_data)
{
    ThumbTask   *task    = (ThumbTask *)data;
    gchar       *tmpdir  = NULL;
    gchar       *argv[4];
    gchar       *stdout_out = NULL;
    gchar       *stderr_out = NULL;
    gint         exit_status;
    GError      *error   = NULL;
    GdkPixbuf   *pixbuf  = NULL;
    struct stat  st;
    ApplyData   *apply;

    /* Create a temporary directory for extraction */
    tmpdir = g_dir_make_tmp("appimage-icon-XXXXXX", &error);
    if (tmpdir == NULL) {
        g_warning("appimage-thumbs: failed to create temp dir: %s",
                  error ? error->message : "unknown");
        if (error) g_error_free(error);
        goto done;
    }

    /* Extract just .DirIcon from the AppImage.
     * --appimage-extract supports a glob argument since late 2019. */
    argv[0] = task->path;
    argv[1] = (gchar *)"--appimage-extract";
    argv[2] = (gchar *)".DirIcon";
    argv[3] = NULL;

    if (!g_spawn_sync(
                tmpdir,
                argv,
                NULL,
                G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                NULL, NULL,
                NULL, NULL,
                &exit_status,
                &error
            )) {
        g_warning("appimage-thumbs: spawn failed for %s: %s",
                  task->path, error ? error->message : "unknown");
        if (error) g_error_free(error);
        goto done;
    }

    /* The extracted file will be at {tmpdir}/squashfs-root/.DirIcon */
    {
        gchar *icon_path = g_build_filename(
                tmpdir, "squashfs-root", ".DirIcon", NULL
            );

        pixbuf = gdk_pixbuf_new_from_file_at_size(
                icon_path, task->size, task->size, &error
            );

        if (pixbuf == NULL) {
            /* .DirIcon might be a symlink to a named icon inside the
             * squashfs — try a .png and .svg fallback in the root */
            g_clear_error(&error);

            const gchar *exts[] = { "png", "svg", "xpm", NULL };
            gchar       *base   = g_path_get_basename(task->path);
            /* Strip .AppImage suffix */
            gchar       *dot    = g_strrstr(base, ".");
            if (dot) *dot = '\0';

            for (gint i = 0; exts[i] != NULL && pixbuf == NULL; i++) {
                gchar *fallback = g_build_filename(
                        tmpdir, "squashfs-root",
                        g_strconcat(base, ".", exts[i], NULL),
                        NULL
                    );
                pixbuf = gdk_pixbuf_new_from_file_at_size(
                        fallback, task->size, task->size, NULL
                    );
                g_free(fallback);
            }
            g_free(base);
        }

        g_free(icon_path);
    }

    if (pixbuf == NULL) {
        g_warning("appimage-thumbs: could not load icon from %s",
                  task->path);
        goto done;
    }

    /* Cache the thumbnail */
    if (g_stat(task->path, &st) == 0) {
        save_thumbnail(pixbuf, task->cache_path, task->uri,
                       (glong)st.st_mtime);
    }

done:
    /* Clean up temp directory */
    if (tmpdir != NULL) {
        gchar *squashfs_root = g_build_filename(tmpdir, "squashfs-root", NULL);
        /* Recursively remove squashfs-root then tmpdir.
         * We use GLib's file utilities to stay portable. */
        GFile *root_file = g_file_new_for_path(squashfs_root);
        g_file_delete(root_file, NULL, NULL); /* best-effort */
        g_object_unref(root_file);
        g_free(squashfs_root);

        /* Fall back to shell rm -rf for the recursive case */
        {
            gchar *argv_rm[] = { (gchar *)"rm", (gchar *)"-rf", tmpdir, NULL };
            g_spawn_async(NULL, argv_rm, NULL, G_SPAWN_SEARCH_PATH,
                          NULL, NULL, NULL, NULL);
        }
        g_free(tmpdir);
    }

    g_free(stdout_out);
    g_free(stderr_out);

    /* Schedule UI update on the main thread */
    apply          = g_new0(ApplyData, 1);
    apply->image_ref = task->image_ref;
    apply->pixbuf  = pixbuf; /* transfer ownership */
    g_idle_add(apply_pixbuf_idle, apply);

    thumb_task_free(task);
}

/* ── Public API ─────────────────────────────────────────────────────────── */

/* Thread pool shared across all calls — at most 2 concurrent extractions
 * so we don't spawn a dozen AppImage processes at once. */
static GThreadPool *thumb_pool = NULL;

static void
ensure_thread_pool(void)
{
    if (thumb_pool == NULL) {
        thumb_pool = g_thread_pool_new(
                extract_icon_thread,
                NULL,
                2,      /* max threads */
                FALSE,  /* not exclusive */
                NULL
            );
    }
}

void
appimage_load_icon_async(GtkImage    *image,
                         const gchar *path)
{
    gchar      *uri;
    gchar      *cache_path;
    GdkPixbuf  *cached;
    struct stat  st;
    ThumbTask  *task;

    g_return_if_fail(GTK_IS_IMAGE(image));
    g_return_if_fail(path != NULL);

    uri        = g_filename_to_uri(path, NULL, NULL);
    cache_path = thumb_cache_path(uri);

    /* ── Fast path: valid cached thumbnail ────────────────────────────── */
    cached = load_valid_thumbnail(cache_path, uri, 16);
    if (cached != NULL) {
        gtk_image_set_from_pixbuf(image, cached);
        g_object_unref(cached);
        g_free(uri);
        g_free(cache_path);
        return;
    }

    /* ── Slow path: extract on background thread ──────────────────────── */
    ensure_thread_pool();

    task             = g_new0(ThumbTask, 1);
    task->image_ref  = g_new0(GWeakRef, 1);
    g_weak_ref_init(task->image_ref, G_OBJECT(image));
    task->path       = g_strdup(path);
    task->uri        = uri;     /* transfer */
    task->cache_path = cache_path; /* transfer */
    task->size       = 16;

    g_thread_pool_push(thumb_pool, task, NULL);
}
