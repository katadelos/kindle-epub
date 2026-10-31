.syntax unified
.thumb
.text

/* Stock calls retain their original LR and stack. In particular, libMobi can
 * throw through the factory; inserting a C++ frame breaks its unwinding. The
 * displaced instructions and PC-relative literals live in runtime trampolines.
 */
.balign 4
.global kindle_epub_kpp_factory_entry
.hidden kindle_epub_kpp_factory_entry
.type kindle_epub_kpp_factory_entry, %function
.thumb_func
kindle_epub_kpp_factory_entry:
    push.w {r0, r1, r2, r3, r4, lr}
    mov r0, r1
    bl kindle_epub_kpp_is_epub_path
    cmp r0, #0
    pop.w {r0, r1, r2, r3, r4, lr}
    bne .Lfactory_epub
    ldr.w ip, .Lfactory_original
.Lfactory_pc:
    add ip, pc
    ldr.w ip, [ip]
    bx ip
.Lfactory_epub:
    b.w kindle_epub_kpp_factory_hook
.balign 4
.Lfactory_original:
    .word kindle_epub_kpp_factory_original - (.Lfactory_pc + 4)
.size kindle_epub_kpp_factory_entry, .-kindle_epub_kpp_factory_entry

.balign 4
.global kindle_epub_kpp_extension_entry
.hidden kindle_epub_kpp_extension_entry
.type kindle_epub_kpp_extension_entry, %function
.thumb_func
kindle_epub_kpp_extension_entry:
    cmp r1, #0
    beq .Lextension_stock
    ldr r2, [r1]
    cmp r2, #0
    beq .Lextension_stock
#if KINDLE_EPUB_KPP_CXX11_ABI
    ldr r3, [r1, #4]
#else
    ldr r3, [r2, #-12]
#endif
    cmp r3, #4
    bne .Lextension_stock
    ldrb r3, [r2]
    orr.w r3, r3, #0x20
    cmp r3, #'e'
    bne .Lextension_stock
    ldrb r3, [r2, #1]
    orr.w r3, r3, #0x20
    cmp r3, #'p'
    bne .Lextension_stock
    ldrb r3, [r2, #2]
    orr.w r3, r3, #0x20
    cmp r3, #'u'
    bne .Lextension_stock
    ldrb r3, [r2, #3]
    orr.w r3, r3, #0x20
    cmp r3, #'b'
    bne .Lextension_stock
    movs r0, #6
    bx lr
.Lextension_stock:
    ldr.w ip, .Lextension_original
.Lextension_pc:
    add ip, pc
    ldr.w ip, [ip]
    bx ip
.balign 4
.Lextension_original:
    .word kindle_epub_kpp_extension_original - (.Lextension_pc + 4)
.size kindle_epub_kpp_extension_entry, .-kindle_epub_kpp_extension_entry

.section .note.GNU-stack,"",%progbits
