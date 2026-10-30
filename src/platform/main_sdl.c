#include "audio.h"
#include "smbcore/mario.h"
#include "movie.h"
#include "render_opengl.h"
#include "render_raster.h"
#include "timer.h"

#ifdef USE_SDL2
#  include <SDL.h>
#else
#  include <SDL3/SDL.h>
#endif
#include "palette_builtin.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct rom_extension {
  char s[8];
};

#define log_error(msg, ...) fprintf(stderr, "ERROR: " msg "\n", ##__VA_ARGS__)
#define log_info(msg, ...)  fprintf(stdout, "INFO: " msg "\n", ##__VA_ARGS__)

#ifdef OPENGL_ENABLED
static SMBgl_functionptr_f sdl_gl_get_proc_address(const char *name) {
  return (SMBgl_functionptr_f)SDL_GL_GetProcAddress(name);
}
#endif

#include "config.h"

static bool io_readtobuffer(const char *filename, size_t size, void *buf) {
  FILE *f = fopen(filename, "rb");
  if (!f) {
    log_error("%s", strerror(errno));
    return false;
  }
  if (fread(buf, size, 1, f) != 1) {
    log_error("Could not read bytes in file");
    return false;
  }
  if (fclose(f) != 0) {
    log_error("Could not close file");
    return false;
  }
  return true;
}

static bool io_writefrombuffer(const char *filename, size_t size, void *buf) {
  FILE *f = fopen(filename, "wb");
  if (!f) {
    log_error("%s", strerror(errno));
    return false;
  }
  if (fwrite(buf, size, 1, f) != 1) {
    log_error("Could not write bytes in file");
    return false;
  }
  if (fclose(f) != 0) {
    log_error("Could not close file");
    return false;
  }
  return true;
}

struct sdl_key_scancodes {
  SDL_Scancode u,d,l,r,select,start,b,a;
};

struct frontend_userdata {
  FILE *romfile;
  struct SMB_state *smb_state;
  struct SMB_audio *audio;
  struct Movie *movie;
  struct SMB_buttons player1_buttons;
  struct SMB_buttons player2_buttons;
#ifdef USE_SDL2
  SDL_GameController *gamecontroller;
  struct SMB_buttons player1_gamepad_buttons;
#endif
  bool rendering_enabled;
  struct config *cfg;

  SDL_Window *window;
  int video_scale;
  bool fullscreen;
  struct sdl_key_scancodes sdl_key_scancodes;

  u8 smb2j_games_beaten;

  // OpenGL mode
  struct SMBgl *smb_gl;

  // Raster mode
  struct SMBraster *smb_raster;
  SDL_Surface *surf;
  int pixels_stride;
};

void joy1(void *userdata, struct SMB_buttons *buttons) {
  struct frontend_userdata *fe = userdata;
#ifdef USE_SDL2
  const struct SMB_buttons *kb = &fe->player1_buttons;
  const struct SMB_buttons *pad = &fe->player1_gamepad_buttons;
  buttons->u      = kb->u      || pad->u;
  buttons->d      = kb->d      || pad->d;
  buttons->l      = kb->l      || pad->l;
  buttons->r      = kb->r      || pad->r;
  buttons->select = kb->select || pad->select;
  buttons->start  = kb->start  || pad->start;
  buttons->b      = kb->b      || pad->b;
  buttons->a      = kb->a      || pad->a;
#else
  *buttons = fe->player1_buttons;
#endif
}
void joy2(void *userdata, struct SMB_buttons *buttons) {
  struct frontend_userdata *fe = userdata;
  *buttons = fe->player2_buttons;
}

#ifdef USE_SDL2
static void poll_gamepad(struct frontend_userdata *fe) {
  struct SMB_buttons b = {0};
  SDL_GameController *gc = fe->gamecontroller;

  if (gc) {
    const int deadzone = 12000;
    const int ax = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX);
    const int ay = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY);

    b.u      = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_UP)    || ay < -deadzone;
    b.d      = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN)  || ay >  deadzone;
    b.l      = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_LEFT)  || ax < -deadzone;
    b.r      = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || ax >  deadzone;
    b.a      = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_A);
    b.b      = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_B)
            || SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_X);
    b.start  = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_START);
    b.select = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_BACK);
  }

  fe->player1_gamepad_buttons = b;
}
#endif

// Classic-mode rules: infinite lives and no time limit.
// Only touch the SMB1 RAM layout.
static void apply_classic_rules(struct SMB_state *state) {
  if (SMB_which_game(state) != GAME_SMB1) {
    return;
  }

  u8 *ram = SMB_ram(state);

  // NumberofLives / OffScr_NumberofLives: keep 9 and reset every frame.
  ram[0x075A] = 9;
  ram[0x0761] = 9;

  // GameTimerDisplay: freeze at 999.
  ram[0x07F8] = 9;
  ram[0x07F9] = 9;
  ram[0x07FA] = 9;

  // GameTimerExpiredFlag: never show TIME UP.
  ram[0x0759] = 0;
}


void update_pattern_tables(void *userdata, const u8 *chrrom) {
  struct frontend_userdata *fe = userdata;

  if (fe->smb_gl) {
    SMBgl_update_pattern_tables(fe->smb_gl, chrrom);
    return;
  } else if (fe->smb_raster) {
    SMBraster_update_pattern_tables(fe->smb_raster, chrrom);
  }
}

void update_palette(void *userdata, const u8 *palette_indices) {
  struct frontend_userdata *fe = userdata;

  if (fe->smb_gl) {
    SMBgl_update_palette(fe->smb_gl, palette_indices);
  } else if (fe->smb_raster) {
    SMBraster_update_palette(fe->smb_raster, palette_indices);
  }
}
void draw_tile(void *userdata, const struct SMB_tile tile) {
  struct frontend_userdata *fe = userdata;

  if (!fe->rendering_enabled) {
    return;
  }

  if (fe->smb_gl) {
    SMBgl_draw_tile(fe->smb_gl, tile);
  } else if (fe->smb_raster) {
    SMBraster_draw_tile(fe->smb_raster, tile);
  }
}

bool load_palette(u8 *rgb_palette, const char *filename) {
  return io_readtobuffer(filename, (size_t)0x40*3, rgb_palette);
}

bool load_ppuram(struct SMB_state *state, const char *filename) {
  return io_readtobuffer(filename, 0x4000, SMB_ppuram(state));
}

bool dump_ppuram(struct SMB_state *state, const char *filename) {
  return io_writefrombuffer(filename, 0x4000, SMB_ppuram(state));
}

bool load_ram(struct SMB_state *state, const char *filename) {
  if (!io_readtobuffer(filename, 0x800, SMB_ram(state))) {
    return false;
  }
  SMB_ram_finishwrite(state);
  return true;
}

bool dump_ram(struct SMB_state *state, const char *filename) {
  return io_writefrombuffer(filename, 0x800, SMB_ram(state));
}

u8 smb2j_load_games_beaten(void *userdata) {
  struct frontend_userdata *fe = userdata;
  return fe->smb2j_games_beaten;
}
bool smb2j_save_games_beaten(void *userdata, u8 games_beaten) {
  struct frontend_userdata *fe = userdata;
  log_info("Pretending to save game...");
  log_info("Games beaten: %d", games_beaten);
  fe->smb2j_games_beaten = games_beaten;
  return true;
}

bool advance_movie(struct frontend_userdata *fe) {
  if (!fe->movie) {
    return false;
  }

  struct MovieButtons movie_buttons;

  bool use_movie_buttons = movie_next(fe->movie, &movie_buttons, 0);
  if (use_movie_buttons) {
    fe->player1_buttons.u = movie_buttons.u;
    fe->player1_buttons.d = movie_buttons.d;
    fe->player1_buttons.l = movie_buttons.l;
    fe->player1_buttons.r = movie_buttons.r;
    fe->player1_buttons.b = movie_buttons.b;
    fe->player1_buttons.a = movie_buttons.a;
    fe->player1_buttons.select = movie_buttons.select;
    fe->player1_buttons.start = movie_buttons.start;
  }
  return use_movie_buttons;
}

static void set_fullscreen(struct frontend_userdata *fe, bool fullscreen) {
  fe->fullscreen = fullscreen;
#ifdef USE_SDL2
  SDL_SetWindowFullscreen(fe->window, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
#else
  SDL_SetWindowFullscreen(fe->window, fullscreen);
#endif
}

// Called whenvere a key is newly pressed, or newly released
// Does not send repeat keys from holding it down
static int on_keypress_change(struct frontend_userdata *fe, SDL_Scancode sc, bool isdown) {
  struct SMB_state *smb_state = fe->smb_state;

  if (isdown) {
    switch (sc) {
    case SDL_SCANCODE_1:
      if (load_ppuram(smb_state, fe->cfg->debug.dump_ppu_filename)) {
        if (!load_ram(smb_state, fe->cfg->debug.dump_ram_filename)) {
          // Couldn't load RAM - exit
          return 1;
        }
      }
      break;

    case SDL_SCANCODE_0:
      // dump the RAM and PPU
      dump_ppuram(smb_state, fe->cfg->debug.dump_ppu_filename);
      dump_ram(smb_state, fe->cfg->debug.dump_ram_filename);
      break;

    case SDL_SCANCODE_ESCAPE:
      // Stop the movie and let the player take control
      if (fe->movie) {
        movie_fini(fe->movie);
        free(fe->movie);
        fe->movie = 0;
      }
      break;

    case SDL_SCANCODE_F11:
      set_fullscreen(fe, !fe->fullscreen);
      break;

    case SDL_SCANCODE_Q:
#ifdef USE_SDL2
      if ((SDL_GetModState() & KMOD_ALT) != 0) {
#else
      if ((SDL_GetModState() & SDL_KMOD_ALT) != 0) {
#endif
        set_fullscreen(fe, !fe->fullscreen);
      }
      break;

    default:
      break;
    }
  }

#define KEY(key)  if (sc == fe->sdl_key_scancodes.key) { fe->player1_buttons.key = isdown; }
  KEY(u);
  KEY(d);
  KEY(l);
  KEY(r);
  KEY(select);
  KEY(start);
  KEY(b);
  KEY(a);
#undef KEY

  return 0;
}

int sdl_tick(void *userdata) {
  struct frontend_userdata *fe = userdata;
  struct SMB_state *smb_state = fe->smb_state;

#ifdef USE_SDL2
  SDL_Event eventData;
  while (SDL_PollEvent(&eventData)) {
    switch (eventData.type) {
    case SDL_KEYDOWN:
    case SDL_KEYUP:
      {
        bool isdown = eventData.key.state == SDL_PRESSED;
        SDL_Scancode sc = eventData.key.keysym.scancode;
        if (!eventData.key.repeat) {
          int res = on_keypress_change(fe, sc, isdown);
          if (res) {
            return res;
          }
        }
      }
    break;

    case SDL_CONTROLLERDEVICEADDED:
      if (!fe->gamecontroller && SDL_IsGameController(eventData.cdevice.which)) {
        fe->gamecontroller = SDL_GameControllerOpen(eventData.cdevice.which);
        if (fe->gamecontroller) {
          log_info("Gamepad connected: %s", SDL_GameControllerName(fe->gamecontroller));
        }
      }
      break;

    case SDL_CONTROLLERDEVICEREMOVED:
      if (fe->gamecontroller) {
        SDL_Joystick *joy = SDL_GameControllerGetJoystick(fe->gamecontroller);
        if (joy && SDL_JoystickInstanceID(joy) == eventData.cdevice.which) {
          SDL_GameControllerClose(fe->gamecontroller);
          fe->gamecontroller = NULL;
          log_info("Gamepad disconnected");
        }
      }
      break;

    case SDL_QUIT:
      return 1;
    }
  }
#else
  SDL_Event eventData;
  while (SDL_PollEvent(&eventData)) {
    switch (eventData.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
      {
        bool isdown = eventData.type == SDL_EVENT_KEY_DOWN;
        SDL_Scancode sc = eventData.key.scancode;
        if (!eventData.key.repeat) {
          int res = on_keypress_change(fe, sc, isdown);
          if (res) {
            return res;
          }
        }
      }
    break;

    case SDL_EVENT_QUIT:
      return 1;
    }
  }
#endif

  // Overwrite the player buttons if a movie exists with remaining frames
  advance_movie(fe);

#ifdef USE_SDL2
  poll_gamepad(fe);
#endif

  if (fe->smb_gl) {
    SMB_tick(smb_state);
    apply_classic_rules(smb_state);

    int draw_w = 0;
    int draw_h = 0;
#ifdef USE_SDL2
    SDL_GL_GetDrawableSize(fe->window, &draw_w, &draw_h);
#else
    SDL_GetWindowSizeInPixels(fe->window, &draw_w, &draw_h);
#endif
    SMBgl_set_viewport(fe->smb_gl, draw_w, draw_h);

    if (!SMBgl_render_frame(fe->smb_gl)) {
      log_error("Error rendering GL frame");
    }

    SDL_GL_SwapWindow(fe->window);
    return 0;
  } else {
    // raster framebuffer

    SDL_Surface *surf = fe->surf;
    
    SDL_LockSurface(surf);

    // draw to the buffer!
    // draw_tile is invoked for each tile during SMB_tick, and will update the pixel buffer
    SMBraster_set_buffer(fe->smb_raster, surf->pixels, fe->pixels_stride);
    SMBraster_clear(fe->smb_raster);
    SMB_tick(smb_state);
    apply_classic_rules(smb_state);

    SDL_UnlockSurface(surf);

    const SDL_Rect srcrect = {0, 0, 256, 240};
    SDL_Surface *win_surf = SDL_GetWindowSurface(fe->window);
    const int win_w = win_surf->w;
    const int win_h = win_surf->h;

    int dst_w = win_w;
    int dst_h = (win_w * 240) / 256;
    if (dst_h > win_h) {
      dst_h = win_h;
      dst_w = (win_h * 256) / 240;
    }

    SDL_Rect dstrect = {
      (win_w - dst_w) / 2,
      (win_h - dst_h) / 2,
      dst_w,
      dst_h
    };

#ifdef USE_SDL2
    SDL_FillRect(win_surf, NULL, 0);
    SDL_BlitScaled(surf, &srcrect, win_surf, &dstrect);
#else
    SDL_FillSurfaceRect(win_surf, NULL, 0);
    SDL_BlitSurfaceScaled(surf, &srcrect, win_surf, &dstrect, SDL_SCALEMODE_NEAREST);
#endif

    SDL_UpdateWindowSurface(fe->window);
    return 0;
  }
}

bool read_rom_bytes(void *userdata, u8 *buf, size_t size) {
  struct frontend_userdata *fe = userdata;
  return fread(buf, size, 1, fe->romfile) == 1;
}

bool seek_rom(void *userdata, size_t offset) {
  struct frontend_userdata *fe = userdata;
  fseek(fe->romfile, offset, SEEK_SET);
  return true;
}

void apu_write_register(void *userdata, u16 addr, u8 data) {
  struct frontend_userdata *fe = userdata;
  if (fe->audio) {
    SMB_audio_write_register(fe->audio, addr, data);
  }
}
void apu_end_frame(void *userdata) {
  struct frontend_userdata *fe = userdata;
  if (fe->audio) {
    SMB_audio_end_frame(fe->audio);
  }
}

// Go with one unashamedly-long main. (keep it simple!)

int main(int argc, char *argv[]) {
  /******** Declare/allocate some memory ********/

  struct SMB_state *smb_state = malloc(SMB_state_size());
  struct frontend_userdata *fe = malloc(sizeof(struct frontend_userdata));
  struct config cfg = {0};
  SDL_GLContext glcontext = 0;
  struct SMB_callbacks callbacks = {0};

  if (!smb_state || !fe) {
    goto exit;
  }
  memset(fe, 0, sizeof(struct frontend_userdata));

  if (!config_init(&cfg)) {
    goto exit;
  }

  fe->cfg = &cfg;


  /******** Config defaults ********/

  cfg.audio.enabled = true;
  cfg.audio.samplerate = 44100;
  cfg.audio.maxlatency_ms = 100;
  cfg.smb2j.override_games_beaten = -1;


  /******** Parse config, arguments ********/

  if (!parse_options("smbvanilla.ini", argc, argv, &cfg)) {
    log_error("Could not parse options");
    goto exit;
  }


  /******** Validate arguments ********/

  if (!cfg.general.rom_filename) {
    log_error("Please provide a SMB1 or SMB2J (Lost Levels) rom!");
    goto exit;
  }

  if (1) {
    SDL_Scancode *key_scancodes = &cfg.bindings.keys[0];
    fe->sdl_key_scancodes.u      = key_scancodes[0];
    fe->sdl_key_scancodes.d      = key_scancodes[1];
    fe->sdl_key_scancodes.l      = key_scancodes[2];
    fe->sdl_key_scancodes.r      = key_scancodes[3];
    fe->sdl_key_scancodes.select = key_scancodes[4];
    fe->sdl_key_scancodes.start  = key_scancodes[5];
    fe->sdl_key_scancodes.b      = key_scancodes[6];
    fe->sdl_key_scancodes.a      = key_scancodes[7];
  }

  if (cfg.smb2j.override_games_beaten >= 0) {
    fe->smb2j_games_beaten = (u8)cfg.smb2j.override_games_beaten;
  }


  /******** Initialize audio ********/

  if (cfg.audio.enabled) {
    fe->audio = malloc(SMB_audio_size());
  }

  if (fe->audio && !SMB_audio_init(fe->audio, cfg.audio.samplerate, cfg.audio.maxlatency_ms)) {
    log_error("Could not initialize audio!");
    free(fe->audio);
    fe->audio = 0;
  }


  /******** Initialize SDL / OpenGL ********/

  u8 palette_rgb[NUM_PALETTE_ENTRIES][3];

  memcpy(&palette_rgb[0][0], &PALETTE_BUILTIN[0][0], sizeof(palette_rgb));

  if (cfg.general.palette_filename) {
    if (!load_palette(&palette_rgb[0][0], cfg.general.palette_filename)) {
      log_info("Could not load palette from file %s. Using built-in palette.", cfg.general.palette_filename);
    }
  }

  fe->video_scale = 3;

  if (cfg.graphics.video_scale != 0) {
    fe->video_scale = cfg.graphics.video_scale;
  }

  fe->fullscreen = cfg.graphics.fullscreen;

#ifdef USE_SDL2
  if (SDL_Init(SDL_INIT_VIDEO) < 0) {
    log_error("Could not initialize SDL video: %s", SDL_GetError());
    goto exit;
  }

  if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) < 0) {
    log_info("Gamepad subsystem unavailable: %s", SDL_GetError());
  } else {
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
      if (SDL_IsGameController(i)) {
        fe->gamecontroller = SDL_GameControllerOpen(i);
        if (fe->gamecontroller) {
          log_info("Gamepad connected: %s", SDL_GameControllerName(fe->gamecontroller));
          break;
        }
      }
    }
  }
#else
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    log_error("Could not initialize SDL video: %s", SDL_GetError());
    goto exit;
  }
#endif

#ifdef USE_SDL2
  SDL_WindowFlags window_flags = SDL_WINDOW_SHOWN;
  if (fe->fullscreen) {
    window_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
  }
#else
  SDL_WindowFlags window_flags = 0;
  if (fe->fullscreen) {
    window_flags |= SDL_WINDOW_FULLSCREEN;
  }
#endif

#ifdef OPENGL_ENABLED
  if (cfg.graphics.opengl) {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

    window_flags |= SDL_WINDOW_OPENGL;
  }
#endif

#ifdef USE_SDL2
  fe->window = SDL_CreateWindow("SMB Vanilla", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 256 * fe->video_scale, 240 * fe->video_scale, window_flags);
#else
  fe->window = SDL_CreateWindow("SMB Vanilla", 256 * fe->video_scale, 240 * fe->video_scale, window_flags);
#endif
  if (!fe->window) {
    log_error("Could not create SDL window: %s", SDL_GetError());
    goto exit;
  }

#ifdef OPENGL_ENABLED
  if (cfg.graphics.opengl) {
    glcontext = SDL_GL_CreateContext(fe->window);

    if (cfg.general.maxspeed) {
      // We want to play as fast as possible, so disable any vsync
      SDL_GL_SetSwapInterval(0);
    }

    if (glcontext) {
      fe->smb_gl = malloc(SMBgl_size());

      if (!SMBgl_init(fe->smb_gl, sdl_gl_get_proc_address)) {
        log_error("Could not initialize OpenGL");
        free(fe->smb_gl);
        fe->smb_gl = 0;
#ifdef USE_SDL2
        SDL_GL_DeleteContext(glcontext);
#else
        SDL_GL_DestroyContext(glcontext);
#endif
        glcontext = 0;
      }
    }
  }
#endif

  if (cfg.graphics.software && !fe->smb_gl) {
    // Fallback to raster
    fe->smb_raster = malloc(SMBraster_size());
    if (!SMBraster_init(fe->smb_raster)) {
      log_error("Could not initialize software renderer");
      free(fe->smb_raster);
      fe->smb_raster = 0;
    }
  }

  if (fe->smb_gl) {
    log_info("Using OpenGL renderer");

    SMBgl_provide_palette_lookup(fe->smb_gl, &palette_rgb[0][0]);
  } else if (fe->smb_raster) {
    log_info("Using software renderer");
    SMBraster_provide_palette_lookup(fe->smb_raster, &palette_rgb[0][0]);
  } else {
    log_error("No renderer available");
    goto exit;
  }

  if (fe->smb_raster) {
#ifdef USE_SDL2
    fe->surf = SDL_CreateRGBSurface(0, 256, 240, 24, 0x0000FF, 0x00FF00, 0xFF0000, 0);
#else
    fe->surf = SDL_CreateSurface(256, 240, SDL_PIXELFORMAT_RGB24);
#endif
    if (!fe->surf) {
      log_error("Could not create SDL surface: %s", SDL_GetError());
      goto exit;
    }
    fe->pixels_stride = 256;
  }


  /******** Load ROM, initialize SMB state ********/

  fe->romfile = fopen(cfg.general.rom_filename, "rb");
  if (!fe->romfile) {
    log_error("Could not open ROM %s: %s", cfg.general.rom_filename, strerror(errno));
    goto exit;
  }

  callbacks.userdata = fe;

  callbacks.read_rom_bytes = read_rom_bytes;
  callbacks.seek_rom = seek_rom;
  callbacks.smb2j_load_games_beaten = smb2j_load_games_beaten;
  callbacks.smb2j_save_games_beaten = smb2j_save_games_beaten;

  callbacks.apu_write_register = apu_write_register;
  callbacks.apu_end_frame = apu_end_frame;

  callbacks.update_pattern_tables = update_pattern_tables;
  callbacks.update_palette = update_palette;
  callbacks.draw_tile = draw_tile;

  callbacks.joy1 = joy1;
  callbacks.joy2 = joy2;

  if (!SMB_state_init(smb_state, &callbacks)) {
    log_error("Could not initialize game");
    goto exit;
  }
  SMB_start_on_level(smb_state, cfg.world, cfg.level);
  fe->smb_state = smb_state;

  log_info("Started!");


  /******** Step through the movie, if one is provided ********/

  if (cfg.movie_prefix) {
    static const size_t filename_size = 1024;
    char *buttons_filename = 0;
    char *ram_filename = 0;

    buttons_filename = malloc(filename_size);
    snprintf(buttons_filename, filename_size, "%smovie-buttons.txt", cfg.movie_prefix);

    if (cfg.movie_readram) {
      ram_filename = malloc(filename_size);
      snprintf(ram_filename, filename_size, "%smovie-ram.bin", cfg.movie_prefix);
    }

    fe->movie = malloc(sizeof(struct Movie));

    if (!movie_init(fe->movie, buttons_filename, ram_filename)) {
      log_error("Could not load movie");
      free(fe->movie);
      fe->movie = 0;
    }
    if (ram_filename) {
      free(ram_filename);
    }
    if (buttons_filename) {
      free(buttons_filename);
    }
  }

  int skipto = cfg.movie_skip_to_before_frame;

  if (skipto > 0) {
    bool movie_is_valid = true;

    if (fe->movie) {
      for (int i = 0; i < skipto; i++) {
        if (advance_movie(fe)) {
          SMB_tick(smb_state);
        } else {
          movie_is_valid = false;
          break;
        }
      }
      if (movie_is_valid && cfg.movie_readram) {
        movie_readram(fe->movie, SMB_ram(smb_state));
      }
    }
  }


  /******** Start game loop ********/

  fe->rendering_enabled = true;

  // On an NTSC NES: clocks per second, divided by clocks per frame
  // (approx 60.0988 fps)
  const double fps = 1789773.0 / 29780.5;

  if (cfg.general.maxspeed) {
    while (1) {
      if (sdl_tick(fe) != 0) {
        break;
      }
    }
  } else {
    timer_run_at_frequency(fps, fe, sdl_tick);
  }


  /******** Cleanup ********/

  // a successful exit, as well as error conditions, jump here and finalize/free anything that needs to be
exit:

  if (smb_state) {
    free(smb_state);
  }
  if (fe) {
    if (fe->smb_gl) {
      SMBgl_fini(fe->smb_gl);
      free(fe->smb_gl);
    }
    if (fe->smb_raster) {
      SMBraster_fini(fe->smb_raster);
      free(fe->smb_raster);
    }
    if (fe->romfile) {
      fclose(fe->romfile);
    }
    if (fe->movie) {
      movie_fini(fe->movie);
      free(fe->movie);
    }
#ifdef USE_SDL2
    if (fe->gamecontroller) {
      SDL_GameControllerClose(fe->gamecontroller);
      fe->gamecontroller = NULL;
    }
#endif
    if (fe->window) {
      SDL_DestroyWindow(fe->window);
    }
    if (fe->audio) {
      SMB_audio_fini(fe->audio);
    }
    free(fe);
  }
  if (glcontext) {
#ifdef USE_SDL2
    SDL_GL_DeleteContext(glcontext);
#else
    SDL_GL_DestroyContext(glcontext);
#endif
  }
  SDL_Quit();

  return 0;
}
