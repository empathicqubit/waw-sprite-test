#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <6502.h>
#include <conio.h>
#include <c64.h>
#include "c64.h"
#include <errno.h>

#define IRQ_STACK_SIZE 128

extern void updatepalntsc(void);

/* Check if system is PAL
 */
void pal_system(void) {
    updatepalntsc();
}

unsigned char* irq_stack;

#define SCREEN_LAST_RASTER SCREEN_SPRITE_BORDER_Y_START + SCREEN_BITMAP_HEIGHT
#define SPRITE_BUFFER_LINE_COUNT 8
#define SPRITE_BUFFER_LINE_HEIGHT (SCREEN_BITMAP_HEIGHT / SPRITE_BUFFER_LINE_COUNT)
#define SCREEN_FIRST_RASTER SCREEN_SPRITE_BORDER_Y_START - SPRITE_BUFFER_LINE_HEIGHT

/** Disable the IO page 
 */ 
void __fastcall__ hide_io(void) { 
    *(unsigned char *)CIA1_CRA &= ~CIA1_CR_START_STOP; 
 
    *(unsigned char *)CPU_PORT &= ~CPU_PORT_BANK_IO_VISIBLE_CHARACTER_ROM_INVISIBLE; 
} 
 
/** Enable the IO page 
 */ 
void __fastcall__ show_io(void) { 
    *(unsigned char *)CPU_PORT |= CPU_PORT_BANK_IO_VISIBLE_CHARACTER_ROM_INVISIBLE; 
 
    *(unsigned char *)CIA1_CRA |= CIA1_CR_START_STOP; 
} 

/** Copies character ROM to RAM.
 * @param use_graphics_charset - Use fancy graphics chars with no lowercase
 */
void character_init(bool use_graphics_charset) {
    hide_io();
    memcpy(CHARACTER_START, CHARACTER_ROM + (!use_graphics_charset * VIC_VIDEO_ADR_CHAR_DIVISOR), CHARACTER_ROM_SIZE);
    show_io();
}

/** Reset the screen to VIC bank #3
 * @param clear - Clear the screen before switching to it
 */
void screen_init (bool clear) {
    unsigned char screen_ptr = ((SCREEN_START % VIC_BANK_SIZE) / VIC_VIDEO_ADR_SCREEN_DIVISOR) << 4;

    // Update kernal
    *(unsigned char *)SCREEN_IO_HI_PTR = SCREEN_START >> 8;

    // Switch to bank 3
    *(unsigned char *)CIA2_PRA &= ~3;

    // Switch to screen memory
    VIC.addr &= ~(VIC_VIDEO_ADR_SCREEN_PTR_MASK);
    VIC.addr |= screen_ptr;

    // Switch to character memory
    VIC.addr &= ~(VIC_VIDEO_ADR_CHAR_PTR_MASK);
    VIC.addr |= ((CHARACTER_START % VIC_BANK_SIZE) / VIC_VIDEO_ADR_CHAR_DIVISOR) << 1;

    // Fix HLINE IRQ
    VIC.rasterline = SCREEN_FIRST_RASTER;
    VIC.ctrl1 &= ~VIC_CTRL1_HLINE_MSB;

    // Switch off bitmap mode
    VIC.ctrl1 &= ~VIC_CTRL1_BITMAP_ON;
    VIC.ctrl2 &= ~VIC_CTRL2_MULTICOLOR_ON;

    // Enable raster interrupts
    VIC.imr |= VIC_IRQ_RASTER;

    if(clear) {
        clrscr();
        printf("hallo!");

        VIC.bgcolor0 = COLOR_BLACK;
        VIC.bgcolor1 = COLOR_BLACK;
        VIC.bgcolor2 = COLOR_BLACK;
        VIC.bgcolor3 = COLOR_BLACK;
        VIC.bordercolor = COLOR_BLACK;
        bordercolor(COLOR_BLACK);
    }
}

bool is_pal = false;

unsigned char setup_irq_handler(unsigned char (*handler)(void)) {
    if(!irq_stack) {
        if(!(irq_stack = malloc(IRQ_STACK_SIZE))) {
            return EXIT_FAILURE;
        }
    }

    set_irq(handler, irq_stack, IRQ_STACK_SIZE);

    return EXIT_SUCCESS;
}
    
unsigned char raster_clock = 0;
unsigned int game_clock = 0;
unsigned int last_updated = 0;

void fatal(unsigned char* message) {
    while(true);
}

#define SPD_PADDING 55

struct spd_sprite {
    unsigned char sprite_data[63];
    unsigned char metadata;
};
typedef struct spd_sprite spd_sprite;

struct spd {
    unsigned char padding[SPD_PADDING];
    unsigned char magic[3];
    unsigned char version;
    unsigned char sprite_count;
    unsigned char animation_count;
    unsigned char background_color;
    unsigned char multicolor_0;
    unsigned char multicolor_1;
    spd_sprite sprites[];
};
typedef struct spd spd;

#define SPRITE_MAX 80

/* Load a sprite sheet in SpritePad format
 * @param filename - The filename on disk
 * @return - Whether the sheet successfully loaded into memory.
 */
unsigned char spritesheet_load(unsigned char* filename) {
    static FILE* fp;
    static spd* spd_data;
    static unsigned char* header;

    fp = fopen(filename, "rb");
    if(!fp) {
        if(errno != 0) {
            return errno;
        }
        else {
            return EXIT_FAILURE;
        }
    }

    // Used to write this directly to the memory area, but we can't read it if we do.
    if(!(header = calloc(1, VIC_SPR_SIZE))
        || !fread(header + SPD_PADDING, VIC_SPR_SIZE - SPD_PADDING, 1, fp)) {
        fclose(fp);
        return EXIT_FAILURE;
    }

    spd_data = (spd*)header;

    if(spd_data->sprite_count + 1 > SPRITE_MAX) {
        free(header);
        fclose(fp);
        return EXIT_FAILURE;
    }

    memcpy(SPRITE_START, header, VIC_SPR_SIZE);

    if(!fread(SPRITE_START + VIC_SPR_SIZE, VIC_SPR_SIZE, spd_data->sprite_count + 1, fp)) {
        fclose(fp);
        return EXIT_FAILURE;
    }

    // FIXME tf is background? The background is transparent!

    VIC.spr_mcolor0 = spd_data->multicolor_0;
    VIC.spr_mcolor1 = spd_data->multicolor_1;

    free(header);
    fclose(fp);

    return EXIT_SUCCESS;
}

#define SPD_SPRITE_MULTICOLOR_ENABLE_MASK 0x80
#define SPD_SPRITE_COLOR_VALUE_MASK 0x0F

struct sprite_data {
    unsigned char color;
    unsigned char pointer;
    unsigned char lo_x;
    unsigned char lo_y;

    unsigned char ena;
    unsigned char hi_x;
    unsigned char dbl;
    unsigned char multi;
};
typedef struct sprite_data* sprite_handle;

#define SPRITE_POOL_SIZE 32
struct sprite_data _sprite_pool[SPRITE_POOL_SIZE];
sprite_handle _sprite_list[SPRITE_POOL_SIZE];
unsigned char sprite_count;

void init_sprite_pool(void) {
    memset(&_sprite_pool, 0x00, sizeof(struct sprite_data) * SPRITE_POOL_SIZE);
    memset(&_sprite_list, NULL, sizeof(sprite_handle) * SPRITE_POOL_SIZE);
}

void set_sprite_pointer(sprite_handle handle, unsigned char sprite_pointer) {
    static spd_sprite* sprite;

    sprite = (spd_sprite*)(SCREEN_START + sprite_pointer * VIC_SPR_SIZE);

    handle->pointer = sprite_pointer;
    handle->color = sprite->metadata & SPD_SPRITE_COLOR_VALUE_MASK;
    if(sprite->metadata & SPD_SPRITE_MULTICOLOR_ENABLE_MASK) {
        handle->multi = 1<<((handle - _sprite_pool)%VIC_SPR_COUNT);
    }
    else {
        handle->multi = 0;
    }
}

void set_sprite_graphic(sprite_handle handle, unsigned char sheet_index) {
    static spd* s = (spd*)SPRITE_START;
    set_sprite_pointer(handle, ((unsigned int)(&s->sprites[sheet_index]) % VIC_BANK_SIZE) / VIC_SPR_SIZE);
}

void set_sprite_position(sprite_handle a, unsigned int x, unsigned char y) {
    static sprite_handle *start_handle, *comp_handle, *current_handle;
    static sprite_handle arg, comp;
    static unsigned char index, last_index, hi_mask, comp_y, yarg;
    static bool direction;

    yarg = y;
    arg = a;

    comp_y = arg->lo_y;
    hi_mask = 1<<(index%VIC_SPR_COUNT);
    if(x>>8) {
        arg->hi_x = hi_mask;
    }
    else {
        arg->hi_x = 0;
    }

    arg->lo_x = (unsigned char)x;
    if(yarg == comp_y) {
        return;
    }
    arg->lo_y = yarg;

    index = 0;
    for(
        start_handle = _sprite_list;
        *start_handle != arg;
        start_handle++
    ) {
        index++;
    }

    if(yarg > comp_y) {
        last_index = sprite_count - 1;
        if(last_index == index) {
            return;
        }
        direction = true;
    }
    else {
        if(index == 0) {
            return;
        }
        direction = false;
    }

    current_handle = start_handle;
    do {
        if(direction) {
            comp_handle = current_handle + 1;
        }
        else {
            comp_handle = current_handle - 1;
        }
        comp = *comp_handle;
        if((
            direction
                ? (yarg <= comp->lo_y 
                    || index == last_index)
                : (comp->lo_y <= yarg
                    || index == 0)
        ) 
        ) {
            if(current_handle == start_handle) {
                break;
            }

            comp = arg;
        }

        hi_mask = 1<<(index%VIC_SPR_COUNT);

        __asm__("lda %v", comp);
        __asm__("ldx %v+1", comp);
        __asm__("ldy #%b", offsetof(struct sprite_data, ena));
        __asm__("sta ptr1");
        __asm__("stx ptr1+1");

        __asm__("ldx %v", hi_mask);
        __asm__("loop: lda (ptr1),Y");
        __asm__("beq done");
        __asm__("txa");
        __asm__("sta (ptr1),Y");
        __asm__("done: iny");
        __asm__("tya");
        __asm__("sbc #%b", offsetof(struct sprite_data, multi));
        __asm__("bne loop");
        
        if(comp == arg) {
            *current_handle = comp;
            break;
        }

        *current_handle = comp;
        *comp_handle = arg;

        if(direction) {
            index++;
        }
        else {
            index--;
        }
        
        if(direction) {
            current_handle++;
        }
        else {
            current_handle--;
        }
    } while (true);
}

void discard_sprite(sprite_handle handle) {
    set_sprite_position(handle, 0xff, 0xff);
    sprite_count--;
    _sprite_list[sprite_count] = NULL;
}

sprite_handle new_sprite(bool dbl) {
    static sprite_handle handle;

    handle = &_sprite_pool[sprite_count];
    _sprite_list[sprite_count] = handle;
    handle->dbl = dbl<<(sprite_count%VIC_SPR_COUNT);
    handle->ena = 1<<(sprite_count%VIC_SPR_COUNT);
    handle->lo_x = 0xfe;
    handle->lo_y = 0xfe;
    sprite_count++;

    return handle;
}

#define SPR_POINTERS SCREEN_START + 0x3F8

unsigned char main_raster_irq(void) {
    static unsigned char sprite_index = 0xff;
    static unsigned char vic_sprite = 0;
    static unsigned char current_y = 0;
    static unsigned char new_y = 0;
    static unsigned char hi_mask = 0;
    static sprite_handle cs;

    if(!(VIC.irr & VIC_IRQ_RASTER)) {
        return IRQ_NOT_HANDLED;
    }

    VIC.irr |= VIC_IRQ_RASTER;
    if(sprite_index == 0xff) {
        sprite_index = 0;
        vic_sprite = 0;

        // NTSC frame skip
        if(is_pal) {
            game_clock++;
        }
        else if(++raster_clock < 6) {
            game_clock++;
        }
        else {
            raster_clock = 0;
        }
    }

    cs = _sprite_list[sprite_index];
    new_y = cs->lo_y;

    do {
        current_y = new_y;

        hi_mask = ~(1<<vic_sprite);

#define himasker(dest) { \
    __asm__("lda %w", dest); \
    __asm__("and %v", hi_mask); \
    __asm__("ora (ptr1),Y"); \
    __asm__("sta %w", dest); \
}

        __asm__("lda %v", cs);
        __asm__("ldx %v+1", cs);
        __asm__("sta ptr1");
        __asm__("stx ptr1+1");
        __asm__("ldy #%b", offsetof(struct sprite_data, color));

        __asm__("ldx %v", vic_sprite);

        __asm__("lda (ptr1),Y");
        __asm__("sta %w,X", VIC_SPR0_COLOR);

        __asm__("iny");
        __asm__("lda (ptr1),Y");
        __asm__("sta %w,X", SPR_POINTERS);

        __asm__("txa");
        __asm__("asl");
        __asm__("tax");
        __asm__("iny");
        __asm__("lda (ptr1),Y");
        __asm__("sta %w,X", VIC_SPR0_X);

        __asm__("iny");
        __asm__("lda (ptr1),Y");
        __asm__("sta %w,X", VIC_SPR0_Y);
        __asm__("tax");
        __asm__("inx");
        __asm__("stx %w", VIC_HLINE);

        __asm__("iny");
        himasker(VIC_SPR_ENA);

        __asm__("iny");
        himasker(VIC_SPR_HI_X);

        __asm__("iny");
        himasker(VIC_SPR_EXP_X);
        __asm__("sta %w", VIC_SPR_EXP_Y);

        __asm__("iny");
        himasker(VIC_SPR_MCOLOR);

        vic_sprite++;
        if(vic_sprite >= VIC_SPR_COUNT) {
            vic_sprite = 0;
        }

        // sprite_index++
        __asm__("ldx %v", sprite_index);
        __asm__("inx");
        __asm__("txa");

        // if(sprite_index == sprite_count)
        __asm__("cmp %v", sprite_count);

        // else
        __asm__("bne moarcs");

        // then
        __asm__("lda #$ff");
        __asm__("sta %v", sprite_index);
        __asm__("sta %w", VIC_HLINE);
        return IRQ_HANDLED;

        // store the new sprite index
        __asm__("moarcs:");
        __asm__("sta %v", sprite_index);

        __asm__("asl");
        __asm__("tax");

        // cs = _sprite_list[sprite_index]
        __asm__("ldy %v,X", _sprite_list);
        __asm__("sty %v", cs);
        __asm__("sty ptr1");

        __asm__("inx");
        __asm__("ldy %v,X", _sprite_list);
        __asm__("sty %v+1", cs);
        __asm__("sty ptr1+1");

        // new_y = cs->lo_y
        __asm__("ldy #%b", offsetof(struct sprite_data, lo_y));
        __asm__("lda (ptr1),Y");
        __asm__("sta %v", new_y);

        // if new_y >= current_y + buffer
        __asm__("lda %v", current_y);
        __asm__("clc");
        __asm__("adc #%b", VIC_SPR_HEIGHT - 2);
        __asm__("cmp %v", new_y);
        __asm__("bcc exitloop");
    } while(true);
    __asm__("exitloop: ");

    return IRQ_HANDLED;
}

#define WAW_SPRITE_COUNT 9
#define WAW_SPRITE_OFFSET 0
#define WAW_COLUMNS 3
#define WAW_ROWS 3
#define WAW_MINMOUTH -(VIC_SPR_HEIGHT / 2)
#define WAW_MAXMOUTH VIC_SPR_HEIGHT
#define WAW_MAXFLOAT VIC_SPR_HEIGHT * 2 * WAW_ROWS
#define WAW_MOUTHINDEX 7
#define WAW_MOUTHSPEED 5
#define WAW_MOVESPEED 3

struct waw {
    unsigned int x;
    unsigned char y;
    signed char mouth_offset;
    bool mouth_direction;
    bool float_direction;
    sprite_handle sprites[WAW_SPRITE_COUNT];
};
typedef struct waw waw;

void init_waw(waw* w) {
    static unsigned char i, j, x, y, sprite_x, sprite_y, idx;
    static waw* waw;
    static sprite_handle sprite;
    static sprite_handle* sprites;
    waw = w;

    x = waw->x + SCREEN_SPRITE_BORDER_X_START;
    y = waw->y + SCREEN_SPRITE_BORDER_Y_START;
    waw->y = y;

    idx = 0;
    sprites = waw->sprites;
    for(i = 0; i < WAW_COLUMNS; i++) {
        sprite_y = y + i * VIC_SPR_HEIGHT * 2;
        for(j = 0; j < WAW_ROWS; j++) {
            sprite_x = x + j * VIC_SPR_WIDTH * 2;

            sprite = new_sprite(true);
            set_sprite_graphic(sprite, WAW_SPRITE_OFFSET + idx);
            if(idx == WAW_MOUTHINDEX) {
                set_sprite_position(sprite, sprite_x, sprite_y + waw->mouth_offset);
            }
            else {
                set_sprite_position(sprite, sprite_x, sprite_y);
            }
            sprites[idx] = sprite;
            idx++;
        }
    }
}

void update_waw(waw* w) {
    static unsigned int sprite_x;
    static unsigned char y, sprite_y;
    static unsigned char idx;
    static signed char change_y, mouth_offset;
    static sprite_handle* current_sprite;
    static sprite_handle sprite;
    static waw* waw;
    waw = w;
    mouth_offset = waw->mouth_offset;
    if(waw->mouth_direction) {
        mouth_offset+=WAW_MOUTHSPEED;
        if(mouth_offset > WAW_MAXMOUTH) {
            mouth_offset = WAW_MAXMOUTH;
            waw->mouth_direction = false;
        }
    }
    else {
        mouth_offset-=WAW_MOUTHSPEED;
        if(mouth_offset < WAW_MINMOUTH) {
            mouth_offset = WAW_MINMOUTH;
            waw->mouth_direction = true;
        }
    }

    waw->mouth_offset = mouth_offset;

    y = waw->y;

    if(waw->float_direction) {
        change_y = WAW_MOVESPEED;
        if(change_y + y > WAW_MAXFLOAT) {
            change_y = 0;
            waw->float_direction = false;
        }
    }
    else {
        change_y = -WAW_MOVESPEED;
        if(change_y + y < SCREEN_SPRITE_BORDER_Y_START) {
            change_y = 0;
            waw->float_direction = true;
        }
    }

    waw->y = y + change_y;

    current_sprite = waw->sprites;
    for(idx = 0; idx < WAW_SPRITE_COUNT; idx++) {
        sprite = *current_sprite;
        ((unsigned char*)&sprite_x)[0] = sprite->lo_x; 
        ((unsigned char*)&sprite_x)[1] = sprite->hi_x;
        sprite_y = sprite->lo_y + change_y;
        // Mouth
        if(idx == WAW_MOUTHINDEX) {
            set_sprite_position(sprite, sprite_x, current_sprite[-1]->lo_y + mouth_offset);
        }
        else {
            set_sprite_position(sprite, sprite_x, sprite_y);
        }

        current_sprite++;
    }
}

unsigned char main(void) {
    static unsigned char err;
    //static waw waw2 = {VIC_SPR_WIDTH * WAW_COLUMNS * 2,VIC_SPR_HEIGHT * 2,0,true,true};
    static waw waw = {0,0,0,true,true};

    updatepalntsc();
    is_pal = get_tv();

    if(err = spritesheet_load("sprites.spd")) {
        printf("Spritesheet failed to load: %d", errno);
        return EXIT_FAILURE;
    }

    init_sprite_pool();
    init_waw(&waw);
    //init_waw(&waw2);

    character_init(true);
    setup_irq_handler(&main_raster_irq);
    screen_init(true);

    do {
        if(last_updated == game_clock) {
            continue;
        }

        update_waw(&waw);
        //update_waw(&waw2);

        last_updated++;
    } while(true);

    while(true);

    return 0;
}