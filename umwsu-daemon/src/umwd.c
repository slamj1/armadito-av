#include <libumwsu/scan.h>

#include <assert.h>
#include <glib.h>
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <dirent.h>

#define UMW

static guint inotify_event_hash(gconstpointer key);
static gboolean inotify_event_equal(gconstpointer a, gconstpointer b);

struct umwd {
  int inotify_fd;
  GHashTable *watch_table;
  GHashTable *event_queue;
#ifdef UMW
  struct umw *umw;
#endif
};

static void error(const char *msg)
{
  perror(msg);
  exit(EXIT_FAILURE);
}

static void umwd_init(struct umwd *d)
{
  d->inotify_fd = inotify_init();
  if (d->inotify_fd == -1)
    error("inotify_init");

  d->watch_table = g_hash_table_new(g_direct_hash, g_direct_equal);
  d->event_queue = g_hash_table_new(inotify_event_hash, inotify_event_equal);

#ifdef UMW
  d->umw = umw_open();
#endif
}

static void umwd_watch(struct umwd *d, const char *path, int recurse)
{
  int wd;

  if (recurse) {
    DIR *dir;
    struct dirent *entry;

    dir = opendir(path);
    if (dir == NULL)
      error("opendir");

    while((entry = readdir(dir)) != NULL) {
      char *entry_path;

      if (entry->d_type != DT_DIR || !strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
	continue;
      
      if (asprintf(&entry_path, "%s/%s", path, entry->d_name) == -1)
	error("asprintf");

      umwd_watch(d, entry_path, 1);
      
      free(entry_path);
    }
  }

  wd = inotify_add_watch(d->inotify_fd, path, IN_ALL_EVENTS);

  if (wd == -1)
    error("inotify_add_watch");

  fprintf(stderr, "adding watch %d on %s\n", wd, path);

  g_hash_table_insert(d->watch_table, GINT_TO_POINTER(wd), (gpointer)strdup(path));
}

static void inotify_event_print(FILE *out, const struct inotify_event *e)
{
  fprintf(out, "umwd: inotify event: wd = %2d ", e->wd);

  if (e->cookie > 0)
    fprintf(out, "cookie = %4d ", e->cookie);

  fprintf(out, "mask = ");

#define M(_mask, _mask_bit) do { if ((_mask) & (_mask_bit)) fprintf(out, #_mask_bit " "); } while(0)

  M(e->mask, IN_ACCESS);
  M(e->mask, IN_ATTRIB);
  M(e->mask, IN_CLOSE_NOWRITE);
  M(e->mask, IN_CLOSE_WRITE);
  M(e->mask, IN_CREATE);
  M(e->mask, IN_DELETE);
  M(e->mask, IN_DELETE_SELF);
  M(e->mask, IN_IGNORED);
  M(e->mask, IN_ISDIR);
  M(e->mask, IN_MODIFY);
  M(e->mask, IN_MOVE_SELF);
  M(e->mask, IN_MOVED_FROM);
  M(e->mask, IN_MOVED_TO);
  M(e->mask, IN_OPEN);
  M(e->mask, IN_Q_OVERFLOW);
  M(e->mask, IN_UNMOUNT);

  if (e->len > 0)
    fprintf(out, " name = %s", e->name);

  fprintf(out, "\n");
}

static struct inotify_event *inotify_event_clone(struct inotify_event *event)
{
  struct inotify_event *clone = (struct inotify_event *)malloc(sizeof(struct inotify_event) + event->len);
  
  assert(clone != NULL);

  clone->wd = event->wd;
  clone->mask = event->mask;
  clone->cookie = event->cookie;
  clone->len = event->len;
  if (event->len > 0)
    strncpy(clone->name, event->name, event->len);

  return clone;
}

static guint inotify_event_hash(gconstpointer key)
{
  const struct inotify_event *e = (const struct inotify_event *)key;
  guint h;

  h = g_int_hash((gconstpointer)&e->wd);
  h += g_int_hash((gconstpointer)&e->mask);
  h += g_int_hash((gconstpointer)&e->cookie);

  if (e->len) {
    h += g_int_hash((gconstpointer)&e->len);
    h += g_str_hash((gconstpointer)e->name);
  }

  return h;
}

static gboolean inotify_event_equal(gconstpointer a, gconstpointer b)
{
  const struct inotify_event *ea = (const struct inotify_event *)a;
  const struct inotify_event *eb = (const struct inotify_event *)b;

  if (ea->wd != eb->wd)
    return FALSE;

  if (ea->mask != eb->mask)
    return FALSE;

  if (ea->cookie != eb->cookie)
    return FALSE;

  if (ea->len != eb->len)
    return FALSE;

  if (ea->len != 0)
    if (strcmp(ea->name, eb->name))
      return FALSE;

  return TRUE;
}

static char *umwd_event_full_path(struct umwd *d, struct inotify_event *event)
{
  char *full_path, *dir;

  dir = (char *)g_hash_table_lookup(d->watch_table, GINT_TO_POINTER(event->wd));

  if (dir == NULL)
    error("dir lookup");

  if (asprintf(&full_path, "%s/%s", dir, event->name) == -1)
    error("asprintf");

  return full_path;
}

static void event_queue_print_entry(gpointer key, gpointer value, gpointer user_data)
{
  inotify_event_print(stderr, (const struct inotify_event *)key);
}

static void umwd_print_event_queue(struct umwd *d, const char *msg)
{
  fprintf(stderr, "umwd: event queue (%s):\n", msg);
  g_hash_table_foreach(d->event_queue, event_queue_print_entry, NULL);
  fprintf(stderr, "umwd: end of event queue:\n");
}

static void umwd_process_dir_create(struct umwd *d, struct inotify_event *event)
{
}

static void umwd_process_file_create(struct umwd *d, struct inotify_event *event)
{
  struct inotify_event *close_event = inotify_event_clone(event);

  fprintf(stderr, "umwd: processing file create path = %s\n", event->name);

  close_event->mask = IN_CLOSE_WRITE;

  g_hash_table_add(d->event_queue, close_event);

#if 0
  umwd_print_event_queue(d, "create");
#endif
}

static void umwd_process_file_close_write(struct umwd *d, struct inotify_event *event)
{
  char *full_path = umwd_event_full_path(d, event);

  fprintf(stderr, "umwd: processing file close write full_path = %s\n", full_path);

#ifdef UMW
  if (g_hash_table_contains(d->event_queue, event)) {
#if 0
    fprintf(stderr, "umwd: event is in queue\n");
#endif

    umw_scan_file(d->umw, full_path);

    g_hash_table_remove(d->event_queue, event);

#if 0
    umwd_print_event_queue(d, "close write");
#endif
  }
#endif

  free(full_path);
}

static void umwd_process_event(struct umwd *d, struct inotify_event *event)
{
  inotify_event_print(stderr, event);

  if ((event->mask & IN_CLOSE_WRITE) && !(event->mask & IN_ISDIR))
    umwd_process_file_close_write(d, event);
  else if ((event->mask & IN_CREATE) && !(event->mask & IN_ISDIR))
    umwd_process_file_create(d, event);
  else if ((event->mask & IN_CREATE) && (event->mask & IN_ISDIR))
    umwd_process_dir_create(d, event);
}

void print_entry(gpointer key, gpointer value, gpointer user_data)
{
  fprintf(stderr, "hash table entry %d -> %s\n", GPOINTER_TO_INT(key), (char *)value);
}

#define N_EVENTS 10
#define EVENT_BUFFER_LEN (N_EVENTS * (sizeof(struct inotify_event) + NAME_MAX + 1))

static void umwd_loop(struct umwd *d)
{
  char *event_buffer;

#if 0
  g_hash_table_foreach(d->watch_table, print_entry, NULL);
#endif

  event_buffer = (char *)malloc(EVENT_BUFFER_LEN);
  assert(event_buffer != NULL);

  while(1) {
    ssize_t nread;
    char *p;

    nread = read(d->inotify_fd, event_buffer, EVENT_BUFFER_LEN);
    
    if (nread == 0)
      error("read returned 0");

    if (nread < 0)
      error("read");

    fprintf(stderr, "read %ld from %d\n", nread, d->inotify_fd);

    p = event_buffer;
    while (p < event_buffer + nread) {
      struct inotify_event *event = (struct inotify_event *) p;

      umwd_process_event(d, event);

      p += sizeof(struct inotify_event) + event->len;
    }
  }
}

static void usage(int argc, char **argv)
{
  fprintf(stderr, "usage: %s [-r] FILE|DIR ...\n", argv[0]);
  exit(EXIT_FAILURE);
}

int main(int argc, char **argv)
{
  struct umwd umwd;
  int argp = 1;
  int recurse = 0;
  
  if (argc < 2)
    usage(argc, argv);

  if (!strcmp(argv[argp], "-r")) {
    argp++;
    recurse = 1;
  }

  umwd_init(&umwd);

  while (argp < argc) {
    struct stat sb;

    if (stat(argv[argp], &sb) == -1) {
      perror("stat");
      exit(EXIT_FAILURE);
    }

    if (!S_ISDIR(sb.st_mode)) {
      fprintf(stderr, "%s: not a directory\n", argv[argp]);
      exit(EXIT_FAILURE);
    }

    umwd_watch(&umwd, argv[argp], recurse);

    argp++;
  }

  umwd_loop(&umwd);
}
