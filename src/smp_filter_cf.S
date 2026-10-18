/* The filter's frame loop for the ColdFire (MCF5441x, EMAC), see src/smp_filter.h: the Chamberlin
   form on the EMAC in fractional, saturating mode. smp_filter.c holds the same loop as C for the
   host; the two agree bit for bit by construction (products truncated to 2^-39, sums floored to
   Q31 when read, saturated).

   void smp_filter_frames(smp_filter *f, int32_t *buf): 32 frames in place.
   The struct (smp_filter.h): +0 l, +4 b, +8 f/2, +12 q/2, +16 df/2, +20 dq/2, +24 mode (2 = HP).
   Coefficients are halves, used twice, since f and q reach 2. The loop runs on half the signal
   and doubles its result through the accumulator, which saturates. Two constants: -1.0
   (0x80000000, a2) subtracts a register from an accumulator exactly; ONE (0x7fffffff, a4) added
   as a factor doubles a value, 2^-31 short, as the C model does.
   Keeps d2-d7/a2-a6 and MACSR; leaves acc0 and acc1 at zero. */

	.text
	.globl	smp_filter_frames
smp_filter_frames:
	lea	-40(%sp), %sp
	movem.l	%d2-%d7/%a2-%a4, (%sp)
	move.l	%macsr, %d0
	move.l	%d0, 36(%sp)
	move.l	#0xa0, %macsr
	movea.l	44(%sp), %a1			/* f */
	movea.l	48(%sp), %a0			/* buf */
	move.l	(%a1), %d2			/* l */
	move.l	4(%a1), %d3			/* b */
	move.l	8(%a1), %d4			/* f/2 */
	move.l	12(%a1), %d5			/* q/2 */
	move.l	16(%a1), %d6			/* df/2 per frame */
	move.l	20(%a1), %d7			/* dq/2 per frame */
	movea.l	#0x80000000, %a2		/* -1.0 */
	movea.l	#0x7fffffff, %a4		/* ONE */
	moveq	#32, %d0
	move.l	24(%a1), %d1
	cmpi.l	#2, %d1
	beq	2f

/* lowpass */
1:	move.l	(%a0), %d1			/* x */
	asr.l	#1, %d1				/* x/2 */
	move.l	%d2, %acc0			/* l */
	mac.l	%d4, %d3, %acc0			/* + f b */
	mac.l	%d4, %d3, %acc0
	move.l	%d1, %acc1			/* x/2 */
	msac.l	%d5, %d3, %acc1			/* - q b */
	msac.l	%d5, %d3, %acc1
	movclr.l %acc0, %d2			/* l' */
	mac.l	%a2, %d2, %acc1			/* - l' */
	movclr.l %acc1, %a3			/* h */
	move.l	%d3, %acc0			/* b */
	mac.l	%d4, %a3, %acc0			/* + f h */
	mac.l	%d4, %a3, %acc0
	movclr.l %acc0, %d3			/* b' */
	move.l	%d2, %acc0			/* l' */
	mac.l	%d4, %d3, %acc0			/* + f b' */
	mac.l	%d4, %d3, %acc0
	movclr.l %acc0, %d1			/* lp/2 */
	move.l	%d1, %acc0
	mac.l	%a4, %d1, %acc0			/* 2 lp, saturated */
	movclr.l %acc0, %d1
	move.l	%d1, (%a0)+
	add.l	%d6, %d4
	add.l	%d7, %d5
	subq.l	#1, %d0
	bne	1b
	bra	3f

/* highpass: the same, then 2 (x/2 - lp/2) */
2:	move.l	(%a0), %d1
	asr.l	#1, %d1
	move.l	%d2, %acc0
	mac.l	%d4, %d3, %acc0
	mac.l	%d4, %d3, %acc0
	move.l	%d1, %acc1
	msac.l	%d5, %d3, %acc1
	msac.l	%d5, %d3, %acc1
	movclr.l %acc0, %d2
	mac.l	%a2, %d2, %acc1
	movclr.l %acc1, %a3
	move.l	%d3, %acc0
	mac.l	%d4, %a3, %acc0
	mac.l	%d4, %a3, %acc0
	movclr.l %acc0, %d3
	move.l	%d2, %acc0
	mac.l	%d4, %d3, %acc0
	mac.l	%d4, %d3, %acc0
	movclr.l %acc0, %a3			/* lp/2 */
	move.l	%d1, %acc0			/* x/2 */
	mac.l	%a4, %d1, %acc0			/* + ONE x/2 */
	mac.l	%a2, %a3, %acc0			/* - lp/2 */
	msac.l	%a4, %a3, %acc0			/* - ONE lp/2 */
	movclr.l %acc0, %d1
	move.l	%d1, (%a0)+
	add.l	%d6, %d4
	add.l	%d7, %d5
	subq.l	#1, %d0
	bne	2b

3:	move.l	%d2, (%a1)
	move.l	%d3, 4(%a1)
	move.l	36(%sp), %d0
	move.l	%d0, %macsr
	movem.l	(%sp), %d2-%d7/%a2-%a4
	lea	40(%sp), %sp
	rts
