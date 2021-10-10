.macpack longbranch
.import __sprite_list, _irq_setup_done, _is_pal, _game_clock, _sprite_count, _main_raster_irq
.importzp ptr1
.include "c64.inc"
.interruptor raster_irq, 2

.define IRQ_NOT_HANDLED #$00
.define IRQ_HANDLED #$01

.define VIC_IRQ_RASTER #$01

; FIXME
.define SPR_POINTERS $C000+$3F8

.define VIC_SPR_COUNT #$8
.define VIC_SPR_HEIGHT #$21


; ARG A = sprite index
; SETS ptr1 and current_sprite to sprite at index in list
; SETS A and new_y to sprite's y value
.macro get_next_sprite
    asl
    tax

    ldy __sprite_list,X
    sty current_sprite
    sty ptr1

    inx
    ldy __sprite_list,X
    sty current_sprite+1
    sty ptr1+1

    ldy #$03 ; FIXME offsetof(lo_y)
    lda (ptr1),Y
    sta new_y
.endmacro

.macro himasker dest
    lda dest
    and hi_mask
    ora (ptr1),Y
    sta dest
.endmacro

.segment "DATA"
ptr1_save: .res 2

sprite_index:   .byte $ff
vic_sprite:     .byte $00
current_y:      .byte $00
new_y:          .byte $00
hi_mask:        .byte $00
current_sprite: .res 2
raster_clock:   .byte $06

.segment "CODE"

.proc main_raster_irq
    ; Make sure this is a raster interrupt and we're ready
    lda _irq_setup_done
    jeq unhandled
    lda VIC_IRQ_RASTER
    bit VIC_IRR
    jeq unhandled

    ; set the flag so we know we've handled it
    ora VIC_IRR
    sta VIC_IRR

    ; if we're at the beginning of the sprite loop, initialize the variables
    lda sprite_index
    cmp #$ff
    bne sprite_index_updated
    lda #$00
    sta sprite_index
    sta vic_sprite

    ; If we're PAL, the game clock is already 50hz
    ldx _is_pal
    bne update_game_clock

    ; If we're NTSC, skip every 6th frame
    dec raster_clock
    bne update_game_clock

    ; Reset the raster clock
    ldx #$06
    stx raster_clock
    jmp sprite_index_updated
update_game_clock:
    inc _game_clock
    bne sprite_index_updated
    inc _game_clock+1
sprite_index_updated:

    get_next_sprite

sprite_update_loop:
    ; set the current sprite y value
    lda new_y
    sta current_y

    lda #$01
    ldx vic_sprite
    beq done
shift:
    asl
    dex
    bne shift
done:
    eor #$ff
    sta hi_mask

    ; prep the pointer for struct access
    lda current_sprite
    ldx current_sprite+1
    sta ptr1
    stx ptr1+1
    ldy #$00 ; FIXME offsetof(ena)

    iny
    himasker VIC_SPR_ENA

    iny
    himasker VIC_SPR_HI_X

    iny
    himasker VIC_SPR_EXP_X
    sta VIC_SPR_EXP_Y

    iny
    himasker VIC_SPR_MCOLOR

    ; store color and pointer into arrays
    ldx vic_sprite

    lda (ptr1),y
    sta VIC_SPR0_COLOR,X

    iny
    lda (ptr1),y
    sta SPR_POINTERS,X

    ; inc vic_sprite for next loop
    inx
    txa
    cmp VIC_SPR_COUNT
    bne write_vic_sprite
    lda #$00
write_vic_sprite:
    sta vic_sprite

    ; Double the previous value of vic_sprite and use it to set the x and y
    dex

    txa
    asl
    tax
    iny
    lda (ptr1),y
    sta VIC_SPR0_X,X

    iny
    lda (ptr1),Y
    sta VIC_SPR0_Y,x
    tax
    inx
    stx VIC_HLINE

    ; inc sprite_index
    ldx sprite_index
    inx
    txa

    cmp _sprite_count
    bne moarcs

    ; if the index equals the count, reset
    lda #$ff
    sta sprite_index
    sta VIC_HLINE
    jmp end_sprite_update_loop
moarcs:
    sta sprite_index

    get_next_sprite

    ; if new_y >= current_y + buffer
    lda current_y
    clc
    adc VIC_SPR_HEIGHT-$2
    cmp new_y
    jcs sprite_update_loop
end_sprite_update_loop:

handled:
    lda IRQ_HANDLED
    rts

unhandled:
    lda IRQ_NOT_HANDLED
    rts
.endproc

.proc raster_irq
    lda ptr1
    ldx ptr1+1
    sta ptr1_save
    stx ptr1_save+1

    clc
    jsr main_raster_irq
    ; mark interrupt as handled / unhandled
    lsr

    lda ptr1_save
    ldx ptr1_save+1
    sta ptr1
    stx ptr1+1
    rts
.endproc