/* The filter's frame loops for the ColdFire (MCF5441x, EMAC), see src/smp_filter.h: the Chamberlin
   form on the EMAC in fractional, saturating mode. smp_filter.c holds the same loops as C for the
   host; the two agree bit for bit by construction (products truncated to 2^-39, sums floored to
   Q31 when read, saturated).

   void smp_filter_frames(smp_filter *f, int32_t *buf): 32 frames in place.
   The struct (smp_filter.h): +0 l, +4 b, +8 f/2, +12 q/2, +16 df/2, +20 dq/2, +24 mode,
   +28 loop (0 lowpass, 1 highpass, 2 the weights at work), +32 wet, +36 dwet, +40 mix, +44 dmix.
   Coefficients are halves, used twice, since f and q reach 2. The loops run on half the signal
   and double their result through the accumulator, which saturates. Two constants: -1.0
   (0x80000000, a2) subtracts a register from an accumulator exactly; ONE (0x7fffffff, a4) added
   as a factor doubles a value, 2^-31 short, as the C model does.
   The third loop blends: filt = lp + mix (hp - lp), y = x + wet (filt - x), with wet and mix
   moving by dwet and dmix per frame; it is the one for the blocks in which the knob crosses the
   centre, changes side or stands within a step of the centre.
   Keeps d2-d7/a2-a6 and MACSR; leaves acc0 and acc1 at zero. */

	.text
	.globl	smp_filter_frames
smp_filter_frames:
	lea	-44(%sp), %sp
	movem.l	%d2-%d7/%a2-%a4/%a6, (%sp)
	move.l	%macsr, %d0
	move.l	%d0, 40(%sp)
	move.l	#0xa0, %macsr
	movea.l	48(%sp), %a1			/* f */
	movea.l	52(%sp), %a0			/* buf */
	move.l	(%a1), %d2			/* l */
	move.l	4(%a1), %d3			/* b */
	move.l	8(%a1), %d4			/* f/2 */
	move.l	12(%a1), %d5			/* q/2 */
	move.l	16(%a1), %d6			/* df/2 per frame */
	move.l	20(%a1), %d7			/* dq/2 per frame */
	movea.l	#0x80000000, %a2		/* -1.0 */
	movea.l	#0x7fffffff, %a4		/* ONE */
	moveq	#32, %d0
	move.l	28(%a1), %d1			/* loop */
	cmpi.l	#1, %d1
	beq	2f
	cmpi.l	#2, %d1
	beq	4f

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
	bra	3f

/* the weights at work: the same filter, then filt = lp + mix (hp - lp), y = x + wet (filt - x).
   wet and mix take d6 and d7; the four per-frame changes come from the struct. */
4:	move.l	32(%a1), %d6			/* wet */
	move.l	40(%a1), %d7			/* mix */
5:	move.l	(%a0), %d1
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
	movclr.l %acc0, %a6			/* lp/2 */
	move.l	%d1, %acc0			/* x/2 */
	mac.l	%a2, %a6, %acc0			/* - lp/2 */
	mac.l	%a2, %a6, %acc0			/* - lp/2: hp/2 - lp/2 */
	movclr.l %acc0, %a3
	move.l	%a6, %acc0			/* lp/2 */
	mac.l	%d7, %a3, %acc0			/* + mix (hp/2 - lp/2) */
	movclr.l %acc0, %a6			/* filt/2 */
	move.l	%a6, %acc0
	mac.l	%a2, %d1, %acc0			/* - x/2 */
	movclr.l %acc0, %a3			/* filt/2 - x/2 */
	move.l	%d1, %acc0			/* x/2 */
	mac.l	%d6, %a3, %acc0			/* + wet (filt/2 - x/2) */
	movclr.l %acc0, %d1			/* y/2 */
	move.l	%d1, %acc0
	mac.l	%a4, %d1, %acc0			/* 2 y, saturated */
	movclr.l %acc0, %d1
	move.l	%d1, (%a0)+
	add.l	16(%a1), %d4
	add.l	20(%a1), %d5
	add.l	36(%a1), %d6
	add.l	44(%a1), %d7
	subq.l	#1, %d0
	bne	5b

3:	move.l	%d2, (%a1)
	move.l	%d3, 4(%a1)
	move.l	40(%sp), %d0
	move.l	%d0, %macsr
	movem.l	(%sp), %d2-%d7/%a2-%a4/%a6
	lea	44(%sp), %sp
	rts
