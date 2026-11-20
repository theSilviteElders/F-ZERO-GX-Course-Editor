# Custom cups for F-Zero GX (GFZE01): code and data placed into a new main.dol
# section at BASE, right after the main stack; OSInit's arena start is moved
# behind it (the apploader only accepts sections below 0x80700000). See docs/cup-format.md. Assembled by tools/build_cup_patch.py,
# which defines BASE and the module offsets (M1_*, SEL_*) used below.
#
# Layout:   header | config (written by the editor) | runtime variables |
#           engine (OSLink hook, patch applier) | game hooks | patch table
#
# Custom cups use the game's unused cup indices 9 and 10 (E3 demo cups), so the
# game's own cup tables (11 rows) keep their size. Only Grand Prix offers them.

        .org BASE
header:
        .ascii "FZCE"
        .long 1                         # version
        .long config - BASE
        .long patches - BASE

# --- config (offsets are part of the editor <-> patch contract) --------------
        .align 32
config:
cfg_count:      .byte 0                 # number of custom cups (0..2)
                .byte 0, 0, 0
cfg_rows:       .space 24               # 2 cups x 6 s16 course ids (-1 = empty)
cfg_sprites:    .long 0xa102, 0xa102    # icon sprite per custom cup (0xa1NN = bmp_selgf.tpl index NN)
cfg_arrows:     .long 0xa131, 0x9a00    # page arrows: left (page 2; first texture appended to bmp_selgf.tpl), right (page 1)
cfg_names:      .space 64               # 2 x 32 bytes, NUL-terminated (cup select, intro)
cfg_names_uc:   .space 64               # the same in upper case (results screen)
cfg_nameptrs:   .long cfg_names, cfg_names, cfg_names, cfg_names, cfg_names, cfg_names
                .long cfg_names+32, cfg_names+32, cfg_names+32, cfg_names+32, cfg_names+32, cfg_names+32
cfg_nameptrs_uc: .long cfg_names_uc, cfg_names_uc+32
cfg_xpos:                               # icon x positions per icon count (6 halfwords per row)
                .short 0, 0, 0, 0, 0, 0
                .short 320, 0, 0, 0, 0, 0
                .short 200, 440, 0, 0, 0, 0
                .short 130, 320, 510, 0, 0, 0
                .short 98, 246, 394, 542, 0, 0
                .short 80, 200, 320, 440, 560, 0
                .short 80, 176, 272, 368, 464, 560
cfg_word_r25:   cmpwi 29, 10            # replaces "cmpwi r29, 5": cup index for name/highlight up to 10
        .set CFG_COUNT, cfg_count - config
        .set CFG_SPRITES, cfg_sprites - config
        .set CFG_ARROWS, cfg_arrows - config
        .set CFG_XPOS, cfg_xpos - config

# --- runtime variables ----------------------------------------------------------
        .align 32
vars:
v_setup:        .long 0                 # race setup struct (module 1 BSS)
v_table2:       .long 0                 # FindCup's cup table
v_page:         .long 0                 # 0 = no paging, 1 = original cups, 2 = custom cups
v_norig:        .long 0                 # number of original cup icons on page 1
v_list:         .space 32               # per-icon value list (cup-1, 0x7e/0x7f = arrows)
v_links:        .long 0                 # number of module links seen (debug)
        .set V_SETUP, v_setup - config
        .set V_PAGE, v_page - config
        .set V_NORIG, v_norig - config
        .set V_LIST, v_list - config

# --- engine ---------------------------------------------------------------------
        .align 4
# Replaces "bl Link" in OSLink/OSLinkFixed; r3 = module.
hook_link:
        stwu 1, -16(1)
        mflr 0
        stw 0, 20(1)
        stw 31, 12(1)
        mr 31, 3
        bl 0x8000e030
        cmpwi 3, 0
        beq link_done
        stw 3, 8(1)
        mr 3, 31
        bl apply
        lwz 3, 8(1)
link_done:
        lwz 31, 12(1)
        lwz 0, 20(1)
        mtlr 0
        addi 1, 1, 16
        blr

# apply(module): run the patch table entries of this module id.
# entry: u8 kind, u8 module, u8 section, u8 0, u32 offset, u32 addr, u32 extra
#   1 copy   extra bytes from addr to module(section, offset)
#   2 branch at module(section, offset) to addr (extra = 1: bl)
#   3 branch at addr back to module(section, offset)
#   5 store  module(section, offset) as u32 at addr
apply:
        stwu 1, -48(1)
        mflr 0
        stw 0, 52(1)
        stmw 26, 16(1)
        mr 31, 3
        lwz 30, 0(31)
        lis 29, @h(patches)
        ori 29, 29, @l(patches)
        lis 3, @h(v_links)
        ori 3, 3, @l(v_links)
        lwz 4, 0(3)
        addi 4, 4, 1
        stw 4, 0(3)
apply_loop:
        lbz 26, 0(29)
        cmpwi 26, 0
        beq apply_end
        lbz 4, 1(29)
        cmpw 4, 30
        bne apply_next
        lbz 4, 2(29)
        lwz 5, 0x10(31)
        slwi 4, 4, 3
        lwzx 6, 5, 4
        rlwinm 6, 6, 0, 0, 30
        lwz 7, 4(29)
        add 28, 6, 7
        lwz 27, 8(29)
        lwz 8, 12(29)
        cmpwi 26, 1
        beq k_copy
        cmpwi 26, 2
        beq k_branch
        cmpwi 26, 3
        beq k_back
        cmpwi 26, 5
        beq k_store
        b apply_next
k_copy:
        mtctr 8
        addi 9, 27, -1
        addi 10, 28, -1
copy_loop:
        lbzu 0, 1(9)
        stbu 0, 1(10)
        bdnz copy_loop
        mr 3, 28
        mr 4, 8
        bl flush
        b apply_next
k_branch:
        subf 0, 28, 27
        rlwinm 0, 0, 0, 6, 29
        oris 0, 0, 0x4800
        or 0, 0, 8
        stw 0, 0(28)
        mr 3, 28
        li 4, 4
        bl flush
        b apply_next
k_back:
        subf 0, 27, 28
        rlwinm 0, 0, 0, 6, 29
        oris 0, 0, 0x4800
        stw 0, 0(27)
        mr 3, 27
        li 4, 4
        bl flush
        b apply_next
k_store:
        stw 28, 0(27)
apply_next:
        addi 29, 29, 16
        b apply_loop
apply_end:
        lmw 26, 16(1)
        lwz 0, 52(1)
        mtlr 0
        addi 1, 1, 48
        blr

# flush(addr r3, len r4): write back data cache, invalidate instruction cache.
flush:
        add 4, 3, 4
        rlwinm 3, 3, 0, 0, 26
flush_loop:
        dcbst 0, 3
        sync
        icbi 0, 3
        addi 3, 3, 32
        cmplw 3, 4
        blt flush_loop
        sync
        isync
        blr

# --- helpers --------------------------------------------------------------------
# r11 = config, cr7 = "cup r3 is an enabled custom cup and the mode is Grand Prix"
# (sets cr7 eq). Clobbers r11, r12.
is_custom:
        lis 11, @h(config)
        ori 11, 11, @l(config)
        cmpwi cr7, 3, 9
        blt cr7, not_custom
        lbz 12, CFG_COUNT(11)
        addi 12, 12, 9
        cmpw cr7, 3, 12
        bge cr7, not_custom
        lwz 12, V_SETUP(11)
        lha 12, 0(12)
        cmpwi cr7, 12, 1
        blr
not_custom:
        cmpwi cr7, 11, 0                # r11 != 0: ne
        blr

# --- game hooks: cup select (fze.sel.rel, module 10) ---------------------------
# Cursor step, reject path "li r0, 0" (r28 = candidate cup); continues at +4.
hk_step:
        mflr 0
        mr 10, 0
        mr 3, 28
        bl is_custom
        mtlr 10
        li 0, 0
        bne cr7, hk_step_back
        li 0, 1
hk_step_back:
        b hk_step_back                             # patched: back to cursor step

# CupSelectable(), reject path "li r3, 0" (r31 = cup); continues at +4.
hk_sel:
        mflr 10
        extsh 3, 31
        bl is_custom
        mtlr 10
        li 3, 0
        bne cr7, hk_sel_back
        li 3, 1
hk_sel_back:
        b hk_sel_back

# Icon count (bl at "lis r3, -0x7fc0" after the count is known; r18 = count of
# original cups, r15 = race setup).
hk_count:
        lis 11, @h(config)
        ori 11, 11, @l(config)
        li 12, 0
        lbz 0, CFG_COUNT(11)
        cmpwi cr7, 0, 0
        beq cr7, count_done
        lha 12, 0(15)
        cmpwi cr7, 12, 1                # Grand Prix only
        li 12, 0
        bne cr7, count_done
        stw 18, V_NORIG(11)
        lha 12, 0x8c(15)
        cmpwi cr7, 12, 9
        li 12, 1
        blt cr7, count_page1
        lbz 18, CFG_COUNT(11)           # page 2: back arrow + custom cups
        addi 18, 18, 1
        li 12, 2
        b count_done
count_page1:
        addi 18, 18, 1                  # page 1: original cups + arrow
count_done:
        stw 12, V_PAGE(11)
        lis 3, -0x7fc0                  # replaced instruction
        blr

# Icon loop setup (bl at "li r20, 0"): r23 = list of per-icon values.
hk_list:
        li 20, 0
        lis 11, @h(config)
        ori 11, 11, @l(config)
        lwz 12, V_PAGE(11)
        cmpwi cr7, 12, 0
        beqlr 7
        addi 10, 11, V_LIST
        cmpwi cr7, 12, 2
        beq cr7, list_page2
        lwz 12, V_NORIG(11)
        mtctr 12
        addi 9, 23, -4
        addi 8, 10, -4
list_copy:
        lwzu 0, 4(9)
        stwu 0, 4(8)
        bdnz list_copy
        li 0, 0x7f
        stw 0, 4(8)
        mr 23, 10
        blr
list_page2:
        li 0, 0x7e
        stw 0, 0(10)
        lbz 12, CFG_COUNT(11)
        mtctr 12
        li 9, 8                         # cup 9 -> value 8
        mr 8, 10
list_fill:
        stwu 9, 4(8)
        addi 9, 9, 1
        bdnz list_fill
        mr 23, 10
        blr

# Icon x position (bl at "lhzx r18, r22, r4"); r20 = icon index, r21 = count.
# cr0 and r0 are live here.
hk_xpos:
        lis 11, @h(config)
        ori 11, 11, @l(config)
        lwz 12, V_PAGE(11)
        cmpwi cr7, 12, 0
        bne cr7, xpos_own
        lhzx 18, 22, 4
        blr
xpos_own:
        mulli 12, 21, 12
        addi 12, 12, CFG_XPOS
        add 12, 12, 11
        slwi 18, 20, 1
        lhzx 18, 12, 18
        blr

# Icon sprite (bl at "lwzx r4, r4, r0"); r0 = value * 4.
hk_sprite:
        cmplwi cr7, 0, 0x1f8
        bge cr7, sprite_arrow
        cmplwi cr7, 0, 32
        bge cr7, sprite_custom
        lwzx 4, 4, 0
        blr
sprite_custom:
        lis 4, @h(config)
        ori 4, 4, @l(config)
        add 4, 4, 0
        lwz 4, CFG_SPRITES-32(4)
        blr
sprite_arrow:
        lis 4, @h(config)
        ori 4, 4, @l(config)
        add 4, 4, 0
        lwz 4, CFG_ARROWS-0x1f8(4)
        blr

# Icon size (bl at "lhzx r0, r14, r27"); r27 = count * 2.
hk_size:
        cmplwi cr7, 27, 6
        blt cr7, size_big
        cmplwi cr7, 27, 12
        bge cr7, size_small
        lhzx 0, 14, 27
        blr
size_big:
        li 0, 176
        blr
size_small:
        li 0, 100
        blr

# --- game hooks: main module (module 1) -----------------------------------------
# FindCup(course r3, &cup r4, &slot r5): in a custom-cup Grand Prix, search the
# active cup first so that courses shared with an original cup map to it.
hk_find:
        mflr 0
        mr 9, 0
        lis 11, @h(config)
        ori 11, 11, @l(config)
        lwz 12, V_SETUP(11)
        lha 6, 0x8c(12)
        mr 7, 3
        mr 3, 6
        bl is_custom
        mr 3, 7
        mtlr 9
        bne cr7, find_orig
        lis 7, @h(v_table2)
        ori 7, 7, @l(v_table2)
        lwz 7, 0(7)
        mulli 8, 6, 12
        add 7, 7, 8
        extsh 10, 3
        li 8, 0
find_loop:
        lha 9, 0(7)
        cmpw cr7, 9, 10
        beq cr7, find_found
        addi 7, 7, 2
        addi 8, 8, 1
        cmpwi cr7, 8, 6
        blt cr7, find_loop
find_orig:
        stwu 1, -0x90(1)                # replaced instruction
find_back:
        b find_back
find_found:
        sth 6, 0(4)
        sth 8, 0(5)
        blr

# --- patch table ------------------------------------------------------------------
        .align 4
patches:
        # module 1 (main): addresses, cup rows, FindCup
        .byte 5, 1, 6, 0
        .long M1_SETUP, v_setup, 0
        .byte 5, 1, 4, 0
        .long M1_TABLE2, v_table2, 0
        .byte 1, 1, 4, 0
        .long M1_TABLE1+108, cfg_rows, 24
        .byte 1, 1, 4, 0
        .long M1_TABLE2+108, cfg_rows, 24
        .byte 1, 1, 4, 0
        .long M1_TABLE3+108, cfg_rows, 24
        .byte 2, 1, 1, 0
        .long M1_FINDCUP, hk_find, 0
        .byte 3, 1, 1, 0
        .long M1_FINDCUP+4, find_back, 0
        .byte 1, 1, 4, 0                # cup -> icon sprite (intro, course screen)
        .long M1_CUP_SPRITES+36, cfg_sprites, 8
        # module 15 (fze.winning.rel): results screen names [language][cup], English row
        .byte 1, 15, 5, 0
        .long WIN_CUP_NAMES+36, cfg_nameptrs_uc, 8
        # module 10 (fze.sel.rel): cup select
        .byte 2, 10, 1, 0
        .long SEL_STEP_REJECT, hk_step, 0
        .byte 3, 10, 1, 0
        .long SEL_STEP_REJECT+4, hk_step_back, 0
        .byte 2, 10, 1, 0
        .long SEL_SELECTABLE_REJECT, hk_sel, 0
        .byte 3, 10, 1, 0
        .long SEL_SELECTABLE_REJECT+4, hk_sel_back, 0
        .byte 1, 10, 1, 0
        .long SEL_R25_CLAMP, cfg_word_r25, 4
        .byte 2, 10, 1, 0
        .long SEL_COUNT, hk_count, 1
        .byte 2, 10, 1, 0
        .long SEL_LIST, hk_list, 1
        .byte 2, 10, 1, 0
        .long SEL_XPOS, hk_xpos, 1
        .byte 2, 10, 1, 0
        .long SEL_SPRITE, hk_sprite, 1
        .byte 2, 10, 1, 0
        .long SEL_SIZE, hk_size, 1
        .byte 1, 10, 5, 0
        .long SEL_NAME_E3_0, cfg_nameptrs, 48
        .byte 1, 10, 4, 0
        .long SEL_CUP_SPRITES+36, cfg_sprites, 8
        .long 0, 0, 0, 0
patches_end:
