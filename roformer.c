#define _GNU_SOURCE
#include <deadbeef/deadbeef.h>
#include <gtk/gtk.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <ctype.h>
#include <sndfile.h>

#include <gtkui_api.h>

extern char **environ;

static DB_functions_t *deadbeef;
static DB_decoder_t plugin;
static int roformer_seek_sample(DB_fileinfo_t *_info, int sample);
static int get_samplerate(DB_playItem_t *it);
static int get_channels(DB_playItem_t *it);
static void notify_waveform_refresh_for_source(const char *source_uri);
static int cache_index_lookup(const char *uri, int mode, int64_t mtime, int64_t size, int *samplerate, int *channels, char *cache_file_out, size_t out_sz);
static int resolve_cache_final_path(DB_playItem_t *it, int mode, char *out, size_t out_sz);
static void cache_index_rewrite_with_filter(const char *remove_cache_path);
static int convert_raw_to_mp3(const char *raw_path, const char *mp3_path, int samplerate, int channels);

#define ROFORMER_MODE_ORIGINAL 0
#define ROFORMER_MODE_INSTRUMENTAL 1
#define ROFORMER_MODE_VOCAL 2

#define CACHE_LIMIT_BYTES (1024LL * 1024LL * 1024LL)
#define CACHE_LIMIT_DEFAULT_MB 1024
typedef struct {
    DB_fileinfo_t info;
    DB_playItem_t *track;
    int mode;
    int samplerate;
    int channels;
    int bytes_per_frame;
    int64_t data_bytes_read;

    int using_cache;
    FILE *cache_fp;
    SNDFILE *cache_snd;
    SF_INFO cache_sfinfo;
    int64_t cache_bytes_written;
    int64_t cache_data_offset;
    int64_t cache_frames_written;

    int pipe_fd;
    pid_t child_pid;
    FILE *cache_fp_write;
    char cache_final_path[PATH_MAX];
    char cache_raw_path[PATH_MAX];
    int owns_temp_cache;
    int stream_finished;
    int cache_valid;
    int need_spawn;
    char source_uri[PATH_MAX];
    int64_t source_mtime;
    int64_t source_size;
} roformer_info_t;

static void finish_stream(roformer_info_t *ri, int aborted);

typedef struct {
    char path[PATH_MAX];
    time_t mtime;
    off_t size;
} cache_entry_t;

typedef struct {
    int mode;
    int64_t mtime;
    int64_t size;
    int samplerate;
    int channels;
    int complete;
    char cache_file[PATH_MAX];
    char uri[PATH_MAX];
} cache_index_entry_t;

static GtkWidget *mode_button;
static ddb_gtkui_t *gtkui_api;
static volatile int precache_running;
static volatile int precache_rescan;

static DB_plugin_action_t mode_action;

#define trace(...) do { \
    if (deadbeef->conf_get_int("roformer.trace", 0)) { \
        deadbeef->log_detailed(&plugin.plugin, 0, __VA_ARGS__); \
    } \
} while (0)

static const char *
mode_emoji(int mode) {
    switch (mode) {
    case ROFORMER_MODE_INSTRUMENTAL:
        return "🎸";
    case ROFORMER_MODE_VOCAL:
        return "🎤";
    default:
        return "🔊";
    }
}

static int
roformer_get_mode(void) {
    int mode = deadbeef->conf_get_int("roformer.mode", ROFORMER_MODE_ORIGINAL);
    if (mode < ROFORMER_MODE_ORIGINAL || mode > ROFORMER_MODE_VOCAL) {
        mode = ROFORMER_MODE_ORIGINAL;
    }
    return mode;
}

static void
roformer_set_mode(int mode) {
    deadbeef->conf_set_int("roformer.mode", mode);
    deadbeef->conf_save();
    trace("roformer: mode set to %d\n", mode);
}

static void
ensure_dir(const char *path) {
    struct stat st = {0};
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        return;
    }
    mkdir(path, 0700);
}

static void
get_cache_dir(char *out, size_t out_sz) {
    if (!out || out_sz == 0) {
        return;
    }
    int n = snprintf(out, out_sz, "/tmp/deadbeef-%d-roformer-cache", (int)getuid());
    if (n < 0 || (size_t)n >= out_sz) {
        out[0] = 0;
        return;
    }
}

static void
get_f32tmp_dir(char *out, size_t out_sz) {
    if (!out || out_sz == 0) {
        return;
    }
    int n = snprintf(out, out_sz, "/tmp/deadbeef-%d-roformer-cache-f32tmp", (int)getuid());
    if (n < 0 || (size_t)n >= out_sz) {
        out[0] = 0;
        return;
    }
}

static int
safe_path_join2(char *out, size_t out_sz, const char *a, const char *b) {
    if (!out || out_sz == 0 || !a || !b) {
        return -1;
    }
    size_t la = strlen(a);
    size_t lb = strlen(b);
    int need_slash = la > 0 && a[la - 1] != '/';
    size_t need = la + (size_t)need_slash + lb + 1;
    if (need > out_sz) {
        return -1;
    }
    memcpy(out, a, la);
    size_t off = la;
    if (need_slash) {
        out[off++] = '/';
    }
    memcpy(out + off, b, lb);
    out[off + lb] = 0;
    return 0;
}

static int
get_cache_index_path(char *out, size_t out_sz) {
    char cache_dir[PATH_MAX];
    get_cache_dir(cache_dir, sizeof(cache_dir));
    if (!cache_dir[0]) {
        return -1;
    }
    return safe_path_join2(out, out_sz, cache_dir, "index.txt");
}

static void
remove_dir_all_files(const char *dirpath) {
    if (!dirpath || !dirpath[0]) {
        return;
    }
    DIR *dir = opendir(dirpath);
    if (!dir) {
        return;
    }
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') {
            continue;
        }
        char full[PATH_MAX];
        if (safe_path_join2(full, sizeof(full), dirpath, de->d_name) != 0) {
            continue;
        }
        unlink(full);
    }
    closedir(dir);
    rmdir(dirpath);
}

static int
get_uri_file_stat(const char *uri, int64_t *mtime, int64_t *size) {
    if (mtime) {
        *mtime = 0;
    }
    if (size) {
        *size = 0;
    }
    if (!uri || !uri[0]) {
        return -1;
    }
    const char *path = uri;
    if (!strncmp(uri, "file://", 7)) {
        path = uri + 7;
    }
    else if (strstr(uri, "://")) {
        return -1;
    }
    struct stat st = {0};
    if (stat(path, &st) != 0) {
        return -1;
    }
    if (mtime) {
        *mtime = (int64_t)st.st_mtime;
    }
    if (size) {
        *size = (int64_t)st.st_size;
    }
    return 0;
}

static int
parse_mode_token(const char *tok) {
    if (!tok || !tok[0]) {
        return -1;
    }
    if (!strcmp(tok, "instr")) {
        return ROFORMER_MODE_INSTRUMENTAL;
    }
    if (!strcmp(tok, "vocal")) {
        return ROFORMER_MODE_VOCAL;
    }
    int mode = atoi(tok); // backward compatibility with old numeric index entries
    if (mode == ROFORMER_MODE_INSTRUMENTAL || mode == ROFORMER_MODE_VOCAL) {
        return mode;
    }
    return -1;
}

static const char *
mode_to_token(int mode) {
    return mode == ROFORMER_MODE_VOCAL ? "vocal" : "instr";
}

static int
index_parse_line(char *line, cache_index_entry_t *e) {
    if (!line || !e) {
        return -1;
    }
    if (line[0] == '#') {
        return -1;
    }
    char *nl = strchr(line, '\n');
    if (nl) {
        *nl = 0;
    }
    memset(e, 0, sizeof(*e));

    char *save = NULL;
    char *tok = strtok_r(line, "\t", &save);
    if (!tok) return -1;
    e->mode = parse_mode_token(tok);
    if (e->mode < 0) return -1;

    tok = strtok_r(NULL, "\t", &save);
    if (!tok) return -1;
    e->mtime = atoll(tok);

    tok = strtok_r(NULL, "\t", &save);
    if (!tok) return -1;
    e->size = atoll(tok);

    tok = strtok_r(NULL, "\t", &save);
    if (!tok) return -1;
    e->samplerate = atoi(tok);

    tok = strtok_r(NULL, "\t", &save);
    if (!tok) return -1;
    e->channels = atoi(tok);

    tok = strtok_r(NULL, "\t", &save);
    if (!tok) return -1;
    if (tok[0] == '/') {
        e->complete = 1;
        strncpy(e->cache_file, tok, sizeof(e->cache_file) - 1);
    }
    else {
        e->complete = atoi(tok);
        tok = strtok_r(NULL, "\t", &save);
        if (!tok) return -1;
        strncpy(e->cache_file, tok, sizeof(e->cache_file) - 1);
    }

    tok = strtok_r(NULL, "\t", &save);
    if (!tok) return -1;
    strncpy(e->uri, tok, sizeof(e->uri) - 1);
    if (e->samplerate <= 0 || e->channels <= 0 || e->channels > 8) {
        return -1;
    }
    return 0;
}

static void
cache_index_write_header(FILE *fp) {
    if (!fp) {
        return;
    }
    fputs("# mode\tmtime\tsize\tsamplerate\tchannels\tcomplete\tcache_file\turi\n", fp);
}

static int
path_has_mp3_ext(const char *path) {
    if (!path) {
        return 0;
    }
    const char *ext = strrchr(path, '.');
    if (!ext) {
        return 0;
    }
    return !strcasecmp(ext, ".mp3");
}

static int
cache_index_find_uri_by_cache_file(const char *cache_file, char *uri_out, size_t uri_out_sz) {
    if (!cache_file || !uri_out || uri_out_sz == 0) {
        return -1;
    }
    uri_out[0] = 0;
    char index_path[PATH_MAX];
    if (get_cache_index_path(index_path, sizeof(index_path)) != 0) {
        return -1;
    }
    FILE *fp = fopen(index_path, "r");
    if (!fp) {
        return -1;
    }
    char line[PATH_MAX * 2];
    cache_index_entry_t e;
    int found = -1;
    while (fgets(line, sizeof(line), fp)) {
        if (index_parse_line(line, &e) != 0) {
            continue;
        }
        if (!strcmp(e.cache_file, cache_file)) {
            snprintf(uri_out, uri_out_sz, "%s", e.uri);
            found = 0;
            break;
        }
    }
    fclose(fp);
    return found;
}

static int
get_track_source_uri(DB_playItem_t *it, char *uri_out, size_t uri_out_sz) {
    if (!it || !uri_out || uri_out_sz == 0) {
        return -1;
    }
    uri_out[0] = 0;
    deadbeef->pl_lock();
    const char *src = deadbeef->pl_find_meta(it, ":ROFORMER_SOURCE_URI");
    if (!src) {
        src = deadbeef->pl_find_meta(it, ":URI");
    }
    if (src) {
        strncpy(uri_out, src, uri_out_sz - 1);
    }
    deadbeef->pl_unlock();
    return uri_out[0] ? 0 : -1;
}

static int
uri_in_current_playlist(const char *uri) {
    if (!uri || !uri[0]) {
        return 0;
    }
    deadbeef->pl_lock();
    ddb_playlist_t *plt = deadbeef->plt_get_curr();
    deadbeef->pl_unlock();
    if (!plt) {
        return 0;
    }
    deadbeef->pl_lock();
    DB_playItem_t *it = deadbeef->plt_get_first(plt, PL_MAIN);
    deadbeef->pl_unlock();
    int found = 0;
    while (it) {
        char it_uri[PATH_MAX] = {0};
        get_track_source_uri(it, it_uri, sizeof(it_uri));
        deadbeef->pl_lock();
        DB_playItem_t *next = deadbeef->pl_get_next(it, PL_MAIN);
        deadbeef->pl_unlock();
        deadbeef->pl_item_unref(it);
        it = next;
        if (it_uri[0] && !strcmp(it_uri, uri)) {
            found = 1;
            break;
        }
    }
    deadbeef->plt_unref(plt);
    return found;
}

static int
uri_is_currently_playing_or_loaded(const char *uri) {
    if (!uri || !uri[0]) {
        return 0;
    }
    DB_playItem_t *playing = deadbeef->streamer_get_playing_track_safe();
    if (playing) {
        char playing_uri[PATH_MAX] = {0};
        get_track_source_uri(playing, playing_uri, sizeof(playing_uri));
        deadbeef->pl_item_unref(playing);
        if (playing_uri[0] && !strcmp(playing_uri, uri)) {
            return 1;
        }
    }
    DB_playItem_t *streaming = deadbeef->streamer_get_streaming_track();
    if (streaming) {
        char streaming_uri[PATH_MAX] = {0};
        get_track_source_uri(streaming, streaming_uri, sizeof(streaming_uri));
        deadbeef->pl_item_unref(streaming);
        if (streaming_uri[0] && !strcmp(streaming_uri, uri)) {
            return 1;
        }
    }
    return 0;
}

static void
cache_remove_unrelated_files(void) {
    char cache_dir[PATH_MAX];
    get_cache_dir(cache_dir, sizeof(cache_dir));
    if (!cache_dir[0]) {
        return;
    }
    DIR *dir = opendir(cache_dir);
    if (!dir) {
        return;
    }
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.' || !strcmp(de->d_name, "index.txt")) {
            continue;
        }
        char full[PATH_MAX];
        if (safe_path_join2(full, sizeof(full), cache_dir, de->d_name) != 0) {
            continue;
        }
        struct stat st = {0};
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        char uri[PATH_MAX] = {0};
        if (cache_index_find_uri_by_cache_file(full, uri, sizeof(uri)) != 0 || !uri[0]) {
            continue;
        }
        if (!uri_in_current_playlist(uri) && !uri_is_currently_playing_or_loaded(uri)) {
            if (unlink(full) == 0) {
                cache_index_rewrite_with_filter(full);
                trace("roformer: removed unrelated cache %s uri=%s\n", full, uri);
            }
        }
    }
    closedir(dir);
}

static int
uri_protected_from_precache_eviction(const char *uri) {
    if (!uri || !uri[0]) {
        return 0;
    }
    DB_playItem_t *playing = deadbeef->streamer_get_playing_track_safe();
    char playing_uri[PATH_MAX] = {0};
    if (playing) {
        get_track_source_uri(playing, playing_uri, sizeof(playing_uri));
        deadbeef->pl_item_unref(playing);
    }

    deadbeef->pl_lock();
    ddb_playlist_t *plt = deadbeef->plt_get_curr();
    deadbeef->pl_unlock();
    if (!plt) {
        return 0;
    }
    deadbeef->pl_lock();
    DB_playItem_t *it = deadbeef->plt_get_first(plt, PL_MAIN);
    deadbeef->pl_unlock();
    int found = 0;
    int seen_playing = playing_uri[0] ? 0 : 1;
    while (it) {
        char copy[PATH_MAX] = {0};
        get_track_source_uri(it, copy, sizeof(copy));
        if (!seen_playing && copy[0] && !strcmp(copy, playing_uri)) {
            seen_playing = 1;
        }
        deadbeef->pl_lock();
        DB_playItem_t *next = deadbeef->pl_get_next(it, PL_MAIN);
        deadbeef->pl_unlock();
        deadbeef->pl_item_unref(it);
        it = next;
        if (seen_playing && copy[0] && !strcmp(copy, uri)) {
            found = 1;
            break;
        }
    }
    deadbeef->plt_unref(plt);
    return found;
}

static int
precache_make_room_or_stop(void) {
    cache_remove_unrelated_files();
    int limit_mb = deadbeef->conf_get_int("roformer.cache_limit_mb", CACHE_LIMIT_DEFAULT_MB);
    if (limit_mb < 1) {
        limit_mb = 1;
    }
    int64_t limit_bytes = (int64_t)limit_mb * 1024LL * 1024LL;
    char cache_dir[PATH_MAX];
    get_cache_dir(cache_dir, sizeof(cache_dir));
    if (!cache_dir[0]) {
        return 0;
    }

    while (1) {
        DIR *dir = opendir(cache_dir);
        if (!dir) {
            return errno == ENOENT ? 1 : 0;
        }
        int64_t total = 0;
        cache_entry_t oldest = {0};
        int have_oldest = 0;
        struct dirent *de;
        while ((de = readdir(dir)) != NULL) {
            if (de->d_name[0] == '.') {
                continue;
            }
            if (!strcmp(de->d_name, "index.txt")) {
                continue;
            }
            char full[PATH_MAX];
            if (safe_path_join2(full, sizeof(full), cache_dir, de->d_name) != 0) {
                continue;
            }
            struct stat st = {0};
            if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) {
                continue;
            }
            total += st.st_size;
            if (!have_oldest || st.st_mtime < oldest.mtime) {
                strncpy(oldest.path, full, sizeof(oldest.path) - 1);
                oldest.path[sizeof(oldest.path) - 1] = 0;
                oldest.mtime = st.st_mtime;
                oldest.size = st.st_size;
                have_oldest = 1;
            }
        }
        closedir(dir);

        if (total <= limit_bytes || !have_oldest) {
            return 1;
        }

        char oldest_uri[PATH_MAX] = {0};
        if (cache_index_find_uri_by_cache_file(oldest.path, oldest_uri, sizeof(oldest_uri)) == 0
            && oldest_uri[0]
            && uri_protected_from_precache_eviction(oldest_uri)) {
            trace("roformer: precache paused, oldest cache is at/after current playing entry: %s\n", oldest_uri);
            return 0;
        }

        if (unlink(oldest.path) == 0) {
            cache_index_rewrite_with_filter(oldest.path);
            trace("roformer: precache evicted oldest cache %s\n", oldest.path);
        }
        else {
            return 0;
        }
    }
}

static int
cache_has_complete_entry(const char *uri, int mode, int64_t mtime, int64_t size) {
    int sr = 0;
    int ch = 0;
    char cache_file[PATH_MAX];
    return cache_index_lookup(uri, mode, mtime, size, &sr, &ch, cache_file, sizeof(cache_file)) == 0;
}

static int
cache_index_lookup(const char *uri, int mode, int64_t mtime, int64_t size, int *samplerate, int *channels, char *cache_file_out, size_t out_sz) {
    char index_path[PATH_MAX];
    if (get_cache_index_path(index_path, sizeof(index_path)) != 0) {
        return -1;
    }
    FILE *fp = fopen(index_path, "r");
    if (!fp) {
        if (errno != ENOENT) {
            trace("roformer: index open failed path=%s errno=%d\n", index_path, errno);
        }
        return -1;
    }
    char line[PATH_MAX * 2];
    cache_index_entry_t e;
    int found = -1;
    while (fgets(line, sizeof(line), fp)) {
        if (index_parse_line(line, &e) != 0) {
            continue;
        }
        if (e.mode == mode && e.mtime == mtime && e.size == size && !strcmp(e.uri, uri)) {
            if (!e.complete) {
                continue;
            }
            if (!path_has_mp3_ext(e.cache_file)) {
                continue;
            }
            if (snprintf(cache_file_out, out_sz, "%s", e.cache_file) < 0) {
                fclose(fp);
                return -1;
            }
            *samplerate = e.samplerate;
            *channels = e.channels;
            found = 0;
            break;
        }
    }
    fclose(fp);
    if (found != 0) {
        trace("roformer: index miss mode=%d mtime=%lld size=%lld uri=%s\n",
            mode, (long long)mtime, (long long)size, uri ? uri : "(null)");
    }
    return found;
}

static void
cache_index_rewrite_with_filter(const char *remove_cache_path) {
    char index_path[PATH_MAX];
    char cache_dir[PATH_MAX];
    if (get_cache_index_path(index_path, sizeof(index_path)) != 0) {
        return;
    }
    get_cache_dir(cache_dir, sizeof(cache_dir));
    if (!cache_dir[0]) {
        return;
    }
    ensure_dir(cache_dir);
    char tmp_name[64];
    if (snprintf(tmp_name, sizeof(tmp_name), "index.txt.tmp.%d", (int)getpid()) < 0) {
        return;
    }
    char tmp_path[PATH_MAX];
    if (safe_path_join2(tmp_path, sizeof(tmp_path), cache_dir, tmp_name) != 0) {
        return;
    }

    FILE *in = fopen(index_path, "r");
    FILE *out = fopen(tmp_path, "w");
    if (!out) {
        if (in) fclose(in);
        return;
    }
    if (!in) {
        cache_index_write_header(out);
        fclose(out);
        rename(tmp_path, index_path);
        return;
    }

    cache_index_write_header(out);
    char line[PATH_MAX * 2];
    cache_index_entry_t e;
    while (fgets(line, sizeof(line), in)) {
        if (line[0] == '#') {
            continue;
        }
        char line_copy[PATH_MAX * 2];
        strncpy(line_copy, line, sizeof(line_copy) - 1);
        line_copy[sizeof(line_copy) - 1] = 0;
        if (index_parse_line(line_copy, &e) != 0) {
            continue;
        }
        if (remove_cache_path && !strcmp(e.cache_file, remove_cache_path)) {
            continue;
        }
        fputs(line, out);
    }
    fclose(in);
    fclose(out);
    if (rename(tmp_path, index_path) != 0) {
        unlink(tmp_path);
    }
}

static void
cache_index_upsert(const char *uri, int mode, int64_t mtime, int64_t size, int samplerate, int channels, int complete, const char *cache_file) {
    char cache_dir[PATH_MAX];
    get_cache_dir(cache_dir, sizeof(cache_dir));
    if (!cache_dir[0]) {
        return;
    }
    ensure_dir(cache_dir);
    cache_index_rewrite_with_filter(cache_file);
    char index_path[PATH_MAX];
    if (get_cache_index_path(index_path, sizeof(index_path)) != 0) {
        return;
    }
    FILE *fp = fopen(index_path, "a");
    if (!fp) {
        return;
    }
    struct stat st = {0};
    if (stat(index_path, &st) == 0 && st.st_size == 0) {
        cache_index_write_header(fp);
    }
    fprintf(fp, "%s\t%lld\t%lld\t%d\t%d\t%d\t%s\t%s\n",
        mode_to_token(mode), (long long)mtime, (long long)size, samplerate, channels, complete, cache_file, uri);
    fclose(fp);
    trace("roformer: index upsert mode=%d sr=%d ch=%d complete=%d mtime=%lld size=%lld cache=%s uri=%s\n",
        mode, samplerate, channels, complete, (long long)mtime, (long long)size, cache_file, uri);
}

static void
cache_index_prune_invalid(void) {
    char index_path[PATH_MAX];
    char cache_dir[PATH_MAX];
    if (get_cache_index_path(index_path, sizeof(index_path)) != 0) {
        return;
    }
    get_cache_dir(cache_dir, sizeof(cache_dir));
    if (!cache_dir[0]) {
        return;
    }
    FILE *in = fopen(index_path, "r");
    if (!in) {
        return;
    }
    char tmp_name[64];
    if (snprintf(tmp_name, sizeof(tmp_name), "index.txt.tmp.%d", (int)getpid()) < 0) {
        fclose(in);
        return;
    }
    char tmp_path[PATH_MAX];
    if (safe_path_join2(tmp_path, sizeof(tmp_path), cache_dir, tmp_name) != 0) {
        fclose(in);
        return;
    }
    FILE *out = fopen(tmp_path, "w");
    if (!out) {
        fclose(in);
        return;
    }

    cache_index_write_header(out);
    char line[PATH_MAX * 2];
    cache_index_entry_t e;
    while (fgets(line, sizeof(line), in)) {
        if (line[0] == '#') {
            continue;
        }
        char line_copy[PATH_MAX * 2];
        strncpy(line_copy, line, sizeof(line_copy) - 1);
        line_copy[sizeof(line_copy) - 1] = 0;
        if (index_parse_line(line_copy, &e) != 0) {
            continue;
        }
        struct stat st = {0};
        if (stat(e.cache_file, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        fputs(line, out);
    }
    fclose(in);
    fclose(out);
    if (rename(tmp_path, index_path) != 0) {
        unlink(tmp_path);
    }
}

static int
path_list_contains(char **paths, size_t npaths, const char *path) {
    if (!paths || !path) {
        return 0;
    }
    for (size_t i = 0; i < npaths; i++) {
        if (paths[i] && !strcmp(paths[i], path)) {
            return 1;
        }
    }
    return 0;
}

static void
cache_remove_unindexed_mp3s(void) {
    char cache_dir[PATH_MAX];
    get_cache_dir(cache_dir, sizeof(cache_dir));
    if (!cache_dir[0]) {
        return;
    }

    char index_path[PATH_MAX];
    if (get_cache_index_path(index_path, sizeof(index_path)) != 0) {
        return;
    }

    char **indexed = NULL;
    size_t nindexed = 0;
    size_t cap = 0;

    FILE *fp = fopen(index_path, "r");
    if (fp) {
        char line[PATH_MAX * 2];
        cache_index_entry_t e;
        while (fgets(line, sizeof(line), fp)) {
            if (index_parse_line(line, &e) != 0) {
                continue;
            }
            if (!path_has_mp3_ext(e.cache_file)) {
                continue;
            }
            if (nindexed == cap) {
                cap = cap ? cap * 2 : 32;
                char **tmp = realloc(indexed, cap * sizeof(char *));
                if (!tmp) {
                    break;
                }
                indexed = tmp;
            }
            indexed[nindexed++] = strdup(e.cache_file);
        }
        fclose(fp);
    }

    DIR *dir = opendir(cache_dir);
    if (dir) {
        struct dirent *de;
        while ((de = readdir(dir)) != NULL) {
            if (de->d_name[0] == '.') {
                continue;
            }
            if (!path_has_mp3_ext(de->d_name)) {
                continue;
            }
            char full[PATH_MAX];
            if (safe_path_join2(full, sizeof(full), cache_dir, de->d_name) != 0) {
                continue;
            }
            if (!path_list_contains(indexed, nindexed, full)) {
                trace("roformer: removing orphan cache %s\n", full);
                unlink(full);
            }
        }
        closedir(dir);
    }

    for (size_t i = 0; i < nindexed; i++) {
        free(indexed[i]);
    }
    free(indexed);
}

static int
path_has_suffix(const char *path, const char *suffix) {
    if (!path || !suffix) {
        return 0;
    }
    size_t n = strlen(path);
    size_t s = strlen(suffix);
    return n >= s && !strcmp(path + n - s, suffix);
}

static void
cache_remove_stale_temp_files(void) {
    char cache_dir[PATH_MAX];
    get_f32tmp_dir(cache_dir, sizeof(cache_dir));
    if (!cache_dir[0]) {
        return;
    }
    DIR *dir = opendir(cache_dir);
    if (!dir) {
        return;
    }
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') {
            continue;
        }
        if (!(path_has_suffix(de->d_name, ".f32tmp") || strstr(de->d_name, ".f32tmp.")
              || path_has_suffix(de->d_name, ".mp3tmp") || strstr(de->d_name, ".mp3tmp."))) {
            continue;
        }
        char full[PATH_MAX];
        if (safe_path_join2(full, sizeof(full), cache_dir, de->d_name) != 0) {
            continue;
        }
        struct stat st = {0};
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        if (unlink(full) == 0) {
            trace("roformer: removed stale temp cache %s\n", full);
        }
    }
    closedir(dir);
}

static int
find_existing_temp_raw_for_cache(const char *cache_final_path, char *out, size_t out_sz) {
    if (!cache_final_path || !cache_final_path[0] || !out || out_sz == 0) {
        return -1;
    }
    out[0] = 0;
    const char *slash = strrchr(cache_final_path, '/');
    if (!slash) {
        return -1;
    }
    char cache_dir[PATH_MAX] = {0};
    get_f32tmp_dir(cache_dir, sizeof(cache_dir));
    if (!cache_dir[0]) {
        return -1;
    }
    const char *base = slash + 1;

    char prefix[PATH_MAX] = {0};
    if (snprintf(prefix, sizeof(prefix), "%s.f32tmp.", base) < 0) {
        return -1;
    }

    DIR *dir = opendir(cache_dir);
    if (!dir) {
        return -1;
    }
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') {
            continue;
        }
        if (strncmp(de->d_name, prefix, strlen(prefix)) != 0) {
            continue;
        }
        char full[PATH_MAX];
        if (safe_path_join2(full, sizeof(full), cache_dir, de->d_name) != 0) {
            continue;
        }
        struct stat st = {0};
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        if (out && out_sz > 0) {
            strncpy(out, full, out_sz);
            out[out_sz - 1] = 0;
        }
        closedir(dir);
        return 0;
    }
    closedir(dir);
    return -1;
}

static void
cache_remove_dir_all(void) {
    char cache_dir[PATH_MAX];
    get_cache_dir(cache_dir, sizeof(cache_dir));
    remove_dir_all_files(cache_dir);
    char f32tmp_dir[PATH_MAX];
    get_f32tmp_dir(f32tmp_dir, sizeof(f32tmp_dir));
    remove_dir_all_files(f32tmp_dir);
}

static int
cache_entry_cmp(const void *a, const void *b) {
    const cache_entry_t *ea = a;
    const cache_entry_t *eb = b;
    if (ea->mtime < eb->mtime) {
        return -1;
    }
    if (ea->mtime > eb->mtime) {
        return 1;
    }
    return strcmp(ea->path, eb->path);
}

static void
cache_enforce_limit(void) {
    cache_remove_unrelated_files();
    int limit_mb = deadbeef->conf_get_int("roformer.cache_limit_mb", CACHE_LIMIT_DEFAULT_MB);
    if (limit_mb < 1) {
        limit_mb = 1;
    }
    int64_t limit_bytes = (int64_t)limit_mb * 1024LL * 1024LL;
    char cache_dir[PATH_MAX];
    get_cache_dir(cache_dir, sizeof(cache_dir));
    if (!cache_dir[0]) {
        return;
    }

    DIR *dir = opendir(cache_dir);
    if (!dir) {
        return;
    }

    cache_entry_t *entries = NULL;
    size_t nentries = 0;
    size_t cap = 0;
    int64_t total = 0;

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') {
            continue;
        }
        if (!strcmp(de->d_name, "index.txt")) {
            continue;
        }
        char full[PATH_MAX];
        if (safe_path_join2(full, sizeof(full), cache_dir, de->d_name) != 0) {
            continue;
        }
        struct stat st = {0};
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        if (nentries == cap) {
            cap = cap ? cap * 2 : 64;
            entries = realloc(entries, cap * sizeof(cache_entry_t));
            if (!entries) {
                closedir(dir);
                return;
            }
        }
        strncpy(entries[nentries].path, full, sizeof(entries[nentries].path));
        entries[nentries].path[sizeof(entries[nentries].path) - 1] = 0;
        entries[nentries].mtime = st.st_mtime;
        entries[nentries].size = st.st_size;
        total += st.st_size;
        nentries++;
    }
    closedir(dir);

    if (total <= limit_bytes || nentries == 0) {
        free(entries);
        return;
    }

    trace("roformer: cache over limit total=%lld limit=%lld, evicting oldest entries\n",
        (long long)total, (long long)limit_bytes);
    qsort(entries, nentries, sizeof(cache_entry_t), cache_entry_cmp);
    for (size_t i = 0; i < nentries && total > limit_bytes; i++) {
        if (unlink(entries[i].path) == 0) {
            cache_index_rewrite_with_filter(entries[i].path);
            total -= entries[i].size;
        }
    }
    free(entries);
}

static void
update_button_label(void) {
    if (!mode_button) {
        return;
    }
    gtk_button_set_label(GTK_BUTTON(mode_button), mode_emoji(roformer_get_mode()));
}

static void
apply_mode_to_track(DB_playItem_t *it, int mode) {
    deadbeef->pl_lock();
    const char *decoder = deadbeef->pl_find_meta(it, ":DECODER");
    const char *uri = deadbeef->pl_find_meta(it, ":URI");
    char decoder_copy[128] = {0};
    char uri_copy[PATH_MAX] = {0};
    if (decoder) {
        strncpy(decoder_copy, decoder, sizeof(decoder_copy) - 1);
    }
    if (uri) {
        strncpy(uri_copy, uri, sizeof(uri_copy) - 1);
    }
    deadbeef->pl_unlock();

    if (!uri_copy[0]) {
        return;
    }

    if (mode == ROFORMER_MODE_ORIGINAL) {
        deadbeef->pl_lock();
        const char *current_dec = deadbeef->pl_find_meta(it, ":DECODER");
        const char *orig_dec = deadbeef->pl_find_meta(it, ":ROFORMER_ORIG_DECODER");
        char orig_copy[128] = {0};
        char current_copy[128] = {0};
        if (orig_dec) {
            strncpy(orig_copy, orig_dec, sizeof(orig_copy) - 1);
        }
        if (current_dec) {
            strncpy(current_copy, current_dec, sizeof(current_copy) - 1);
        }
        deadbeef->pl_unlock();

        if (current_copy[0] && !strcmp(current_copy, plugin.plugin.id) && orig_copy[0]) {
            deadbeef->pl_replace_meta(it, ":DECODER", orig_copy);
        }
        deadbeef->pl_delete_meta(it, ":ROFORMER_ORIG_DECODER");
        deadbeef->pl_delete_meta(it, ":ROFORMER_SOURCE_URI");
        return;
    }

    if (decoder_copy[0] && strcmp(decoder_copy, plugin.plugin.id)) {
        deadbeef->pl_replace_meta(it, ":ROFORMER_ORIG_DECODER", decoder_copy);
    }
    deadbeef->pl_replace_meta(it, ":ROFORMER_SOURCE_URI", uri_copy);
    deadbeef->pl_replace_meta(it, ":DECODER", plugin.plugin.id);
}

static void
apply_mode_to_current_playlist(int mode) {
    deadbeef->pl_lock();
    ddb_playlist_t *plt = deadbeef->plt_get_curr();
    deadbeef->pl_unlock();
    if (!plt) {
        return;
    }

    deadbeef->pl_lock();
    DB_playItem_t *it = deadbeef->plt_get_first(plt, PL_MAIN);
    deadbeef->pl_unlock();
    while (it) {
        apply_mode_to_track(it, mode);
        deadbeef->pl_lock();
        DB_playItem_t *next = deadbeef->pl_get_next(it, PL_MAIN);
        deadbeef->pl_unlock();
        deadbeef->pl_item_unref(it);
        it = next;
    }
    deadbeef->plt_unref(plt);
    deadbeef->sendmessage(DB_EV_PLAYLISTCHANGED, 0, DDB_PLAYLIST_CHANGE_CONTENT, 0);
}

static void
restart_if_playing(void) {
    DB_output_t *output = deadbeef->get_output();
    if (!output) {
        return;
    }
    ddb_playback_state_t st = output->state();
    if (st == DDB_PLAYBACK_STATE_PLAYING || st == DDB_PLAYBACK_STATE_PAUSED) {
        deadbeef->sendmessage(DB_EV_PLAY_CURRENT, 0, 0, 0);
    }
}

static void
ensure_playing_track_mode(void) {
    int mode = roformer_get_mode();
    if (mode == ROFORMER_MODE_ORIGINAL) {
        return;
    }
    DB_playItem_t *it = deadbeef->streamer_get_playing_track_safe();
    if (!it) {
        return;
    }
    deadbeef->pl_lock();
    const char *decoder = deadbeef->pl_find_meta(it, ":DECODER");
    char decoder_copy[128] = {0};
    if (decoder) {
        strncpy(decoder_copy, decoder, sizeof(decoder_copy) - 1);
    }
    deadbeef->pl_unlock();
    if (!decoder_copy[0] || strcmp(decoder_copy, plugin.plugin.id)) {
        apply_mode_to_track(it, mode);
        deadbeef->sendmessage(DB_EV_PLAY_CURRENT, 0, 0, 0);
    }
    deadbeef->pl_item_unref(it);
}

static void
precache_single_track(DB_playItem_t *it, int mode) {
    if (!it || mode == ROFORMER_MODE_ORIGINAL) {
        return;
    }
    if (!precache_make_room_or_stop()) {
        return;
    }
    deadbeef->pl_lock();
    const char *uri = deadbeef->pl_find_meta(it, ":URI");
    char uri_copy[PATH_MAX] = {0};
    if (uri) {
        strncpy(uri_copy, uri, sizeof(uri_copy) - 1);
    }
    deadbeef->pl_unlock();
    if (!uri_copy[0]) {
        return;
    }
    int64_t mtime = 0;
    int64_t size = 0;
    get_uri_file_stat(uri_copy, &mtime, &size);
    if (cache_has_complete_entry(uri_copy, mode, mtime, size)) {
        return;
    }
    if (uri_is_currently_playing_or_loaded(uri_copy)) {
        return;
    }

    char cache_dir[PATH_MAX];
    get_cache_dir(cache_dir, sizeof(cache_dir));
    if (!cache_dir[0]) {
        return;
    }
    ensure_dir(cache_dir);
    char final_path[PATH_MAX];
    if (resolve_cache_final_path(it, mode, final_path, sizeof(final_path)) != 0) {
        return;
    }

    if (cache_has_complete_entry(uri_copy, mode, mtime, size)) {
        return;
    }
    if (uri_is_currently_playing_or_loaded(uri_copy)) {
        return;
    }

    char script[PATH_MAX];
    const char *script_key = mode == ROFORMER_MODE_VOCAL ? "roformer.vocal_separation_script" : "roformer.instrumental_separation_script";
    deadbeef->conf_get_str(script_key, "", script, sizeof(script));
    if (!script[0]) {
        return;
    }

    int sr = get_samplerate(it);
    int ch = get_channels(it);
    const char *stream_arg = mode == ROFORMER_MODE_VOCAL ? "--stream-f32le-vocal" : "--stream-f32le-instrumental";

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return;
    }

    trace("roformer: precache spawn inference: \"%s\" --input \"%s\" %s\n", script, uri_copy, stream_arg);

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return;
    }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        execlp(script, script, "--input", uri_copy, stream_arg, (char *)NULL);
        _exit(127);
    }

    close(pipefd[1]);

    SF_INFO out_sfinfo = {0};
    out_sfinfo.format = SF_FORMAT_MPEG | SF_FORMAT_MPEG_LAYER_III;
    out_sfinfo.samplerate = sr;
    out_sfinfo.channels = ch;
    SNDFILE *out = sf_open(final_path, SFM_WRITE, &out_sfinfo);
    if (!out) {
        trace("roformer: failed to open mp3 for precache: %s, error: %s\n", final_path, sf_strerror(NULL));
        close(pipefd[0]);
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        return;
    }

    float buf[8192];
    ssize_t got;
    size_t frame_sz = ch * 4;
    while ((got = read(pipefd[0], buf, sizeof(buf))) > 0) {
        if (uri_is_currently_playing_or_loaded(uri_copy)) {
            trace("roformer: precache stopped, track started playing: %s\n", uri_copy);
            close(pipefd[0]);
            kill(pid, SIGTERM);
            waitpid(pid, NULL, 0);
            sf_close(out);
            unlink(final_path);
            return;
        }
        sf_count_t frames = (sf_count_t)(got / frame_sz);
        if (frames > 0) {
            sf_writef_float(out, buf, frames);
        }
    }
    close(pipefd[0]);
    sf_close(out);

    int status = 0;
    waitpid(pid, &status, 0);
    if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
        trace("roformer: precache inference failed status=%d\n", WEXITSTATUS(status));
        unlink(final_path);
        return;
    }

    struct stat st = {0};
    if (stat(final_path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 64) {
        unlink(final_path);
        return;
    }
    cache_index_upsert(uri_copy, mode, mtime, size, sr, ch, 1, final_path);
    notify_waveform_refresh_for_source(uri_copy);
    (void)precache_make_room_or_stop();
}

static void
precache_worker(void *ctx) {
    (void)ctx;
    while (1) {
        precache_rescan = 0;
        int mode = roformer_get_mode();
        if (mode == ROFORMER_MODE_ORIGINAL) {
            break;
        }
        deadbeef->pl_lock();
        ddb_playlist_t *plt = deadbeef->plt_get_curr();
        deadbeef->pl_unlock();
        if (!plt) {
            break;
        }
        deadbeef->pl_lock();
        DB_playItem_t *it = deadbeef->plt_get_first(plt, PL_MAIN);
        deadbeef->pl_unlock();
        DB_playItem_t *playing = deadbeef->streamer_get_playing_track_safe();
        if (playing && it) {
            int matched = 0;
            while (it) {
                if (it == playing) {
                    matched = 1;
                    break;
                }
                deadbeef->pl_lock();
                DB_playItem_t *next_skip = deadbeef->pl_get_next(it, PL_MAIN);
                deadbeef->pl_unlock();
                deadbeef->pl_item_unref(it);
                it = next_skip;
            }
            if (!matched) {
                deadbeef->pl_lock();
                it = deadbeef->plt_get_first(plt, PL_MAIN);
                deadbeef->pl_unlock();
            }
        }
        if (playing) {
            deadbeef->pl_item_unref(playing);
        }
        while (it) {
            precache_single_track(it, mode);
            if (!precache_make_room_or_stop()) {
                deadbeef->pl_lock();
                DB_playItem_t *next_pause = deadbeef->pl_get_next(it, PL_MAIN);
                deadbeef->pl_unlock();
                deadbeef->pl_item_unref(it);
                it = next_pause;
                while (it) {
                    deadbeef->pl_lock();
                    DB_playItem_t *next_drop = deadbeef->pl_get_next(it, PL_MAIN);
                    deadbeef->pl_unlock();
                    deadbeef->pl_item_unref(it);
                    it = next_drop;
                }
                break;
            }
            deadbeef->pl_lock();
            DB_playItem_t *next = deadbeef->pl_get_next(it, PL_MAIN);
            deadbeef->pl_unlock();
            deadbeef->pl_item_unref(it);
            it = next;
            if (precache_rescan || roformer_get_mode() == ROFORMER_MODE_ORIGINAL) {
                while (it) {
                    deadbeef->pl_lock();
                    DB_playItem_t *next2 = deadbeef->pl_get_next(it, PL_MAIN);
                    deadbeef->pl_unlock();
                    deadbeef->pl_item_unref(it);
                    it = next2;
                }
                break;
            }
        }
        deadbeef->plt_unref(plt);
        if (!precache_rescan) {
            break;
        }
    }
    precache_running = 0;
}

static void
schedule_precache_rescan(void) {
    precache_rescan = 1;
    if (precache_running || roformer_get_mode() == ROFORMER_MODE_ORIGINAL) {
        return;
    }
    precache_running = 1;
    intptr_t tid = deadbeef->thread_start_low_priority(precache_worker, NULL);
    if (tid) {
        deadbeef->thread_detach(tid);
    }
    else {
        precache_running = 0;
    }
}

static int
cycle_mode(void) {
    int mode = roformer_get_mode();
    mode = (mode + 1) % 3;
    roformer_set_mode(mode);
    update_button_label();
    apply_mode_to_current_playlist(mode);
    restart_if_playing();
    schedule_precache_rescan();
    return mode;
}

static int
mode_action_cb(DB_plugin_action_t *act, ddb_action_context_t ctx) {
    (void)act;
    (void)ctx;
    cycle_mode();
    return 0;
}

static DB_plugin_action_t *
roformer_get_actions(DB_playItem_t *it) {
    (void)it;
    return &mode_action;
}

static GtkWidget *
find_statusbar(GtkWidget *root) {
    if (!root) {
        return NULL;
    }
    if (GTK_IS_STATUSBAR(root)) {
        return root;
    }
    if (!GTK_IS_CONTAINER(root)) {
        return NULL;
    }
    GList *children = gtk_container_get_children(GTK_CONTAINER(root));
    for (GList *l = children; l; l = l->next) {
        GtkWidget *w = find_statusbar(GTK_WIDGET(l->data));
        if (w) {
            g_list_free(children);
            return w;
        }
    }
    g_list_free(children);
    return NULL;
}

static void
on_mode_button_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    cycle_mode();
}

static void
roformer_window_init(void *userdata) {
    (void)userdata;
    if (!gtkui_api || mode_button) {
        return;
    }
    GtkWidget *mainwin = gtkui_api->get_mainwin();
    if (!mainwin) {
        return;
    }

    GtkWidget *statusbar = find_statusbar(mainwin);
    if (!statusbar) {
        return;
    }

    GtkWidget *msg_area = gtk_statusbar_get_message_area(GTK_STATUSBAR(statusbar));
    if (!msg_area || !GTK_IS_BOX(msg_area)) {
        return;
    }

    mode_button = gtk_button_new_with_label(mode_emoji(roformer_get_mode()));
    gtk_widget_set_can_focus(mode_button, FALSE);
    gtk_button_set_relief(GTK_BUTTON(mode_button), GTK_RELIEF_NONE);
    gtk_box_pack_end(GTK_BOX(msg_area), mode_button, FALSE, FALSE, 4);
    g_signal_connect(mode_button, "clicked", G_CALLBACK(on_mode_button_clicked), NULL);
    gtk_widget_show(mode_button);
}

static void
try_install_gui_button(void) {
    DB_plugin_t *gtk3 = deadbeef->plug_get_for_id("gtkui3_1");
    DB_plugin_t *gtk2 = deadbeef->plug_get_for_id("gtkui_1");
    DB_plugin_t *g = gtk3 ? gtk3 : gtk2;
    if (!g) {
        return;
    }
    gtkui_api = (ddb_gtkui_t *)g;
    if (gtkui_api->add_window_init_hook) {
        gtkui_api->add_window_init_hook(roformer_window_init, NULL);
    }
}

static int
get_samplerate(DB_playItem_t *it) {
    int sr = deadbeef->pl_find_meta_int(it, ":SAMPLERATE", 44100);
    if (sr <= 0) {
        sr = 44100;
    }
    return sr;
}

static int
get_channels(DB_playItem_t *it) {
    int ch = deadbeef->pl_find_meta_int(it, ":CHANNELS", 2);
    if (ch <= 0 || ch > 8) {
        ch = 2;
    }
    return ch;
}

static void
sanitize_filename_component(const char *in, char *out, size_t out_sz) {
    if (!out || out_sz == 0) {
        return;
    }
    out[0] = 0;
    if (!in || !in[0]) {
        return;
    }
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 1 < out_sz; i++) {
        unsigned char c = (unsigned char)in[i];
        if (isalnum(c) || c == '-' || c == '_' || c == ' ') {
            out[j++] = (char)c;
        }
        else {
            out[j++] = '_';
        }
    }
    while (j > 0 && (out[j - 1] == ' ' || out[j - 1] == '.')) {
        j--;
    }
    out[j] = 0;
}

static void
build_cache_filename(DB_playItem_t *it, int mode, char *name_out, size_t name_out_sz) {
    if (!name_out || name_out_sz == 0) {
        return;
    }
    name_out[0] = 0;
    if (!it) {
        return;
    }

    deadbeef->pl_lock();
    const char *uri = deadbeef->pl_find_meta(it, ":ROFORMER_SOURCE_URI");
    if (!uri) {
        uri = deadbeef->pl_find_meta(it, ":URI");
    }
    char uri_copy[PATH_MAX] = {0};
    if (uri) {
        strncpy(uri_copy, uri, sizeof(uri_copy) - 1);
    }
    deadbeef->pl_unlock();

    const char *path = uri_copy;
    if (!strncmp(path, "file://", 7)) {
        path += 7;
    }
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (!base || !base[0]) {
        base = "track";
    }

    char stem[PATH_MAX] = {0};
    strncpy(stem, base, sizeof(stem));
    stem[sizeof(stem) - 1] = 0;
    char *dot = strrchr(stem, '.');
    if (dot) {
        *dot = 0;
    }

    char safe_stem[PATH_MAX] = {0};
    sanitize_filename_component(stem, safe_stem, sizeof(safe_stem));
    if (!safe_stem[0]) {
        strncpy(safe_stem, "track", sizeof(safe_stem) - 1);
    }

    const char *suffix = mode == ROFORMER_MODE_VOCAL ? ".vocal.mp3" : ".instr.mp3";
    snprintf(name_out, name_out_sz, "%s%s", safe_stem, suffix);
}

static int
resolve_cache_final_path(DB_playItem_t *it, int mode, char *out, size_t out_sz) {
    if (!it || !out || out_sz == 0) {
        return -1;
    }
    out[0] = 0;
    char cache_dir[PATH_MAX];
    get_cache_dir(cache_dir, sizeof(cache_dir));
    if (!cache_dir[0]) {
        return -1;
    }
    char name[PATH_MAX];
    build_cache_filename(it, mode, name, sizeof(name));
    if (!name[0]) {
        return -1;
    }
    return safe_path_join2(out, out_sz, cache_dir, name);
}

static int
convert_raw_to_mp3(const char *raw_path, const char *mp3_path, int samplerate, int channels) {
    if (!raw_path || !raw_path[0] || !mp3_path || !mp3_path[0] || samplerate <= 0 || channels <= 0) {
        return -1;
    }

    SF_INFO in_sfinfo = {0};
    in_sfinfo.format = SF_FORMAT_RAW | SF_FORMAT_FLOAT;
    in_sfinfo.samplerate = samplerate;
    in_sfinfo.channels = channels;
    SNDFILE *in = sf_open(raw_path, SFM_READ, &in_sfinfo);
    if (!in) {
        trace("roformer: failed to open raw for conversion: %s, error: %s\n", raw_path, sf_strerror(NULL));
        return -1;
    }

    SF_INFO out_sfinfo = {0};
    out_sfinfo.format = SF_FORMAT_MPEG | SF_FORMAT_MPEG_LAYER_III;
    out_sfinfo.samplerate = samplerate;
    out_sfinfo.channels = channels;
    SNDFILE *out = sf_open(mp3_path, SFM_WRITE, &out_sfinfo);
    if (!out) {
        trace("roformer: failed to open mp3 for conversion: %s, error: %s\n", mp3_path, sf_strerror(NULL));
        sf_close(in);
        return -1;
    }

    float buf[8192];
    sf_count_t read_frames;
    while ((read_frames = sf_readf_float(in, buf, 8192 / channels)) > 0) {
        sf_writef_float(out, buf, read_frames);
    }

    sf_close(in);
    sf_close(out);
    return 0;
}

static int
spawn_inference(roformer_info_t *ri, const char *src_uri, int mode) {
    if (!src_uri || !src_uri[0]) {
        return -1;
    }

    const char *path = src_uri;
    if (!strncmp(src_uri, "file://", 7)) {
        path = src_uri + 7;
    }

    char script[PATH_MAX];
    const char *script_key = mode == ROFORMER_MODE_VOCAL ? "roformer.vocal_separation_script" : "roformer.instrumental_separation_script";
    deadbeef->conf_get_str(script_key, "", script, sizeof(script));
    if (!script[0]) {
        return -1;
    }

    const char *stream_arg = mode == ROFORMER_MODE_VOCAL ? "--stream-f32le-vocal" : "--stream-f32le-instrumental";
    trace("roformer: spawn inference cmd: \"%s\" --input \"%s\" %s\n",
        script, path, stream_arg);
    char cache_dir[PATH_MAX];
    get_cache_dir(cache_dir, sizeof(cache_dir));
    if (!cache_dir[0]) {
        return -1;
    }
    ensure_dir(cache_dir);

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        execlp(
            script, script,
            "--input", path,
            stream_arg,
            (char *)NULL
        );
        _exit(127);
    }

    close(pipefd[1]);
    ri->pipe_fd = pipefd[0];
    ri->child_pid = pid;
    int flags = fcntl(ri->pipe_fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(ri->pipe_fd, F_SETFL, flags | O_NONBLOCK);
    }
    trace("roformer: spawn success pid=%d pipe_fd=%d\n", (int)pid, ri->pipe_fd);

    const char *base = strrchr(ri->cache_final_path, '/');
    base = base ? base + 1 : ri->cache_final_path;
    char f32tmp_dir[PATH_MAX];
    get_f32tmp_dir(f32tmp_dir, sizeof(f32tmp_dir));
    if (!f32tmp_dir[0]) {
        return -1;
    }
    ensure_dir(f32tmp_dir);
    char raw_name[PATH_MAX];
    if (snprintf(raw_name, sizeof(raw_name), "%s.f32tmp.%d.%llx",
            base, (int)getpid(), (unsigned long long)(uintptr_t)ri) < 0) {
        return -1;
    }
    if (safe_path_join2(ri->cache_raw_path, sizeof(ri->cache_raw_path), f32tmp_dir, raw_name) != 0) {
        return -1;
    }
    ri->cache_fp_write = fopen(ri->cache_raw_path, "wb");
    if (!ri->cache_fp_write) {
        trace("roformer: cache open failed path=%s errno=%d\n", ri->cache_raw_path, errno);
        return 0;
    }
    ri->owns_temp_cache = 1;
    trace("roformer: cache write opened path=%s\n", ri->cache_raw_path);
    ri->cache_bytes_written = 0;
    ri->cache_data_offset = 0;
    ri->cache_frames_written = 0;
    if (!ri->cache_fp) {
        ri->cache_fp = fopen(ri->cache_raw_path, "rb");
        if (!ri->cache_fp) {
            trace("roformer: cache read open failed path=%s errno=%d\n", ri->cache_raw_path, errno);
        }
        else {
            ri->using_cache = 1;
        }
    }

    return 0;
}

static int
open_cache_if_available(roformer_info_t *ri, const char *cache_path) {
    struct stat st = {0};
    if (stat(cache_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return -1;
    }
    SF_INFO sfinfo = {0};
    SNDFILE *sfp = sf_open(cache_path, SFM_READ, &sfinfo);
    if (!sfp) {
        return -1;
    }
    if (sfinfo.samplerate <= 0 || sfinfo.channels <= 0 || sfinfo.channels > 8
        || sfinfo.samplerate != ri->samplerate || sfinfo.channels != ri->channels) {
        sf_close(sfp);
        return -1;
    }
    if (ri->cache_fp) {
        fclose(ri->cache_fp);
        ri->cache_fp = NULL;
    }
    ri->cache_snd = sfp;
    ri->cache_sfinfo = sfinfo;
    ri->using_cache = 1;
    ri->cache_bytes_written = st.st_size;
    ri->cache_data_offset = 0;
    ri->cache_frames_written = sfinfo.frames;
    ri->stream_finished = 1;
    ri->cache_valid = 1;
    trace("roformer: cache hit %s\n", cache_path);
    return 0;
}

static void
drain_pipe_to_cache(roformer_info_t *ri, int64_t min_total_bytes) {
    if (ri->pipe_fd < 0 || !ri->cache_fp_write || ri->stream_finished) {
        return;
    }
    uint8_t buf[65536];
    for (;;) {
        if (min_total_bytes >= 0 && ri->cache_bytes_written >= min_total_bytes) {
            struct pollfd pfd = {.fd = ri->pipe_fd, .events = POLLIN | POLLHUP | POLLERR, .revents = 0};
            int pr = poll(&pfd, 1, 0);
            if (pr <= 0 || !(pfd.revents & (POLLIN | POLLHUP | POLLERR))) {
                break;
            }
        }
        else {
            struct pollfd pfd = {.fd = ri->pipe_fd, .events = POLLIN | POLLHUP | POLLERR, .revents = 0};
            int pr = poll(&pfd, 1, 100);
            if (pr < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            if (pr == 0) {
                break;
            }
        }

        int got = (int)read(ri->pipe_fd, buf, sizeof(buf));
        if (got > 0) {
            fwrite(buf, 1, (size_t)got, ri->cache_fp_write);
            ri->cache_bytes_written += got;
            continue;
        }
        if (got == 0) {
            finish_stream(ri, 0);
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        trace("roformer: read pipe error errno=%d\n", errno);
        finish_stream(ri, 0);
        break;
    }
    fflush(ri->cache_fp_write);
}

static int
cache_file_plausible(roformer_info_t *ri, const char *cache_path) {
    struct stat st = {0};
    if (stat(cache_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return 0;
    }
    if (st.st_size < ri->bytes_per_frame) {
        return 0;
    }
    deadbeef->pl_lock();
    DB_playItem_t *curr = deadbeef->streamer_get_playing_track_safe();
    float dur = 0;
    if (curr) {
        dur = deadbeef->pl_get_item_duration(curr);
        deadbeef->pl_item_unref(curr);
    }
    deadbeef->pl_unlock();
    if (dur <= 0.0f) {
        return 1;
    }
    int64_t expected = (int64_t)(dur * (float)ri->samplerate * (float)ri->bytes_per_frame * 0.25f);
    if (expected < ri->bytes_per_frame) {
        expected = ri->bytes_per_frame;
    }
    return st.st_size >= expected;
}

static void
notify_waveform_refresh_for_source(const char *source_uri) {
    if (!source_uri || !source_uri[0]) {
        return;
    }
    DB_playItem_t *playing = deadbeef->streamer_get_playing_track_safe();
    if (!playing) {
        return;
    }
    char playing_src[PATH_MAX] = {0};
    get_track_source_uri(playing, playing_src, sizeof(playing_src));
    if (playing_src[0] && !strcmp(playing_src, source_uri)) {
        deadbeef->pl_replace_meta(playing, ":WAVEFORM_FORCED_REGEN_TOKEN", "1");
        ddb_event_track_t *ev = (ddb_event_track_t *)deadbeef->event_alloc(DB_EV_TRACKINFOCHANGED);
        if (ev) {
            ev->track = playing;
            deadbeef->pl_item_ref(ev->track);
            deadbeef->event_send((ddb_event_t *)ev, 0, 0);
        }
    }
    deadbeef->pl_item_unref(playing);
}

static DB_fileinfo_t *
roformer_open(uint32_t hints) {
    (void)hints;
    roformer_info_t *ri = calloc(1, sizeof(roformer_info_t));
    if (!ri) {
        return NULL;
    }
    ri->pipe_fd = -1;
    return &ri->info;
}

static int
roformer_init(DB_fileinfo_t *_info, DB_playItem_t *it) {
    roformer_info_t *ri = (roformer_info_t *)_info;
    ri->track = it;
    deadbeef->pl_item_ref(it);
    ri->mode = roformer_get_mode();
    if (ri->mode == ROFORMER_MODE_ORIGINAL) {
        return -1;
    }

    char cache_dir[PATH_MAX];
    get_cache_dir(cache_dir, sizeof(cache_dir));
    if (!cache_dir[0]) {
        return -1;
    }
    ensure_dir(cache_dir);

    deadbeef->pl_lock();
    const char *src = deadbeef->pl_find_meta(it, ":ROFORMER_SOURCE_URI");
    if (!src) {
        src = deadbeef->pl_find_meta(it, ":URI");
    }
    if (src) {
        strncpy(ri->source_uri, src, sizeof(ri->source_uri) - 1);
    }
    deadbeef->pl_unlock();
    if (!ri->source_uri[0]) {
        trace("roformer: init failed no source uri\n");
        return -1;
    }
    get_uri_file_stat(ri->source_uri, &ri->source_mtime, &ri->source_size);
    trace("roformer: init mode=%d uri=%s mtime=%lld size=%lld\n",
        ri->mode, ri->source_uri, (long long)ri->source_mtime, (long long)ri->source_size);

    if (cache_index_lookup(
            ri->source_uri,
            ri->mode,
            ri->source_mtime,
            ri->source_size,
            &ri->samplerate,
            &ri->channels,
            ri->cache_final_path,
            sizeof(ri->cache_final_path)) == 0) {
        trace("roformer: index hit %s\n", ri->cache_final_path);
    }
    else {
        ri->samplerate = get_samplerate(it);
        ri->channels = get_channels(it);
        if (resolve_cache_final_path(it, ri->mode, ri->cache_final_path, sizeof(ri->cache_final_path)) != 0) {
            return -1;
        }
    }
    ri->bytes_per_frame = ri->channels * 4;
    trace("roformer: init format sr=%d ch=%d bpf=%d cache_final=%s\n",
        ri->samplerate, ri->channels, ri->bytes_per_frame, ri->cache_final_path);

    if (open_cache_if_available(ri, ri->cache_final_path) != 0 || !cache_file_plausible(ri, ri->cache_final_path)) {
        char existing_raw[PATH_MAX] = {0};
        if (find_existing_temp_raw_for_cache(ri->cache_final_path, existing_raw, sizeof(existing_raw)) == 0) {
            ri->need_spawn = 0;
            ri->using_cache = 1;
            ri->cache_data_offset = 0;
            if (ri->cache_fp) {
                fclose(ri->cache_fp);
                ri->cache_fp = NULL;
            }
            ri->cache_fp = fopen(existing_raw, "rb");
            if (ri->cache_fp) {
                struct stat rst = {0};
                if (stat(existing_raw, &rst) == 0 && S_ISREG(rst.st_mode)) {
                    ri->cache_bytes_written = rst.st_size;
                }
                trace("roformer: reusing active temp cache %s for %s\n", existing_raw, ri->cache_final_path);
                _info->plugin = &plugin;
                _info->fmt.channels = ri->channels;
                _info->fmt.bps = 32;
                _info->fmt.is_float = 1;
                _info->fmt.samplerate = ri->samplerate;
                _info->fmt.channelmask = ri->channels == 1 ? DDB_SPEAKER_FRONT_LEFT : (DDB_SPEAKER_FRONT_LEFT | DDB_SPEAKER_FRONT_RIGHT);
                _info->readpos = 0;
                cache_enforce_limit();
                return 0;
            }
        }
        if (ri->cache_fp) {
            fclose(ri->cache_fp);
            ri->cache_fp = NULL;
            ri->using_cache = 0;
        }
        if (ri->cache_snd) {
            sf_close(ri->cache_snd);
            ri->cache_snd = NULL;
        }
        trace("roformer: cache miss %s\n", ri->cache_final_path);
        if (!access(ri->cache_final_path, F_OK)) {
            trace("roformer: preserving existing partial cache %s\n", ri->cache_final_path);
        }
        ri->need_spawn = 1;
        trace("roformer: defer spawn need_spawn=1\n");
    }

    _info->plugin = &plugin;
    _info->fmt.channels = ri->channels;
    _info->fmt.bps = 32;
    _info->fmt.is_float = 1;
    _info->fmt.samplerate = ri->samplerate;
    _info->fmt.channelmask = ri->channels == 1 ? DDB_SPEAKER_FRONT_LEFT : (DDB_SPEAKER_FRONT_LEFT | DDB_SPEAKER_FRONT_RIGHT);
    _info->readpos = 0;
    cache_enforce_limit();
    return 0;
}

static void
finish_stream(roformer_info_t *ri, int aborted) {
    if (ri->stream_finished) {
        return;
    }
    ri->stream_finished = 1;

    if (ri->pipe_fd >= 0) {
        close(ri->pipe_fd);
        ri->pipe_fd = -1;
    }

    int child_ok = 1;
    if (ri->child_pid > 0) {
        int status = 0;
        waitpid(ri->child_pid, &status, 0);
        child_ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        trace("roformer: child exit pid=%d status=%d exited=%d code=%d signaled=%d sig=%d\n",
            (int)ri->child_pid,
            status,
            WIFEXITED(status),
            WIFEXITED(status) ? WEXITSTATUS(status) : -1,
            WIFSIGNALED(status),
            WIFSIGNALED(status) ? WTERMSIG(status) : -1);
        ri->child_pid = 0;
    }

    if (ri->cache_fp_write) {
        fclose(ri->cache_fp_write);
        ri->cache_fp_write = NULL;

        int convert_ok = !aborted && child_ok;
        char mp3_tmp_path[PATH_MAX] = {0};
        if (convert_ok) {
            if (snprintf(mp3_tmp_path, sizeof(mp3_tmp_path), "%s.mp3tmp.%d.%llx",
                    ri->cache_final_path, (int)getpid(), (unsigned long long)(uintptr_t)ri) < 0) {
                convert_ok = 0;
            }
        }

        if (convert_ok) {
            convert_ok = convert_raw_to_mp3(ri->cache_raw_path, mp3_tmp_path, ri->samplerate, ri->channels) == 0;
            if (!convert_ok) {
                trace("roformer: failed to convert raw to mp3 %s -> %s\n", ri->cache_raw_path, mp3_tmp_path);
            }
        }

        int finalized = 0;
        int keep_raw_for_playback = 0;
        if (convert_ok) {
            struct stat st = {0};
            int have_size = stat(mp3_tmp_path, &st) == 0 && S_ISREG(st.st_mode);
            int enough_data = have_size && st.st_size >= 64;
            if (enough_data && rename(mp3_tmp_path, ri->cache_final_path) == 0) {
                finalized = 1;
            }
            else {
                if (!enough_data) {
                    trace("roformer: mp3 temp too small for finalize %s\n", mp3_tmp_path);
                }
                else {
                    trace("roformer: failed to rename %s -> %s errno=%d\n", mp3_tmp_path, ri->cache_final_path, errno);
                }
                unlink(mp3_tmp_path);
                convert_ok = 0;
                if (!aborted && child_ok) {
                    keep_raw_for_playback = 1;
                }
            }
        }
        else if (!aborted && child_ok) {
            keep_raw_for_playback = 1;
        }

        if (!keep_raw_for_playback) {
            if (ri->owns_temp_cache && ri->cache_raw_path[0] && unlink(ri->cache_raw_path) != 0 && errno != ENOENT) {
                trace("roformer: failed to remove temp cache %s errno=%d\n", ri->cache_raw_path, errno);
            }
            ri->owns_temp_cache = 0;
            ri->cache_raw_path[0] = 0;
        }
        if (ri->cache_snd) {
            sf_close(ri->cache_snd);
            ri->cache_snd = NULL;
        }

        if (!finalized && !keep_raw_for_playback && open_cache_if_available(ri, ri->cache_final_path) == 0) {
            finalized = 1;
        }
        if (keep_raw_for_playback) {
            if (ri->cache_fp) {
                fclose(ri->cache_fp);
                ri->cache_fp = NULL;
            }
            if (ri->cache_raw_path[0]) {
                ri->cache_fp = fopen(ri->cache_raw_path, "rb");
                if (ri->cache_fp) {
                    struct stat rst = {0};
                    if (stat(ri->cache_raw_path, &rst) == 0 && S_ISREG(rst.st_mode)) {
                        ri->cache_bytes_written = rst.st_size;
                    }
                    ri->using_cache = 1;
                }
            }
        }
        if (finalized) {
            if (ri->cache_fp) {
                fclose(ri->cache_fp);
                ri->cache_fp = NULL;
            }
            if (ri->owns_temp_cache && ri->cache_raw_path[0] && unlink(ri->cache_raw_path) != 0 && errno != ENOENT) {
                trace("roformer: failed to remove temp cache %s errno=%d\n", ri->cache_raw_path, errno);
            }
            ri->owns_temp_cache = 0;
            ri->cache_raw_path[0] = 0;
            ri->cache_valid = 1;
            ri->cache_data_offset = 0;
            cache_index_upsert(
                ri->source_uri,
                ri->mode,
                ri->source_mtime,
                ri->source_size,
                ri->samplerate,
                ri->channels,
                1,
                ri->cache_final_path);
            trace("roformer: cache finalized %s\n", ri->cache_final_path);
        }
        if (!aborted && child_ok) {
            notify_waveform_refresh_for_source(ri->source_uri);
        }
        if (!finalized) {
            if (mp3_tmp_path[0] && unlink(mp3_tmp_path) != 0 && errno != ENOENT) {
                trace("roformer: failed to remove mp3 temp %s errno=%d\n", mp3_tmp_path, errno);
            }
            trace("roformer: cache stop aborted=%d child_ok=%d convert_ok=%d keep_raw=%d cache=%s\n",
                aborted, child_ok, convert_ok, keep_raw_for_playback, ri->cache_final_path);
        }
    }
    cache_enforce_limit();
}

static void
roformer_free(DB_fileinfo_t *_info) {
    roformer_info_t *ri = (roformer_info_t *)_info;
    if (!ri) {
        return;
    }

    trace("roformer: free need_spawn=%d using_cache=%d stream_finished=%d data_bytes=%lld child_pid=%d\n",
        ri->need_spawn, ri->using_cache, ri->stream_finished, (long long)ri->data_bytes_read, (int)ri->child_pid);
    if (ri->cache_fp) {
        fclose(ri->cache_fp);
        ri->cache_fp = NULL;
    }
    if (ri->cache_snd) {
        sf_close(ri->cache_snd);
        ri->cache_snd = NULL;
    }

    if (ri->child_pid > 0) {
        kill(ri->child_pid, SIGTERM);
    }
    finish_stream(ri, 1);
    if (ri->owns_temp_cache && ri->cache_raw_path[0]) {
        if (unlink(ri->cache_raw_path) != 0 && errno != ENOENT) {
            trace("roformer: failed to remove temp cache on free %s errno=%d\n", ri->cache_raw_path, errno);
        }
    }
    ri->owns_temp_cache = 0;
    ri->cache_raw_path[0] = 0;
    if (ri->track) {
        deadbeef->pl_item_unref(ri->track);
        ri->track = NULL;
    }
    free(ri);
}

static int
roformer_read(DB_fileinfo_t *_info, char *buffer, int nbytes) {
    roformer_info_t *ri = (roformer_info_t *)_info;
    if (nbytes <= 0) {
        return 0;
    }
    int aligned = nbytes - (nbytes % ri->bytes_per_frame);
    if (aligned <= 0) {
        return 0;
    }

    int read_bytes = 0;
    if (ri->need_spawn && ri->pipe_fd < 0 && !ri->stream_finished) {
            if (ri->cache_raw_path[0] || !ri->owns_temp_cache) {
                char existing_raw[PATH_MAX] = {0};
                if (find_existing_temp_raw_for_cache(ri->cache_final_path, existing_raw, sizeof(existing_raw)) == 0) {
                    if (ri->cache_fp) {
                        fclose(ri->cache_fp);
                        ri->cache_fp = NULL;
                    }
                    ri->cache_fp = fopen(existing_raw, "rb");
                    if (ri->cache_fp) {
                        ri->need_spawn = 0;
                        ri->using_cache = 1;
                        struct stat rst = {0};
                        if (stat(existing_raw, &rst) == 0 && S_ISREG(rst.st_mode)) {
                            ri->cache_bytes_written = rst.st_size;
                        }
                        trace("roformer: using existing active temp cache %s\n", existing_raw);
                    }
                }
            }
    }
    if (ri->need_spawn && ri->pipe_fd < 0 && !ri->stream_finished) {
            DB_output_t *output = deadbeef->get_output();
            ddb_playback_state_t st = output ? output->state() : DDB_PLAYBACK_STATE_STOPPED;
            DB_playItem_t *playing = deadbeef->streamer_get_playing_track_safe();
            int is_active_playback = 0;
            char playing_src_copy[PATH_MAX] = {0};
            if ((st == DDB_PLAYBACK_STATE_PLAYING || st == DDB_PLAYBACK_STATE_PAUSED) && playing) {
                deadbeef->pl_lock();
                const char *playing_src = deadbeef->pl_find_meta(playing, ":ROFORMER_SOURCE_URI");
                if (!playing_src) {
                    playing_src = deadbeef->pl_find_meta(playing, ":URI");
                }
                if (playing_src) {
                    strncpy(playing_src_copy, playing_src, sizeof(playing_src_copy) - 1);
                }
                deadbeef->pl_unlock();
                if (playing_src_copy[0] && !strcmp(playing_src_copy, ri->source_uri)) {
                    is_active_playback = 1;
                }
            }
            if (playing) {
                deadbeef->pl_item_unref(playing);
            }
            if (!is_active_playback) {
                trace("roformer: suppress spawn st=%d active=%d src=%s playing_src=%s\n",
                    st, is_active_playback, ri->source_uri, playing_src_copy[0] ? playing_src_copy : "(none)");
                return 0;
            }
            if (spawn_inference(ri, ri->source_uri, ri->mode) != 0) {
                trace("roformer: failed to spawn inference\n");
                return 0;
            }
            ri->need_spawn = 0;
            trace("roformer: spawn consumed need_spawn=0\n");
    }

    if (ri->pipe_fd >= 0) {
        int64_t target = ri->cache_data_offset + ri->data_bytes_read + ri->bytes_per_frame;
        drain_pipe_to_cache(ri, target);
        if (!ri->stream_finished && ri->cache_bytes_written <= ri->cache_data_offset + ri->data_bytes_read) {
            int tries = 0;
            while (!ri->stream_finished && ri->cache_bytes_written <= ri->cache_data_offset + ri->data_bytes_read && tries++ < 200) {
                struct pollfd pfd = {.fd = ri->pipe_fd, .events = POLLIN | POLLHUP | POLLERR, .revents = 0};
                int pr = poll(&pfd, 1, 50);
                if (pr < 0 && errno != EINTR) {
                    break;
                }
                drain_pipe_to_cache(ri, ri->cache_data_offset + ri->data_bytes_read + ri->bytes_per_frame);
                if (pr == 0 && ri->cache_bytes_written <= ri->cache_data_offset + ri->data_bytes_read) {
                    continue;
                }
            }
        }
    }

    if (ri->stream_finished) {
        if (!ri->cache_snd && ri->cache_valid && ri->cache_final_path[0]) {
            open_cache_if_available(ri, ri->cache_final_path);
        }
        if (ri->cache_snd) {
            sf_count_t frame = (sf_count_t)(ri->data_bytes_read / ri->bytes_per_frame);
            if (sf_seek(ri->cache_snd, frame, SEEK_SET) >= 0) {
                sf_count_t want_frames = aligned / ri->bytes_per_frame;
                sf_count_t got_frames = sf_readf_float(ri->cache_snd, (float *)buffer, want_frames);
                if (got_frames > 0) {
                    read_bytes = (int)(got_frames * ri->bytes_per_frame);
                }
            }
        }
        else if (ri->cache_fp) {
            int64_t avail = ri->cache_bytes_written - (ri->cache_data_offset + ri->data_bytes_read);
            if (avail > 0) {
                int to_read = aligned;
                if ((int64_t)to_read > avail) {
                    to_read = (int)(avail - (avail % ri->bytes_per_frame));
                }
                if (to_read > 0) {
                    clearerr(ri->cache_fp);
                    if (fseeko(ri->cache_fp, (off_t)(ri->cache_data_offset + ri->data_bytes_read), SEEK_SET) == 0) {
                        read_bytes = (int)fread(buffer, 1, (size_t)to_read, ri->cache_fp);
                    }
                }
            }
        }
    }
    else if (ri->cache_fp) {
        int64_t avail = ri->cache_bytes_written - (ri->cache_data_offset + ri->data_bytes_read);
        if (avail > 0) {
            int to_read = aligned;
            if ((int64_t)to_read > avail) {
                to_read = (int)(avail - (avail % ri->bytes_per_frame));
            }
            if (to_read > 0) {
                clearerr(ri->cache_fp);
                if (fseeko(ri->cache_fp, (off_t)(ri->cache_data_offset + ri->data_bytes_read), SEEK_SET) == 0) {
                    read_bytes = (int)fread(buffer, 1, (size_t)to_read, ri->cache_fp);
                }
            }
        }
    }

    read_bytes -= (read_bytes % ri->bytes_per_frame);
    if (read_bytes <= 0) {
        return 0;
    }

    ri->data_bytes_read += read_bytes;
    _info->readpos = (float)ri->data_bytes_read / (float)(ri->samplerate * ri->bytes_per_frame);
    return read_bytes;
}

static int
roformer_seek(DB_fileinfo_t *_info, float time) {
    roformer_info_t *ri = (roformer_info_t *)_info;
    if (time < 0) {
        time = 0;
    }
    int64_t frame = (int64_t)(time * ri->samplerate);
    return roformer_seek_sample(_info, (int)frame);
}

static int
roformer_seek_sample(DB_fileinfo_t *_info, int sample) {
    roformer_info_t *ri = (roformer_info_t *)_info;
    if (sample < 0) {
        sample = 0;
    }
    if (!ri->stream_finished) {
        return sample == 0 ? 0 : -1;
    }
    if (!ri->using_cache || (!ri->cache_fp && !ri->cache_snd)) {
        if (ri->stream_finished && ri->cache_valid && !ri->using_cache && ri->cache_final_path[0]) {
            if (open_cache_if_available(ri, ri->cache_final_path) != 0) {
                return sample == 0 ? 0 : -1;
            }
        }
        else {
            // only support seek to start while streaming
            return sample == 0 ? 0 : -1;
        }
    }
    if (ri->cache_snd) {
        if (sf_seek(ri->cache_snd, (sf_count_t)sample, SEEK_SET) < 0) {
            return -1;
        }
    }
    else {
        off_t off = (off_t)ri->cache_data_offset + (off_t)sample * (off_t)ri->bytes_per_frame;
        if (fseeko(ri->cache_fp, off, SEEK_SET) != 0) {
            return -1;
        }
    }
    ri->data_bytes_read = (int64_t)sample * ri->bytes_per_frame;
    _info->readpos = (float)sample / (float)ri->samplerate;
    return 0;
}

DB_plugin_t *
roformer_load(DB_functions_t *api) {
    deadbeef = api;
    return DB_PLUGIN(&plugin);
}

static int
roformer_start(void) {
    char cache_dir[PATH_MAX];
    get_cache_dir(cache_dir, sizeof(cache_dir));
    if (cache_dir[0]) {
        ensure_dir(cache_dir);
    }
    roformer_set_mode(ROFORMER_MODE_ORIGINAL);
    if (deadbeef->conf_get_int("roformer.trace", 0)) {
        plugin.plugin.flags |= DDB_PLUGIN_FLAG_LOGGING;
    }
    else {
        plugin.plugin.flags &= ~DDB_PLUGIN_FLAG_LOGGING;
    }
    cache_enforce_limit();
    cache_index_prune_invalid();
    cache_remove_unindexed_mp3s();
    cache_remove_stale_temp_files();
    try_install_gui_button();
    update_button_label();
    int mode = roformer_get_mode();
    if (mode != ROFORMER_MODE_ORIGINAL) {
        apply_mode_to_current_playlist(mode);
    }
    schedule_precache_rescan();
    return 0;
}

static int
roformer_message(uint32_t id, uintptr_t ctx, uint32_t p1, uint32_t p2) {
    (void)ctx;
    (void)p1;
    (void)p2;
    if (id == DB_EV_PLUGINSLOADED || id == DB_EV_CONFIGCHANGED) {
        if (deadbeef->conf_get_int("roformer.trace", 0)) {
            plugin.plugin.flags |= DDB_PLUGIN_FLAG_LOGGING;
        }
        else {
            plugin.plugin.flags &= ~DDB_PLUGIN_FLAG_LOGGING;
        }
        try_install_gui_button();
        update_button_label();
        schedule_precache_rescan();
    }
    if (id == DB_EV_SONGSTARTED) {
        ensure_playing_track_mode();
        schedule_precache_rescan();
    }
    if (id == DB_EV_PLAYLISTCHANGED) {
        schedule_precache_rescan();
    }
    return 0;
}

static int
roformer_stop(void) {
    cache_remove_dir_all();
    mode_button = NULL;
    gtkui_api = NULL;
    return 0;
}

static const char *exts[] = {"wav", "flac", "mp3", NULL};

static const char settings_dlg[] =
    "property \"Vocal Separation Script\" file roformer.vocal_separation_script \"\";\n"
    "property \"Instrumental Separation Script\" file roformer.instrumental_separation_script \"\";\n"
    "property \"Cache size limit (MB)\" entry roformer.cache_limit_mb 1024;\n"
    "property \"Enable verbose logging\" checkbox roformer.trace 0;\n";


static DB_plugin_action_t mode_action = {
    .title = "Playback/Toggle Roformer Mode",
    .name = "roformer_toggle_mode",
    .flags = DB_ACTION_COMMON,
    .callback2 = mode_action_cb,
    .next = NULL
};

static DB_decoder_t plugin = {
    DDB_PLUGIN_SET_API_VERSION
    .plugin.version_major = 1,
    .plugin.version_minor = 0,
    .plugin.type = DB_PLUGIN_DECODER,
    .plugin.id = "roformer",
    .plugin.name = "Roformer Source Separation",
    .plugin.descr = "Separates vocals/instrumentals",
    .plugin.copyright =
        "Roformer plugin for DeaDBeeF\n"
        "Copyright (C) 2026\n",
    .plugin.website = "https://github.com/DeaDBeeF-Player/deadbeef",
    .plugin.start = roformer_start,
    .plugin.stop = roformer_stop,
    .plugin.message = roformer_message,
    .plugin.configdialog = settings_dlg,
    .plugin.get_actions = roformer_get_actions,
    .open = roformer_open,
    .init = roformer_init,
    .free = roformer_free,
    .read = roformer_read,
    .seek = roformer_seek,
    .seek_sample = roformer_seek_sample,
    .insert = NULL,
    .exts = exts,
};
