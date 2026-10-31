// The start of something magical! ✨

#include "mario.h"
#include "base.h"
#include "interface.h"
#include "vars.h"
#include "consts.h"
#include "types.h"
#include "tiles.h"

#include <string.h>

void smb1_Reset(void);
void smb1_NMI(void);

bool smb2j_load_file(struct SMB_state *state, const char *name);
void smb2j_Reset(void);
void smb2j_NMI(void);

void smb1_sync_data(void);
void smb2j_sync_data(void);

void smb1_set_world_and_level(u8 world, u8 level);
void smb2j_set_world_and_level(u8 world, u8 level);


struct sprite {
  struct SMB_tile tile;
  bool draw_behind;
};

static inline void transfer_sprite_data(struct sprite sprites[64], const u8 *data) {
  for (int i = 0; i < 64; i++) {
    sprites[i].tile.y = data[i * 4 + 0] + 1;
    sprites[i].tile.tileidx = data[i * 4 + 1];
    u8 attr = data[i * 4 + 2];
    sprites[i].tile.x = data[i * 4 + 3];

    sprites[i].tile.paletteidx = (attr & 0x03) + 4;
    sprites[i].tile.flip_horz = (attr & 0x40) != 0;
    sprites[i].tile.flip_vert = (attr & 0x80) != 0;

    sprites[i].tile.extra_type = TILE_TYPE_SPRITE;
    sprites[i].tile.extra_spriteidx = i;

    sprites[i].draw_behind = (attr & 0x20) != 0;
  }
}

static void draw_graphics(const struct sprite sprites[64]);

#ifdef THREAD_LOCAL_SMBSTATE
thread_local struct SMB_state *SMB_STATE;
#else
struct SMB_state *SMB_STATE;
#endif

static bool load_smb1(struct SMB_state *state, size_t prg_offset, size_t chr_offset) {
  if (!seek_rom(state, prg_offset)) { return false; }
  if (!read_rom_bytes(state, state->rammem + 0x8000, 0x8000)) { return false; }
  if (!seek_rom(state, chr_offset)) { return false; }
  if (!read_rom_bytes(state, state->chrrom, 0x2000)) { return false; }
  state->which_game = GAME_SMB1;
  update_pattern_tables(state);
  return true;
}


struct fds_file {
  const char *name;
  size_t file_offset;
  u16 size;
  u16 org;
  int type;
};

#define TYPE_PRGRAM 0
#define TYPE_CHRRAM 1

#define SMB2J_FDS_FILES_COUNT 6

// hard-code some offsets for now
// the offsets are relative to after the 16-byte FDS header
static struct fds_file SMB2J_FDS_FILES[SMB2J_FDS_FILES_COUNT] = {
  {"SM2CHAR1", 0x013C, 0x2000, 0x0000, TYPE_CHRRAM},
  {"SM2CHAR2", 0x214D, 0x0040, 0x0760, TYPE_CHRRAM},
  {"SM2MAIN ", 0x219E, 0x8000, 0x6000, TYPE_PRGRAM},
  {"SM2DATA2", 0xA1AF, 0x0E2F, 0xC470, TYPE_PRGRAM},
  {"SM2DATA3", 0xAFEF, 0x0CCF, 0xC5D0, TYPE_PRGRAM},
  {"SM2DATA4", 0xBCCF, 0x0F4C, 0xC2B4, TYPE_PRGRAM},
};

bool smb2j_load_file(struct SMB_state *state, const char *name) {
  if (strncmp(name, "SM2SAVE ", 8) == 0) {
    // treat the save byte specially

    state->rammem[0xD29F] = smb2j_load_games_beaten();

    return true;
  }

  for (int i = 0; i < SMB2J_FDS_FILES_COUNT; i++) {
    const struct fds_file *a = &SMB2J_FDS_FILES[i];
    bool eq = strncmp(name, a->name, 8) == 0;
    if (eq) {
      // Found it!
      u8 *copy_to;
      if (a->type == TYPE_CHRRAM) {
        // Copy the bytes over to CHRRAM
        copy_to = state->chrrom + a->org;
      } else if (a->type == TYPE_PRGRAM) {
        // Copy the bytes over to RAM
        copy_to = state->rammem + a->org;
      }
      if (!seek_rom(state, state->smb2j_disk_offset + a->file_offset)) { return false; }
      if (!read_rom_bytes(state, copy_to, a->size)) { return false; }

      if (a->type == TYPE_CHRRAM) {
        update_pattern_tables(state);
      }

      return true;
    }
  }
  return false;
}

bool load_smb2j(struct SMB_state *state, size_t disk_offset) {
  // In this case, the 16 bit header is this:
  static char fds_disk_verification[16] = {0x01, '*', 'N', 'I', 'N', 'T', 'E', 'N', 'D', 'O', '-', 'H', 'V', 'C', '*', 0x01};
  u8 headerbuf[16];
  if (!seek_rom(state, disk_offset)) { return false; }
  if (!read_rom_bytes(state, headerbuf, 16)) { return false; }

  bool match = memcmp(headerbuf, fds_disk_verification, 16) == 0;
  if (!match) { return false; }
  state->which_game = GAME_SMB2J;
  state->smb2j_disk_offset = disk_offset;

  // Load these automatically
  smb2j_load_file(state, "SM2CHAR1");
  smb2j_load_file(state, "SM2MAIN ");
  smb2j_load_file(state, "SM2SAVE ");
  return true;
}


static bool detect_and_load_rom(struct SMB_state *state) {
  u8 headerbuf[16];
  if (!read_rom_bytes(state, headerbuf, 16)) {
    return false;
  }

  if (headerbuf[0] == 'N' && headerbuf[1] == 'E' && headerbuf[2] == 'S' && headerbuf[3] == 0x1A) {
    // An INES header. Probably SMB1.
    if (!load_smb1(state, 0x10, 0x8010)) {
      return false;
    }
  } else if (headerbuf[0] == 'F' && headerbuf[1] == 'D' && headerbuf[2] == 'S' && headerbuf[3] == 0x1A) {
    // An FDS header. Probably SMB2J.
    // TODO: there's an FDS version of SMB1. Maybe handle it?
    if (!load_smb2j(state, 0x10)) {
      return false;
    }
  } else {
    // There are some ROMs that don't include an FDS header, but just the single disk image.
    if (!load_smb2j(state, 0)) {
      return false;
    }
  }

  SMB_ram_finishwrite(state);

  return true;
}

size_t SMB_state_size(void) { return sizeof(struct SMB_state); }

bool SMB_state_init(struct SMB_state *state, const struct SMB_callbacks *cb) {
  SMB_STATE = state;

  memset(state, 0, sizeof(struct SMB_state));
  state->callbacks = *cb;
  state->start_on_world = 1;
  state->start_on_level = 1;
  state->respawn_page = -1;

  return detect_and_load_rom(state);
}

void SMB_start_on_level(struct SMB_state *state, u8 world, u8 level) {
  state->start_on_world = world;
  state->start_on_level = level;
}

void SMB_return_to_title(struct SMB_state *state) {
  state->return_to_title = true;
  state->reset_occurred = false;
}

int SMB_which_game(const struct SMB_state *state) { return state->which_game; }

u8 *SMB_ram(struct SMB_state *state) { return state->rammem; }

u8 *SMB_ppuram(struct SMB_state *state) { return state->ppuram; }

void SMB_ram_finishwrite(struct SMB_state *state) {
  // Set a global variable while accessing the RAM values
  SMB_STATE = state;

  if (state->which_game == GAME_SMB1) {
    smb1_sync_data();
  }
  if (state->which_game == GAME_SMB2J) {
    smb2j_sync_data();
  }
}

void smb1_GetPlayerColors(void);
void smb1_WriteBlockMetatile(const u8 metatile, const u16 mt_x, const u16 mt_y);
void smb1_MushFlowerBlock(const u8 param_1);
void smb1_CoinBlock(const u8 param_1);
void smb1_ExtraLifeMushBlock(const u8 param_1);
void smb1_StarBlock(const u8 param_1);
void smb1_EraseEnemyObject(const u8 param_1);

void SMB_set_hold_at_flag(struct SMB_state *state, bool hold) {
  state->hold_at_flag = hold;
}

void SMB_set_respawn_page(struct SMB_state *state, int page) {
  state->respawn_page = page;
}

bool SMB_take_eco_break(struct SMB_state *state, int *world_x, int *row, int *metatile) {  if (!state->eco_break_pending) {
    return false;
  }

  state->eco_break_pending = false;

  if (world_x) *world_x = state->eco_break_x;
  if (row) *row = state->eco_break_row;
  if (metatile) *metatile = state->eco_break_metatile;

  return true;
}

bool SMB_eco_break_at(struct SMB_state *state, int world_x, int row, int metatile) {
  if (state->which_game != GAME_SMB1) {
    return false;
  }

  SMB_STATE = state;

  if (row < 0 || row >= 13 || world_x < 0) {
    return false;
  }

  const u16 mt_x = (u16)(world_x / 16);
  const u16 mt_y = (u16)row;

  // The block buffer only holds the two screens around the camera. If the
  // partner's block is outside of it, leave it alone: it will be parsed from
  // the level data when it scrolls in.
  const int cam_col = (ScreenLeft_PageLoc & ~1) * 16;
  if ((int)mt_x < cam_col || (int)mt_x >= cam_col + 32) {
    return false;
  }

  // If this is a "?" block in our world, make the item pop out here too, so
  // both players see the mushroom/flower/coin.
  const u8 was = get_metatile(mt_x, mt_y);
  if ((u8)metatile == MT_BLOCK_EMPTY && metatile_is_itemblock(was)) {
    // Point the block object at the block we are opening.
    Block_PageLoc[0] = (u8)(world_x >> 8);
    Block_X_Position[0] = (u8)(world_x & 0xFF);
    Block_Y_HighPos[0] = 1;
    Block_Y_Position[0] = (u8)((row + MT_Y_TOP_MARGIN) * 16);
    Block_X_Speed[0] = 0;
    Block_Y_Speed[0] = 0;

    switch (was) {
    case MT_QUESTIONBLOCK_POWERUP:
    case MT_BRICK_2_POWERUP:
    case MT_BRICK_POWERUP:
      smb1_MushFlowerBlock(0);
      break;
    case MT_QUESTIONBLOCK_COIN:
    case MT_HIDDEN_1COIN:
    case MT_BRICK_2_COINS:
    case MT_BRICK_COINS:
      smb1_CoinBlock(0);
      break;
    case MT_HIDDEN_1UP:
    case MT_BRICK_2_1UP:
    case MT_BRICK_1UP:
      smb1_ExtraLifeMushBlock(0);
      break;
    case MT_BRICK_2_STAR:
    case MT_BRICK_STAR:
      smb1_StarBlock(0);
      break;
    default:
      break;
    }
  }

  set_metatile(mt_x, mt_y, (u8)metatile);
  smb1_WriteBlockMetatile((u8)metatile, mt_x, mt_y);

  return true;
}

bool SMB_take_item_pickup(struct SMB_state *state, int *world_x, int *y) {
  if (!state->eco_item_pending) {
    return false;
  }

  state->eco_item_pending = false;

  if (world_x) *world_x = state->eco_item_x;
  if (y) *y = state->eco_item_y;

  return true;
}

void SMB_remove_item_at(struct SMB_state *state, int world_x, int y) {
  if (state->which_game != GAME_SMB1) {
    return;
  }

  SMB_STATE = state;

  (void)world_x;
  (void)y;

  // Each world only has one power-up on screen at a time and it always lives in
  // sprite slot 5, so identity is enough: positions drift between the two
  // worlds while the item pops out and falls.
  if (Enemy_ID[5] == A_POWERUP) {
    smb1_EraseEnemyObject(5);
  }
}

void SMB_give_power(struct SMB_state *state, int power) {
  if (state->which_game != GAME_SMB1) {
    return;
  }

  // The macros below read the global SMB_STATE.
  SMB_STATE = state;

  if (power == 3) {
    // Star: temporary invincibility.
    StarInvincibleTimer = 0x23;
    return;
  }

  if (power < 1 || power > 2) {
    return;
  }

  if (PlayerStatus >= power) {
    return; // never downgrade
  }

  if (GameEngineSubroutine != GR_UNK_8) {
    return; // only while the player is in normal control
  }

  PlayerStatus = (u8)power;
  Player_State = PLAYERSTATE_UNK_0;
  TimerControl = 0xff;
  ScrollAmount = 0;
  GameEngineSubroutine = (power == 2) ? GR_UNK_12 : GR_UNK_9;

  smb1_GetPlayerColors();
}

void SMB_tick(struct SMB_state *state) {
  // Set a global variable while it's being ticked
  SMB_STATE = state;

  struct sprite sprites[64];

  if (state->which_game == GAME_SMB1) {
    if (!state->reset_occurred) {
      smb1_Reset();
      if (state->return_to_title) {
        state->return_to_title = false;
        state->reset_occurred = true;
      }
    }

    // The NES wrote to OAM registers to initiate copying sprites
    // $2003 = 0
    // $4014 = 2
    transfer_sprite_data(&sprites[0], &Sprite_Data[0]);
    smb1_NMI();

    if (!state->reset_occurred) {
      smb1_set_world_and_level(state->start_on_world - 1, state->start_on_level - 1);
      state->reset_occurred = true;
    }
  }

  if (state->which_game == GAME_SMB2J) {
    if (!state->reset_occurred) {
      smb2j_Reset();
      if (state->return_to_title) {
        state->return_to_title = false;
        state->reset_occurred = true;
      }
    }

    // The NES wrote to OAM registers to initiate copying sprites
    // $2003 = 0
    // $4014 = 2
    transfer_sprite_data(&sprites[0], &Sprite_Data[0]);
    smb2j_NMI();

    if (!state->reset_occurred) {
      smb2j_set_world_and_level(state->start_on_world - 1, state->start_on_level - 1);
      state->reset_occurred = true;
    }
  }

  draw_graphics(&sprites[0]);
}

static inline void draw_nametable_tile(int x, int y, u16 ppu_offset, int tilex,
                                       int tiley) {
  int j = tilex;
  int i = tiley;
  int off = i * 32 + j;
  int tileidx = PPURAM(ppu_offset + off) + 0x100;
  // Find which part of the pallete to use
  // Look it up in the attribute table

  u8 a = PPURAM(ppu_offset + 0x03c0 + (i / 4) * 8 + (j / 4));
  a >>= ((j / 2) % 2) * 2;
  a >>= ((i / 2) % 2) * 4;
  a &= 0x03;
  int paletteidx = a;

  struct SMB_tile tile;
  tile.x = x;
  tile.y = y;
  tile.tileidx = tileidx;
  tile.paletteidx = paletteidx;
  tile.flip_horz = false;
  tile.flip_vert = false;
  tile.extra_type = TILE_TYPE_BG;
  tile.extra_bg.x = tilex;
  tile.extra_bg.y = tilex;

  draw_tile(tile);
}

static inline void draw_nametable_rect(int x, int y, u16 ppu_offset, int fromx,
                                       int fromy, int tox, int toy) {
  for (int i = fromy; i < toy; i++) {
    for (int j = fromx; j < tox; j++) {
      draw_nametable_tile(x + (j - fromx) * 8, y + (i - fromy) * 8, ppu_offset, j, i);
    }
  }
}

void draw_graphics(const struct sprite sprites[64]) {
  // The palette can change even if the screen is off (the background color, namely)
  update_palette();

  if (!PPU_STATE.screen_on) {
    return;
  }
  if (!can_draw_tile()) {
    return;
  }

  for (int spriteidx = 64-1; spriteidx >= 0; spriteidx--) {
    if (sprites[spriteidx].draw_behind) {
      draw_tile(sprites[spriteidx].tile);
    }
  }

  const u16 scroll_x = PPU_STATE.scroll_x;

  // Status bar
  draw_nametable_rect(0, 0, 0x2000, 0, 0, 32, 4);

  int scroll_off = -(scroll_x % 256);
  int scroll_nametable = scroll_x / 256;

  if (scroll_nametable == 0) {
    draw_nametable_rect(scroll_off, 32, 0x2000, 0, 4, 32, 30);
    draw_nametable_rect(256 + scroll_off, 32, 0x2400, 0, 4, 32, 30);
  } else {
    draw_nametable_rect(scroll_off, 32, 0x2400, 0, 4, 32, 30);
    draw_nametable_rect(256 + scroll_off, 32, 0x2000, 0, 4, 32, 30);
  }

  for (int spriteidx = 64-1; spriteidx >= 0; spriteidx--) {
    if (!sprites[spriteidx].draw_behind) {
      draw_tile(sprites[spriteidx].tile);
    }
  }
}
