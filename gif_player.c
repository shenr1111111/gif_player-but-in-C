

#define _POSIX_C_SOURCE 200809L

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <dirent.h>
#include <limits.h>
#include <fcntl.h>

#define DEFAULT_WIDTH  480
#define DEFAULT_HEIGHT 270
#define DEFAULT_FPS    24.0

#define TMP_DIR        "/tmp/gif_player_frames"
#define CONFIG_SUBDIR  "gif_player"
#define CONFIG_FILE    "config.toml"
#define CONFIG_MAXLEN  4096

typedef struct {
    char   video[PATH_MAX];   /* empty string = not set */
    int    width;
    int    height;
    double fps;
} Config;

static void config_init_defaults(Config *cfg)
{
    cfg->video[0] = '\0';
    cfg->width    = DEFAULT_WIDTH;
    cfg->height   = DEFAULT_HEIGHT;
    cfg->fps      = DEFAULT_FPS;
}


static char *strtrim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *end = s + strlen(s);
    while (end > s && (*(end-1) == ' ' || *(end-1) == '\t'
                       || *(end-1) == '\r' || *(end-1) == '\n'))
        *--end = '\0';
    return s;
}

static void parse_toml(Config *cfg, const char *contents)
{
    char buf[CONFIG_MAXLEN];
    strncpy(buf, contents, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *line = buf;
    char *next;
    while (line && *line) {
        next = strchr(line, '\n');
        if (next) *next++ = '\0';

        char *trimmed = strtrim(line);

        
        if (*trimmed == '#' || *trimmed == '\0') {
            line = next;
            continue;
        }

        
        char *eq = strchr(trimmed, '=');
        if (!eq) { line = next; continue; }
        *eq = '\0';
        char *key = strtrim(trimmed);
        char *val = strtrim(eq + 1);

        if (strcmp(key, "video") == 0) {
            /* Strip surrounding quotes */
            size_t vlen = strlen(val);
            if (vlen >= 2 && val[0] == '"' && val[vlen-1] == '"') {
                val[vlen-1] = '\0';
                val++;
            }
            strncpy(cfg->video, val, PATH_MAX - 1);
            cfg->video[PATH_MAX - 1] = '\0';
        } else if (strcmp(key, "width") == 0) {
            cfg->width = (int)strtol(val, NULL, 10);
        } else if (strcmp(key, "height") == 0) {
            cfg->height = (int)strtol(val, NULL, 10);
        } else if (strcmp(key, "fps") == 0) {
            cfg->fps = strtod(val, NULL);
        }

        line = next;
    }
}


static void config_path(char *out, size_t outsz)
{
    const char *home = getenv("HOME");
    if (!home || !*home) home = ".";
    snprintf(out, outsz, "%s/.config/%s/%s", home, CONFIG_SUBDIR, CONFIG_FILE);
}

static const char CONFIG_TEMPLATE[] =
    "# gif_player configuration\n"
    "#\n"
    "# Window size in pixels (optional, defaults shown):\n"
    "# width = 480\n"
    "# height = 270\n"
    "\n"
    "# Playback speed in frames per second (optional):\n"
    "# fps = 24\n"
    "\n"
    "# Path to your video (optional — you can also pass it as an argument):\n"
    "# video = \"/path/to/video.mp4\"\n";

static void load_or_create_config(Config *cfg)
{
    config_init_defaults(cfg);

    char path[PATH_MAX];
    config_path(path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (f) {
        char contents[CONFIG_MAXLEN];
        size_t n = fread(contents, 1, sizeof(contents) - 1, f);
        contents[n] = '\0';
        fclose(f);
        parse_toml(cfg, contents);
        return;
    }

    
    char dir[PATH_MAX];
    config_path(dir, sizeof(dir));
    
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0';

    
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", dir);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);

    f = fopen(path, "w");
    if (f) {
        fputs(CONFIG_TEMPLATE, f);
        fclose(f);
        fprintf(stderr, "Created default config at %s\n", path);
        fprintf(stderr, "Please set the 'video' path in that file, "
                        "or pass a video as an argument.\n");
    } else {
        fprintf(stderr, "Warning: could not write config to %s: %s\n",
                path, strerror(errno));
    }
}


static int has_flag(int argc, char *argv[], const char *flag)
{
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], flag) == 0) return 1;
    return 0;
}


static const char *get_arg(int argc, char *argv[], const char *key)
{
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "--%s=", key);
    size_t plen = strlen(prefix);
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], prefix, plen) == 0)
            return argv[i] + plen;
    }
    return NULL;
}


static const char *first_positional(int argc, char *argv[])
{
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') return argv[i];
    }
    return NULL;
}


static void relaunch_background(int argc, char *argv[],
                                const char *video,
                                int width, int height, double fps)
{
    /* Build argument list:
     *   argv[0] video --width=W --height=H --fps=F --background
     */
    int extra = 5; /* video, --width, --height, --fps, --background */
    char **newargs = calloc((size_t)(extra + 2), sizeof(char *));
    if (!newargs) { perror("calloc"); exit(1); }

    char wbuf[32], hbuf[32], fbuf[64];
    snprintf(wbuf, sizeof(wbuf), "--width=%d",  width);
    snprintf(hbuf, sizeof(hbuf), "--height=%d", height);
    snprintf(fbuf, sizeof(fbuf), "--fps=%g",    fps);

    int idx = 0;
    newargs[idx++] = argv[0];
    newargs[idx++] = (char *)video;
    newargs[idx++] = wbuf;
    newargs[idx++] = hbuf;
    newargs[idx++] = fbuf;
    newargs[idx++] = "--background";
    newargs[idx]   = NULL;

    (void)argc; /* suppress unused warning */

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); exit(1); }
    if (pid == 0) {
        /* Child: detach from terminal */
        setsid();

        
        int devnull = open("/dev/null", 0);  /* O_RDONLY = 0 */
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        execvp(argv[0], newargs);
        _exit(1); /* exec failed */
    }
    
    free(newargs);
}


static int extract_frames(const char *video_path, int width, int height)
{
    
    {
        
        char cmd[PATH_MAX + 32];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", TMP_DIR);
        (void)system(cmd);
    }
    if (mkdir(TMP_DIR, 0755) != 0) {
        fprintf(stderr, "Could not create temp dir %s: %s\n",
                TMP_DIR, strerror(errno));
        return 0;
    }

    char scale_arg[64];
    snprintf(scale_arg, sizeof(scale_arg), "scale=%d:%d", width, height);

    char out_pattern[PATH_MAX];
    snprintf(out_pattern, sizeof(out_pattern), "%s/frame%%05d.png", TMP_DIR);

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 0; }

    if (pid == 0) {
        /* Redirect stdout/stderr to /dev/null (quiet mode) */
        int devnull = open("/dev/null", 0);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        char *ffargs[] = {
            "ffmpeg",
            "-v", "quiet",
            "-i", (char *)video_path,
            "-vf", scale_arg,
            out_pattern,
            NULL
        };
        execvp("ffmpeg", ffargs);
        _exit(127); /* ffmpeg not found */
    }

    int status;
    waitpid(pid, &status, 0);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "ffmpeg failed. Is the video path correct?\n"
                        "(install ffmpeg: sudo pacman -S ffmpeg)\n");
        return 0;
    }
    return 1;
}


typedef struct {
    uint8_t *pixels; /* raw RGBA bytes, width*height*4 */
} Frame;


static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char **)a, *(const char **)b);
}

static Frame *load_frames(int width, int height, size_t *count_out)
{
    DIR *d = opendir(TMP_DIR);
    if (!d) {
        fprintf(stderr, "Cannot open temp dir %s\n", TMP_DIR);
        return NULL;
    }

    char **names = NULL;
    size_t name_count = 0, name_cap = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (name_count == name_cap) {
            name_cap = name_cap ? name_cap * 2 : 64;
            names = realloc(names, name_cap * sizeof(char *));
            if (!names) { perror("realloc"); closedir(d); return NULL; }
        }
        names[name_count++] = strdup(ent->d_name);
    }
    closedir(d);

    if (name_count == 0) {
        fprintf(stderr, "No frames extracted!\n");
        free(names);
        return NULL;
    }

    qsort(names, name_count, sizeof(char *), cmp_str);

    Frame *frames = calloc(name_count, sizeof(Frame));
    if (!frames) { perror("calloc"); goto cleanup; }

    size_t loaded = 0;
    for (size_t i = 0; i < name_count; i++) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", TMP_DIR, names[i]);

        SDL_Surface *surf = IMG_Load(path);
        if (!surf) {
            fprintf(stderr, "Warning: cannot load frame %s: %s\n",
                    path, IMG_GetError());
            continue;
        }

        
        SDL_Surface *conv = SDL_ConvertSurfaceFormat(
            surf, SDL_PIXELFORMAT_RGBA32, 0);
        SDL_FreeSurface(surf);
        if (!conv) {
            fprintf(stderr, "Warning: convert failed: %s\n", SDL_GetError());
            continue;
        }

        size_t nbytes = (size_t)(width * height * 4);
        frames[loaded].pixels = malloc(nbytes);
        if (!frames[loaded].pixels) { perror("malloc"); SDL_FreeSurface(conv); break; }
        memcpy(frames[loaded].pixels, conv->pixels, nbytes);
        SDL_FreeSurface(conv);
        loaded++;
    }

    *count_out = loaded;

cleanup:
    for (size_t i = 0; i < name_count; i++) free(names[i]);
    free(names);

    return frames;
}

static void free_frames(Frame *frames, size_t count)
{
    for (size_t i = 0; i < count; i++) free(frames[i].pixels);
    free(frames);
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int main(int argc, char *argv[])
{
    
    int background = has_flag(argc, argv, "--background");

    if (!background) {
        
        Config cfg;
        load_or_create_config(&cfg);

        const char *video = first_positional(argc, argv);
        if (!video && cfg.video[0]) video = cfg.video;
        if (!video) {
            char cpath[PATH_MAX];
            config_path(cpath, sizeof(cpath));
            fprintf(stderr, "No video specified. Either:\n");
            fprintf(stderr, "  1. Set 'video' in %s\n", cpath);
            fprintf(stderr, "  2. Pass a path as an argument: gif_player /path/to/video.mp4\n");
            return 1;
        }

        
        const char *w_str = get_arg(argc, argv, "width");
        const char *h_str = get_arg(argc, argv, "height");
        const char *f_str = get_arg(argc, argv, "fps");
        int    width  = w_str ? (int)strtol(w_str, NULL, 10) : cfg.width;
        int    height = h_str ? (int)strtol(h_str, NULL, 10) : cfg.height;
        double fps    = f_str ? strtod(f_str, NULL)          : cfg.fps;

        relaunch_background(argc, argv, video, width, height, fps);
        return 0;
    }

    
    const char *video_path = first_positional(argc, argv);
    if (!video_path) {
        fprintf(stderr, "No video path received.\n");
        return 1;
    }

    const char *w_str = get_arg(argc, argv, "width");
    const char *h_str = get_arg(argc, argv, "height");
    const char *f_str = get_arg(argc, argv, "fps");
    int    width  = w_str ? (int)strtol(w_str, NULL, 10) : DEFAULT_WIDTH;
    int    height = h_str ? (int)strtol(h_str, NULL, 10) : DEFAULT_HEIGHT;
    double fps    = f_str ? strtod(f_str, NULL)           : DEFAULT_FPS;

    
    if (!extract_frames(video_path, width, height)) return 1;

    
    size_t frame_count = 0;
    Frame *frames = load_frames(width, height, &frame_count);
    if (!frames || frame_count == 0) {
        fprintf(stderr, "No frames loaded.\n");
        return 1;
    }

    
    {
        char cmd[PATH_MAX + 32];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", TMP_DIR);
        (void)system(cmd);
    }

    
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        free_frames(frames, frame_count);
        return 1;
    }
    if (!(IMG_Init(IMG_INIT_PNG) & IMG_INIT_PNG)) {
        fprintf(stderr, "IMG_Init failed: %s\n", IMG_GetError());
        SDL_Quit();
        free_frames(frames, frame_count);
        return 1;
    }

    
    SDL_Window *window = SDL_CreateWindow(
        "GIF Player",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        width, height,
        SDL_WINDOW_SHOWN | SDL_WINDOW_ALWAYS_ON_TOP
    );
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        IMG_Quit(); SDL_Quit();
        free_frames(frames, frame_count);
        return 1;
    }
    SDL_SetWindowResizable(window, SDL_FALSE);

    SDL_Renderer *renderer = SDL_CreateRenderer(
        window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        /* Fallback: software renderer */
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!renderer) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        IMG_Quit(); SDL_Quit();
        free_frames(frames, frame_count);
        return 1;
    }

    
    SDL_Texture *texture = SDL_CreateTexture(
        renderer,
        SDL_PIXELFORMAT_RGBA32,
        SDL_TEXTUREACCESS_STREAMING,
        width, height
    );
    if (!texture) {
        fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        IMG_Quit(); SDL_Quit();
        free_frames(frames, frame_count);
        return 1;
    }

    
    size_t   frame_index = 0;
    uint64_t frame_ns    = (uint64_t)(1e9 / fps);
    uint64_t last_frame  = now_ns();

    int      dragging     = 0;
    int      drag_off_x   = 0;
    int      drag_off_y   = 0;

    int running = 1;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT:
                running = 0;
                break;

            case SDL_MOUSEBUTTONDOWN:
                if (ev.button.button == SDL_BUTTON_LEFT) {
                    dragging = 1;
                    /* cursor position in global screen coords */
                    int gx, gy;
                    SDL_GetGlobalMouseState(&gx, &gy);
                    int wx, wy;
                    SDL_GetWindowPosition(window, &wx, &wy);
                    drag_off_x = gx - wx;
                    drag_off_y = gy - wy;
                }
                break;

            case SDL_MOUSEBUTTONUP:
                if (ev.button.button == SDL_BUTTON_LEFT)
                    dragging = 0;
                break;

            case SDL_MOUSEMOTION:
                if (dragging) {
                    int gx, gy;
                    SDL_GetGlobalMouseState(&gx, &gy);
                    SDL_SetWindowPosition(window,
                                         gx - drag_off_x,
                                         gy - drag_off_y);
                }
                break;

            default:
                break;
            }
        }

        
        uint64_t now = now_ns();
        if (now - last_frame >= frame_ns) {
            last_frame   = now;
            frame_index  = (frame_index + 1) % frame_count;

            
            SDL_UpdateTexture(texture, NULL,
                              frames[frame_index].pixels,
                              width * 4);
        }

        
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);

        
        SDL_Delay(1);
    }

    
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    IMG_Quit();
    SDL_Quit();
    free_frames(frames, frame_count);

    return 0;
}
