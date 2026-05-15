#include <kernel/irq.h>
#include <kernel/system.h>
#include <interrupt.h>
#include <x86_smp.h>
#include "arch.h"
#include "x86_machine_smp.h"

#define PIC1_CMD   0x20
#define PIC1_DATA  0x21
#define PIC2_CMD   0xA0
#define PIC2_DATA  0xA1
#define PIC_EOI    0x20
#define ICW1_INIT  0x10
#define ICW1_ICW4  0x01
#define ICW4_8086  0x01
#define PIC_MASTER_OFFSET 0x20
#define PIC_SLAVE_OFFSET  0x28

typedef struct {
	uint16_t offset_low;
	uint16_t selector;
	uint8_t ist;
	uint8_t type_attr;
	uint16_t offset_mid;
	uint32_t offset_high;
	uint32_t zero;
} __attribute__((packed)) idt_entry_t;

typedef struct {
	uint16_t limit;
	uint64_t base;
} __attribute__((packed)) idt_ptr_t;

#define DECLARE_ISR(n) extern void isr_##n(void);
DECLARE_ISR(0) DECLARE_ISR(1) DECLARE_ISR(2) DECLARE_ISR(3)
DECLARE_ISR(4) DECLARE_ISR(5) DECLARE_ISR(6) DECLARE_ISR(7)
DECLARE_ISR(8) DECLARE_ISR(9) DECLARE_ISR(10) DECLARE_ISR(11)
DECLARE_ISR(12) DECLARE_ISR(13) DECLARE_ISR(14) DECLARE_ISR(15)
DECLARE_ISR(16) DECLARE_ISR(17) DECLARE_ISR(18) DECLARE_ISR(19)
DECLARE_ISR(20) DECLARE_ISR(21) DECLARE_ISR(22) DECLARE_ISR(23)
DECLARE_ISR(24) DECLARE_ISR(25) DECLARE_ISR(26) DECLARE_ISR(27)
DECLARE_ISR(28) DECLARE_ISR(29) DECLARE_ISR(30) DECLARE_ISR(31)
DECLARE_ISR(32) DECLARE_ISR(33) DECLARE_ISR(34) DECLARE_ISR(35)
DECLARE_ISR(36) DECLARE_ISR(37) DECLARE_ISR(38) DECLARE_ISR(39)
DECLARE_ISR(40) DECLARE_ISR(41) DECLARE_ISR(42) DECLARE_ISR(43)
DECLARE_ISR(44) DECLARE_ISR(45) DECLARE_ISR(46) DECLARE_ISR(47)
DECLARE_ISR(48) DECLARE_ISR(49) DECLARE_ISR(50) DECLARE_ISR(51)
DECLARE_ISR(52) DECLARE_ISR(53) DECLARE_ISR(54) DECLARE_ISR(55)
DECLARE_ISR(56) DECLARE_ISR(57) DECLARE_ISR(58) DECLARE_ISR(59)
DECLARE_ISR(60) DECLARE_ISR(61) DECLARE_ISR(62) DECLARE_ISR(63)
DECLARE_ISR(64) DECLARE_ISR(65) DECLARE_ISR(66) DECLARE_ISR(67)
DECLARE_ISR(68) DECLARE_ISR(69) DECLARE_ISR(70) DECLARE_ISR(71)
DECLARE_ISR(72) DECLARE_ISR(73) DECLARE_ISR(74) DECLARE_ISR(75)
DECLARE_ISR(76) DECLARE_ISR(77) DECLARE_ISR(78) DECLARE_ISR(79)
DECLARE_ISR(80) DECLARE_ISR(81) DECLARE_ISR(82) DECLARE_ISR(83)
DECLARE_ISR(84) DECLARE_ISR(85) DECLARE_ISR(86) DECLARE_ISR(87)
DECLARE_ISR(88) DECLARE_ISR(89) DECLARE_ISR(90) DECLARE_ISR(91)
DECLARE_ISR(92) DECLARE_ISR(93) DECLARE_ISR(94) DECLARE_ISR(95)
DECLARE_ISR(96) DECLARE_ISR(97) DECLARE_ISR(98) DECLARE_ISR(99)
DECLARE_ISR(100) DECLARE_ISR(101) DECLARE_ISR(102) DECLARE_ISR(103)
DECLARE_ISR(104) DECLARE_ISR(105) DECLARE_ISR(106) DECLARE_ISR(107)
DECLARE_ISR(108) DECLARE_ISR(109) DECLARE_ISR(110) DECLARE_ISR(111)
DECLARE_ISR(112) DECLARE_ISR(113) DECLARE_ISR(114) DECLARE_ISR(115)
DECLARE_ISR(116) DECLARE_ISR(117) DECLARE_ISR(118) DECLARE_ISR(119)
DECLARE_ISR(120) DECLARE_ISR(121) DECLARE_ISR(122) DECLARE_ISR(123)
DECLARE_ISR(124) DECLARE_ISR(125) DECLARE_ISR(126) DECLARE_ISR(127)
DECLARE_ISR(128) DECLARE_ISR(129) DECLARE_ISR(130) DECLARE_ISR(131)
DECLARE_ISR(132) DECLARE_ISR(133) DECLARE_ISR(134) DECLARE_ISR(135)
DECLARE_ISR(136) DECLARE_ISR(137) DECLARE_ISR(138) DECLARE_ISR(139)
DECLARE_ISR(140) DECLARE_ISR(141) DECLARE_ISR(142) DECLARE_ISR(143)
DECLARE_ISR(144) DECLARE_ISR(145) DECLARE_ISR(146) DECLARE_ISR(147)
DECLARE_ISR(148) DECLARE_ISR(149) DECLARE_ISR(150) DECLARE_ISR(151)
DECLARE_ISR(152) DECLARE_ISR(153) DECLARE_ISR(154) DECLARE_ISR(155)
DECLARE_ISR(156) DECLARE_ISR(157) DECLARE_ISR(158) DECLARE_ISR(159)
DECLARE_ISR(160) DECLARE_ISR(161) DECLARE_ISR(162) DECLARE_ISR(163)
DECLARE_ISR(164) DECLARE_ISR(165) DECLARE_ISR(166) DECLARE_ISR(167)
DECLARE_ISR(168) DECLARE_ISR(169) DECLARE_ISR(170) DECLARE_ISR(171)
DECLARE_ISR(172) DECLARE_ISR(173) DECLARE_ISR(174) DECLARE_ISR(175)
DECLARE_ISR(176) DECLARE_ISR(177) DECLARE_ISR(178) DECLARE_ISR(179)
DECLARE_ISR(180) DECLARE_ISR(181) DECLARE_ISR(182) DECLARE_ISR(183)
DECLARE_ISR(184) DECLARE_ISR(185) DECLARE_ISR(186) DECLARE_ISR(187)
DECLARE_ISR(188) DECLARE_ISR(189) DECLARE_ISR(190) DECLARE_ISR(191)
DECLARE_ISR(192) DECLARE_ISR(193) DECLARE_ISR(194) DECLARE_ISR(195)
DECLARE_ISR(196) DECLARE_ISR(197) DECLARE_ISR(198) DECLARE_ISR(199)
DECLARE_ISR(200) DECLARE_ISR(201) DECLARE_ISR(202) DECLARE_ISR(203)
DECLARE_ISR(204) DECLARE_ISR(205) DECLARE_ISR(206) DECLARE_ISR(207)
DECLARE_ISR(208) DECLARE_ISR(209) DECLARE_ISR(210) DECLARE_ISR(211)
DECLARE_ISR(212) DECLARE_ISR(213) DECLARE_ISR(214) DECLARE_ISR(215)
DECLARE_ISR(216) DECLARE_ISR(217) DECLARE_ISR(218) DECLARE_ISR(219)
DECLARE_ISR(220) DECLARE_ISR(221) DECLARE_ISR(222) DECLARE_ISR(223)
DECLARE_ISR(224) DECLARE_ISR(225) DECLARE_ISR(226) DECLARE_ISR(227)
DECLARE_ISR(228) DECLARE_ISR(229) DECLARE_ISR(230) DECLARE_ISR(231)
DECLARE_ISR(232) DECLARE_ISR(233) DECLARE_ISR(234) DECLARE_ISR(235)
DECLARE_ISR(236) DECLARE_ISR(237) DECLARE_ISR(238) DECLARE_ISR(239)
DECLARE_ISR(240) DECLARE_ISR(241) DECLARE_ISR(242) DECLARE_ISR(243)
DECLARE_ISR(244) DECLARE_ISR(245) DECLARE_ISR(246) DECLARE_ISR(247)
DECLARE_ISR(248) DECLARE_ISR(249) DECLARE_ISR(250) DECLARE_ISR(251)
DECLARE_ISR(252) DECLARE_ISR(253) DECLARE_ISR(254) DECLARE_ISR(255)
#undef DECLARE_ISR

uint32_t interrupt_table_start = 0;
uint32_t interrupt_table_end = 0;
static idt_ptr_t _idt_ptr;
static idt_entry_t _idt[256];
static uint8_t _pic_mask_master = 0xFB;
static uint8_t _pic_mask_slave = 0xFF;

static void idt_set_gate(uint8_t vector, void (*handler)(void), uint8_t dpl) {
	uint64_t addr = (uint64_t)(uintptr_t)handler;
	_idt[vector].offset_low = addr & 0xFFFF;
	_idt[vector].selector = 0x08;
	_idt[vector].ist = 0;
	_idt[vector].type_attr = 0x8E | ((dpl & 0x3) << 5);
	_idt[vector].offset_mid = (addr >> 16) & 0xFFFF;
	_idt[vector].offset_high = (addr >> 32) & 0xFFFFFFFF;
	_idt[vector].zero = 0;
}

static void idt_init(void) {
	void (*handlers[256])(void) = {
		isr_0, isr_1, isr_2, isr_3, isr_4, isr_5, isr_6, isr_7,
		isr_8, isr_9, isr_10, isr_11, isr_12, isr_13, isr_14, isr_15,
		isr_16, isr_17, isr_18, isr_19, isr_20, isr_21, isr_22, isr_23,
		isr_24, isr_25, isr_26, isr_27, isr_28, isr_29, isr_30, isr_31,
		isr_32, isr_33, isr_34, isr_35, isr_36, isr_37, isr_38, isr_39,
		isr_40, isr_41, isr_42, isr_43, isr_44, isr_45, isr_46, isr_47,
		isr_48, isr_49, isr_50, isr_51, isr_52, isr_53, isr_54, isr_55,
		isr_56, isr_57, isr_58, isr_59, isr_60, isr_61, isr_62, isr_63,
		isr_64, isr_65, isr_66, isr_67, isr_68, isr_69, isr_70, isr_71,
		isr_72, isr_73, isr_74, isr_75, isr_76, isr_77, isr_78, isr_79,
		isr_80, isr_81, isr_82, isr_83, isr_84, isr_85, isr_86, isr_87,
		isr_88, isr_89, isr_90, isr_91, isr_92, isr_93, isr_94, isr_95,
		isr_96, isr_97, isr_98, isr_99, isr_100, isr_101, isr_102, isr_103,
		isr_104, isr_105, isr_106, isr_107, isr_108, isr_109, isr_110, isr_111,
		isr_112, isr_113, isr_114, isr_115, isr_116, isr_117, isr_118, isr_119,
		isr_120, isr_121, isr_122, isr_123, isr_124, isr_125, isr_126, isr_127,
		isr_128, isr_129, isr_130, isr_131, isr_132, isr_133, isr_134, isr_135,
		isr_136, isr_137, isr_138, isr_139, isr_140, isr_141, isr_142, isr_143,
		isr_144, isr_145, isr_146, isr_147, isr_148, isr_149, isr_150, isr_151,
		isr_152, isr_153, isr_154, isr_155, isr_156, isr_157, isr_158, isr_159,
		isr_160, isr_161, isr_162, isr_163, isr_164, isr_165, isr_166, isr_167,
		isr_168, isr_169, isr_170, isr_171, isr_172, isr_173, isr_174, isr_175,
		isr_176, isr_177, isr_178, isr_179, isr_180, isr_181, isr_182, isr_183,
		isr_184, isr_185, isr_186, isr_187, isr_188, isr_189, isr_190, isr_191,
		isr_192, isr_193, isr_194, isr_195, isr_196, isr_197, isr_198, isr_199,
		isr_200, isr_201, isr_202, isr_203, isr_204, isr_205, isr_206, isr_207,
		isr_208, isr_209, isr_210, isr_211, isr_212, isr_213, isr_214, isr_215,
		isr_216, isr_217, isr_218, isr_219, isr_220, isr_221, isr_222, isr_223,
		isr_224, isr_225, isr_226, isr_227, isr_228, isr_229, isr_230, isr_231,
		isr_232, isr_233, isr_234, isr_235, isr_236, isr_237, isr_238, isr_239,
		isr_240, isr_241, isr_242, isr_243, isr_244, isr_245, isr_246, isr_247,
		isr_248, isr_249, isr_250, isr_251, isr_252, isr_253, isr_254, isr_255
	};
	for (uint32_t i = 0; i < 256; ++i) {
		idt_set_gate(i, handlers[i], 0);
	}
	idt_set_gate(0x80, isr_128, 3);
	_idt_ptr.limit = sizeof(_idt) - 1;
	_idt_ptr.base = (uint64_t)(uintptr_t)_idt;
}

static void pic_update_mask(void) {
	outb(PIC1_DATA, _pic_mask_master);
	outb(PIC2_DATA, _pic_mask_slave);
}

static void pic_remap(void) {
	uint8_t master = inb(PIC1_DATA);
	uint8_t slave = inb(PIC2_DATA);

	outb(PIC1_CMD, ICW1_INIT | ICW1_ICW4);
	io_wait();
	outb(PIC2_CMD, ICW1_INIT | ICW1_ICW4);
	io_wait();
	outb(PIC1_DATA, PIC_MASTER_OFFSET);
	io_wait();
	outb(PIC2_DATA, PIC_SLAVE_OFFSET);
	io_wait();
	outb(PIC1_DATA, 0x04);
	io_wait();
	outb(PIC2_DATA, 0x02);
	io_wait();
	outb(PIC1_DATA, ICW4_8086);
	io_wait();
	outb(PIC2_DATA, ICW4_8086);
	io_wait();

	outb(PIC1_DATA, master);
	outb(PIC2_DATA, slave);
	_pic_mask_master = 0xFB;
	_pic_mask_slave = 0xFF;
	pic_update_mask();
}

static int irq_to_line(uint32_t irq) {
	if (irq == IRQ_TIMER0) {
		return 0;
	}
	if (irq < 16) {
		return (int)irq;
	}
	return -1;
}

void irq_init_arch(void) {
	pic_remap();
	x86_irq_percpu_init();
}

void x86_irq_percpu_init(void) {
	idt_init();
	set_vector_table((ewokos_addr_t)&_idt_ptr);
	x86_lapic_init();
}

void irq_enable_arch(uint32_t irq) {
	int line = irq_to_line(irq);
	if (line < 0) {
		return;
	}
	if (line < 8) {
		_pic_mask_master &= ~(1u << line);
	}
	else {
		_pic_mask_slave &= ~(1u << (line - 8));
		_pic_mask_master &= ~(1u << 2);
	}
	pic_update_mask();
}

void irq_enable_core_arch(uint32_t core, uint32_t irq) {
	(void)core;
	irq_enable_arch(irq);
}

void irq_clear_core_arch(uint32_t core, uint32_t irq) {
	(void)core;
	(void)irq;
}

void irq_clear_arch(uint32_t irq) {
	(void)irq;
}

void irq_disable_arch(uint32_t irq) {
	int line = irq_to_line(irq);
	if (line < 0) {
		return;
	}
	if (line < 8) {
		_pic_mask_master |= (1u << line);
	}
	else {
		_pic_mask_slave |= (1u << (line - 8));
	}
	pic_update_mask();
}

uint32_t irq_get_arch(void) {
	/*
	 * x86 raw IRQ vectors are taken directly from the trap frame by the
	 * common kernel irq handler, so there is no per-platform global raw IRQ.
	 */
	return 0;
}

uint32_t irq_get_unified_arch(uint32_t irq_raw) {
	if (irq_raw == X86_VECTOR_IPI) {
		return IRQ_IPI;
	}
	if (irq_raw >= PIC_MASTER_OFFSET && irq_raw < PIC_MASTER_OFFSET + 16) {
		uint32_t irq = irq_raw - PIC_MASTER_OFFSET;
		if (irq == 0) {
			return IRQ_TIMER0;
		}
		return irq;
	}
	return irq_raw;
}

void irq_eoi_arch(uint32_t irq_raw) {
	if (irq_raw == X86_VECTOR_IPI) {
		x86_lapic_write(LAPIC_EOI_REG, 0);
		return;
	}
	if (irq_raw >= PIC_SLAVE_OFFSET && irq_raw < PIC_SLAVE_OFFSET + 8) {
		outb(PIC2_CMD, PIC_EOI);
	}
	if (irq_raw >= PIC_MASTER_OFFSET && irq_raw < PIC_MASTER_OFFSET + 16) {
		outb(PIC1_CMD, PIC_EOI);
	}
}
