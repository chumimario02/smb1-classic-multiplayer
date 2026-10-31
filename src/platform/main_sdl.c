#include "audio.h"
#include "smbcore/mario.h"
#include "movie.h"
#include "netplay.h"
#include "render_common.h"
#include "render_opengl.h"
#include "render_raster.h"
#include "timer.h"

#ifdef USE_SDL2
#  include <SDL.h>
#else
#  include <SDL3/SDL.h>
#endif
#include "palette_builtin.h"

#include <ctype.h>
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

enum menu_state {
  MENU_MAIN,
  MENU_MULTIPLAYER,
  MENU_JOIN,
  MENU_GAME
};

struct frontend_userdata {
  FILE *romfile;
  struct SMB_state *smb_state;
  struct SMB_audio *audio;
  struct Movie *movie;
  struct netplay *netplay;
  enum menu_state menu;
  int menu_selection;
  char join_code[8];
  int join_code_len;
  int skin;
  uint32_t last_ghost_send_ms;
  uint32_t last_ghost_seq;
  bool ghost_valid;
  int ghost_x;
  int ghost_y;
  int ghost_level;
  int ghost_draw_x;
  int ghost_draw_y;
  int ghost_sprite_count;
  int ghost_sprites[8][4];
  int ghost_skin;

  // Online co-op state
  int coop_level;
  int coop_last_status;
  int coop_last_star;
  bool coop_clear_sent;
  bool coop_local_cleared;
  bool coop_peer_cleared;
  bool coop_peer_dead;
  bool coop_hold;
  uint32_t coop_wait_start_ms;
  bool coop_dead_sent;






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
  bool pause_menu_active;
  int pause_menu_selection;
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

  // GameTimerCtrlTimer: never let the countdown reach the display, so the
  // on-screen clock stays at 999 instead of ticking to 998.
  ram[0x0787] = 0xFF;

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

static void draw_font_tile(struct frontend_userdata *fe, int x, int y, int ch, int palette) {
  int font_index;

  if (ch >= 'A' && ch <= 'Z') {
    font_index = 0x0A + (ch - 'A');
  } else if (ch >= '0' && ch <= '9') {
    font_index = ch - '0';
  } else if (ch == '-') {
    font_index = 0x28;
  } else {
    font_index = 0x24; // space
  }

  struct SMB_tile tile = {0};
  tile.x = x;
  tile.y = y;
  tile.tileidx = 0x100 + font_index;
  tile.paletteidx = palette;
  tile.extra_type = TILE_TYPE_BG;

  if (fe->smb_gl) {
    SMBgl_draw_tile(fe->smb_gl, tile);
  } else if (fe->smb_raster) {
    SMBraster_draw_tile(fe->smb_raster, tile);
  }
}

static void draw_font_text(struct frontend_userdata *fe, int x, int y, const char *text, int palette) {
  for (const char *p = text; *p; p++) {
    draw_font_tile(fe, x, y, *p, palette);
    x += 8;
  }
}

static void update_pause_menu(struct frontend_userdata *fe) {
  u8 *ram = SMB_ram(fe->smb_state);
  const bool paused = (ram[0x0776] & 1) != 0;

  if (paused && !fe->pause_menu_active) {
    fe->pause_menu_active = true;
    fe->pause_menu_selection = 0;
  } else if (!paused) {
    fe->pause_menu_active = false;
  }
}

static void draw_pause_menu(struct frontend_userdata *fe) {
  draw_font_text(fe, 108, 72, "PAUSA", 2);
  draw_font_text(fe, 92, 104, "CONTINUAR", 2);
  draw_font_text(fe, 72, 124, "MENU PRINCIPAL", 2);

  const int y = fe->pause_menu_selection == 0 ? 104 : 124;
  const int x = fe->pause_menu_selection == 0 ? 80 : 44;
  draw_font_text(fe, x, y, "-", 2);
}

static void draw_online_status(struct frontend_userdata *fe) {
  if (!fe->netplay) {
    return;
  }

  char line[64];

  if (netplay_error(fe->netplay)) {
    snprintf(line, sizeof(line), "ERROR RED");
  } else if (!netplay_connected(fe->netplay)) {
    snprintf(line, sizeof(line), "CONECTANDO...");
  } else if (!netplay_joined(fe->netplay)) {
    snprintf(line, sizeof(line), "CONECTADO");
  } else {
    const char *room = netplay_room(fe->netplay);
    snprintf(line, sizeof(line), "SALA %s", room ? room : "----");
  }

  draw_font_text(fe, 8, 48, line, 2);

  if (netplay_joined(fe->netplay)) {
    draw_font_text(fe, 8, 60,
                   netplay_has_peer(fe->netplay) ? "AMIGO CONECTADO" : "ESPERANDO...",
                   2);
  }
}

static void ensure_menu_palette(struct frontend_userdata *fe) {
  u8 *ppu = SMB_ppuram(fe->smb_state);

  for (int i = 0; i < 32; i++) {
    ppu[0x3F00 + i] = 0x0F;
  }

  // Palette 2: black background, white text.
  ppu[0x3F08] = 0x0F;
  ppu[0x3F09] = 0x30;
  ppu[0x3F0A] = 0x30;
  ppu[0x3F0B] = 0x0F;

  if (fe->smb_gl) {
    SMBgl_update_palette(fe->smb_gl, ppu + 0x3F00);
  } else if (fe->smb_raster) {
    SMBraster_update_palette(fe->smb_raster, ppu + 0x3F00);
  }
}

static const char *skin_names[4] = {"MARIO", "LUIGI", "HIELO", "FUEGO"};

// Recolors the player's sprite palette. SMB1 stores the player colors in sprite
// palette 0 (PPU $3F11-$3F13); overwriting them every frame gives us skins that
// also survive power-ups and level changes.
static void apply_skin(struct frontend_userdata *fe) {
  static const u8 skins[4][3] = {
    {0x16, 0x27, 0x18}, // MARIO: red shirt, skin, brown overalls
    {0x1A, 0x27, 0x18}, // LUIGI: green
    {0x21, 0x30, 0x11}, // HIELO: ice blue
    {0x37, 0x27, 0x16}, // FUEGO: white/red
  };

  const int skin = fe->skin & 3;
  u8 *ppu = SMB_ppuram(fe->smb_state);
  ppu[0x3F11] = skins[skin][0];
  ppu[0x3F12] = skins[skin][1];
  ppu[0x3F13] = skins[skin][2];

  // The partner's ghost uses sprite palette 2, so it can show their skin too.
  const int gskin = fe->ghost_skin & 3;
  ppu[0x3F19] = skins[gskin][0];
  ppu[0x3F1A] = skins[gskin][1];
  ppu[0x3F1B] = skins[gskin][2];
}

static void draw_menu(struct frontend_userdata *fe) {
  ensure_menu_palette(fe);

  draw_font_text(fe, 80, 56, "SMB1 CLASSIC", 2);

  if (fe->menu == MENU_MAIN) {
    draw_font_text(fe, 80, 112, "SINGLEPLAYER", 2);
    draw_font_text(fe, 84, 136, "MULTIPLAYER", 2);

    const int y = fe->menu_selection == 0 ? 112 : 136;
    draw_font_text(fe, fe->menu_selection == 0 ? 64 : 68, y, "-", 2);
    draw_font_text(fe, 72, 184, "ENTER = ELEGIR", 2);
  } else if (fe->menu == MENU_MULTIPLAYER) {
    draw_font_text(fe, 84, 88, "MULTIPLAYER", 2);
    draw_font_text(fe, 88, 112, "HOST GAME", 2);
    draw_font_text(fe, 88, 136, "JOIN GAME", 2);

    char skin_line[24];
    snprintf(skin_line, sizeof(skin_line), "SKIN: %s", skin_names[fe->skin % 4]);
    draw_font_text(fe, 72, 160, skin_line, 2);

    draw_font_text(fe, 108, 184, "BACK", 2);

    const int y = 112 + fe->menu_selection * 24;
    const int x = fe->menu_selection == 3 ? 96 : (fe->menu_selection == 2 ? 60 : 76);
    draw_font_text(fe, x, y, "-", 2);
  } else if (fe->menu == MENU_JOIN) {
    draw_font_text(fe, 88, 96, "JOIN GAME", 2);
    draw_font_text(fe, 88, 128, "CODE:", 2);

    for (int i = 0; i < 4; i++) {
      char c[2];
      c[0] = fe->join_code_len > i ? fe->join_code[i] : '-';
      c[1] = 0;
      draw_font_text(fe, 136 + i * 8, 128, c, 2);
    }

    draw_font_text(fe, 64, 168, "ENTER = CONECTAR", 2);
    draw_font_text(fe, 80, 192, "ESC = VOLVER", 2);
  }
}

static int present_menu(struct frontend_userdata *fe) {
  if (fe->smb_gl) {
    draw_menu(fe);

    int draw_w = 0;
    int draw_h = 0;
#ifdef USE_SDL2
    SDL_GL_GetDrawableSize(fe->window, &draw_w, &draw_h);
#else
    SDL_GetWindowSizeInPixels(fe->window, &draw_w, &draw_h);
#endif
    SMBgl_set_viewport(fe->smb_gl, draw_w, draw_h);

    if (!SMBgl_render_frame(fe->smb_gl)) {
      return 1;
    }

    SDL_GL_SwapWindow(fe->window);
    return 0;
  }

  if (!fe->smb_raster || !fe->surf) {
    return 1;
  }

  SDL_LockSurface(fe->surf);
  SMBraster_set_buffer(fe->smb_raster, fe->surf->pixels, fe->pixels_stride);
  SMBraster_clear(fe->smb_raster);
  draw_menu(fe);
  SDL_UnlockSurface(fe->surf);

  SDL_Surface *win_surf = SDL_GetWindowSurface(fe->window);
  const SDL_Rect srcrect = {0, 0, SMB_RENDER_WIDTH, SMB_RENDER_HEIGHT};
  SDL_Rect dstrect = {0, 0, win_surf->w, win_surf->h};
#ifdef USE_SDL2
  SDL_FillRect(win_surf, NULL, 0);
  SDL_BlitScaled(fe->surf, &srcrect, win_surf, &dstrect);
#else
  SDL_FillSurfaceRect(win_surf, NULL, 0);
  SDL_BlitSurfaceScaled(fe->surf, &srcrect, win_surf, &dstrect, SDL_SCALEMODE_NEAREST);
#endif
  SDL_UpdateWindowSurface(fe->window);

  return 0;
}

static void start_singleplayer(struct frontend_userdata *fe) {
  if (fe->netplay) {
    netplay_destroy(fe->netplay);
    fe->netplay = NULL;
  }

  fe->menu = MENU_GAME;
  SMB_return_to_title(fe->smb_state);
}

static void start_online(struct frontend_userdata *fe, bool host, const char *room) {
  if (fe->netplay) {
    netplay_destroy(fe->netplay);
    fe->netplay = NULL;
  }

  struct netplay_config npcfg;
  memset(&npcfg, 0, sizeof(npcfg));
  npcfg.server_url = fe->cfg->online_server;
  npcfg.room = room;
  npcfg.name = fe->cfg->online_name;
  npcfg.host = host;

  fe->netplay = netplay_create(&npcfg);
  fe->menu = MENU_GAME;
  SMB_return_to_title(fe->smb_state);

  // Start from the current power state so joining does not spam an upgrade.
  u8 *ram = SMB_ram(fe->smb_state);
  fe->coop_last_status = ram[0x0756];
  fe->coop_last_star = ram[0x079F];
  fe->coop_clear_sent = false;
  fe->coop_local_cleared = false;
  fe->coop_peer_cleared = false;
  fe->coop_peer_dead = false;
  fe->coop_dead_sent = false;
  fe->coop_hold = false;
}

static void return_to_main_menu(struct frontend_userdata *fe) {
  if (fe->netplay) {
    netplay_destroy(fe->netplay);
    fe->netplay = NULL;
  }

  SMB_set_hold_at_flag(fe->smb_state, false);
  fe->coop_hold = false;
  fe->ghost_valid = false;

  u8 *ram = SMB_ram(fe->smb_state);
  ram[0x0776] = 0;
  ram[0x0777] = 0;

  fe->pause_menu_active = false;
  fe->menu = MENU_MAIN;
  fe->menu_selection = 0;
}

static void handle_menu_key(struct frontend_userdata *fe, SDL_Scancode sc) {
  if (fe->menu == MENU_MAIN) {
    if (sc == SDL_SCANCODE_UP || sc == SDL_SCANCODE_W) {
      fe->menu_selection = 0;
    } else if (sc == SDL_SCANCODE_DOWN || sc == SDL_SCANCODE_S) {
      fe->menu_selection = 1;
    } else if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_SPACE) {
      if (fe->menu_selection == 0) {
        start_singleplayer(fe);
      } else {
        fe->menu = MENU_MULTIPLAYER;
        fe->menu_selection = 0;
      }
    }
  } else if (fe->menu == MENU_MULTIPLAYER) {
    if (sc == SDL_SCANCODE_UP || sc == SDL_SCANCODE_W) {
      if (fe->menu_selection > 0) fe->menu_selection--;
    } else if (sc == SDL_SCANCODE_DOWN || sc == SDL_SCANCODE_S) {
      if (fe->menu_selection < 3) fe->menu_selection++;
    } else if (sc == SDL_SCANCODE_LEFT || sc == SDL_SCANCODE_A ||
               sc == SDL_SCANCODE_RIGHT || sc == SDL_SCANCODE_D) {
      if (fe->menu_selection == 2) {
        const int dir = (sc == SDL_SCANCODE_LEFT || sc == SDL_SCANCODE_A) ? 3 : 1;
        fe->skin = (fe->skin + dir) % 4;
      }
    } else if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_SPACE) {
      if (fe->menu_selection == 0) {
        start_online(fe, true, NULL);
      } else if (fe->menu_selection == 1) {
        fe->menu = MENU_JOIN;
        fe->join_code_len = 0;
        fe->join_code[0] = 0;
      } else if (fe->menu_selection == 2) {
        fe->skin = (fe->skin + 1) % 4;
      } else {
        fe->menu = MENU_MAIN;
        fe->menu_selection = 0;
      }
    } else if (sc == SDL_SCANCODE_ESCAPE) {
      fe->menu = MENU_MAIN;
      fe->menu_selection = 0;
    }
  } else if (fe->menu == MENU_JOIN) {
    if (sc == SDL_SCANCODE_BACKSPACE) {
      if (fe->join_code_len > 0) {
        fe->join_code[--fe->join_code_len] = 0;
      }
    } else if (sc == SDL_SCANCODE_RETURN) {
      if (fe->join_code_len == 4) {
        start_online(fe, false, fe->join_code);
      }
    } else if (sc == SDL_SCANCODE_ESCAPE) {
      fe->menu = MENU_MULTIPLAYER;
      fe->menu_selection = 1;
    }
  }
}

static void send_ghost_state(struct frontend_userdata *fe, uint32_t now_ms) {
  if (!fe->netplay || !netplay_joined(fe->netplay) || !netplay_has_peer(fe->netplay)) {
    return;
  }

  if (now_ms - fe->last_ghost_send_ms < 33) {
    return;
  }
  fe->last_ghost_send_ms = now_ms;

  u8 *ram = SMB_ram(fe->smb_state);
  const int page = ram[0x006D];
  const int px = ram[0x0086];
  const int py = ram[0x00CE];
  const int spr_off = ram[0x06E4];

  if (spr_off > 0xF0) {
    return;
  }

  const int abs_x = page * 256 + px;
  const int cam_abs = ram[0x071A] * 256 + ram[0x071C];
  // SMB1 sprite coordinates are screen coordinates, so sprite offsets must be
  // relative to the player's *screen* position, not to its world position.
  const int screen_x = (abs_x - cam_abs) & 0xFF;
  const int base = 0x0200 + spr_off;
  const int level = ram[0x075F] * 10 + ram[0x075C];

  char data[768];
  int n = snprintf(data, sizeof(data),
                   "{\"kind\":\"g\",\"l\":%d,\"k\":%d,\"x\":%d,\"y\":%d,\"s\":[",
                   level, fe->skin & 3, abs_x, py);

  int count = 0;
  for (int i = 0; i < 8; i++) {
    const int oy = ram[base + i * 4] + 1;
    const int tile = ram[base + i * 4 + 1];
    const int attr = ram[base + i * 4 + 2];
    const int ox = ram[base + i * 4 + 3];

    if (tile == 0xFC || oy >= 240) {
      continue;
    }

    int dx = ox - screen_x;
    int dy = oy - py;
    if (dx > 128) dx -= 256;
    if (dx < -128) dx += 256;
    if (dy > 128) dy -= 256;
    if (dy < -128) dy += 256;

    n += snprintf(data + n, sizeof(data) - n, "%s[%d,%d,%d,%d]",
                  count ? "," : "", dx, dy, tile, attr);
    count++;
  }

  snprintf(data + n, sizeof(data) - n, "]}");
  netplay_send_relay(fe->netplay, data);
}

static void parse_ghost_data(struct frontend_userdata *fe, const char *json) {
  if (!json || !strstr(json, "\"kind\":\"g\"")) {
    return;
  }

  const char *p = strstr(json, "\"x\":");
  if (!p) return;
  const int x = atoi(p + 4);

  p = strstr(json, "\"y\":");
  if (!p) return;
  const int y = atoi(p + 4);

  p = strstr(json, "\"l\":");
  fe->ghost_level = p ? atoi(p + 4) : -1;

  p = strstr(json, "\"k\":");
  if (p) fe->ghost_skin = atoi(p + 4) & 3;

  fe->ghost_x = x;
  fe->ghost_y = y;

  if (!fe->ghost_valid) {
    fe->ghost_draw_x = x;
    fe->ghost_draw_y = y;
  }

  int count = 0;
  p = strstr(json, "\"s\":[");
  if (p) {
    p += 5;

    while (*p && *p != ']' && count < 8) {
      if (*p == '[') {
        int dx, dy, tile, attr;
        if (sscanf(p, "[%d,%d,%d,%d]", &dx, &dy, &tile, &attr) != 4) {
          break;
        }

        fe->ghost_sprites[count][0] = dx;
        fe->ghost_sprites[count][1] = dy;
        fe->ghost_sprites[count][2] = tile;
        fe->ghost_sprites[count][3] = attr;
        count++;

        p = strchr(p, ']');
        if (!p) break;
        p++;
      } else {
        p++;
      }
    }
  }

  fe->ghost_sprite_count = count;
  fe->ghost_valid = true;
}

static void handle_relay(struct frontend_userdata *fe, const char *json) {
  if (!json || json[0] != '{') {
    return;
  }

  if (strstr(json, "\"kind\":\"g\"")) {
    parse_ghost_data(fe, json);
    return;
  }

  if (strstr(json, "\"kind\":\"clr\"")) {
    const char *l = strstr(json, "\"l\":");
    const int level = l ? atoi(l + 4) : -1;
    u8 *ram = SMB_ram(fe->smb_state);
    const int local = ram[0x075F] * 10 + ram[0x075C];

    if (level < 0 || level == local) {
      fe->coop_peer_cleared = true;
    }
    return;
  }

  if (strstr(json, "\"kind\":\"dead\"")) {
    fe->coop_peer_dead = true;
    return;
  }

  if (strstr(json, "\"kind\":\"pw\"")) {
    const char *p = strstr(json, "\"p\":");
    if (p) {
      SMB_give_power(fe->smb_state, atoi(p + 4));
    }
    return;
  }
}

static void update_ghost(struct frontend_userdata *fe) {
  if (!fe->netplay || !netplay_has_peer(fe->netplay)) {
    fe->ghost_valid = false;
    return;
  }

  char relay[2048];
  if (!netplay_get_relay(fe->netplay, &fe->last_ghost_seq, relay, sizeof(relay))) {
    return;
  }

  handle_relay(fe, relay);
}

// True when the local player has reached the flagpole and is finishing the level.
static bool player_finished_level(const u8 *ram) {
  const u8 gr = ram[0x000E];
  return gr == 4 || gr == 5; // FlagpoleSlide / PlayerEndLevel
}

static bool player_is_dead(const u8 *ram) {
  const u8 gr = ram[0x000E];
  return gr == 6 || gr == 11; // PlayerLoseLife / PlayerDeath
}

static void send_coop_event(struct frontend_userdata *fe, const char *json) {
  if (!fe->netplay || !netplay_joined(fe->netplay) || !netplay_has_peer(fe->netplay)) {
    return;
  }
  netplay_send_relay(fe->netplay, json);
}

static void update_coop(struct frontend_userdata *fe, uint32_t now_ms) {
  u8 *ram = SMB_ram(fe->smb_state);
  const int level = ram[0x075F] * 10 + ram[0x075C];
  const bool online = fe->netplay && netplay_joined(fe->netplay) &&
                      netplay_has_peer(fe->netplay);

  // New level: forget the previous one's clear/death state.
  if (level != fe->coop_level) {
    fe->coop_level = level;
    fe->coop_local_cleared = false;
    fe->coop_peer_cleared = false;
    fe->coop_peer_dead = false;
    fe->coop_clear_sent = false;
    fe->coop_dead_sent = false;
    fe->coop_hold = false;

    fe->coop_wait_start_ms = now_ms;

  }

  // Shared power-ups: announce upgrades so the partner gets them too.
  if (online) {
    const int status = ram[0x0756];
    if (status > fe->coop_last_status) {
      char msg[64];
      snprintf(msg, sizeof(msg), "{\"kind\":\"pw\",\"p\":%d}", status);
      send_coop_event(fe, msg);
    }
    fe->coop_last_status = status;

    const int star = ram[0x079F];
    if (star > fe->coop_last_star) {
      send_coop_event(fe, "{\"kind\":\"pw\",\"p\":3}");
    }
    fe->coop_last_star = star;
  }

  // Reaching the flag: tell the partner and wait for them.
  const bool finished = player_finished_level(ram);

  if (finished && online && !fe->coop_clear_sent) {
    char msg[64];
    snprintf(msg, sizeof(msg), "{\"kind\":\"clr\",\"l\":%d}", level);
    send_coop_event(fe, msg);
    fe->coop_clear_sent = true;
    fe->coop_local_cleared = true;
    fe->coop_wait_start_ms = now_ms;
  } else if (finished) {
    fe->coop_local_cleared = true;
  } else if (!finished && !fe->coop_hold) {
    fe->coop_local_cleared = false;
    fe->coop_clear_sent = false;
  }

  const bool waited_too_long = now_ms - fe->coop_wait_start_ms > 60000;

  fe->coop_hold = online && fe->coop_local_cleared && !fe->coop_peer_cleared &&
                  fe->coop_peer_dead == false && !waited_too_long;

  SMB_set_hold_at_flag(fe->smb_state, fe->coop_hold);

  // Death: respawn on the screen where the partner is, instead of the checkpoint.
  if (online && player_is_dead(ram)) {
    if (!fe->coop_dead_sent) {
      send_coop_event(fe, "{\"kind\":\"dead\"}");
      fe->coop_dead_sent = true;

      if (fe->ghost_valid && fe->ghost_level == level) {
        // The game reloads the level around this page when it processes the
        // death, so the partner's screen is parsed and valid.
        SMB_set_respawn_page(fe->smb_state, (fe->ghost_x >> 8) & 0xFF);
      }
    }
  } else if (fe->coop_dead_sent && ram[0x000E] == 8) {
    fe->coop_dead_sent = false;
  }
}

static void draw_ghost(struct frontend_userdata *fe) {
  if (!fe->ghost_valid) {
    return;
  }

  u8 *ram = SMB_ram(fe->smb_state);
  const int level = ram[0x075F] * 10 + ram[0x075C];
  if (fe->ghost_level >= 0 && fe->ghost_level != level) {
    return; // partner is in another level
  }

  const int cam_abs = ram[0x071A] * 256 + ram[0x071C];

  const int dx = fe->ghost_x - fe->ghost_draw_x;
  const int dy = fe->ghost_y - fe->ghost_draw_y;

  if (abs(dx) > 48 || abs(dy) > 48) {
    // Big jump (level change, teleport): snap instead of gliding for ages.
    fe->ghost_draw_x = fe->ghost_x;
    fe->ghost_draw_y = fe->ghost_y;
  } else {
    fe->ghost_draw_x += (dx * 2) / 3;
    fe->ghost_draw_y += (dy * 2) / 3;
    if (abs(fe->ghost_x - fe->ghost_draw_x) <= 1) fe->ghost_draw_x = fe->ghost_x;
    if (abs(fe->ghost_y - fe->ghost_draw_y) <= 1) fe->ghost_draw_y = fe->ghost_y;
  }

  const int gx = fe->ghost_draw_x - cam_abs;
  const int gy = fe->ghost_draw_y;

  if (gx < -32 || gx > SMB_RENDER_WIDTH + 32) {
    return;
  }

  for (int i = 0; i < fe->ghost_sprite_count; i++) {
    const int sx = fe->ghost_sprites[i][0];
    const int sy = fe->ghost_sprites[i][1];
    const int tileidx = fe->ghost_sprites[i][2];
    const int attr = fe->ghost_sprites[i][3];

    if (tileidx == 0xFC) {
      continue;
    }

    struct SMB_tile tile = {0};
    tile.x = gx + sx;
    tile.y = gy + sy;
    tile.tileidx = tileidx;
    tile.paletteidx = 6; // ghost sprite palette
    tile.flip_horz = (attr & 0x40) != 0;
    tile.flip_vert = (attr & 0x80) != 0;
    tile.extra_type = TILE_TYPE_SPRITE;

    if (fe->smb_gl) {
      SMBgl_draw_tile(fe->smb_gl, tile);
    } else if (fe->smb_raster) {
      SMBraster_draw_tile(fe->smb_raster, tile);
    }
  }
}

// Called whenvere a key is newly pressed, or newly released
// Does not send repeat keys from holding it down
static int on_keypress_change(struct frontend_userdata *fe, SDL_Scancode sc, bool isdown) {
  struct SMB_state *smb_state = fe->smb_state;

  if (isdown) {
    if (sc == SDL_SCANCODE_F11) {
      set_fullscreen(fe, !fe->fullscreen);
      return 0;
    }
#ifdef USE_SDL2
    if (sc == SDL_SCANCODE_Q && (SDL_GetModState() & KMOD_ALT) != 0) {
#else
    if (sc == SDL_SCANCODE_Q && (SDL_GetModState() & SDL_KMOD_ALT) != 0) {
#endif
      set_fullscreen(fe, !fe->fullscreen);
      return 0;
    }
  }

  if (fe->menu != MENU_GAME) {
    if (isdown) {
      handle_menu_key(fe, sc);
    }
    return 0;
  }

  if (isdown) {
    u8 *ram = SMB_ram(smb_state);
    if ((ram[0x0776] & 1) != 0) {
      if (sc == SDL_SCANCODE_UP || sc == SDL_SCANCODE_W) {
        fe->pause_menu_selection = 0;
        return 0;
      }
      if (sc == SDL_SCANCODE_DOWN || sc == SDL_SCANCODE_S) {
        fe->pause_menu_selection = 1;
        return 0;
      }
      if (sc == SDL_SCANCODE_SPACE || sc == SDL_SCANCODE_X) {
        if (fe->pause_menu_selection == 1) {
          return_to_main_menu(fe);
        } else {
          ram[0x0776] = 0;
          ram[0x0777] = 0;
          fe->pause_menu_active = false;
        }
        return 0;
      }
    }

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

    case SDL_TEXTINPUT:
      if (fe->menu == MENU_JOIN) {
        for (const char *p = eventData.text.text; *p; p++) {
          const char c = (char)toupper((unsigned char)*p);
          if (((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) && fe->join_code_len < 4) {
            fe->join_code[fe->join_code_len++] = c;
            fe->join_code[fe->join_code_len] = 0;
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

    case SDL_MOUSEBUTTONDOWN:
      if (eventData.button.button == SDL_BUTTON_LEFT) {
        u8 *ram = SMB_ram(fe->smb_state);
        if ((ram[0x0776] & 1) != 0) {
          int ww = 0;
          int wh = 0;
          SDL_GetWindowSize(fe->window, &ww, &wh);

          const int mx = ww > 0 ? eventData.button.x * SMB_RENDER_WIDTH / ww : eventData.button.x;
          const int my = wh > 0 ? eventData.button.y * SMB_RENDER_HEIGHT / wh : eventData.button.y;

          if (mx >= 40 && mx <= 220 && my >= 116 && my <= 140) {
            return_to_main_menu(fe);
          } else if (mx >= 70 && mx <= 190 && my >= 94 && my <= 116) {
            ram[0x0776] = 0;
            ram[0x0777] = 0;
            fe->pause_menu_active = false;
          }
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

    case SDL_EVENT_TEXT_INPUT:
      if (fe->menu == MENU_JOIN) {
        for (const char *p = eventData.text.text; *p; p++) {
          const char c = (char)toupper((unsigned char)*p);
          if (((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) && fe->join_code_len < 4) {
            fe->join_code[fe->join_code_len++] = c;
            fe->join_code[fe->join_code_len] = 0;
          }
        }
      }
      break;

    case SDL_EVENT_QUIT:
      return 1;
    }
  }
#endif

  if (fe->menu != MENU_GAME) {
    return present_menu(fe);
  }

  // Overwrite the player buttons if a movie exists with remaining frames
  advance_movie(fe);

#ifdef USE_SDL2
  poll_gamepad(fe);
#endif

  if (fe->netplay) {
    netplay_update(fe->netplay, SDL_GetTicks());
  }

  if (fe->smb_gl) {
    apply_classic_rules(smb_state);
    apply_skin(fe);
    SMB_tick(smb_state);
    apply_classic_rules(smb_state);
    send_ghost_state(fe, SDL_GetTicks());
    update_ghost(fe);
    update_coop(fe, SDL_GetTicks());
    draw_ghost(fe);
    update_pause_menu(fe);
    if (fe->pause_menu_active) {
      draw_pause_menu(fe);
    }
    draw_online_status(fe);

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
    apply_classic_rules(smb_state);
    apply_skin(fe);
    SMB_tick(smb_state);
    apply_classic_rules(smb_state);
    send_ghost_state(fe, SDL_GetTicks());
    update_ghost(fe);
    update_coop(fe, SDL_GetTicks());
    draw_ghost(fe);
    update_pause_menu(fe);
    if (fe->pause_menu_active) {
      draw_pause_menu(fe);
    }
    draw_online_status(fe);

    SDL_UnlockSurface(surf);

    const SDL_Rect srcrect = {0, 0, SMB_RENDER_WIDTH, SMB_RENDER_HEIGHT};
    SDL_Surface *win_surf = SDL_GetWindowSurface(fe->window);
    SDL_Rect dstrect = {0, 0, win_surf->w, win_surf->h};

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
  fe->window = SDL_CreateWindow("SMB Vanilla", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, SMB_RENDER_WIDTH * fe->video_scale, SMB_RENDER_HEIGHT * fe->video_scale, window_flags);
#else
  fe->window = SDL_CreateWindow("SMB Vanilla", SMB_RENDER_WIDTH * fe->video_scale, SMB_RENDER_HEIGHT * fe->video_scale, window_flags);
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
    fe->surf = SDL_CreateRGBSurface(0, SMB_RENDER_WIDTH, SMB_RENDER_HEIGHT, 24, 0x0000FF, 0x00FF00, 0xFF0000, 0);
#else
    fe->surf = SDL_CreateSurface(SMB_RENDER_WIDTH, SMB_RENDER_HEIGHT, SDL_PIXELFORMAT_RGB24);
#endif
    if (!fe->surf) {
      log_error("Could not create SDL surface: %s", SDL_GetError());
      goto exit;
    }
    fe->pixels_stride = SMB_RENDER_WIDTH;
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

  if (cfg.online) {
    struct netplay_config npcfg;
    memset(&npcfg, 0, sizeof(npcfg));
    npcfg.server_url = cfg.online_server;
    npcfg.room = cfg.online_room;
    npcfg.name = cfg.online_name;
    npcfg.host = cfg.online_host;

    fe->netplay = netplay_create(&npcfg);
    fe->menu = MENU_GAME;
    log_info("Online: server=%s room=%s host=%d",
             cfg.online_server,
             cfg.online_room ? cfg.online_room : "(auto)",
             cfg.online_host);
  } else {
    fe->menu = MENU_MAIN;
  }

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
    if (fe->netplay) {
      netplay_destroy(fe->netplay);
      fe->netplay = NULL;
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

