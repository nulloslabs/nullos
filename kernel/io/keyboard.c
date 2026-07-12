#include <freestanding/stdint.h>
#include <freestanding/stddef.h>
#include <io/keyboard.h>
#include <main/halt.h>

uint8_t key_buffer[128] = {0};
volatile uint32_t key_head = 0;
volatile uint32_t key_tail = 0;

static bool shift_pressed = false;
static bool caps_lock = false;
static bool ctrl_pressed = false;
static bool alt_pressed = false;

bool kbd_alt_pressed(void) { return alt_pressed; }

uint8_t get_scancode(void) {
    if (key_head == key_tail) return 0;
    uint8_t sc = key_buffer[key_tail];
    key_tail = (key_tail + 1) & 127;
    return sc;
}

char scancode_to_ascii(uint8_t sc) {
    if (sc & 0x80) { // key release
        uint8_t released = sc & 0x7F;
        
        // Handle shift release
        if (released == 0x2A || released == 0x36) {
            shift_pressed = false;
        }
        // Handle Ctrl release
        if (released == 0x1D) {
            ctrl_pressed = false;
        }
        // Handle Alt release
        if (released == 0x38) {
            alt_pressed = false;
        }
        return 0;
    }

    // Key press
    switch (sc) {
        case 0x1C: return '\n'; // Enter
        case 0x0E: return '\b'; // Backspace
        case 0x39: return ' ';  // Space
        case 0x3A:
            // Caps Lock press - toggle caps
            caps_lock = !caps_lock;
            return 0;
        case 0x2A: case 0x36:
            shift_pressed = true;
            return 0;
        case 0x1D:
            ctrl_pressed = true;
            return 0;
        case 0x38:
            alt_pressed = true;
            return 0;
    }

    static const char lower[128] = {
        0, '\033', '1','2','3','4','5','6','7','8','9','0','-','=','\b', 0,
        'q','w','e','r','t','y','u','i','o','p','[',']','\n',0,'a','s',
        'd','f','g','h','j','k','l',';','\'','`',0,'\\','z','x','c','v',
        'b','n','m',',','.','/',0,'*',0,' ',0,0,0,0,0,0,
    };

    static const char upper[128] = {
        0, '\033', '!','@','#','$','%','^','&','*','(',')','_','+','\b', 0,
        'Q','W','E','R','T','Y','U','I','O','P','{','}','\n',0,'A','S',
        'D','F','G','H','J','K','L',':','"','~',0,'|','Z','X','C','V',
        'B','N','M','<','>','?',0,'*',0,' ',0,0,0,0,0,0,
    };

    if (sc >= 128) return 0;
    
    // Caps Lock only affects letters (a-z), not numbers/symbols
    // Shift affects all keys
    int is_letter = (sc >= 0x10 && sc <= 0x19) ||  // q-p
                    (sc >= 0x1E && sc <= 0x26) ||  // a-l
                    (sc >= 0x2C && sc <= 0x32);    // z-m
    
    char c;
    if (shift_pressed) {
        c = upper[sc];  // Shift always uses upper table
    } else if (caps_lock && is_letter) {
        c = upper[sc];  // Caps Lock only affects letters
    } else {
        c = lower[sc];  // Normal lowercase/numbers
    }
    
    // Ctrl+letter -> control character (Ctrl+A=0x01, ..., Ctrl+Z=0x1A)
    if (ctrl_pressed && c >= 'a' && c <= 'z') {
        c = c - 'a' + 1;  // a->0x01, b->0x02, c->0x03, ..., z->0x1A
    } else if (ctrl_pressed && c >= 'A' && c <= 'Z') {
        c = c - 'A' + 1;
    }
    
    return c;
}

char getc(void) {
    while (1) {
        while (*(volatile uint32_t*)(&key_head) != *(volatile uint32_t*)(&key_tail)) {
            uint8_t sc = get_scancode();
            char c = scancode_to_ascii(sc);
            if (c != 0) return c;
        }
        cli();
        if (*(volatile uint32_t*)(&key_head) == *(volatile uint32_t*)(&key_tail)) {
            wfi();
        }
        sti();
    }
}
