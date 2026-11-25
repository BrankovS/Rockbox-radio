/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "plugin.h"

#define BATCH_SIZE 24
#define MAX_DELAY 30
#define RADIO_DIR PLUGIN_APPS_DATA_DIR "/rockbox-radio"
#define CONFIG_PATH RADIO_DIR "/settings"
#define CATALOG_PATH RADIO_DIR "/catalog.tmp"
#define HEADER LCD_RGBPACK(12, 21, 20)
#define BLACK LCD_RGBPACK(5, 12, 11)
#define GREEN LCD_RGBPACK(130, 239, 164)
#define AMBER LCD_RGBPACK(255, 184, 86)
#define PALE LCD_RGBPACK(223, 240, 217)
#define DIM LCD_RGBPACK(121, 159, 145)
#define LINE LCD_RGBPACK(39, 76, 61)

enum band { RANDOM_FM, ERA_70, ERA_80, ERA_90, ERA_00, ERA_10, NINETIES, BAND_COUNT };
enum radio_screen { RADIO, STATIONS, ERAS, SETTINGS };
enum setting {
    SURPRISE, DELAY_MIN, DELAY_MAX, RANDOM_POSITION, REMEMBER, FULL_CYCLE,
    DIFFERENT_ARTIST, SAVED_BAND, SAVED_ERA, SETTING_COUNT
};

static const char *const era_label[] = {"1970s", "1980s", "1990s", "2000s", "2010s"};
static const char *const station_label[] = {"RANDOM FM", "CUSTOM ERA FM", "90s & 00s FM"};
static const char *const setting_label[] = {
    "Surprise Start", "Minimum delay", "Maximum delay", "Random position",
    "Remember last station", "Repeat after full cycle", "Separate same artist"
};
static const char *const queue_files[] = {"queue-a.m3u8", "queue-b.m3u8"};
static const uint8_t defaults[] = {1, 0, 8, 0, 1, 1, 1, RANDOM_FM, ERA_90};
static uint8_t settings[SETTING_COUNT];

struct library_track {
    uint32_t offset;
    uint32_t artist;
    uint8_t bands;
    uint8_t played;
};

struct station_queue {
    uint16_t track[BATCH_SIZE];
    int length;
    int next;
    int last;
    int failed_batches;
};

static struct station_queue queues[BAND_COUNT];
static struct library_track *library;
static uint16_t *candidates;
static unsigned int capacity, library_count;
static int catalog = -1;
static struct tagcache_search search;
static struct mp3entry probe;
static bool catalog_ready;
static bool usb;
static bool dirty;
static bool repeat_overridden;
static int saved_repeat;
static int active = -1;
static int pending = -1;
static bool pending_surprise;
static long tune_deadline;
static long play_deadline;
static bool batch_has_played;
static int position;
static enum radio_screen screen;
static int selected;
static const char *message;
static long message_until;
static uint32_t random_state;
static char path[MAX_PATH];
static char title[MAX_PATH], artist[MAX_PATH], album[MAX_PATH], now_path[MAX_PATH];
static unsigned long elapsed, duration;
static int year;
static long track_since;

static uint32_t random_number(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

static unsigned int pick(unsigned int count)
{
    return random_number() % count;
}

static void load_settings(void)
{
    uint8_t data[4 + SETTING_COUNT];
    rb->memcpy(settings, defaults, sizeof(settings));
    int fd = rb->open(CONFIG_PATH, O_RDONLY);

    if (fd < 0)
        return;
    ssize_t size = rb->read(fd, data, sizeof(data));
    rb->close(fd);
    if (size != sizeof(data) || rb->memcmp(data, "RBR1", 4))
        return;

    for (int i = 0; i < SETTING_COUNT; ++i)
    {
        int value = data[4 + i];
        if (i == DELAY_MIN || i == DELAY_MAX)
            settings[i] = MIN(value, MAX_DELAY);
        else if (i == SAVED_BAND && value < BAND_COUNT)
            settings[i] = value;
        else if (i == SAVED_ERA && value >= ERA_70 && value <= ERA_10)
            settings[i] = value;
        else if (i < SAVED_BAND && value <= 1)
            settings[i] = value;
    }
    settings[DELAY_MAX] = MAX(settings[DELAY_MAX], settings[DELAY_MIN]);
}

static bool save_settings(void)
{
    if (!dirty)
        return true;
    uint8_t data[4 + SETTING_COUNT];
    rb->memcpy(data, "RBR1", 4);
    rb->memcpy(data + 4, settings, sizeof(settings));

    int fd = rb->open(CONFIG_PATH ".tmp", O_CREAT | O_TRUNC | O_WRONLY, 0666);
    if (fd < 0)
        return false;
    bool ok = rb->write(fd, data, sizeof(data)) == sizeof(data);
    if (rb->close(fd) < 0)
        ok = false;
    if (ok && rb->rename(CONFIG_PATH ".tmp", CONFIG_PATH) == 0)
    {
        dirty = false;
        return true;
    }
    rb->remove(CONFIG_PATH ".tmp");
    return false;
}

static void notice(const char *text)
{
    message = text;
    message_until = *rb->current_tick + 5 * HZ;
}

static void restore_repeat(void)
{
    if (repeat_overridden)
    {
        rb->global_settings->repeat_mode = saved_repeat;
        repeat_overridden = false;
    }
}

static void flush_radio(void *unused)
{
    (void)unused;
    save_settings();
    restore_repeat();
    if (catalog >= 0)
    {
        rb->close(catalog);
        catalog = -1;
        rb->remove(CATALOG_PATH);
    }
}

static void handle_usb(long button)
{
    usb = rb->default_event_handler_ex(button, flush_radio, NULL)
        == SYS_USB_CONNECTED;
}

static void label(int x, int y, unsigned color, const char *str)
{
    rb->lcd_set_foreground(color);
    rb->lcd_putsxy(x, y, str);
}

static void rule(int y)
{
    rb->lcd_set_foreground(LINE);
    rb->lcd_hline(16, 303, y);
}

static bool is_era(int band)
{
    return band >= ERA_70 && band <= ERA_10;
}

static const char *station_for(int band)
{
    return station_label[band == NINETIES ? 2 : is_era(band) ? 1 : 0];
}

static void frame(const char *heading)
{
    rb->lcd_set_viewport(NULL);
    rb->lcd_set_drawmode(DRMODE_SOLID);
    rb->lcd_setfont(FONT_SYSFIXED);
    rb->lcd_set_background(BLACK);
    rb->lcd_clear_display();
    rb->lcd_set_foreground(HEADER);
    rb->lcd_fillrect(0, 0, LCD_WIDTH, 31);
    label(16, 11, GREEN, "ROCKBOX RADIO");
    label(208, 11, DIM, heading);
    rule(31);
    rule(216);
}

static void draw_tuning(bool indexing)
{
    unsigned int phase = (unsigned long)*rb->current_tick / (HZ / 12);
    char count[40];
    frame("TUNER");
    label(24, 63, GREEN, station_for(pending));
    if (is_era(pending))
        label(24, 80, DIM, era_label[pending - ERA_70]);
    label(24, 110, AMBER, indexing ? "INDEXING TRACKS" : "TUNING");
    if (indexing)
    {
        rb->snprintf(count, sizeof(count), "%u tracks read", library_count);
        label(24, 126, PALE, count);
    }
    int marker = phase % 38;
    for (int i = 0; i < 38; ++i)
    {
        rb->lcd_set_foreground(i == marker ? AMBER : i % 5 == 0 ? GREEN : LINE);
        rb->lcd_fillrect(24 + 7 * i, 151, 2, i % 5 == 0 ? 18 : 11);
    }
    label(16, 224, DIM, "MENU  Cancel");
    rb->lcd_update();
}

static uint32_t artist_key(const char *tag)
{
    if (!tag[0] || !rb->strcmp(tag, UNTAGGED))
        return 0;

    uint32_t key = 2166136261u;
    while (*tag)
    {
        unsigned char c = *tag++;
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
        key = (key ^ c) * 16777619u;
    }
    return key ? key : 1;
}

static uint8_t track_bands(long release_year)
{
    uint8_t bands = 1u << RANDOM_FM;
    if (release_year >= 1970 && release_year <= 2019)
        bands |= 1u << (ERA_70 + (release_year - 1970) / 10);
    if (release_year >= 1990 && release_year <= 2009)
        bands |= 1u << NINETIES;
    return bands;
}

static void discard_catalog(void)
{
    if (catalog >= 0)
        rb->close(catalog);
    catalog = -1;
    rb->remove(CATALOG_PATH);
    library_count = 0;
}

static bool build_catalog(void)
{
    char filename[TAGCACHE_BUFSZ], tag[TAGCACHE_BUFSZ];
    uint32_t offset = 0;
    bool ok = true;

    if (catalog_ready)
        return true;
    if (!rb->tagcache_get_stat()->ready || !rb->tagcache_search(&search, tag_filename))
    {
        rb->tagcache_search_finish(&search);
        notice("Initialize or update Rockbox's Database");
        return false;
    }

    catalog = rb->open(CATALOG_PATH, O_CREAT | O_TRUNC | O_RDWR, 0666);
    if (catalog < 0)
    {
        rb->tagcache_search_finish(&search);
        notice("Could not write radio data");
        return false;
    }

    long refresh = *rb->current_tick;
    while (rb->tagcache_get_next(&search, filename, sizeof(filename)))
    {
        if (TIME_AFTER(*rb->current_tick, refresh))
        {
            draw_tuning(true);
            refresh = *rb->current_tick + HZ / 8;
            long button = rb->button_get(false);
            if (button == BUTTON_MENU || button == SYS_USB_CONNECTED)
            {
                rb->tagcache_search_finish(&search);
                discard_catalog();
                if (button == SYS_USB_CONNECTED)
                    handle_usb(button);
                return false;
            }
            rb->yield();
        }

        size_t size = rb->strlen(filename) + 1;
        if (filename[0] != '/' || size > MAX_PATH ||
            rb->strchr(filename, '\n') || rb->strchr(filename, '\r'))
            continue;
        if (library_count == capacity)
        {
            notice("Library exceeds the radio's index capacity");
            ok = false;
            break;
        }

        long release_year = rb->tagcache_get_numeric(&search, tag_year);
        bool tagged = rb->tagcache_retrieve(&search, search.idx_id, tag_artist,
                                           tag, sizeof(tag));
        if (rb->write(catalog, filename, size) != (ssize_t)size)
        {
            notice("Could not write radio catalog");
            ok = false;
            break;
        }
        library[library_count++] = (struct library_track) {
            offset, tagged ? artist_key(tag) : 0, track_bands(release_year), 0
        };
        offset += size;
    }
    rb->tagcache_search_finish(&search);

    if (!ok || !library_count)
    {
        if (ok)
            notice("No music found in Rockbox's Database");
        discard_catalog();
        return false;
    }
    catalog_ready = true;
    return true;
}

static bool read_path(int track)
{
    off_t offset = library[track].offset;
    if (rb->lseek(catalog, offset, SEEK_SET) != offset)
        return false;
    ssize_t count = rb->read(catalog, path, sizeof(path));
    return count > 0 && rb->memchr(path, '\0', count) != NULL;
}

static unsigned int collect(uint8_t mask, bool reset)
{
    unsigned int count = 0;
    for (unsigned int i = 0; i < library_count; ++i)
    {
        if (reset)
            library[i].played &= ~mask;
        if ((library[i].bands & mask) && !(library[i].played & mask))
            candidates[count++] = i;
    }
    return count;
}

static unsigned int candidate_at(unsigned int count, int last)
{
    unsigned int start = pick(count);
    if (last < 0 || count == 1)
        return start;

    uint32_t previous = settings[DIFFERENT_ARTIST] ? library[last].artist : 0;
    if (candidates[start] != last &&
        (!previous || library[candidates[start]].artist != previous))
        return start;

    unsigned int fallback = start, preferred = start;
    unsigned int alternatives = 0, different = 0;
    for (unsigned int pos = 0; pos < count; ++pos)
    {
        int track = candidates[pos];
        if (track == last)
            continue;
        if (pick(++alternatives) == 0)
            fallback = pos;
        if ((!previous || library[track].artist != previous) &&
            pick(++different) == 0)
            preferred = pos;
    }
    return different ? preferred : fallback;
}

static bool fill_queue(int band)
{
    struct station_queue *queue = &queues[band];
    uint8_t mask = 1u << band;
    queue->length = queue->next = 0;
    int last = queue->last;
    int limit = MIN(BATCH_SIZE, rb->global_settings->max_files_in_playlist);
    bool reset = !settings[FULL_CYCLE];

    for (;;)
    {
        unsigned int count = collect(mask, reset);
        while (count && queue->length < limit)
        {
            unsigned int pos = candidate_at(count, last);
            int track = candidates[pos];
            candidates[pos] = candidates[--count];

            if (!read_path(track) || !rb->file_exists(path))
            {
                library[track].bands = 0;
                continue;
            }
            queue->track[queue->length++] = track;
            library[track].played |= mask;
            last = track;
        }
        if (queue->length || reset)
            return queue->length > 0;
        reset = true;
    }
}

static void remember_position(void)
{
    if (active >= 0 && position >= 0)
    {
        struct station_queue *queue = &queues[active];
        queue->last = queue->track[position];
        queue->next = position + 1;
    }
}

static void clear_track(void)
{
    now_path[0] = '\0';
    elapsed = duration = 0;
    year = 0;
}

static unsigned long random_elapsed(int track)
{
    if (!settings[RANDOM_POSITION] || !read_path(track))
        return 0;
    int fd = rb->open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    bool valid = rb->get_metadata(&probe, fd, path);
    rb->close(fd);
    if (!valid || probe.length < 10000)
        return 0;

    /* Resume by time. Keep at least 10% at each end and avoid overflow. */
    return (probe.length / 100) * (10 + pick(81));
}

static void start_at(int index, unsigned long start)
{
    position = index;
    clear_track();
    play_deadline = *rb->current_tick + 8 * HZ;
    rb->playlist_start(index, start, 0);
}

static bool load_queue(int band, bool surprise)
{
    struct station_queue *queue = &queues[band];
    unsigned long start = 0;
    if (queue->next >= queue->length && !fill_queue(band))
    {
        notice(band == RANDOM_FM ? "No accessible tracks in the library" :
                                  "No accessible tracks in this era");
        return false;
    }

    /* Write to the file the core is not using. Its current playlist may
       still be playing after a previous Radio session. */
    const struct playlist_info *current = rb->playlist_get_current();
    const char *file = queue_files[0];
    if (!rb->strcmp(current->filename, RADIO_DIR "/queue-a.m3u8"))
        file = queue_files[1];

    char list_path[MAX_PATH];
    rb->snprintf(list_path, sizeof(list_path), RADIO_DIR "/%s", file);
    int fd = rb->open(list_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
    {
        notice("Could not write the radio queue");
        return false;
    }

    bool ok = true;
    for (int i = 0; i < queue->length; ++i)
    {
        if (!read_path(queue->track[i]))
        {
            ok = false;
            break;
        }
        size_t length = rb->strlen(path);
        path[length++] = '\n';
        if (rb->write(fd, path, length) != (ssize_t)length)
        {
            ok = false;
            break;
        }
    }
    if (rb->close(fd) < 0)
        ok = false;
    if (!ok)
    {
        notice("Could not write the radio queue");
        return false;
    }
    if (surprise)
        start = random_elapsed(queue->track[queue->next]);

    rb->audio_stop();
    if (!repeat_overridden)
    {
        saved_repeat = rb->global_settings->repeat_mode;
        repeat_overridden = true;
        rb->global_settings->repeat_mode = REPEAT_OFF;
    }
    if (rb->playlist_create(RADIO_DIR, file) < 0 ||
        rb->playlist_amount() != queue->length)
    {
        active = -1;
        clear_track();
        restore_repeat();
        notice("Rockbox could not load the radio queue");
        return false;
    }

    active = band;
    batch_has_played = false;
    message = NULL;
    start_at(queue->next, start);
    return true;
}

static void begin_tune(int band, bool surprise)
{
    screen = RADIO;
    pending = band;
    pending_surprise = surprise;
    unsigned int delay = surprise ? settings[DELAY_MIN] +
        pick(settings[DELAY_MAX] - settings[DELAY_MIN] + 1) : 0;
    tune_deadline = *rb->current_tick + delay * HZ;
    message = NULL;
}

static void tune(void)
{
    draw_tuning(!catalog_ready);
    if (!build_catalog())
    {
        pending = -1;
        return;
    }
    if (TIME_BEFORE(*rb->current_tick, tune_deadline))
        return;

    int band = pending;
    pending = -1;
    if (band == active)
        return;
    if (is_era(band))
    {
        settings[SAVED_ERA] = band;
        dirty = true;
    }
    remember_position();
    queues[band].failed_batches = 0;
    if (load_queue(band, pending_surprise))
    {
        settings[SAVED_BAND] = band;
        dirty = true;
    }
    if (!save_settings())
        notice("Could not save radio settings");
}

static const char *track_basename(const char *name)
{
    const char *slash = rb->strrchr(name, '/');
    return slash ? slash + 1 : name;
}

static const char *tag_or(const char *value, const char *fallback)
{
    return value && *value ? value : fallback;
}

static void capture_track(void)
{
    struct mp3entry *id3 = rb->audio_current_track();
    if (!id3 || !id3->path[0])
        return;

    struct station_queue *queue = &queues[active];
    int index = rb->playlist_get_display_index() - 1;
    if (index < 0 || index >= queue->length)
        return;
    if (index != position || rb->strcmp(now_path, id3->path))
    {
        position = index;
        rb->strlcpy(now_path, id3->path, sizeof(now_path));
        rb->strlcpy(title, tag_or(id3->title, track_basename(now_path)), sizeof(title));
        rb->strlcpy(artist, tag_or(id3->artist, "Unknown artist"), sizeof(artist));
        rb->strlcpy(album, tag_or(id3->album, "Unknown album"), sizeof(album));
        year = id3->year;
        track_since = *rb->current_tick;
    }
    elapsed = id3->elapsed;
    duration = id3->length;
    batch_has_played = true;
    queue->failed_batches = 0;
}

static void leave_station(const char *reason)
{
    active = -1;
    clear_track();
    restore_repeat();
    if (reason)
        notice(reason);
}

static void advance_batch(void)
{
    int band = active;
    struct station_queue *queue = &queues[band];
    remember_position();
    queue->next = queue->length;
    if (!load_queue(band, false))
        leave_station(NULL);
}

static void poll_audio(void)
{
    if (active < 0)
        return;

    int status = rb->audio_status();
    if (status & AUDIO_STATUS_PLAY)
    {
        capture_track();
        if (now_path[0] || (status & AUDIO_STATUS_PAUSE) ||
            TIME_BEFORE(*rb->current_tick, play_deadline))
            return;
    }
    else if (TIME_BEFORE(*rb->current_tick, play_deadline) && !now_path[0])
        return;

    struct station_queue *queue = &queues[active];
    int index = rb->playlist_get_display_index() - 1;
    position = MAX(position, MIN(index, queue->length - 1));
    if (position + 1 < queue->length)
    {
        start_at(position + 1, 0);
        return;
    }
    if (!batch_has_played && ++queue->failed_batches >= 2)
    {
        rb->audio_stop();
        queue->next = queue->length;
        leave_station("Rockbox could not play this station's tracks");
        return;
    }
    advance_batch();
}

static void draw_track(int y, unsigned color, const char *value)
{
    struct viewport vp;
    rb->viewport_set_defaults(&vp, SCREEN_MAIN);
    vp.x = 24;
    vp.y = y;
    vp.width = 272;
    vp.height = 12;
    vp.font = FONT_SYSFIXED;
    vp.drawmode = DRMODE_SOLID;
    vp.fg_pattern = color;
    vp.bg_pattern = BLACK;
    rb->lcd_set_viewport(&vp);
    int width;
    rb->lcd_getstringsize(value, &width, NULL);
    int offset = 0;
    long age = *rb->current_tick - track_since;
    if (width > vp.width && age > HZ)
    {
        int end = width - vp.width;
        offset = ((age - HZ) / (HZ / 16)) % (2 * end + 32);
        offset = MIN(offset, MAX(0, 2 * end + 32 - offset));
        offset = MIN(offset, end);
    }
    rb->lcd_putsxy(-offset, 0, value);
    rb->lcd_set_viewport(NULL);
}

static void draw_radio(void)
{
    int band = active >= 0 ? active : settings[REMEMBER] ? settings[SAVED_BAND] : RANDOM_FM;
    bool paused = active >= 0 && (rb->audio_status() & AUDIO_STATUS_PAUSE);
    bool playing = active >= 0 && now_path[0] && !paused;
    const char *frequency = band == RANDOM_FM ? "87.9 MHz" : band == NINETIES ?
                            "99.9 MHz" : "93.5 MHz";
    frame("FM RECEIVER");
    rb->lcd_set_foreground(HEADER);
    rb->lcd_fillrect(16, 42, 288, 39);
    label(24, 48, GREEN, station_for(band));
    label(24, 63, DIM, is_era(band) ? era_label[band - ERA_70] : frequency);
    label(238, 48, AMBER, playing ? "ON AIR" : paused ? "PAUSED" :
          active >= 0 ? "LOADING" : "STANDBY");
    if (is_era(band))
        label(238, 63, DIM, frequency);

    if (message)
    {
        label(24, 101, AMBER, message);
        if (active < 0)
            label(24, 120, DIM, "SELECT to choose a station");
    }
    else if (now_path[0])
    {
        label(24, 91, DIM, "NOW PLAYING");
        draw_track(105, PALE, title);
        draw_track(120, GREEN, artist);
        draw_track(135, DIM, album);
    }
    else
    {
        label(24, 101, DIM, active >= 0 ? "Waiting for audio..." : "SELECT to choose a station");
    }

    unsigned int phase = (unsigned long)*rb->current_tick / (HZ / 12);
    for (int i = 0; i < 24; ++i)
    {
        int h = playing ? 3 + ((phase * (i % 3 + 1) + i * i) % 12) : 1;
        rb->lcd_set_foreground(i % 6 == 5 ? AMBER : playing ? GREEN : LINE);
        rb->lcd_fillrect(24 + i * 11, 171 - h, 7, h);
    }
    rb->lcd_set_foreground(LINE);
    rb->lcd_fillrect(24, 180, 264, 2);
    if (duration)
    {
        char time[52];
        unsigned long seconds = elapsed / 1000, total = duration / 1000;
        unsigned long part = duration / 264 + 1;
        rb->lcd_set_foreground(GREEN);
        rb->lcd_fillrect(24, 180, MIN(264ul, elapsed / part), 2);
        rb->snprintf(time, sizeof(time), "%lu:%02lu / %lu:%02lu",
                     seconds / 60, seconds % 60, total / 60, total % 60);
        label(24, 189, DIM, time);
        if (year > 0)
        {
            rb->snprintf(time, sizeof(time), "%d", year);
            label(264, 189, DIM, time);
        }
    }
    label(16, 205, DIM, "PLAY  Pause / Resume");
    label(16, 224, DIM, "<< Restart  >> Next  SELECT Stations  MENU Exit");
    rb->lcd_update();
}

static int menu_length(void)
{
    return screen == SETTINGS ? SAVED_BAND : 5;
}

static void draw_menu(void)
{
    frame(screen == SETTINGS ? "SETTINGS" : screen == ERAS ? "CUSTOM ERA FM" : "STATIONS");
    int step = screen == SETTINGS ? 21 : 28;
    for (int i = 0; i < menu_length(); ++i)
    {
        int y = 42 + step * i;
        unsigned color = i == selected ? AMBER : PALE;
        if (i == selected)
        {
            rb->lcd_set_foreground(HEADER);
            rb->lcd_fillrect(16, y, 288, step - 2);
            rb->lcd_set_foreground(AMBER);
            rb->lcd_fillrect(16, y, 2, step - 2);
        }
        if (screen == SETTINGS)
        {
            char value[16];
            if (i == DELAY_MIN || i == DELAY_MAX)
                rb->snprintf(value, sizeof(value), "%u sec", (unsigned)settings[i]);
            else
                rb->strlcpy(value, settings[i] ? "ON" : "OFF", sizeof(value));
            label(26, y + 5, color, setting_label[i]);
            label(228, y + 5, color, value);
        }
        else if (screen == ERAS)
            label(26, y + 8, color, era_label[i]);
        else
            label(26, y + 8, color, i < 3 ? station_label[i] : i == 3 ? "Settings" : "Exit radio");
    }
    label(16, 224, DIM, screen == SETTINGS ? "WHEEL Move   << / >>  Adjust   MENU Back" :
                                            "WHEEL Move   SELECT Choose    MENU Back");
    rb->lcd_update();
}

static void change_setting(int delta)
{
    int index = selected;
    if (index == DELAY_MIN || index == DELAY_MAX)
    {
        settings[index] = MIN(MAX((int)settings[index] + delta, 0), MAX_DELAY);
        if (settings[DELAY_MIN] > settings[DELAY_MAX])
            settings[index == DELAY_MIN ? DELAY_MAX : DELAY_MIN] = settings[index];
    }
    else
        settings[index] ^= 1;
    dirty = true;
}

static bool input(long button)
{
    if (pending >= 0)
    {
        if (button == BUTTON_MENU)
            pending = -1;
        return true;
    }
    if (screen == RADIO)
    {
        switch (button)
        {
        case BUTTON_MENU:
            return false;
        case BUTTON_SELECT:
            screen = STATIONS;
            selected = active == NINETIES ? 2 : is_era(active) ? 1 : 0;
            break;
        case BUTTON_PLAY:
            if (active < 0)
                begin_tune(settings[REMEMBER] ? settings[SAVED_BAND] : RANDOM_FM, false);
            else if (rb->audio_status() & AUDIO_STATUS_PAUSE)
                rb->audio_resume();
            else
                rb->audio_pause();
            break;
        case BUTTON_LEFT:
            if (active >= 0 && now_path[0])
                rb->audio_ff_rewind(0);
            break;
        case BUTTON_RIGHT:
            if (active >= 0)
            {
                if (position + 1 < queues[active].length)
                    start_at(position + 1, 0);
                else
                    advance_batch();
            }
            break;
        }
        return true;
    }

    int length = menu_length();
    if ((button & ~BUTTON_REPEAT) == BUTTON_SCROLL_FWD)
        selected = (selected + 1) % length;
    else if ((button & ~BUTTON_REPEAT) == BUTTON_SCROLL_BACK)
        selected = (selected + length - 1) % length;
    else if (button == BUTTON_MENU)
    {
        if (screen == ERAS || screen == SETTINGS)
        {
            if (!save_settings())
                notice("Could not save radio settings");
            selected = screen == ERAS ? 1 : 3;
            screen = STATIONS;
        }
        else
            screen = RADIO;
    }
    else if (screen == SETTINGS && (button == BUTTON_SELECT ||
             (button & ~BUTTON_REPEAT) == BUTTON_LEFT ||
             (button & ~BUTTON_REPEAT) == BUTTON_RIGHT))
        change_setting((button & ~BUTTON_REPEAT) == BUTTON_LEFT ? -1 : 1);
    else if (button == BUTTON_SELECT)
    {
        if (screen == ERAS)
            begin_tune(ERA_70 + selected, false);
        else if (selected == 0 || selected == 2)
            begin_tune(selected == 0 ? RANDOM_FM : NINETIES, false);
        else if (selected == 1)
        {
            screen = ERAS;
            selected = settings[SAVED_ERA] - ERA_70;
        }
        else if (selected == 3)
        {
            screen = SETTINGS;
            selected = 0;
        }
        else
            return false;
    }
    return true;
}

enum plugin_status plugin_start(const void *parameter)
{
    (void)parameter;
    size_t bytes = 0;
    library = rb->plugin_get_buffer(&bytes);
    capacity = MIN(bytes / (sizeof(*library) + sizeof(*candidates)), 65535u);
    if (!library || !capacity)
    {
        rb->splash(2 * HZ, "No memory for Rockbox Radio");
        return PLUGIN_ERROR;
    }
    candidates = (uint16_t *)(library + capacity);
    for (int band = 0; band < BAND_COUNT; ++band)
        queues[band].last = -1;
    random_state = (uint32_t)*rb->current_tick ^ 0xa8b4135fu;
    if (!random_state)
        random_state = 1;

    rb->mkdir(PLUGIN_APPS_DATA_DIR);
    rb->mkdir(RADIO_DIR);
    load_settings();
    rb->lcd_set_backdrop(NULL);

    if (settings[SURPRISE])
        begin_tune(settings[REMEMBER] ? settings[SAVED_BAND] : RANDOM_FM, true);

    while (!usb)
    {
        poll_audio();
        if (message && TIME_AFTER(*rb->current_tick, message_until))
            message = NULL;

        if (pending >= 0)
            tune();
        else if (screen == RADIO)
            draw_radio();
        else
            draw_menu();
        if (usb)
            break;

        long button = rb->button_get_w_tmo(HZ / 12);
        if (button == SYS_USB_CONNECTED)
            handle_usb(button);
        else if (button == SYS_POWEROFF || button == SYS_REBOOT)
        {
            save_settings();
            if (rb->default_event_handler(button) == button)
                break;
        }
        else if (button & SYS_EVENT)
            rb->default_event_handler(button);
        else if (!input(button))
            break;
    }

    restore_repeat();
    if (catalog >= 0)
        discard_catalog();
    if (!usb && !save_settings())
        rb->splash(HZ, "Could not save radio settings");
    return usb ? PLUGIN_USB_CONNECTED : PLUGIN_OK;
}
