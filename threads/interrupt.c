#include "threads/interrupt.h"
#include <debug.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include "threads/flags.h"
#include "threads/intr-stubs.h"
#include "threads/io.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "devices/timer.h"
#include "intrinsic.h"
#ifdef USERPROG
#include "userprog/gdt.h"
#endif

/* x86_64 인터럽트 개수 */
#define INTR_CNT 256

/* FUNCTION을 호출하는 게이트를 생성함.

   게이트는 DPL이라는 특권 레벨을 가지며, 이는 프로세서가 해당 DPL이나
   더 낮은 번호의 링에 있을 때 게이트가 의도적으로 호출될 수 있음을 의미함.
   실제로 DPL==3은 유저 모드에서 게이트에 접근할 수 있도록 하고, 
   DPL==0은 유저 모드에서의 호출을 방지함. 
   유저 모드에서 발생하는 오류와 예외는 DPL==0 게이트를 호출함.

   TYPE은 14 (인터럽트 게이트) 또는 15 (트랩 게이트) 중 하나여야 함. 
   인터럽트 게이트에 진입하면 인터럽트가 비활성화되지만, 
   트랩 게이트에 진입하면 비활성화되지 않음. 
   자세한 내용은 [IA32-v3a] 섹션 5.12.1.2 "예외 또는 인터럽트 핸들러 프로시저에 의한 플래그 사용" 참조. */

struct gate {
	unsigned off_15_0 : 16;   // 세그먼트 내 오프셋의 하위 16비트
	unsigned ss : 16;         // 세그먼트 선택자
	unsigned ist : 3;        // # 인수, 인터럽트/트랩 게이트는 0
	unsigned rsv1 : 5;        // 예약됨(0이어야 함)
	unsigned type : 4;        // 타입(STS_{TG,IG32,TG32})
	unsigned s : 1;           // 반드시 0 (시스템)
	unsigned dpl : 2;         // 디스크립터 특권 레벨
	unsigned p : 1;           // 존재 여부
	unsigned off_31_16 : 16;  // 세그먼트 내 오프셋의 상위 비트
	uint32_t off_32_63;
	uint32_t rsv2;
};

/* 인터럽트 디스크립터 테이블 (IDT).
   형식은 CPU에 의해 고정됨. 
   [IA32-v3a] 섹션 5.10 "인터럽트 디스크립터 테이블 (IDT)", 
   5.11 "IDT 디스크립터", 5.12.1.2 "예외 또는 인터럽트 핸들러 프로시저에 의한 플래그 사용" 참조. */
static struct gate idt[INTR_CNT];

static struct desc_ptr idt_desc = {
	.size = sizeof(idt) - 1,
	.address = (uint64_t) idt
};


#define make_gate(g, function, d, t) \
{ \
	ASSERT ((function) != NULL); \
	ASSERT ((d) >= 0 && (d) <= 3); \
	ASSERT ((t) >= 0 && (t) <= 15); \
	*(g) = (struct gate) { \
		.off_15_0 = (uint64_t) (function) & 0xffff, \
		.ss = SEL_KCSEG, \
		.ist = 0, \
		.rsv1 = 0, \
		.type = (t), \
		.s = 0, \
		.dpl = (d), \
		.p = 1, \
		.off_31_16 = ((uint64_t) (function) >> 16) & 0xffff, \
		.off_32_63 = ((uint64_t) (function) >> 32) & 0xffffffff, \
		.rsv2 = 0, \
	}; \
}

/* DPL을 가진 FUNCTION을 호출하는 인터럽트 게이트 생성 */
#define make_intr_gate(g, function, dpl) make_gate((g), (function), (dpl), 14)

/* DPL을 가진 FUNCTION을 호출하는 트랩 게이트 생성 */
#define make_trap_gate(g, function, dpl) make_gate((g), (function), (dpl), 15)



/* 각 인터럽트에 대한 인터럽트 핸들러 함수 */
static intr_handler_func *intr_handlers[INTR_CNT];

/* 디버깅 목적으로 각 인터럽트의 이름 */
static const char *intr_names[INTR_CNT];

/* 외부 인터럽트는 CPU 외부 장치에서 발생하는 인터럽트를 의미하며, 
   예를 들어 타이머가 이에 해당함. 
   외부 인터럽트는 인터럽트를 비활성화된 상태로 실행되므로 중첩되지 않으며 
   선점되지도 않음. 외부 인터럽트 핸들러는 대기할 수 없지만, 
   인터럽트가 반환되기 직전에 새로운 프로세스를 예약하도록 intr_yield_on_return()을 
   호출할 수 있음. */
static bool in_external_intr;   /* 외부 인터럽트를 처리 중인지 여부 */
static bool yield_on_return;    /* 인터럽트 반환 시 양보할지 여부 */

/* 프로그래머블 인터럽트 컨트롤러 보조 함수들 */
static void pic_init (void);
static void pic_end_of_interrupt (int irq);

/* 인터럽트 핸들러들 */
void intr_handler (struct intr_frame *args);

/* 현재 인터럽트 상태를 반환 */
enum intr_level
intr_get_level (void) {
	uint64_t flags;

	/* 플래그 레지스터를 프로세서 스택에 푸시한 다음, 
	   스택에서 값을 `flags`에 팝핑함. 
	   [IA32-v2b] "PUSHF"와 "POP" 및 [IA32-v3a] 5.8.1 "마스크 가능한 하드웨어 
	   인터럽트 마스킹" 참조. */
	asm volatile ("pushfq; popq %0" : "=g" (flags));

	return flags & FLAG_IF ? INTR_ON : INTR_OFF;
}

/* LEVEL에 따라 인터럽트를 활성화 또는 비활성화하고, 이전 인터럽트 상태를 반환 */
enum intr_level
intr_set_level (enum intr_level level) {
	return level == INTR_ON ? intr_enable () : intr_disable ();
}

/* 인터럽트를 활성화하고 이전 인터럽트 상태를 반환 */
enum intr_level
intr_enable (void) {
	enum intr_level old_level = intr_get_level ();
	ASSERT (!intr_context ());

	/* 인터럽트 플래그를 설정하여 인터럽트를 활성화

	   [IA32-v2b] "STI"와 [IA32-v3a] 5.8.1 "마스크 가능한 하드웨어 
	   인터럽트 마스킹" 참조. */
	asm volatile ("sti");

	return old_level;
}

/* 인터럽트를 비활성화하고 이전 인터럽트 상태를 반환 */
enum intr_level
intr_disable (void) {
	enum intr_level old_level = intr_get_level ();

	/* 인터럽트 플래그를 지워서 인터럽트를 비활성화.
	   [IA32-v2b] "CLI" 및 [IA32-v3a] 5.8.1 "마스크 가능한 
	   하드웨어 인터럽트 마스킹" 참조. */
	asm volatile ("cli" : : : "memory");

	return old_level;
}

/* 인터럽트 시스템 초기화 */
void
intr_init (void) {
	int i;

	/* 인터럽트 컨트롤러 초기화 */
	pic_init ();

	/* IDT 초기화 */
	for (i = 0; i < INTR_CNT; i++) {
		make_intr_gate(&idt[i], intr_stubs[i], 0);
		intr_names[i] = "unknown";
	}

#ifdef USERPROG
	/* TSS 로드 */
	ltr (SEL_TSS);
#endif

	/* IDT 레지스터 로드 */
	lidt(&idt_desc);

	/* intr_names 초기화 */
	intr_names[0] = "#DE Divide Error";
	intr_names[1] = "#DB Debug Exception";
	intr_names[2] = "NMI Interrupt";
	intr_names[3] = "#BP Breakpoint Exception";
	intr_names[4] = "#OF Overflow Exception";
	intr_names[5] = "#BR BOUND Range Exceeded Exception";
	intr_names[6] = "#UD Invalid Opcode Exception";
	intr_names[7] = "#NM Device Not Available Exception";
	intr_names[8] = "#DF Double Fault Exception";
	intr_names[9] = "Coprocessor Segment Overrun";
	intr_names[10] = "#TS Invalid TSS Exception";
	intr_names[11] = "#NP Segment Not Present";
	intr_names[12] = "#SS Stack Fault Exception";
	intr_names[13] = "#GP General Protection Exception";
	intr_names[14] = "#PF Page-Fault Exception";
	intr_names[16] = "#MF x87 FPU Floating-Point Error";
	intr_names[17] = "#AC Alignment Check Exception";
	intr_names[18] = "#MC Machine-Check Exception";
	intr_names[19] = "#XF SIMD Floating-Point Exception";
}

/* 인터럽트 VEC_NO에 HANDLER를 등록하고, 디스크립터 특권 레벨 DPL을 설정.
   디버깅을 위한 이름을 NAME으로 지정하고, 인터럽트 핸들러는 LEVEL 상태로 호출됨. */
static void
register_handler (uint8_t vec_no, int dpl, enum intr_level level,
		intr_handler_func *handler, const char *name) {
	ASSERT (intr_handlers[vec_no] == NULL);
	if (level == INTR_ON) {
		make_trap_gate(&idt[vec_no], intr_stubs[vec_no], dpl);
	}
	else {
		make_intr_gate(&idt[vec_no], intr_stubs[vec_no], dpl);
	}
	intr_handlers[vec_no] = handler;
	intr_names[vec_no] = name;
}

/* 외부 인터럽트 VEC_NO를 등록하고, HANDLER를 호출함.
   디버깅을 위해 NAME으로 명명되며, 핸들러는 인터럽트가 비활성화된 상태로 실행됨. */
void
intr_register_ext (uint8_t vec_no, intr_handler_func *handler,
		const char *name) {
	ASSERT (vec_no >= 0x20 && vec_no <= 0x2f);
	register_handler (vec_no, 0, INTR_OFF, handler, name);
}

/* 내부 인터럽트 VEC_NO를 등록하고, HANDLER를 호출하며, 
   디버깅을 위해 NAME으로 명명됨. 인터럽트 핸들러는 LEVEL 상태로 호출됨.

   핸들러는 디스크립터 특권 레벨 DPL을 가지며, 프로세서가 해당 DPL이나
   더 낮은 레벨에 있을 때 호출될 수 있음. DPL==3이면 유저 모드에서
   인터럽트를 호출할 수 있고, DPL==0이면 이러한 호출을 방지함.
   유저 모드에서 발생하는 오류와 예외는 여전히 DPL==0의 인터럽트를 호출함.
   자세한 내용은 [IA32-v3a] 섹션 4.5 "특권 레벨" 및 4.8.1.1
   "비일치 코드 세그먼트 접근" 참조. */
void
intr_register_int (uint8_t vec_no, int dpl, enum intr_level level,
		intr_handler_func *handler, const char *name)
{
	ASSERT (vec_no < 0x20 || vec_no > 0x2f);
	register_handler (vec_no, dpl, level, handler, name);
}

/* 외부 인터럽트 처리 중일 때 true를 반환하고, 그 외에는 false를 반환. */
bool
intr_context (void) {
	return in_external_intr;
}

/* 외부 인터럽트 처리 중일 때, 인터럽트가 반환되기 직전에 새로운 프로세스에 
   양보하도록 인터럽트 핸들러를 지시함. 다른 시점에서 호출될 수 없음. */
void
intr_yield_on_return (void) {
	ASSERT (intr_context ());
	// yield_on_return는 플래그 중 하나로 스케줄링과 CPU 양보를 요청
	yield_on_return = true;
}

/* 8259A 프로그래머블 인터럽트 컨트롤러 (PIC). */

/* 모든 PC에는 두 개의 8259A 프로그래머블 인터럽트 컨트롤러(PIC) 칩이 있음.
   하나는 포트 0x20 및 0x21에서 접근 가능한 "마스터"이며, 다른 하나는 
   마스터의 IRQ 2 라인에 연결된 "슬레이브"로, 포트 0xa0 및 0xa1에서 접근 가능함.
   포트 0x20에 접근하면 A0 라인이 0으로 설정되고, 0x21에 접근하면 A1 라인이 
   1로 설정됨. 슬레이브 PIC의 경우도 마찬가지임.

   기본적으로 PIC가 전달하는 인터럽트 0...15는 인터럽트 벡터 0...15로 
   전달됨. 불행히도 이 벡터들은 CPU 트랩과 예외에도 사용됨.
   따라서 인터럽트 0...15가 인터럽트 벡터 32...47 (0x20...0x2f)으로 
   전달되도록 PIC를 다시 설정함. */

/* PIC 초기화. [8259A] 참조. */
static void
pic_init (void) {
	/* 두 PIC의 모든 인터럽트를 마스킹 */
	outb (0x21, 0xff);
	outb (0xa1, 0xff);

	/* 마스터 초기화 */
	outb (0x20, 0x11); /* ICW1: 싱글 모드, 에지 트리거, ICW4 예상 */
	outb (0x21, 0x20); /* ICW2: 라인 IR0...7 -> irq 0x20...0x27 */
	outb (0x21, 0x04); /* ICW3: 슬레이브 PIC는 라인 IR2에 연결됨 */
	outb (0x21, 0x01); /* ICW4: 8086 모드, 일반 EOI, 비버퍼링 */

	/* 슬레이브 초기화 */
	outb (0xa0, 0x11); /* ICW1: 싱글 모드, 에지 트리거, ICW4 예상 */
	outb (0xa1, 0x28); /* ICW2: 라인 IR0...7 -> irq 0x28...0x2f */
	outb (0xa1, 0x02); /* ICW3: 슬레이브 ID는 2 */
	outb (0xa1, 0x01); /* ICW4: 8086 모드, 일반 EOI, 비버퍼링 */

	/* 모든 인터럽트를 마스크 해제 */
	outb (0x21, 0x00);
	outb (0xa1, 0x00);
}

/* 주어진 IRQ에 대해 PIC에 인터럽트 종료 신호를 보냄.
   IRQ를 인식하지 않으면 다시 전달되지 않으므로 매우 중요함. */
static void
pic_end_of_interrupt (int irq) {
	ASSERT (irq >= 0x20 && irq < 0x30);

	/* 마스터 PIC에 인식 신호 전송 */
	outb (0x20, 0x20);

	/* 슬레이브 인터럽트인 경우 슬레이브 PIC에도 인식 신호 전송 */
	if (irq >= 0x28)
		outb (0xa0, 0x20);
}
/* 인터럽트 핸들러들 */

/* 모든 인터럽트, 오류, 예외에 대한 핸들러.
   이 함수는 intr-stubs.S에 있는 어셈블리어 인터럽트 스텁에 의해 호출됨.
   FRAME은 인터럽트와 중단된 스레드의 레지스터를 설명함. */
void
intr_handler (struct intr_frame *frame) {
	bool external;
	intr_handler_func *handler;

	/* 외부 인터럽트는 특별함.
	   한 번에 하나씩만 처리할 수 있으며 (따라서 인터럽트는 꺼져 있어야 함) 
	   PIC에서 인식해야 함 (아래 참조).
	   외부 인터럽트 핸들러는 대기할 수 없음. */
	external = frame->vec_no >= 0x20 && frame->vec_no < 0x30;
	if (external) {
		ASSERT (intr_get_level () == INTR_OFF);
		ASSERT (!intr_context ());

		in_external_intr = true;
		yield_on_return = false;
	}

	/* 인터럽트 핸들러 호출 */
	handler = intr_handlers[frame->vec_no];
	if (handler != NULL)
		handler (frame);
	else if (frame->vec_no == 0x27 || frame->vec_no == 0x2f) {
		/* 핸들러가 없지만 하드웨어 결함 또는 경합 조건으로 인해 
		   해당 인터럽트가 불필요하게 발생할 수 있음. 무시함. */
	} else {
		/* 핸들러가 없고 불필요하지 않은 경우.
		   예기치 않은 인터럽트 핸들러를 호출함. */
		intr_dump_frame (frame);
		PANIC ("Unexpected interrupt");
	}

	/* 외부 인터럽트 처리 완료 */
	if (external) {
		ASSERT (intr_get_level () == INTR_OFF);
		ASSERT (intr_context ());

		in_external_intr = false;
		pic_end_of_interrupt (frame->vec_no);

		if (yield_on_return)
			thread_yield ();
	}
}

/* F에 있는 인터럽트 프레임을 콘솔에 덤프하여 디버깅에 사용 */
void
intr_dump_frame (const struct intr_frame *f) {
	/* CR2는 마지막 페이지 폴트의 선형 주소임.
	   [IA32-v2a] "MOV--제어 레지스터로부터/로 이동" 및 
	   [IA32-v3a] 5.14 "인터럽트 14--페이지 폴트 예외 (#PF)" 참조. */
	uint64_t cr2 = rcr2();
	printf ("Interrupt %#04llx (%s) at rip=%llx\n",
			f->vec_no, intr_names[f->vec_no], f->rip);
	printf (" cr2=%016llx error=%16llx\n", cr2, f->error_code);
	printf ("rax %016llx rbx %016llx rcx %016llx rdx %016llx\n",
			f->R.rax, f->R.rbx, f->R.rcx, f->R.rdx);
	printf ("rsp %016llx rbp %016llx rsi %016llx rdi %016llx\n",
			f->rsp, f->R.rbp, f->R.rsi, f->R.rdi);
	printf ("rip %016llx r8 %016llx  r9 %016llx r10 %016llx\n",
			f->rip, f->R.r8, f->R.r9, f->R.r10);
	printf ("r11 %016llx r12 %016llx r13 %016llx r14 %016llx\n",
			f->R.r11, f->R.r12, f->R.r13, f->R.r14);
	printf ("r15 %016llx rflags %08llx\n", f->R.r15, f->eflags);
	printf ("es: %04x ds: %04x cs: %04x ss: %04x\n",
			f->es, f->ds, f->cs, f->ss);
}

/* 인터럽트 VEC의 이름을 반환 */
const char *
intr_name (uint8_t vec) {
	return intr_names[vec];
}
