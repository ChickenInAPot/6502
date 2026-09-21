.setcpu "65C02"

; Ben Eater 6502 memory map.
VIA_PORTB = $6000
VIA_DDRB  = $6002

.segment "CODE"

reset:
    sei                     ; ignore maskable interrupts during this test
    cld                     ; binary arithmetic
    ldx #$ff
    txs                     ; initialize the stack

    lda #$ff
    sta VIA_DDRB            ; make all eight VIA Port B pins outputs

    lda #$00
count:
    sta VIA_PORTB           ; display the binary count on PB0-PB7
    jsr delay
    clc
    adc #$01                ; wraps from $FF back to $00
    bra count

; Approximately 100 ms at a 1 MHz CPU clock, so the count advances around
; ten times per second. X and Y are preserved for easy future modification.
delay:
    phx
    phy
    ldy #$50
outer:
    ldx #$ff
inner:
    dex
    bne inner
    dey
    bne outer
    ply
    plx
    rts

nmi:
    rti

irq:
    rti

.segment "VECTORS"
    .word nmi               ; $FFFA-$FFFB
    .word reset             ; $FFFC-$FFFD
    .word irq               ; $FFFE-$FFFF
